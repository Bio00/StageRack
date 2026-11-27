#include <numeric>
#include "AudioEngine.h"
#include <unordered_map>
#include "HopAligner.h"
#include "RtLock.h"
#include "RtGuard.h"
#include <map>

namespace stagerack
{

static const juce::Colour rackPalette[] = {
    juce::Colour (0xff31c4f3), juce::Colour (0xff7ed957), juce::Colour (0xffffb341), juce::Colour (0xffff5f6d),
    juce::Colour (0xffb98cff), juce::Colour (0xff45e0c0), juce::Colour (0xfff26fd1), juce::Colour (0xffe8e35a)
};

AudioEngine::AudioEngine()
{
    for (int i = 0; i < maxDeviceChannels; ++i)
        inputMap[(size_t) i] = outputMap[(size_t) i] = i;

    orderedStage.reserve (1024);
    racks.reserve (1024);   // adding a rack under the lock never reallocates the vector
}

void AudioEngine::setChannelMaps (const juce::BigInteger& activeIns, int numIns, const juce::BigInteger& activeOuts, int numOuts)
{
    auto build = [] (std::array<int, maxDeviceChannels>& map, const juce::BigInteger& active, int num)
    {
        int packed = 0;
        for (int i = 0; i < maxDeviceChannels; ++i)
            map[(size_t) i] = i < num && active[i] ? packed++ : -1;
        return juce::jmin (num, maxDeviceChannels);
    };

    numDeviceInputs = build (inputMap, activeIns, numIns);
    numDeviceOutputs = build (outputMap, activeOuts, numOuts);
}

AudioEngine::~AudioEngine()
{
    deviceManager.removeMidiInputDeviceCallback ({}, this);
    deviceManager.removeAudioCallback (this);
    deviceManager.closeAudioDevice();
    clearRacks();
}

void AudioEngine::initialiseDevice (const juce::XmlElement* savedState, bool selectDefaultOnFailure)
{
    const auto error = deviceManager.initialise (64, 64, savedState, selectDefaultOnFailure);

    if (error.isNotEmpty())
        DBG ("Audio device error: " << error);

    deviceManager.addAudioCallback (this);
    deviceManager.addMidiInputDeviceCallback ({}, this);
}

//==============================================================================
int AudioEngine::nextRackId() const
{
    int maxId = 0;
    for (auto& r : racks)
        maxId = juce::jmax (maxId, r->getId());
    return maxId + 1;
}

Rack& AudioEngine::addRack()
{
    return addRackWithId (nextRackId());
}

Rack& AudioEngine::addRackWithId (int id)
{
    auto rack = std::make_unique<Rack> (id);
    const auto index = (int) racks.size();

    rack->name = "Rack " + juce::String (index + 1);
    rack->colour = rackPalette[(size_t) index % std::size (rackPalette)];

    rack->inputChannel = 0;
    rack->outputChannel = 0;
    rack->tempo = &tempo;
    rack->prepare (sampleRate, blockSize);

    auto& ref = *rack;
    auto list = rackList();
    list.push_back (rack.get());
    auto plan = planGraph (list);
    {
        const juce::SpinLock::ScopedLockType sl (racksLock);
        racks.push_back (std::move (rack));
        applyGraphLocked (plan);
    }
    return ref;
}

int AudioEngine::indexOf (const Rack& rack) const
{
    for (size_t i = 0; i < racks.size(); ++i)
        if (racks[i].get() == &rack)
            return (int) i;
    return -1;
}

std::vector<Rack*> AudioEngine::rackList() const
{
    std::vector<Rack*> list;
    list.reserve (racks.size() + 1);
    for (auto& r : racks)
        list.push_back (r.get());
    return list;
}

void AudioEngine::removeRack (Rack& rack)
{
    auto list = rackList();
    list.erase (std::remove (list.begin(), list.end(), &rack), list.end());
    auto plan = planGraph (list);

    std::unique_ptr<Rack> doomed;
    {
        const juce::SpinLock::ScopedLockType sl (racksLock);

        for (auto it = racks.begin(); it != racks.end(); ++it)
        {
            if (it->get() == &rack)
            {
                doomed = std::move (*it);
                racks.erase (it);
                break;
            }
        }

        applyGraphLocked (plan);
    }
    // `doomed` (and its plugins) and the old graph are destroyed here, on the message thread.
}

void AudioEngine::moveRack (int from, int to)
{
    if (! juce::isPositiveAndBelow (from, (int) racks.size()) || ! juce::isPositiveAndBelow (to, (int) racks.size()))
        return;

    auto list = rackList();
    auto* moving = list[(size_t) from];
    list.erase (list.begin() + from);
    list.insert (list.begin() + to, moving);
    auto plan = planGraph (list);

    const juce::SpinLock::ScopedLockType sl (racksLock);
    auto item = std::move (racks[(size_t) from]);
    racks.erase (racks.begin() + from);
    racks.insert (racks.begin() + to, std::move (item));
    applyGraphLocked (plan);
}

void AudioEngine::clearRacks()
{
    std::vector<std::unique_ptr<Rack>> doomed;
    std::vector<std::vector<Rack*>> oldStages;
    {
        const juce::SpinLock::ScopedLockType sl (racksLock);
        std::swap (doomed, racks);
        std::swap (oldStages, stages);
    }
    racks.reserve (1024);
}

Rack* AudioEngine::findRack (int id) const
{
    for (auto& r : racks)
        if (r->getId() == id)
            return r.get();
    return nullptr;
}

int AudioEngine::choosePhase (int hopCalls, const Rack::Slot* exclude, const Rack* forRack) const
{
    const auto cost = phaseCosts (hopCalls, exclude, forRack);
    return (int) (std::min_element (cost.begin(), cost.end()) - cost.begin());
}

std::vector<int> AudioEngine::phaseCosts (int hopCalls, const Rack::Slot* exclude, const Rack* forRack) const
{
    // Least loaded phase among the instances with a hop. Instances that run in series with
    // this one (same rack, or racks chained to it) weigh far more: their hops would add up on
    // one core, while hops of independent racks run on different cores.
    // Different hops collide too: phase p of hop H and phase q of hop H2 meet in some callback
    // iff p == q modulo gcd (H, H2) (every lcm callbacks), never otherwise. E.g. De-Feedback
    // (192 samples = 12 callbacks @16) and soothe3 in Low latency (1024 = 64 callbacks) in one
    // rack: gcd 4, so 3 phases in 4 keep their hops apart for good. Same hop: gcd = H, the
    // plain "same phase" rule.
    std::vector<int> cost ((size_t) hopCalls, 0);

    for (auto& r : racks)
    {
        const bool serial = forRack != nullptr && runsInSeries (*r, *forRack);

        for (int i = 0; i < Rack::maxSlots; ++i)
        {
            auto& s = r->getSlot (i);
            const int otherHop = s.hopCalls.load(), otherPhase = s.hopPhase.load();

            if (&s == exclude || s.plugin == nullptr || otherHop <= 1 || ! juce::isPositiveAndBelow (otherPhase, otherHop))
                continue;

            if (! mixedHopCosts && otherHop != hopCalls)
                continue;

            const int g = std::gcd (hopCalls, otherHop);

            for (int p = otherPhase % g; p < hopCalls; p += g)
                cost[(size_t) p] += serial ? 1000 : 1;
        }
    }

    return cost;
}

std::vector<std::pair<int, int>> AudioEngine::watchHops()
{
    std::vector<std::pair<int, int>> toRespread;

    if (! hopAlignEnabled || blockSize <= 0)
        return toRespread;

    const auto now = callbackCount.load();
    const auto perSecond = (juce::uint64) juce::jmax (1.0, sampleRate / blockSize);

    for (auto& r : racks)
    {
        for (int i = 0; i < Rack::maxSlots; ++i)
        {
            auto& s = r->getSlot (i);

            if (s.plugin == nullptr || r->isHandingOver (i))
            {
                s.watchPlugin = nullptr;
                continue;
            }

            // CoreWakeAssist need, per plugin type: a new instance of a type that needed it
            // starts with it (no slow first hop)
            if (s.hopCalls.load() > 1)
            {
                const auto id = s.plugin->getPluginDescription().createIdentifierString();

                if (s.wakeNeed.load() == 1)
                    wakeNeedTypes.insert (id);
                else if (wakeNeedTypes.count (id) > 0)
                    s.wakeNeed = 1;
            }

            if (s.watchPlugin != s.plugin.get() || now < s.watchSince)
            {
                // new instance (or the device restarted): its log starts now
                s.watchPlugin = s.plugin.get();
                s.watchSince = now;
                s.watchSeen = 0;
                s.watchRespreads = 0;
                continue;
            }

            if (now < s.watchSince + perSecond / 2)
                continue;

            // true bypass (fading, draining, idle): not called as usual, its hop keeps its
            // phase (Rack::processBypassTransition catches up on resume)
            if (s.bypassState.load() != Rack::Slot::bpActive)
            {
                s.watchSince = now;   // on resume its log starts afresh
                s.watchSeen = 0;
                continue;
            }

            // heavy callbacks logged since watchSince, oldest first
            std::vector<juce::uint64> at;
            const auto count = s.heavyCount.load (std::memory_order_acquire);

            for (auto k = count > (juce::uint32) Rack::Slot::heavyRing ? count - (juce::uint32) Rack::Slot::heavyRing : 0u; k < count; ++k)
                if (const auto v = s.heavyAt[k % Rack::Slot::heavyRing].load (std::memory_order_relaxed);
                    v >= s.watchSince && v <= now && (at.empty() || v > at.back()))
                    at.push_back (v);

            if (at.empty() || now - at.back() > perSecond)
            {
                // no heavy calls for a second: switched off / bypassed. Its phase is free again
                // (and the wake assist stops waiting for it).
                if (s.hopCalls > 0 && now - s.watchSince > perSecond)
                {
                    s.hopCalls = 0;
                    s.hopPhase = -1;
                }
                s.watchSeen = 0;
                continue;
            }

            if (at.size() < 8)
                continue;

            std::map<int, int> gaps;
            for (size_t k = 1; k < at.size(); ++k)
                ++gaps[(int) (at[k] - at[k - 1])];

            const auto best = std::max_element (gaps.begin(), gaps.end(), [] (auto& a, auto& b) { return a.second < b.second; });
            const int hop = best->first;

            if (hop < 2 || hop > 128 || best->second < (int) (0.6 * (double) (at.size() - 1)))
                continue;

            std::vector<int> hits ((size_t) hop, 0);
            for (auto v : at)
                ++hits[(size_t) (v % (juce::uint64) hop)];

            const int phase = (int) (std::max_element (hits.begin(), hits.end()) - hits.begin());

            if (hits[(size_t) phase] < (int) (0.7 * (double) at.size()))
                continue;

            if (hop == s.hopCalls && phase == s.hopPhase)
            {
                s.watchSeen = 0;

                // Settled, but sharing its phase with an instance in series (a track whose
                // master got a plugin later, or an output just routed to a bus): the two hops
                // add up on one core. Move it if a phase without series partners exists. The
                // master itself never moves (all its phases are in series); every move removes
                // a series collision, so this cannot oscillate.
                const auto cost = phaseCosts (hop, &s, r.get());
                if (cost[(size_t) phase] >= 1000 && *std::min_element (cost.begin(), cost.end()) < 1000
                    && s.watchRespreads < 3)
                {
                    ++s.watchRespreads;
                    ++seriesRespreads;
                    toRespread.push_back ({ r->getId(), i });
                    if (logWatch)
                    {
                        juce::String c; for (auto v : cost) c << v << " ";
                        std::printf ("    [watch] rack id %d slot %d phase %d shares it with a series partner -> move (costs %s)\n", r->getId(), i, phase, c.toRawUTF8());
                    }
                }
                continue;
            }

            // the hop appeared or moved: act only when two checks in a row agree
            if (hop != s.watchHop || phase != s.watchPhase)
            {
                s.watchHop = hop;
                s.watchPhase = phase;
                s.watchSeen = 1;
                continue;
            }

            if (++s.watchSeen < 2)
                continue;

            s.watchSeen = 0;
            const auto cost = phaseCosts (hop, &s, r.get());
            const bool crowded = cost[(size_t) phase] > *std::min_element (cost.begin(), cost.end());

            if (logWatch)
            {
                juce::String c; for (auto v : cost) c << v << " ";
                std::printf ("    [watch] rack id %d slot %d hop moved %d/%d -> %d/%d (costs %s)%s\n", r->getId(), i, s.hopCalls.load(), s.hopPhase.load(),
                             hop, phase, c.toRawUTF8(), crowded ? " crowded" : "");
            }
            s.hopCalls = hop;       // where it really is now
            s.hopPhase = phase;
            ++hopsAdopted;

            // a better phase is free: hand over to a re-phased twin (at most 3 tries per instance)
            if (crowded && s.watchRespreads < 3)
            {
                ++s.watchRespreads;
                toRespread.push_back ({ r->getId(), i });
            }
        }
    }

    return toRespread;
}

bool AudioEngine::runsInSeries (const Rack& a, const Rack& b) const
{
    // same rack, or one feeds the other (directly or through other racks)
    return &a == &b || feedsInto (a, b) || feedsInto (b, a);
}

bool AudioEngine::feedsInto (const Rack& upstream, const Rack& rack) const
{
    std::vector<const Rack*> todo { &rack }, seen;

    while (! todo.empty())
    {
        const auto* r = todo.back();
        todo.pop_back();

        for (const auto& f : r->feeds)
        {
            if (! r->feedIsOn (f))
                continue;
            if (f.src == &upstream)
                return true;
            if (std::find (seen.begin(), seen.end(), f.src) == seen.end() && seen.size() < 1024)
            {
                seen.push_back (f.src);
                todo.push_back (f.src);
            }
        }
    }

    return false;
}

bool AudioEngine::wouldCreateCycle (const Rack& rack, int sourceRackId) const
{
    const auto* src = findRack (sourceRackId);
    return src != nullptr && (src == &rack || feedsInto (rack, *src));
}

bool AudioEngine::setRackInputSource (Rack& rack, int sourceRackId)
{
    if (sourceRackId >= 0 && (sourceRackId == rack.getId() || findRack (sourceRackId) == nullptr || wouldCreateCycle (rack, sourceRackId)))
        return false;

    rack.inputRackId = sourceRackId;
    rebuildGraph();
    updateLatencyCompensation();
    return true;
}

void AudioEngine::setRoutingEnabled (bool on)
{
    routingEnabled = on;
    rebuildRouting();
}

bool AudioEngine::setRackBus (Rack& rack, int letter)
{
    letter = juce::jlimit (0, Rack::maxBuses, letter);

    if (letter > 0)
        for (auto& r : racks)
            if (r.get() != &rack && r->busIndex.load() == letter)
                return false;

    rack.busIndex = letter;

    if (letter > 0 && rack.outputBus.load() == letter)
        rack.outputBus = 0;

    if (letter > 0)
        for (auto& o : rack.extraOutputs)
            if (o.load() == Rack::outBus + letter)
                o = 0;

    rebuildRouting();
    return true;
}

bool AudioEngine::setRackOutputBus (Rack& rack, int letter)
{
    letter = juce::jlimit (0, Rack::maxBuses, letter);

    if (letter > 0 && ! canRouteToBus (rack, letter))
        return false;

    rack.outputBus = letter;
    rebuildRouting();
    return true;
}

std::vector<int> AudioEngine::setRackExtraOutputs (Rack& rack, const std::vector<int>& codes)
{
    std::vector<int> kept;
    const auto main = rack.mainOutputCode (true);
    bool busesChanged = false;

    for (auto c : codes)
    {
        if (c <= Rack::outNone || c == main || std::find (kept.begin(), kept.end(), c) != kept.end()
            || (int) kept.size() >= Rack::maxExtraOutputs)
            continue;

        if (c > Rack::outBus)
        {
            const auto k = c - Rack::outBus;
            // a bus that does not exist yet / with routing off is kept (it works when it does);
            // only a loop or the rack's own letter is refused
            if (k > Rack::maxBuses || rack.busIndex.load() == k || (findBus (k) != nullptr && ! canRouteToBus (rack, k)))
                continue;
        }
        else if (c < Rack::outStereo || c >= Rack::outBus)
        {
            continue;
        }

        kept.push_back (c);
    }

    for (int i = 0; i < Rack::maxExtraOutputs; ++i)
    {
        const auto c = i < (int) kept.size() ? kept[(size_t) i] : 0;
        const auto old = rack.extraOutputs[(size_t) i].load();
        busesChanged = busesChanged || (old != c && (old > Rack::outBus || c > Rack::outBus));
        rack.extraOutputs[(size_t) i] = c;
    }

    if (busesChanged)
        rebuildRouting();

    return kept;
}

bool AudioEngine::canRouteToBus (const Rack& rack, int letter) const
{
    auto* bus = findBus (letter);
    return bus != nullptr && bus != &rack && ! feedsInto (*bus, rack);
}

Rack* AudioEngine::findBus (int letter) const
{
    for (auto& r : racks)
        if (r->isActiveBus && r->busIndex.load() == letter)
            return r.get();
    return nullptr;
}

int AudioEngine::firstFreeBus() const
{
    for (int k = 1; k <= Rack::maxBuses; ++k)
    {
        bool used = false;
        for (auto& r : racks)
            used = used || r->busIndex.load() == k;
        if (! used)
            return k;
    }
    return 0;
}

void AudioEngine::setSend (Rack& r, int letter, float db)
{
    if (letter <= 0 || letter > Rack::maxBuses)
        return;

    auto& send = r.sends[(size_t) letter - 1];
    const bool wasOn = send.isOn();
    send.db = juce::jlimit (Rack::sendOffDb, 12.0f, db);

    if (! r.isActiveBus || wasOn == send.isOn())
        return;

    if (send.isOn())
    {
        rebuildRouting();   // a bus -> bus edge appears (its gain ramps up from 0)
    }
    else
    {
        // let the feed ramp down to 0 in the next blocks, then drop the edge
        juce::Timer::callAfterDelay (150, [weak = std::weak_ptr<int> (aliveToken), this]
        {
            if (weak.lock() != nullptr)
                rebuildRouting();
        });
    }
}

juce::uint32 AudioEngine::busSendMask() const
{
    juce::uint32 m = 0;
    int bit = 0;
    for (auto& r : racks)
        if (r->isActiveBus)
            for (auto& snd : r->sends)
                m |= (snd.isOn() ? 1u : 0u) << (bit++ & 31);
    return m;
}

void AudioEngine::rebuildRouting()
{
    rebuildGraph();
    updateLatencyCompensation();
}

void AudioEngine::rebuildGraph()
{
    auto plan = planGraph (rackList());
    {
        const juce::SpinLock::ScopedLockType sl (racksLock);
        applyGraphLocked (plan);
    }
    // the previous graph (now in `plan`) is freed here, outside the lock
}

void AudioEngine::applyGraphLocked (GraphPlan& plan) noexcept
{
    // only swaps and stores: the audio thread waits for this at most a few microseconds
    for (size_t i = 0; i < plan.list.size(); ++i)
    {
        auto& r = *plan.list[i];
        auto& n = plan.nodes[i];

        // send / bus ramps: the gains the audio thread reached until now
        for (auto& f : n.feeds)
            for (const auto& old : r.feeds)
                if (old.src == f.src && old.kind == f.kind && old.send == f.send)
                    f.gain = old.gain;

        std::swap (r.feeds, n.feeds);
        r.sourceRack = n.sourceRack;
        r.outBusRack = n.outBusRack;
        r.outToBus = n.outToBus;
        r.feedsABus = n.feedsABus;
        r.isActiveBus = n.isActiveBus;
        r.stage = n.stage;
        r.sendAllowed = n.sendAllowed;
    }

    std::swap (stages, plan.stages);
}

AudioEngine::GraphPlan AudioEngine::planGraph (const std::vector<Rack*>& list) const
{
    // Everything that feeds a rack (chain source, racks routed or sending to it as a bus) runs
    // in an earlier stage of the same callback: stage = 1 + the latest stage feeding it.
    // Runs on the message thread (the only writer of the graph) without the lock: it reads the
    // racks' current feeds only for the ramp gains, refreshed again in applyGraphLocked.
    GraphPlan plan;
    plan.list = list;
    plan.nodes.resize (list.size());

    std::unordered_map<const Rack*, size_t> index;
    for (size_t i = 0; i < list.size(); ++i)
        index[list[i]] = i;

    auto node = [&] (const Rack* r) -> GraphPlan::Node& { return plan.nodes[index.at (r)]; };
    auto find = [&] (int id) -> Rack*
    {
        for (auto* r : list)
            if (r->getId() == id)
                return r;
        return nullptr;
    };

    const bool routing = isRoutingActive();
    std::array<Rack*, Rack::maxBuses> buses {};

    for (auto* r : list)
        if (const auto b = r->busIndex.load(); routing && b > 0 && b <= Rack::maxBuses && buses[(size_t) b - 1] == nullptr)
        {
            buses[(size_t) b - 1] = r;
            node (r).isActiveBus = true;
        }

    // does `from` already reach `to` through the edges added so far (all of them, on or off)?
    auto reaches = [&] (const Rack* from, const Rack* to)
    {
        std::vector<const Rack*> todo { to }, seen;

        while (! todo.empty())
        {
            const auto* r = todo.back();
            todo.pop_back();

            if (r == from)
                return true;

            for (const auto& f : node (r).feeds)
                if (std::find (seen.begin(), seen.end(), f.src) == seen.end() && seen.size() < 1024)
                {
                    seen.push_back (f.src);
                    todo.push_back (f.src);
                }
        }

        return false;
    };

    auto addFeed = [&] (Rack* src, Rack* dst, int kind, int send)
    {
        if (src == dst || reaches (dst, src))
            return false;   // would close a loop

        float g = 0.0f;
        for (const auto& old : dst->feeds)   // keeps the send ramps going
            if (old.src == src && old.kind == kind && old.send == send)
                g = old.gain;

        node (dst).feeds.push_back ({ src, kind, send, g });
        return true;
    };

    // 1. chains (a bus takes its input from its feeds instead)
    for (auto* r : list)
        if (! node (r).isActiveBus && r->inputRackId.load() >= 0)
            if (auto* src = find (r->inputRackId.load()); src != nullptr && addFeed (src, r, 0, -1))
                node (r).sourceRack = src;

    // 2. outputs routed into buses (missing bus or loop = silent, never the device by surprise)
    if (routing)
    {
        for (auto* r : list)
        {
            const auto b = r->outputBus.load();

            if (b <= 0 || b > Rack::maxBuses)
                continue;

            node (r).outToBus = true;

            if (auto* bus = buses[(size_t) b - 1]; bus != nullptr && addFeed (r, bus, 1, -1))
            {
                node (r).outBusRack = bus;
                node (r).feedsABus = true;
            }
        }

        // extra outputs into buses (after every main one: a main output wins a would-be loop)
        for (auto* r : list)
            for (auto c : r->getExtraOutputs())
                if (const auto k = c - Rack::outBus; k > 0 && k <= Rack::maxBuses)
                    if (auto* bus = buses[(size_t) k - 1]; bus != nullptr && bus != node (r).outBusRack && addFeed (r, bus, 1, -1))
                        node (r).feedsABus = true;
    }

    // 3. sends: every rack can send to every bus that does not feed it. Sends that are on are
    //    placed first; normal racks before buses, buses in letter order.
    if (routing)
    {
        std::vector<Rack*> order;
        for (auto* r : list)
            if (! node (r).isActiveBus)
                order.push_back (r);
        for (auto* b : buses)
            if (b != nullptr)
                order.push_back (b);

        // A send from a bus that is off is not an edge: buses can send to each other in any
        // direction until one of the two is turned on (setSend rebuilds then).
        for (int pass = 0; pass < 2; ++pass)
            for (auto* r : order)
                for (int k = 0; k < Rack::maxBuses; ++k)
                    if (buses[(size_t) k] != nullptr && r->sends[(size_t) k].isOn() == (pass == 0)
                        && ! (pass == 1 && node (r).isActiveBus)
                        && addFeed (r, buses[(size_t) k], 2, k))
                        node (r).sendAllowed |= 1u << k;

        for (auto* b : buses)
            if (b != nullptr)
                for (int k = 0; k < Rack::maxBuses; ++k)
                    if (auto* to = buses[(size_t) k]; to != nullptr && to != b && ! b->sends[(size_t) k].isOn() && ! reaches (to, b))
                        node (b).sendAllowed |= 1u << k;   // possible: turning it on adds the edge
    }

    std::function<int (Rack*, int)> stageOf = [&] (Rack* r, int depth) -> int
    {
        auto& n = node (r);
        if (n.stage >= 0)
            return n.stage;

        int st = 0;

        if (depth < 256)
            for (const auto& f : n.feeds)
                st = juce::jmax (st, stageOf (f.src, depth + 1) + 1);

        return n.stage = st;
    };

    for (auto* r : list)
    {
        const auto st = stageOf (r, 0);

        if ((int) plan.stages.size() <= st)
            plan.stages.resize ((size_t) st + 1);

        plan.stages[(size_t) st].push_back (r);
    }

    return plan;
}

void AudioEngine::updateLatencyCompensation()
{
    // latency at each rack's output, before compensation: upstream + own plugins
    std::map<const Rack*, int> base;
    std::function<int (const Rack*, int)> baseOf = [&] (const Rack* r, int depth) -> int
    {
        if (auto it = base.find (r); it != base.end())
            return it->second;

        int upstream = 0;   // the latest of the racks feeding it (chain, buses)

        if (depth < 256)
            for (const auto& f : r->feeds)
                if (r->feedIsOn (f))
                    upstream = juce::jmax (upstream, baseOf (f.src, depth + 1) + f.src->compensationSamples.load());

        return base[r] = upstream + r->getLatencySamples();
    };

    std::map<int, int> groupMax;

    for (auto& r : racks)
        if (const auto g = r->latencyGroup.load(); g > 0)
            groupMax[g] = juce::jmax (groupMax[g], baseOf (r.get(), 0));

    for (auto& r : racks)
    {
        const auto g = r->latencyGroup.load();
        const auto b = baseOf (r.get(), 0);
        const auto comp = g > 0 ? juce::jlimit (0, Rack::maxCompensation - 1, groupMax[g] - b) : 0;

        r->compensationSamples = comp;
        r->totalLatencySamples = b + comp;
    }
}

std::vector<Rack*> AudioEngine::alignmentOrder() const
{
    // Racks in series with many others first (a master fed by 8 tracks): they take a phase of
    // their own, then the independent tracks share the remaining phases among themselves.
    // Aligned last, the master would find every phase taken by a track that feeds it
    // (measured: 8 De-Feedback tracks + one on the master @32 = 991 late callbacks in 10 s,
    // master first = 0).
    std::vector<std::pair<int, Rack*>> counted;

    for (auto& a : racks)
    {
        int n = 0;
        for (auto& b : racks)
            if (a != b && runsInSeries (*a, *b))
                ++n;
        counted.push_back ({ n, a.get() });
    }

    std::stable_sort (counted.begin(), counted.end(), [] (auto& x, auto& y) { return x.first > y.first; });

    std::vector<Rack*> order;
    for (auto& c : counted)
        order.push_back (c.second);
    return order;
}

void AudioEngine::alignAllRacks()
{
    // Called while the callback is stopped: plugins can be driven directly.
    // Racks in series with many others first (see alignmentOrder)
    for (auto* r : alignmentOrder())
    {
        if (! hopAlignEnabled)
            continue;

        for (int i = 0; i < Rack::maxSlots; ++i)
        {
            auto& slot = r->getSlot (i);

            slot.heavyCount = 0;
            slot.watchPlugin = nullptr;

            if (slot.plugin == nullptr)
                continue;

            const auto res = HopAligner::detectAndAlign (*slot.plugin, blockSize,
                                                         [&] (int hop) { return choosePhase (hop, &slot, r); },
                                                         [] { return (juce::uint64) 0; });
            slot.hopCalls = res.hopCalls;
            slot.hopPhase = res.phase;
            slot.owed = 0;   // its phase now counts from here (true bypass catch-up)

            if (std::getenv ("STAGERACK_LOG_ALIGN") != nullptr)
                std::printf ("    [align] rack %d slot %d: hop %d phase %d (block %d)\n", r->getId(), i, res.hopCalls, res.phase, blockSize);
        }
    }
}

void AudioEngine::alignNewPlugin (juce::AudioPluginInstance& p, Rack& rack, int& hopCalls, int& hopPhase,
                                  const Rack::Slot* replacing, juce::uint64* firstLiveCallback)
{
    hopCalls = 0;
    hopPhase = -1;

    if (! hopAlignEnabled)
        return;

    // The plugin goes live in the next callback (measured: a twin started exactly at
    // callbackCount; the old "+2" put every live-aligned hop two phases early). A twin that
    // still starts later is corrected on the audio thread (Rack::processSlot).
    const auto res = HopAligner::detectAndAlign (p, rack.getBlockSize(),
                                                 [this, &rack, replacing] (int hop) { return choosePhase (hop, replacing, &rack); },
                                                 [this, firstLiveCallback]
                                                 {
                                                     const auto live = callbackCount.load();
                                                     if (firstLiveCallback != nullptr) *firstLiveCallback = live;
                                                     return live;
                                                 });
    hopCalls = res.hopCalls;
    hopPhase = res.phase;
}

void AudioEngine::realignRack (Rack& rack)
{
    if (! hopAlignEnabled)
        return;

    rack.withChainLocked ([&]
    {
        for (int i = 0; i < Rack::maxSlots; ++i)
        {
            auto& slot = rack.getSlot (i);
            slot.watchPlugin = nullptr;

            if (slot.plugin == nullptr)
                continue;

            const auto res = HopAligner::detectAndAlign (*slot.plugin, rack.getBlockSize(),
                                                         [&] (int hop) { return choosePhase (hop, &slot, &rack); },
                                                         [this] { return callbackCount.load() + 1; });
            slot.hopCalls = res.hopCalls;
            slot.hopPhase = res.phase;
            slot.owed = 0;   // its phase now counts from here (true bypass catch-up)
        }
    });
}

void AudioEngine::setWakeAssistMode (int mode)
{
    wakeAssistMode = mode;
    wakeAssist.setAlwaysOn (mode == 1);
}

void AudioEngine::armWakeAssist (juce::uint64 index) noexcept
{
    Rack::wakeProbeNow.store (false, std::memory_order_relaxed);

    if (wakeAssistMode.load (std::memory_order_relaxed) != 0)
        return;

    // Soonest scheduled hop (0 = in this callback): of every plugin (they keep the cores
    // awake) and of the ones the assist is for.
    int soonestAny = std::numeric_limits<int>::max();
    int soonest = std::numeric_limits<int>::max();

    for (auto& r : racks)
    {
        for (int i = 0; i < Rack::maxSlots; ++i)
        {
            auto& s = r->getSlot (i);
            const auto hop = s.hopCalls.load (std::memory_order_relaxed);
            const auto phase = s.hopPhase.load (std::memory_order_relaxed);

            if (hop <= 1 || phase < 0 || s.bypassState.load (std::memory_order_relaxed) == Rack::Slot::bpIdle)
                continue;

            const auto d = (phase - (int) (index % (juce::uint64) hop) + hop) % hop;
            soonestAny = juce::jmin (soonestAny, d);

            // plugins that compute their hop on the calling thread (measured: no waiting)
            // gain nothing from a woken core, e.g. EverClean; the others are tried without
            // the assist first (wakeNeed, see Rack::Slot)
            const auto wait = s.hopWaitMs.load (std::memory_order_relaxed);

            if ((wait < 0.0f || wait > 0.005f) && s.wakeNeed.load (std::memory_order_relaxed) == 1)
                soonest = juce::jmin (soonest, d);
        }
    }

    if (soonestAny == std::numeric_limits<int>::max())
        return;

    const auto periodMs = 1000.0 * blockSize / juce::jmax (1.0, sampleRate);

    if (soonestAny == 0)
    {
        // a hop now after an idle gap, not covered by the assist: a valid probe
        const auto gapMs = (double) (index - lastHopCallback) * periodMs;
        Rack::wakeProbeNow.store (gapMs > 0.8 && index > assistCoversUntil, std::memory_order_relaxed);
        lastHopCallback = index;
    }

    if (soonest == std::numeric_limits<int>::max())
        return;

    const auto leadCallbacks = juce::jmax (1, (int) std::ceil (wakeLeadMs.load (std::memory_order_relaxed) / periodMs));

    // Only when the cores would have gone idle before that hop: with dense hops (many
    // instances) they never sleep and the assist would just waste CPU.
    const auto gapMs = (double) (index + (juce::uint64) soonest - lastHopCallback) * periodMs;

    if (soonest >= 1 && soonest <= leadCallbacks && gapMs > 0.8)
    {
        const auto period = juce::Time::secondsToHighResolutionTicks (blockSize / juce::jmax (1.0, sampleRate));
        wakeAssist.armUntil (juce::Time::getHighResolutionTicks() + period * (soonest + 1));
        assistCoversUntil = index + (juce::uint64) soonest;
    }
}

void AudioEngine::setWorkerThreads (int numWorkers)
{
    workerSetting = numWorkers;

    // Re-adding the callback re-runs audioDeviceAboutToStart, which restarts the pool.
    deviceManager.removeAudioCallback (this);
    deviceManager.addAudioCallback (this);
}

//==============================================================================
void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    dropouts.reset();   // a new run: no "gap" back to the previous one
    outputsBlocked = isDeviceAllowed != nullptr && ! isDeviceAllowed();
    // read once here: the UI must not ask CoreAudio on every paint (a busy coreaudiod would
    // freeze the window; seen after a Dante device was reopened)
    deviceIoLatencySamples = device->getInputLatencyInSamples() + device->getOutputLatencyInSamples();

