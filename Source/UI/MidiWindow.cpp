#include "MidiWindow.h"
#include "Theme.h"

namespace stagerack
{

static juce::String rackLabel (Session& s, const Rack& r)
{
    return r.name.isNotEmpty() ? r.name : "Rack " + juce::String (s.getEngine().indexOf (r) + 1);
}

//==============================================================================
// Target menu
static void addRackTargets (juce::PopupMenu& menu, Session& session, MidiControl& midi, int mode, int rackRef, Rack* resolved,
                            const std::function<void (MidiTarget)>& chosen)
{
    auto item = [&] (const juce::String& name, int kind, int index = 0)
    {
        menu.addItem (name, [chosen, kind, mode, rackRef, index] { chosen (MidiTarget::forRack (kind, mode, rackRef, index)); });
    };
    item ("Gain", MidiTarget::rackGain);
    item ("Input trim", MidiTarget::rackInput);
    item ("Mute", MidiTarget::rackMute);
    item ("Solo", MidiTarget::rackSolo);
    item ("Bypass", MidiTarget::rackBypass);
    item ("Automix weight", MidiTarget::rackWeight);

    juce::PopupMenu sends;
    for (int b = 1; b <= Rack::maxBuses; ++b)
    {
        juce::String name = "Send " + juce::String::charToString ((juce::juce_wchar) ('A' + b - 1));
        if (auto* bus = session.getEngine().findBus (b))
            name << "  (" << rackLabel (session, *bus) << ")";
        sends.addItem (name, [chosen, mode, rackRef, b] { chosen (MidiTarget::forRack (MidiTarget::rackSend, mode, rackRef, b)); });
    }
    menu.addSubMenu ("Sends", sends);

    juce::PopupMenu plugins;
    for (int s = 0; s < Rack::maxSlots; ++s)
    {
        auto* p = resolved != nullptr ? resolved->getPlugin (s) : nullptr;
        juce::PopupMenu slot;
        slot.addItem ("On / off", [chosen, mode, rackRef, s] { chosen (MidiTarget::forRack (MidiTarget::slotOn, mode, rackRef, s)); });

        if (p != nullptr)
        {
            slot.addSeparator();
            auto& params = p->getParameters();
            for (int k = 0; k < juce::jmin (params.size(), 400); ++k)
            {
                auto* prm = params[k];
                if (! prm->isAutomatable())
                    continue;
                const auto name = prm->getName (64);
                slot.addItem (name, [chosen, mode, rackRef, s, k, name]
                {
                    auto t = MidiTarget::forRack (MidiTarget::pluginParam, mode, rackRef, s, k);
                    t.paramName = name;
                    chosen (t);
                });
            }
        }
        plugins.addSubMenu (juce::String (s + 1) + ". " + (p != nullptr ? p->getName() : juce::String ("(empty)")), slot, p != nullptr || mode != MidiTarget::fixedRack);
    }
    menu.addSubMenu ("Plugins", plugins);
    juce::ignoreUnused (midi);
}

void MidiPanel::addTargetMenu (juce::PopupMenu& m, Session& session, MidiControl& midi, std::function<void (MidiTarget)> chosen)
{
    juce::PopupMenu racks, bank, selected, scenes, global, taps;

    for (auto* r : midi.visibleRacks())
    {
        juce::PopupMenu rm;
        addRackTargets (rm, session, midi, MidiTarget::fixedRack, r->getId(), r, chosen);
        racks.addSubMenu (rackLabel (session, *r), rm);
    }

    for (int pos = 0; pos < midi.getBankSize(); ++pos)
    {
        auto* r = midi.resolveRack (MidiTarget::forRack (MidiTarget::rackGain, MidiTarget::bankRack, pos));
        juce::PopupMenu bm;
        addRackTargets (bm, session, midi, MidiTarget::bankRack, pos, r, chosen);
        bank.addSubMenu ("Strip " + juce::String (pos + 1) + (r != nullptr ? "  (now " + rackLabel (session, *r) + ")" : juce::String()), bm);
    }

    addRackTargets (selected, session, midi, MidiTarget::selectedRack, 0, midi.selectedRack != nullptr ? midi.selectedRack() : nullptr, chosen);

    auto g = [&] (juce::PopupMenu& menu, const juce::String& name, int kind, int index = 0)
    {
        menu.addItem (name, [chosen, kind, index] { chosen (MidiTarget::global (kind, index)); });
    };
    g (global, "MUTE ALL", MidiTarget::masterMute);
    g (global, "Automix on / off", MidiTarget::automixOn);
    g (global, "Clear solos", MidiTarget::clearSolos);
    global.addSeparator();
    g (global, "Next scene", MidiTarget::sceneNext);
    g (global, "Previous scene", MidiTarget::scenePrev);
    for (int i = 0; i < juce::jmax (8, session.getNumScenes()); ++i)
        g (scenes, "Scene " + juce::String (i + 1) + (i < session.getNumScenes() ? ": " + session.getScene (i).name : juce::String()), MidiTarget::sceneRecall, i);
    global.addSubMenu ("Recall scene", scenes);
    global.addSeparator();
    g (global, "Bank <  (previous racks)", MidiTarget::bankPrev);
    g (global, "Bank >  (next racks)", MidiTarget::bankNext);
    g (global, "Shift  (hold: the other layer of the controls)", MidiTarget::shift);
    for (int i = 0; i < session.getEngine().tempo.getNumTempos(); ++i)
        g (taps, i == 0 ? juce::String ("Master tempo") : "Tempo T" + juce::String (i), MidiTarget::tapTempo, i);
    global.addSubMenu ("Tap tempo", taps);

    m.addSubMenu ("Rack", racks);
    m.addSubMenu ("Bank strip (follows the bank)", bank);
    m.addSubMenu ("Selected rack", selected);
    m.addSubMenu ("Global", global);
}

//==============================================================================
// Controllers
class MidiPanel::ControllerRow final : public juce::Component
{
public:
    ControllerRow (MidiPanel& p, const MidiControl::Controller& c) : panel (p), info (c)
    {
        use.setButtonText (c.name + (c.connected ? juce::String() : juce::String ("  (not connected)")));
        use.setToggleState (c.enabled, juce::dontSendNotification);
        use.setTooltip ("Use this controller for MIDI remote control");
        use.onClick = [this] { panel.midi.setControllerEnabled (info.id, info.name, use.getToggleState()); };
        addAndMakeVisible (use);

        profile.setText (midiprofiles::profileName (c.profile), juce::dontSendNotification);
        profile.setFont (theme::font (11.0f));
        profile.setColour (juce::Label::textColourId, c.profile != midiprofiles::generic ? theme::midiBlue.brighter (0.3f) : theme::textDim);
        addAndMakeVisible (profile);

        output.addItem ("LEDs: automatic", 1);
        output.addItem ("LEDs: none", 2);
        outs = juce::MidiOutput::getAvailableDevices();
        int sel = c.outputId == "-" ? 2 : 1;
        for (int i = 0; i < outs.size(); ++i)
        {
            output.addItem ("LEDs: " + outs[i].name, 10 + i);
            if (outs[i].identifier == c.outputId) sel = 10 + i;
        }
        output.setSelectedId (sel, juce::dontSendNotification);
        output.setTooltip ("Where the LED / motor-fader feedback goes (automatic = the output with the controller's name)"
                           + (c.resolvedOutputName.isNotEmpty() ? "\nNow: " + c.resolvedOutputName : juce::String()));
        output.onChange = [this]
        {
            const auto id = output.getSelectedId();
            panel.midi.setFeedbackOutput (info.id, id == 1 ? juce::String() : id == 2 ? juce::String ("-") : outs[id - 10].identifier);
        };
        output.setEnabled (c.enabled);
        addAndMakeVisible (output);

        quick.setButtonText ("QUICK SETUP");
        quick.setTooltip ("Ready-made layout for " + midiprofiles::profileName (c.profile)
                          + ": the 8 strips on the bank of racks (faders = gain, knobs = input trim / sends A-B, "
                            "Mute, SOLO + Mute = solo, Rec Arm = bypass, Bank buttons). Replaces this controller's mappings.");
        quick.onClick = [this]
        {
            juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::QuestionIcon).withTitle ("Quick setup")
                .withMessage ("Map " + info.name + " as an 8-strip mixer on the bank of racks?\nIts current mappings are replaced.")
                .withButton ("Map it", 1).withButton ("Cancel", 0).withAssociatedComponent (this),
                juce::ModalCallbackFunction::create ([safe = juce::Component::SafePointer<MidiPanel> (&panel), id = info.id] (int r)
                {
                    if (r == 1 && safe != nullptr) { safe->midi.quickSetup (id); safe->refreshAll(); }
                }));
        };
        addChildComponent (quick);
        quick.setVisible (c.profile != midiprofiles::generic);
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (info.connected ? (info.enabled ? theme::green : theme::textDim) : theme::red);
        g.fillEllipse (2.0f, (float) getHeight() / 2.0f - 4.0f, 8.0f, 8.0f);
    }

    void resized() override
    {
        auto r = getLocalBounds().withTrimmedLeft (16);
        quick.setBounds (r.removeFromRight (100).reduced (0, 2));
        r.removeFromRight (8);
        output.setBounds (r.removeFromRight (240).reduced (0, 2));
        r.removeFromRight (8);
        profile.setBounds (r.removeFromRight (140));
        use.setBounds (r);
    }

    MidiPanel& panel;
    MidiControl::Controller info;
    juce::ToggleButton use;
    juce::Label profile;
    juce::ComboBox output;
    juce::TextButton quick;
    juce::Array<juce::MidiDeviceInfo> outs;
};

