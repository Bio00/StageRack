#include "PluginBrowser.h"
#include "Theme.h"
#include <map>

namespace stagerack
{

static juce::String shortFormatName (const juce::String& f)
{
    return f == "AudioUnit" ? juce::String ("AU") : f;
}

// Remembered between openings: the browser comes back where it was left.
static juce::String lastCategory, lastMaker;
static int lastFormatId = 1;

//==============================================================================
juce::String PluginBrowser::categoryOf (const juce::PluginDescription& d)
{
    if (d.isInstrument)
        return "Instrument";

    // VST3 sub-categories ("Fx|Dynamics") are precise when the vendor fills them in
    static const std::pair<const char*, const char*> vst3[] = {
        { "Reverb", "Reverb" }, { "Delay", "Delay" }, { "EQ", "EQ" }, { "Filter", "EQ" }, { "Dynamics", "Dynamics" },
        { "Distortion", "Harmonic" }, { "Restoration", "Noise Reduction" }, { "Analyzer", "Metering" },
        { "Spatial", "Sound Field" }, { "Surround", "Sound Field" }, { "Pitch Shift", "Pitch Shift" },
        { "Modulation", "Modulation" }, { "Instrument", "Instrument" }, { "Synth", "Instrument" } };

    for (auto& tok : juce::StringArray::fromTokens (d.category, "|", ""))
        for (auto& [k, v] : vst3)
            if (tok.trim().equalsIgnoreCase (k))
                return v;

    // otherwise (most AUs only say "Effect") guess from the name; order matters
    const auto n = " " + d.name.toLowerCase() + " ";

    // vector, not initializer_list: a list copied into std::pair would dangle
    static const std::vector<std::pair<const char*, std::vector<const char*>>> rules = {
        { "Instrument",      { "synth", "sampler", "musicdevice", "instrument", " ba-1 " } },
        { "Noise Reduction", { "feedback", "fdbk", "noise", "clean", "clarity", "dereverb", "click", "crackle", "-hum",
                               " hum", "debreath", "silencer", "isolation", " void", " wns", " ns1", " w43", "d.n.a", "denois" } },
        { "Metering",        { "meter", "analyzer", "analyser", "paz-", " rta", "measure", "spl " } },
        { "Channel Strip",   { "channel", " strip", "nls ", "magma channel", "redd", "tg12345" } },
        { "Pitch Shift",     { "pitch", " tune", "harmony", "bender", "shifter", "ovox", "morphoder", "torque", "humanoid" } },
        { "Reverb",          { "reverb", "verb", " rev ", "plate", "spring", "supermassive", "irlive", "space", "crystalline" } },
        { "Delay",           { "delay", "echo", " dub", "relayer", "eternity", "transit", "h-delay" } },
        { "Dynamics",        { "comp", "limit", "gate", "de-ess", "deess", "sibilance", "rider", "maximizer", "pro-l",
                               "dynamic", "-2a", "-3a", "-76", "2500", "dbx", "puigchild", "maxxvolume", "idx",
                               "smack", "transient", "trackspacer", "god particle", "pressure", "pumper", "spiff",
                               "aggressor", " c1 ", " c6", "mv2", "rs124", "soothe", "smooth operator", "tg12345",
                               "silk vocal", " rvox", "vocal", "kramer pie" } },
        { "EQ",              { " eq", "eq ", "-eq", "pro-q", "filter", "shelf", "pass", "curves", "puigtec", "-550",
                               "-560", "tract", " f6", "meq", "veq", "graphiceq", "rs56", "bandeq", "q4", "lineq" } },
        { "Harmonic",        { "dist", "drive", "fuzz", "screamer", "satur", "tape", "tube", "amp", "exciter", "vinyl",
                               "lofi", "lo-fi", "retro fi", "vhs", "cassette", "valvola", "kramer", "magma", "maxxbass",
                               "rbass", "loair", "spice", "brighter", "phatter", "vitamin", "stressbox", "aphex", "valve",
                               "louder", "taip", "ihny", "hls", "tone shaper", "rogerbeep", "lp", "luxe", "angle", "rooster",
                               "tekno", "comeback" } },
        { "Modulation",      { "chorus", "flang", "phaser", "doubler", "adt", "tremolo", "vibrato", "wetter", "freqecho" } },
        { "Sound Field",     { "pan", "imager", " s1 ", "center", "mixer", "inphase", "sub align", "matrix", "shuffler",
                               "spatial", "zerobus", "pse", "splitter", "sync vx", "wider", "stereo" } } };

    for (auto& [cat, keys] : rules)
        for (auto* k : keys)
            if (n.contains (k))
                return cat;

    return "Other";
}

//==============================================================================
/** Left column: All / CATEGORY / MANUFACTURER entries with counts. Click selects, click again clears. */
class PluginBrowser::Sidebar final : public juce::Component,
                                     private juce::ListBoxModel
{
public:
    struct Entry { int type; juce::String label, key; int count; };   // type 0 = heading, 1 = all, 2 = category, 3 = maker

    Sidebar (PluginBrowser& o) : owner (o)
    {
        list.setModel (this);
        list.setRowHeight (22);
        list.setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
        list.setOutlineThickness (0);
        addAndMakeVisible (list);
    }

    void build()
    {
        std::map<juce::String, int> cats;
        std::map<juce::String, std::pair<juce::String, int>> makers;   // lowercase -> (shown name, count)

        for (int i = 0; i < owner.all.size(); ++i)
        {
            ++cats[owner.categories[i]];
            auto maker = owner.all.getReference (i).manufacturerName.trim();
            if (maker.isEmpty()) maker = "Unknown";
            auto& m = makers[maker.toLowerCase()];
            if (m.first.isEmpty()) m.first = maker;
            ++m.second;
        }

        entries.clear();
        entries.push_back ({ 1, "All plugins", {}, owner.all.size() });
        entries.push_back ({ 0, "CATEGORY", {}, 0 });

        for (auto& [c, n] : cats)
            if (c != "Other")
                entries.push_back ({ 2, c, c, n });

        if (cats.count ("Other") > 0)
            entries.push_back ({ 2, "Other", "Other", cats["Other"] });

        entries.push_back ({ 0, "MANUFACTURER", {}, 0 });

        for (auto& [k, m] : makers)
            entries.push_back ({ 3, m.first, k, m.second });

        list.updateContent();
        list.repaint();
    }

    void resized() override { list.setBounds (getLocalBounds()); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::panelLight);
        g.setColour (theme::outline);
        g.drawVerticalLine (getWidth() - 1, 0.0f, (float) getHeight());
    }

private:
    int getNumRows() override { return (int) entries.size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool) override
    {
        if (! juce::isPositiveAndBelow (row, (int) entries.size()))
            return;

        const auto& e = entries[(size_t) row];

        if (e.type == 0)
        {
            g.setColour (theme::accent);
            g.setFont (theme::font (10.0f, true));
            g.drawText (e.label, 10, 0, w - 12, h - 2, juce::Justification::bottomLeft);
            return;
        }

        const bool on = (e.type == 1 && lastCategory.isEmpty() && lastMaker.isEmpty())
                     || (e.type == 2 && lastCategory == e.key)
                     || (e.type == 3 && lastMaker == e.key);

        if (on)
        {
            g.setColour (theme::accent.withAlpha (0.22f));
            g.fillRoundedRectangle (juce::Rectangle<float> (4.0f, 1.0f, (float) w - 10.0f, (float) h - 2.0f), 4.0f);
        }

        g.setColour (on ? theme::text : theme::text.withAlpha (0.85f));
        g.setFont (theme::font (12.0f, on));
        g.drawText (e.label, 14, 0, w - 54, h, juce::Justification::centredLeft, true);
        g.setColour (theme::textDim);
        g.setFont (theme::font (10.5f));
        g.drawText (juce::String (e.count), w - 46, 0, 34, h, juce::Justification::centredRight);
    }

