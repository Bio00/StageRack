#import <Cocoa/Cocoa.h>
#include "PluginThumbnail.h"

namespace stagerack
{

static constexpr int thumbnailMaxWidth = 520;

/** Renders the part of the window covered by `comp` (native subviews included) into an image. */
static juce::Image renderNative (juce::Component& comp)
{
    auto* peer = comp.getPeer();

    if (peer == nullptr || comp.getWidth() <= 0 || comp.getHeight() <= 0)
        return {};

    auto* root = (NSView*) peer->getNativeHandle();
    const auto area = peer->getComponent().getLocalArea (&comp, comp.getLocalBounds());

    // JUCE's NSView is flipped, so JUCE and AppKit coordinates match here
    const NSRect rect = NSMakeRect (area.getX(), area.getY(), area.getWidth(), area.getHeight());
    NSBitmapImageRep* rep = [root bitmapImageRepForCachingDisplayInRect: rect];

    if (rep == nil)
        return {};

    [root cacheDisplayInRect: rect toBitmapImageRep: rep];

    CGImageRef cg = rep.CGImage;

    if (cg == nullptr)
        return {};

    const auto srcW = (int) CGImageGetWidth (cg), srcH = (int) CGImageGetHeight (cg);
    const auto outW = juce::jmin (srcW, thumbnailMaxWidth);
    const auto outH = juce::jmax (1, srcH * outW / juce::jmax (1, srcW));

    juce::Image img (juce::Image::ARGB, outW, outH, true);
    {
        juce::Image::BitmapData bd (img, juce::Image::BitmapData::writeOnly);
        auto cs = CGColorSpaceCreateDeviceRGB();
        auto ctx = CGBitmapContextCreate (bd.data, (size_t) outW, (size_t) outH, 8, (size_t) bd.lineStride, cs,
                                          (uint32_t) kCGImageAlphaPremultipliedFirst | (uint32_t) kCGBitmapByteOrder32Little);
        CGColorSpaceRelease (cs);

        if (ctx == nullptr)
            return {};

        CGContextSetInterpolationQuality (ctx, kCGInterpolationHigh);
        CGContextDrawImage (ctx, CGRectMake (0, 0, outW, outH), cg);
        CGContextRelease (ctx);
    }

    return img;
}

//==============================================================================
/** Invisible window hosting the editor just long enough for it to draw once. */
class PluginThumbnails::OffscreenCapture final : public juce::Component,
                                                 private juce::Timer
{
public:
    OffscreenCapture (PluginThumbnails& o, juce::AudioPluginInstance& p, juce::AudioProcessorEditor* ed)
        : owner (o), plugin (p), editor (ed)
    {
        setOpaque (true);
        addAndMakeVisible (editor.get());
        setSize (editor->getWidth(), editor->getHeight());
        setAlpha (0.0f);   // window exists and draws, but is fully transparent
        addToDesktop (juce::ComponentPeer::windowIsTemporary | juce::ComponentPeer::windowIgnoresMouseClicks);
        setTopLeftPosition (0, 0);
        setVisible (true);
        startTimer (700);  // plugins often draw their first frame from a timer
    }

    ~OffscreenCapture() override
    {
        stopTimer();
        editor = nullptr;  // delete the editor before the window
    }

    const juce::AudioPluginInstance* getPlugin() const noexcept { return &plugin; }

    void paint (juce::Graphics& g) override { g.fillAll (juce::Colours::black); }

private:
    void timerCallback() override
    {
        stopTimer();
        owner.captureFrom (&plugin, *editor);

        // delete ourselves asynchronously (we are owned by owner.pending)
        juce::MessageManager::callAsync ([&o = owner, self = this]
        {
            o.pending.removeObject (self);
        });
    }

    PluginThumbnails& owner;
    juce::AudioPluginInstance& plugin;
    std::unique_ptr<juce::AudioProcessorEditor> editor;
};

//==============================================================================
PluginThumbnails& PluginThumbnails::get()
{
    static PluginThumbnails instance;
    return instance;
}

PluginThumbnails::Entry PluginThumbnails::find (const juce::AudioPluginInstance* p) const
{
    if (auto it = entries.find (p); it != entries.end())
        return it->second;

    return {};
}

void PluginThumbnails::captureFrom (const juce::AudioPluginInstance* p, juce::Component& editor)
{
    auto img = renderNative (editor);

    if (img.isValid())
        entries[p] = { img, nextVersion++ };
}

void PluginThumbnails::requestOffscreen (juce::AudioPluginInstance& p)
{
    for (auto* c : pending)
        if (c->getPlugin() == &p)
            return;

    if (! p.hasEditor() || p.getActiveEditor() != nullptr)
        return;

    if (auto* ed = p.createEditorAndMakeActive())
        pending.add (new OffscreenCapture (*this, p, ed));
}

void PluginThumbnails::cancelOffscreen (const juce::AudioPluginInstance* p)
{
    for (int i = pending.size(); --i >= 0;)
        if (pending[i]->getPlugin() == p)
            pending.remove (i);
}

void PluginThumbnails::forget (const juce::AudioPluginInstance* p)
{
    cancelOffscreen (p);
    entries.erase (p);
}

void PluginThumbnails::reset()
{
    pending.clear();
    entries.clear();
}

bool PluginThumbnails::looksBlank (const juce::Image& img)
{
    if (! img.isValid())
        return true;

    float lo = 1.0f, hi = 0.0f;

    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
        {
            const auto b = img.getPixelAt ((x * 2 + 1) * img.getWidth() / 32, (y * 2 + 1) * img.getHeight() / 32).getBrightness();
            lo = juce::jmin (lo, b);
            hi = juce::jmax (hi, b);
        }

    return hi - lo < 0.03f;
}

} // namespace stagerack
