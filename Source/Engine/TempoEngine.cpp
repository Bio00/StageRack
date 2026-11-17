#include "TempoEngine.h"

namespace stagerack
{

//==============================================================================
bool TapEstimator::tap (double t, const Settings& s)
{
    if (count > 0)
    {
        const auto dt = t - times[(size_t) count - 1];

        if (dt < s.minIntervalSec)
            return false;                                  // bounce / double trigger

        if (dt > s.resetSec)
        {
            count = 0;                                     // a pause: new series
        }
        else if (count >= 2 && std::abs (dt - period) > s.changeTolerance * period)
        {
            times[0] = times[(size_t) count - 1];          // tempo changed: start again from the last tap
            count = 1;
        }
    }

    const int maxTaps = juce::jlimit (2, capacity, s.maxTaps);

    while (count >= maxTaps)
    {
        std::copy (times.begin() + 1, times.begin() + count, times.begin());
        --count;
    }

    times[(size_t) count++] = t;
    lastBeat = t;

    if (count >= 2)
        fit();

    return true;
}

void TapEstimator::fit() noexcept
{
    // least squares: t_i = a + period * i
    const double n = count, xMean = (n - 1.0) * 0.5;
    double tMean = 0.0;

    for (int i = 0; i < count; ++i)
        tMean += times[(size_t) i];

    tMean /= n;
    double sxy = 0.0, sxx = 0.0;

    for (int i = 0; i < count; ++i)
    {
        const auto dx = i - xMean;
        sxy += dx * (times[(size_t) i] - tMean);
        sxx += dx * dx;
    }

    period = sxy / sxx;
    lastBeat = tMean + period * ((n - 1.0) - xMean);
}

//==============================================================================
void BeatTracker::addOnset (double t)
{
    onsets.push_back (t);

    const auto oldest = t - windowSec;
    onsets.erase (onsets.begin(), std::find_if (onsets.begin(), onsets.end(), [oldest] (double o) { return o >= oldest; }));

    if (onsets.size() > 96)
        onsets.erase (onsets.begin(), onsets.begin() + (long) (onsets.size() - 96));
}

bool BeatTracker::estimate (double now, double centreBpm, Result& result) const
{
    std::vector<double> o;
    for (auto t : onsets)
        if (t >= now - windowSec)
            o.push_back (t);

    if (o.size() < 6)
        return false;

    centreBpm = juce::jlimit (30.0, 300.0, centreBpm);
    const double pMin = 60.0 / (centreBpm * std::sqrt (2.0)), pMax = 60.0 / (centreBpm / std::sqrt (2.0));
    constexpr double sigma = 0.012;   // onset timing spread (s)

    // 1) comb score over every pair of onsets: intervals that are 1..8 beats
    double bestP = 0.0, bestScore = 0.0;

    for (double p = pMin; p <= pMax; p *= 1.001)
    {
        double score = 0.0;

        for (size_t i = 0; i < o.size(); ++i)
            for (size_t j = i + 1; j < o.size(); ++j)
            {
                const auto ioi = o[j] - o[i];
                if (ioi > 8.5 * p) break;

                const auto k = ioi / p;
                const auto r = std::round (k);
                if (r < 1.0) continue;

                const auto e = (k - r) * p;
                score += std::exp (-(e * e) / (2.0 * sigma * sigma));
            }

        if (score > bestScore)
        {
            bestScore = score;
            bestP = p;
        }
    }

    if (bestP <= 0.0)
        return false;

    // 2) phase: circular mean of the onsets on that period
    double c = 0.0, s = 0.0;
    for (auto t : o)
    {
        const auto a = juce::MathConstants<double>::twoPi * std::fmod (t - o.front(), bestP) / bestP;
        c += std::cos (a);
        s += std::sin (a);
    }
    const auto phase = o.front() + bestP * std::atan2 (s, c) / juce::MathConstants<double>::twoPi;

    // 3) least squares on the onsets that sit on the beat grid
    double sn = 0, st = 0, snn = 0, snt = 0, maxN = -1e9;
    int inliers = 0;

    for (auto t : o)
    {
        const auto n = std::round ((t - phase) / bestP);
        if (std::abs (t - phase - n * bestP) > 0.045)
            continue;   // off-beat hits (eighths, syncopations) don't define the beat

        sn += n; st += t; snn += n * n; snt += n * t;
        maxN = juce::jmax (maxN, n);
        ++inliers;
    }

    if (inliers < 4)
        return false;

    const double m = inliers;
    const auto denom = m * snn - sn * sn;
    if (std::abs (denom) < 1e-9)
        return false;

    const auto period = (m * snt - sn * st) / denom;
    const auto a = (st - period * sn) / m;

    if (period < pMin * 0.97 || period > pMax * 1.03)
        return false;

    result.bpm = 60.0 / period;
    result.beatTime = a + period * maxN;
    result.confidence = (double) inliers / (double) o.size();
    return true;
}

//==============================================================================
double TempoEngine::tapValueBeats (int v) noexcept
{
    switch (v)
    {
        case tapEighth: return 0.5;
        case tapHalf:   return 2.0;
        case tapBar:    return 4.0;
        default:        return 1.0;
    }
}

juce::String TempoEngine::tapValueName (int v)
{
    switch (v)
    {
        case tapEighth: return "1/8";
        case tapHalf:   return "1/2";
        case tapBar:    return "Bar";
        default:        return "1/4";
    }
}

juce::String TempoEngine::shortName (int index)
{
    return index <= 0 ? juce::String ("M") : "T" + juce::String (index);
}

juce::String TempoEngine::sourceName (int s)
{
    switch (s)
    {
        case srcButton: return "TAP";
        case srcKey:    return "KEY";
        case srcMidi:   return "MIDI";
        case srcClock:  return "CLOCK";
        case srcAudio:  return "AUDIO";
        case srcManual: return "SET";
        case srcScene:  return "SCENE";
        case srcOsc:    return "OSC";
        default:        return {};
    }
}

TempoEngine::TempoEngine()
{
    for (int i = 0; i < maxTempos; ++i)
    {
        audioChannels[(size_t) i] = -1;
        audioPoints[(size_t) i] = 0;
        audioRackIds[(size_t) i] = -1;
        audioModes[(size_t) i] = 0;
        audioThresholds[(size_t) i] = -30.0f;
        audioLevels[(size_t) i] = 0.0f;
        onsetCounts[(size_t) i] = 0;
    }

    resetToDefaults();
}

void TempoEngine::resetToDefaults()
{
    const std::lock_guard<std::mutex> sl (lock);
    const auto now = nowSec();

    for (int i = 0; i < maxTempos; ++i)
        initTempo (i, now);

    count = 1;
    settings = Settings();
    learning = -1;
    oscLearning = -1;
    publish();
    ++changeCount;
}

void TempoEngine::initTempo (int i, double now)
{
    auto& s = st[(size_t) i];
    s = State();
    s.cfg.name = i == 0 ? juce::String ("Master") : "Tempo " + juce::String (i);
    s.cfg.ratio = i == 0 ? 0.0 : 1.0;
    s.cfg.keyCode = i == 0 ? 'T' : 0;
    s.gridTime = now;
    syncAudioConfig (i);
}

int TempoEngine::addTempo()
{
    const std::lock_guard<std::mutex> sl (lock);
    const int i = count.load();

    if (i >= maxTempos)
        return -1;

    initTempo (i, nowSec());
    count = i + 1;
    publish();
    ++changeCount;
    return i;
}

void TempoEngine::removeTempo (int index)
{
    const std::lock_guard<std::mutex> sl (lock);
    const int n = count.load();

    if (index <= 0 || index >= n)
        return;

    for (int i = index; i < n - 1; ++i)
    {
        st[(size_t) i] = st[(size_t) i + 1];
        if (st[(size_t) i].cfg.name == "Tempo " + juce::String (i + 1))
            st[(size_t) i].cfg.name = "Tempo " + juce::String (i);   // default names follow the numbering
        syncAudioConfig (i);
    }

    initTempo (n - 1, nowSec());   // unused slot: follows the master (a stray rack on it stays in time)
    count = n - 1;

    if (learning.load() == index) learning = -1;
    if (oscLearning.load() == index) oscLearning = -1;
    publish();
    ++changeCount;
}

//==============================================================================
TempoEngine::Config TempoEngine::getConfig (int i) const
{
    const std::lock_guard<std::mutex> sl (lock);
    return st[(size_t) i].cfg;
}

void TempoEngine::setConfig (int i, const Config& c)
{
    const std::lock_guard<std::mutex> sl (lock);
    auto& s = st[(size_t) i];
    const auto wasFollower = i > 0 && s.cfg.ratio > 0.0;
    double bpm, time, ppq;
    effectiveGrid (i, bpm, time, ppq);

    s.cfg = c;
    if (i == 0) s.cfg.ratio = 0.0;

    if (wasFollower && s.cfg.ratio <= 0.0)
    {
        s.bpm = bpm; s.gridTime = time; s.gridPpq = ppq;   // unlinked: same tempo and beat, now its own
    }

    if (c.audioMode != audioFollow || c.audioChannel < 0)
    {
        s.tracker.clear();
        s.trackLocked = false;
    }

    syncAudioConfig (i);
    ++changeCount;
    publish();
}

TempoEngine::Settings TempoEngine::getSettings() const
{
    const std::lock_guard<std::mutex> sl (lock);
    return settings;
}

void TempoEngine::setSettings (const Settings& s)
{
    const std::lock_guard<std::mutex> sl (lock);
    settings = s;
    settings.minBpm = juce::jlimit (20.0, 200.0, settings.minBpm);
    settings.maxBpm = juce::jlimit (settings.minBpm * 2.0, 400.0, settings.maxBpm);   // an octave at least: folding must land
}

bool TempoEngine::isFollower (int i) const
{
    const std::lock_guard<std::mutex> sl (lock);
    return i > 0 && st[(size_t) i].cfg.ratio > 0.0;
}

void TempoEngine::syncAudioConfig (int i)
{
    const auto& c = st[(size_t) i].cfg;
    audioThresholds[(size_t) i] = c.audioThresholdDb;
    audioModes[(size_t) i] = c.audioMode;
    audioChannels[(size_t) i] = c.audioPoint == pointDeviceInput ? c.audioChannel : -1;
    audioPoints[(size_t) i] = c.audioPoint;
    audioRackIds[(size_t) i] = c.audioRackId;
}

//==============================================================================
double TempoEngine::ppqAt (const State& s, double t) const noexcept
{
    return s.gridPpq + (t - s.gridTime) * s.bpm / 60.0;
}

void TempoEngine::effectiveGrid (int i, double& bpm, double& time, double& ppq) const noexcept
{
    const auto& s = st[(size_t) i];

    if (i > 0 && s.cfg.ratio > 0.0)
    {
        const auto& m = st[0];
        bpm = m.bpm * s.cfg.ratio;
        time = m.gridTime;
        ppq = m.gridPpq * s.cfg.ratio;
        return;
    }

    bpm = s.bpm;
    time = s.gridTime;
    ppq = s.gridPpq;
}

void TempoEngine::unlink (int i)
{
    auto& s = st[(size_t) i];

    if (i == 0 || s.cfg.ratio <= 0.0)
        return;

    effectiveGrid (i, s.bpm, s.gridTime, s.gridPpq);
    s.cfg.ratio = 0.0;
    ++changeCount;
}

void TempoEngine::setOwnBpm (int i, double bpm, double t)
{
    auto& s = st[(size_t) i];
    s.gridPpq = ppqAt (s, t);
    s.gridTime = t;
    s.bpm = juce::jlimit (20.0, 400.0, bpm);
}

void TempoEngine::alignTo (int i, double beatTime, double unit)
{
    auto& s = st[(size_t) i];
    const auto p = ppqAt (s, beatTime);
    s.gridPpq = std::round (p / unit) * unit;
    s.gridTime = beatTime;
}

double TempoEngine::foldAndRound (double bpm) const noexcept
{
    if (bpm <= 0.0 || ! std::isfinite (bpm))
        return 120.0;

    while (bpm > settings.maxBpm && bpm * 0.5 >= settings.minBpm) bpm *= 0.5;
    while (bpm < settings.minBpm && bpm * 2.0 <= settings.maxBpm) bpm *= 2.0;

    if (settings.roundStep > 0.0)
        bpm = std::round (bpm / settings.roundStep) * settings.roundStep;

    return juce::jlimit (20.0, 400.0, bpm);
}

TapEstimator::Settings TempoEngine::tapSettings() const noexcept
{
    TapEstimator::Settings t;
    t.maxTaps = settings.maxTaps;
    t.resetSec = settings.resetSec;
    return t;
}

void TempoEngine::publish()
{
    for (int i = 0; i < maxTempos; ++i)
    {
        double bpm, time, ppq;
        effectiveGrid (i, bpm, time, ppq);

        auto& g = grids[(size_t) i];
        const auto s0 = g.seq.load (std::memory_order_relaxed);
        g.seq.store (s0 + 1, std::memory_order_relaxed);
        std::atomic_thread_fence (std::memory_order_release);
        g.bpm.store (bpm, std::memory_order_relaxed);
        g.timeSec.store (time, std::memory_order_relaxed);
        g.ppq.store (ppq, std::memory_order_relaxed);
        g.seq.store (s0 + 2, std::memory_order_release);
    }
}

//==============================================================================
double TempoEngine::getBpm (int i) const
{
    const std::lock_guard<std::mutex> sl (lock);
    double bpm, time, ppq;
    effectiveGrid (i, bpm, time, ppq);
    return bpm;
}

void TempoEngine::setBpm (int i, double bpm, int source)
{
    const std::lock_guard<std::mutex> sl (lock);
    unlink (i);
    setOwnBpm (i, bpm, nowSec());
    st[(size_t) i].taps.reset();
    st[(size_t) i].lastSource = source;
    st[(size_t) i].anchorBpm = st[(size_t) i].bpm;   // a tempo you set counts like a tap
    st[(size_t) i].differsSince = -1.0;
    publish();
}

void TempoEngine::multiplyBpm (int i, double factor)
{
    const std::lock_guard<std::mutex> sl (lock);
    auto& s = st[(size_t) i];

    if (i > 0 && s.cfg.ratio > 0.0)
    {
        s.cfg.ratio = juce::jlimit (0.125, 8.0, s.cfg.ratio * factor);   // a follower keeps following
        ++changeCount;
    }
    else
    {
        setOwnBpm (i, s.bpm * factor, nowSec());
        s.taps.reset();
        if (s.anchorBpm > 0.0) s.anchorBpm = s.bpm;
    }

    s.lastSource = srcManual;
    publish();
}

void TempoEngine::link (int i, double ratio)
{
    if (i <= 0 || ratio <= 0.0)
        return;

    const std::lock_guard<std::mutex> sl (lock);
    st[(size_t) i].cfg.ratio = ratio;
    st[(size_t) i].taps.reset();
    st[(size_t) i].anchorBpm = 0.0;
    ++changeCount;
    publish();
}

void TempoEngine::tap (int i, double t, int source)
{
    const std::lock_guard<std::mutex> sl (lock);
    tapLocked (i, t, source);
}

void TempoEngine::tapLocked (int i, double t, int source)
{
    unlink (i);
    auto& s = st[(size_t) i];

    if (! s.taps.tap (t, tapSettings()))
        return;

    if (source != srcAudio)
        userTaps[(size_t) i].fetch_add (1, std::memory_order_release);   // TempoOutput sends it on ("my taps")

    s.lastSource = source;
    s.lastEvent = t;
    const auto unit = tapValueBeats (s.cfg.tapValue);

    if (s.taps.hasPeriod())
    {
        const auto bpm = foldAndRound (60.0 * unit / s.taps.getPeriod());
        const auto beat = s.taps.getLastBeatTime();
        setOwnBpm (i, bpm, beat);

        if (source != srcAudio)
        {
            s.anchorBpm = bpm;          // your tempo: the audio may only fine-tune it now
            s.differsSince = -1.0;
        }

        if (settings.alignBeat)
            alignTo (i, beat, unit);
    }
    else if (settings.alignBeat)
    {
        alignTo (i, t, unit);   // a single tap already puts the beat (or the bar) there
    }

    publish();
}

bool TempoEngine::handleKey (int keyCode, double t)
{
    const std::lock_guard<std::mutex> sl (lock);
    bool used = false;

    auto same = [] (int a, int b)
    {
        if (a < 256 && b < 256)
            return juce::CharacterFunctions::toLowerCase ((juce::juce_wchar) a) == juce::CharacterFunctions::toLowerCase ((juce::juce_wchar) b);
        return a == b;
    };

    for (int i = 0; i < maxTempos; ++i)
        if (st[(size_t) i].cfg.keyCode != 0 && same (st[(size_t) i].cfg.keyCode, keyCode))
        {
            tapLocked (i, t, srcKey);
            used = true;
        }

    return used;
}

bool TempoEngine::isKeyUsed (int keyCode) const
{
    const std::lock_guard<std::mutex> sl (lock);

    for (auto& s : st)
        if (s.cfg.keyCode != 0
            && (s.cfg.keyCode == keyCode
                || (keyCode < 256 && s.cfg.keyCode < 256
                    && juce::CharacterFunctions::toLowerCase ((juce::juce_wchar) keyCode) == juce::CharacterFunctions::toLowerCase ((juce::juce_wchar) s.cfg.keyCode))))
            return true;

    return false;
}

//==============================================================================
void TempoEngine::learnOsc (int i, int kind)
{
    oscLearningKind = kind;
    oscLearning = i;
    ++changeCount;
}

void TempoEngine::phaseLocked (int i, double t, double unit)
{
    // a follower takes its beat from the master: move the master's
    const int target = st[(size_t) i].cfg.ratio > 0.0 && i > 0 ? 0 : i;
    alignTo (target, t, unit);
    st[(size_t) target].lastSource = srcOsc;
    st[(size_t) target].lastEvent = t;
    publish();
}

bool TempoEngine::handleOsc (const juce::String& address, const std::vector<float>& values, double t)
{
    const bool press = values.empty() || values[0] > 0.5f;   // buttons send 1 on press, 0 on release
    const std::lock_guard<std::mutex> sl (lock);
    const int n = count.load();

    // learn: the next message (a button press, or a number for BPM)
    if (const auto l = oscLearning.load(); l >= 0 && l < n && ! address.startsWithIgnoreCase ("/stagerack/"))
    {
        auto& c = st[(size_t) l].cfg;

        if (oscLearningKind.load() == oscLearnBpm)
        {
            if (values.empty())
                return false;   // wait for a message with a number
            c.oscBpm = address;
        }
        else
        {
            if (! press)
                return false;
            c.oscTap = address;
        }

        oscLearning = -1;
        ++changeCount;
        return true;
    }

    // fixed addresses: /stagerack/<cmd>[/<i>] or /stagerack/tempo/<i>/<cmd>
    if (address.startsWithIgnoreCase ("/stagerack/"))
    {
        auto parts = juce::StringArray::fromTokens (address.substring (11), "/", {});
        parts.removeEmptyStrings();
        juce::String cmd, which;

        if (parts.size() >= 3 && parts[0].equalsIgnoreCase ("tempo")) { which = parts[1]; cmd = parts[2]; }
        else if (parts.size() >= 1)                                     { cmd = parts[0]; which = parts[1]; }

        int i = 0;
        if (which.isNotEmpty() && ! which.equalsIgnoreCase ("m") && ! which.equalsIgnoreCase ("master"))
        {
            const auto digits = which.retainCharacters ("0123456789");
            if (digits.isEmpty()) return false;
            i = digits.getIntValue();
        }

        if (! juce::isPositiveAndBelow (i, n))
            return false;

        cmd = cmd.toLowerCase();
        auto& s = st[(size_t) i];

        if (cmd == "tap")
        {
            if (! press) return false;
            tapLocked (i, t, srcOsc);
            return true;
        }

        if (cmd == "bpm" && ! values.empty() && std::isfinite (values[0]) && values[0] >= 20.0f && values[0] <= 400.0f)
        {
            unlink (i);
            if (std::abs (s.bpm - (double) values[0]) > 0.001)
                setOwnBpm (i, values[0], t);
            s.taps.reset();
            s.anchorBpm = s.bpm;
            s.differsSince = -1.0;
            s.lastSource = srcOsc;
            s.lastEvent = t;
            publish();
            return true;
        }

        if (cmd == "beat" || cmd == "bar")
        {
            if (! press) return false;
            phaseLocked (i, t, cmd == "bar" ? 4.0 : 1.0);
            return true;
        }

        if ((cmd == "half" || cmd == "double") && press)
        {
            const double f = cmd == "half" ? 0.5 : 2.0;
            if (i > 0 && s.cfg.ratio > 0.0) { s.cfg.ratio = juce::jlimit (0.125, 8.0, s.cfg.ratio * f); ++changeCount; }
            else                             { setOwnBpm (i, s.bpm * f, t); s.taps.reset(); }
            s.lastSource = srcOsc;
            publish();
            return true;
        }

        if (cmd == "follow" && i > 0)
        {
            const double r = values.empty() ? 1.0 : (double) values[0];
            if (r > 0.0) { s.cfg.ratio = juce::jlimit (0.125, 8.0, r); ++changeCount; }
            else         unlink (i);
            publish();
            return true;
        }

        return false;
    }

    // learned addresses
    bool used = false;

    for (int i = 0; i < n; ++i)
    {
        auto& s = st[(size_t) i];

        if (s.cfg.oscTap.isNotEmpty() && address == s.cfg.oscTap && press)
        {
            tapLocked (i, t, srcOsc);
            used = true;
        }

        if (s.cfg.oscBpm.isNotEmpty() && address == s.cfg.oscBpm && ! values.empty()
            && std::isfinite (values[0]) && values[0] >= 20.0f && values[0] <= 400.0f)
        {
            unlink (i);
            if (std::abs (s.bpm - (double) values[0]) > 0.001)
            {
                setOwnBpm (i, values[0], t);
                s.taps.reset();
            }
            s.anchorBpm = s.bpm;
            s.differsSince = -1.0;
            s.lastSource = srcOsc;
            s.lastEvent = t;
            publish();
            used = true;
        }
    }

    return used;
}

void TempoEngine::learnMidi (int i)
{
    learning = i;
    ++changeCount;
}

void TempoEngine::handleMidi (const juce::MidiMessage& m)
{
    // JUCE stamps incoming MIDI in seconds on the millisecond-counter clock (= nowSec())
    auto t = m.getTimeStamp();
    if (t <= 0.0 || std::abs (t - nowSec()) > 5.0)
        t = nowSec();

    const std::lock_guard<std::mutex> sl (lock);

    const bool isNote = m.isNoteOn() && m.getVelocity() > 0;
    const bool isCC = m.isController() && m.getControllerValue() > 63;

    if (const auto l = learning.load(); l >= 0 && l < maxTempos && (isNote || isCC))
    {
        auto& c = st[(size_t) l].cfg;
        c.midiKind = isNote ? midiNote : midiCC;
        c.midiNumber = isNote ? m.getNoteNumber() : m.getControllerNumber();
        c.midiChannel = m.getChannel();
        learning = -1;
        ++changeCount;
        return;
    }

    if (m.isMidiClock())
    {
        for (int i = 0; i < maxTempos; ++i)
            if (st[(size_t) i].cfg.midiClock)
                clockTick (i, t);
        return;
    }

    if (m.isMidiStart() || m.isSongPositionPointer())
    {
        // the next clock tick is the beat (or the 16th of the song position)
        const juce::int64 pos = m.isMidiStart() ? -1 : (juce::int64) m.getSongPositionPointerMidiBeat() * 6 - 1;

        for (auto& s : st)
            if (s.cfg.midiClock)
                s.clock.tickPos = pos;
        return;
    }

    if (! isNote && ! isCC)
        return;

    for (int i = 0; i < maxTempos; ++i)
    {
        const auto& c = st[(size_t) i].cfg;

        if (c.midiKind == midiNone || (c.midiChannel != 0 && c.midiChannel != m.getChannel()))
            continue;

        if ((c.midiKind == midiNote && isNote && m.getNoteNumber() == c.midiNumber)
            || (c.midiKind == midiCC && isCC && m.getControllerNumber() == c.midiNumber))
            tapLocked (i, t, srcMidi);
    }
}

void TempoEngine::clockTick (int i, double t)
{
    auto& s = st[(size_t) i];
    auto& c = s.clock;
    constexpr int cap = (int) std::tuple_size<decltype (c.ticks)>::value;

    if (t - c.lastTick > 0.5)
    {
        c.count = 0;   // clock was stopped: a fresh estimate
        c.head = 0;
    }

    c.lastTick = t;
    c.ticks[(size_t) c.head] = t;
    c.head = (c.head + 1) % cap;
    c.count = juce::jmin (c.count + 1, cap);

    if (c.tickPos >= -1)
        ++c.tickPos;

    const bool onBeat = c.tickPos >= 0 && c.tickPos % 24 == 0;

    if (c.count < 24 || (! onBeat && ++c.ticksSeen % 6 != 0))
        return;

    // tick period: least squares over the ring (MIDI timing jitter averages out)
    const int n = c.count;
    const int first = (c.head - n + cap) % cap;
    double tMean = 0.0;
    for (int k = 0; k < n; ++k)
        tMean += c.ticks[(size_t) ((first + k) % cap)];
    tMean /= n;

    const double xMean = (n - 1) * 0.5;
    double sxy = 0.0, sxx = 0.0;
    for (int k = 0; k < n; ++k)
    {
        const auto dx = k - xMean;
        sxy += dx * (c.ticks[(size_t) ((first + k) % cap)] - tMean);
        sxx += dx * dx;
    }

    const auto tickPeriod = sxy / sxx;
    if (tickPeriod <= 0.0)
        return;

    unlink (i);
    const auto bpm = juce::jlimit (20.0, 400.0, 60.0 / (24.0 * tickPeriod));
    const auto fitted = tMean + tickPeriod * ((n - 1) - xMean);   // where the line puts this tick

    if (std::abs (bpm - s.bpm) > 0.02)
        setOwnBpm (i, bpm, fitted);

    if (onBeat)
    {
        s.gridTime = fitted;
        s.gridPpq = (double) c.tickPos / 24.0;   // beats since Start: in phase with the sender
    }

    s.lastSource = srcClock;
    s.lastEvent = t;
    publish();
}

void TempoEngine::poll (double now)
{
    const std::lock_guard<std::mutex> sl (lock);

    int start1, size1, start2, size2;
    onsetFifo.prepareToRead (onsetFifo.getNumReady(), start1, size1, start2, size2);

    auto handle = [this] (const Onset& o)
    {
        if (! juce::isPositiveAndBelow (o.index, maxTempos))
            return;

        auto& s = st[(size_t) o.index];

        if (s.cfg.audioMode == audioTrigger)
        {
            // the hits make their own series (never mixed with your taps)
            if (! s.audioTaps.tap (o.timeSec, tapSettings()))
                return;

            s.lastAudio = o.timeSec;
            const auto unit = tapValueBeats (s.cfg.tapValue);

            if (s.audioTaps.hasPeriod())
                audioSays (o.index, foldAndRound (60.0 * unit / s.audioTaps.getPeriod()), s.audioTaps.getLastBeatTime(), unit, o.timeSec);
            else if (s.anchorBpm <= 0.0 && settings.alignBeat)
            {
                unlink (o.index);
                alignTo (o.index, o.timeSec, unit);   // a single hit already puts the beat there
                s.lastSource = srcAudio;
                s.lastEvent = o.timeSec;
                publish();
            }
        }
        else
        {
            s.tracker.addOnset (o.timeSec);
            s.lastOnset = o.timeSec;
        }
    };

    for (int k = 0; k < size1; ++k) handle (onsetRing[(size_t) (start1 + k)]);
    for (int k = 0; k < size2; ++k) handle (onsetRing[(size_t) (start2 + k)]);
    onsetFifo.finishedRead (size1 + size2);

    for (int i = 0; i < maxTempos; ++i)
        if ((st[(size_t) i].cfg.audioPoint == pointDeviceInput ? st[(size_t) i].cfg.audioChannel >= 0 : st[(size_t) i].cfg.audioRackId >= 0)
            && st[(size_t) i].cfg.audioMode == audioFollow)
            trackBeats (i, now);
}

void TempoEngine::trackBeats (int i, double now)
{
    auto& s = st[(size_t) i];

    if (now - s.lastOnset > 3.0)
    {
        s.trackLocked = false;   // the drums stopped: keep the tempo, show it's not tracking
        s.trackCandidate = 0.0;
        s.differsSince = -1.0;
        return;
    }

    if (now - s.lastTrackEval < 0.5 || s.lastOnset <= s.lastTrackEval)
        return;

    s.lastTrackEval = now;

    double centre, time, ppq;
    effectiveGrid (i, centre, time, ppq);

    BeatTracker::Result r;
    if (! s.tracker.estimate (now, centre, r) || r.confidence < 0.4)
    {
        s.trackConfidence = 0.0f;
        return;
    }

    s.trackConfidence = (float) r.confidence;
    const bool stable = s.trackCandidate > 0.0 && std::abs (r.bpm - s.trackCandidate) < 0.01 * s.trackCandidate;
    s.trackCandidate = r.bpm;

    if (! stable)
        return;

    s.trackLocked = true;
    s.lastAudio = now;
    const auto bpm = settings.roundStep > 0.0 ? std::round (r.bpm / settings.roundStep) * settings.roundStep : r.bpm;
    audioSays (i, bpm, r.beatTime, 1.0, now);
}

void TempoEngine::audioSays (int i, double bpm, double beatTime, double unit, double now)
{
    auto& s = st[(size_t) i];
    s.audioBpm = bpm;
    const bool follow = s.cfg.audioMode == audioFollow;

    if (s.anchorBpm > 0.0 && std::abs (bpm - s.anchorBpm) > 0.01 * (double) s.cfg.audioWindowPct * s.anchorBpm)
    {
        // far from what you gave: your tempo wins; with "return after N s" the audio takes over
        // only if it keeps saying the same thing that long
        if (s.differsSince < 0.0)
            s.differsSince = now;

        if (s.cfg.returnToAudioSec <= 0 || now - s.differsSince < (double) s.cfg.returnToAudioSec)
            return;

        s.anchorBpm = 0.0;   // back to the audio
    }

    s.differsSince = -1.0;
    unlink (i);
    s.lastSource = srcAudio;
    s.lastEvent = beatTime;
    bool changed = false;

    // within the window (or free): the audio sets the tempo and keeps the beat on it
    if (! follow || std::abs (bpm - s.bpm) > 0.1)
    {
        setOwnBpm (i, bpm, beatTime);
        changed = true;
    }

    if (follow)
    {
        // keep the beat on the drums, without nudging the plugins for nothing
        const auto p = ppqAt (s, beatTime);
        if (std::abs (p - std::round (p)) > 0.08)
        {
            alignTo (i, beatTime, 1.0);
            changed = true;
        }
    }
    else if (settings.alignBeat)
    {
        alignTo (i, beatTime, unit);
        changed = true;
    }

    if (changed)
        publish();
}

TempoEngine::AudioOpinion TempoEngine::getAudioOpinion (int i, double now) const
{
    const std::lock_guard<std::mutex> sl (lock);
    const auto& s = st[(size_t) i];
    AudioOpinion o;
    if (now < 0.0) now = nowSec();

    if (s.cfg.audioPoint == pointDeviceInput ? s.cfg.audioChannel < 0 : s.cfg.audioRackId < 0)
        return o;

    o.anchored = s.anchorBpm > 0.0;
    if (now - s.lastAudio > 4.0)
        return o;

    o.bpm = s.audioBpm;
    o.differs = s.differsSince >= 0.0;
    if (o.differs && s.cfg.returnToAudioSec > 0)
        o.takesOverIn = juce::jmax (0.0, (double) s.cfg.returnToAudioSec - (now - s.differsSince));
    return o;
}

//==============================================================================
double TempoEngine::getBeatPosition (int i, double t) const
{
    const std::lock_guard<std::mutex> sl (lock);
    double bpm, time, ppq;
    effectiveGrid (i, bpm, time, ppq);
    return ppq + (t - time) * bpm / 60.0;
}

int TempoEngine::getTapCount (int i) const
{
    const std::lock_guard<std::mutex> sl (lock);
    return st[(size_t) i].taps.getNumTaps();
}

int TempoEngine::getLastSource (int i) const
{
    const std::lock_guard<std::mutex> sl (lock);
    return st[(size_t) i].lastSource;
}

double TempoEngine::getLastEventTime (int i) const
{
    const std::lock_guard<std::mutex> sl (lock);
    return st[(size_t) i].lastEvent;
}

bool TempoEngine::isClockRunning (int i) const
{
    const std::lock_guard<std::mutex> sl (lock);
    return st[(size_t) i].cfg.midiClock && nowSec() - st[(size_t) i].clock.lastTick < 0.5;
}

bool TempoEngine::isTracking (int i) const
{
    const std::lock_guard<std::mutex> sl (lock);
    return st[(size_t) i].trackLocked;
}

float TempoEngine::getTrackConfidence (int i) const
{
    const std::lock_guard<std::mutex> sl (lock);
    return st[(size_t) i].trackConfidence;
}

float TempoEngine::readAudioLevel (int i) noexcept
{
    return audioLevels[(size_t) i].exchange (0.0f, std::memory_order_relaxed);
}

//==============================================================================
juce::ValueTree TempoEngine::toTree() const
{
    const std::lock_guard<std::mutex> sl (lock);
    juce::ValueTree t ("TEMPO");
    t.setProperty ("maxTaps", settings.maxTaps, nullptr)
     .setProperty ("resetSec", settings.resetSec, nullptr)
     .setProperty ("round", settings.roundStep, nullptr)
     .setProperty ("minBpm", settings.minBpm, nullptr)
     .setProperty ("maxBpm", settings.maxBpm, nullptr)
     .setProperty ("align", settings.alignBeat, nullptr)
     .setProperty ("count", count.load(), nullptr);

    for (int i = 0; i < count.load(); ++i)
    {
        const auto& s = st[(size_t) i];
        const auto& c = s.cfg;
        juce::ValueTree e ("T");
        e.setProperty ("index", i, nullptr)
         .setProperty ("name", c.name, nullptr)
         .setProperty ("bpm", s.bpm, nullptr)
         .setProperty ("ratio", c.ratio, nullptr)
         .setProperty ("tapValue", c.tapValue, nullptr)
         .setProperty ("key", c.keyCode != 0 ? juce::KeyPress (c.keyCode).getTextDescription() : juce::String(), nullptr)
         .setProperty ("midiKind", c.midiKind, nullptr)
         .setProperty ("midiNumber", c.midiNumber, nullptr)
         .setProperty ("midiChannel", c.midiChannel, nullptr)
         .setProperty ("clock", c.midiClock, nullptr)
         .setProperty ("audioCh", c.audioChannel, nullptr)
         .setProperty ("audioMode", c.audioMode, nullptr)
         .setProperty ("audioThr", c.audioThresholdDb, nullptr)
         .setProperty ("audioPoint", c.audioPoint, nullptr)
         .setProperty ("audioRack", c.audioRackId, nullptr)
         .setProperty ("returnSec", c.returnToAudioSec, nullptr)
         .setProperty ("audioWin", c.audioWindowPct, nullptr)
         .setProperty ("scene", c.sceneRecall, nullptr);
        if (c.sendsOut() || c.midiOutId.isNotEmpty() || c.oscOutHost != "127.0.0.1" || c.oscOutPort != 9000)
            e.setProperty ("outWhat", c.outWhat, nullptr)
             .setProperty ("midiOut", c.midiOutId, nullptr)
             .setProperty ("midiOutName", c.midiOutName, nullptr)
             .setProperty ("midiOutKind", c.midiOutKind, nullptr)
             .setProperty ("midiOutNum", c.midiOutNumber, nullptr)
             .setProperty ("midiOutCh", c.midiOutChannel, nullptr)
             .setProperty ("midiOutClock", c.midiOutClock, nullptr)
             .setProperty ("oscOut", c.oscOut, nullptr)
             .setProperty ("oscOutHost", c.oscOutHost, nullptr)
             .setProperty ("oscOutPort", c.oscOutPort, nullptr)
             .setProperty ("oscOutBeat", c.oscOutBeat, nullptr)
             .setProperty ("oscOutBpm", c.oscOutBpm, nullptr);
        if (c.oscTap.isNotEmpty()) e.setProperty ("oscTap", c.oscTap, nullptr);
        if (c.oscBpm.isNotEmpty()) e.setProperty ("oscBpm", c.oscBpm, nullptr);
        t.appendChild (e, nullptr);
    }

    return t;
}

void TempoEngine::fromTree (const juce::ValueTree& t)
{
    resetToDefaults();

    if (! t.isValid())
        return;

    const std::lock_guard<std::mutex> sl (lock);
    Settings def;
    settings.maxTaps = juce::jlimit (2, 32, (int) t.getProperty ("maxTaps", def.maxTaps));
    settings.resetSec = juce::jlimit (0.5, 10.0, (double) t.getProperty ("resetSec", def.resetSec));
    settings.roundStep = juce::jlimit (0.0, 1.0, (double) t.getProperty ("round", def.roundStep));
    settings.minBpm = juce::jlimit (20.0, 200.0, (double) t.getProperty ("minBpm", def.minBpm));
    settings.maxBpm = juce::jlimit (settings.minBpm * 2.0, 400.0, (double) t.getProperty ("maxBpm", def.maxBpm));
    settings.alignBeat = (bool) t.getProperty ("align", def.alignBeat);
    const auto now = nowSec();
    int n = 1;
    for (const auto& e : t)
        n = juce::jmax (n, (int) e["index"] + 1);
    count = juce::jlimit (1, maxTempos, (int) t.getProperty ("count", n));

    for (const auto& e : t)
    {
        const int i = e["index"];
        if (! juce::isPositiveAndBelow (i, count.load()))
            continue;

        auto& s = st[(size_t) i];
        auto& c = s.cfg;
        c.name = e.getProperty ("name", c.name).toString();
        c.ratio = i == 0 ? 0.0 : juce::jlimit (0.0, 8.0, (double) e["ratio"]);
        c.tapValue = juce::jlimit (0, (int) tapBar, (int) e.getProperty ("tapValue", (int) tapQuarter));
        const auto key = e["key"].toString();
        c.keyCode = key.isNotEmpty() ? juce::KeyPress::createFromDescription (key).getKeyCode() : 0;
        c.midiKind = juce::jlimit (0, (int) midiCC, (int) e["midiKind"]);
        c.midiNumber = juce::jlimit (0, 127, (int) e["midiNumber"]);
        c.midiChannel = juce::jlimit (0, 16, (int) e["midiChannel"]);
        c.midiClock = (bool) e["clock"];
        c.audioChannel = juce::jlimit (-1, 255, (int) e.getProperty ("audioCh", -1));
        c.audioMode = juce::jlimit (0, (int) audioFollow, (int) e["audioMode"]);
        c.audioThresholdDb = juce::jlimit (-70.0f, 0.0f, (float) e.getProperty ("audioThr", -30.0f));
        c.audioPoint = juce::jlimit (0, (int) pointPostFader, (int) e.getProperty ("audioPoint", 0));
        c.audioRackId = (int) e.getProperty ("audioRack", -1);
        c.returnToAudioSec = juce::jlimit (0, 120, (int) e.getProperty ("returnSec", 0));
        c.audioWindowPct = juce::jlimit (1.0f, 25.0f, (float) e.getProperty ("audioWin", 4.0f));
        c.outWhat = juce::jlimit (0, (int) outTaps, (int) e["outWhat"]);
        c.midiOutId = e["midiOut"].toString();
        c.midiOutName = e["midiOutName"].toString();
        c.midiOutKind = juce::jlimit (0, (int) midiCC, (int) e["midiOutKind"]);
        c.midiOutNumber = juce::jlimit (0, 127, (int) e.getProperty ("midiOutNum", 60));
        c.midiOutChannel = juce::jlimit (1, 16, (int) e.getProperty ("midiOutCh", 1));
        c.midiOutClock = (bool) e["midiOutClock"];
        c.oscOut = (bool) e["oscOut"];
        c.oscOutHost = e.getProperty ("oscOutHost", "127.0.0.1").toString();
        c.oscOutPort = juce::jlimit (1, 65535, (int) e.getProperty ("oscOutPort", 9000));
        c.oscOutBeat = e["oscOutBeat"].toString();
        c.oscOutBpm = e["oscOutBpm"].toString();
        c.sceneRecall = (bool) e.getProperty ("scene", true);
        c.oscTap = e["oscTap"].toString();
        c.oscBpm = e["oscBpm"].toString();
        s.bpm = juce::jlimit (20.0, 400.0, (double) e.getProperty ("bpm", 120.0));
        s.gridTime = now;
        s.gridPpq = 0.0;
        syncAudioConfig (i);
    }

    publish();
    ++changeCount;
}

void TempoEngine::storeInScene (juce::ValueTree& data) const
{
    const std::lock_guard<std::mutex> sl (lock);

    for (int i = 0; i < count.load(); ++i)
    {
        const auto& s = st[(size_t) i];
        if (! s.cfg.sceneRecall)
            continue;

        double bpm, time, ppq;
        effectiveGrid (i, bpm, time, ppq);
        data.setProperty ("tempo" + juce::String (i), bpm, nullptr);
        if (i > 0)
            data.setProperty ("tempoRatio" + juce::String (i), s.cfg.ratio, nullptr);
    }
}

void TempoEngine::recallFromScene (const juce::ValueTree& data)
{
    const std::lock_guard<std::mutex> sl (lock);
    const auto now = nowSec();

    for (int i = 0; i < count.load(); ++i)
    {
        auto& s = st[(size_t) i];
        const auto key = "tempo" + juce::String (i);

        if (! s.cfg.sceneRecall || ! data.hasProperty (key))
            continue;

        // a tempo driven from outside (MIDI clock, drums) is not overridden by a scene
        if ((s.cfg.midiClock && now - s.clock.lastTick < 0.5) || (s.cfg.audioMode == audioFollow && s.trackLocked))
            continue;

        const auto ratio = i > 0 ? (double) data.getProperty ("tempoRatio" + juce::String (i), 0.0) : 0.0;

        if (ratio > 0.0)
        {
            s.cfg.ratio = ratio;
        }
        else
        {
            unlink (i);
            setOwnBpm (i, (double) data[key], now);
            s.anchorBpm = s.bpm;   // the song's tempo: the drums fine-tune it
            s.differsSince = -1.0;
        }

        s.taps.reset();
        s.lastSource = srcScene;
    }

    ++changeCount;
    publish();
}

//==============================================================================
void TempoEngine::prepare (double sr)
{
    sampleRate = sr;
    samplePos = 0;
    firstBlock = true;

    auto coeff = [sr] (double seconds) { return (float) (1.0 - std::exp (-1.0 / (seconds * sr))); };
    atkFast = coeff (0.0005);
    relFast = coeff (0.010);
    atkSlow = coeff (0.030);
    relSlow = coeff (0.300);

    for (auto& d : detectors)
        d = Detector();
}

void TempoEngine::processBlock (const float* const* ins, int numIns, int numSamples, double blockSec, juce::uint64 hostNs) noexcept
{
    for (int i = 0; i < maxTempos; ++i)
    {
        auto& g = grids[(size_t) i];
        auto& b = blocks[(size_t) i];
        double bpm = b.bpm, time = 0.0, ppq = 0.0;
        juce::uint32 seq = 0;
        bool ok = false;

        for (int attempt = 0; attempt < 4 && ! ok; ++attempt)
        {
            seq = g.seq.load (std::memory_order_acquire);
            if (seq & 1u) continue;
            bpm = g.bpm.load (std::memory_order_relaxed);
            time = g.timeSec.load (std::memory_order_relaxed);
            ppq = g.ppq.load (std::memory_order_relaxed);
            std::atomic_thread_fence (std::memory_order_acquire);
            ok = g.seq.load (std::memory_order_relaxed) == seq;
        }

        if (! ok)
        {
            bpm = b.bpm;   // being written right now: carry on with the last block's tempo
            seq = seenSeq[(size_t) i];
        }

        auto& a = acc[(size_t) i];

        if (ok)
        {
            const auto ideal = ppq + (blockSec - time) * bpm / 60.0;
            const auto err = ideal - a;

            // a new tap / beat: jump onto it; otherwise only glide (callback timing jitter,
            // audio clock vs. system clock), so the plugins see a smooth beat position
            if (firstBlock || (seq != seenSeq[(size_t) i] && std::abs (err) > 0.02) || std::abs (err) > 0.25)
                a = ideal;
            else
                a += 0.02 * err;
        }

        seenSeq[(size_t) i] = seq;
        b.bpm = bpm;
        b.ppq = a;
        b.timeSec = blockSec;
        b.samplePos = samplePos;
        b.hostTimeNs = hostNs;
        a += (double) numSamples / sampleRate * bpm / 60.0;
    }

    firstBlock = false;

    // audio taps from device inputs (the rack points come later: detectRackAudio)
    blockStartSec = blockSec;

    for (int i = 0; i < maxTempos; ++i)
    {
        if (audioPoints[(size_t) i].load (std::memory_order_relaxed) != pointDeviceInput)
            continue;

        const auto ch = audioChannels[(size_t) i].load (std::memory_order_relaxed);
        detect (i, juce::isPositiveAndBelow (ch, numIns) ? ins[ch] : nullptr, numSamples);
    }

    samplePos += numSamples;
}

bool TempoEngine::readGrid (int i, double& bpm, double& time, double& ppq) const noexcept
{
    const auto& g = grids[(size_t) juce::jlimit (0, maxTempos - 1, i)];

    for (int attempt = 0; attempt < 8; ++attempt)
    {
        const auto seq = g.seq.load (std::memory_order_acquire);
        if (seq & 1u) continue;
        bpm = g.bpm.load (std::memory_order_relaxed);
        time = g.timeSec.load (std::memory_order_relaxed);
        ppq = g.ppq.load (std::memory_order_relaxed);
        std::atomic_thread_fence (std::memory_order_acquire);
        if (g.seq.load (std::memory_order_relaxed) == seq)
            return true;
    }

    return false;
}

void TempoEngine::detectRackAudio (int i, const float* x, int numSamples) noexcept
{
    if (juce::isPositiveAndBelow (i, maxTempos) && audioPoints[(size_t) i].load (std::memory_order_relaxed) != pointDeviceInput)
        detect (i, x, numSamples);
}

// transient = fast envelope above the threshold and 6 dB over the slow one
void TempoEngine::detect (int i, const float* x, int numSamples) noexcept
{
    auto& d = detectors[(size_t) i];

    if (x == nullptr)
    {
        d.fast = d.slow = 0.0f;
        return;
    }

    const auto thr = juce::Decibels::decibelsToGain (audioThresholds[(size_t) i].load (std::memory_order_relaxed));
    const auto refractory = (int) (sampleRate * (audioModes[(size_t) i].load (std::memory_order_relaxed) == audioFollow ? 0.09 : 0.12));
    float peak = 0.0f;

    for (int n = 0; n < numSamples; ++n)
    {
        const auto v = std::abs (x[n]);
        d.fast += (v > d.fast ? atkFast : relFast) * (v - d.fast);
        d.slow += (v > d.slow ? atkSlow : relSlow) * (v - d.slow);
        peak = juce::jmax (peak, d.fast);

        if (++d.sinceOnset > refractory && d.fast > thr && d.fast > 2.0f * d.slow)
        {
            d.sinceOnset = 0;
            onsetCounts[(size_t) i].fetch_add (1, std::memory_order_relaxed);

            int s1, n1, s2, n2;
            onsetFifo.prepareToWrite (1, s1, n1, s2, n2);
            if (n1 > 0)
            {
                onsetRing[(size_t) s1] = { i, blockStartSec + n / sampleRate };
                onsetFifo.finishedWrite (1);
            }
        }
    }

    if (peak > audioLevels[(size_t) i].load (std::memory_order_relaxed))
        audioLevels[(size_t) i].store (peak, std::memory_order_relaxed);
}

} // namespace stagerack