    // the IO workgroup, handed to the Audio Units that ask for it (see AuWorkgroup.h);
    // callbacks are stopped here, so the old one can go
    auwg::releaseWorkgroup (auwg::current.exchange (auwg::copyDeviceWorkgroup (device->getName())));

    {
        DeviceChannels c { device->getInputChannelNames(), device->getOutputChannelNames(),
                           device->getActiveInputChannels(), device->getActiveOutputChannels() };
        const std::lock_guard<std::mutex> l (channelsLock);
        deviceChannels = std::move (c);
    }

    setChannelMaps (device->getActiveInputChannels(), device->getInputChannelNames().size(),
                    device->getActiveOutputChannels(), device->getOutputChannelNames().size());
    prepare (device->getCurrentSampleRate(), device->getCurrentBufferSizeSamples(), device->getWorkgroup());
}

void AudioEngine::prepare (double newSampleRate, int newBlockSize, const juce::AudioWorkgroup& workgroup)
{
    sampleRate = newSampleRate;
    blockSize = newBlockSize;
    tempo.prepare (sampleRate);

    {
        const juce::SpinLock::ScopedLockType sl (racksLock);
        for (auto& r : racks)
            r->prepare (sampleRate, blockSize);
    }

    workgroupActive = (bool) workgroup;

    int workers = workerSetting < 0 ? WorkerPool::getRecommendedWorkerCount() : workerSetting;

    if (workgroup)
        if (const auto maxParallel = (int) workgroup.getMaxParallelThreadCount(); maxParallel > 1)
            workers = juce::jmin (workers, maxParallel - 1);

    pool.start (workers, blockSize, sampleRate);
    pool.setWorkgroup (workgroup);

    callbackCount = 0;
    alignAllRacks();

    masterGain.reset (sampleRate, 0.02);
    masterGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (masterGainDb.load()));
}

