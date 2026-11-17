#pragma once

#include "MidiMap.h"
#include "MidiProfiles.h"

namespace stagerack
{

class Session;
class Rack;

/**
    MIDI remote control (Ableton style MIDI map): any controller drives rack gains, mutes, sends,
    plugin parameters, scenes, the tap tempo... with LED / motor-fader feedback.

    Threads: MIDI arrives on the CoreMIDI thread (AudioEngine's MIDI callback -> handleMidi),
    is queued and applied on the message thread (racks, plugins and the session live there).
    The feedback (LEDs) is sent from a 50 Hz timer, only when a value changed.

    The mappings belong to the session (saved in the .stagerack file); which controllers are
    used is an app setting (the hardware belongs to this Mac). A session without mappings
    starts from the default map, if one was set.
*/
class MidiControl final : private juce::AsyncUpdater,
                          private juce::Timer
{
public:
    explicit MidiControl (Session&);
    ~MidiControl() override;

    //==============================================================================
    // Controllers (message thread)
    struct Controller
    {
        juce::String id, name;            // MIDI input
        bool enabled = false;             // used for control
        juce::String outputId;            // feedback output: "" = automatic (same name), "-" = none
        bool connected = false;           // present now
        juce::String resolvedOutputName;  // the output actually used ("" = none)
        midiprofiles::Profile profile = midiprofiles::generic;
    };
    /** Every MIDI input present or enabled (enabled ones that are missing included). */
    std::vector<Controller> getControllers() const;
    void setControllerEnabled (const juce::String& id, const juce::String& name, bool);
    void setFeedbackOutput (const juce::String& inputId, const juce::String& outputId);
    bool isControllerEnabled (const juce::String& id) const;
    /** Settings (app): enabled controllers and their outputs. */
    juce::ValueTree controllersToTree() const;
    void controllersFromTree (const juce::ValueTree&);
    /** Enables the controllers' inputs in the device manager (call once the audio device is up). */
    void openDevices();

    //==============================================================================
    // Incoming MIDI (any thread). True = the message drives a mapping (or is being learned):
    // the other MIDI functions (scene program change / CCs, tempo taps) ignore it.
    bool handleMidi (const juce::String& deviceId, const juce::String& deviceName, const juce::MidiMessage&);

    //==============================================================================
    // Mappings (message thread)
    const std::vector<MidiMapping>& getMappings() const noexcept   { return mappings; }
    int addMapping (MidiMapping);
    void updateMapping (const MidiMapping&);        // by id (keeps the runtime state)
    void removeMapping (int id);
    void removeMappingsFor (const MidiTarget&);
    void clearMappings();
    std::vector<const MidiMapping*> mappingsFor (const MidiTarget&) const;
    const MidiMapping* findMapping (int id) const;
    /** Quick setup for a known controller: replaces that controller's mappings. */
    int quickSetup (const juce::String& deviceId);

    //==============================================================================
    // Map mode and learn (message thread)
    void setMapMode (bool);
    bool isMapMode() const noexcept                 { return mapMode; }
    /** The control selected in map mode: the next control moved on a controller is mapped to it. */
    void setLearnTarget (const MidiTarget&);
    const MidiTarget& getLearnTarget() const noexcept { return learnTarget; }
    /** Learn only the control of an existing mapping (MIDI window), -1 = stop. */
    void learnSourceFor (int mappingId);
    int getLearningMapping() const noexcept         { return learnMappingId; }
    /** How map mode maps a rack's control: that rack, its place in the bank, or the selected rack. */
    int learnRackMode = MidiTarget::fixedRack;

    //==============================================================================
    // Bank: the controller's strips follow racks bankOffset .. bankOffset + bankSize - 1
    // (visible racks, in order).
    int getBankOffset() const noexcept              { return bankOffset; }
    int getBankSize() const noexcept                { return bankSize; }
    void setBankSize (int);
    void setBankOffset (int);
    void stepBank (int direction);                  // by one bank
    std::vector<Rack*> visibleRacks() const;
    /** Position of the rack in the bank (0-based), or -1. */
    int bankPosition (const Rack&) const;
    bool hasBankMappings() const;

    //==============================================================================
    // Targets
    Rack* resolveRack (const MidiTarget&) const;
    int behaviour (const MidiTarget&) const;        // MidiTarget::Behaviour
    bool readTarget (const MidiTarget&, float& normalised) const;
    void writeTarget (const MidiTarget&, float normalised);
    void triggerTarget (const MidiTarget&);
    juce::String describeTarget (const MidiTarget&, bool withRack = true) const;
    juce::String valueText (const MidiTarget&, float normalised) const;
    /** "Fader 3" (known controller) or "CC 27". */
    juce::String describeSource (const MidiMapping&) const;
    /** Short label for map mode: the control(s) mapped to this target, or empty. */
    juce::String labelFor (const MidiTarget&) const;

    bool isShiftHeld() const noexcept               { return shiftHeld; }

    //==============================================================================
    // MIDI monitor (MIDI window)
    struct Event
    {
        juce::uint32 time = 0;
        juce::String device;
        MidiSource source;
        int value = 0;
        juce::String action;    // what it did ("Gain Voce 1", "learned", "no mapping")
    };
    std::vector<Event> getRecentEvents() const;
    juce::uint32 getEventCount() const noexcept     { return eventCount; }

    //==============================================================================
    // Session and presets
    void loadFromSession();                         // after a session was opened / created
    juce::ValueTree toTree() const;                 // mappings + bank
    void fromTree (const juce::ValueTree&);
    bool savePreset (const juce::File&) const;
    bool loadPreset (const juce::File&);
    /** Mappings for new sessions and sessions saved without any (app setting). */
    juce::ValueTree defaultMap;

    /** Callbacks set by the UI: the selected rack; the racks an edit also goes to (edit groups). */
    std::function<Rack*()> selectedRack;
    std::function<std::vector<Rack*> (Rack&)> linkedRacks;
    /** Something to redraw: mappings, map mode, learn target, bank, monitor. */
    std::function<void()> onChange;
    /** Saved settings changed (controllers). */
    std::function<void()> onSettingsChanged;

    /** Tests: feeds a message as if it came from this controller and processes it now. */
    void injectForTests (const juce::String& deviceId, const juce::MidiMessage&);
    /** Tests: what the feedback would send (instead of a MIDI output). */
    std::function<void (const juce::String& deviceId, const juce::MidiMessage&)> feedbackSinkForTests;
    void runFeedbackForTests()                      { sendFeedback (true); }

private:
    void handleAsyncUpdate() override;
    void timerCallback() override;

    struct Incoming { juce::String device, deviceName; juce::MidiMessage message; };
    void process (const Incoming&);
    void apply (MidiMapping&, const MidiSource&, int value);
    bool learn (const Incoming&, const MidiSource&, int value);
    void sendFeedback (bool force);
    int feedbackValue (const MidiMapping&) const;   // -1 = nothing to send
    void changed (bool store = true);
    void storeToSession();
    void rebuildSourceIndex();
    void logEvent (const juce::String& device, const MidiSource&, int value, const juce::String& action);
    juce::String deviceNameFor (const juce::String& id) const;
    juce::MidiOutput* outputFor (const juce::String& inputId);
    void markRackEdited();
    bool layerMatches (const MidiMapping&) const noexcept;
    float positionOf (const MidiMapping&, float targetNorm) const noexcept;
    float targetOf (const MidiMapping&, float position) const noexcept;
    juce::AudioProcessorParameter* findParameter (const MidiTarget&, Rack*&) const;
    /** A plugin parameter with a tempo-synced twin is stored as the free one (one control for both). */
    MidiTarget canonical (MidiTarget) const;
    float pickupWindow (const MidiMapping&) const;

    Session& session;
    std::vector<MidiMapping> mappings;
    int nextId = 1;

    std::vector<Controller> controllers;            // enabled ones (+ their output choice)
    std::map<juce::String, std::unique_ptr<juce::MidiOutput>> outputs;   // by input id
    juce::uint32 lastDeviceScan = 0;
    bool devicesOpen = false;
    juce::MidiDeviceListConnection deviceListConnection;   // CoreMIDI says the device list changed -> rescan                        // openDevices() called (CoreAudio / the device manager is up)
    juce::StringArray enabledForLearning;            // inputs opened only while map mode listens
    std::map<juce::String, int> sentValues;          // feedback: last value per controller + LED address

    // incoming queue (MIDI thread -> message thread)
    std::mutex queueLock;
    std::vector<Incoming> queue;
    // sources mapped, per device (read on the MIDI thread to answer "consumed?")
    mutable std::mutex sourceLock;
    std::vector<std::pair<juce::String, MidiSource>> sourceIndex;
    std::atomic<bool> learning { false };

    bool mapMode = false;
    MidiTarget learnTarget;
    int learnMappingId = -1, justLearnedId = -1;
    bool shiftHeld = false;

    int bankOffset = 0, bankSize = 8;
    bool rackEditPending = false;
    juce::uint32 rackEditAt = 0;

    std::vector<Event> events;                      // ring
    juce::uint32 eventCount = 0;

    JUCE_DECLARE_NON_COPYABLE (MidiControl)
};

} // namespace stagerack