//==============================================================================
// Monitor
class MidiPanel::MonitorList final : public juce::ListBox,
                                     private juce::ListBoxModel
{
public:
    explicit MonitorList (MidiControl& m) : midi (m)
    {
        setModel (this);
        setRowHeight (17);
        setColour (juce::ListBox::backgroundColourId, theme::panel);
    }

    void update()
    {
        events = midi.getRecentEvents();
        std::reverse (events.begin(), events.end());   // newest first
        updateContent();
        repaint();
    }

    int getNumRows() override { return (int) events.size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool) override
    {
        if (! juce::isPositiveAndBelow (row, (int) events.size()))
            return;

        auto& e = events[(size_t) row];
        const auto name = midiprofiles::controlName (midiprofiles::detect (e.device), e.source);
        auto r = juce::Rectangle<int> (6, 0, w - 12, h);
        g.setFont (theme::font (11.0f));
        g.setColour (theme::textDim);
        g.drawText (e.device, r.removeFromLeft (120), juce::Justification::centredLeft, true);
        g.setColour (theme::text);
        g.drawText ((name.isNotEmpty() ? name + "  " : juce::String()) + "(" + e.source.describe() + ")", r.removeFromLeft (190), juce::Justification::centredLeft, true);
        g.drawText (juce::String (e.value), r.removeFromLeft (50), juce::Justification::centredRight, true);
        r.removeFromLeft (14);
        g.setColour (e.action == "no mapping" ? theme::textDim : theme::midiBlue.brighter (0.4f));
        g.drawText (juce::String::fromUTF8 ("\xe2\x86\x92 ") + e.action, r, juce::Justification::centredLeft, true);
    }

private:
    MidiControl& midi;
    std::vector<MidiControl::Event> events;
};

