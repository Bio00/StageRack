#include "Session.h"
#include "MidiControl.h"
#include <deque>

namespace stagerack
{

static const juce::Identifier idRoot ("STAGERACK"), idRacks ("RACKS"), idRack ("RACK"), idPlugin ("PLUGIN"),
                              idScenes ("SCENES"), idScene ("SCENE"), idSlot ("SLOT");

static juce::String stateOf (juce::AudioPluginInstance& p)
{
    juce::MemoryBlock mb;
    p.getStateInformation (mb);
    return mb.toBase64Encoding();
}

static void restoreState (juce::AudioPluginInstance& p, const juce::String& base64)
{
    if (base64.isEmpty())
        return;

    juce::MemoryBlock mb;
    if (mb.fromBase64Encoding (base64) && mb.getSize() > 0)
        p.setStateInformation (mb.getData(), (int) mb.getSize());
}

//==============================================================================
/** Drives Session::respreadLive(): one plugin at a time, on the message thread. */
class Session::LiveRespreader final : private juce::Timer
{
public:
    explicit LiveRespreader (Session& s) : session (s) {}
    ~LiveRespreader() override { stopTimer(); *alive = false; }

    // Twin runs this long on the live input before the crossfade (its adaptive state settles),
    // then the output crossfades over fadeSeconds.
    static constexpr double warmSeconds = 2.0, fadeSeconds = 0.08;

    void start()
    {
        cancel();
        auto& engine = session.engine;

        for (auto* rack : engine.alignmentOrder())   // a master before the tracks feeding it
            for (int i = 0; i < Rack::maxSlots; ++i)
                if (auto* p = rack->getPlugin (i))
                    jobs.push_back ({ rack->getId(), i, p });

        total = (int) jobs.size();
        done = 0;
        changed = 0;
        startTimerHz (30);
        next();
    }

    void cancel()
    {
        jobs.clear();
        *alive = false;
        alive = std::make_shared<bool> (true);
        state = State::idle;
        // a hand-over already running is left to finish on its own (it is harmless); the
        // timer keeps collecting it
    }

    /** Queues single plugins (found by the hop watch) without cancelling what is running. */
    void add (const std::vector<std::pair<int, int>>& slots)
    {
        const bool wasIdle = ! isBusy();

        if (wasIdle)
        {
            total = 0;
            done = 0;
            changed = 0;
        }

        for (auto& [rackId, slot] : slots)
        {
            auto* rack = session.engine.findRack (rackId);
            auto* p = rack != nullptr ? rack->getPlugin (slot) : nullptr;

            if (p == nullptr || (state != State::idle && current.original == p)
                || std::any_of (jobs.begin(), jobs.end(), [p] (const Job& j) { return j.original == p; }))
                continue;

            jobs.push_back ({ rackId, slot, p });
            ++total;
        }

        if (wasIdle && ! jobs.empty())
        {
            startTimerHz (30);
            next();
        }
    }

    bool isBusy() const noexcept { return state != State::idle || ! jobs.empty(); }

    juce::String status() const
    {
        if (! isBusy())
            return total > 0 ? "Re-spread done: " + juce::String (changed) + " plugin(s) moved, no dropout" : juce::String();
        return "Re-spreading live " + juce::String (juce::jmin (done + 1, total)) + "/" + juce::String (total) + "...";
    }

private:
    struct Job { int rackId, slot; juce::AudioPluginInstance* original; };
    enum class State { idle, creating, handingOver };

    Session& session;
    std::deque<Job> jobs;
    Job current {};
    juce::AudioPluginInstance* twinPtr = nullptr;
    State state = State::idle;
    int total = 0, done = 0, changed = 0;
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);

    Rack* currentRack() const
    {
        auto* r = session.engine.findRack (current.rackId);
        return r != nullptr && r->getPlugin (current.slot) != nullptr ? r : nullptr;
    }

    static void copyParameters (juce::AudioPluginInstance& from, juce::AudioPluginInstance& to)
    {
        const auto& a = from.getParameters();
        const auto& b = to.getParameters();

        for (int i = 0; i < juce::jmin (a.size(), b.size()); ++i)
            if (const auto v = a[i]->getValue(); std::abs (v - b[i]->getValue()) > 1.0e-6f)
                b[i]->setValue (v);
    }

    void next()
    {
        state = State::idle;
        twinPtr = nullptr;

        while (! jobs.empty())
        {
            current = jobs.front();
            jobs.pop_front();
            ++done;

            auto* rack = session.engine.findRack (current.rackId);
            if (rack == nullptr || rack->getPlugin (current.slot) != current.original
                || rack->isHandingOver (current.slot))
                continue;

            state = State::creating;
            const auto token = alive;
            session.plugins.createPlugin (current.original->getPluginDescription(), rack->getSampleRate(), rack->getBlockSize(),
                [this, token] (std::unique_ptr<juce::AudioPluginInstance> twin, const juce::String&)
                {
                    if (*token)
                        twinCreated (std::move (twin));
                });
            return;
        }

        if (! hasPendingRetired())
            stopTimer();
    }

    void twinCreated (std::unique_ptr<juce::AudioPluginInstance> twin)
    {
        auto* rack = session.engine.findRack (current.rackId);

        // A twin that is not used is parked like every instance removed while the session is
        // open, never destroyed here: it has just run (alignment), and destroying a De-Feedback
        // instance right after it ran corrupts the heap (crash in its helper's cblas_sgemm,
        // reproduced with 12 instances @16 samples).
        auto skip = [this, &twin]
        {
            if (twin != nullptr)
                session.retirePlugin (std::move (twin));
            next();
        };

        if (twin == nullptr || rack == nullptr || rack->getPlugin (current.slot) != current.original)
            return skip();

        auto& slot = rack->getSlot (current.slot);
        Rack::configurePlugin (*twin, rack->getSampleRate(), rack->getBlockSize());
        restoreState (*twin, stateOf (*current.original));
        copyParameters (*current.original, *twin);

        int hop = 0, phase = -1;
        juce::uint64 firstCallback = 0;
        session.engine.alignNewPlugin (*twin, *rack, hop, phase, &slot, &firstCallback);

        // no periodic hop, or already where it should be: nothing to move
        if (hop == 0 || (slot.hopCalls == hop && slot.hopPhase == phase))
            return skip();

        copyParameters (*current.original, *twin);   // in case they moved during the measurement
        auto* raw = twin.get();
        const auto sr = rack->getSampleRate();

        if (! rack->beginHandover (current.slot, twin, hop, phase, firstCallback, (int) (warmSeconds * sr), (int) (fadeSeconds * sr)))
            return skip();   // beginHandover leaves the twin with us when it refuses

        twinPtr = raw;
        state = State::handingOver;
    }

    bool hasPendingRetired() const
    {
        for (int r = 0; r < session.engine.getNumRacks(); ++r)
            for (int i = 0; i < Rack::maxSlots; ++i)
                if (session.engine.getRack (r).isHandingOver (i))
                    return true;
        return false;
    }

    void deleteOnMessageThread (std::unique_ptr<juce::AudioPluginInstance> p)
    {
        session.retirePlugin (std::move (p));   // parked, not destroyed, while the session is open
    }

    void timerCallback() override
    {
        auto& engine = session.engine;

        // instances dropped by an interrupted hand-over, and finished ones of a cancelled run
        for (int r = 0; r < engine.getNumRacks(); ++r)
        {
            auto& rack = engine.getRack (r);
            for (auto& p : rack.takeDiscarded())
                deleteOnMessageThread (std::move (p));

            if (state != State::handingOver || rack.getId() != current.rackId)
                for (int i = 0; i < Rack::maxSlots; ++i)
                    if (auto old = rack.collectHandover (i))
                    {
                        session.pluginReplaced (old.get(), rack.getPlugin (i));
                        deleteOnMessageThread (std::move (old));
                        session.notify();
                    }
        }

        if (state != State::handingOver)
        {
            if (state == State::idle && jobs.empty() && ! hasPendingRetired())
                stopTimer();
            return;
        }

        auto* rack = engine.findRack (current.rackId);

        if (rack == nullptr || ! rack->isHandingOver (current.slot))
            return next();   // rack deleted, or the slot was changed meanwhile

        if (rack->getPlugin (current.slot) == current.original)
        {
            copyParameters (*current.original, *twinPtr);   // the user may still be tweaking it
            return;
        }

        if (auto old = rack->collectHandover (current.slot))
        {
            ++changed;
            session.pluginReplaced (old.get(), twinPtr);
            deleteOnMessageThread (std::move (old));
            session.notify();
            next();
        }
    }
};