    void listBoxItemClicked (int row, const juce::MouseEvent&) override
    {
        if (! juce::isPositiveAndBelow (row, (int) entries.size()))
            return;

        const auto& e = entries[(size_t) row];

        if (e.type == 1)      { lastCategory = {}; lastMaker = {}; }
        else if (e.type == 2) lastCategory = lastCategory == e.key ? juce::String() : e.key;
        else if (e.type == 3) lastMaker = lastMaker == e.key ? juce::String() : e.key;
        else                  return;

        list.deselectAllRows();
        list.repaint();
        owner.filter();
    }

    PluginBrowser& owner;
    juce::ListBox list;
    std::vector<Entry> entries;
};

//==============================================================================
class PluginBrowser::PluginList final : public juce::ListBox,
                                        private juce::ListBoxModel
{
public:
    explicit PluginList (PluginBrowser& o) : owner (o)
    {
        setModel (this);
        setRowHeight (26);
        setOutlineThickness (0);
        setColour (juce::ListBox::backgroundColourId, theme::panel);
    }

    void lookAndFeelChanged() override
    {
        setColour (juce::ListBox::backgroundColourId, theme::panel);
        juce::ListBox::lookAndFeelChanged();
    }

private:
    int getNumRows() override { return owner.filtered.size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
    {
        if (! juce::isPositiveAndBelow (row, owner.filtered.size()))
            return;

        const auto& d = owner.filtered.getReference (row);
        const auto ms = owner.manager.getLatencyMs (d);

        if (selected)
            g.fillAll (theme::accent.withAlpha (0.25f));
        else if (row % 2 == 1)
            g.fillAll (theme::text.withAlpha (0.025f));

        // latency dot
        const auto dot = juce::Rectangle<float> (10.0f, (float) h / 2.0f - 5.0f, 10.0f, 10.0f);

        if (ms >= 0.0f)
        {
            g.setColour (theme::latencyColour (ms));
            g.fillEllipse (dot);
        }
        else
        {
            g.setColour (theme::textDim.withAlpha (0.7f));
            g.drawEllipse (dot.reduced (0.5f), 1.2f);
        }

        const int right = w - 52;
        g.setColour (theme::text);
        g.setFont (theme::font (13.0f, true));
        g.drawText (d.name, 30, 0, right - 30 - 250, h, juce::Justification::centredLeft, true);

        g.setColour (theme::textDim);
        g.setFont (theme::font (11.0f));
        g.drawText (categoryOf (d), right - 250, 0, 90, h, juce::Justification::centredLeft, true);
        g.drawText (d.manufacturerName, right - 160, 0, 104, h, juce::Justification::centredLeft, true);

        // latency value
        juce::String lat = ms >= 0.0f ? (ms < 0.01f ? juce::String ("0") : juce::String (ms, ms < 10.0f ? 1 : 0) + " ms")
                                      : juce::String (ms == PluginManager::latencyFailed ? "?" : "-");
        g.setColour (ms >= 0.0f ? theme::latencyColour (ms) : theme::textDim);
        g.setFont (theme::font (11.0f, true));
        g.drawText (lat, right - 54, 0, 48, h, juce::Justification::centredRight);

        auto badge = juce::Rectangle<int> (w - 44, 5, 38, h - 10).toFloat();
        g.setColour (theme::slot);
        g.fillRoundedRectangle (badge, 3.0f);
        g.setColour (theme::accent);
        g.setFont (theme::font (10.0f, true));
        g.drawText (shortFormatName (d.pluginFormatName), badge, juce::Justification::centred);
    }

    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override { owner.choose (row); }
    void returnKeyPressed (int row) override                                   { owner.choose (row); }

    PluginBrowser& owner;
};

//==============================================================================
PluginBrowser::PluginBrowser (PluginManager& pm, std::function<void (const juce::PluginDescription&)> onChoose)
    : manager (pm), chosen (std::move (onChoose))
{
    search.setTextToShowWhenEmpty ("Search plugins...", theme::textDim);
    search.onTextChange = [this] { filter(); };
    search.onReturnKey = [this] { choose (list->getSelectedRow() >= 0 ? list->getSelectedRow() : 0); };
    search.setFont (theme::font (14.0f));
    addAndMakeVisible (search);

    format.addItem ("All formats", 1);
    int id = 2;
    for (auto f : { "AudioUnit", "VST3", "VST", "CLAP", "LV2" })
        format.addItem (shortFormatName (f), id++);
    format.setSelectedId (lastFormatId, juce::dontSendNotification);
    format.onChange = [this] { lastFormatId = format.getSelectedId(); filter(); };
    addAndMakeVisible (format);

    measureButton.setTooltip ("Loads each plugin whose latency is unknown in a separate process and reads its latency "
                              "(48 kHz). Runs in the background; plugins you load in a rack are measured automatically.");
    measureButton.onClick = [this]
    {
        if (manager.isProbingLatency()) manager.stopLatencyProbe();
        else                            manager.startLatencyProbe();
        updateMeasureButton();
    };
    addAndMakeVisible (measureButton);

    all = manager.getKnownList().getTypes();
    std::sort (all.begin(), all.end(), [] (const auto& a, const auto& b) { return a.name.compareIgnoreCase (b.name) < 0; });

    for (auto& d : all)
        categories.add (categoryOf (d));

    sidebar = std::make_unique<Sidebar> (*this);
    addAndMakeVisible (*sidebar);
    list = std::make_unique<PluginList> (*this);
    addAndMakeVisible (*list);

    sidebar->build();
    filter();
    updateMeasureButton();
    startTimerHz (4);
    setSize (820, 560);
}

PluginBrowser::~PluginBrowser() = default;

void PluginBrowser::visibilityChanged()
{
    if (isShowing())
        search.grabKeyboardFocus();
}

void PluginBrowser::resized()
{
    auto r = getLocalBounds();
    auto top = r.removeFromTop (30);
    format.setBounds (top.removeFromRight (110));
    top.removeFromRight (6);
    search.setBounds (top);
    r.removeFromTop (6);

    auto bottom = r.removeFromBottom (30);
    measureButton.setBounds (bottom.removeFromRight (190).reduced (0, 3));
    legendArea = bottom;

    sidebar->setBounds (r.removeFromLeft (190));
    list->setBounds (r);
}

void PluginBrowser::paint (juce::Graphics& g)
{
    // legend
    auto r = legendArea.reduced (4, 0);
    g.setFont (theme::font (11.0f));

    auto item = [&] (float ms, const juce::String& text)
    {
        auto dot = r.removeFromLeft (12).toFloat().withSizeKeepingCentre (10.0f, 10.0f);

        if (ms >= 0.0f) { g.setColour (theme::latencyColour (ms)); g.fillEllipse (dot); }
        else            { g.setColour (theme::textDim.withAlpha (0.7f)); g.drawEllipse (dot.reduced (0.5f), 1.2f); }

        r.removeFromLeft (5);
        g.setColour (theme::textDim);
        const auto tw = juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), text) + 4;
        g.drawText (text, r.removeFromLeft (tw), juce::Justification::centredLeft);
        r.removeFromLeft (14);
    };

    item (0.0f, "0 latency");
    item (1.0f, "< 5 ms");
    item (10.0f, juce::String::fromUTF8 ("\xe2\x89\xa5 5 ms"));
    item (-1.0f, "not measured");

    if (all.isEmpty())
    {
        g.setColour (theme::textDim);
        g.setFont (theme::font (13.0f));
        g.drawFittedText ("No plugins yet.\nOpen PLUGINS in the top bar and run a scan.",
                          list->getBounds().withTrimmedTop (60), juce::Justification::centredTop, 3);
    }
}

