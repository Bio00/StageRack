#pragma once

#include "Engine/AudioEngine.h"
#include "Plugins/PluginManager.h"
#include <iostream>
#include <map>

namespace stagerack
{

/** "StageRack --automix-probe <format> <id> [--set Name=Text ...] [--csv dir]"
    Black-box comparison of an automixer plugin (one linked instance per channel, e.g. QUORUM)
    with StageRack's own automixer: same signals into both, gains measured from outside. */
namespace automixprobe
{

struct Mixer
{
    virtual ~Mixer() = default;
    virtual juce::String name() const = 0;
    virtual bool setup (int numCh, double sr, int bs) = 0;
    virtual void setPriority (int ch, float db) = 0;
    virtual void process (const std::vector<const float*>& in, const std::vector<float*>& out, int n) = 0;
    virtual int latency() const { return 0; }
};

//==============================================================================
struct OurMixer final : Mixer
{
    std::unique_ptr<AudioEngine> engine;
    int numCh = 0, bs = 64;
    juce::AudioIODeviceCallbackContext ctx;

    juce::String name() const override { return "StageRack"; }

    bool setup (int n, double sr, int blockSize) override
    {
        numCh = n; bs = blockSize;
        engine = std::make_unique<AudioEngine>();
        for (int i = 0; i < n; ++i)
        {
            auto& r = engine->addRack();
            r.inputChannel = i; r.stereoInput = false;
            r.outputChannel = i; r.stereoOutput = false;
            r.automixGroup = 1;
        }
        engine->automixOverlap = overlap;
        engine->prepare (sr, bs, {});
        return true;
    }

    int overlap = 0;
    void setPriority (int ch, float db) override { engine->getRack (ch).automixWeightDb = db; }

    void process (const std::vector<const float*>& in, const std::vector<float*>& out, int n) override
    {
        engine->audioDeviceIOCallbackWithContext (in.data(), (int) in.size(), out.data(), (int) out.size(), n, ctx);
    }
};

//==============================================================================
struct PluginMixer final : Mixer
{
    juce::AudioPluginFormatManager& fm;
    juce::PluginDescription desc;
    std::map<juce::String, juce::String> settings;
    std::vector<std::unique_ptr<juce::AudioPluginInstance>> inst;
    juce::AudioBuffer<float> buf;
    juce::MidiBuffer midi;
    int linkId = 1;
    double cpuSeconds = 0.0; juce::int64 cpuBlocks = 0;

    PluginMixer (juce::AudioPluginFormatManager& f, const juce::PluginDescription& d) : fm (f), desc (d) {}

    juce::String label;
    juce::String name() const override { return label.isNotEmpty() ? label : desc.name; }

    static juce::AudioProcessorParameter* find (juce::AudioPluginInstance& p, const juce::String& name)
    {
        for (auto* prm : p.getParameters())
            if (prm->getName (64).equalsIgnoreCase (name))
                return prm;
        return nullptr;
    }

    /** Sets a parameter to the normalized value whose text matches (or whose number is closest). */
    static bool setByText (juce::AudioPluginInstance& p, const juce::String& name, const juce::String& text)
    {
        auto* prm = find (p, name);
        if (prm == nullptr) return false;

        const bool numeric = text.containsOnly ("+-.0123456789");
        float best = -1.0f, bestErr = 1.0e9f;

        for (int i = 0; i <= 2000; ++i)
        {
            const auto v = (float) i / 2000.0f;
            const auto t = prm->getText (v, 64);

            if (! numeric && t.trim().equalsIgnoreCase (text.trim())) { best = v; break; }

            if (numeric)
                if (const auto err = std::abs (t.retainCharacters ("+-.0123456789").getFloatValue() - text.getFloatValue()); err < bestErr)
                    { bestErr = err; best = v; }
        }

        if (best < 0.0f) return false;
        prm->setValueNotifyingHost (best);
        return true;
    }

    bool setup (int n, double sr, int bs) override
    {
        // a fresh link group for every test, so instances of the previous test can't take part
        inst.clear();
        ++linkId;

        for (int i = 0; i < n; ++i)
        {
            juce::String err;
            auto p = fm.createPluginInstance (desc, sr, bs, err);
            if (p == nullptr) { std::cout << "create failed: " << err << std::endl; return false; }
            Rack::configurePlugin (*p, sr, bs);

            for (auto& [k, v] : settings)
                if (! setByText (*p, k, v))
                    std::cout << "  (could not set " << k << " = " << v << ")" << std::endl;

            setByText (*p, "Link ID", juce::String (linkId));
            inst.push_back (std::move (p));
        }

        buf.setSize (2, bs);
        return true;
    }

    void setPriority (int ch, float db) override { setByText (*inst[(size_t) ch], "Priority", juce::String (db, 1)); }

    int latency() const override { return inst.empty() ? 0 : inst[0]->getLatencySamples(); }

    void process (const std::vector<const float*>& in, const std::vector<float*>& out, int n) override
    {
        const auto t0 = juce::Time::getHighResolutionTicks();

        for (size_t i = 0; i < inst.size(); ++i)
        {
            buf.copyFrom (0, 0, in[i], n);
            buf.copyFrom (1, 0, in[i], n);
            juce::AudioBuffer<float> view (buf.getArrayOfWritePointers(), 2, n);
            midi.clear();
            inst[i]->processBlock (view, midi);
            juce::FloatVectorOperations::copy (out[i], buf.getReadPointer (0), n);
        }

        cpuSeconds += juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0);
        cpuBlocks += (juce::int64) inst.size();
    }
};

//==============================================================================
/** Independent pink noise per channel, unit RMS (Kellet filter, calibrated). */
struct Pink
{
    juce::Random rnd;
    float b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0, scale = 1.0f;