void AudioEngine::audioDeviceStopped()
{
    pool.stop();

    const juce::SpinLock::ScopedLockType sl (racksLock);
    for (auto& r : racks)
        r->release();
}

bool AudioEngine::automixGains (const float* weightedPowers, float* gains, int n) noexcept
{
    double sum = 0.0;

    for (int i = 0; i < n; ++i)
        sum += weightedPowers[i];

    if (sum < 1.0e-12)   // whole group below ~-120 dBFS: keep the last gains (last mic hold)
        return false;

    for (int i = 0; i < n; ++i)
        gains[i] = juce::jmax (1.0e-3f, (float) std::sqrt (weightedPowers[i] / sum));   // floor -60 dB

    return true;
}

void AudioEngine::automixOverlapGains (float* gains, const float* talk, int n, float maxOpen) noexcept
{
    float talkers = 0.0f, total = 0.0f;

    for (int i = 0; i < n; ++i)
    {
        talkers += talk[i];
        total += gains[i] * gains[i];
    }

    const auto target = juce::jlimit (1.0f, juce::jmax (1.0f, maxOpen), talkers);

    if (target <= 1.0f || total <= 0.0f)
        return;

    const auto scale = std::sqrt (target / total);

    for (int i = 0; i < n; ++i)
        gains[i] = juce::jmin (1.0f, gains[i] * scale);
}