Session::Session (AudioEngine& e, PluginManager& p) : engine (e), plugins (p)
{
    engine.onProgramChange = [this] (int program) { recallScene (program); };
    engine.onSceneStep = [this] (int delta) { stepScene (delta); };
    respreader = std::make_unique<LiveRespreader> (*this);
}

Session::~Session() = default;

void Session::respreadLive()                    { respreader->start(); }
void Session::watchHops()
{
    // While a twin is being aligned and runs next to the original, the CPU is busier and a
    // plugin helper (De-Feedback) can finish late: its hop cost then shows up one callback
    // later and the watch would "see" an instance move that did not (measured: a master moved
    // back and forth). Watch only when nothing is being handed over, and 1.5 s after.
    const auto now = juce::Time::getMillisecondCounterHiRes();

    if (respreader->isBusy())
    {
        watchQuietUntilMs = now + 1500.0;
        return;
    }

    if (now < watchQuietUntilMs)
        return;

    if (auto list = engine.watchHops(); ! list.empty())
        respreader->add (list);
}
void Session::cancelRespread()                  { respreader->cancel(); }
bool Session::isRespreading() const noexcept    { return respreader->isBusy(); }
juce::String Session::getRespreadStatus() const
{
    auto s = respreader->status();
    if (! retired.empty())
        s << (s.isEmpty() ? "" : "  |  ") << (int) retired.size() << " old instance(s) parked (kept for undo, freed after "
          << (int) (parkedQuietMs / 1000) << " s when not needed, at most " << maxParkedPlugins << ")";
    return s;
}

int Session::addTempo()
{
    const auto i = engine.tempo.addTempo();
    if (i > 0) markDirty();
    return i;
}

void Session::removeTempo (int index)
{
    if (index <= 0 || index >= engine.tempo.getNumTempos())
        return;

    engine.tempo.removeTempo (index);

    for (int r = 0; r < engine.getNumRacks(); ++r)
    {
        auto& rack = engine.getRack (r);
        const auto t = rack.tempoIndex.load();
        if (t == index)     rack.tempoIndex = 0;
        else if (t > index) rack.tempoIndex = t - 1;
    }

    // scenes keep their tempos under the new numbers
    for (auto& sc : scenes)
    {
        auto data = sc.data.createCopy();
        data.removeProperty ("tempo" + juce::String (index), nullptr);
        data.removeProperty ("tempoRatio" + juce::String (index), nullptr);

        for (int i = index + 1; i < TempoEngine::maxTempos; ++i)
            for (auto* key : { "tempo", "tempoRatio" })
                if (const juce::Identifier from (key + juce::String (i)); data.hasProperty (from))
                {
                    data.setProperty (key + juce::String (i - 1), data[from], nullptr);
                    data.removeProperty (from, nullptr);
                }

        sc.data = data;
    }

    markDirty();
}

void Session::markDirty()
{
    normalizeFolders();
    dirty = true;
    recordUndoStep();
    notify();
}

//==============================================================================
Rack& Session::addRack()
{
    addRacks (1, false);
    return engine.getRack (engine.getNumRacks() - 1);
}

void Session::followInput (Rack& r, bool routingActive)
{
    if (! outputsFollowInputs || r.inputRackId.load() >= 0 || r.inputChannel.load() < 0)
        return;

    if (r.outputChannel.load() < 0 || (routingActive && r.outputBus.load() > 0))
        return;   // no output or into a bus: a deliberate choice, kept

    r.outputChannel = r.inputChannel.load();
    r.stereoOutput = r.stereoInput.load();
}

void Session::setRoutingEnabled (bool on)
{
    if (on == engine.isRoutingEnabled())
        return;

    engine.setRoutingEnabled (on);
    markDirty();
}

Rack* Session::addBus()
{
    if (! features::has (features::routing))
        return nullptr;   // SETTINGS > Features: buses and sends are off

    const auto letter = engine.firstFreeBus();

    if (letter == 0)
        return nullptr;

    engine.setRoutingEnabled (true);

    // outputs: like the last rack (a bus usually goes to the main outputs); no device input
    int out = 0;
    bool stereoOut = true;
    if (engine.getNumRacks() > 0)
    {
        auto& last = engine.getRack (engine.getNumRacks() - 1);
        out = last.outputChannel.load();
        stereoOut = last.stereoOutput.load();
    }

    auto& r = engine.addRack();
    r.name = "Bus " + juce::String::charToString ((juce::juce_wchar) ('A' + letter - 1));
    r.inputChannel = -1;
    r.outputChannel = out;
    r.stereoOutput = stereoOut;
    engine.setRackBus (r, letter);
    markDirty();
    return &r;
}

//==============================================================================
void Session::normalizeFolders()
{
    const int n = engine.getNumRacks();

    // a member only of an existing folder, never (through nesting) of itself
    for (int i = 0; i < n; ++i)
    {
        auto& r = engine.getRack (i);

        if (r.folderId < 0)
            continue;

        auto* f = engine.findRack (r.folderId);
        bool ok = f != nullptr && f->isFolder && f != &r;

        for (int depth = 0; ok && f->folderId >= 0 && depth <= n; ++depth)
        {
            f = engine.findRack (f->folderId);
            ok = f != nullptr && f != &r;
        }

        if (! ok)
            r.folderId = -1;
    }

    // order: each folder followed by its members (in their current order), recursively
    std::vector<Rack*> order;
    std::function<void (int)> emit = [&] (int parent)
    {
        for (int i = 0; i < n; ++i)
            if (auto& r = engine.getRack (i); r.folderId == parent)
            {
                order.push_back (&r);
                if (r.isFolder) emit (r.getId());
            }
    };
    emit (-1);

    if ((int) order.size() != n)
        return;   // cannot happen after the check above; never lose a rack

    for (int i = 0; i < n; ++i)
        if (&engine.getRack (i) != order[(size_t) i])
            engine.moveRack (engine.indexOf (*order[(size_t) i]), i);
}

bool Session::isHiddenInFolder (const Rack& r) const
{
    auto* f = r.folderId >= 0 ? engine.findRack (r.folderId) : nullptr;

    for (int depth = 0; f != nullptr && depth <= engine.getNumRacks(); ++depth)
    {
        if (! f->folderOpen)
            return true;
        f = f->folderId >= 0 ? engine.findRack (f->folderId) : nullptr;
    }

    return false;
}

int Session::numFolderMembers (const Rack& folder) const
{
    int count = 0;
    for (int i = 0; i < engine.getNumRacks(); ++i)
        if (engine.getRack (i).folderId == folder.getId())
            ++count;
    return count;
}

Rack* Session::groupIntoFolder (const juce::Array<int>& rackIds)
{
    if (! features::has (features::routing))
        return nullptr;   // a folder is a bus

    // the racks in list order; a rack inside another chosen folder comes along with it
    std::vector<Rack*> members;
    for (int i = 0; i < engine.getNumRacks(); ++i)
        if (auto& r = engine.getRack (i); rackIds.contains (r.getId()))
            members.push_back (&r);

    auto insideChosen = [&] (const Rack& r)
    {
        for (auto* f = r.folderId >= 0 ? engine.findRack (r.folderId) : nullptr; f != nullptr;
             f = f->folderId >= 0 ? engine.findRack (f->folderId) : nullptr)
            if (rackIds.contains (f->getId()))
                return true;
        return false;
    };
    members.erase (std::remove_if (members.begin(), members.end(), [&] (Rack* r) { return insideChosen (*r); }), members.end());

    const auto letter = engine.firstFreeBus();

    if (members.empty() || letter == 0)
        return nullptr;

    engine.setRoutingEnabled (true);
    auto& first = *members.front();

    auto& f = engine.addRack();
    f.name = "Folder";                      // its own colour (the next in the palette): members show it on their strip
    f.isFolder = true;
    f.folderOpen = true;
    f.folderId = first.folderId;            // where the first one was (also inside a folder)
    f.inputChannel = -1;
    f.outputChannel = first.outputChannel.load();
    f.stereoOutput = first.stereoOutput.load();
    engine.setRackBus (f, letter);

    if (const auto ob = first.outputBus.load(); ob > 0)
        engine.setRackOutputBus (f, ob);    // the members went into a bus: so does the folder

    engine.moveRack (engine.indexOf (f), engine.indexOf (first));

    for (auto* m : members)
    {
        m->folderId = f.getId();
        engine.setRackOutputBus (*m, letter);   // refused if it would make a loop (the rack keeps its output)
    }

    markDirty();
    return &f;
}

