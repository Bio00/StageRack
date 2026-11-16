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
    void loadPlugin (Rack&, int slot, const juce::PluginDescription&, std::function<void (const juce::String& error)> done = {},
                     std::shared_ptr<bool> sharedUndoStep = {});   // several loads, one undo step (first arrival records it)
    void removePlugin (Rack&, int slot);
    void removePlugins (const std::vector<std::pair<Rack*, int>>&);   // one undo step
    void removeRack (Rack&);
    Rack& addRack();
    /** Adds `count` racks in one edit (one undo step). Inputs: consecutive, after the highest
        input used by any rack; outputs: like the last rack. */
    void addRacks (int count, bool stereoInputs);

    /** Called before a plugin is destroyed, so editor windows can be closed. */
    std::function<void (juce::AudioPluginInstance*)> onPluginAboutToBeDeleted;

    /** Instances taken out of the session while it is open (removed, replaced, re-spread,
        rack deleted) are PARKED here instead of being destroyed: alive, but never processed,
        so they cost nothing in the audio callback and add no latency. They are destroyed only
        when the session is closed or replaced, or the app quits.
        Why: some plugins (Alpha Labs De-Feedback 1.1.4) rarely corrupt the heap when an instance
        is destroyed while other instances are running; this keeps destruction out of the show. */
    void retirePlugin (std::unique_ptr<juce::AudioPluginInstance>);
    void releaseRetiredPlugins();
    int getNumRetiredPlugins() const noexcept { return (int) retired.size(); }

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

    /** Hop watch (call ~2x/s): plugins whose hop appeared or moved onto a busy callback
        (e.g. EverClean switched on later, a plugin un-bypassed) are re-spread live. */
    void watchHops();
    bool isRespreading() const noexcept;
    juce::String getRespreadStatus() const;

    //==============================================================================
    // Editing (message thread)
    juce::ValueTree rackToTree (const Rack&) const;                     // settings + plugins + states
    juce::ValueTree pluginToTree (const Rack&, int slot) const;         // description + state
    Rack& createRackFromTree (const juce::ValueTree&, int insertIndex); // new id; plugins load async
    /** newUndoStep: the plugin's arrival is an edit of its own (paste); otherwise it completes
        the edit that started the load (duplicate, session open). */
    void loadPluginFromTree (Rack&, int slot, const juce::ValueTree& pluginTree, bool newUndoStep = false);
    void duplicateRacks (const juce::Array<int>& rackIds);              // copies go after the last one
    void moveRackTo (int rackId, int newIndex);
    void movePlugin (Rack& from, int fromSlot, Rack& to, int toSlot);   // swaps if the target is used
    void copyPlugin (Rack& from, int fromSlot, Rack& to, int toSlot);
    /** Consecutive channels for the racks, in rack order. With startChannel >= 0 and firstRackId set,
        that rack gets startChannel and the numbering continues downward (racks above it come last);
        otherwise the first rack keeps its channel. With firstRackId set, only that rack takes `stereo`:
        the others keep their own mono/stereo width and the numbering advances by 1 or 2 for each. */
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
    /** The whole document (racks, plugin states, scenes), as saved in a .stagerack file. */
    juce::ValueTree toTree() const;
    /** Loads a document tree; `file` becomes the session's file (may be none). */
    void loadFromTree (const juce::ValueTree& root, const juce::File& file, bool markAsChanged);
    static bool isSessionTree (const juce::ValueTree&);
    /** Plugins of a loaded session still being created (their state isn't in toTree() yet). */
    int getPendingLoads() const noexcept            { return *pendingLoads; }
    /** Changes whenever the document is replaced or saved (new, open, save). */
    int getDocumentGeneration() const noexcept      { return documentGeneration; }
    int getLoadGeneration() const noexcept          { return *loadGeneration; }   // new/opened session
    int getRecallCount() const noexcept             { return recallCount; }       // scenes recalled so far
    juce::File getFile() const                      { return file; }
    bool isDirty() const noexcept                   { return dirty; }

    /** Edit groups (Rack::editGroup 1..8): active ones link edits across their racks.
        Saved with the session; switching one on/off is an undo step. */
    bool isEditGroupActive (int group) const noexcept { return group > 0 && (editGroupsOff & (1 << group)) == 0; }
    void setEditGroupActive (int group, bool active)  { editGroupsOff = active ? (editGroupsOff & ~(1 << group)) : (editGroupsOff | (1 << group)); markDirty(); }
    int numRacksInEditGroup (int group) const;
    void markDirty();

    //==============================================================================
    /** Undo/redo of the edits made in StageRack (racks, plugins added/removed/moved, routing,
        gains, mutes, groups, slot views, scenes). Each markDirty() is a step. A step stores no
        plugin state: instances taken out are parked alive (see retirePlugin), so undo puts the
        very same instance back, with its state, without loading anything. Changes made inside a
        plugin's own editor are the plugin's business (its own undo). History clears on new/open. */
    bool canUndo() const noexcept                   { return ! undoStack.empty() && *pendingLoads == 0; }
    bool canRedo() const noexcept                   { return ! redoStack.empty() && *pendingLoads == 0; }
    bool undo();
    bool redo();
    int getNumUndoSteps() const noexcept            { return (int) undoStack.size(); }

private:
    struct UndoState
    {
        juce::ValueTree tree;          // racks, slots (instance tokens), globals
        std::vector<Scene> scenes;     // shares the scene trees (no copy)
        int currentScene = -1;
        bool sameAs (const UndoState&) const;
    };
    UndoState captureUndoState() const;
    void applyUndoState (const UndoState&);
    void recordUndoStep();             // markDirty(): the state changed by a user edit
    void amendUndoState();             // same edit, completed later (plugin arrived, scene recalled)
    void resetUndoHistory();
    void pluginReplaced (juce::AudioPluginInstance* oldOne, juce::AudioPluginInstance* newOne);
    juce::AudioPluginInstance* takeInstance (juce::int64 token, std::map<juce::int64, std::unique_ptr<juce::AudioPluginInstance>>& pool);
    std::vector<UndoState> undoStack, redoStack;
    UndoState undoCurrent;
    std::map<juce::int64, juce::int64> replacedTokens;   // live re-spread: old instance -> its twin
    bool applyingUndo = false;
    static constexpr size_t maxUndoSteps = 200;

    juce::ValueTree rackSettingsToTree (const Rack&) const;
    void applyRackSettings (Rack&, const juce::ValueTree&);
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
    int editGroupsOff = 0;   // bit g set = group g suspended
    std::shared_ptr<int> loadGeneration = std::make_shared<int> (0);
    std::shared_ptr<int> pendingLoads = std::make_shared<int> (0);
    int documentGeneration = 0, recallCount = 0;

    class LiveRespreader;
    std::unique_ptr<LiveRespreader> respreader;
    std::vector<std::unique_ptr<juce::AudioPluginInstance>> retired;   // see retirePlugin()
    bool closingSession = false;
};

} // namespace stagerack