void AudioEngine::computeAutomixGains (bool anySolo) noexcept
{
    // Gain sharing (Dan Dugan, 1975): every rack of a group gets the share of the group's
    // power it is contributing. One talker -> 0 dB on that mic, the others way down; two
    // talkers -> -3 dB each; nobody -> all at -10 log10 (N): constant total gain, no gating,
    // no threshold, no attack delay. Runs once per callback on the IO thread, after all
    // racks finished: zero latency, ~N multiplies.
    constexpr int maxGroupRacks = 128;
    const bool enabled = automixEnabled.load (std::memory_order_relaxed) && features::has (features::automix);

    for (int g = 1; g <= Rack::maxAutomixGroups; ++g)
    {
        Rack* members[maxGroupRacks];
        float powers[maxGroupRacks], gains[maxGroupRacks], talk[maxGroupRacks];
        int n = 0;

        for (auto& r : racks)
        {
            if (r->automixGroup.load (std::memory_order_relaxed) != g || n >= maxGroupRacks)
                continue;

            if (! enabled)
            {
                r->automixTarget = 1.0f;
                continue;
            }

            // only racks that reach the mix take part
            juce::ignoreUnused (anySolo);
            bool reachesMix = r->feedsABus || (! r->outToBus && r->outputChannel.load (std::memory_order_relaxed) >= 0);
            for (auto& o : r->extraOutputs)
                if (const auto c = o.load (std::memory_order_relaxed); c >= Rack::outStereo && c < Rack::outBus)
                    reachesMix = true;
            const bool audible = r->producedAudio() && reachesMix && r->passesSolo();
            const auto w = juce::Decibels::decibelsToGain (r->automixWeightDb.load (std::memory_order_relaxed), -100.0f);

            members[n] = r.get();
            powers[n] = audible ? r->automixLevel * w * w : 0.0f;
            talk[n] = audible ? r->automixTalk : 0.0f;
            gains[n] = r->automixTarget;
            ++n;
        }

        if (n > 0 && automixGains (powers, gains, n))
        {
            if (const auto mode = automixOverlap.load (std::memory_order_relaxed); mode > 0)
            {
                // a talker = above its own noise floor AND close to the loudest mic of the group
                // (a mic that only hears another talker's bleed, ~15 dB down, is not one)
                float loudest = 0.0f;
                for (int i = 0; i < n; ++i)
                    loudest = juce::jmax (loudest, powers[i]);

                for (int i = 0; i < n; ++i)
                {
                    const auto relDb = powers[i] > 0.0f ? 10.0f * std::log10 (powers[i] / loudest) : -100.0f;
                    talk[i] *= juce::jlimit (0.0f, 1.0f, (relDb + 12.0f) / 6.0f);
                }

                automixOverlapGains (gains, talk, n, mode == 1 ? 1.5f : 8.0f);
            }

            for (int i = 0; i < n; ++i)
                members[i]->automixTarget = gains[i];
        }
    }

    for (auto& r : racks)
    {
        if (r->automixGroup.load (std::memory_order_relaxed) <= 0)
            r->automixTarget = 1.0f;

        r->automixGainDb.store (juce::Decibels::gainToDecibels (r->automixTarget, -100.0f), std::memory_order_relaxed);
    }
}