//==============================================================================
// Table cells: each one edits a field of the mapping with this id
class MidiPanel::ChoiceCell final : public juce::ComboBox
{
public:
    ChoiceCell (MidiPanel& p, int column) : panel (p), col (column)
    {
        if (col == colMode)
            for (int i = 0; i < MidiMapping::numModes; ++i) addItem (MidiMapping::modeName (i), i + 1);
        else if (col == colTakeover)
        { addItem ("None (jump)", 1); addItem ("Pick-up", 2); addItem ("Value scaling", 3); }
        else
        { addItem ("Always", 1); addItem ("Without shift", 2); addItem ("With shift", 3); }

        setTooltip (col == colMode ? "How the control acts: Auto (faders/knobs absolute, buttons toggle), Toggle, Momentary (on while held), "
                                     "Trigger, or a Relative encoder type"
                  : col == colTakeover ? "Absolute faders and knobs: Pick-up = the value moves only once the control reaches it "
                                         "(no jumps after a bank change, a scene or the mouse); Value scaling = they meet gradually; None = jump"
                  : "Shift layer: a control can do one thing normally and another while the Shift control is held");
        onChange = [this]
        {
            if (auto* m = panel.midi.findMapping (id))
            {
                auto u = *m;
                const auto v = getSelectedId() - 1;
                if (col == colMode) u.mode = v; else if (col == colTakeover) u.takeover = v; else u.layer = v;
                panel.midi.updateMapping (u);
            }
        };
    }

    void update (int mappingId)
    {
        id = mappingId;
        if (auto* m = panel.midi.findMapping (id))
            setSelectedId ((col == colMode ? m->mode : col == colTakeover ? m->takeover : m->layer) + 1, juce::dontSendNotification);
        if (col == colTakeover)
            if (auto* m = panel.midi.findMapping (id))
                setEnabled (panel.midi.behaviour (m->target) == MidiTarget::continuous);
    }

    MidiPanel& panel;
    const int col;
    int id = 0;
};

class MidiPanel::RangeCell final : public juce::Slider
{
public:
    RangeCell (MidiPanel& p, bool isMax) : juce::Slider (juce::Slider::LinearBar, juce::Slider::TextBoxLeft), panel (p), max (isMax)
    {
        setRange (0.0, 1.0, 0.0);
        setTextBoxIsEditable (false);
        setColour (juce::Slider::trackColourId, theme::midiBlue.withAlpha (0.45f));
        setTooltip (max ? "Value at the top of the control (drag; put it below Min to invert)" : "Value at the bottom of the control (drag)");
        textFromValueFunction = [this] (double v)
        {
            if (auto* m = panel.midi.findMapping (id))
                return panel.midi.valueText (m->target, (float) v);
            return juce::String (v, 2);
        };
        onValueChange = [this]
        {
            if (auto* m = panel.midi.findMapping (id))
            {
                auto u = *m;
                (max ? u.maxValue : u.minValue) = (float) getValue();
                panel.midi.updateMapping (u);
            }
        };
        setDoubleClickReturnValue (true, max ? 1.0 : 0.0);
    }