    explicit Pink (int seed) : rnd (seed)
    {
        double sum = 0; const int n = 200000;
        for (int i = 0; i < n; ++i) { const auto x = raw(); sum += x * x; }
        scale = (float) (1.0 / std::sqrt (sum / n));
    }

    float raw()
    {
        const float w = rnd.nextFloat() * 2.0f - 1.0f;
        b0 = 0.99886f * b0 + w * 0.0555179f; b1 = 0.99332f * b1 + w * 0.0750759f;
        b2 = 0.96900f * b2 + w * 0.1538520f; b3 = 0.86650f * b3 + w * 0.3104856f;
        b4 = 0.55000f * b4 + w * 0.5329522f; b5 = -0.7616f * b5 - w * 0.0168980f;
        const float out = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362f;
        b6 = w * 0.115926f;
        return out;
    }

    float next() { return raw() * scale; }
};

/** What each channel plays: a level (dBFS RMS) over time, pink noise or a sine. */
struct Source
{
    std::function<float (double t)> levelDb;   // -200 = digital silence
    float sineHz = 0.0f;                       // 0 = pink noise
};

struct Run
{
    std::vector<std::vector<float>> in, out;   // per channel, whole run
};

inline Run render (Mixer& m, const std::vector<Source>& src, double seconds, double sr, int bs)
{
    const int numCh = (int) src.size();
    const int total = (int) (seconds * sr) / bs * bs;
    Run r;
    r.in.assign ((size_t) numCh, std::vector<float> ((size_t) total));
    r.out.assign ((size_t) numCh, std::vector<float> ((size_t) total));

    for (int c = 0; c < numCh; ++c)
    {
        Pink pink (100 + c);
        for (int i = 0; i < total; ++i)
        {
            const auto t = i / sr;
            const auto db = src[(size_t) c].levelDb (t);
            const auto g = db <= -199.0f ? 0.0f : juce::Decibels::decibelsToGain (db, -300.0f);
            const auto x = src[(size_t) c].sineHz > 0.0f ? (float) (std::sqrt (2.0) * std::sin (2.0 * juce::MathConstants<double>::pi * src[(size_t) c].sineHz * t))
                                                         : pink.next();
            r.in[(size_t) c][(size_t) i] = g * x;
        }
    }

    std::vector<const float*> ins ((size_t) numCh);
    std::vector<float*> outs ((size_t) numCh);

    for (int pos = 0; pos < total; pos += bs)
    {
        for (int c = 0; c < numCh; ++c)
        {
            ins[(size_t) c] = r.in[(size_t) c].data() + pos;
            outs[(size_t) c] = r.out[(size_t) c].data() + pos;
        }
        m.process (ins, outs, bs);
    }

    return r;
}

/** Gain in dB of channel c over [t0, t1): out RMS / in RMS (out shifted by the latency). */
inline float gainDb (const Run& r, int c, double t0, double t1, double sr, int lat)
{
    const auto a = (size_t) (t0 * sr), b = juce::jmin ((size_t) (t1 * sr), r.in[(size_t) c].size() - (size_t) lat);
    double si = 0, so = 0;
    for (size_t i = a; i < b; ++i)
    {
        si += (double) r.in[(size_t) c][i] * r.in[(size_t) c][i];
        so += (double) r.out[(size_t) c][i + (size_t) lat] * r.out[(size_t) c][i + (size_t) lat];
    }
    return si < 1.0e-30 ? -999.0f : (float) (10.0 * std::log10 (juce::jmax (1.0e-30, so) / si));
}

inline juce::String fmt (float db)
{
    return db <= -998.0f ? juce::String ("   n/a") : juce::String (db, 1).paddedLeft (' ', 6);
}

inline int measureLatency (const Run& r, int c)
{
    int best = 0; double bestCorr = -1;
    const size_t n = r.in[(size_t) c].size();
    for (int lag = 0; lag < 4096; ++lag)
    {
        double s = 0;
        for (size_t i = n / 2; i + (size_t) lag < n && i < n / 2 + 24000; ++i)
            s += (double) r.in[(size_t) c][i] * r.out[(size_t) c][i + (size_t) lag];
        if (s > bestCorr) { bestCorr = s; best = lag; }
    }
    return best;
}

/** Time (ms after `from`) when the gain curve of channel c crosses `targetFrac` of the way
    from its value before `from` to its final value. Curve in 1 ms windows. */
inline double crossingMs (const Run& r, int c, double from, double to, double sr, int lat, float frac)
{
    const auto before = gainDb (r, c, from - 0.3, from - 0.01, sr, lat);
    const auto after  = gainDb (r, c, to - 0.3, to - 0.01, sr, lat);
    const auto target = before + frac * (after - before);

    for (double t = from; t < to; t += 0.001)
    {
        const auto g = gainDb (r, c, t, t + 0.002, sr, lat);
        if ((after > before && g >= target) || (after < before && g <= target))
            return (t - from) * 1000.0;
    }
    return -1.0;
}

//==============================================================================
inline void runSuite (Mixer& m, double sr, int bs, const juce::File& csvDir)
{
    auto C = [] (float db) { return Source { [db] (double) { return db; } }; };
    std::cout << "\n=== " << m.name() << " ===" << std::endl;

    // 1. static law (pink noise, independent per mic; "floor" = -60 dBFS room noise)
    struct Case { const char* what; std::vector<float> levels; };
    const std::vector<Case> cases = {
        { "1 talks (-20), 1 floor (-60)        ", { -20, -60 } },
        { "1 talks (-20), 1 digital silence    ", { -20, -200 } },
        { "2 equal (-20,-20)                   ", { -20, -20 } },
        { "2 talk, one 6 dB louder (-14,-20)   ", { -14, -20 } },
        { "2 talk, one 20 dB louder (-10,-30)  ", { -10, -30 } },
        { "4 equal (-20 x4)                    ", { -20, -20, -20, -20 } },
        { "1 talks + 3 floor (-20,-60,-60,-60) ", { -20, -60, -60, -60 } },
        { "4 floor only (-60 x4)               ", { -60, -60, -60, -60 } },
        { "8 equal (-20 x8)                    ", { -20, -20, -20, -20, -20, -20, -20, -20 } },
    };

    std::cout << "\n-- static gains (dB), steady state --" << std::endl;
    for (auto& cs : cases)
    {
        std::vector<Source> src;
        for (auto l : cs.levels) src.push_back (C (l));
        m.setup ((int) src.size(), sr, bs);
        const auto lat = m.latency();
        auto r = render (m, src, 3.0, sr, bs);
        juce::String line = juce::String (cs.what) + ": ";
        double sumPow = 0;
        for (int c = 0; c < (int) src.size(); ++c)
        {
            const auto g = gainDb (r, c, 2.0, 2.9, sr, lat);
            line << fmt (g);
            if (g > -900) sumPow += std::pow (10.0, g / 10.0);
        }
        line << "   | sum of gains^2 = " << juce::String (10.0 * std::log10 (juce::jmax (1e-12, sumPow)), 1) << " dB";
        std::cout << line << std::endl;
    }

    // 2. priority / weight
    {
        m.setup (2, sr, bs);
        m.setPriority (0, 6.0f);
        auto r = render (m, { C (-20), C (-20) }, 3.0, sr, bs);
        std::cout << "\nPriority +6 dB on mic 1, both -20      : " << fmt (gainDb (r, 0, 2, 2.9, sr, m.latency())) << fmt (gainDb (r, 1, 2, 2.9, sr, m.latency())) << std::endl;
    }

    // 3. latency
    {
        m.setup (2, sr, bs);
        auto r = render (m, { C (-20), C (-60) }, 2.0, sr, bs);
        std::cout << "\nLatency: reported " << m.latency() << " samples, measured (x-corr) " << measureLatency (r, 0) << " samples" << std::endl;
    }

    // 4. dynamics: mic 2 starts talking at 1.0 s, stops at 2.5 s; mic 1 talks all the time
    {
        m.setup (2, sr, bs);
        const auto lat = m.latency();
        Source talker1 = C (-20);
        Source talker2 { [] (double t) { return (t >= 1.0 && t < 2.5) ? -20.0f : -60.0f; } };
        auto r = render (m, { talker1, talker2 }, 4.5, sr, bs);

        std::cout << "\n-- dynamics: mic 2 floor -> talks (-20) at 1.0 s -> floor at 2.5 s, mic 1 talks all the time --" << std::endl;
        std::cout << "mic 2 opening: 50% in " << juce::String (crossingMs (r, 1, 1.0, 2.5, sr, lat, 0.5), 1)
                  << " ms, 90% in " << juce::String (crossingMs (r, 1, 1.0, 2.5, sr, lat, 0.9), 1) << " ms" << std::endl;
        std::cout << "mic 2 gain in the first 5/10/20/50 ms of its first word: "
                  << fmt (gainDb (r, 1, 1.000, 1.005, sr, lat)) << fmt (gainDb (r, 1, 1.005, 1.010, sr, lat))
                  << fmt (gainDb (r, 1, 1.010, 1.020, sr, lat)) << fmt (gainDb (r, 1, 1.020, 1.050, sr, lat)) << "  (steady " << fmt (gainDb (r, 1, 2.2, 2.49, sr, lat)) << ")" << std::endl;
        std::cout << "mic 1 ducking when mic 2 starts: 50% in " << juce::String (crossingMs (r, 0, 1.0, 2.5, sr, lat, 0.5), 1)
                  << " ms, 90% in " << juce::String (crossingMs (r, 0, 1.0, 2.5, sr, lat, 0.9), 1) << " ms" << std::endl;
        std::cout << "mic 2 closing after it stops: 50% in " << juce::String (crossingMs (r, 1, 2.5, 4.5, sr, lat, 0.5), 1)
                  << " ms, 90% in " << juce::String (crossingMs (r, 1, 2.5, 4.5, sr, lat, 0.9), 1) << " ms" << std::endl;
        std::cout << "mic 1 recovering: 50% in " << juce::String (crossingMs (r, 0, 2.5, 4.5, sr, lat, 0.5), 1)
                  << " ms, 90% in " << juce::String (crossingMs (r, 0, 2.5, 4.5, sr, lat, 0.9), 1) << " ms" << std::endl;

        if (csvDir.isDirectory())
        {
            juce::String csv = "t_ms,mic1_dB,mic2_dB\n";
            for (double t = 0.8; t < 3.2; t += 0.002)
                csv << juce::String (t * 1000.0, 0) << "," << gainDb (r, 0, t, t + 0.002, sr, lat) << "," << gainDb (r, 1, t, t + 0.002, sr, lat) << "\n";
            csvDir.getChildFile ("dyn-" + m.name().retainCharacters ("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz") + ".csv").replaceWithText (csv);
        }
    }

    // 5. what the detector listens to: same RMS, different frequencies
    {
        std::cout << "\n-- detector band: two mics, same RMS (-20), different content --" << std::endl;
        for (auto pair : std::vector<std::pair<float, float>> { { 1000, 80 }, { 1000, 50 }, { 1000, 8000 }, { 1000, 200 } })
        {
            m.setup (2, sr, bs);
            Source a = C (-20); a.sineHz = pair.first;
            Source b = C (-20); b.sineHz = pair.second;
            auto r = render (m, { a, b }, 3.0, sr, bs);
            std::cout << juce::String (pair.first, 0) << " Hz vs " << juce::String (pair.second, 0).paddedLeft (' ', 4) << " Hz: "
                      << fmt (gainDb (r, 0, 2, 2.9, sr, m.latency())) << fmt (gainDb (r, 1, 2, 2.9, sr, m.latency())) << std::endl;
        }
    }

    // 6. gain modulation on low notes: one mic alone with a 100 Hz sine vs a talker
    {
        m.setup (2, sr, bs);
        Source a = C (-20); a.sineHz = 100.0f;
        auto r = render (m, { a, C (-20) }, 3.0, sr, bs);
        float lo = 999, hi = -999;
        for (double t = 2.0; t < 2.9; t += 0.0005)   // 0.5 ms windows: 1/20 of a period
        {
            // instantaneous gain from the envelope: |out| / |in| on samples with enough signal
            const auto i = (size_t) (t * sr);
            const auto x = r.in[0][i];
            if (std::abs (x) > 0.05f)
            {
                const auto g = juce::Decibels::gainToDecibels (std::abs (r.out[0][i + (size_t) m.latency()] / x));
                lo = juce::jmin (lo, g); hi = juce::jmax (hi, g);
            }
        }
        std::cout << "\nGain ripple on a 100 Hz tone (vs a pink talker): " << juce::String (hi - lo, 2) << " dB p-p (mean "
                  << fmt (gainDb (r, 0, 2, 2.9, sr, m.latency())) << " dB)" << std::endl;
    }
}

//==============================================================================
/** A panel discussion built from speech files: 4 mics, each = its talker + the other talkers
    bleeding in at -15 dB (3-9 ms later) + room noise. Components are kept apart, so what the
    automixer lets through of each can be measured exactly (the automixer is a gain per mic). */
struct Scene
{
    static constexpr int numCh = 4;
    std::vector<std::vector<float>> direct, other;   // per mic: own talker / bleed + noise
    struct Utt { int talker, start, end; juce::String file; };
    std::vector<Utt> utts;
    int total = 0;