void Session::releaseFolderMembers (Rack& folder)
{
    // members go up one level; those that played into the folder now go where it went
    for (int i = 0; i < engine.getNumRacks(); ++i)
    {
        auto& m = engine.getRack (i);

        if (m.folderId != folder.getId())
            continue;

        m.folderId = folder.folderId;

        if (folder.busIndex.load() > 0 && m.outputBus.load() == folder.busIndex.load())
        {
            m.outputChannel = folder.outputChannel.load();
            m.stereoOutput = folder.stereoOutput.load();
            if (! engine.setRackOutputBus (m, folder.outputBus.load()))
                engine.setRackOutputBus (m, 0);
        }
    }
}

void Session::ungroupFolder (Rack& folder)
{
    if (! folder.isFolder)
        return;

    releaseFolderMembers (folder);
    removeRack (folder);
}

void Session::removeFromFolder (Rack& r)
{
    auto* folder = r.folderId >= 0 ? engine.findRack (r.folderId) : nullptr;

    if (folder == nullptr)
        return;

    r.folderId = folder->folderId;

    if (folder->busIndex.load() > 0 && r.outputBus.load() == folder->busIndex.load())
    {
        r.outputChannel = folder->outputChannel.load();
        r.stereoOutput = folder->stereoOutput.load();
        if (! engine.setRackOutputBus (r, folder->outputBus.load()))
            engine.setRackOutputBus (r, 0);
    }

    // normalizeFolders (in markDirty) puts it right after the folder's block: it is still
    // inside the block, so it is the first rack of the folder's level after it
    markDirty();
}

void Session::moveIntoFolder (Rack& r, Rack& folder)
{
    if (! folder.isFolder || &r == &folder)
        return;

    for (auto* f = &folder; f != nullptr; f = f->folderId >= 0 ? engine.findRack (f->folderId) : nullptr)
        if (f == &r)
            return;   // a folder into one of its own members

    r.folderId = folder.getId();
    if (engine.isRoutingEnabled() && folder.busIndex.load() > 0)
        engine.setRackOutputBus (r, folder.busIndex.load());
    folder.folderOpen = true;
    markDirty();
}

void Session::setFolderOpen (Rack& folder, bool open)
{
    folder.folderOpen = open;   // a view setting: saved, not an undo step
    dirty = true;
    notify();
}

void Session::setRackView (const RackView& v)
{
    if (v == rackView)
        return;

    rackView = v;   // a view setting: saved, not an undo step
    dirty = true;
    notify();
}

void Session::setRackSendsShown (Rack& r, int mask)
{
    r.uiSendsShown = mask < 0 ? -1 : (mask & 0xff);
    dirty = true;
    notify();
}

int Session::hiddenBits (const RackView& v) noexcept
{
    return (v.io ? 0 : 1) | (v.input ? 0 : 2) | (v.inserts ? 0 : 4) | (v.sends ? 0 : 8) | (v.info ? 0 : 16);
}

Session::RackView Session::withHidden (RackView v, int hidden) noexcept
{
    v.io = (hidden & 1) == 0;
    v.input = (hidden & 2) == 0;
    v.inserts = (hidden & 4) == 0;
    v.sends = (hidden & 8) == 0;
    v.info = (hidden & 16) == 0;
    return v;
}

void Session::setRackHiddenSections (Rack& r, int hidden)
{
    r.uiHideSections = hidden < 0 ? -1 : (hidden & 31);   // a view setting: saved, not an undo step
    dirty = true;
    notify();
}

void Session::addRacks (int count, bool stereoInputs)
{
    int nextIn = 0, out = 0;
    bool stereoOut = true;

    for (int i = 0; i < engine.getNumRacks(); ++i)
    {
        auto& r = engine.getRack (i);

        if (r.inputRackId.load() < 0 && r.inputChannel.load() >= 0)
            nextIn = juce::jmax (nextIn, r.inputChannel.load() + (r.stereoInput.load() ? 2 : 1));

        out = r.outputChannel.load();
        stereoOut = r.stereoOutput.load();
    }

    for (int i = 0; i < count; ++i)
    {
        auto& r = engine.addRack();
        r.inputChannel = nextIn;
        r.stereoInput = stereoInputs;
        r.outputChannel = outputsFollowInputs ? nextIn : out;
        r.stereoOutput = outputsFollowInputs ? stereoInputs : stereoOut;
        nextIn += stereoInputs ? 2 : 1;
    }

    markDirty();
}

void Session::removeRack (Rack& rack)
{
    if (rack.isFolder)
        releaseFolderMembers (rack);   // the members stay (out of the folder)

    for (int i = 0; i < Rack::maxSlots; ++i)
        if (auto* p = rack.getPlugin (i); p != nullptr && onPluginAboutToBeDeleted)
            onPluginAboutToBeDeleted (p);

    if (! closingSession)   // live edit: park the instances instead of destroying them with the rack
    {
        for (int i = 0; i < Rack::maxSlots; ++i)
            retirePlugin (rack.setPlugin (i, nullptr));
        for (auto& p : rack.takeDiscarded())
            retirePlugin (std::move (p));
    }

    engine.removeRack (rack);
    markDirty();
}

void Session::loadPlugin (Rack& rack, int slot, const juce::PluginDescription& desc,
                          std::function<void (const juce::String&)> done, std::shared_ptr<bool> sharedUndoStep)
{
    const auto rackId = rack.getId();
    const auto sr = rack.getSampleRate();
    const auto bs = rack.getBlockSize();

    plugins.createPlugin (desc, sr, bs, [this, rackId, slot, done, sharedUndoStep] (std::unique_ptr<juce::AudioPluginInstance> p, const juce::String& error)
    {
        auto* target = engine.findRack (rackId);

        if (p == nullptr || target == nullptr)
        {
            if (done) done (error.isNotEmpty() ? error : juce::String ("Rack no longer exists"));
            return;
        }

        Rack::configurePlugin (*p, target->getSampleRate(), target->getBlockSize());
        PluginManager::applyInsertDefaults (*p);   // e.g. EverClean: on (before the hop is measured)

        int hopCalls = 0, hopPhase = -1;
        engine.alignNewPlugin (*p, *target, hopCalls, hopPhase);

        if (auto* old = target->getPlugin (slot); old != nullptr && onPluginAboutToBeDeleted)
            onPluginAboutToBeDeleted (old);

        retirePlugin (target->setPlugin (slot, std::move (p))); // previous plugin is parked
        target->getSlot (slot).hopCalls = hopCalls;
        target->getSlot (slot).hopPhase = hopPhase;

        if (sharedUndoStep != nullptr && *sharedUndoStep)
        {
            dirty = true;
            amendUndoState();   // same insert on several racks: one undo step
            notify();
        }
        else
        {
            if (sharedUndoStep != nullptr)
                *sharedUndoStep = true;
            markDirty();
        }

        if (done) done ({});
    });
}

void Session::removePlugin (Rack& rack, int slot)
{
    if (auto* p = rack.getPlugin (slot); p != nullptr && onPluginAboutToBeDeleted)
        onPluginAboutToBeDeleted (p);

    retirePlugin (rack.setPlugin (slot, nullptr));
    markDirty();
}

void Session::removePlugins (const std::vector<std::pair<Rack*, int>>& where)
{
    for (auto& [r, slot] : where)
        retirePlugin (r->setPlugin (slot, nullptr));   // retirePlugin closes the editor

    if (! where.empty())
        markDirty();
}

void Session::retirePlugin (std::unique_ptr<juce::AudioPluginInstance> p)
{
    if (p == nullptr)
        return;

    if (onPluginAboutToBeDeleted)
        onPluginAboutToBeDeleted (p.get());   // closes its editor window

    if (closingSession)
        p.reset();
    else
    {
        retiredAt[p.get()] = juce::Time::getMillisecondCounter();
        retired.push_back (std::move (p));
    }
}

void Session::releaseRetiredPlugins()
{
    // destroyed one at a time, newest first (same order as a normal teardown)
    while (! retired.empty())
        retired.pop_back();
    retiredAt.clear();
}