    void update (int mappingId)
    {
        id = mappingId;
        if (auto* m = panel.midi.findMapping (id))
        {
            setValue (max ? m->maxValue : m->minValue, juce::dontSendNotification);
            setEnabled (panel.midi.behaviour (m->target) == MidiTarget::continuous);
            updateText();
        }
    }

    MidiPanel& panel;
    const bool max;
    int id = 0;
};

class MidiPanel::LedCell final : public juce::ToggleButton
{
public:
    explicit LedCell (MidiPanel& p) : panel (p)
    {
        setTooltip ("Feedback: the controller's LED / motor fader / ring follows the value (right-click a row's ... for LED values)");
        onClick = [this]
        {
            if (auto* m = panel.midi.findMapping (id)) { auto u = *m; u.feedback = getToggleState(); panel.midi.updateMapping (u); }
        };
    }
    void update (int mappingId)
    {
        id = mappingId;
        if (auto* m = panel.midi.findMapping (id)) setToggleState (m->feedback, juce::dontSendNotification);
    }
    MidiPanel& panel;
    int id = 0;
};

class MidiPanel::ActionsCell final : public juce::Component
{
public:
    explicit ActionsCell (MidiPanel& p) : panel (p)
    {
        learn.setTooltip ("Learn: the next control you move on a controller replaces this one");
        learn.onClick = [this] { panel.midi.learnSourceFor (panel.midi.getLearningMapping() == id ? -1 : id); };
        more.setTooltip ("LED values, rack mode, duplicate...");
        more.onClick = [this] { panel.showRowMenu (id, &more); };
        remove.setTooltip ("Delete this mapping");
        remove.onClick = [this] { panel.midi.removeMapping (id); panel.refreshAll(); };
        for (auto* b : { &learn, &more, &remove }) addAndMakeVisible (b);
    }
    void update (int mappingId)
    {
        id = mappingId;
        const bool learning = panel.midi.getLearningMapping() == id;
        learn.setColour (juce::TextButton::buttonColourId, learning ? theme::amber : theme::slot);
        learn.setColour (juce::TextButton::textColourOffId, learning ? juce::Colours::black : theme::text);
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (2, 2);
        remove.setBounds (r.removeFromRight (24));
        r.removeFromRight (3);
        more.setBounds (r.removeFromRight (26));
        r.removeFromRight (3);
        learn.setBounds (r);
    }
    MidiPanel& panel;
    int id = 0;
    juce::TextButton learn { "LEARN" }, more { juce::String::fromUTF8 ("\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2") }, remove { juce::String::fromUTF8 ("\xc3\x97") };
};

