#pragma once

#include "../Engine/Autosave.h"
#include "../Engine/Session.h"
#include "../Engine/DeviceGuard.h"
#include "Theme.h"
#include "EditController.h"
#include "AppMenu.h"

namespace stagerack
{

class TopBar;
class SceneListComponent;
class RackComponent;
class PluginManagerWindow;
class AutomixerWindow;
class TempoWindow;
class EditGroupsWindow;
class SettingsWindow;
class TapKeys;
namespace update { class Controller; }

class MainComponent final : public juce::Component,
                            public juce::DragAndDropContainer,
                            public juce::ApplicationCommandTarget,
                            private juce::ChangeListener,
                            private juce::Timer
{
public:
    /** Release notes: since the version seen before (after an update) or the whole history. */
    static void showWhatsNew (bool sinceUpdate, const juce::String& seenBefore = {});
    MainComponent();
    ~MainComponent() override;

    /** Quit: closes every window, stops the audio, destroys the plugins and writes all
        settings, once. After it only the components are left (nothing to save): the app
        then exits without tearing them down, so a plugin that damaged the heap cannot turn
        a clean quit into a crash report. */
    void prepareToQuit();

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    bool keyStateChanged (bool isKeyDown) override;

    Session& getSession() noexcept { return *session; }

    void newSession();
    void openSession();
    void saveSession (bool saveAs);

    // menu bar commands
    juce::ApplicationCommandTarget* getNextCommandTarget() override { return nullptr; }
    void getAllCommands (juce::Array<juce::CommandID>&) override;
    void getCommandInfo (juce::CommandID, juce::ApplicationCommandInfo&) override;
    bool perform (const InvocationInfo&) override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    bool quitPrepared = false;
    void syncRacks();
    void layoutRacks();
    void setVerticalLayout (bool);
    void setLightTheme (bool);
    void lookAndFeelChanged() override;

    bool verticalLayout = false;
    int latencyCheckCounter = 0;

    theme::LookAndFeel lnf;
    PluginManager pluginManager;
    AudioEngine engine;
    std::unique_ptr<Session> session;
    std::unique_ptr<Autosave> autosave;
    void openLastSession();
    void offerRecovery (Autosave::Recovered);
    void startAudioDevice();
    void loadStartupSession();
    bool audioReady = false;          // CoreAudio answered and the device manager is initialised
    std::unique_ptr<DeviceGuard> deviceGuard;   // keeps the chosen interface, never the Mac's speakers by itself
    juce::Label coreAudioWaitLabel;
    int waitingGeneration = -1;       // document generation of the empty session shown while waiting
    std::unique_ptr<EditController> edit;

    std::unique_ptr<TopBar> topBar;
    std::unique_ptr<SceneListComponent> sceneList;
    juce::Viewport rackViewport;
    void mouseDown (const juce::MouseEvent&) override;   // click on empty space = deselect
    /** Holds the racks; paints the folders behind them (a band in the folder's colour over its
        racks, which sit a step lower, and a tint behind the whole block). */
    struct RackContainer final : juce::Component
    {
        struct Folder { juce::Rectangle<int> block, band; juce::Colour colour; juce::String name; };
        std::vector<Folder> folders;   // outer folders first
        bool vertical = false;
        void paint (juce::Graphics&) override;
    };
    RackContainer rackContainer;
    static constexpr int folderStep = 16;   // how much shorter a folder's racks are (per nesting level)
    juce::OwnedArray<RackComponent> rackComponents;
    /** "+ ADD RACK": click adds one, right-click offers several at once (consecutive inputs). */
    struct AddRackButton final : juce::TextButton
    {
        std::function<void()> onRightClick;
        void mouseDown (const juce::MouseEvent& e) override
        {
            if (e.mods.isPopupMenu()) { if (onRightClick) onRightClick(); return; }
            juce::TextButton::mouseDown (e);
        }
        void mouseUp (const juce::MouseEvent& e) override
        {
            if (! e.mods.isPopupMenu()) juce::TextButton::mouseUp (e);
        }
    } addRackButton;
    void showAddRacksMenu();
    void commonConstructorTail();
    void openSettings();
    void openPluginManager();
    void openAutomixer();
    void openTempo();
    void openEditGroups();
    void toggleLightTheme();
    /** Slim strip that collapses/expands the scene column. */
    struct ScenesToggle final : juce::Component
    {
        bool collapsed = false;
        std::function<void()> onClick;

        void paint (juce::Graphics& g) override
        {
            auto b = getLocalBounds().toFloat();
            g.setColour (isMouseOver() ? theme::panelLight : theme::panel);
            g.fillRoundedRectangle (b, 3.0f);

            const auto c = b.getCentre();
            juce::Path arrow;
            if (collapsed) arrow.addTriangle (c.x - 3.0f, c.y - 6.0f, c.x - 3.0f, c.y + 6.0f, c.x + 4.0f, c.y);
            else           arrow.addTriangle (c.x + 3.0f, c.y - 6.0f, c.x + 3.0f, c.y + 6.0f, c.x - 4.0f, c.y);
            g.setColour (isMouseOver() ? theme::text : theme::textDim);
            g.fillPath (arrow);
        }

        void mouseEnter (const juce::MouseEvent&) override { repaint(); }
        void mouseExit (const juce::MouseEvent&) override  { repaint(); }
        void mouseUp (const juce::MouseEvent& e) override  { if (! e.mouseWasDraggedSinceMouseDown() && onClick) onClick(); }
    } scenesToggle;
    bool scenesCollapsed = false;
    void setScenesCollapsed (bool);
    juce::TooltipWindow tooltips { this, 600 };

    std::unique_ptr<PluginManagerWindow> pluginWindow;
    std::unique_ptr<SettingsWindow> settingsWindow;
    std::unique_ptr<AutomixerWindow> automixerWindow;
    std::unique_ptr<TempoWindow> tempoWindow;
    std::unique_ptr<EditGroupsWindow> editGroupsWindow;
    std::unique_ptr<TapKeys> tapKeys;   // tap tempo from the computer keyboard
    std::unique_ptr<juce::FileChooser> chooser;

    // updates from GitHub: checked at startup, "Check for Updates..." in the app menu and in Settings
    std::unique_ptr<update::Controller> updater;
    juce::ApplicationCommandManager commands;
    struct AppMenu final : juce::MenuBarModel
    {
        explicit AppMenu (MainComponent& o) : owner (o) {}
        juce::StringArray getMenuBarNames() override;
        juce::PopupMenu getMenuForIndex (int, const juce::String&) override;
        void menuItemSelected (int, int) override {}
        MainComponent& owner;
    } appMenu { *this };
    juce::PopupMenu commandItems (std::initializer_list<juce::CommandID>);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace stagerack