bool AudioEngine::hasHopNow (Rack& r, juce::uint64 index) noexcept
{
    for (int i = 0; i < Rack::maxSlots; ++i)
    {
        auto& s = r.getSlot (i);
        const auto hop = s.hopCalls.load (std::memory_order_relaxed);
        const auto phase = s.hopPhase.load (std::memory_order_relaxed);

        if (hop > 1 && phase >= 0 && (int) (index % (juce::uint64) hop) == phase
            && s.bypassState.load (std::memory_order_relaxed) != Rack::Slot::bpIdle)
            return true;
    }
    return false;
}

void AudioEngine::processRackTask (void* ctx, int index, int threadIndex)
{
    const rt::HostScope host;
    auto& self = *static_cast<AudioEngine*> (ctx);
    (*self.currentStage)[(size_t) index]->process (self.cbInputs, self.cbNumInputs, self.cbNumSamples, threadIndex);
}

void AudioEngine::audioDeviceIOCallbackWithContext (const float* const* inputs, int numInputs,
                                                    float* const* outputs, int numOutputs,
                                                    int numSamples, const juce::AudioIODeviceCallbackContext& context)
{
    const rt::HostScope host;
    const juce::ScopedNoDenormals noDenormals;
    const auto t0 = juce::Time::getHighResolutionTicks();

    for (int ch = 0; ch < numOutputs; ++ch)
        if (outputs[ch] != nullptr)
            juce::FloatVectorOperations::clear (outputs[ch], numSamples);

    const auto callbackIndex = callbackCount.fetch_add (1, std::memory_order_relaxed);

    // the message thread holds this only to swap a new graph in (see applyGraphLocked)
    const bool locked = rt::enterWithin (racksLock, 60.0);
    const rt::ScopedExit unlock { locked ? &racksLock : nullptr };

    if (! locked)
    {
        seenLockMisses = rt::lockMisses.load (std::memory_order_relaxed);   // counted here, once
        dropouts.note (callbackIndex, context.hostTimeNs != nullptr ? *context.hostTimeNs : (juce::uint64) 0, numSamples, sampleRate,
                       0.0, -1, 0.0f, true);
        return;
    }

    armWakeAssist (callbackIndex);

    for (auto& r : racks)
        r->callbackIndex = callbackIndex;

    // device channel numbering (what the I/O menus show) -> packed callback buffers
    for (int ch = 0; ch < numDeviceInputs; ++ch)
    {
        const auto idx = inputMap[(size_t) ch];
        deviceIns[(size_t) ch] = juce::isPositiveAndBelow (idx, numInputs) ? inputs[idx] : nullptr;
    }

    for (int ch = 0; ch < numDeviceOutputs; ++ch)
    {
        const auto idx = outputMap[(size_t) ch];
        deviceOuts[(size_t) ch] = juce::isPositiveAndBelow (idx, numOutputs) ? outputs[idx] : nullptr;
    }

    cbInputs = deviceIns.data();
    cbNumInputs = numDeviceInputs;
    cbNumSamples = numSamples;

    // tempo: beat position of this block for every tempo (the racks' play heads), audio taps.
    // CoreAudio's host time and nowSec() both count mach_absolute_time.
    {
        const auto hostNs = context.hostTimeNs != nullptr ? *context.hostTimeNs : (juce::uint64) 0;
        // tap tempo switched off in Features: play heads only, no audio onset detectors
        tempo.processBlock (deviceIns.data(), features::has (features::tempo) ? numDeviceInputs : 0, numSamples,
                            hostNs != 0 ? (double) hostNs * 1.0e-9 : TempoEngine::nowSec(), hostNs);
    }

    // tempo audio taps taken from racks: tell each rack which points to copy this block
    for (auto& r : racks)
    {
        int mask = 0;
        for (int i = 0; features::has (features::tempo) && i < tempo.getNumTempos(); ++i)
            if (tempo.getAudioRackId (i) == r->getId())
                mask |= 1 << tempo.getAudioPoint (i);
        r->tempoTapMask.store (mask, std::memory_order_relaxed);
    }

    // Solo: a soloed rack is heard with what it feeds (its buses); the racks feeding a soloed
    // rack or bus still reach it, but are not heard on their own.
    bool anySolo = false;
    for (auto& r : racks)
        anySolo = anySolo || r->solo.load (std::memory_order_relaxed);

    for (auto& r : racks)
    {
        r->anySolo = anySolo;
        r->soloHeard = r->soloFeeds = anySolo && r->solo.load (std::memory_order_relaxed);
    }

    if (anySolo)
    {
        for (const auto& stageRacks : stages)
            for (auto* r : stageRacks)
                for (const auto& f : r->feeds)
                    if (f.src->soloHeard && r->feedIsOn (f))
                        r->soloHeard = true;

        for (auto st = stages.rbegin(); st != stages.rend(); ++st)
            for (auto* r : *st)
                if (r->soloFeeds)
                    for (const auto& f : r->feeds)
                        if (r->feedIsOn (f))
                            f.src->soloFeeds = true;
    }

    const bool hopFirst = hopFirstOrder.load (std::memory_order_relaxed);

    for (const auto& stageRacks : stages)
    {
        currentStage = &stageRacks;

        if (hopFirst && stageRacks.size() > 1 && stageRacks.size() <= orderedStage.capacity())
        {
            orderedStage.clear();   // capacity kept: no allocation

            for (auto* r : stageRacks)
                if (hasHopNow (*r, callbackIndex))
                    orderedStage.push_back (r);

            if (! orderedStage.empty() && orderedStage.size() < stageRacks.size())
            {
                for (auto* r : stageRacks)
                    if (! hasHopNow (*r, callbackIndex))
                        orderedStage.push_back (r);

                currentStage = &orderedStage;
            }
        }

        pool.run ((int) stageRacks.size(), &AudioEngine::processRackTask, this, parallelMode.load (std::memory_order_relaxed));
    }

    // Adaptive fan-out: waking worker threads costs a few microseconds, so with a light
    // load it is cheaper to stay on the IO thread. The decision follows the *peak* load of
    // the last seconds (live audio is about the worst block, e.g. FFT plugins), with hysteresis.
    if (forceParallel)
    {
        parallelMode.store (true, std::memory_order_relaxed);
    }
    else
    {
        double workMs = 0.0, heaviestMs = 0.0;

        for (auto& r : racks)
        {
            const auto ms = (double) r->callbackMs.load (std::memory_order_relaxed);
            workMs += ms;
            heaviestMs = juce::jmax (heaviestMs, ms);
        }

        const auto budgetMs = 1000.0 * numSamples / juce::jmax (1.0, sampleRate);
        const auto decay = std::pow (0.5, numSamples / (2.0 * juce::jmax (1.0, sampleRate))); // ~2 s half-life
        peakParallelGainMs = juce::jmax (workMs - heaviestMs, peakParallelGainMs * decay);
        peakSerialWorkMs = juce::jmax (workMs, peakSerialWorkMs * decay);

        // Parallel only pays when the serial work is a real share of the budget: otherwise the
        // IO thread does it all with a wide margin, and waking the workers every callback (3000
        // times/s at 16 samples, each spinning after its batch) costs more than it saves. At
        // small buffers a fraction of the budget alone is tiny (8% of 16 samples = 27 us, which
        // twenty racks with no plugins already exceeded).
        if (parallelMode.load (std::memory_order_relaxed))
        {
            if (peakParallelGainMs < budgetMs * 0.04 || peakSerialWorkMs < budgetMs * 0.20)
                parallelMode.store (false, std::memory_order_relaxed);
        }
        else if (peakParallelGainMs > budgetMs * 0.08 && peakSerialWorkMs > budgetMs * 0.30)
        {
            parallelMode.store (true, std::memory_order_relaxed);
        }
    }

    // the racks ran: tempo detectors on their audio (same block, so the beat time is exact)
    for (int i = 0; features::has (features::tempo) && i < tempo.getNumTempos(); ++i)
    {
        if (const auto id = tempo.getAudioRackId (i); id >= 0)
        {
            const float* x = nullptr;
            for (auto& r : racks)
                if (r->getId() == id)
                    x = r->getTempoTap (tempo.getAudioPoint (i));
            tempo.detectRackAudio (i, x, numSamples);
        }
    }

    computeAutomixGains (anySolo);

    // dest += src * gain, the gain ramping linearly over the block (automix: no zipper noise)
    auto addRamped = [numSamples] (float* dest, const float* src, float g0, float g1)
    {
        if (g0 == g1)
        {
            juce::FloatVectorOperations::addWithMultiply (dest, src, g0, numSamples);
            return;
        }

        const auto step = (g1 - g0) / (float) numSamples;

        for (int i = 0; i < numSamples; ++i)
            dest[i] += src[i] * (g0 + step * (float) (i + 1));
    };

    for (auto& r : racks)
    {
        // automix gain of this block: from the last one to the new target
        const auto g0 = r->automixGain, g1 = r->automixTarget;
        r->automixGain = g1;

        if (! r->producedAudio() || (anySolo && ! r->soloHeard))
            continue;

        const auto& buf = r->getOutput();

        // one device destination: a pair from outCh, or (L+R)/2 on one channel
        auto toDevice = [&] (int outCh, bool stereo)
        {
            if (! stereo)
            {
                if (juce::isPositiveAndBelow (outCh, numDeviceOutputs) && deviceOuts[(size_t) outCh] != nullptr)
                {
                    addRamped (deviceOuts[(size_t) outCh], buf.getReadPointer (0), 0.5f * g0, 0.5f * g1);
                    addRamped (deviceOuts[(size_t) outCh], buf.getReadPointer (1), 0.5f * g0, 0.5f * g1);
                }
                return;
            }

            for (int i = 0; i < 2; ++i)
                if (juce::isPositiveAndBelow (outCh + i, numDeviceOutputs) && deviceOuts[(size_t) (outCh + i)] != nullptr)
                    addRamped (deviceOuts[(size_t) (outCh + i)], buf.getReadPointer (i), g0, g1);
        };

        // main destination: the device, unless routed into a bus (the bus sums it) or "OUT: none"
        int mainCode = -1;

        if (! r->outToBus)
            if (const int outCh = r->outputChannel.load (std::memory_order_relaxed); outCh >= 0)
            {
                const bool stereo = r->stereoOutput.load (std::memory_order_relaxed);
                toDevice (outCh, stereo);
                mainCode = (stereo ? Rack::outStereo : Rack::outMono) + outCh;
            }

        // extra device destinations (extra buses are feeds of those buses); one that became the
        // main output (consecutive outputs) is not summed twice
        for (auto& o : r->extraOutputs)
        {
            const auto c = o.load (std::memory_order_relaxed);

            if (c == mainCode)
                continue;

            if (c >= Rack::outStereo && c < Rack::outMono)   toDevice (c - Rack::outStereo, true);
            else if (c >= Rack::outMono && c < Rack::outBus) toDevice (c - Rack::outMono, false);
        }
    }

    // master gain + meters on the main pair
    masterGain.setTargetValue (masterMuted.load (std::memory_order_relaxed)
                                   ? 0.0f : juce::Decibels::decibelsToGain (masterGainDb.load (std::memory_order_relaxed), -100.0f));

    if (masterGain.isSmoothing() || masterGain.getTargetValue() != 1.0f)
    {
        const auto start = masterGain.getCurrentValue();
        masterGain.skip (numSamples);
        const auto end = masterGain.getCurrentValue();

        for (int ch = 0; ch < numOutputs; ++ch)
            if (outputs[ch] != nullptr)
            {
                juce::AudioBuffer<float> view (outputs + ch, 1, numSamples);
                view.applyGainRamp (0, 0, numSamples, start, end);
            }
    }

    if (outputsBlocked.load (std::memory_order_relaxed))
        for (int ch = 0; ch < numOutputs; ++ch)
            if (outputs[ch] != nullptr)
                juce::FloatVectorOperations::clear (outputs[ch], numSamples);

    const auto elapsed = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0);
    const auto budget = numSamples / juce::jmax (1.0, sampleRate);
    const auto load = elapsed / budget;

    {
        int heaviestId = -1;
        float heaviestMs = 0.0f;
        for (auto& r : racks)
            if (const auto ms = r->callbackMs.load (std::memory_order_relaxed); ms > heaviestMs)
            {
                heaviestMs = ms;
                heaviestId = r->getId();
            }

        // a rack that gave up waiting for its own lock (plugin swapped in at that instant) was
        // silent for this block: it counts as a dropout like a skipped engine block
        const auto misses = rt::lockMisses.load (std::memory_order_relaxed);
        const bool rackSilent = misses != seenLockMisses;
        seenLockMisses = misses;

        dropouts.note (callbackIndex, context.hostTimeNs != nullptr ? *context.hostTimeNs : (juce::uint64) 0,
                       numSamples, sampleRate, elapsed, heaviestId, heaviestMs, rackSilent);
    }
    const auto prev = callbackLoad.load (std::memory_order_relaxed);
    callbackLoad.store (load > prev ? load : prev * 0.9 + load * 0.1, std::memory_order_relaxed);
    averageLoad.store (averageLoad.load (std::memory_order_relaxed) * 0.998 + load * 0.002, std::memory_order_relaxed);
}

//==============================================================================
void AudioEngine::handleIncomingMidiMessage (juce::MidiInput* source, const juce::MidiMessage& m)
{
    if (features::has (features::midi) && midiRemote != nullptr && source != nullptr
        && midiRemote (source->getIdentifier(), source->getName(), m))
        return;   // a MIDI mapping uses it

    if (features::has (features::tempo))
        tempo.handleMidi (m);   // taps (note / CC), clock, learn

    if (! features::has (features::snapshots))
        return;

    if (m.isProgramChange())
    {
        const auto program = m.getProgramChangeNumber();
        juce::MessageManager::callAsync ([this, program] { if (onProgramChange) onProgramChange (program); });
    }
    else if (m.isController() && m.getControllerValue() > 63)
    {
        const auto cc = m.getControllerNumber();
        const int delta = cc == nextSceneCC.load() ? 1 : (cc == prevSceneCC.load() ? -1 : 0);

        if (delta != 0)
            juce::MessageManager::callAsync ([this, delta] { if (onSceneStep) onSceneStep (delta); });
    }
}

} // namespace stagerack