bool PluginBrowser::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::downKey || k == juce::KeyPress::upKey)
    {
        const auto row = juce::jlimit (0, juce::jmax (0, filtered.size() - 1),
                                       list->getSelectedRow() + (k == juce::KeyPress::downKey ? 1 : -1));
        list->selectRow (row);
        return true;
    }

    return false;
}

void PluginBrowser::timerCallback()
{
    const bool probing = manager.isProbingLatency();

    if (probing || probing != wasProbing)
    {
        updateMeasureButton();
        list->repaint();
    }

    wasProbing = probing;
}

void PluginBrowser::updateMeasureButton()
{
    if (manager.isProbingLatency())
    {
        const auto [done, total] = manager.getLatencyProbeProgress();
        measureButton.setButtonText ("Measuring " + juce::String (done) + "/" + juce::String (total) + "  (stop)");
        measureButton.setEnabled (true);
        return;
    }

    const auto unknown = manager.getNumUnknownLatencies();
    measureButton.setButtonText (unknown > 0 ? "Measure latency (" + juce::String (unknown) + ")" : "All latencies measured");
    measureButton.setEnabled (unknown > 0);
}

void PluginBrowser::filter()
{
    const auto words = juce::StringArray::fromTokens (search.getText(), " ", "");
    const auto formatName = format.getSelectedId() <= 1 ? juce::String() : format.getText();
    filtered.clearQuick();

    for (int i = 0; i < all.size(); ++i)
    {
        const auto& d = all.getReference (i);

        if (formatName.isNotEmpty() && shortFormatName (d.pluginFormatName) != formatName)
            continue;

        if (lastCategory.isNotEmpty() && categories[i] != lastCategory)
            continue;

        if (lastMaker.isNotEmpty())
        {
            auto maker = d.manufacturerName.trim();
            if (maker.isEmpty()) maker = "Unknown";
            if (maker.toLowerCase() != lastMaker)
                continue;
        }

        const auto hay = d.name + " " + d.manufacturerName + " " + categories[i];
        bool ok = true;

        for (auto& w : words)
            ok = ok && hay.containsIgnoreCase (w);

        if (ok)
            filtered.add (d);
    }

    list->updateContent();
    list->selectRow (0);
    list->repaint();
    repaint();
}

void PluginBrowser::choose (int row)
{
    if (! juce::isPositiveAndBelow (row, filtered.size()))
        return;

    const auto d = filtered[row];
    auto cb = chosen;

    if (auto* box = findParentComponentOfClass<juce::CallOutBox>())
        box->dismiss();

    cb (d);
}

} // namespace stagerack