//==============================================================================
// sends as "dB,dB,..." (one per bus) + a pre-fader bitmask
static juce::String sendsToString (const Rack& r)
{
    juce::StringArray a;
    for (auto& snd : r.sends)
        a.add (juce::String (snd.db.load(), 1));
    return a.joinIntoString (",");
}

static int sendPreMask (const Rack& r)
{
    int m = 0;
    for (int k = 0; k < Rack::maxBuses; ++k)
        if (r.sends[(size_t) k].pre.load())
            m |= 1 << k;
    return m;
}

static void sendsFromTree (Rack& r, const juce::ValueTree& t)
{
    const auto a = juce::StringArray::fromTokens (t["sends"].toString(), ",", "");
    const int pre = t["sendPre"];

    for (int k = 0; k < Rack::maxBuses; ++k)
    {
        r.sends[(size_t) k].db = k < a.size() ? juce::jlimit (Rack::sendOffDb, 12.0f, a[k].getFloatValue()) : Rack::sendOffDb;
        r.sends[(size_t) k].pre = (pre & (1 << k)) != 0;
    }
}

juce::ValueTree Session::captureSceneData() const
{
    juce::ValueTree data (idScene);

    for (int i = 0; i < engine.getNumRacks(); ++i)
    {
        auto& r = engine.getRack (i);
        juce::ValueTree rt (idRack);
        rt.setProperty ("id", r.getId(), nullptr)
          .setProperty ("gain", r.gainDb.load(), nullptr)
          .setProperty ("inGain", r.inputGainDb.load(), nullptr)
          .setProperty ("mute", r.muted.load(), nullptr)
          .setProperty ("bypass", r.bypassed.load(), nullptr)
          .setProperty ("solo", r.solo.load(), nullptr)
          .setProperty ("amGroup", r.automixGroup.load(), nullptr)
          .setProperty ("amWeight", r.automixWeightDb.load(), nullptr)
          .setProperty ("sends", sendsToString (r), nullptr)
          .setProperty ("sendPre", sendPreMask (r), nullptr);

        for (int s = 0; s < Rack::maxSlots; ++s)
        {
            if (auto* p = r.getPlugin (s))
            {
                juce::ValueTree st (idSlot);
                st.setProperty ("index", s, nullptr)
                  .setProperty ("uid", p->getPluginDescription().createIdentifierString(), nullptr)
                  .setProperty ("bypass", r.getSlot (s).bypassed.load(), nullptr)
                  .setProperty ("state", stateOf (*p), nullptr);
                rt.appendChild (st, nullptr);
            }
        }

        data.appendChild (rt, nullptr);
    }

    data.setProperty ("master", engine.masterGainDb.load(), nullptr)
        .setProperty ("automix", engine.automixEnabled.load(), nullptr)
        .setProperty ("amOverlap", engine.automixOverlap.load(), nullptr);
    engine.tempo.storeInScene (data);
    return data;
}

void Session::applySceneData (const juce::ValueTree& data)
{
    const auto busSendsBefore = engine.busSendMask();
    engine.tempo.recallFromScene (data);   // song tempo (tempos set to be recalled)

    if (data.hasProperty ("automix"))
        engine.automixEnabled = (bool) data["automix"];
    if (data.hasProperty ("amOverlap"))
        engine.automixOverlap = juce::jlimit (0, 2, (int) data["amOverlap"]);

    for (const auto& rt : data)
    {
        auto* r = engine.findRack (rt["id"]);

        if (r == nullptr)
            continue;

        r->gainDb = (float) rt["gain"];
        if (rt.hasProperty ("inGain"))   // scenes stored before the input gain leave it alone
            r->inputGainDb = (float) rt["inGain"];
        r->muted = (bool) rt["mute"];
        r->bypassed = (bool) rt["bypass"];
        r->solo = (bool) rt["solo"];

        if (rt.hasProperty ("amGroup"))   // scenes stored before the automixer leave it alone
        {
            r->automixGroup = juce::jlimit (0, Rack::maxAutomixGroups, (int) rt["amGroup"]);
            r->automixWeightDb = (float) rt["amWeight"];
        }

        if (rt.hasProperty ("sends"))   // scenes stored before the sends leave them alone
            sendsFromTree (*r, rt);

        for (const auto& st : rt)
        {
            const int s = st["index"];
            auto* p = r->getPlugin (s);

            // Scenes recall settings; they never change which plugins are loaded.
            if (p == nullptr || p->getPluginDescription().createIdentifierString() != st["uid"].toString())
                continue;

            r->getSlot (s).bypassed = (bool) st["bypass"];
            restoreState (*p, st["state"].toString());
        }
    }

    if (engine.busSendMask() != busSendsBefore)
        engine.rebuildRouting();   // a send between buses turned on or off

}

void Session::addScene (const juce::String& name)
{
    scenes.push_back ({ name, captureSceneData() });
    currentScene = (int) scenes.size() - 1;
    markDirty();
}

void Session::storeScene (int index)
{
    if (! juce::isPositiveAndBelow (index, getNumScenes()))
        return;

    scenes[(size_t) index].data = captureSceneData();
    currentScene = index;
    markDirty();
}

void Session::recallScene (int index)
{
    if (! juce::isPositiveAndBelow (index, getNumScenes()))
        return;

    ++recallCount;
    applySceneData (scenes[(size_t) index].data);
    currentScene = index;
    amendUndoState();   // a recall is not an edit: undo never jumps back across it
    notify();
}

void Session::stepScene (int delta)
{
    if (scenes.empty())
        return;

    recallScene (juce::jlimit (0, getNumScenes() - 1, (currentScene < 0 ? 0 : currentScene + delta)));
}

void Session::renameScene (int index, const juce::String& name)
{
    if (juce::isPositiveAndBelow (index, getNumScenes()))
    {
        scenes[(size_t) index].name = name;
        markDirty();
    }
}

void Session::deleteScene (int index)
{
    if (! juce::isPositiveAndBelow (index, getNumScenes()))
        return;

    scenes.erase (scenes.begin() + index);

    if (currentScene >= getNumScenes())
        currentScene = getNumScenes() - 1;

    markDirty();
}

void Session::moveScene (int from, int to)
{
    if (! juce::isPositiveAndBelow (from, getNumScenes()) || ! juce::isPositiveAndBelow (to, getNumScenes()) || from == to)
        return;

    auto s = scenes[(size_t) from];
    scenes.erase (scenes.begin() + from);
    scenes.insert (scenes.begin() + to, s);

    if (currentScene == from)
        currentScene = to;

    markDirty();
}

//==============================================================================
int Session::numRacksInEditGroup (int group) const
{
    int n = 0;

    for (int i = 0; i < engine.getNumRacks(); ++i)
        n += engine.getRack (i).editGroup == group ? 1 : 0;

    return n;
}

void Session::newSession()
{
    ++*loadGeneration;
    cancelRespread();

    // closing the session: everything goes, parked instances included (not during the show)
    closingSession = true;
    for (int i = engine.getNumRacks(); --i >= 0;)
        removeRack (engine.getRack (i));
    releaseRetiredPlugins();
    closingSession = false;

    scenes.clear();
    currentScene = -1;
    file = juce::File();
    engine.masterGainDb = 0.0f;
    engine.automixEnabled = true;
    engine.automixOverlap = 0;
    engine.tempo.resetToDefaults();
    editGroupsOff = 0;
    rackView = {};
    midiMap = {};
    loadedAppSettings = {};
    engine.setRoutingEnabled (false);
    {
        auto& first = engine.addRack();   // mono, IN 1 -> OUT 1
        first.stereoInput = false;
        first.stereoOutput = false;
    }
    dirty = false;
    ++documentGeneration;
    replacedTokens.clear();
    resetUndoHistory();
    if (midiControl != nullptr) midiControl->loadFromSession();
    notify();
}