//==============================================================================
MidiPanel::MidiPanel (Session& s, MidiControl& m) : session (s), midi (m)
{
    auto title = [this] (juce::Label& l, const juce::String& text)
    {
        l.setText (text, juce::dontSendNotification);
        l.setFont (theme::font (13.0f, true));
        l.setColour (juce::Label::textColourId, theme::text);
        addAndMakeVisible (l);
    };
    title (controllersTitle, "CONTROLLERS");
    title (mappingsTitle, "MAPPINGS");
    title (monitorTitle, "MONITOR");

    mapModeButton.setTooltip ("Map mode (Cmd+M): click a control in the main window, then move a control on the controller");
    mapModeButton.onClick = [this] { if (onToggleMapMode) onToggleMapMode(); };
    addAndMakeVisible (mapModeButton);

    addButton.setTooltip ("New mapping: choose what it drives, then move a control on the controller");
    addButton.onClick = [this] { addMapping(); };
    addAndMakeVisible (addButton);

    bankPrev.onClick = [this] { midi.stepBank (-1); };
    bankNext.onClick = [this] { midi.stepBank (1); };
    bankPrev.setTooltip ("Previous bank of racks");
    bankNext.setTooltip ("Next bank of racks");
    for (int n : { 1, 2, 4, 6, 8, 12, 16, 24, 32 })
        bankSizeBox.addItem (juce::String (n) + " strips", n);
    bankSizeBox.setTooltip ("How many racks a bank holds (the controller's strips)");
    bankSizeBox.onChange = [this] { midi.setBankSize (bankSizeBox.getSelectedId()); };
    for (auto* c : std::initializer_list<juce::Component*> { &bankPrev, &bankNext, &bankSizeBox, &bankLabel })
        addAndMakeVisible (c);
    bankLabel.setFont (theme::font (12.0f));
    bankLabel.setColour (juce::Label::textColourId, theme::text);

    presetsButton.setTooltip ("Save the mappings as a preset, load one, make them the default for new sessions");
    presetsButton.onClick = [this]
    {
        juce::PopupMenu menu;
        const auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("StageRack/MIDI Maps");
        menu.addItem ("Save preset...", [this, dir]
        {
            dir.createDirectory();
            auto chooser = std::make_shared<juce::FileChooser> ("Save MIDI map", dir.getChildFile ("My controller.stagerackmidi"), "*.stagerackmidi");
            chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                                  [this, chooser] (const juce::FileChooser& fc)
                                  { if (fc.getResult() != juce::File()) midi.savePreset (fc.getResult().withFileExtension ("stagerackmidi")); });
        });
        menu.addItem ("Load preset...", [this, dir]
        {
            auto chooser = std::make_shared<juce::FileChooser> ("Load MIDI map", dir, "*.stagerackmidi");
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                  [this, chooser] (const juce::FileChooser& fc)
                                  { if (fc.getResult().existsAsFile()) { midi.loadPreset (fc.getResult()); refreshAll(); } });
        });

        juce::PopupMenu recent;
        for (auto& f : dir.findChildFiles (juce::File::findFiles, false, "*.stagerackmidi"))
            recent.addItem (f.getFileNameWithoutExtension(), [this, f] { midi.loadPreset (f); refreshAll(); });
        menu.addSubMenu ("Load", recent, recent.getNumItems() > 0);
        menu.addSeparator();
        menu.addItem ("Use these mappings for new sessions", ! midi.getMappings().empty(), false, [this]
        {
            midi.defaultMap = midi.toTree();
            midi.defaultMap.removeProperty ("bank", nullptr);
            if (onDefaultMapChanged) onDefaultMapChanged();
        });
        menu.addItem ("No default mappings", midi.defaultMap.isValid(), false, [this]
        {
            midi.defaultMap = {};
            if (onDefaultMapChanged) onDefaultMapChanged();
        });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&presetsButton));
    };
    addAndMakeVisible (presetsButton);

    clearButton.setTooltip ("Delete every mapping of this session");
    clearButton.onClick = [this]
    {
        juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::WarningIcon).withTitle ("Clear all mappings")
                                        .withMessage ("Delete every MIDI mapping of this session?")
                                        .withButton ("Delete", 1).withButton ("Cancel", 0).withAssociatedComponent (this),
                                      juce::ModalCallbackFunction::create ([safe = juce::Component::SafePointer<MidiPanel> (this)] (int r)
                                            { if (r == 1 && safe != nullptr) { safe->midi.clearMappings(); safe->refreshAll(); } }));
    };
    addAndMakeVisible (clearButton);

    auto& h = table.getHeader();
    h.addColumn ("Control", colControl, 170, 80, 400);
    h.addColumn ("Drives", colTarget, 250, 100, 600);
    h.addColumn ("Now", colValue, 80, 50, 160);
    h.addColumn ("Mode", colMode, 112, 70, 200);
    h.addColumn ("Min", colMin, 92, 60, 200);
    h.addColumn ("Max", colMax, 92, 60, 200);
    h.addColumn ("Takeover", colTakeover, 104, 70, 200);
    h.addColumn ("LED", colLed, 38, 30, 60);
    h.addColumn ("Layer", colLayer, 104, 70, 200);
    h.addColumn ("", colActions, 116, 116, 116);
    table.setModel (this);
    table.setRowHeight (24);
    h.setColour (juce::TableHeaderComponent::backgroundColourId, theme::panelLight);
    h.setColour (juce::TableHeaderComponent::textColourId, theme::text);
    h.setColour (juce::TableHeaderComponent::outlineColourId, theme::outline);
    table.setColour (juce::ListBox::backgroundColourId, theme::panel);
    table.setMultipleSelectionEnabled (false);
    addAndMakeVisible (table);

    monitor = std::make_unique<MonitorList> (midi);
    addAndMakeVisible (*monitor);

    helpLabel.setFont (theme::font (11.0f));
    helpLabel.setColour (juce::Label::textColourId, theme::textDim);
    helpLabel.setText ("Map: Cmd+M, click a control (or move a plugin parameter in its window), move a knob / fader / button. "
                       "Faders pick the value up (no jumps). Click \"Drives\" to change the target. Mappings are saved with the session.",
                       juce::dontSendNotification);
    addAndMakeVisible (helpLabel);

    refreshAll();
    startTimerHz (15);
}

