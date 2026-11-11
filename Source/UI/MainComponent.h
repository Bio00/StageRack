#pragma once

#include "../Engine/Autosave.h"
#include "../Engine/Session.h"
#include "Theme.h"
#include "EditController.h"

namespace stagerack
{

class TopBar;
class SceneListComponent;
class RackComponent;
class PluginManagerWindow;
class AutomixerWindow;
class SettingsWindow;
namespace update { class Controller; }

class MainComponent final : public juce::Component,
                            public juce::DragAndDropContainer,
                            private juce::ChangeListener,
                            private juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

    Session& getSession() noexcept { return *session; }

    void newSession();
    void openSession();
    void saveSession (bool saveAs);

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
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
    std::unique_ptr<EditController> edit;

    std::unique_ptr<TopBar> topBar;
    std::unique_ptr<SceneListComponent> sceneList;
    juce::Viewport rackViewport;
    juce::Component rackContainer;
    juce::OwnedArray<RackComponent> rackComponents;
    juce::TextButton addRackButton { "+  ADD RACK" };
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
    std::unique_ptr<juce::FileChooser> chooser;

    // updates from GitHub: checked at startup, "Check for Updates..." in the app menu and in Settings
    std::unique_ptr<update::Controller> updater;
    struct AppMenu final : juce::MenuBarModel
    {
        juce::StringArray getMenuBarNames() override                 { return {}; }
        juce::PopupMenu getMenuForIndex (int, const juce::String&) override { return {}; }
        void menuItemSelected (int, int) override {}
    } appMenu;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace stagerack
