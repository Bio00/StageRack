#include "Updater.h"
#include <unistd.h>

namespace stagerack::update
{

//==============================================================================
juce::String currentVersion()
{
    const auto args = juce::JUCEApplicationBase::getCommandLineParameterArray();

    if (const auto i = args.indexOf ("--fake-version"); i >= 0 && i + 1 < args.size())
        return args[i + 1];

    return STAGERACK_VERSION;
}

static juce::String stripV (juce::String v)
{
    v = v.trim();
    return v.startsWithIgnoreCase ("v") ? v.substring (1) : v;
}

int compareVersions (const juce::String& aIn, const juce::String& bIn)
{
    const auto a = stripV (aIn).upToFirstOccurrenceOf ("+", false, false);   // build metadata ignored
    const auto b = stripV (bIn).upToFirstOccurrenceOf ("+", false, false);

    const auto coreA = juce::StringArray::fromTokens (a.upToFirstOccurrenceOf ("-", false, false), ".", "");
    const auto coreB = juce::StringArray::fromTokens (b.upToFirstOccurrenceOf ("-", false, false), ".", "");

    for (int i = 0; i < juce::jmax (coreA.size(), coreB.size()); ++i)
        if (const auto x = coreA[i].getIntValue(), y = coreB[i].getIntValue(); x != y)
            return x < y ? -1 : 1;

    const auto preA = a.fromFirstOccurrenceOf ("-", false, false);
    const auto preB = b.fromFirstOccurrenceOf ("-", false, false);

    if (preA.isEmpty() || preB.isEmpty())
        return preA.isEmpty() == preB.isEmpty() ? 0 : (preA.isEmpty() ? 1 : -1);   // release > pre-release

    const auto idsA = juce::StringArray::fromTokens (preA, ".", "");
    const auto idsB = juce::StringArray::fromTokens (preB, ".", "");

    for (int i = 0; i < juce::jmin (idsA.size(), idsB.size()); ++i)
    {
        const auto& x = idsA[i];
        const auto& y = idsB[i];
        const bool nx = x.containsOnly ("0123456789"), ny = y.containsOnly ("0123456789");

        if (nx && ny)
        {
            if (x.getLargeIntValue() != y.getLargeIntValue())
                return x.getLargeIntValue() < y.getLargeIntValue() ? -1 : 1;
        }
        else if (nx != ny)
        {
            return nx ? -1 : 1;   // numeric identifiers sort before words
        }
        else if (const auto c = x.compare (y); c != 0)
        {
            return c < 0 ? -1 : 1;
        }
    }

    return idsA.size() == idsB.size() ? 0 : (idsA.size() < idsB.size() ? -1 : 1);
}

//==============================================================================
static const char* userAgent()
{
    return "User-Agent: StageRack-Updater\r\n";
}

std::optional<Release> fetchLatest (juce::String& error)
{
    const juce::URL url ("https://api.github.com/repos/" + juce::String (repository) + "/releases?per_page=30");
    int status = 0;

    auto stream = url.createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                             .withExtraHeaders (juce::String (userAgent()) + "Accept: application/vnd.github+json\r\n")
                                             .withConnectionTimeoutMs (10000)
                                             .withStatusCode (&status));

    if (stream == nullptr)
    {
        error = "No connection to GitHub.";
        return {};
    }

    const auto body = stream->readEntireStreamAsString();

    if (status != 200)
    {
        error = "GitHub answered " + juce::String (status) + ".";
        return {};
    }

    const auto json = juce::JSON::parse (body);

    if (! json.isArray())
    {
        error = "Unexpected answer from GitHub.";
        return {};
    }

    std::optional<Release> best;