//==============================================================================
juce::ValueTree Session::pluginToTree (const Rack& r, int s) const
{
    juce::ValueTree pt (idPlugin);
    auto* p = const_cast<Rack&> (r).getPlugin (s);

    if (p == nullptr)
        return {};

    auto& slot = const_cast<Rack&> (r).getSlot (s);
    juce::StringArray params;
    for (auto i : slot.shownParams)
        params.add (juce::String (i));

    pt.setProperty ("slot", s, nullptr)
      .setProperty ("bypass", slot.bypassed.load(), nullptr)
      .setProperty ("state", stateOf (*p), nullptr)
      .setProperty ("paramsChosen", slot.paramsChosen, nullptr)
      .setProperty ("slotSize", slot.uiSize, nullptr)
      .setProperty ("view", slot.viewMode, nullptr)
      .setProperty ("params", params.joinIntoString (","), nullptr);

    if (! slot.hiddenMeters.isEmpty())
        pt.setProperty ("hiddenMeters", slot.hiddenMeters.joinIntoString ("\n"), nullptr);

    if (auto xml = p->getPluginDescription().createXml())
        pt.appendChild (juce::ValueTree::fromXml (*xml), nullptr);

    return pt;
}

juce::ValueTree Session::rackToTree (const Rack& r) const
{
    auto rt = rackSettingsToTree (r);

    for (int s = 0; s < Rack::maxSlots; ++s)
        if (auto pt = pluginToTree (r, s); pt.isValid())
            rt.appendChild (pt, nullptr);

    return rt;
}

juce::ValueTree Session::rackSettingsToTree (const Rack& r) const
{
    juce::ValueTree rt (idRack);
    rt.setProperty ("id", r.getId(), nullptr)
      .setProperty ("name", r.name, nullptr)
      .setProperty ("colour", r.colour.toString(), nullptr)
      .setProperty ("in", r.inputChannel.load(), nullptr)
      .setProperty ("stereoIn", r.stereoInput.load(), nullptr)
      .setProperty ("out", r.outputChannel.load(), nullptr)
      .setProperty ("stereoOut", r.stereoOutput.load(), nullptr)
      .setProperty ("gain", r.gainDb.load(), nullptr)
      .setProperty ("inGain", r.inputGainDb.load(), nullptr)
      .setProperty ("mute", r.muted.load(), nullptr)
      .setProperty ("bypass", r.bypassed.load(), nullptr)
      .setProperty ("solo", r.solo.load(), nullptr)
      .setProperty ("size", r.uiSize, nullptr)
      .setProperty ("sendsV", r.uiSendsV, nullptr)
      .setProperty ("sendsH", r.uiSendsH, nullptr)
      .setProperty ("inV", r.uiInputV, nullptr)
      .setProperty ("outV", r.uiOutputV, nullptr)
      .setProperty ("sendsShown", r.uiSendsShown, nullptr)
      .setProperty ("hide", r.uiHideSections, nullptr)
      .setProperty ("inRack", r.inputRackId.load(), nullptr)
      .setProperty ("latGroup", r.latencyGroup.load(), nullptr)
      .setProperty ("editGroup", r.editGroup, nullptr)
      .setProperty ("amGroup", r.automixGroup.load(), nullptr)
      .setProperty ("amWeight", r.automixWeightDb.load(), nullptr)
      .setProperty ("tempo", r.tempoIndex.load(), nullptr)
      .setProperty ("bus", r.busIndex.load(), nullptr)
      .setProperty ("outBus", r.outputBus.load(), nullptr)
      .setProperty ("outs", [&r] { juce::StringArray a; for (auto c : r.getExtraOutputs()) a.add (juce::String (c)); return a.joinIntoString (","); }(), nullptr)
      .setProperty ("sends", sendsToString (r), nullptr)
      .setProperty ("sendPre", sendPreMask (r), nullptr)
      .setProperty ("folder", r.isFolder, nullptr)
      .setProperty ("folderOpen", r.folderOpen, nullptr)
      .setProperty ("inFolder", r.folderId, nullptr);
    return rt;
}

void Session::buildRack (Rack& r, const juce::ValueTree& rt)
{
    applyRackSettings (r, rt);

    for (const auto& pt : rt)
        loadPluginFromTree (r, (int) pt["slot"], pt);
}

void Session::applyRackSettings (Rack& r, const juce::ValueTree& rt)
{
    r.name = rt["name"].toString();
    r.colour = juce::Colour::fromString (rt["colour"].toString());
    r.inputChannel = (int) rt["in"];
    r.stereoInput = (bool) rt["stereoIn"];
    r.outputChannel = (int) rt["out"];
    r.stereoOutput = rt.hasProperty ("stereoOut") ? (bool) rt["stereoOut"] : true;
    r.gainDb = (float) rt["gain"];
    r.inputGainDb = (float) rt.getProperty ("inGain", 0.0f);
    r.muted = (bool) rt["mute"];
    r.bypassed = (bool) rt["bypass"];
    r.solo = (bool) rt["solo"];
    r.uiSize = rt.hasProperty ("size") ? (float) rt["size"] : 1.0f;
    r.uiSendsV = (float) rt.getProperty ("sendsV", -1.0f);
    r.uiSendsH = (float) rt.getProperty ("sendsH", -1.0f);
    r.uiInputV = (float) rt.getProperty ("inV", -1.0f);
    r.uiOutputV = (float) rt.getProperty ("outV", -1.0f);
    r.uiSendsShown = (int) rt.getProperty ("sendsShown", -1);
    r.uiHideSections = (int) rt.getProperty ("hide", -1);
    r.inputRackId = rt.hasProperty ("inRack") ? (int) rt["inRack"] : -1;
    r.latencyGroup = (int) rt["latGroup"];
    r.editGroup = juce::jlimit (0, Rack::maxEditGroups, (int) rt["editGroup"]);
    r.automixGroup = juce::jlimit (0, Rack::maxAutomixGroups, (int) rt["amGroup"]);
    r.automixWeightDb = (float) rt["amWeight"];
    r.tempoIndex = juce::jlimit (0, TempoEngine::maxTempos - 1, (int) rt["tempo"]);
    r.busIndex = juce::jlimit (0, Rack::maxBuses, (int) rt["bus"]);
    r.outputBus = juce::jlimit (0, Rack::maxBuses, (int) rt["outBus"]);
    {
        // extra outputs (codes as Rack::extraOutputs); checked again when the graph is rebuilt
        const auto codes = juce::StringArray::fromTokens (rt["outs"].toString(), ",", {});
        for (int i = 0; i < Rack::maxExtraOutputs; ++i)
            r.extraOutputs[(size_t) i] = i < codes.size() ? juce::jmax (0, codes[i].getIntValue()) : 0;
    }
    sendsFromTree (r, rt);
    r.isFolder = (bool) rt.getProperty ("folder", false);
    r.folderOpen = (bool) rt.getProperty ("folderOpen", true);
    r.folderId = (int) rt.getProperty ("inFolder", -1);
}

void Session::loadPluginFromTree (Rack& r, int slot, const juce::ValueTree& pt, bool newUndoStep)
{
    juce::PluginDescription saved;

    if (auto descXml = pt.getChild (0).createXml(); descXml == nullptr || ! saved.loadFromXml (*descXml))
        return;

    const auto known = plugins.findKnown (saved);

    if (! known)
        return;

    const bool bypass = pt["bypass"];
    const auto state = pt["state"].toString();
    const bool paramsChosen = pt["paramsChosen"];
    const float slotSize = pt.hasProperty ("slotSize") ? (float) pt["slotSize"] : 1.0f;
    const int viewMode = pt["view"];
    const auto hiddenMeters = juce::StringArray::fromLines (pt["hiddenMeters"].toString());
    juce::Array<int> shownParams;
    for (auto& t : juce::StringArray::fromTokens (pt["params"].toString(), ",", ""))
        if (t.isNotEmpty())
            shownParams.add (t.getIntValue());
    const auto rackId = r.getId();
    const auto generation = *loadGeneration;
    std::weak_ptr<int> weakGen = loadGeneration;
    std::weak_ptr<int> weakPending = pendingLoads;
    ++*pendingLoads;

    plugins.createPlugin (*known, r.getSampleRate(), r.getBlockSize(),
        [this, weakGen, weakPending, generation, rackId, slot, bypass, state, paramsChosen, shownParams, slotSize, viewMode, hiddenMeters, newUndoStep] (std::unique_ptr<juce::AudioPluginInstance> p, const juce::String&)
        {
            if (auto pending = weakPending.lock())
                --*pending;

            auto gen = weakGen.lock();
            auto* rack = engine.findRack (rackId);

            // dropped if the session was replaced or the rack deleted meanwhile
            if (gen == nullptr || *gen != generation || rack == nullptr || p == nullptr)
                return;

            Rack::configurePlugin (*p, rack->getSampleRate(), rack->getBlockSize());
            restoreState (*p, state);

            int hopCalls = 0, hopPhase = -1;
            engine.alignNewPlugin (*p, *rack, hopCalls, hopPhase);

            if (auto* old = rack->getPlugin (slot); old != nullptr && onPluginAboutToBeDeleted)
                onPluginAboutToBeDeleted (old);

            retirePlugin (rack->setPlugin (slot, std::move (p)));
            rack->getSlot (slot).bypassed = bypass;
            rack->getSlot (slot).hopCalls = hopCalls;
            rack->getSlot (slot).hopPhase = hopPhase;
            rack->getSlot (slot).paramsChosen = paramsChosen;
            rack->getSlot (slot).shownParams = shownParams;
            rack->getSlot (slot).uiSize = slotSize;
            rack->getSlot (slot).viewMode = viewMode;
            rack->getSlot (slot).hiddenMeters = hiddenMeters;
            rack->getSlot (slot).hiddenMeters.removeEmptyStrings();

            if (newUndoStep)
                markDirty();
            else
            {
                amendUndoState();
                notify();
            }
        });
}

