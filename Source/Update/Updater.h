#pragma once

#include <JuceHeader.h>
#include <optional>

namespace stagerack
{

/**
    Updates from the GitHub releases of the project (pre-releases included, it is a beta).

    1. check:    GET api.github.com/repos/<repo>/releases, newest version with a macOS .zip asset
    2. download: the zip, on a background thread, with progress and cancel
    3. verify:   unzip (ditto), bundle id, version, code signature
    4. install:  a small script waits for StageRack to quit, swaps the .app (old one kept until
                 the new one is in place, admin password asked only if the folder is not
                 writable) and reopens it.

    Nothing is downloaded or installed without the user saying so.
*/
namespace update
{
    inline constexpr const char* repository = "Bio00/StageRack";
    inline constexpr const char* bundleId   = "com.bio.stagerack";

    struct Release
    {
        juce::String tag, version, name, notes, pageUrl, assetName, assetUrl;
        juce::int64 assetSize = 0;
        bool prerelease = false;
    };

    /** Version of this build ("0.2.0-beta"); `--fake-version x` overrides it (tests). */
    juce::String currentVersion();

    /** < 0 if a is older than b, 0 if equal, > 0 if newer. "v" prefix ignored;
        1.2.0 > 1.2.0-beta.2 > 1.2.0-beta.1 > 1.2.0-alpha (semver rules). */
    int compareVersions (const juce::String& a, const juce::String& b);

    /** Blocking (call off the message thread). Newest release with a macOS zip, or nothing + error. */
    std::optional<Release> fetchLatest (juce::String& error);

    /** Blocking download with progress (0..1) and cancel. Returns an error, or {} on success. */
    juce::String download (const Release&, const juce::File& target,
                           std::function<void (double)> progress, std::function<bool()> shouldCancel);

    /** Unzips, finds StageRack.app and checks bundle id, version and signature.
        Returns the new .app, or an invalid File + error. */
    juce::File unpackAndVerify (const juce::File& zip, const Release&, juce::String& error);

    /** The .app being run, and whether it can be replaced in place. */
    juce::File runningBundle();
    bool needsAdminToReplace (const juce::File& bundle);
    bool isTranslocated (const juce::File& bundle);

    /** Starts the detached installer script (waits for this process to exit, swaps, reopens).
        Call right before quitting. Returns an error, or {} on success. */
    juce::String launchInstaller (const juce::File& newApp, const juce::File& target, bool reopen = true);

    /** Installer to start when the app really quits (after the save prompt); cleared if the
        user cancels the quit. */
    void setPendingInstall (std::function<void()>);
    void clearPendingInstall();
    void runPendingInstall();

    //==============================================================================
    /** UI flow: check (silently at startup, or with messages when asked), ask, download with a
        progress window, verify, then `quitForInstall` (which must save/ask and then quit). */
    class Controller final : private juce::Timer
    {
    public:
        Controller (juce::PropertiesFile& settings, std::function<void (std::function<void()> launchAndQuit)> quitForInstall);
        ~Controller() override;

        void checkAtStartup();               // honours the "checkUpdates" setting and "skipped" version
        void checkNow();                     // the button: always reports the result

        static constexpr const char* settingCheck = "checkUpdates";
        static constexpr const char* settingSkip  = "skippedUpdate";

    private:
        class Worker;
        void startCheck (bool userAsked);
        void onChecked (std::optional<Release>, juce::String error, bool userAsked);
        void offer (const Release&);
        void startDownload (const Release&);
        void timerCallback() override;

        juce::PropertiesFile& settings;
        std::function<void (std::function<void()>)> quitForInstall;
        std::unique_ptr<Worker> worker;
        std::unique_ptr<juce::AlertWindow> progressWindow;
        double progress = 0.0;
        std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);
    };
}

} // namespace stagerack
