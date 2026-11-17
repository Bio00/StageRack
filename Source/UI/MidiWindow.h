#pragma once

#include "../Engine/Session.h"
#include "../Engine/MidiControl.h"

namespace stagerack
{

/**
    MIDI window: the controllers (use / LED output / quick setup for known ones), every mapping
    with its range, mode, takeover, LED and shift layer, the controller bank, presets, and a
    monitor of what arrives and what it did.
*/
class MidiPanel final : public juce::Component,
                        private juce::Timer,
                        private juce::TableListBoxModel
{
public:
    MidiPanel (Session&, MidiControl&);
    ~MidiPanel() override;

    std::function<void()> onToggleMapMode;
    /** Default map changed (saved in the app settings). */
    std::function<void()> onDefaultMapChanged;

    void paint (juce::Graphics&) override;
    void resized() override;
    void refreshAll();

    /** The menu of every target (racks, bank strips, selected rack, global). */
    static void addTargetMenu (juce::PopupMenu&, Session&, MidiControl&, std::function<void (MidiTarget)> chosen);

private:
    class ControllerRow;
    class MonitorList;
    class ModeCell;
    class RangeCell;
    class ChoiceCell;
    class LedCell;
    class ActionsCell;

    enum Columns { colControl = 1, colTarget, colValue, colMode, colMin, colMax, colTakeover, colLed, colLayer, colActions };

    void timerCallback() override;
    int getNumRows() override;
    void paintRowBackground (juce::Graphics&, int row, int w, int h, bool selected) override;
    void paintCell (juce::Graphics&, int row, int col, int w, int h, bool selected) override;
    juce::Component* refreshComponentForCell (int row, int col, bool selected, juce::Component* existing) override;
    void cellClicked (int row, int col, const juce::MouseEvent&) override;

    const MidiMapping* mappingAt (int row) const;
    void showRowMenu (int mappingId, juce::Component* near);
    void editLed (int mappingId);
    void addMapping();
    void rebuildControllers();
    void updateBank();

    Session& session;
    MidiControl& midi;

    juce::Label controllersTitle, mappingsTitle, monitorTitle, bankLabel, helpLabel;
    juce::OwnedArray<ControllerRow> controllerRows;
    juce::String controllersKey;

    juce::TextButton mapModeButton { "MIDI MAP" }, addButton { "+ ADD MAPPING" };
    juce::TextButton bankPrev { juce::String::charToString (0x25C0) }, bankNext { juce::String::charToString (0x25B6) };
    juce::ComboBox bankSizeBox;
    juce::TextButton presetsButton { "PRESETS" }, clearButton { "CLEAR ALL" };

    juce::TableListBox table;
    std::vector<int> rowIds;            // mapping id per row
    std::unique_ptr<MonitorList> monitor;
    juce::uint32 eventsShown = 0;
    int tick = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiPanel)
};

class MidiWindow final : public juce::DocumentWindow
{
public:
    MidiWindow (Session&, MidiControl&);
    MidiPanel& getPanel()               { return *static_cast<MidiPanel*> (getContentComponent()); }
    void closeButtonPressed() override  { setVisible (false); }
};

} // namespace stagerack
