#pragma once

// The playback engine: advances show time and evaluates the show (scenes,
// cues, layers, media scheduling) into a per-frame rendering state.
//
// Design principle: the visual state at show time t is a pure function of
// (show, t, manual overrides). Scene selection and layer cues (opacity,
// visibility, blend, position) are re-derived for every frame from the last
// matching cue at or before t, so seeking is trivially correct. tick() moves
// the clock and fires transport cues (stop/pause/seek) as the playhead
// crosses them; the transport state (PlayState) is the only live,
// non-derived state. The engine is fully deterministic, which is what makes
// it unit-testable without a GPU.

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "mediamax/blend/Blend.h"
#include "mediamax/media/MediaLibrary.h"
#include "mediamax/show/Show.h"
#include "mediamax/time/FrameRate.h"
#include "mediamax/time/Time.h"
#include "mediamax/util/Id.h"

namespace mediamax
{

enum class PlayState : std::uint8_t
{
    Stopped,
    Playing,
    Paused,
};

// One layer's resolved state for a single frame.
struct LayerFrame
{
    Id layerId = kInvalidId;

    // Resolved transform (tracks already applied).
    float x = 0.f;
    float y = 0.f;
    float rotation = 0.f; // radians
    float scaleX = 1.f;
    float scaleY = 1.f;
    float anchorX = 0.5f;
    float anchorY = 0.5f;

    float opacity = 1.f;
    BlendMode blend = BlendMode::Normal;
    bool visible = true;
    int zIndex = 0;

    // Media: kInvalidId for color layers.
    Id mediaId = kInvalidId;
    Time mediaTime; // current position within the media's timeline
    bool mediaAtEnd = false;
    Color4 color;
};

// The complete, resolved state to render for one frame.
// Layers are sorted for drawing: active scenes in show-list order, and
// zIndex ascending within each scene (stable).
struct FrameState
{
    Time showTime;
    // The primary (bottom-most in compositing order) active scene.
    Id activeSceneId = kInvalidId;
    // All active scenes, in show-list order (= compositing order, bottom to
    // top). activeSceneId is the first one, kept for compatibility.
    std::vector<Id> activeSceneIds;
    bool playing = false;
    std::vector<LayerFrame> layers;
};

class PlaybackEngine
{
public:
    explicit PlaybackEngine(MediaLibrary& media) : m_Media(media) {}

    // --- Show management ---------------------------------------------------

    // Replaces the current show. Without preserveState, playback resets to
    // time 0, stopped (no scene override, no layer overrides).
    //
    // With preserveState, the live playback state carries over to the new
    // show wherever the new show allows: the play state (playing/paused/
    // stopped) is kept, the playhead is kept (clamped to the new duration),
    // a manual scene override is kept when that scene id still exists, and
    // layer overrides are kept for layers that still exist. Used for live
    // edits pushed from the control interface, which must not restart
    // playback.
    void loadShow(Show show, bool preserveState = false);

    const Show& show() const { return m_Show; }
    Show& show() { return m_Show; }

    // --- Transport ---------------------------------------------------------

    PlayState state() const { return m_State; }
    Time time() const { return m_Time; }

    // Starts (or resumes) playback. When stopped at the end of a finite show,
    // playback restarts from the beginning.
    void play();
    void pause();
    void stop();

    // Moves the playhead. The visual state is re-derived from time, so
    // seeking is always consistent. Clamped to [0, duration] for finite
    // shows. (Transport state is not re-derived: cues that already fired are
    // not replayed by a seek.)
    void seek(const Time& t);

    // Advances the clock by dt (only while playing) and fires transport cues
    // (StopPlayback, PausePlayback, SeekTo) whose time lies in
    // (oldTime, oldTime + dt]. A SeekTo cue moves the playhead and ends the
    // scan. A PlayScene cue releases the operator's scene override (see
    // setSceneNow), returning scene selection to the timeline. Called once
    // per frame by the host application with the measured vsync delta.
    void tick(const Time& dt);

    // --- State queries -----------------------------------------------------

    // The resolved state at the current time.
    FrameState frameState() const;

    // The resolved state at an arbitrary show time (pure function).
    FrameState frameStateAt(const Time& t) const;