    bool load (const juce::File& dir, double sr)
    {
        const float talkerDb[numCh] = { -24.0f, -26.0f, -28.0f, -31.0f };   // D is a soft voice
        juce::WavAudioFormat wav;
        std::vector<std::vector<float>> voice ((size_t) numCh);
        juce::StringArray lines;
        lines.addLines (dir.getChildFile ("schedule.txt").loadFileAsString());

        struct Clip { int talker, start; std::vector<float> x; juce::String file; };
        std::vector<Clip> clips;

        for (auto& l : lines)
        {
            auto t = juce::StringArray::fromTokens (l, " ", "");
            if (t.size() < 3) continue;
            auto f = dir.getChildFile (t[2] + ".wav");
            std::unique_ptr<juce::AudioFormatReader> rd (wav.createReaderFor (f.createInputStream().release(), true));
            if (rd == nullptr) { std::cout << "cannot read " << f.getFullPathName() << std::endl; return false; }
            juce::AudioBuffer<float> b (1, (int) rd->lengthInSamples);
            rd->read (&b, 0, (int) rd->lengthInSamples, 0, true, false);

            const auto talker = t[1].getIntValue();
            double e = 0; int n = 0;   // RMS over the voiced part only
            for (int i = 0; i < b.getNumSamples(); ++i)
                if (std::abs (b.getSample (0, i)) > 0.01f) { e += b.getSample (0, i) * b.getSample (0, i); ++n; }
            const auto g = juce::Decibels::decibelsToGain (talkerDb[talker]) / (float) std::sqrt (e / juce::jmax (1, n));

            Clip c { talker, (int) (t[0].getDoubleValue() * sr), {}, t[2] };
            c.x.resize ((size_t) b.getNumSamples());
            for (int i = 0; i < b.getNumSamples(); ++i) c.x[(size_t) i] = b.getSample (0, i) * g;

            // the utterance: from the first to the last audible sample
            int first = 0, last = (int) c.x.size() - 1;
            while (first < last && std::abs (c.x[(size_t) first]) < 1.0e-3f) ++first;
            while (last > first && std::abs (c.x[(size_t) last]) < 1.0e-3f) --last;
            utts.push_back ({ talker, c.start + first, c.start + last, t[2] });

            total = juce::jmax (total, c.start + (int) c.x.size() + (int) (1.5 * sr));
            clips.push_back (std::move (c));
        }

        for (auto& v : voice) v.assign ((size_t) total, 0.0f);
        for (auto& c : clips)
            for (size_t i = 0; i < c.x.size(); ++i)
                voice[(size_t) c.talker][(size_t) c.start + i] += c.x[i];

        direct = voice;
        other.assign ((size_t) numCh, std::vector<float> ((size_t) total, 0.0f));
        const auto bleed = juce::Decibels::decibelsToGain (-15.0f);
        Pink hvac (999);
        std::vector<float> common ((size_t) total);
        for (auto& x : common) x = hvac.next() * juce::Decibels::decibelsToGain (-64.0f);

        for (int m = 0; m < numCh; ++m)
        {
            Pink own (500 + m);
            for (int i = 0; i < total; ++i)
            {
                float x = own.next() * juce::Decibels::decibelsToGain (-62.0f) + common[(size_t) i];
                for (int j = 0; j < numCh; ++j)
                {
                    if (j == m) continue;
                    const int d = (int) ((3 + 2 * std::abs (m - j)) * sr / 1000.0);
                    if (i >= d) x += bleed * voice[(size_t) j][(size_t) (i - d)];
                }
                other[(size_t) m][(size_t) i] = x;
            }
        }
        return true;
    }
};

inline void runSpeech (Mixer& m, const Scene& sc, double sr, int bs, const juce::File& outDir)
{
    const int numCh = Scene::numCh;
    const int total = sc.total / bs * bs;
    std::vector<std::vector<float>> in ((size_t) numCh, std::vector<float> ((size_t) total)), out = in;

    for (int c = 0; c < numCh; ++c)
        for (int i = 0; i < total; ++i)
            in[(size_t) c][(size_t) i] = sc.direct[(size_t) c][(size_t) i] + sc.other[(size_t) c][(size_t) i];

    m.setup (numCh, sr, bs);
    const int lat = m.latency();
    std::vector<const float*> ins ((size_t) numCh);
    std::vector<float*> outs ((size_t) numCh);

    for (int pos = 0; pos < total; pos += bs)
    {
        for (int c = 0; c < numCh; ++c) { ins[(size_t) c] = in[(size_t) c].data() + pos; outs[(size_t) c] = out[(size_t) c].data() + pos; }
        m.process (ins, outs, bs);
    }

    // gain per mic in 2 ms windows (the automixer is a gain: out = g * in)
    const int W = 96, numW = (total - lat) / W;
    std::vector<std::vector<float>> g ((size_t) numCh, std::vector<float> ((size_t) numW));
    for (int c = 0; c < numCh; ++c)
        for (int w = 0; w < numW; ++w)
        {
            double si = 0, so = 0;
            for (int i = w * W; i < (w + 1) * W; ++i)
            {
                si += (double) in[(size_t) c][(size_t) i] * in[(size_t) c][(size_t) i];
                so += (double) out[(size_t) c][(size_t) (i + lat)] * out[(size_t) c][(size_t) (i + lat)];
            }
            g[(size_t) c][(size_t) w] = (float) std::sqrt (so / juce::jmax (1.0e-30, si));
        }

    auto energy = [&] (const std::vector<float>& x, int w) { double e = 0; for (int i = w * W; i < (w + 1) * W; ++i) e += (double) x[(size_t) i] * x[(size_t) i]; return e; };
    auto db = [] (double r) { return juce::String (10.0 * std::log10 (juce::jmax (1.0e-30, r)), 1); };

    // 1. speech through + clipped speech: windows where a talker is audible on its own mic
    double dIn = 0, dOut = 0, oIn = 0, oOut = 0; int active = 0, clipped6 = 0, clipped3 = 0;
    int soloActive = 0, soloClipped3 = 0, soloClipped6 = 0;
    auto talkersAt = [&] (int sample) { int k = 0; for (auto& u : sc.utts) if (sample >= u.start - (int) (0.05 * sr) && sample < u.end + (int) (0.05 * sr)) ++k; return k; };
    for (int c = 0; c < numCh; ++c)
        for (int w = 0; w < numW; ++w)
        {
            const auto ed = energy (sc.direct[(size_t) c], w);
            const auto eo = energy (sc.other[(size_t) c], w);
            const auto gg = (double) g[(size_t) c][(size_t) w] * g[(size_t) c][(size_t) w];
            dIn += ed; dOut += gg * ed; oIn += eo; oOut += gg * eo;
            if (ed / W > std::pow (10.0, -45.0 / 10.0))   // talker audible (> -45 dBFS)
            {
                ++active;
                if (gg < std::pow (10.0, -6.0 / 10.0)) ++clipped6;
                if (gg < std::pow (10.0, -3.0 / 10.0)) ++clipped3;

                if (talkersAt (w * W) == 1)
                {
                    ++soloActive;
                    if (gg < std::pow (10.0, -3.0 / 10.0)) ++soloClipped3;
                    if (gg < std::pow (10.0, -6.0 / 10.0)) ++soloClipped6;
                }
            }
        }

    std::cout << "\n=== " << m.name() << " (speech scene, " << juce::String (total / sr, 1) << " s) ===" << std::endl;
    std::cout << "speech of the talkers through (energy) : " << db (dOut / dIn) << " dB   (0 = nothing lost)" << std::endl;
    std::cout << "talker windows below -3 / -6 dB        : " << juce::String (100.0 * clipped3 / juce::jmax (1, active), 1) << " % / "
              << juce::String (100.0 * clipped6 / juce::jmax (1, active), 1) << " %   (words attenuated)" << std::endl;
    std::cout << "  only one person talking: below -3 / -6 dB " << juce::String (100.0 * soloClipped3 / juce::jmax (1, soloActive), 1) << " % / "
              << juce::String (100.0 * soloClipped6 / juce::jmax (1, soloActive), 1) << " %   (" << juce::String (100.0 * soloActive / juce::jmax (1, active), 0) << " % of the speech)" << std::endl;
    std::cout << "bleed + noise through (whole scene)    : " << db (oOut / oIn) << " dB   (vs all mics open)" << std::endl;
    std::cout << "speech / (bleed+noise) improvement     : " << db ((dOut / dIn) / (oOut / oIn)) << " dB" << std::endl;

    // 2. onsets: gain of the talker's mic at the start of each utterance
    juce::String on20, on50; double s20 = 0, s50 = 0, s150 = 0; int n = 0;
    for (auto& u : sc.utts)
    {
        auto avg = [&] (int a, int b) { double e = 0; int k = 0; for (int w = (u.start + a) / W; w < (u.start + b) / W && w < numW; ++w) { e += 20.0 * std::log10 (juce::jmax (1.0e-6f, g[(size_t) u.talker][(size_t) w])); ++k; } return e / juce::jmax (1, k); };
        const auto a = avg (0, (int) (0.020 * sr)), b = avg ((int) (0.020 * sr), (int) (0.050 * sr)), c = avg ((int) (0.050 * sr), (int) (0.150 * sr));
        s20 += a; s50 += b; s150 += c; ++n;
        on20 << " " << u.file << ":" << juce::String (a, 0);
    }
    std::cout << "onset gain, avg over " << n << " utterances  : 0-20 ms " << juce::String (s20 / n, 1) << " dB, 20-50 ms " << juce::String (s50 / n, 1)
              << " dB, 50-150 ms " << juce::String (s150 / n, 1) << " dB" << std::endl;
    std::cout << "  first 20 ms per utterance:" << on20 << std::endl;

    // 3. quiet gap (nobody talks): total noise vs all open
    {
        const int a = (int) (31.2 * sr) / W, b = (int) (32.8 * sr) / W;
        double ei = 0, eo = 0;
        juce::String gains;
        for (int c = 0; c < numCh; ++c)
        {
            double gc = 0;
            for (int w = a; w < b; ++w) { ei += energy (in[(size_t) c], w); eo += (double) g[(size_t) c][(size_t) w] * g[(size_t) c][(size_t) w] * energy (in[(size_t) c], w); gc += 20.0 * std::log10 (juce::jmax (1.0e-6f, g[(size_t) c][(size_t) w])); }
            gains << juce::String (gc / (b - a), 1) << " ";
        }
        std::cout << "pause 31.2-32.8 s: room noise " << db (eo / ei) << " dB vs all open; mic gains " << gains << "(last talker: mic 2)" << std::endl;
    }

    // 4. two people at once (A and C, 23.6-24.9 s)
    {
        const int a = (int) (23.7 * sr) / W, b = (int) (24.9 * sr) / W;
        juce::String gains;
        for (int c = 0; c < numCh; ++c)
        {
            double gc = 0;
            for (int w = a; w < b; ++w) gc += 20.0 * std::log10 (juce::jmax (1.0e-6f, g[(size_t) c][(size_t) w]));
            gains << juce::String (gc / (b - a), 1) << " ";
        }
        std::cout << "A and C together (23.7-24.9 s): mic gains " << gains << "(A, B, C, D)" << std::endl;
    }

    // 5. a mic that is not talking: how far down, and how quiet (gain while someone else talks)
    {
        double sum = 0; int k = 0;
        for (int c = 0; c < numCh; ++c)
            for (int w = 0; w < numW; ++w)
            {
                bool own = false, anyone = false;
                const int t = w * W;
                for (auto& u : sc.utts)
                    if (t >= u.start && t < u.end) { anyone = true; own = own || u.talker == c; }
                if (anyone && ! own) { sum += 20.0 * std::log10 (juce::jmax (1.0e-6f, g[(size_t) c][(size_t) w])); ++k; }
            }
        std::cout << "non-talking mics while others talk     : " << juce::String (sum / juce::jmax (1, k), 1) << " dB average" << std::endl;
    }

    if (outDir.isDirectory())
    {
        auto writeWav = [&] (const juce::String& fname, const std::function<float (int)>& sample)
        {
            juce::AudioBuffer<float> b (1, total - lat);
            for (int i = 0; i < total - lat; ++i) b.setSample (0, i, sample (i));
            juce::WavAudioFormat wav;
            auto f = outDir.getChildFile (fname);
            f.deleteFile();
            std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (f), sr, 1, 24, {}, 0));
            if (w != nullptr) w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
        };
        const auto tag = m.name().retainCharacters ("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-");
        writeWav ("mix-" + tag + ".wav", [&] (int i) { float s = 0; for (int c = 0; c < numCh; ++c) s += out[(size_t) c][(size_t) (i + lat)]; return s; });
        writeWav ("mix-all-open.wav", [&] (int i) { float s = 0; for (int c = 0; c < numCh; ++c) s += in[(size_t) c][(size_t) i]; return s; });