    for (const auto& r : *json.getArray())
    {
        if ((bool) r["draft"])
            continue;

        Release rel;
        rel.tag = r["tag_name"].toString();
        rel.version = stripV (rel.tag);
        rel.name = r["name"].toString();
        rel.notes = r["body"].toString();
        rel.pageUrl = r["html_url"].toString();
        rel.prerelease = (bool) r["prerelease"];

        // the macOS app zip (prefer the universal one)
        if (const auto* assets = r["assets"].getArray())
        {
            for (const auto& a : *assets)
            {
                const auto name = a["name"].toString();

                if (! name.endsWithIgnoreCase (".zip") || ! name.containsIgnoreCase ("macos"))
                    continue;

                if (rel.assetUrl.isEmpty() || name.containsIgnoreCase ("universal"))
                {
                    rel.assetName = name;
                    rel.assetUrl = a["browser_download_url"].toString();
                    rel.assetSize = (juce::int64) a["size"];
                }
            }
        }

        if (rel.version.isEmpty() || rel.assetUrl.isEmpty())
            continue;

        if (! best.has_value() || compareVersions (rel.version, best->version) > 0)
            best = rel;
    }

    if (! best.has_value())
        error = "No release with a macOS download yet.";

    return best;
}

//==============================================================================
juce::String download (const Release& rel, const juce::File& target,
                       std::function<void (double)> progress, std::function<bool()> shouldCancel)
{
    int status = 0;
    auto stream = juce::URL (rel.assetUrl).createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                                                 .withExtraHeaders (juce::String (userAgent()) + "Accept: application/octet-stream\r\n")
                                                                 .withConnectionTimeoutMs (15000)
                                                                 .withNumRedirectsToFollow (8)
                                                                 .withStatusCode (&status));

    if (stream == nullptr || status != 200)
        return "Download failed" + (status != 0 ? " (HTTP " + juce::String (status) + ")." : juce::String ("."));

    target.deleteFile();
    target.getParentDirectory().createDirectory();
    juce::FileOutputStream out (target);

    if (! out.openedOk())
        return "Cannot write " + target.getFullPathName();

    const auto total = rel.assetSize > 0 ? rel.assetSize : stream->getTotalLength();
    juce::HeapBlock<char> buffer (1 << 16);
    juce::int64 done = 0;

    for (;;)
    {
        if (shouldCancel && shouldCancel())
            return "Cancelled.";

        const auto n = stream->read (buffer, 1 << 16);

        if (n <= 0)
            break;

        if (! out.write (buffer, (size_t) n))
            return "Disk full?";

        done += n;

        if (progress && total > 0)
            progress (juce::jlimit (0.0, 1.0, (double) done / (double) total));
    }

    out.flush();

    if (rel.assetSize > 0 && done != rel.assetSize)
        return "Download incomplete (" + juce::String (done) + " of " + juce::String (rel.assetSize) + " bytes).";

    return {};
}

