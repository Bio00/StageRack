#include "MidiControl.h"
#include "../Plugins/ParamTwins.h"
#include "Session.h"

namespace stagerack
{

static juce::String letterOf (int letter)   { return juce::String::charToString ((juce::juce_wchar) ('A' + letter - 1)); }

MidiControl::MidiControl (Session& s) : session (s)
{
    session.midiControl = this;
    // devices are scanned only when the list changes (a scan blocks the message thread)
    deviceListConnection = juce::MidiDeviceListConnection::make ([this] { lastDeviceScan = 0; });
    startTimerHz (50);
}

MidiControl::~MidiControl()
{
    stopTimer();
    cancelPendingUpdate();

    // LEDs off when the app goes away (a dark controller = not controlling anything)
    for (auto& [id, out] : outputs)
        if (out != nullptr)
            for (auto& m : mappings)
                if (m.feedback && (m.device == id || m.device.isEmpty()))
                {
                    const auto num = m.feedbackNumber >= 0 ? m.feedbackNumber : m.source.number;
                    if (m.source.type == MidiSource::note)
                        out->sendMessageNow (juce::MidiMessage::noteOn (m.source.channel, num, (juce::uint8) 0));
                }

    if (session.midiControl == this)
        session.midiControl = nullptr;
}

//==============================================================================
// Controllers
std::vector<MidiControl::Controller> MidiControl::getControllers() const
{
    std::vector<Controller> result;
    const auto available = juce::MidiInput::getAvailableDevices();

    for (auto& d : available)
    {
        Controller c;
        c.id = d.identifier; c.name = d.name; c.connected = true;
        c.profile = midiprofiles::detect (d.name);

        for (auto& e : controllers)
            if (e.id == d.identifier || (e.name == d.name && e.name.isNotEmpty()))
            {
                c.enabled = e.enabled;
                c.outputId = e.outputId;
            }

        if (auto it = outputs.find (c.id); it != outputs.end() && it->second != nullptr)
            c.resolvedOutputName = it->second->getName();
        result.push_back (c);
    }

    for (auto& e : controllers)
    {
        const bool present = std::any_of (result.begin(), result.end(), [&] (auto& c) { return c.id == e.id || c.name == e.name; });
        if (e.enabled && ! present)
        {
            auto c = e;
            c.connected = false;
            c.profile = midiprofiles::detect (e.name);
            result.push_back (c);
        }
    }
    return result;
}

bool MidiControl::isControllerEnabled (const juce::String& id) const
{
    for (auto& e : controllers)
        if (e.id == id)
            return e.enabled;
    return false;
}

void MidiControl::setControllerEnabled (const juce::String& id, const juce::String& name, bool enabled)
{
    auto it = std::find_if (controllers.begin(), controllers.end(), [&] (auto& c) { return c.id == id; });

    if (it == controllers.end())
    {
        Controller c;
        c.id = id; c.name = name;
        controllers.push_back (c);
        it = std::prev (controllers.end());
    }

    it->enabled = enabled;

    if (enabled && devicesOpen)
        session.getEngine().getDeviceManager().setMidiInputDeviceEnabled (id, true);
    else if (! enabled)
        outputs.erase (id);

    rebuildSourceIndex();
    lastDeviceScan = 0;   // open its output now
    if (onSettingsChanged) onSettingsChanged();
    if (onChange) onChange();
}

void MidiControl::setFeedbackOutput (const juce::String& inputId, const juce::String& outputId)
{
    for (auto& c : controllers)
        if (c.id == inputId)
            c.outputId = outputId;

    outputs.erase (inputId);
    lastDeviceScan = 0;
    if (onSettingsChanged) onSettingsChanged();
    if (onChange) onChange();
}

juce::ValueTree MidiControl::controllersToTree() const
{
    juce::ValueTree t ("MIDICONTROLLERS");
    for (auto& c : controllers)
        t.appendChild (juce::ValueTree ("C").setProperty ("id", c.id, nullptr).setProperty ("name", c.name, nullptr)
                                            .setProperty ("enabled", c.enabled, nullptr).setProperty ("out", c.outputId, nullptr), nullptr);
    return t;
}

void MidiControl::controllersFromTree (const juce::ValueTree& t)
{
    controllers.clear();
    for (const auto& c : t)
    {
        Controller e;
        e.id = c["id"].toString(); e.name = c["name"].toString();
        e.enabled = c["enabled"]; e.outputId = c["out"].toString();
        controllers.push_back (e);
    }
    rebuildSourceIndex();
}

void MidiControl::openDevices()
{
    devicesOpen = true;
    lastDeviceScan = 0;
    timerCallback();
}

juce::String MidiControl::deviceNameFor (const juce::String& id) const
{
    for (auto& c : controllers)
        if (c.id == id)
            return c.name;
    return {};
}

juce::MidiOutput* MidiControl::outputFor (const juce::String& inputId)
{
    if (auto it = outputs.find (inputId); it != outputs.end())
        return it->second.get();
    return nullptr;
}

//==============================================================================
// Incoming (MIDI thread)
bool MidiControl::handleMidi (const juce::String& deviceId, const juce::String& deviceName, const juce::MidiMessage& m)
{
    int value = 0;
    const auto src = MidiSource::fromMessage (m, value);

    if (! src.isValid())
        return false;

    bool consumed = false, controller = false;
    {
        const std::lock_guard<std::mutex> sl (sourceLock);

        for (auto& [d, s] : sourceIndex)
        {
            if (s.type == MidiSource::none)   // enabled controller marker
            {
                controller = controller || d == deviceId;
                continue;
            }
            if ((d.isEmpty() || d == deviceId) && s == src)
                consumed = true;
        }
    }

    const bool learnNow = learning.load();

    if (! (consumed || controller || learnNow))
        return false;

    {
        const std::lock_guard<std::mutex> ql (queueLock);
        if (queue.size() < 4096)
            queue.push_back ({ deviceId, deviceName, m });
    }
    triggerAsyncUpdate();
    return consumed || learnNow;
}

void MidiControl::injectForTests (const juce::String& deviceId, const juce::MidiMessage& m)
{
    process ({ deviceId, deviceNameFor (deviceId), m });
}

void MidiControl::handleAsyncUpdate()
{
    std::vector<Incoming> batch;
    {
        const std::lock_guard<std::mutex> ql (queueLock);
        batch.swap (queue);
    }

    for (auto& in : batch)
        process (in);
}

void MidiControl::rebuildSourceIndex()
{
    std::vector<std::pair<juce::String, MidiSource>> idx;

    for (auto& c : controllers)
        if (c.enabled)
            idx.push_back ({ c.id, MidiSource {} });

    for (auto& m : mappings)
        if (m.source.isValid())
        {
            idx.push_back ({ m.device, m.source });

            // the same controller under another identifier (reconnected): match by name
            for (auto& c : controllers)
                if (c.id != m.device && c.name == m.deviceName && m.deviceName.isNotEmpty())
                    idx.push_back ({ c.id, m.source });
        }

    const std::lock_guard<std::mutex> sl (sourceLock);
    sourceIndex.swap (idx);
}

//==============================================================================
// Processing (message thread)
bool MidiControl::layerMatches (const MidiMapping& m) const noexcept
{
    return m.layer == MidiMapping::anyLayer || (m.layer == MidiMapping::withShift) == shiftHeld;
}

void MidiControl::process (const Incoming& in)
{
    int value = 0;
    const auto src = MidiSource::fromMessage (in.message, value);

    if (! src.isValid())
        return;

    if (learnMappingId >= 0 || (mapMode && learnTarget.isValid()))
        if (learn (in, src, value))
            return;

    juce::String action;
    const bool wasShift = shiftHeld;

    for (auto& m : mappings)
    {
        const bool deviceMatches = m.device.isEmpty() || m.device == in.device || (m.deviceName.isNotEmpty() && m.deviceName == in.deviceName);

        // the layer is decided by the shift state BEFORE this message (a shift control itself always matches)
        if (! deviceMatches || ! (m.source == src) || ! (m.target.kind == MidiTarget::shift || m.layer == MidiMapping::anyLayer
                                                          || (m.layer == MidiMapping::withShift) == wasShift))
            continue;

        apply (m, src, value);
        action << (action.isEmpty() ? "" : ", ") << describeTarget (m.target);

        if (m.takeover == MidiMapping::pickup && ! m.engaged && behaviour (m.target) == MidiTarget::continuous
            && (m.mode == MidiMapping::autoMode || m.mode == MidiMapping::absolute))
        {
            float cur = 0.0f;
            readTarget (m.target, cur);
            action << " (pick-up: bring it to " << valueText (m.target, cur) << ")";
        }
    }

    if (src.type == MidiSource::note && value == 0 && action.isEmpty())
        return;   // releases of unmapped buttons: not worth a line in the monitor

    logEvent (in.deviceName.isNotEmpty() ? in.deviceName : in.device, src, value, action.isEmpty() ? "no mapping" : action);

    if (shiftHeld != wasShift)
        sendFeedback (false);   // the LEDs show the other layer now
}

bool MidiControl::learn (const Incoming& in, const MidiSource& src, int value)
{
    if (src.type == MidiSource::note && value == 0)
        return true;   // the release of the button just learned

    // learning from a controller that is not enabled yet: it is now
    if (! isControllerEnabled (in.device))
        setControllerEnabled (in.device, in.deviceName, true);

    if (learnMappingId >= 0)
    {
        for (auto& m : mappings)
            if (m.id == learnMappingId)
            {
                m.device = in.device; m.deviceName = in.deviceName; m.source = src;
                m.engaged = false; m.lastHw = -1.0f;
                logEvent (in.deviceName, src, value, "learned for " + describeTarget (m.target));
            }

        learnMappingId = -1;
        learning = mapMode && learnTarget.isValid();
        changed();
        sendFeedback (true);
        return true;
    }

    // map mode: this control now drives the selected target. Moving another control while the
    // same target is still selected moves the mapping to it (as in Ableton).
    auto sameSource = [&] (const MidiMapping& m)
    {
        return m.source == src && (m.device == in.device || m.deviceName == in.deviceName);
    };

    if (auto* last = const_cast<MidiMapping*> (findMapping (justLearnedId)); last != nullptr && last->target == learnTarget)
    {
        if (sameSource (*last))
            return true;   // still turning the same knob

        const auto keepId = last->id;
        mappings.erase (std::remove_if (mappings.begin(), mappings.end(),
                                        [&] (auto& m) { return m.id != keepId && sameSource (m) && m.layer == MidiMapping::anyLayer; }),
                        mappings.end());

        if (auto* again = const_cast<MidiMapping*> (findMapping (keepId)))
        {
            again->device = in.device; again->deviceName = in.deviceName; again->source = src;
            again->engaged = false; again->lastHw = -1.0f;
        }
    }
    else
    {
        // a control drives one target per layer: learning it again moves it
        mappings.erase (std::remove_if (mappings.begin(), mappings.end(),
                                        [&] (auto& m) { return sameSource (m) && m.layer == MidiMapping::anyLayer; }),
                        mappings.end());

        MidiMapping m;
        m.device = in.device; m.deviceName = in.deviceName;
        m.source = src;
        m.target = learnTarget;

        const auto beh = behaviour (learnTarget);
        if (learnTarget.kind == MidiTarget::shift)
            m.mode = MidiMapping::momentary;
        else if (beh != MidiTarget::continuous && src.type == MidiSource::cc && (value == 127 || value == 0))
            m.mode = beh == MidiTarget::trigger ? MidiMapping::trigger : MidiMapping::toggle;   // a button sending CCs

        justLearnedId = addMapping (m);
    }

    logEvent (in.deviceName, src, value, "mapped to " + describeTarget (learnTarget));
    changed();
    sendFeedback (true);
    return true;
}

float MidiControl::positionOf (const MidiMapping& m, float t) const noexcept
{
    const auto range = m.maxValue - m.minValue;
    return std::abs (range) < 1.0e-6f ? 0.0f : juce::jlimit (0.0f, 1.0f, (t - m.minValue) / range);
}

float MidiControl::targetOf (const MidiMapping& m, float pos) const noexcept
{
    return m.minValue + juce::jlimit (0.0f, 1.0f, pos) * (m.maxValue - m.minValue);
}

void MidiControl::apply (MidiMapping& m, const MidiSource& src, int value)
{
    const auto beh = behaviour (m.target);

    if (m.target.isRackTarget())
    {
        auto* r = resolveRack (m.target);
        if (r == nullptr)
            return;

        if (r->getId() != m.resolvedRack)   // the bank moved: this control must pick the new value up
        {
            m.resolvedRack = r->getId();
            m.engaged = false;
            m.lastSet = -1.0f;
        }
    }

    int mode = m.mode;
    if (mode == MidiMapping::autoMode)
    {
        if (m.target.kind == MidiTarget::shift)  mode = MidiMapping::momentary;
        else if (beh == MidiTarget::trigger)     mode = MidiMapping::trigger;
        else if (src.type == MidiSource::note)   mode = MidiMapping::toggle;
        else                                     mode = MidiMapping::absolute;
    }

    const auto maxV = (float) src.maxValue();
    const auto pos = juce::jlimit (0.0f, 1.0f, (float) value / maxV);
    const bool press = src.type == MidiSource::note ? value > 0 : pos > 0.5f;
    const auto now = juce::Time::getMillisecondCounterHiRes() * 0.001;

    // a button's press (debounced; buttons that only send "on" count every time)
    auto pressEdge = [&]
    {
        const bool edge = press && (! m.held || now - m.lastPress > 0.25);
        if (press) m.lastPress = now;
        m.held = press;
        return edge;
    };

    auto flip = [&]
    {
        float cur = 0.0f;
        readTarget (m.target, cur);

        if (beh == MidiTarget::continuous)   // between the two ends of the range
            writeTarget (m.target, std::abs (cur - m.maxValue) < std::abs (cur - m.minValue) ? m.minValue : m.maxValue);
        else
            writeTarget (m.target, cur >= 0.5f ? 0.0f : 1.0f);
    };

    switch (mode)
    {
        case MidiMapping::absolute:
        {
            if (beh == MidiTarget::trigger)
            {
                if (pressEdge()) triggerTarget (m.target);
                break;
            }

            if (beh == MidiTarget::toggle)
            {
                const auto on = targetOf (m, pos) >= 0.5f;
                writeTarget (m.target, on ? 1.0f : 0.0f);
                break;
            }

            float cur = 0.0f;
            readTarget (m.target, cur);
            const auto curPos = positionOf (m, cur);

            // somebody else moved it (mouse, scene, undo): the control has to catch it again
            if (m.lastSet >= 0.0f && std::abs (cur - m.lastSet) > 0.004f)
                m.engaged = false;

            auto newPos = pos;

            if (m.takeover == MidiMapping::takeoverNone)
            {
                m.engaged = true;
            }
            else if (! m.engaged)
            {
                const bool crossed = m.lastHw >= 0.0f && (m.lastHw - curPos) * (pos - curPos) <= 0.0f;

                if (std::abs (pos - curPos) <= pickupWindow (m) || crossed)
                    m.engaged = true;
                else if (m.takeover == MidiMapping::scale && m.lastHw >= 0.0f)
                {
                    // value scaling: move the rest of the way in proportion, meet at the end
                    newPos = pos > m.lastHw ? curPos + (pos - m.lastHw) * (1.0f - curPos) / juce::jmax (0.001f, 1.0f - m.lastHw)
                                            : curPos - (m.lastHw - pos) * curPos / juce::jmax (0.001f, m.lastHw);
                    if (std::abs (newPos - pos) <= 0.02f)
                        m.engaged = true;
                }
            }

            m.lastHw = pos;

            if (! m.engaged && m.takeover == MidiMapping::pickup)
                break;   // not caught yet: nothing moves

            writeTarget (m.target, targetOf (m, m.engaged ? pos : newPos));
            readTarget (m.target, m.lastSet);
            break;
        }

        case MidiMapping::toggle:
            if (pressEdge())
            {
                if (beh == MidiTarget::trigger) triggerTarget (m.target);
                else                            flip();
            }
            break;

        case MidiMapping::momentary:
            if (press != m.held || m.target.kind == MidiTarget::shift)
            {
                m.held = press;
                if (beh == MidiTarget::trigger)  { if (press) triggerTarget (m.target); }
                else if (beh == MidiTarget::continuous) writeTarget (m.target, press ? m.maxValue : m.minValue);
                else                             writeTarget (m.target, press ? 1.0f : 0.0f);
            }
            break;

        case MidiMapping::trigger:
            if (pressEdge())
            {
                if (beh == MidiTarget::trigger) triggerTarget (m.target);
                else                            flip();
            }
            break;

        case MidiMapping::relTwos:
        case MidiMapping::relBinOffset:
        case MidiMapping::relSignMag:
        {
            int delta = 0;
            if (mode == MidiMapping::relTwos)           delta = value < 64 ? value : value - 128;
            else if (mode == MidiMapping::relBinOffset) delta = value - 64;
            else                                        delta = (value & 64) != 0 ? -(value & 63) : (value & 63);

            if (delta == 0)
                break;

            if (beh == MidiTarget::continuous)
            {
                float cur = 0.0f;
                readTarget (m.target, cur);
                const auto p = positionOf (m, cur) + (float) delta / 100.0f;
                writeTarget (m.target, targetOf (m, p));
                readTarget (m.target, m.lastSet);
            }
            else if (beh == MidiTarget::toggle)
                writeTarget (m.target, delta > 0 ? 1.0f : 0.0f);
            else if (m.target.kind == MidiTarget::bankNext || m.target.kind == MidiTarget::bankPrev)
                stepBank (delta > 0 ? 1 : -1);
            else if (m.target.kind == MidiTarget::sceneNext || m.target.kind == MidiTarget::scenePrev)
                session.stepScene (delta > 0 ? 1 : -1);
            else
                triggerTarget (m.target);
            break;
        }

        default:
            break;
    }
}

//==============================================================================
// Targets
std::vector<Rack*> MidiControl::visibleRacks() const
{
    std::vector<Rack*> v;
    auto& engine = session.getEngine();

    for (int i = 0; i < engine.getNumRacks(); ++i)
        if (auto& r = engine.getRack (i); ! session.isHiddenInFolder (r))
            v.push_back (&r);
    return v;
}

int MidiControl::bankPosition (const Rack& r) const
{
    const auto v = visibleRacks();
    const auto it = std::find (v.begin(), v.end(), &r);

    if (it == v.end())
        return -1;

    const auto pos = (int) (it - v.begin()) - bankOffset;
    return juce::isPositiveAndBelow (pos, bankSize) ? pos : -1;
}

bool MidiControl::hasBankMappings() const
{
    return std::any_of (mappings.begin(), mappings.end(), [] (auto& m) { return m.target.isRackTarget() && m.target.rackMode == MidiTarget::bankRack; });
}

void MidiControl::setBankSize (int n)
{
    bankSize = juce::jlimit (1, 64, n);
    changed();
}

void MidiControl::setBankOffset (int offset)
{
    const auto n = (int) visibleRacks().size();
    const auto o = juce::jlimit (0, juce::jmax (0, n - 1), offset);

    if (o == bankOffset)
        return;

    bankOffset = o;
    session.setMidiMap (toTree(), false);   // remembered with the session, not an edit
    if (onChange) onChange();
    sendFeedback (false);
}

void MidiControl::stepBank (int direction)
{
    const auto n = (int) visibleRacks().size();
    auto o = bankOffset + direction * bankSize;

    if (o >= n)
        return;   // no racks beyond the last bank

    setBankOffset (juce::jmax (0, o));
}

Rack* MidiControl::resolveRack (const MidiTarget& t) const
{
    switch (t.rackMode)
    {
        case MidiTarget::fixedRack:    return session.getEngine().findRack (t.rack);
        case MidiTarget::selectedRack: return selectedRack != nullptr ? selectedRack() : nullptr;
        case MidiTarget::bankRack:
        {
            const auto v = visibleRacks();
            const auto i = bankOffset + t.rack;
            return juce::isPositiveAndBelow (i, (int) v.size()) && t.rack < bankSize ? v[(size_t) i] : nullptr;
        }
        default: return nullptr;
    }
}

juce::AudioProcessorParameter* MidiControl::findParameter (const MidiTarget& t, Rack*& r) const
{
    r = resolveRack (t);
    if (r == nullptr || t.param < 0)
        return nullptr;

    auto paramIn = [&] (int slot) -> juce::AudioProcessorParameter*
    {
        if (auto* p = r->getPlugin (slot))
        {
            auto& params = p->getParameters();
            if (juce::isPositiveAndBelow (t.param, params.size())
                && (t.paramName.isEmpty() || params[t.param]->getName (64) == t.paramName))
                return params[t.param];
        }
        return nullptr;
    };

    auto inUse = [&] (int slot, juce::AudioProcessorParameter* prm)
    {
        return paramtwins::active (r->getPlugin (slot)->getParameters(), prm);   // synced twin when the plugin is in sync
    };

    if (auto* p = paramIn (t.index))
        return inUse (t.index, p);

    // the plugin moved to another slot (or another rack of the bank has it elsewhere)
    for (int s = 0; s < Rack::maxSlots; ++s)
        if (s != t.index)
            if (auto* p = paramIn (s))
                return inUse (s, p);

    return nullptr;
}

int MidiControl::behaviour (const MidiTarget& t) const
{
    switch (t.kind)
    {
        case MidiTarget::rackMute: case MidiTarget::rackSolo: case MidiTarget::rackBypass: case MidiTarget::slotOn:
        case MidiTarget::masterMute: case MidiTarget::automixOn: case MidiTarget::shift:
            return MidiTarget::toggle;
        case MidiTarget::sceneNext: case MidiTarget::scenePrev: case MidiTarget::sceneRecall: case MidiTarget::bankNext:
        case MidiTarget::bankPrev: case MidiTarget::tapTempo: case MidiTarget::clearSolos:
            return MidiTarget::trigger;
        case MidiTarget::pluginParam:
        {
            Rack* r = nullptr;
            if (auto* p = findParameter (t, r))
                if (p->isBoolean() || (p->isDiscrete() && p->getNumSteps() == 2))
                    return MidiTarget::toggle;
            return MidiTarget::continuous;
        }
        default:
            return MidiTarget::continuous;
    }
}

float MidiControl::pickupWindow (const MidiMapping& m) const
{
    // a stepped parameter (1/8, 1/4.t...) only shows its step: half a step away is "there"
    if (m.target.kind == MidiTarget::pluginParam)
    {
        Rack* r = nullptr;
        if (auto* p = findParameter (m.target, r))
            if (const auto steps = p->getNumSteps(); p->isDiscrete() && steps > 1 && steps < 200)
                return juce::jmax (0.02f, 0.5f / (float) (steps - 1));
    }
    return 0.02f;
}

bool MidiControl::readTarget (const MidiTarget& t, float& v) const
{
    auto& engine = session.getEngine();
    Rack* r = t.isRackTarget() ? resolveRack (t) : nullptr;

    if (t.isRackTarget() && r == nullptr)
        return false;

    switch (t.kind)
    {
        case MidiTarget::rackGain:    v = midilaw::gainToNorm (r->gainDb.load()); return true;
        case MidiTarget::rackInput:   v = midilaw::inputToNorm (r->inputGainDb.load()); return true;
        case MidiTarget::rackMute:    v = r->muted.load() ? 1.0f : 0.0f; return true;
        case MidiTarget::rackSolo:    v = r->solo.load() ? 1.0f : 0.0f; return true;
        case MidiTarget::rackBypass:  v = r->bypassed.load() ? 1.0f : 0.0f; return true;
        case MidiTarget::rackWeight:  v = midilaw::weightToNorm (r->automixWeightDb.load()); return true;
        case MidiTarget::rackSend:
            if (! juce::isPositiveAndBelow (t.index - 1, Rack::maxBuses)) return false;
            v = midilaw::sendToNorm (r->sends[(size_t) t.index - 1].db.load());
            return true;
        case MidiTarget::slotOn:
            if (! juce::isPositiveAndBelow (t.index, Rack::maxSlots) || r->getPlugin (t.index) == nullptr) return false;
            v = r->getSlot (t.index).bypassed.load() ? 0.0f : 1.0f;
            return true;
        case MidiTarget::pluginParam:
        {
            Rack* pr = nullptr;
            if (auto* p = findParameter (t, pr)) { v = p->getValue(); return true; }
            return false;
        }
        case MidiTarget::masterMute:  v = engine.masterMuted.load() ? 1.0f : 0.0f; return true;
        case MidiTarget::automixOn:   v = engine.automixEnabled.load() ? 1.0f : 0.0f; return true;
        case MidiTarget::shift:       v = shiftHeld ? 1.0f : 0.0f; return true;
        case MidiTarget::bankPrev:    v = bankOffset > 0 ? 1.0f : 0.0f; return true;
        case MidiTarget::bankNext:    v = bankOffset + bankSize < (int) visibleRacks().size() ? 1.0f : 0.0f; return true;
        case MidiTarget::sceneRecall: v = session.getCurrentScene() == t.index ? 1.0f : 0.0f; return true;
        case MidiTarget::sceneNext:   v = 0.0f; return true;
        case MidiTarget::scenePrev:   v = 0.0f; return true;
        case MidiTarget::clearSolos:
        {
            v = 0.0f;
            for (int i = 0; i < engine.getNumRacks(); ++i)
                if (engine.getRack (i).solo.load()) v = 1.0f;   // lit while something is soloed
            return true;
        }
        case MidiTarget::tapTempo:
        {
            if (! juce::isPositiveAndBelow (t.index, engine.tempo.getNumTempos())) return false;
            const auto beat = engine.tempo.getBeatPosition (t.index, TempoEngine::nowSec());
            v = beat - std::floor (beat) < 0.2 ? 1.0f : 0.0f;   // blinks on the beat
            return true;
        }
        default:
            return false;
    }
}

void MidiControl::markRackEdited()
{
    rackEditPending = true;
    rackEditAt = juce::Time::getMillisecondCounter();
}

void MidiControl::writeTarget (const MidiTarget& t, float v)
{
    auto& engine = session.getEngine();
    Rack* r = t.isRackTarget() ? resolveRack (t) : nullptr;

    if (t.isRackTarget() && r == nullptr)
        return;

    v = juce::jlimit (0.0f, 1.0f, v);
    const bool on = v >= 0.5f;
    const auto others = r != nullptr && linkedRacks != nullptr ? linkedRacks (*r) : std::vector<Rack*>();

    switch (t.kind)
    {
        case MidiTarget::rackGain:
        {
            const auto db = midilaw::normToGain (v);
            const auto delta = db - r->gainDb.load();
            r->gainDb = db;
            for (auto* o : others) o->gainDb = juce::jlimit (-60.0f, 12.0f, o->gainDb.load() + delta);   // groups keep their offsets
            markRackEdited();
            break;
        }
        case MidiTarget::rackInput:
        {
            const auto db = midilaw::normToInput (v);
            const auto delta = db - r->inputGainDb.load();
            r->inputGainDb = db;
            for (auto* o : others) o->inputGainDb = juce::jlimit (Rack::minInputGainDb, Rack::maxInputGainDb, o->inputGainDb.load() + delta);
            markRackEdited();
            break;
        }
        case MidiTarget::rackMute:   r->muted = on;    for (auto* o : others) o->muted = on;    markRackEdited(); break;
        case MidiTarget::rackSolo:   r->solo = on;     for (auto* o : others) o->solo = on;     markRackEdited(); break;
        case MidiTarget::rackBypass: r->bypassed = on; for (auto* o : others) o->bypassed = on; markRackEdited(); break;
        case MidiTarget::rackWeight: r->automixWeightDb = midilaw::normToWeight (v); markRackEdited(); break;
        case MidiTarget::rackSend:
            if (juce::isPositiveAndBelow (t.index - 1, Rack::maxBuses))
            {
                engine.setSend (*r, t.index, midilaw::normToSend (v));
                markRackEdited();
            }
            break;
        case MidiTarget::slotOn:
            if (juce::isPositiveAndBelow (t.index, Rack::maxSlots) && r->getPlugin (t.index) != nullptr)
            {
                r->getSlot (t.index).bypassed = ! on;
                markRackEdited();
            }
            break;
        case MidiTarget::pluginParam:
        {
            Rack* pr = nullptr;
            if (auto* p = findParameter (t, pr))
            {
                p->beginChangeGesture();
                p->setValueNotifyingHost (v);
                p->endChangeGesture();
            }
            break;
        }
        case MidiTarget::masterMute: engine.masterMuted = on; break;
        case MidiTarget::automixOn:  engine.automixEnabled = on; markRackEdited(); break;
        case MidiTarget::shift:
            if (shiftHeld != on)
            {
                shiftHeld = on;
                if (onChange) onChange();
            }
            break;
        default:
            if (on) triggerTarget (t);
            break;
    }
}

void MidiControl::triggerTarget (const MidiTarget& t)
{
    auto& engine = session.getEngine();

    switch (t.kind)
    {
        case MidiTarget::sceneNext:   session.stepScene (1); break;
        case MidiTarget::scenePrev:   session.stepScene (-1); break;
        case MidiTarget::sceneRecall: session.recallScene (t.index); break;
        case MidiTarget::bankNext:    stepBank (1); break;
        case MidiTarget::bankPrev:    stepBank (-1); break;
        case MidiTarget::tapTempo:
            if (juce::isPositiveAndBelow (t.index, engine.tempo.getNumTempos()))
                engine.tempo.tapNow (t.index, TempoEngine::srcMidi);
            break;
        case MidiTarget::clearSolos:
            for (int i = 0; i < engine.getNumRacks(); ++i)
                engine.getRack (i).solo = false;
            markRackEdited();
            break;
        default:
        {
            float cur = 0.0f;   // a toggle target: flip
            if (readTarget (t, cur))
                writeTarget (t, cur >= 0.5f ? 0.0f : 1.0f);
            break;
        }
    }
}

juce::String MidiControl::describeTarget (const MidiTarget& t, bool withRack) const
{
    juce::String what;

    switch (t.kind)
    {
        case MidiTarget::rackGain:    what = "Gain"; break;
        case MidiTarget::rackInput:   what = "Input trim"; break;
        case MidiTarget::rackMute:    what = "Mute"; break;
        case MidiTarget::rackSolo:    what = "Solo"; break;
        case MidiTarget::rackBypass:  what = "Bypass"; break;
        case MidiTarget::rackWeight:  what = "Automix weight"; break;
        case MidiTarget::rackSend:    what = "Send " + letterOf (t.index); break;
        case MidiTarget::slotOn:
        {
            what = "Plugin " + juce::String (t.index + 1) + " on";
            if (auto* r = resolveRack (t))
                if (auto* p = r->getPlugin (t.index))
                    what = p->getName() + " on";
            break;
        }
        case MidiTarget::pluginParam:
        {
            Rack* r = nullptr;
            juce::String plugin = "Plugin " + juce::String (t.index + 1);
            auto* prm = findParameter (t, r);
            if (prm != nullptr && r != nullptr)
                for (int s = 0; s < Rack::maxSlots; ++s)
                    if (auto* p = r->getPlugin (s); p != nullptr && p->getParameters().contains (prm))
                        plugin = p->getName();
            what = plugin + " > " + (t.paramName.isNotEmpty() ? t.paramName : "param " + juce::String (t.param + 1));
            if (prm != nullptr && t.paramName.isNotEmpty() && prm->getName (64) != t.paramName)
                what << " (now " << prm->getName (64) << ")";   // the synced twin is in use
            break;
        }
        case MidiTarget::masterMute:  return "MUTE ALL";
        case MidiTarget::automixOn:   return "Automix on/off";
        case MidiTarget::sceneNext:   return "Next scene";
        case MidiTarget::scenePrev:   return "Previous scene";
        case MidiTarget::sceneRecall:
            return "Scene " + juce::String (t.index + 1)
                 + (juce::isPositiveAndBelow (t.index, session.getNumScenes()) ? ": " + session.getScene (t.index).name : juce::String());
        case MidiTarget::bankNext:    return "Bank >";
        case MidiTarget::bankPrev:    return "Bank <";
        case MidiTarget::tapTempo:    return t.index == 0 ? juce::String ("Tap tempo") : "Tap tempo T" + juce::String (t.index);
        case MidiTarget::shift:       return "Shift";
        case MidiTarget::clearSolos:  return "Clear solos";
        default:                      return "-";
    }

    if (! withRack)
        return what;

    juce::String rackName;
    auto* r = resolveRack (t);
    const auto name = r != nullptr ? (r->name.isNotEmpty() ? r->name : "Rack " + juce::String (session.getEngine().indexOf (*r) + 1)) : juce::String();

    switch (t.rackMode)
    {
        case MidiTarget::bankRack:     rackName = "Bank " + juce::String (t.rack + 1) + (r != nullptr ? " (" + name + ")" : juce::String (" (empty)")); break;
        case MidiTarget::selectedRack: rackName = "Selected" + (r != nullptr ? " (" + name + ")" : juce::String()); break;
        default:                       rackName = r != nullptr ? name : juce::String ("Missing rack"); break;
    }
    return rackName + " > " + what;
}

static juce::String dbText (float db)
{
    return juce::String (std::abs (db) < 0.05f ? 0.0f : db, 1) + " dB";   // no "-0.0"
}

juce::String MidiControl::valueText (const MidiTarget& t, float v) const
{
    switch (t.kind)
    {
        case MidiTarget::rackGain:   return dbText (midilaw::normToGain (v));
        case MidiTarget::rackInput:  return dbText (midilaw::normToInput (v));
        case MidiTarget::rackWeight: return dbText (midilaw::normToWeight (v));
        case MidiTarget::rackSend:   { const auto db = midilaw::normToSend (v); return db <= Rack::sendOffDb ? juce::String ("off") : dbText (db); }
        case MidiTarget::pluginParam:
        {
            Rack* r = nullptr;
            if (auto* p = findParameter (t, r))
                return p->getText (v, 16) + p->getLabel().trim().quoted (' ').unquoted();
            return juce::String (juce::roundToInt (v * 100.0f)) + "%";
        }
        default:
            return behaviour (t) == MidiTarget::continuous ? juce::String (juce::roundToInt (v * 100.0f)) + "%"
                                                           : juce::String (v >= 0.5f ? "On" : "Off");
    }
}

juce::String MidiControl::describeSource (const MidiMapping& m) const
{
    const auto name = midiprofiles::controlName (midiprofiles::detect (m.deviceName), m.source);
    return name.isNotEmpty() ? name : m.source.describe();
}

juce::String MidiControl::labelFor (const MidiTarget& t) const
{
    juce::StringArray names;
    for (auto& m : mappings)
        if (m.target == t)
            names.addIfNotAlreadyThere (describeSource (m) + (m.layer == MidiMapping::withShift ? " (shift)" : ""));
    return names.joinIntoString (", ");
}

//==============================================================================
// Mappings
MidiTarget MidiControl::canonical (MidiTarget t) const
{
    if (t.kind != MidiTarget::pluginParam)
        return t;

    if (auto* r = resolveRack (t))
        if (auto* p = r->getPlugin (t.index))
        {
            auto& params = p->getParameters();
            if (juce::isPositiveAndBelow (t.param, params.size()))
                if (auto* c = paramtwins::canonical (params, params[t.param]); c != params[t.param])
                {
                    t.param = c->getParameterIndex();   // "Rate Sync" and "Rate" are one control: stored as the free one
                    t.paramName = c->getName (64);
                }
        }
    return t;
}

int MidiControl::addMapping (MidiMapping m)
{
    m.id = nextId++;
    m.target = canonical (m.target);
    mappings.push_back (m);
    changed();
    return m.id;
}

void MidiControl::updateMapping (const MidiMapping& updated)
{
    for (auto& m : mappings)
        if (m.id == updated.id)
        {
            const auto runtimeHw = m.lastHw;
            const bool sourceChanged = ! (m.source == updated.source) || ! (m.target == updated.target);
            m = updated;
            m.target = canonical (m.target);
            m.lastHw = sourceChanged ? -1.0f : runtimeHw;
            m.engaged = false;
            m.lastSet = -1.0f;
        }
    changed();
    sendFeedback (true);
}

void MidiControl::removeMapping (int id)
{
    mappings.erase (std::remove_if (mappings.begin(), mappings.end(), [id] (auto& m) { return m.id == id; }), mappings.end());
    changed();
}

void MidiControl::removeMappingsFor (const MidiTarget& t)
{
    mappings.erase (std::remove_if (mappings.begin(), mappings.end(), [&] (auto& m) { return m.target == t; }), mappings.end());
    changed();
}

void MidiControl::clearMappings()
{
    mappings.clear();
    changed();
}

std::vector<const MidiMapping*> MidiControl::mappingsFor (const MidiTarget& t) const
{
    std::vector<const MidiMapping*> v;
    for (auto& m : mappings)
        if (m.target == t)
            v.push_back (&m);
    return v;
}

const MidiMapping* MidiControl::findMapping (int id) const
{
    for (auto& m : mappings)
        if (m.id == id)
            return &m;
    return nullptr;
}

int MidiControl::quickSetup (const juce::String& deviceId)
{
    juce::String name = deviceNameFor (deviceId);
    for (auto& c : getControllers())
        if (c.id == deviceId)
            name = c.name;

    auto layout = midiprofiles::quickSetup (midiprofiles::detect (name), deviceId, name);
    if (layout.empty())
        return 0;

    mappings.erase (std::remove_if (mappings.begin(), mappings.end(),
                                    [&] (auto& m) { return m.device == deviceId || (m.deviceName == name && name.isNotEmpty()); }),
                    mappings.end());

    for (auto& m : layout)
    {
        m.id = nextId++;
        mappings.push_back (m);
    }

    if (! isControllerEnabled (deviceId))
        setControllerEnabled (deviceId, name, true);

    changed();
    sendFeedback (true);
    return (int) layout.size();
}

//==============================================================================
// Map mode
void MidiControl::setMapMode (bool on)
{
    if (mapMode == on)
        return;

    mapMode = on;
    learnTarget = {};
    justLearnedId = -1;
    learning = learnMappingId >= 0;
    lastDeviceScan = 0;   // map mode listens to every MIDI input (to learn from a new controller)
    if (onChange) onChange();
}

void MidiControl::setLearnTarget (const MidiTarget& t)
{
    learnTarget = canonical (t);
    justLearnedId = -1;
    learning = (mapMode && t.isValid()) || learnMappingId >= 0;
    if (onChange) onChange();
}

void MidiControl::learnSourceFor (int mappingId)
{
    learnMappingId = mappingId;
    learning = mappingId >= 0 || (mapMode && learnTarget.isValid());
    lastDeviceScan = 0;
    if (onChange) onChange();
}

//==============================================================================
// Feedback
int MidiControl::feedbackValue (const MidiMapping& m) const
{
    float v = 0.0f;
    if (! readTarget (m.target, v))
        return m.source.type == MidiSource::note || behaviour (m.target) != MidiTarget::continuous ? m.offValue : 0;   // nothing there: dark

    if (behaviour (m.target) == MidiTarget::continuous && m.source.type != MidiSource::note)
        return juce::roundToInt (positionOf (m, v) * (float) m.source.maxValue());

    bool on = behaviour (m.target) == MidiTarget::continuous ? positionOf (m, v) >= 0.5f : v >= 0.5f;
    if (m.minValue > m.maxValue && behaviour (m.target) != MidiTarget::continuous)
        on = ! on;   // inverted range: LED lit when off
    return on ? m.onValue : m.offValue;
}

void MidiControl::sendFeedback (bool force)
{
    // nobody to light: don't even read the values (this runs 50x/s on the message thread)
    if (feedbackSinkForTests == nullptr
        && std::none_of (outputs.begin(), outputs.end(), [] (auto& o) { return o.second != nullptr; }))
        return;

    // one value per LED address and controller: the mapping of the current shift layer wins
    struct Out { const MidiMapping* m; int value; };
    std::map<juce::String, Out> chosen;

    for (auto& m : mappings)
    {
        if (! m.feedback || ! m.source.isValid() || ! layerMatches (m))
            continue;

        const auto num = m.feedbackNumber >= 0 ? m.feedbackNumber : m.source.number;
        const auto key = m.device + "|" + juce::String (m.source.type) + "|" + juce::String (m.source.channel) + "|" + juce::String (num);

        if (auto it = chosen.find (key); it != chosen.end() && (it->second.m->layer != MidiMapping::anyLayer || m.layer == MidiMapping::anyLayer))
            continue;

        chosen[key] = { &m, feedbackValue (m) };
    }

    for (auto& [key, out] : chosen)
    {
        auto& m = *out.m;

        // last value per LED address (the mapping that owns it may change with the shift layer)
        auto& sent = sentValues[key];
        if (! force && sent == out.value)
            continue;
        sent = out.value;

        const auto num = m.feedbackNumber >= 0 ? m.feedbackNumber : m.source.number;
        juce::MidiMessage msg;

        switch (m.source.type)
        {
            case MidiSource::cc:        msg = juce::MidiMessage::controllerEvent (m.source.channel, num, juce::jlimit (0, 127, out.value)); break;
            case MidiSource::note:      msg = juce::MidiMessage::noteOn (m.source.channel, num, (juce::uint8) juce::jlimit (0, 127, out.value)); break;
            case MidiSource::pitchBend: msg = juce::MidiMessage::pitchWheel (m.source.channel, juce::jlimit (0, 16383, out.value)); break;
            default: continue;
        }

        auto sendTo = [&] (const juce::String& id)
        {
            if (feedbackSinkForTests != nullptr)
                feedbackSinkForTests (id, msg);
            else if (auto* o = outputFor (id))
                o->sendMessageNow (msg);
        };

        if (m.device.isNotEmpty())
            sendTo (m.device);
        else
            for (auto& c : controllers)
                if (c.enabled)
                    sendTo (c.id);
    }
}

//==============================================================================
void MidiControl::timerCallback()
{
    const auto now = juce::Time::getMillisecondCounter();

    // devices (when the list changed, a setting changed or map mode started): open the enabled
    // controllers, their feedback outputs
    if (devicesOpen && lastDeviceScan == 0)
    {
        lastDeviceScan = now == 0 ? 1 : now;
        auto& dm = session.getEngine().getDeviceManager();
        const auto ins = juce::MidiInput::getAvailableDevices();
        const auto outs = juce::MidiOutput::getAvailableDevices();
        bool reconnected = false;

        for (auto& d : ins)
        {
            for (auto& c : controllers)
                if (c.enabled && c.id != d.identifier && c.name == d.name)
                {
                    // same controller, new identifier (another USB port): adopt it
                    for (auto& m : mappings)
                        if (m.device == c.id)
                            m.device = d.identifier;
                    outputs.erase (c.id);
                    c.id = d.identifier;
                    reconnected = true;
                }

            // known controllers are used as soon as they are plugged in (unless switched off)
            if (midiprofiles::detect (d.name) != midiprofiles::generic
                && std::none_of (controllers.begin(), controllers.end(), [&] (auto& c) { return c.id == d.identifier || c.name == d.name; }))
            {
                Controller c;
                c.id = d.identifier; c.name = d.name; c.enabled = true;
                controllers.push_back (c);
                reconnected = true;
                if (onSettingsChanged) onSettingsChanged();
            }

            const bool wanted = isControllerEnabled (d.identifier) || ((mapMode || learnMappingId >= 0));
            if (wanted && ! dm.isMidiInputDeviceEnabled (d.identifier))
            {
                dm.setMidiInputDeviceEnabled (d.identifier, true);
                if (! isControllerEnabled (d.identifier))
                    enabledForLearning.addIfNotAlreadyThere (d.identifier);
                reconnected = true;
            }
        }

        // map mode over: inputs opened only to learn from them are closed again
        if (! mapMode && learnMappingId < 0)
        {
            for (auto& id : enabledForLearning)
                if (! isControllerEnabled (id))
                    dm.setMidiInputDeviceEnabled (id, false);
            enabledForLearning.clear();
        }

        // feedback outputs
        for (auto& c : controllers)
        {
            const bool present = std::any_of (ins.begin(), ins.end(), [&] (auto& d) { return d.identifier == c.id; });

            if (! c.enabled || ! present || c.outputId == "-")
            {
                outputs.erase (c.id);
                continue;
            }

            if (outputs.count (c.id) != 0 && outputs[c.id] != nullptr)
                continue;

            juce::String outId = c.outputId;
            if (outId.isEmpty())
            {
                for (auto& o : outs) if (o.name == c.name) outId = o.identifier;
                if (outId.isEmpty())
                    for (auto& o : outs)
                        if (o.name.containsIgnoreCase (c.name) || c.name.containsIgnoreCase (o.name))
                            outId = o.identifier;
            }

            if (outId.isNotEmpty())
            {
                outputs[c.id] = juce::MidiOutput::openDevice (outId);
                reconnected = reconnected || outputs[c.id] != nullptr;
            }
        }

        if (reconnected)
        {
            rebuildSourceIndex();
            sentValues.clear();   // a controller (re)appeared: all its LEDs again
            if (onChange) onChange();
        }
    }

    // a fader move = one undo step (and the session is marked changed)
    if (rackEditPending && now - rackEditAt > 600)
    {
        rackEditPending = false;
        session.markDirty();
    }

    sendFeedback (false);
}

//==============================================================================
// Monitor
void MidiControl::logEvent (const juce::String& device, const MidiSource& src, int value, const juce::String& action)
{
    Event e;
    e.time = juce::Time::getMillisecondCounter();
    e.device = device; e.source = src; e.value = value; e.action = action;

    if (events.size() < 200)
        events.push_back (e);
    else
        events[eventCount % 200] = e;

    ++eventCount;
    if (onChange) onChange();
}

std::vector<MidiControl::Event> MidiControl::getRecentEvents() const
{
    std::vector<Event> v;
    const auto n = (juce::uint32) events.size();

    for (juce::uint32 i = 0; i < n; ++i)   // oldest first
        v.push_back (events[(size_t) ((eventCount - n + i) % juce::jmax (1u, n == 200 ? 200u : n))]);
    return v;
}

//==============================================================================
// Session and presets
void MidiControl::changed (bool store)
{
    rebuildSourceIndex();
    if (store) storeToSession();
    if (onChange) onChange();
}

void MidiControl::storeToSession()
{
    session.setMidiMap (toTree(), true);
}

juce::ValueTree MidiControl::toTree() const
{
    juce::ValueTree t ("MIDIMAP");
    t.setProperty ("bankSize", bankSize, nullptr).setProperty ("bank", bankOffset, nullptr);
    for (auto& m : mappings)
        t.appendChild (m.toTree(), nullptr);
    return t;
}

void MidiControl::fromTree (const juce::ValueTree& t)
{
    mappings.clear();
    bankSize = juce::jlimit (1, 64, (int) t.getProperty ("bankSize", 8));
    bankOffset = juce::jmax (0, (int) t["bank"]);

    for (const auto& c : t)
        if (c.hasType ("MAP"))
        {
            auto m = MidiMapping::fromTree (c);
            m.id = nextId++;
            if (m.source.isValid() && m.target.isValid())
                mappings.push_back (m);
        }

    learnTarget = {};
    justLearnedId = -1;
    sentValues.clear();
    changed (false);
}

void MidiControl::loadFromSession()
{
    auto t = session.getMidiMap();

    // a session saved without a MIDI map plays with the default one
    if (! t.isValid() && defaultMap.isValid())
    {
        fromTree (defaultMap);
        session.setMidiMap (toTree(), false);
        return;
    }
    fromTree (t.isValid() ? t : juce::ValueTree ("MIDIMAP"));
}

bool MidiControl::savePreset (const juce::File& f) const
{
    auto t = toTree();
    t.removeProperty ("bank", nullptr);
    auto xml = t.createXml();
    return xml != nullptr && xml->writeTo (f);
}

bool MidiControl::loadPreset (const juce::File& f)
{
    auto xml = juce::XmlDocument::parse (f);
    if (xml == nullptr)
        return false;

    auto t = juce::ValueTree::fromXml (*xml);
    if (! t.hasType ("MIDIMAP"))
        return false;

    t.setProperty ("bank", bankOffset, nullptr);
    fromTree (t);
    storeToSession();
    sendFeedback (true);
    return true;
}

} // namespace stagerack
