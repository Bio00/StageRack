#pragma once

#include "AudioEngine.h"
#include "../Plugins/PluginManager.h"

namespace stagerack
{

/** A snapshot of every rack's settings and plugin state (SuperRack "scene" / LiveProfessor "global snapshot"). */
struct Scene
{
    juce::String name;
    juce::ValueTree data;
};

/**
    The document: racks, plugins and scenes. Lives on the message thread.
    Sends a change message whenever its structure or the current scene changes.
*/
class Session final : public juce::ChangeBroadcaster
{
public:
    Session (AudioEngine&, PluginManager&);
    ~Session() override;

    AudioEngine& getEngine() noexcept           { return engine; }
    PluginManager& getPluginManager() noexcept  { return plugins; }

    //==============================================================================
    void loadPlugin (Rack&, int slot, const juce::PluginDescription&, std::function<void (const juce::String& error)> done = {});
    void removePlugin (Rack&, int slot);
    void removeRack (Rack&);
    Rack& addRack();

    /** Called before a plugin is destroyed, so editor windows can be closed. */
    std::function<void (juce::AudioPluginInstance*)> onPluginAboutToBeDeleted;

    /** Called when a live re-spread replaced an instance by its twin (before the old one is
        deleted), so an open editor can move to the new instance. */
    std::function<void (juce::AudioPluginInstance* oldOne, juce::AudioPluginInstance* newOne)> onPluginReplaced;

    //==============================================================================
    /** Re-spreads the hops of every loaded plugin WITHOUT stopping the audio: each plugin with
        a periodic hop gets a twin (same state and parameters) re-phased offline, which runs on
        the live input next to it and then takes over through a short crossfade. One plugin at
        a time; plugins without a hop are left alone. Sound, latency and state are kept. */
    void respreadLive();
    void cancelRespread();
    bool isRespreading() const noexcept;
    juce::String getRespreadStatus() const;

    //==============================================================================
    // Editing (message thread)
    juce::ValueTree rackToTree (const Rack&) const;                     // settings + plugins + states
    juce::ValueTree pluginToTree (const Rack&, int slot) const;         // description + state
    Rack& createRackFromTree (const juce::ValueTree&, int insertIndex); // new id; plugins load async
    void loadPluginFromTree (Rack&, int slot, const juce::ValueTree& pluginTree);
    void duplicateRacks (const juce::Array<int>& rackIds);              // copies go after the last one
    void moveRackTo (int rackId, int newIndex);
    void movePlugin (Rack& from, int fromSlot, Rack& to, int toSlot);   // swaps if the target is used
    void copyPlugin (Rack& from, int fromSlot, Rack& to, int toSlot);
    /** Consecutive channels for the racks, in rack order. With startChannel >= 0 and firstRackId set,
        that rack gets startChannel and the numbering continues downward (racks above it come last);
        otherwise the first rack keeps its channel. */
    void setConsecutiveInputs (const juce::Array<int>& rackIds, bool stereo, int startChannel = -1, int firstRackId = -1);
    void setConsecutiveOutputs (const juce::Array<int>& rackIds, bool stereo, int startChannel = -1, int firstRackId = -1);

    //==============================================================================
    int getNumScenes() const noexcept               { return (int) scenes.size(); }
    const Scene& getScene (int i) const             { return scenes[(size_t) i]; }
    int getCurrentScene() const noexcept            { return currentScene; }

    void addScene (const juce::String& name);       // captures the current state
    void storeScene (int index);                    // overwrite with the current state
    void recallScene (int index);
    void renameScene (int index, const juce::String&);
    void deleteScene (int index);
    void moveScene (int from, int to);
    void stepScene (int delta);

    //==============================================================================
    void newSession();
    bool saveTo (const juce::File&);
    void loadFrom (const juce::File&);
    juce::File getFile() const                      { return file; }
    bool isDirty() const noexcept                   { return dirty; }
    void markDirty();

private:
    juce::ValueTree captureSceneData() const;
    void buildRack (Rack&, const juce::ValueTree& rackTree);
    void applySceneData (const juce::ValueTree&);
    void notify()                                   { sendChangeMessage(); }

    AudioEngine& engine;
    PluginManager& plugins;
    std::vector<Scene> scenes;
    int currentScene = -1;
    juce::File file;
    bool dirty = false;
    std::shared_ptr<int> loadGeneration = std::make_shared<int> (0);

    class LiveRespreader;
    std::unique_ptr<LiveRespreader> respreader;
};

} // namespace stagerack