//==============================================================================
static int run (const juce::StringArray& args, juce::String* output = nullptr, int timeoutMs = 120000)
{
    juce::ChildProcess p;

    if (! p.start (args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
        return -1;

    const auto text = p.readAllProcessOutput();

    if (output != nullptr)
        *output = text;

    if (! p.waitForProcessToFinish (timeoutMs))
    {
        p.kill();
        return -1;
    }

    return (int) p.getExitCode();
}

static juce::String plistValue (const juce::File& app, const juce::String& key)
{
    juce::String out;
    run ({ "/usr/libexec/PlistBuddy", "-c", "Print " + key, app.getChildFile ("Contents/Info.plist").getFullPathName() }, &out, 10000);
    return out.trim();
}

juce::File unpackAndVerify (const juce::File& zip, const Release& rel, juce::String& error)
{
    const auto dir = zip.getSiblingFile ("unpacked");
    dir.deleteRecursively();
    dir.createDirectory();

    juce::String log;

    if (run ({ "/usr/bin/ditto", "-x", "-k", zip.getFullPathName(), dir.getFullPathName() }, &log) != 0)
    {
        error = "Could not unzip the update: " + log.trim();
        return {};
    }

    auto app = dir.getChildFile ("StageRack.app");

    if (! app.isDirectory())
        for (const auto& f : dir.findChildFiles (juce::File::findDirectories, false, "*.app"))
            app = f;

    if (! app.isDirectory())
    {
        error = "The download does not contain StageRack.app.";
        return {};
    }

    if (const auto id = plistValue (app, "CFBundleIdentifier"); id != bundleId)
    {
        error = "The download is not StageRack (bundle id \"" + id + "\").";
        return {};
    }

    const auto core = rel.version.upToFirstOccurrenceOf ("-", false, false);

    if (const auto v = plistValue (app, "CFBundleShortVersionString"); v != core)
    {
        error = "Version mismatch: the release is " + rel.version + " but the app inside says " + v + ".";
        return {};
    }

    if (run ({ "/usr/bin/codesign", "--verify", "--deep", "--strict", app.getFullPathName() }, &log) != 0)
    {
        error = "The downloaded app failed the signature check (damaged download?): " + log.trim();
        return {};
    }

    return app;
}

//==============================================================================
juce::File runningBundle()
{
    return juce::File::getSpecialLocation (juce::File::currentApplicationFile);
}

bool isTranslocated (const juce::File& bundle)
{
    return bundle.getFullPathName().contains ("/AppTranslocation/");
}

bool needsAdminToReplace (const juce::File& bundle)
{
    return ! bundle.getParentDirectory().hasWriteAccess() || ! bundle.hasWriteAccess();
}

static juce::File updateDirectory()
{
    return juce::File::getSpecialLocation (juce::File::userHomeDirectory)
               .getChildFile ("Library/Caches/StageRack/Update");
}

juce::String launchInstaller (const juce::File& newApp, const juce::File& target, bool reopen)
{
    const auto dir = updateDirectory();
    dir.createDirectory();

    // swap.sh <new.app> <target.app>: the old app is kept until the new one is fully copied
    const auto swap = dir.getChildFile ("swap.sh");
    swap.replaceWithText (R"SH(#!/bin/bash
NEW="$1"; TARGET="$2"; OLD="$TARGET.old-update"
rm -rf "$OLD"
if [ -e "$TARGET" ]; then mv "$TARGET" "$OLD" || exit 1; fi
if /usr/bin/ditto "$NEW" "$TARGET"; then
    rm -rf "$OLD"
else
    rm -rf "$TARGET"; mv "$OLD" "$TARGET"; exit 1
fi
/usr/bin/xattr -dr com.apple.quarantine "$TARGET" 2>/dev/null
exit 0
)SH", false, false, "\n");   // JUCE writes CRLF by default: bash would choke on the \r

    // install.sh <pid> <new.app> <target.app> <admin 0/1> <reopen 0/1> <log>
    const auto install = dir.getChildFile ("install.sh");
    install.replaceWithText (R"SH(#!/bin/bash
PID="$1"; NEW="$2"; TARGET="$3"; ADMIN="$4"; REOPEN="$5"; LOG="$6"
DIR="$(dirname "$0")"
exec >>"$LOG" 2>&1
echo "$(date) update: waiting for StageRack ($PID) to quit"
for i in $(seq 1 1200); do kill -0 "$PID" 2>/dev/null || break; sleep 0.25; done
if kill -0 "$PID" 2>/dev/null; then echo "StageRack did not quit: update cancelled"; echo cancelled > "$DIR/result"; exit 1; fi
if [ "$ADMIN" = "1" ]; then
    /usr/bin/osascript -e 'on run argv' -e 'do shell script "/bin/bash " & quoted form of item 1 of argv & " " & quoted form of item 2 of argv & " " & quoted form of item 3 of argv with prompt "StageRack needs your password to install the update." with administrator privileges' -e 'end run' "$DIR/swap.sh" "$NEW" "$TARGET"
else
    /bin/bash "$DIR/swap.sh" "$NEW" "$TARGET"
fi
if [ $? -eq 0 ]; then echo "$(date) update: installed"; echo ok > "$DIR/result"; else echo "$(date) update: FAILED, old version kept"; echo failed > "$DIR/result"; fi
if [ "$REOPEN" = "1" ]; then /usr/bin/open "$TARGET"; fi
)SH", false, false, "\n");

    dir.getChildFile ("result").deleteFile();

    juce::ChildProcess launcher;
    const juce::StringArray args { "/bin/bash", "-c", "nohup /bin/bash \"$0\" \"$@\" >/dev/null 2>&1 &",
                                   install.getFullPathName(),
                                   juce::String ((int) ::getpid()),
                                   newApp.getFullPathName(),
                                   target.getFullPathName(),
                                   needsAdminToReplace (target) ? "1" : "0",
                                   reopen ? "1" : "0",
                                   dir.getChildFile ("install.log").getFullPathName() };

    if (! launcher.start (args, 0))
        return "Could not start the installer.";

    launcher.waitForProcessToFinish (5000);
    return {};
}

static std::function<void()>& pendingInstall()
{
    static std::function<void()> f;
    return f;
}

void setPendingInstall (std::function<void()> f) { pendingInstall() = std::move (f); }
void clearPendingInstall()                        { pendingInstall() = nullptr; }

void runPendingInstall()
{
    if (auto f = std::exchange (pendingInstall(), nullptr))
        f();
}

//==============================================================================
/** Runs one job off the message thread. */
class Controller::Worker final : public juce::Thread
{
public:
    explicit Worker (std::function<void (Worker&)> j) : juce::Thread ("StageRack update"), job (std::move (j)) { startThread(); }
    ~Worker() override { cancel = true; stopThread (20000); }

    std::atomic<bool> cancel { false };
    std::atomic<double> progress { 0.0 };

private:
    void run() override { job (*this); }
    std::function<void (Worker&)> job;
};

Controller::Controller (juce::PropertiesFile& s, std::function<void (std::function<void()>)> q)
    : settings (s), quitForInstall (std::move (q))
{
}

Controller::~Controller()
{
    *alive = false;
    stopTimer();
    progressWindow = nullptr;
    worker = nullptr;
}

void Controller::checkAtStartup()
{
    // tell the user if the last update could not be installed
    const auto result = updateDirectory().getChildFile ("result");

    if (result.loadFileAsString().trim() == "failed")
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "StageRack update",
            "The last update could not be installed: the previous version was kept.\n\nDetails: "
              + updateDirectory().getChildFile ("install.log").getFullPathName());

    result.deleteFile();
    updateDirectory().getChildFile ("unpacked").deleteRecursively();

    if (settings.getBoolValue (settingCheck, true))
        startTimer (3000);   // let the audio device and the session come up first
}

void Controller::checkNow()
{
    startCheck (true);
}

void Controller::timerCallback()
{
    if (progressWindow != nullptr && worker != nullptr)
    {
        progress = worker->progress.load();
        return;
    }

    stopTimer();
    startCheck (false);
}

void Controller::startCheck (bool userAsked)
{
    if (worker != nullptr && worker->isThreadRunning())
        return;   // already checking or downloading

    worker = std::make_unique<Worker> ([token = alive, this, userAsked] (Worker&)
    {
        juce::String error;
        auto rel = fetchLatest (error);

        juce::MessageManager::callAsync ([token, this, rel, error, userAsked]
        {
            if (*token)
                onChecked (rel, error, userAsked);
        });
    });
}

void Controller::onChecked (std::optional<Release> rel, juce::String error, bool userAsked)
{
    if (! rel.has_value())
    {
        if (userAsked)
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Check for updates",
                                                    "Could not check for updates.\n\n" + error);
        return;
    }

    const auto current = currentVersion();

    if (compareVersions (rel->version, current) <= 0)
    {
        if (userAsked)
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Check for updates",
                                                    "StageRack " + current + " is the latest version.");
        return;
    }

    if (! userAsked && settings.getValue (settingSkip) == rel->version)
        return;

    offer (*rel);
}