Rack& Session::createRackFromTree (const juce::ValueTree& rt, int insertIndex)
{
    auto& r = engine.addRack();
    buildRack (r, rt);

    // a copy of a bus becomes a new bus (next free letter), or a normal rack if none is left
    if (const auto b = r.busIndex.load(); b > 0)
    {
        r.busIndex = 0;
        if (! engine.setRackBus (r, b))
            engine.setRackBus (r, engine.firstFreeBus());
    }

    if (juce::isPositiveAndBelow (insertIndex, engine.getNumRacks() - 1))
        engine.moveRack (engine.getNumRacks() - 1, insertIndex);

    engine.rebuildRouting();

    // keep the chain if its source still exists and no loop is created
    if (const auto src = r.inputRackId.load(); src >= 0)
        if (! engine.setRackInputSource (r, src))
            engine.setRackInputSource (r, -1);

    markDirty();
    return r;
}

void Session::duplicateRacks (const juce::Array<int>& rackIds)
{
    int insertAt = -1;
    std::vector<juce::ValueTree> copies;

    for (int i = 0; i < engine.getNumRacks(); ++i)
    {
        auto& r = engine.getRack (i);

        if (rackIds.contains (r.getId()))
        {
            auto t = rackToTree (r);
            t.setProperty ("name", r.name + " copy", nullptr);
            copies.push_back (t);
            insertAt = i + 1;
        }
    }

    for (auto& t : copies)
        createRackFromTree (t, insertAt++);
}

void Session::moveRackTo (int rackId, int newIndex)
{
    if (auto* r = engine.findRack (rackId))
    {
        engine.moveRack (engine.indexOf (*r), juce::jlimit (0, engine.getNumRacks() - 1, newIndex));
        markDirty();
    }
}

void Session::movePlugin (Rack& from, int fromSlot, Rack& to, int toSlot)
{
    if (&from == &to)
    {
        from.moveSlot (fromSlot, toSlot);
        markDirty();
        return;
    }

    // Same engine block size/rate: the instances move as they are (state, hop phase kept).
    auto& fs = from.getSlot (fromSlot);
    auto& ts = to.getSlot (toSlot);
    const bool fb = fs.bypassed, tb = ts.bypassed;
    const int fh = fs.hopCalls, fp = fs.hopPhase, th = ts.hopCalls, tp = ts.hopPhase;

    const auto fParams = fs.shownParams, tParams = ts.shownParams;
    const float fSize = fs.uiSize, tSize = ts.uiSize;
    const int fView = fs.viewMode, tView = ts.viewMode;   // read before setPlugin() resets them
    const bool fChosen = fs.paramsChosen, tChosen = ts.paramsChosen;
    const auto fMeters = fs.hiddenMeters, tMeters = ts.hiddenMeters;

    auto moving = from.setPlugin (fromSlot, nullptr);
    auto displaced = to.setPlugin (toSlot, std::move (moving));
    from.setPlugin (fromSlot, std::move (displaced));

    ts.shownParams = fParams; ts.paramsChosen = fChosen; ts.uiSize = fSize; ts.viewMode = fView;
    fs.shownParams = tParams; fs.paramsChosen = tChosen; fs.uiSize = tSize; fs.viewMode = tView;
    ts.hiddenMeters = fMeters; fs.hiddenMeters = tMeters;

    ts.bypassed = fb; ts.hopCalls = fh; ts.hopPhase = fp;
    fs.bypassed = tb; fs.hopCalls = th; fs.hopPhase = tp;
    markDirty();
}

void Session::copyPlugin (Rack& from, int fromSlot, Rack& to, int toSlot)
{
    if (auto pt = pluginToTree (from, fromSlot); pt.isValid())
        loadPluginFromTree (to, toSlot, pt, true);
}

static juce::Array<Rack*> racksInOrder (AudioEngine& engine, const juce::Array<int>& ids)
{
    juce::Array<Rack*> result;

    for (int i = 0; i < engine.getNumRacks(); ++i)
        if (ids.contains (engine.getRack (i).getId()))
            result.add (&engine.getRack (i));

    return result;
}

/** Racks in order, rotated so that firstRackId (if among them) comes first. */
static juce::Array<Rack*> racksFrom (AudioEngine& engine, const juce::Array<int>& ids, int firstRackId)
{
    auto list = racksInOrder (engine, ids);

    for (int i = 0; i < list.size(); ++i)
        if (list[i]->getId() == firstRackId)
        {
            juce::Array<Rack*> rotated;
            for (int k = 0; k < list.size(); ++k)
                rotated.add (list[(i + k) % list.size()]);
            return rotated;
        }

    return list;
}

void Session::setConsecutiveInputs (const juce::Array<int>& rackIds, bool stereo, int startChannel, int firstRackId)
{
    auto list = racksFrom (engine, rackIds, firstRackId);

    if (list.isEmpty())
        return;

    int ch = startChannel >= 0 ? startChannel : juce::jmax (0, list[0]->inputChannel.load());

    for (auto* r : list)
    {
        const bool st = firstRackId < 0 || r->getId() == firstRackId ? stereo : r->stereoInput.load();
        r->inputChannel = ch;
        r->stereoInput = st;
        engine.setRackInputSource (*r, -1);
        followInput (*r, engine.isRoutingActive());
        ch += st ? 2 : 1;
    }

    markDirty();
}

void Session::setConsecutiveOutputs (const juce::Array<int>& rackIds, bool stereo, int startChannel, int firstRackId)
{
    auto list = racksFrom (engine, rackIds, firstRackId);

    if (list.isEmpty())
        return;

    int ch = startChannel >= 0 ? startChannel : juce::jmax (0, list[0]->outputChannel.load());

    for (auto* r : list)
    {
        const bool st = firstRackId < 0 || r->getId() == firstRackId ? stereo : r->stereoOutput.load();
        r->outputChannel = ch;
        r->stereoOutput = st;
        ch += st ? 2 : 1;
    }

    markDirty();
}

juce::ValueTree Session::toTree() const
{
    juce::ValueTree root (idRoot);
    root.setProperty ("version", 1, nullptr)
        .setProperty ("master", engine.masterGainDb.load(), nullptr)
        .setProperty ("currentScene", currentScene, nullptr)
        .setProperty ("automix", engine.automixEnabled.load(), nullptr)
        .setProperty ("amOverlap", engine.automixOverlap.load(), nullptr)
        .setProperty ("groupsOff", editGroupsOff, nullptr)
        .setProperty ("routing", engine.isRoutingEnabled(), nullptr)
        .setProperty ("viewHide", hiddenBits (rackView), nullptr)
        .setProperty ("viewSends", rackView.sendsMask, nullptr);

    juce::ValueTree racksTree (idRacks);

    for (int i = 0; i < engine.getNumRacks(); ++i)
        racksTree.appendChild (rackToTree (engine.getRack (i)), nullptr);

    root.appendChild (racksTree, nullptr);

    juce::ValueTree scenesTree (idScenes);
    for (auto& s : scenes)
    {
        auto st = s.data.createCopy();
        st.setProperty ("name", s.name, nullptr);
        scenesTree.appendChild (st, nullptr);
    }
    root.appendChild (scenesTree, nullptr);
    root.appendChild (engine.tempo.toTree(), nullptr);

    if (midiMap.isValid() && midiMap.getNumChildren() > 0)
        root.appendChild (midiMap.createCopy(), nullptr);

    if (auto app = captureAppSettings != nullptr ? captureAppSettings() : loadedAppSettings.createCopy(); app.isValid())
        root.appendChild (app, nullptr);
    return root;
}