        juce::String csv = "t_s";
        for (int c = 0; c < numCh; ++c) csv << ",g" << c + 1;
        csv << "\n";
        for (int w = 0; w < numW; w += 5)
        {
            csv << juce::String (w * W / sr, 3);
            for (int c = 0; c < numCh; ++c) csv << "," << juce::String (20.0 * std::log10 (juce::jmax (1.0e-6f, g[(size_t) c][(size_t) w])), 2);
            csv << "\n";
        }
        outDir.getChildFile ("gains-" + tag + ".csv").replaceWithText (csv);
    }
}


//==============================================================================
/** Output recorded elsewhere (e.g. through another host via a loopback device), already aligned
    with the scene: plays it back so runSpeech() can measure it like any other mixer. */
struct RecordedMixer final : Mixer
{
    juce::String label;
    std::vector<std::vector<float>> rec;
    int pos = 0;

    juce::String name() const override { return label; }
    bool setup (int, double, int) override { pos = 0; return true; }
    void setPriority (int, float) override {}

    void process (const std::vector<const float*>&, const std::vector<float*>& out, int n) override
    {
        for (size_t c = 0; c < out.size(); ++c)
            for (int i = 0; i < n; ++i)
                out[c][i] = c < rec.size() && (size_t) (pos + i) < rec[c].size() ? rec[c][(size_t) (pos + i)] : 0.0f;
        pos += n;
    }
};