MidiPanel::~MidiPanel()
{
    table.setModel (nullptr);
}

void MidiPanel::refreshAll()
{
    rowIds.clear();
    for (auto& m : midi.getMappings())
        rowIds.push_back (m.id);

    table.updateContent();
    table.repaint();
    rebuildControllers();
    updateBank();
}

void MidiPanel::rebuildControllers()
{
    const auto list = midi.getControllers();
    juce::String key;
    for (auto& c : list)
        key << c.id << (int) c.enabled << (int) c.connected << c.outputId << c.resolvedOutputName << ";";

    if (key == controllersKey)
        return;

    controllersKey = key;
    controllerRows.clear();
    for (auto& c : list)
        addAndMakeVisible (controllerRows.add (new ControllerRow (*this, c)));
    resized();
}

void MidiPanel::updateBank()
{
    const auto n = (int) midi.visibleRacks().size();
    const auto first = midi.getBankOffset() + 1, last = juce::jmin (n, midi.getBankOffset() + midi.getBankSize());
    bankLabel.setText ("Bank: racks " + juce::String (first) + "-" + juce::String (last) + " of " + juce::String (n), juce::dontSendNotification);
    bankSizeBox.setSelectedId (midi.getBankSize(), juce::dontSendNotification);
    bankPrev.setEnabled (midi.getBankOffset() > 0);
    bankNext.setEnabled (midi.getBankOffset() + midi.getBankSize() < n);

    const bool on = midi.isMapMode();
    mapModeButton.setColour (juce::TextButton::buttonColourId, on ? theme::midiBlue : theme::slot);
    mapModeButton.setColour (juce::TextButton::textColourOffId, on ? juce::Colours::white : theme::text);
}

void MidiPanel::timerCallback()
{
    // the mapping list changed (learned, deleted, quick setup, session opened): rebuild the rows
    std::vector<int> ids;
    for (auto& m : midi.getMappings())
        ids.push_back (m.id);

    if (ids != rowIds)
        refreshAll();
    else
        table.repaint();   // live values, learn state

    for (int r = 0; r < (int) rowIds.size(); ++r)
        if (auto* a = dynamic_cast<ActionsCell*> (table.getCellComponent (colActions, r)))
            a->update (rowIds[(size_t) r]);

    if (midi.getEventCount() != eventsShown)
    {
        eventsShown = midi.getEventCount();
        monitor->update();
    }

    if (++tick % 15 == 0)
        rebuildControllers();
    updateBank();
}

//==============================================================================
const MidiMapping* MidiPanel::mappingAt (int row) const
{
    return juce::isPositiveAndBelow (row, (int) rowIds.size()) ? midi.findMapping (rowIds[(size_t) row]) : nullptr;
}

int MidiPanel::getNumRows()
{
    return (int) rowIds.size();
}

void MidiPanel::paintRowBackground (juce::Graphics& g, int row, int w, int h, bool selected)
{
    auto* m = mappingAt (row);
    const bool learning = m != nullptr && midi.getLearningMapping() == m->id;
    g.setColour (learning ? theme::amber.withAlpha (0.25f) : selected ? theme::midiBlue.withAlpha (0.25f) : (row % 2 ? theme::panel : theme::panelLight));
    g.fillRect (0, 0, w, h);
}

void MidiPanel::paintCell (juce::Graphics& g, int row, int col, int w, int h, bool)
{
    auto* m = mappingAt (row);
    if (m == nullptr)
        return;

    auto r = juce::Rectangle<int> (6, 0, w - 10, h);
    g.setFont (theme::font (12.0f, col == colControl));

    if (col == colControl)
    {
        g.setColour (theme::text);
        auto text = midi.describeSource (*m);
        if (m->deviceName.isNotEmpty())
        {
            g.drawText (text, r, juce::Justification::centredLeft, true);
            g.setColour (theme::textDim);
            g.setFont (theme::font (10.0f));
            g.drawText (m->deviceName, r, juce::Justification::centredRight, true);
        }
        else
            g.drawText (text + "  (any)", r, juce::Justification::centredLeft, true);
    }
    else if (col == colTarget)
    {
        const bool resolves = ! m->target.isRackTarget() || midi.resolveRack (m->target) != nullptr;
        g.setColour (resolves ? theme::text : theme::textDim);
        g.drawText (midi.describeTarget (m->target), r.withTrimmedRight (12), juce::Justification::centredLeft, true);
        g.setColour (theme::textDim);
        g.drawText (juce::String::fromUTF8 ("\xe2\x96\xbe"), r, juce::Justification::centredRight, false);
    }
    else if (col == colValue)
    {
        float v = 0.0f;
        const bool ok = midi.readTarget (m->target, v);
        // a fader that has not caught the value yet (pick-up): amber, with where the control is
        const bool waiting = ok && m->takeover == MidiMapping::pickup && ! m->engaged && m->lastHw >= 0.0f
                          && midi.behaviour (m->target) == MidiTarget::continuous
                          && (m->mode == MidiMapping::autoMode || m->mode == MidiMapping::absolute);
        g.setColour (waiting ? theme::amber : theme::light ? theme::midiBlue.darker (0.2f) : theme::midiBlue.brighter (0.5f));
        g.drawText (ok ? midi.valueText (m->target, v) : juce::String ("-"), r, juce::Justification::centredLeft, true);
        if (waiting)
        {
            const auto hwPos = m->lastHw, curPos = (v - m->minValue) / juce::jmax (1.0e-6f, std::abs (m->maxValue - m->minValue));
            g.drawText (juce::String::fromUTF8 (hwPos < curPos ? "\xe2\x96\xb2" : "\xe2\x96\xbc"), r, juce::Justification::centredRight, false);
        }
    }
}