bool Session::saveTo (const juce::File& f)
{
    auto xml = toTree().createXml();

    if (xml == nullptr || ! xml->writeTo (f))
        return false;

    file = f;
    dirty = false;
    ++documentGeneration;
    notify();
    return true;
}

void Session::loadFrom (const juce::File& f)
{
    auto xml = juce::parseXML (f);

    if (xml == nullptr || ! xml->hasTagName (idRoot.toString()))
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "StageRack",
                                                "Not a StageRack session: " + f.getFileName());
        return;
    }

    loadFromTree (juce::ValueTree::fromXml (*xml), f, false);
}

bool Session::isSessionTree (const juce::ValueTree& root)
{
    return root.hasType (idRoot);
}

void Session::loadFromTree (const juce::ValueTree& root, const juce::File& f, bool markAsChanged)
{
    newSession();
    engine.removeRack (engine.getRack (0));

    engine.masterGainDb = 0.0f;   // no master fader any more: old sessions play at unity
    engine.automixEnabled = root.hasProperty ("automix") ? (bool) root["automix"] : true;
    engine.automixOverlap = juce::jlimit (0, 2, (int) root["amOverlap"]);
    editGroupsOff = (int) root["groupsOff"];
    rackView = withHidden ({}, (int) root["viewHide"]);
    rackView.sendsMask = (int) root.getProperty ("viewSends", 0xff) & 0xff;
    engine.setRoutingEnabled ((bool) root["routing"]);
    engine.tempo.fromTree (root.getChildWithName ("TEMPO"));
    midiMap = root.getChildWithName ("MIDIMAP").createCopy();
    loadedAppSettings = root.getChildWithName ("APPSETTINGS").createCopy();
    ++*loadGeneration;

    for (const auto& rt : root.getChildWithName (idRacks))
        buildRack (engine.addRackWithId (rt["id"]), rt);

    for (const auto& st : root.getChildWithName (idScenes))
    {
        auto data = st.createCopy();
        const auto name = data["name"].toString();
        data.removeProperty ("name", nullptr);
        scenes.push_back ({ name, data });
    }

    // chains may point to racks created after them: validate once all exist
    for (int i = 0; i < engine.getNumRacks(); ++i)
        if (auto& r = engine.getRack (i); r.inputRackId >= 0 && ! engine.setRackInputSource (r, r.inputRackId))
            engine.setRackInputSource (r, -1);

    engine.rebuildRouting();
    normalizeFolders();

    currentScene = juce::jmin ((int) root["currentScene"], getNumScenes() - 1);
    file = f;
    dirty = markAsChanged;
    ++documentGeneration;
    resetUndoHistory();   // its plugins complete this state as they arrive (amendUndoState)
    if (midiControl != nullptr) midiControl->loadFromSession();
    notify();

    if (loadedAppSettings.isValid() && applyAppSettings != nullptr)
        applyAppSettings (loadedAppSettings);
}

//==============================================================================
// Undo
static juce::int64 tokenOf (const juce::AudioPluginInstance* p) noexcept { return (juce::int64) (juce::pointer_sized_int) p; }

bool Session::UndoState::sameAs (const UndoState& o) const
{
    if (currentScene != o.currentScene || scenes.size() != o.scenes.size() || ! tree.isEquivalentTo (o.tree))
        return false;

    for (size_t i = 0; i < scenes.size(); ++i)
        if (scenes[i].name != o.scenes[i].name || scenes[i].data != o.scenes[i].data)   // same shared tree?
            return false;

    return true;
}

Session::UndoState Session::captureUndoState() const
{
    UndoState u;
    u.tree = juce::ValueTree ("UNDO");
    u.tree.setProperty ("automix", engine.automixEnabled.load(), nullptr)
          .setProperty ("amOverlap", engine.automixOverlap.load(), nullptr)
          .setProperty ("groupsOff", editGroupsOff, nullptr)
          .setProperty ("routing", engine.isRoutingEnabled(), nullptr);

    for (int i = 0; i < engine.getNumRacks(); ++i)
    {
        auto& r = engine.getRack (i);
        auto rt = rackSettingsToTree (r);

        for (int s = 0; s < Rack::maxSlots; ++s)
        {
            if (auto* p = r.getPlugin (s))
            {
                auto& sl = r.getSlot (s);
                juce::StringArray params;
                for (auto k : sl.shownParams)
                    params.add (juce::String (k));

                juce::ValueTree st ("SLOT");
                st.setProperty ("slot", s, nullptr)
                  .setProperty ("token", tokenOf (p), nullptr)
                  .setProperty ("bypass", sl.bypassed.load(), nullptr)
                  .setProperty ("paramsChosen", sl.paramsChosen, nullptr)
                  .setProperty ("params", params.joinIntoString (","), nullptr)
                  .setProperty ("slotSize", sl.uiSize, nullptr)
                  .setProperty ("view", sl.viewMode, nullptr)
                  .setProperty ("hiddenMeters", sl.hiddenMeters.joinIntoString ("\n"), nullptr);
                rt.appendChild (st, nullptr);
            }
        }

        u.tree.appendChild (rt, nullptr);
    }

    u.scenes = scenes;
    u.currentScene = currentScene;
    return u;
}

void Session::resetUndoHistory()
{
    undoStack.clear();
    redoStack.clear();
    undoCurrent = captureUndoState();
}

void Session::recordUndoStep()
{
    if (applyingUndo)
        return;

    auto now = captureUndoState();

    if (now.sameAs (undoCurrent))
        return;

    undoStack.push_back (std::move (undoCurrent));
    if (undoStack.size() > maxUndoSteps)
        undoStack.erase (undoStack.begin());

    undoCurrent = std::move (now);
    redoStack.clear();
}

void Session::amendUndoState()
{
    if (! applyingUndo)
        undoCurrent = captureUndoState();
}

void Session::pluginReplaced (juce::AudioPluginInstance* oldOne, juce::AudioPluginInstance* newOne)
{
    if (oldOne != nullptr && newOne != nullptr)
        replacedTokens[tokenOf (oldOne)] = tokenOf (newOne);   // older steps then point at the twin

    amendUndoState();

    if (onPluginReplaced)
        onPluginReplaced (oldOne, newOne);
}

bool Session::undo()
{
    if (! canUndo())
        return false;

    amendUndoState();   // changes that were not a step of their own (e.g. a wheel on a knob) stay redoable
    redoStack.push_back (undoCurrent);
    auto target = std::move (undoStack.back());
    undoStack.pop_back();
    applyUndoState (target);
    return true;
}

bool Session::redo()
{
    if (! canRedo())
        return false;

    amendUndoState();
    undoStack.push_back (undoCurrent);
    auto target = std::move (redoStack.back());
    redoStack.pop_back();
    applyUndoState (target);
    return true;
}

/** The instance a step refers to: in the pool (taken out of a slot just now) or parked. */
juce::AudioPluginInstance* Session::takeInstance (juce::int64 token, std::map<juce::int64, std::unique_ptr<juce::AudioPluginInstance>>& pool)
{
    for (int guard = 0; guard < 64; ++guard)   // follow re-spread replacements
    {
        auto it = replacedTokens.find (token);
        if (it == replacedTokens.end())
            break;
        token = it->second;
    }

    if (auto it = pool.find (token); it != pool.end())
    {
        auto* p = it->second.release();
        pool.erase (it);
        return p;
    }

    for (auto it = retired.begin(); it != retired.end(); ++it)
        if (tokenOf (it->get()) == token)
        {
            auto* p = it->release();
            retired.erase (it);
            retiredAt.erase (p);
            return p;
        }

    return nullptr;
}