/** Plays the scene's 4 mics on outputs outBase.. of a loopback device (twice: the first pass warms
    the other host's automixer up), records the same channels back (reference) and the other host's
    outputs on inBase.., and returns the second pass aligned sample by sample with the scene. */
inline bool recordThroughLoopback (const Scene& sc, const juce::String& deviceName, int outBase, int inBase,
                                   double sr, int bs, std::vector<std::vector<float>>& aligned)
{
    struct Cb final : juce::AudioIODeviceCallback
    {
        const Scene* sc = nullptr;
        int outBase = 0, inBase = 0, total = 0, length = 0;
        std::atomic<int> pos { 0 };
        std::vector<std::vector<float>> ref, out;

        void audioDeviceIOCallbackWithContext (const float* const* ins, int numIns, float* const* outs, int numOuts,
                                               int n, const juce::AudioIODeviceCallbackContext&) override
        {
            for (int c = 0; c < numOuts; ++c)
                if (outs[c] != nullptr) juce::FloatVectorOperations::clear (outs[c], n);

            const int p = pos.load();
            for (int i = 0; i < n && p + i < length; ++i)
            {
                const int idx = p + i, s = idx % total;
                for (int c = 0; c < Scene::numCh; ++c)
                {
                    if (outBase + c < numOuts && outs[outBase + c] != nullptr && idx < 2 * total)
                        outs[outBase + c][i] = sc->direct[(size_t) c][(size_t) s] + sc->other[(size_t) c][(size_t) s];
                    if (inBase + c < numIns && ins[inBase + c] != nullptr)   out[(size_t) c][(size_t) idx] = ins[inBase + c][i];
                    if (outBase + c < numIns && ins[outBase + c] != nullptr) ref[(size_t) c][(size_t) idx] = ins[outBase + c][i];
                }
            }
            pos = p + n;
        }
        void audioDeviceAboutToStart (juce::AudioIODevice*) override {}
        void audioDeviceStopped() override {}
    } cb;

    cb.sc = &sc; cb.outBase = outBase; cb.inBase = inBase;
    cb.total = sc.total;
    cb.length = 2 * sc.total + (int) sr;   // + 1 s for the latencies
    cb.ref.assign (Scene::numCh, std::vector<float> ((size_t) cb.length));
    cb.out = cb.ref;

    juce::AudioDeviceManager dm;
    auto err = dm.initialise (32, 32, nullptr, false, deviceName);
    auto setup = dm.getAudioDeviceSetup();
    setup.inputDeviceName = setup.outputDeviceName = deviceName;
    setup.sampleRate = sr; setup.bufferSize = bs;
    setup.useDefaultInputChannels = setup.useDefaultOutputChannels = false;
    setup.inputChannels.setRange (0, 32, true);
    setup.outputChannels.setRange (0, 32, true);
    if (err.isEmpty()) err = dm.setAudioDeviceSetup (setup, true);

    auto* dev = dm.getCurrentAudioDevice();
    if (err.isNotEmpty() || dev == nullptr || dev->getName() != deviceName)
    {
        std::cout << "cannot open " << deviceName << ": " << err << std::endl;
        return false;
    }

    std::cout << "recording through " << dev->getName() << " (" << dev->getCurrentSampleRate() << " Hz, "
              << dev->getCurrentBufferSizeSamples() << " smp): " << juce::String (cb.length / sr, 0) << " s..." << std::endl;
    dm.addAudioCallback (&cb);
    while (cb.pos.load() < cb.length)
        juce::Thread::sleep (100);
    dm.removeAudioCallback (&cb);
    dm.closeAudioDevice();

    // latency of the loopback (scene -> reference) and of the other host (reference -> its output)
    auto lagOf = [] (const std::vector<float>& a, const std::vector<float>& b, int from, int maxLag)
    {
        int best = 0; double bestC = -1.0e30;
        for (int lag = 0; lag < maxLag; ++lag)
        {
            double c = 0;
            for (int i = from; i < from + 96000 && (size_t) (i + lag) < b.size() && (size_t) i < a.size(); ++i)
                c += (double) a[(size_t) i] * b[(size_t) (i + lag)];
            if (c > bestC) { bestC = c; best = lag; }
        }
        return best;
    };

    std::vector<float> mic0 ((size_t) cb.length, 0.0f);
    for (int i = 0; i < cb.length && i < 2 * sc.total; ++i)
        mic0[(size_t) i] = sc.direct[0][(size_t) (i % sc.total)] + sc.other[0][(size_t) (i % sc.total)];

    const int from = sc.total + (int) (0.5 * sr);   // second pass, first talker (mic 1)
    const int l1 = lagOf (mic0, cb.ref[0], from, 24000);
    const int l2 = lagOf (cb.ref[0], cb.out[0], from + l1, 8192);

    double eRef = 0, eErr = 0;
    for (int i = sc.total; i < 2 * sc.total; ++i)
    {
        const auto x = mic0[(size_t) i], y = cb.ref[0][(size_t) (i + l1)];
        eRef += (double) x * x; eErr += (double) (y - x) * (y - x);
    }
    std::cout << "loopback latency " << l1 << " smp (reference error " << juce::String (10.0 * std::log10 (juce::jmax (1.0e-30, eErr / eRef)), 1)
              << " dB), other host's latency " << l2 << " smp" << std::endl;

    aligned.assign (Scene::numCh, std::vector<float> ((size_t) sc.total));
    for (int c = 0; c < Scene::numCh; ++c)
        for (int i = 0; i < sc.total; ++i)
            aligned[(size_t) c][(size_t) i] = cb.out[(size_t) c][(size_t) (sc.total + i + l1 + l2)];
    return true;
}

