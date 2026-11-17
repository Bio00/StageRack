#pragma once

#include <JuceHeader.h>

namespace stagerack
{

/** A control on a MIDI controller: a CC, a note (button / pad) or pitch bend (14-bit faders). */
struct MidiSource
{
    enum Type { none = 0, cc, note, pitchBend };
    int type = none, channel = 1, number = 0;   // channel 1..16

    bool isValid() const noexcept                      { return type != none; }
    bool operator== (const MidiSource&) const = default;

    /** "CC 19", "Note 1", "Pitch bend" (+ " ch 2" when not channel 1). */
    juce::String describe() const
    {
        juce::String s = type == cc ? "CC " + juce::String (number)
                       : type == note ? "Note " + juce::String (number)
                       : type == pitchBend ? juce::String ("Pitch bend") : juce::String ("-");
        return channel != 1 && isValid() ? s + " ch " + juce::String (channel) : s;
    }

    /** The source of a message and its value: 0..127 (notes: velocity, 0 = released), 0..16383
        for pitch bend. Returns an invalid source for anything else (clock, aftertouch...). */
    static MidiSource fromMessage (const juce::MidiMessage& m, int& value)
    {
        MidiSource s;
        s.channel = m.getChannel();

        if (m.isController())         { s.type = cc; s.number = m.getControllerNumber(); value = m.getControllerValue(); }
        else if (m.isNoteOn (true))   { s.type = note; s.number = m.getNoteNumber(); value = m.getVelocity(); }
        else if (m.isNoteOff (false)) { s.type = note; s.number = m.getNoteNumber(); value = 0; }
        else if (m.isPitchWheel())    { s.type = pitchBend; s.number = 0; value = m.getPitchWheelValue(); }
        return s;
    }

    int maxValue() const noexcept                      { return type == pitchBend ? 16383 : 127; }
};

/** What a control drives: a rack's control (a fixed rack, the n-th rack of the controller's bank,
    or the selected rack), or something global. */
struct MidiTarget
{
    enum Kind
    {
        none = 0,
        rackGain, rackInput, rackMute, rackSolo, rackBypass, rackSend, rackWeight, slotOn, pluginParam,
        masterMute = 100, automixOn, sceneNext, scenePrev, sceneRecall, bankNext, bankPrev, tapTempo, shift, clearSolos
    };
    enum RackMode { fixedRack = 0, bankRack, selectedRack };
    enum Behaviour { continuous, toggle, trigger };

    int kind = none;
    int rackMode = fixedRack;
    int rack = 0;        // fixed: rack id; bank: position in the bank (0-based)
    int index = 0;       // slot (slotOn, pluginParam), bus letter 1..8 (rackSend), scene (sceneRecall), tempo (tapTempo)
    int param = -1;      // pluginParam: parameter index
    juce::String paramName;   // pluginParam: name when mapped (display, and to find it again if the plugin moved)

    bool isValid() const noexcept                      { return kind != none; }
    bool isRackTarget() const noexcept                 { return kind > none && kind < masterMute; }
    bool operator== (const MidiTarget& o) const noexcept
    {
        return kind == o.kind && (! isRackTarget() || (rackMode == o.rackMode && rack == o.rack))
            && index == o.index && param == o.param;
    }

    static MidiTarget global (int k, int idx = 0)      { MidiTarget t; t.kind = k; t.index = idx; return t; }
    static MidiTarget forRack (int k, int mode, int r, int idx = 0, int prm = -1)
    {
        MidiTarget t; t.kind = k; t.rackMode = mode; t.rack = r; t.index = idx; t.param = prm; return t;
    }

    juce::ValueTree toTree() const
    {
        juce::ValueTree t ("TARGET");
        t.setProperty ("kind", kind, nullptr);
        if (isRackTarget()) t.setProperty ("rackMode", rackMode, nullptr).setProperty ("rack", rack, nullptr);
        if (index != 0)     t.setProperty ("index", index, nullptr);
        if (param >= 0)     t.setProperty ("param", param, nullptr).setProperty ("paramName", paramName, nullptr);
        return t;
    }

    static MidiTarget fromTree (const juce::ValueTree& t)
    {
        MidiTarget m;
        m.kind = t["kind"]; m.rackMode = t["rackMode"]; m.rack = t["rack"]; m.index = t["index"];
        m.param = t.getProperty ("param", -1); m.paramName = t["paramName"].toString();
        return m;
    }
};

/** One mapping: a control on a controller -> a target, with its range and behaviour
    (Ableton's MIDI map: min/max, takeover; plus LED feedback and a shift layer). */
struct MidiMapping
{
    enum Mode { autoMode = 0, absolute, toggle, momentary, trigger, relTwos, relBinOffset, relSignMag, numModes };
    enum Takeover { takeoverNone = 0, pickup, scale };
    enum Layer { anyLayer = 0, noShift, withShift };