void Controller::offer (const Release& rel)
{
    auto notes = rel.notes.replace ("\r", "").replace ("**", "").replace ("### ", "").trim();

    // markdown-ish to plain text, short enough for a dialog
    notes = notes.replace ("`", "").replace ("\n\n", "\n");

    if (notes.length() > 700)
        notes = notes.substring (0, 700).upToLastOccurrenceOf ("\n", false, false) + "\n...";

    const auto mb = juce::String ((double) rel.assetSize / (1024.0 * 1024.0), 1);

    auto* w = new juce::AlertWindow ("Update available",
                                     "StageRack " + rel.version + " is available (you have " + currentVersion() + ").\n"
                                       "Download: " + mb + " MB. Installing quits StageRack (you can save the session first) "
                                       "and reopens it: the audio stops for a few seconds, so don't do it during a show.",
                                     juce::MessageBoxIconType::InfoIcon);

    if (notes.isNotEmpty())
        w->addTextBlock ("What's new:\n" + notes);

    w->addButton ("Download and install", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Later", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    w->addButton ("Skip this version", 2);

    w->enterModalState (true, juce::ModalCallbackFunction::create ([token = alive, this, rel] (int result)
    {
        if (! *token)
            return;

        if (result == 2)
            settings.setValue (settingSkip, rel.version);
        else if (result == 1)
            startDownload (rel);
    }), true);
}

void Controller::startDownload (const Release& rel)
{
    worker = nullptr;   // the check that found this release has finished: join it

    progress = 0.0;
    progressWindow = std::make_unique<juce::AlertWindow> ("Downloading StageRack " + rel.version, rel.assetName,
                                                          juce::MessageBoxIconType::NoIcon);
    progressWindow->addProgressBarComponent (progress);
    progressWindow->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    progressWindow->enterModalState (true, juce::ModalCallbackFunction::create ([token = alive, this] (int)
    {
        if (*token && worker != nullptr)
            worker->cancel = true;
    }), false);

    startTimerHz (15);

    worker = std::make_unique<Worker> ([token = alive, this, rel] (Worker& w)
    {
        const auto zip = updateDirectory().getChildFile (rel.assetName);
        auto error = download (rel, zip, [&w] (double p) { w.progress = p; }, [&w] { return w.cancel.load() || w.threadShouldExit(); });
        juce::File app;

        if (error.isEmpty())
            app = unpackAndVerify (zip, rel, error);

        zip.deleteFile();
        const bool cancelled = w.cancel.load();

        juce::MessageManager::callAsync ([token, this, rel, app, error, cancelled]
        {
            if (! *token)
                return;

            stopTimer();

            if (progressWindow != nullptr)
            {
                progressWindow->exitModalState (-1);
                progressWindow = nullptr;
            }

            if (cancelled)
                return;

            if (error.isNotEmpty())
            {
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Update", error);
                return;
            }

            const auto target = runningBundle();

            if (isTranslocated (target) || ! target.getFileName().endsWithIgnoreCase (".app"))
            {
                // run from a quarantined download folder (macOS gives it a read-only copy):
                // the new version is ready, the user moves it where they want
                const auto dest = juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                                      .getChildFile ("Downloads/StageRack " + rel.version + ".app");
                dest.deleteRecursively();
                app.moveFileTo (dest);
                dest.revealToUser();
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Update",
                    "StageRack " + rel.version + " is in your Downloads folder.\n\nThis copy of StageRack runs from a "
                    "read-only location: move the new one to Applications and open it from there "
                    "(future updates will then install by themselves).");
                return;
            }

            auto* ready = new juce::AlertWindow ("Ready to install",
                                             "StageRack " + rel.version + " is downloaded and verified.\n\n"
                                               "Install now? StageRack quits (asking to save the session), is replaced in\n"
                                               + target.getFullPathName() + "\nand reopens."
                                               + (needsAdminToReplace (target) ? "\n\nmacOS will ask for an administrator password." : ""),
                                             juce::MessageBoxIconType::QuestionIcon);
            ready->addButton ("Install and restart", 1, juce::KeyPress (juce::KeyPress::returnKey));
            ready->addButton ("Later", 0, juce::KeyPress (juce::KeyPress::escapeKey));
            ready->enterModalState (true, juce::ModalCallbackFunction::create ([token, this, app, target] (int result)
            {
                if (! *token || result != 1)
                    return;

                quitForInstall ([app, target]
                {
                    if (const auto err = launchInstaller (app, target); err.isNotEmpty())
                        DBG (err);
                });
            }), true);
        });
    });
}

} // namespace stagerack::update