juce::Component* MidiPanel::refreshComponentForCell (int row, int col, bool, juce::Component* existing)
{
    auto* m = mappingAt (row);

    if (m == nullptr || col < colMode)
    {
        delete existing;
        return nullptr;
    }

    switch (col)
    {
        case colMode: case colTakeover: case colLayer:
        {
            auto* c = dynamic_cast<ChoiceCell*> (existing);
            if (c == nullptr || c->col != col) { delete existing; c = new ChoiceCell (*this, col); }
            c->update (m->id);
            return c;
        }
        case colMin: case colMax:
        {
            auto* c = dynamic_cast<RangeCell*> (existing);
            if (c == nullptr || c->max != (col == colMax)) { delete existing; c = new RangeCell (*this, col == colMax); }
            c->update (m->id);
            return c;
        }
        case colLed:
        {
            auto* c = dynamic_cast<LedCell*> (existing);
            if (c == nullptr) { delete existing; c = new LedCell (*this); }
            c->update (m->id);
            return c;
        }
        case colActions:
        {
            auto* c = dynamic_cast<ActionsCell*> (existing);
            if (c == nullptr) { delete existing; c = new ActionsCell (*this); }
            c->update (m->id);
            return c;
        }
        default:
            delete existing;
            return nullptr;
    }
}

void MidiPanel::cellClicked (int row, int col, const juce::MouseEvent& e)
{
    auto* m = mappingAt (row);
    if (m == nullptr)
        return;

    if (e.mods.isPopupMenu())
    {
        showRowMenu (m->id, nullptr);
        return;
    }

    if (col == colTarget)
    {
        juce::PopupMenu menu;
        const auto id = m->id;
        addTargetMenu (menu, session, midi, [this, id] (MidiTarget t)
        {
            if (auto* mm = midi.findMapping (id)) { auto u = *mm; u.target = t; midi.updateMapping (u); table.updateContent(); table.repaint(); }
        });
        menu.showMenuAsync (juce::PopupMenu::Options().withMousePosition());
    }
}

void MidiPanel::showRowMenu (int mappingId, juce::Component* near)
{
    auto* m = midi.findMapping (mappingId);
    if (m == nullptr)
        return;

    juce::PopupMenu menu;
    menu.addSectionHeader (midi.describeSource (*m) + juce::String::fromUTF8 (" \xe2\x86\x92 ") + midi.describeTarget (*&m->target));

    if (m->target.isRackTarget())
    {
        juce::PopupMenu mode;
        auto setMode = [this, mappingId] (int newMode)
        {
            if (auto* mm = midi.findMapping (mappingId))
            {
                auto u = *mm;
                auto* r = midi.resolveRack (u.target);
                if (newMode == MidiTarget::fixedRack && r != nullptr) u.target.rack = r->getId();
                else if (newMode == MidiTarget::bankRack)             u.target.rack = r != nullptr && midi.bankPosition (*r) >= 0 ? midi.bankPosition (*r) : 0;
                else                                                  u.target.rack = 0;
                u.target.rackMode = newMode;
                midi.updateMapping (u);
            }
        };
        mode.addItem ("This rack (fixed)", true, m->target.rackMode == MidiTarget::fixedRack, [setMode] { setMode (MidiTarget::fixedRack); });
        mode.addItem ("Bank strip (follows the bank)", true, m->target.rackMode == MidiTarget::bankRack, [setMode] { setMode (MidiTarget::bankRack); });
        mode.addItem ("Selected rack", true, m->target.rackMode == MidiTarget::selectedRack, [setMode] { setMode (MidiTarget::selectedRack); });
        menu.addSubMenu ("Rack", mode);
    }

    menu.addItem ("LED values...", [this, mappingId] { editLed (mappingId); });
    menu.addItem ("Learn the control again", [this, mappingId] { midi.learnSourceFor (mappingId); });
    menu.addItem ("Duplicate", [this, mappingId]
    {
        if (auto* mm = midi.findMapping (mappingId)) { auto copy = *mm; midi.addMapping (copy); refreshAll(); }
    });
    menu.addSeparator();
    menu.addItem ("Delete", [this, mappingId] { midi.removeMapping (mappingId); refreshAll(); });

    auto opts = juce::PopupMenu::Options();
    menu.showMenuAsync (near != nullptr ? opts.withTargetComponent (near) : opts.withMousePosition());
}