    // All scenes active at show time t, in show-list order (= compositing
    // order: the first is bottom-most). While a manual scene set is live
    // (setSceneNow/toggleSceneNow) that set is returned - possibly empty,
    // meaning no scene is active; otherwise the single timeline scene (the
    // last PlayScene cue at or before t; the preview scene when none has
    // fired).
    std::vector<Id> activeScenesAt(const Time& t) const;

    // The primary active scene at t: the first of activeScenesAt(t)
    // (kInvalidId when none). Kept for compatibility; prefer activeScenesAt.
    Id activeSceneAt(const Time& t) const;

    // The show time at which the primary active scene became active at t.
    Time sceneStartAt(const Time& t) const;

    // --- Manual overrides (via control interface) ---------------------------
    // These apply immediately and are re-evaluated on top of the timeline.

    void setPreviewScene(int sceneIndex);
    int previewScene() const { return m_PreviewScene; }

    // Live scene control from the control interface. setSceneNow() activates
    // exactly the given scene (solo); toggleSceneNow() adds/removes the
    // scene from the manual set - taken from a timeline-driven state it
    // first takes the timeline scene with it, so a toggle adds to (or, for
    // the timeline scene itself, removes) what the timeline shows. Toggling
    // the last manual scene off leaves the manual set in effect but empty:
    // no scene is active (black output) until a scene is toggled back on,
    // scene.clear releases, or a PlayScene cue is crossed. The media in a
    // manually activated scene start from its activation time.
    // clearSceneNow() returns to cue-driven scene selection (a PlayScene cue
    // crossed while playing does the same) and returns how many manual
    // scenes were released (0 for a deliberate empty set, which is
    // released as well).
    bool setSceneNow(Id sceneId);
    bool toggleSceneNow(Id sceneId);
    int clearSceneNow();
    // True while the operator's manual scene set is in effect - including a
    // deliberate empty set (no scene active).
    bool hasSceneOverride() const { return m_ForceScenes.has_value(); }

    bool setLayerOpacity(Id layerId, float value);
    bool setLayerVisible(Id layerId, bool value);
    bool setLayerBlend(Id layerId, BlendMode mode);
    bool setLayerPosition(Id layerId, float x, float y);

    // True when any manual override (opacity/visibility/blend/position) is
    // set for the layer.
    bool hasLayerOverride(Id layerId) const;

    // Clears one layer's overrides; returns false if it had none.
    bool clearLayerOverride(Id layerId);
    // Clears all layer overrides; returns how many were removed.
    int clearLayerOverrides();

    // --- Info --------------------------------------------------------------

    static std::string stateToString(PlayState s);

private:
    // The single scene the timeline selects at t: the last PlayScene cue at
    // or before t, or the preview scene (kInvalidId when the show has no
    // scenes at all).
    Id timelineSceneAt(const Time& t) const;
    // The show time at which scene `sceneId` became active at t: the manual
    // activation time when the scene is in the live manual set, otherwise
    // the last PlayScene cue targeting it (zero when none).
    Time sceneStartFor(const Time& t, Id sceneId) const;
    void resolveLayer(const Layer& layer, const Time& showTime, const Time& sceneStart,
                      LayerFrame& out) const;
    // Applies the layer-targeting cues (opacity/visibility/blend/position)
    // whose time is at or before showTime; the last one wins.
    void applyLayerCues(Id layerId, const Time& showTime, LayerFrame& out) const;
    Time mediaTimeFor(const Layer& layer, const Time& showTime, const Time& sceneStart,
                      bool& atEnd) const;

    MediaLibrary& m_Media;
    Show m_Show;
    PlayState m_State = PlayState::Stopped;
    Time m_Time;
    int m_PreviewScene = 0;
    // The operator's manual scene set (scene.set/scene.toggle), mapping each
    // scene to the show time at which it was activated (its media start from
    // there). While present, this set is the active set - even when empty,
    // which means no scene is active (black output). scene.clear or a
    // crossed PlayScene cue releases it (nullopt), returning scene
    // selection to the timeline.
    std::optional<std::map<Id, Time>> m_ForceScenes;

    struct LayerOverride
    {
        std::optional<float> opacity;
        std::optional<bool> visible;
        std::optional<BlendMode> blend;
        std::optional<float> x;
        std::optional<float> y;
    };
    std::map<Id, LayerOverride> m_Overrides;
};

} // namespace mediamax