void Session::applyUndoState (const UndoState& u)
{
    const juce::ScopedValueSetter<bool> guard (applyingUndo, true);
    cancelRespread();

    engine.automixEnabled = (bool) u.tree["automix"];
    engine.automixOverlap = juce::jlimit (0, 2, (int) u.tree["amOverlap"]);
    editGroupsOff = (int) u.tree["groupsOff"];
    engine.setRoutingEnabled ((bool) u.tree["routing"]);

    // target instance of every (rack, slot)
    std::map<std::pair<int, int>, juce::int64> wanted;
    juce::Array<int> wantedRacks;

    for (const auto& rt : u.tree)
    {
        wantedRacks.add ((int) rt["id"]);
        for (const auto& st : rt)
            wanted[{ (int) rt["id"], (int) st["slot"] }] = (juce::int64) st["token"];
    }

    auto resolve = [this] (juce::int64 t)
    {
        for (int g = 0; g < 64; ++g)
            if (auto it = replacedTokens.find (t); it != replacedTokens.end()) t = it->second; else break;
        return t;
    };

    // 1. take out every instance that is not where it should be (kept alive in the pool)
    std::map<juce::int64, std::unique_ptr<juce::AudioPluginInstance>> pool;
    std::map<juce::int64, std::pair<int, int>> poolHop;   // its live hop calls/phase (moves keep them)

    for (int i = 0; i < engine.getNumRacks(); ++i)
    {
        auto& r = engine.getRack (i);

        for (int s = 0; s < Rack::maxSlots; ++s)
        {
            auto* p = r.getPlugin (s);

            if (p == nullptr)
                continue;

            const auto it = wanted.find ({ r.getId(), s });

            if (it != wanted.end() && resolve (it->second) == tokenOf (p))
                continue;

            const std::pair<int, int> hop { r.getSlot (s).hopCalls.load(), r.getSlot (s).hopPhase.load() };
            auto taken = r.setPlugin (s, nullptr);
            poolHop[tokenOf (taken.get())] = hop;
            pool[tokenOf (taken.get())] = std::move (taken);
        }
    }

    // 2. racks that should not exist (their plugins are already in the pool)
    for (int i = engine.getNumRacks(); --i >= 0;)
    {
        auto& r = engine.getRack (i);

        if (! wantedRacks.contains (r.getId()))
        {
            for (auto& p : r.takeDiscarded())
                retirePlugin (std::move (p));
            engine.removeRack (r);
        }
    }

    // 3. racks that should exist, in order, with their settings
    int index = 0;

    for (const auto& rt : u.tree)
    {
        auto* r = engine.findRack ((int) rt["id"]);

        if (r == nullptr)
            r = &engine.addRackWithId ((int) rt["id"]);

        engine.moveRack (engine.indexOf (*r), index++);
        applyRackSettings (*r, rt);
    }

    for (int i = 0; i < engine.getNumRacks(); ++i)
        if (auto& r = engine.getRack (i); ! engine.setRackInputSource (r, r.inputRackId.load()))
            engine.setRackInputSource (r, -1);

    engine.rebuildRouting();

    // 4. instances back in place: moved ones keep their hop phase, parked ones are re-aligned
    for (const auto& rt : u.tree)
    {
        auto& r = *engine.findRack ((int) rt["id"]);

        for (const auto& st : rt)
        {
            const int s = st["slot"];
            const auto token = resolve ((juce::int64) st["token"]);
            int hopCalls = -1, hopPhase = -1;

            if (r.getPlugin (s) != nullptr && tokenOf (r.getPlugin (s)) == token)
            {
                hopCalls = r.getSlot (s).hopCalls;   // stayed: nothing to do but the slot settings
                hopPhase = r.getSlot (s).hopPhase;
            }
            else
            {
                const bool fromPool = pool.count (token) > 0;
                std::unique_ptr<juce::AudioPluginInstance> p (takeInstance (token, pool));

                if (p == nullptr)
                    continue;   // gone (should not happen: instances are parked until the session closes)

                if (fromPool)
                {
                    hopCalls = poolHop[token].first;
                    hopPhase = poolHop[token].second;
                }
                else
                {
                    // parked: it was not processed meanwhile, its hop may sit anywhere now
                    if (p->getSampleRate() != r.getSampleRate() || p->getBlockSize() != r.getBlockSize())
                        Rack::configurePlugin (*p, r.getSampleRate(), r.getBlockSize());
                    engine.alignNewPlugin (*p, r, hopCalls, hopPhase);
                }

                retirePlugin (r.setPlugin (s, std::move (p)));
            }

            auto& sl = r.getSlot (s);
            sl.bypassed = (bool) st["bypass"];
            sl.paramsChosen = (bool) st["paramsChosen"];
            sl.shownParams.clearQuick();
            for (auto& t : juce::StringArray::fromTokens (st["params"].toString(), ",", ""))
                if (t.isNotEmpty())
                    sl.shownParams.add (t.getIntValue());
            sl.uiSize = (float) st["slotSize"];
            sl.viewMode = (int) st["view"];
            sl.hiddenMeters = juce::StringArray::fromLines (st["hiddenMeters"].toString());
            sl.hiddenMeters.removeEmptyStrings();
            sl.hopCalls = juce::jmax (0, hopCalls);
            sl.hopPhase = hopPhase;
        }
    }

    // 5. whatever is left was added by the undone edit: parked (a redo brings it back)
    for (auto& [t, p] : pool)
        retirePlugin (std::move (p));

    scenes = u.scenes;
    currentScene = juce::jmin (u.currentScene, getNumScenes() - 1);
    engine.updateLatencyCompensation();

    dirty = true;
    undoCurrent = captureUndoState();
    notify();
}

//==============================================================================
std::set<juce::int64> Session::tokensInUndoHistory() const
{
    std::set<juce::int64> tokens;
    auto add = [&tokens] (const UndoState& u)
    {
        for (const auto& rt : u.tree)
            for (const auto& st : rt)
                if (st.hasProperty ("token"))
                    tokens.insert ((juce::int64) st["token"]);
    };

    for (const auto& u : undoStack) add (u);
    for (const auto& u : redoStack) add (u);
    add (undoCurrent);
    return tokens;
}

/** Writes the re-spread replacements into the steps themselves (old token -> its twin), so
    the map can go. Tokens are addresses: once an instance is freed, a new one may get the
    same address, and a leftover "old -> twin" entry would then redirect the newcomer. */
void Session::resolveReplacedTokens()
{
    if (replacedTokens.empty())
        return;

    auto resolve = [this] (juce::int64 t)
    {
        for (int g = 0; g < 64; ++g)
            if (auto it = replacedTokens.find (t); it != replacedTokens.end()) t = it->second; else break;
        return t;
    };

    auto rewrite = [&resolve] (UndoState& u)
    {
        for (auto rt : u.tree)
            for (auto st : rt)
                if (st.hasProperty ("token"))
                    if (const auto t = (juce::int64) st["token"], r = resolve (t); r != t)
                        st.setProperty ("token", r, nullptr);
    };

    for (auto& u : undoStack) rewrite (u);
    for (auto& u : redoStack) rewrite (u);
    rewrite (undoCurrent);
    replacedTokens.clear();
}

bool Session::tidyParkedPlugins()
{
    if (retired.empty() || closingSession || applyingUndo || *pendingLoads > 0 || isRespreading())
        return false;

    resolveReplacedTokens();
    auto inHistory = tokensInUndoHistory();

    auto countHeld = [&] { int n = 0; for (auto& p : retired) n += inHistory.count (tokenOf (p.get())) > 0 ? 1 : 0; return n; };

    // too many kept for undo: the oldest steps go first (like any limited undo)
    if (countHeld() > maxParkedPlugins)
    {
        while (countHeld() > maxParkedPlugins && (! undoStack.empty() || ! redoStack.empty()))
        {
            if (! undoStack.empty()) undoStack.erase (undoStack.begin());
            else                     redoStack.erase (redoStack.begin());   // the farthest redo
            inHistory = tokensInUndoHistory();
        }
    }

    // free the oldest instance that has rested long enough and no step needs
    const auto now = juce::Time::getMillisecondCounter();
    auto victim = retired.end();
    juce::uint32 oldestAge = 0;

    for (auto it = retired.begin(); it != retired.end(); ++it)
    {
        const auto at = retiredAt.find (it->get());
        const auto age = at != retiredAt.end() ? now - at->second : parkedQuietMs;

        if (age >= parkedQuietMs && inHistory.count (tokenOf (it->get())) == 0 && age >= oldestAge)
        {
            victim = it;
            oldestAge = age;
        }
    }

    if (victim == retired.end())
        return false;

    auto p = std::move (*victim);
    retired.erase (victim);
    retiredAt.erase (p.get());
    p.reset();   // message thread, parked for seconds: never processed since, its helpers idle
    return true;
}

} // namespace stagerack