    int id = 0;
    juce::String device, deviceName;   // controller (MIDI input identifier, name); empty = any
    MidiSource source;
    MidiTarget target;
    float minValue = 0.0f, maxValue = 1.0f;   // target range (normalised); min > max = inverted
    int mode = autoMode;
    int takeover = pickup;
    bool feedback = true;                     // LEDs / motor faders / rings follow the target
    int onValue = 127, offValue = 0;          // LED velocities / CC values for on and off (colours on some pads)
    int feedbackNumber = -1;                  // LED on another note / CC than the input (-1 = same)
    int layer = anyLayer;                     // only with / without the Shift control held

    // runtime (message thread, not saved)
    float lastHw = -1.0f;        // last position of the hardware control (0..1 in the mapping's range)
    float lastSet = -1.0f;       // value we last wrote (pickup: someone else changed it -> disengage)
    bool engaged = false;        // pickup: the control has caught the value
    int resolvedRack = -1;       // rack id the target pointed at last time (bank moved -> disengage)
    double lastPress = 0.0;      // button debounce
    bool held = false;

    juce::ValueTree toTree() const
    {
        juce::ValueTree t ("MAP");
        t.setProperty ("device", device, nullptr).setProperty ("deviceName", deviceName, nullptr)
         .setProperty ("type", source.type, nullptr).setProperty ("ch", source.channel, nullptr).setProperty ("num", source.number, nullptr)
         .setProperty ("min", minValue, nullptr).setProperty ("max", maxValue, nullptr)
         .setProperty ("mode", mode, nullptr).setProperty ("takeover", takeover, nullptr)
         .setProperty ("feedback", feedback, nullptr).setProperty ("on", onValue, nullptr).setProperty ("off", offValue, nullptr)
         .setProperty ("ledNum", feedbackNumber, nullptr).setProperty ("layer", layer, nullptr);
        t.appendChild (target.toTree(), nullptr);
        return t;
    }

    static MidiMapping fromTree (const juce::ValueTree& t)
    {
        MidiMapping m;
        m.device = t["device"].toString(); m.deviceName = t["deviceName"].toString();
        m.source.type = t["type"]; m.source.channel = t.getProperty ("ch", 1); m.source.number = t["num"];
        m.minValue = t.getProperty ("min", 0.0f); m.maxValue = t.getProperty ("max", 1.0f);
        m.mode = juce::jlimit (0, (int) numModes - 1, (int) t["mode"]);
        m.takeover = t.getProperty ("takeover", (int) pickup);
        m.feedback = t.getProperty ("feedback", true);
        m.onValue = t.getProperty ("on", 127); m.offValue = t.getProperty ("off", 0);
        m.feedbackNumber = t.getProperty ("ledNum", -1); m.layer = t["layer"];
        m.target = MidiTarget::fromTree (t.getChildWithName ("TARGET"));
        return m;
    }

    static juce::String modeName (int mode)
    {
        static const char* names[] = { "Auto", "Absolute", "Toggle", "Momentary", "Trigger",
                                       "Relative (2's compl.)", "Relative (offset 64)", "Relative (sign bit)" };
        return juce::isPositiveAndBelow (mode, (int) numModes) ? names[mode] : "?";
    }
};

//==============================================================================
/** Value laws shared by the UI and the MIDI mapping (normalised 0..1 <-> units). */
namespace midilaw
{
    inline const juce::NormalisableRange<float>& gainRange()
    {
        static const auto r = [] { juce::NormalisableRange<float> n (-60.0f, 12.0f); n.setSkewForCentre (-12.0f); return n; }();
        return r;
    }
    inline const juce::NormalisableRange<float>& sendRange()   // SendsPanel's law (0 = off)
    {
        static const auto r = [] { juce::NormalisableRange<float> n (-60.0f, 6.0f); n.setSkewForCentre (-12.0f); return n; }();
        return r;
    }
    inline float gainToNorm (float db) noexcept   { return gainRange().convertTo0to1 (juce::jlimit (-60.0f, 12.0f, db)); }
    inline float normToGain (float v) noexcept    { return gainRange().convertFrom0to1 (juce::jlimit (0.0f, 1.0f, v)); }
    inline float inputToNorm (float db) noexcept  { return juce::jlimit (0.0f, 1.0f, (db + 24.0f) / 48.0f); }
    inline float normToInput (float v) noexcept   { return juce::jlimit (0.0f, 1.0f, v) * 48.0f - 24.0f; }
    inline float weightToNorm (float db) noexcept { return juce::jlimit (0.0f, 1.0f, (db + 20.0f) / 26.0f); }
    inline float normToWeight (float v) noexcept  { return juce::jlimit (0.0f, 1.0f, v) * 26.0f - 20.0f; }
    inline float sendToNorm (float db) noexcept   { return db <= -100.0f ? 0.0f : sendRange().convertTo0to1 (juce::jlimit (-60.0f, 6.0f, db)); }
    inline float normToSend (float v) noexcept    { return v <= 0.005f ? -100.0f : sendRange().convertFrom0to1 (juce::jlimit (0.0f, 1.0f, v)); }
}

} // namespace stagerack
