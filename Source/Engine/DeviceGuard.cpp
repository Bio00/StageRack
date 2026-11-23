#include "DeviceGuard.h"
#include "CoreAudioProbe.h"

namespace stagerack
{

static juce::String inName (const juce::XmlElement& x)
{
    return x.getStringAttribute ("audioDeviceName", x.getStringAttribute ("audioInputDeviceName"));
}

static juce::String outName (const juce::XmlElement& x)
{
    return x.getStringAttribute ("audioDeviceName", x.getStringAttribute ("audioOutputDeviceName"));
}

DeviceGuard::DeviceGuard (AudioEngine& e, std::unique_ptr<juce::XmlElement> saved)
    : engine (e), dm (e.getDeviceManager()), preferred (std::move (saved))
{
    if (preferred != nullptr && inName (*preferred).isEmpty() && outName (*preferred).isEmpty())
        preferred.reset();   // an empty state ("no device") is not a choice to keep
}

DeviceGuard::~DeviceGuard()
{
    stopTimer();
    alive.reset();
    engine.isDeviceAllowed = nullptr;
    dm.removeChangeListener (this);

    for (auto* t : watchedTypes)
        t->removeListener (this);
}

bool DeviceGuard::involvesBuiltIn (const juce::AudioDeviceManager::AudioDeviceSetup& s)
{
    return coreaudio::isBuiltInDevice (s.outputDeviceName.toRawUTF8())
        || coreaudio::isBuiltInDevice (s.inputDeviceName.toRawUTF8());
}

juce::String DeviceGuard::preferredName() const
{
    if (preferred == nullptr)
        return {};

    const auto o = outName (*preferred), i = inName (*preferred);
    return o.isNotEmpty() ? (i.isNotEmpty() && i != o ? o + " / " + i : o) : i;
}

bool DeviceGuard::matchesPreferred (const juce::AudioDeviceManager::AudioDeviceSetup& s) const
{
    return preferred != nullptr && s.outputDeviceName == outName (*preferred) && s.inputDeviceName == inName (*preferred);
}

bool DeviceGuard::preferredIsAvailable() const
{
    if (preferred == nullptr)
        return false;

    const auto in = inName (*preferred), out = outName (*preferred);
    const auto typeName = preferred->getStringAttribute ("deviceType");

    for (auto* t : dm.getAvailableDeviceTypes())
    {
        if (typeName.isNotEmpty() && t->getTypeName() != typeName)
            continue;

        t->scanForDevices();
        const bool inOk = in.isEmpty() || t->getDeviceNames (true).contains (in);
        const bool outOk = out.isEmpty() || t->getDeviceNames (false).contains (out);

        if (inOk && outOk)
            return true;
    }

    return false;
}

bool DeviceGuard::isAllowed() const
{
    const auto s = dm.getAudioDeviceSetup();
    return builtInConfirmed || matchesPreferred (s) || ! involvesBuiltIn (s);
}

void DeviceGuard::start()
{
    engine.isDeviceAllowed = [this] { return isAllowed(); };

    ++ownChanges;
    lastOpenAttempt = juce::Time::getMillisecondCounter();

    if (preferred != nullptr)
        engine.initialiseDevice (preferred.get(), false);   // never the default devices instead
    else
        engine.initialiseDevice (nullptr, true);            // first run: the system default, checked below

    dm.addChangeListener (this);

    for (auto* t : dm.getAvailableDeviceTypes())
    {
        t->addListener (this);
        watchedTypes.add (t);
    }

    started = true;
    evaluate (false);
    startTimer (3000);
}

void DeviceGuard::timerCallback()
{
    // while waiting: the device may come back without a list change (Dante restarting)
    if (status.isNotEmpty())
        evaluate (false);
}

void DeviceGuard::openPreferred()
{
    // a device that is listed but fails to open must not be retried in a tight loop
    const auto now = juce::Time::getMillisecondCounter();
    if (now - lastOpenAttempt < 2500)
        return;

    lastOpenAttempt = now;
    ++ownChanges;
    const auto error = dm.initialise (64, 64, preferred.get(), false);

    if (error.isNotEmpty())
        DBG ("Reopening " << preferredName() << ": " << error);
}

bool DeviceGuard::requestDevice (const juce::XmlElement& state)
{
    if (inName (state).isEmpty() && outName (state).isEmpty())
        return false;

    if (preferred != nullptr && preferred->isEquivalentTo (&state, true))
        return true;

    auto previous = std::move (preferred);
    preferred = std::make_unique<juce::XmlElement> (state);

    if (! preferredIsAvailable())
    {
        preferred = std::move (previous);   // not connected here: keep the device in use
        return false;
    }

    lastOpenAttempt = 0;
    openPreferred();

    if (onPreferredChanged != nullptr)
        onPreferredChanged (*preferred);

    evaluate (false);
    return true;
}

void DeviceGuard::closeDevice()
{
    ++ownChanges;
    dm.closeAudioDevice();
}

void DeviceGuard::audioDeviceListChanged()
{
    // JUCE's own listener may have just opened the default device in place of the lost one:
    // look at it once JUCE is done
    listChanged = true;
    juce::MessageManager::callAsync ([this, weak = std::weak_ptr<bool> (alive)] { if (weak.lock() != nullptr && started) evaluate (false); });
}

void DeviceGuard::changeListenerCallback (juce::ChangeBroadcaster*)
{
    const bool own = ownChanges > 0 || listChanged;
    ownChanges = 0;
    listChanged = false;
    evaluate (! own);
}

void DeviceGuard::evaluate (bool userChange)
{
    const auto setup = dm.getAudioDeviceSetup();
    auto* dev = dm.getCurrentAudioDevice();

    // a device that stopped on its own is not a choice: only an open device is
    if (userChange && dev != nullptr)
    {
        // picked in Settings: that is the device to keep from now on (built-in ones included)
        if (dev != nullptr)
        {
            preferred = dm.createStateXml();

            if (preferred == nullptr)   // JUCE only writes a state for explicit settings
                preferred = std::make_unique<juce::XmlElement> ("DEVICESETUP");

            preferred->setAttribute ("deviceType", dm.getCurrentAudioDeviceType());
            preferred->setAttribute ("audioOutputDeviceName", setup.outputDeviceName);
            preferred->setAttribute ("audioInputDeviceName", setup.inputDeviceName);

            if (onPreferredChanged)
                onPreferredChanged (*preferred);
        }

        engine.outputsBlocked = false;
        status = {};
        return;
    }

    if (preferred != nullptr)
    {
        if (dev != nullptr && matchesPreferred (setup))
        {
            engine.outputsBlocked = false;
            status = {};
            return;
        }

        if (preferredIsAvailable())
        {
            openPreferred();

            if (dm.getCurrentAudioDevice() != nullptr && matchesPreferred (dm.getAudioDeviceSetup()))
            {
                engine.outputsBlocked = false;
                status = {};
                return;
            }
        }

        // lost (or not back yet): nothing rather than the Mac's speakers
        if (dm.getCurrentAudioDevice() != nullptr && ! matchesPreferred (dm.getAudioDeviceSetup()))
            closeDevice();

        status = "Waiting for " + preferredName();
        return;
    }

    // no interface chosen yet (first run)
    if (dev != nullptr && ! builtInConfirmed && involvesBuiltIn (setup))
    {
        closeDevice();
        status = "No audio interface chosen";

        if (askUseBuiltIn != nullptr && ! asking)
        {
            asking = true;
            askUseBuiltIn ([this] (bool useBuiltIn)
            {
                asking = false;

                if (! useBuiltIn)
                    return;

                builtInConfirmed = true;
                status = {};
                ++ownChanges;
                dm.initialise (64, 64, nullptr, true);
            });
        }
        return;
    }

    if (dev != nullptr)
    {
        engine.outputsBlocked = ! isAllowed();
        status = {};
    }
}

} // namespace stagerack
