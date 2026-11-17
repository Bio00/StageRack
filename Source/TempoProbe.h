#pragma once

#include "PluginProbe.h"

namespace stagerack
{

/** "StageRack --tempo-probe <format> <id> [--set Name=Value]... [--params] [--bpm a,b,...]"
    End-to-end check of the host tempo: a real plugin in a rack of a (device-less) engine, an
    impulse in, the time of its first echo out at each master tempo. A tempo-synced delay must
    move its echo with the tempo (1/4 at 100 BPM = 600 ms, at 150 = 400 ms). */
inline int runTempoProbe (const juce::StringArray& args)
{
    const auto idx = args.indexOf ("--tempo-probe");
    const auto formatName = args[idx + 1];
    const auto id = args[idx + 2];
    const int bs = 256;
    const double sr = 48000.0;

    juce::AudioPluginFormatManager fm;
    PluginManager::addAllFormats (fm);
    juce::OwnedArray<juce::PluginDescription> found;
    for (auto* f : fm.getFormats())
        if (f->getName() == formatName)
            f->findAllTypesForFile (found, id);
    if (found.isEmpty()) { std::cout << "plugin not found" << std::endl; return 1; }

    juce::String err;
    auto p = fm.createPluginInstance (*found[0], sr, bs, err);
    if (p == nullptr) { std::cout << "create failed: " << err << std::endl; return 1; }

    auto* plugin = p.get();
    AudioEngine e;
    auto& r = e.addRack();
    Rack::configurePlugin (*p, sr, bs);
    p->prepareToPlay (sr, bs);
    r.setPlugin (0, std::move (p));
    e.prepare (sr, bs, {});
    applySetArgs (*plugin, args);

    if (args.contains ("--params"))
        for (auto* prm : plugin->getParameters())
            std::cout << "  [" << prm->getParameterIndex() << "] " << prm->getName (64) << " = " << prm->getCurrentValueAsText() << std::endl;

    juce::Array<double> tempos { 100.0, 150.0 };
    if (const auto bi = args.indexOf ("--bpm"); bi >= 0)
    {
        tempos.clear();
        for (auto& t : juce::StringArray::fromTokens (args[bi + 1], ",", {}))
            tempos.add (t.getDoubleValue());
    }

    juce::AudioBuffer<float> in (2, bs), out (2, bs);
    juce::AudioIODeviceCallbackContext ctx;
    std::cout << found[0]->name << " (" << formatName << ")" << std::endl;

    for (auto bpm : tempos)
    {
        e.tempo.setBpm (0, bpm);

        // flush the previous echoes
        in.clear();
        for (int k = 0; k < (int) (6.0 * sr / bs); ++k)
            e.audioDeviceIOCallbackWithContext (in.getArrayOfReadPointers(), 2, out.getArrayOfWritePointers(), 2, bs, ctx);

        std::vector<float> y;
        for (int k = 0; k < (int) (2.5 * sr / bs); ++k)
        {
            in.clear();
            if (k == 0) { in.setSample (0, 0, 1.0f); in.setSample (1, 0, 1.0f); }
            e.audioDeviceIOCallbackWithContext (in.getArrayOfReadPointers(), 2, out.getArrayOfWritePointers(), 2, bs, ctx);
            for (int s = 0; s < bs; ++s)
                y.push_back (std::abs (out.getSample (0, s)) + std::abs (out.getSample (1, s)));
        }

        // first echo: the loudest peak after the dry part (> 30 ms), then its onset
        size_t first = (size_t) (0.03 * sr);
        float peak = 0.0f;
        for (size_t i = first; i < y.size(); ++i) peak = juce::jmax (peak, y[i]);
        size_t at = 0;
        for (size_t i = first; i < y.size(); ++i)
            if (y[i] > peak * 0.5f) { at = i; break; }

        std::cout << "  " << juce::String (bpm, 1) << " BPM: first echo at " << juce::String (1000.0 * (double) at / sr, 1)
                  << " ms (1/4 = " << juce::String (60000.0 / bpm, 1) << " ms, 1/8 = " << juce::String (30000.0 / bpm, 1)
                  << ")  peak " << juce::String (juce::Decibels::gainToDecibels (peak), 1) << " dB" << std::endl;
    }

    std::cout.flush();
    std::_Exit (0);   // plugin instances: don't tear them down off the message loop
}

} // namespace stagerack