inline int run (const juce::StringArray& args)
{
    const auto idx = args.indexOf ("--automix-probe");
    const double sr = 48000.0;

    // QUORUM links its instances through a shared-memory bus that spans processes, and
    // instances of a process that exits without deleting them stay on it as "ghost" mics
    // (measured: every run got worse). A private bus per run keeps the measurements clean.
    if (! args.contains ("--shared-bus"))
        setenv ("QUORUM_SHARED_BUS_NAMESPACE", ("stagerack-probe-" + juce::String (juce::Time::currentTimeMillis())).toRawUTF8(), 1);
    const int bs = args.contains ("--block") ? args[args.indexOf ("--block") + 1].getIntValue() : 64;
    juce::File csvDir;
    if (args.contains ("--csv")) csvDir = juce::File (args[args.indexOf ("--csv") + 1]);

    juce::AudioPluginFormatManager fm;
    PluginManager::addAllFormats (fm);
    juce::OwnedArray<juce::PluginDescription> found;
    for (auto* f : fm.getFormats())
        if (f->getName() == args[idx + 1])
            f->findAllTypesForFile (found, args[idx + 2]);

    if (found.isEmpty()) { std::cout << "plugin not found" << std::endl; return 1; }

    PluginMixer pm (fm, *found[0]);
    pm.settings["Clean DNS"] = "Off";
    for (int i = 0; i < args.size(); ++i)
        if (args[i] == "--set")
            pm.settings[args[i + 1].upToFirstOccurrenceOf ("=", false, false)] = args[i + 1].fromFirstOccurrenceOf ("=", false, false);

    // parameter map: every distinct text of each parameter
    {
        juce::String err;
        auto p = fm.createPluginInstance (*found[0], sr, bs, err);
        std::cout << found[0]->name << " " << found[0]->version << " (" << found[0]->pluginFormatName << "), latency "
                  << p->getLatencySamples() << ", buses in " << p->getTotalNumInputChannels() << " out " << p->getTotalNumOutputChannels() << std::endl;
        for (auto* prm : p->getParameters())
        {
            juce::StringArray texts;
            for (int i = 0; i <= 200; ++i)
                texts.addIfNotAlreadyThere (prm->getText ((float) i / 200.0f, 64));
            std::cout << "  " << prm->getName (64).paddedRight (' ', 14) << " default " << prm->getCurrentValueAsText().paddedRight (' ', 10)
                      << (texts.size() <= 12 ? texts.joinIntoString (" | ") : texts[0] + " ... " + texts[texts.size() - 1] + "  (" + juce::String (texts.size()) + " values)")
                      << std::endl;
        }
        p.release();   // keep alive (some plugins dislike being destroyed outside the message loop)
    }

    std::cout << "\nSettings:";
    for (auto& [k, v] : pm.settings) std::cout << " " << k << "=" << v;
    std::cout << "   (block " << bs << " @ 48 kHz)" << std::endl;

    if (args.contains ("--am"))   // avgMs,holdMs,releaseDbPerSec,hpfHz,lpfHz
    {
        auto v = juce::StringArray::fromTokens (args[args.indexOf ("--am") + 1], ",", "");
        Rack::automixAvgMs = v[0].getFloatValue(); Rack::automixHoldMs = v[1].getFloatValue();
        Rack::automixReleaseDbPerSec = v[2].getFloatValue(); Rack::automixHighPassHz = v[3].getFloatValue();
        Rack::automixLowPassHz = v[4].getFloatValue();
    }

    if (args.contains ("--name"))
        pm.label = args[args.indexOf ("--name") + 1];

    if (args.contains ("--speech"))
    {
        Scene sc;
        if (! sc.load (juce::File (args[args.indexOf ("--speech") + 1]), sr)) return 1;

        // --loopback "<device>" <firstOut> <firstIn>: measure another host's automixer (e.g. Dugan
        // Speech in SuperRack) that reads the 4 mics from firstOut.. and plays them back on firstIn..
        if (args.contains ("--loopback"))
        {
            const auto li = args.indexOf ("--loopback");
            RecordedMixer rm;
            rm.label = args.contains ("--name") ? args[args.indexOf ("--name") + 1] : juce::String ("Loopback");
            if (! recordThroughLoopback (sc, args[li + 1], args[li + 2].getIntValue() - 1, args[li + 3].getIntValue() - 1, sr, bs, rm.rec))
                return 1;
            runSpeech (rm, sc, sr, bs, csvDir);
            std::fflush (nullptr);
            std::_Exit (0);
        }

        if (! args.contains ("--plugin-only"))
        {
            OurMixer ours;
            if (args.contains ("--overlap")) ours.overlap = args[args.indexOf ("--overlap") + 1].getIntValue();
            runSpeech (ours, sc, sr, bs, csvDir);
        }
        if (! args.contains ("--ours-only")) runSpeech (pm, sc, sr, bs, csvDir);
    }
    else
    {
        if (! args.contains ("--plugin-only")) { OurMixer ours; runSuite (ours, sr, bs, csvDir); }
        if (! args.contains ("--ours-only")) runSuite (pm, sr, bs, csvDir);
    }

    if (pm.cpuBlocks > 0)
        std::cout << "\nCPU " << pm.name() << ": " << juce::String (1.0e6 * pm.cpuSeconds / (double) pm.cpuBlocks, 2)
                  << " us per instance per block (avg over the suite, " << bs << " smp)" << std::endl;

    for (auto& i : pm.inst) i.release();
    std::fflush (nullptr);
    std::_Exit (0);
}

} // namespace automixprobe
} // namespace stagerack
