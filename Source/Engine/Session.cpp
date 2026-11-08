#include "Session.h"
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

        for (int r = 0; r < engine.getNumRacks(); ++r)
        {
            auto& rack = engine.getRack (r);
            for (int i = 0; i < Rack::maxSlots; ++i)
                if (auto* p = rack.getPlugin (i))
                    jobs.push_back ({ rack.getId(), i, p });
        }

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
            if (rack == nullptr || rack->isAsync() || rack->getPlugin (current.slot) != current.original
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

        if (twin == nullptr || rack == nullptr || rack->getPlugin (current.slot) != current.original)
            return next();

        auto& slot = rack->getSlot (current.slot);
        Rack::configurePlugin (*twin, rack->getSampleRate(), rack->getBlockSize());
        restoreState (*twin, stateOf (*current.original));
        copyParameters (*current.original, *twin);

        int hop = 0, phase = -1;
        session.engine.alignNewPlugin (*twin, *rack, hop, phase, &slot);

        // no periodic hop, or already where it should be: nothing to move
        if (hop == 0 || (slot.hopCalls == hop && slot.hopPhase == phase))
            return next();

        copyParameters (*current.original, *twin);   // in case they moved during the measurement
        auto* raw = twin.get();
        const auto sr = rack->getSampleRate();

        if (! rack->beginHandover (current.slot, twin, hop, phase, (int) (warmSeconds * sr), (int) (fadeSeconds * sr)))
            return next();

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
        if (p != nullptr && session.onPluginAboutToBeDeleted)
            session.onPluginAboutToBeDeleted (p.get());
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
                        if (session.onPluginReplaced) session.onPluginReplaced (old.get(), rack.getPlugin (i));
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
            if (session.onPluginReplaced) session.onPluginReplaced (old.get(), twinPtr);
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
void Session::cancelRespread()                  { respreader->cancel(); }
bool Session::isRespreading() const noexcept    { return respreader->isBusy(); }
juce::String Session::getRespreadStatus() const { return respreader->status(); }

void Session::markDirty()
{
    dirty = true;
    notify();
}

//==============================================================================
Rack& Session::addRack()
{
    auto& r = engine.addRack();
    markDirty();
    return r;
}

void Session::removeRack (Rack& rack)
{
    for (int i = 0; i < Rack::maxSlots; ++i)
        if (auto* p = rack.getPlugin (i); p != nullptr && onPluginAboutToBeDeleted)
            onPluginAboutToBeDeleted (p);

    engine.removeRack (rack);
    markDirty();
}

void Session::loadPlugin (Rack& rack, int slot, const juce::PluginDescription& desc,
                          std::function<void (const juce::String&)> done)
{
    const auto rackId = rack.getId();
    const auto sr = rack.getSampleRate();
    const auto bs = rack.getBlockSize();

    plugins.createPlugin (desc, sr, bs, [this, rackId, slot, done] (std::unique_ptr<juce::AudioPluginInstance> p, const juce::String& error)
    {
        auto* target = engine.findRack (rackId);

        if (p == nullptr || target == nullptr)
        {
            if (done) done (error.isNotEmpty() ? error : juce::String ("Rack no longer exists"));
            return;
        }

        Rack::configurePlugin (*p, target->getSampleRate(), target->getBlockSize());

        int hopCalls = 0, hopPhase = -1;
        engine.alignNewPlugin (*p, *target, hopCalls, hopPhase);

        if (auto* old = target->getPlugin (slot); old != nullptr && onPluginAboutToBeDeleted)
            onPluginAboutToBeDeleted (old);

        target->setPlugin (slot, std::move (p)); // previous plugin is destroyed here
        target->getSlot (slot).hopCalls = hopCalls;
        target->getSlot (slot).hopPhase = hopPhase;
        markDirty();

        if (done) done ({});
    });
}

void Session::removePlugin (Rack& rack, int slot)
{
    if (auto* p = rack.getPlugin (slot); p != nullptr && onPluginAboutToBeDeleted)
        onPluginAboutToBeDeleted (p);

    rack.setPlugin (slot, nullptr);
    markDirty();
}

//==============================================================================
juce::ValueTree Session::captureSceneData() const
{
    juce::ValueTree data (idScene);

    for (int i = 0; i < engine.getNumRacks(); ++i)
    {
        auto& r = engine.getRack (i);
        juce::ValueTree rt (idRack);
        rt.setProperty ("id", r.getId(), nullptr)
          .setProperty ("gain", r.gainDb.load(), nullptr)
          .setProperty ("mute", r.muted.load(), nullptr)
          .setProperty ("bypass", r.bypassed.load(), nullptr)
          .setProperty ("solo", r.solo.load(), nullptr);

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

    data.setProperty ("master", engine.masterGainDb.load(), nullptr);
    return data;
}

void Session::applySceneData (const juce::ValueTree& data)
{
    for (const auto& rt : data)
    {
        auto* r = engine.findRack (rt["id"]);

        if (r == nullptr)
            continue;

        r->gainDb = (float) rt["gain"];
        r->muted = (bool) rt["mute"];
        r->bypassed = (bool) rt["bypass"];
        r->solo = (bool) rt["solo"];

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

    applySceneData (scenes[(size_t) index].data);
    currentScene = index;
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
void Session::newSession()
{
    ++*loadGeneration;
    cancelRespread();

    for (int i = engine.getNumRacks(); --i >= 0;)
        removeRack (engine.getRack (i));

    scenes.clear();
    currentScene = -1;
    file = juce::File();
    engine.masterGainDb = 0.0f;
    engine.addRack();
    dirty = false;
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

    if (auto xml = p->getPluginDescription().createXml())
        pt.appendChild (juce::ValueTree::fromXml (*xml), nullptr);

    return pt;
}

juce::ValueTree Session::rackToTree (const Rack& r) const
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
      .setProperty ("mute", r.muted.load(), nullptr)
      .setProperty ("bypass", r.bypassed.load(), nullptr)
      .setProperty ("solo", r.solo.load(), nullptr)
      .setProperty ("size", r.uiSize, nullptr)
      .setProperty ("inRack", r.inputRackId.load(), nullptr)
      .setProperty ("latGroup", r.latencyGroup.load(), nullptr);

    for (int s = 0; s < Rack::maxSlots; ++s)
        if (auto pt = pluginToTree (r, s); pt.isValid())
            rt.appendChild (pt, nullptr);

    return rt;
}

void Session::buildRack (Rack& r, const juce::ValueTree& rt)
{
    r.name = rt["name"].toString();
    r.colour = juce::Colour::fromString (rt["colour"].toString());
    r.inputChannel = (int) rt["in"];
    r.stereoInput = (bool) rt["stereoIn"];
    r.outputChannel = (int) rt["out"];
    r.stereoOutput = rt.hasProperty ("stereoOut") ? (bool) rt["stereoOut"] : true;
    r.gainDb = (float) rt["gain"];
    r.muted = (bool) rt["mute"];
    r.bypassed = (bool) rt["bypass"];
    r.solo = (bool) rt["solo"];
    r.uiSize = rt.hasProperty ("size") ? (float) rt["size"] : 1.0f;
    r.inputRackId = rt.hasProperty ("inRack") ? (int) rt["inRack"] : -1;
    r.latencyGroup = (int) rt["latGroup"];

    for (const auto& pt : rt)
        loadPluginFromTree (r, (int) pt["slot"], pt);
}

void Session::loadPluginFromTree (Rack& r, int slot, const juce::ValueTree& pt)
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
    juce::Array<int> shownParams;
    for (auto& t : juce::StringArray::fromTokens (pt["params"].toString(), ",", ""))
        if (t.isNotEmpty())
            shownParams.add (t.getIntValue());
    const auto rackId = r.getId();
    const auto generation = *loadGeneration;
    std::weak_ptr<int> weakGen = loadGeneration;

    plugins.createPlugin (*known, r.getSampleRate(), r.getBlockSize(),
        [this, weakGen, generation, rackId, slot, bypass, state, paramsChosen, shownParams, slotSize, viewMode] (std::unique_ptr<juce::AudioPluginInstance> p, const juce::String&)
        {
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

            rack->setPlugin (slot, std::move (p));
            rack->getSlot (slot).bypassed = bypass;
            rack->getSlot (slot).hopCalls = hopCalls;
            rack->getSlot (slot).hopPhase = hopPhase;
            rack->getSlot (slot).paramsChosen = paramsChosen;
            rack->getSlot (slot).shownParams = shownParams;
            rack->getSlot (slot).uiSize = slotSize;
            rack->getSlot (slot).viewMode = viewMode;
            notify();
        });
}

Rack& Session::createRackFromTree (const juce::ValueTree& rt, int insertIndex)
{
    auto& r = engine.addRack();
    buildRack (r, rt);

    if (juce::isPositiveAndBelow (insertIndex, engine.getNumRacks() - 1))
        engine.moveRack (engine.getNumRacks() - 1, insertIndex);

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

    auto moving = from.setPlugin (fromSlot, nullptr);
    auto displaced = to.setPlugin (toSlot, std::move (moving));
    from.setPlugin (fromSlot, std::move (displaced));

    ts.shownParams = fParams; ts.paramsChosen = fChosen; ts.uiSize = fSize; ts.viewMode = fView;
    fs.shownParams = tParams; fs.paramsChosen = tChosen; fs.uiSize = tSize; fs.viewMode = tView;

    ts.bypassed = fb; ts.hopCalls = fh; ts.hopPhase = fp;
    fs.bypassed = tb; fs.hopCalls = th; fs.hopPhase = tp;
    markDirty();
}

void Session::copyPlugin (Rack& from, int fromSlot, Rack& to, int toSlot)
{
    if (auto pt = pluginToTree (from, fromSlot); pt.isValid())
        loadPluginFromTree (to, toSlot, pt);
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
        r->inputChannel = ch;
        r->stereoInput = stereo;
        engine.setRackInputSource (*r, -1);
        ch += stereo ? 2 : 1;
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
        r->outputChannel = ch;
        r->stereoOutput = stereo;
        ch += stereo ? 2 : 1;
    }

    markDirty();
}

bool Session::saveTo (const juce::File& f)
{
    juce::ValueTree root (idRoot);
    root.setProperty ("version", 1, nullptr)
        .setProperty ("master", engine.masterGainDb.load(), nullptr)
        .setProperty ("currentScene", currentScene, nullptr);

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

    auto xml = root.createXml();

    if (xml == nullptr || ! xml->writeTo (f))
        return false;

    file = f;
    dirty = false;
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

    newSession();
    engine.removeRack (engine.getRack (0));

    const auto root = juce::ValueTree::fromXml (*xml);
    engine.masterGainDb = 0.0f;   // no master fader any more: old sessions play at unity
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

    engine.updateLatencyCompensation();

    currentScene = juce::jmin ((int) root["currentScene"], getNumScenes() - 1);
    file = f;
    dirty = false;
    notify();
}

} // namespace stagerack