void MidiPanel::editLed (int mappingId)
{
    auto* m = midi.findMapping (mappingId);
    if (m == nullptr)
        return;

    auto* w = new juce::AlertWindow ("LED of " + midi.describeSource (*m),
                                     "Values sent to the controller for on and off (most: 127 / 0; pads with colours use other "
                                     "velocities: see the controller's manual). LED address: another note / CC than the input, if the "
                                     "light sits elsewhere (empty = same).",
                                     juce::MessageBoxIconType::NoIcon, this);
    w->addTextEditor ("on", juce::String (m->onValue), "On value");
    w->addTextEditor ("off", juce::String (m->offValue), "Off value");
    w->addTextEditor ("num", m->feedbackNumber >= 0 ? juce::String (m->feedbackNumber) : juce::String(), "LED note / CC");
    w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    w->enterModalState (true, juce::ModalCallbackFunction::create ([this, w, mappingId] (int r)
    {
        if (r == 1)
            if (auto* mm = midi.findMapping (mappingId))
            {
                auto u = *mm;
                u.onValue = juce::jlimit (0, 127, w->getTextEditorContents ("on").getIntValue());
                u.offValue = juce::jlimit (0, 127, w->getTextEditorContents ("off").getIntValue());
                const auto num = w->getTextEditorContents ("num").trim();
                u.feedbackNumber = num.isEmpty() ? -1 : juce::jlimit (0, 127, num.getIntValue());
                midi.updateMapping (u);
            }
    }), true);
}

void MidiPanel::addMapping()
{
    juce::PopupMenu menu;
    menu.addSectionHeader ("What will the control drive?");
    addTargetMenu (menu, session, midi, [this] (MidiTarget t)
    {
        MidiMapping m;
        m.target = t;
        m.source = {};   // learned next
        const auto id = midi.addMapping (m);
        midi.learnSourceFor (id);
        refreshAll();
    });
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&addButton));
}

//==============================================================================
void MidiPanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::bg);
}

void MidiPanel::resized()
{
    auto r = getLocalBounds().reduced (12, 10);

    controllersTitle.setBounds (r.removeFromTop (22));
    for (auto* row : controllerRows)
        row->setBounds (r.removeFromTop (28));
    if (controllerRows.isEmpty())
        r.removeFromTop (28);
    r.removeFromTop (10);

    auto bar = r.removeFromTop (28);
    mappingsTitle.setBounds (bar.removeFromLeft (90));
    mapModeButton.setBounds (bar.removeFromLeft (86));
    bar.removeFromLeft (6);
    addButton.setBounds (bar.removeFromLeft (120));
    clearButton.setBounds (bar.removeFromRight (86));
    bar.removeFromRight (6);
    presetsButton.setBounds (bar.removeFromRight (80));
    bar.removeFromRight (16);
    bankSizeBox.setBounds (bar.removeFromRight (100));
    bar.removeFromRight (6);
    bankNext.setBounds (bar.removeFromRight (28));
    bankPrev.setBounds (bar.removeFromRight (28));
    bar.removeFromRight (6);
    bankLabel.setBounds (bar.removeFromRight (200));
    r.removeFromTop (6);

    helpLabel.setBounds (r.removeFromBottom (32));
    auto mon = r.removeFromBottom (juce::jmin (170, r.getHeight() / 3));
    monitorTitle.setBounds (mon.removeFromTop (22));
    monitor->setBounds (mon);
    r.removeFromBottom (8);
    table.setBounds (r);
}

//==============================================================================
MidiWindow::MidiWindow (Session& s, MidiControl& m)
    : juce::DocumentWindow ("MIDI", theme::bg, juce::DocumentWindow::closeButton)
{
    setUsingNativeTitleBar (true);
    setContentOwned (new MidiPanel (s, m), false);
    setResizable (true, false);
    setResizeLimits (900, 480, 3000, 2000);
    centreWithSize (1240, 760);
}

} // namespace stagerack
