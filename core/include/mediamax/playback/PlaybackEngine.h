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
// Layers are sorted for drawing (zIndex ascending, stable).
struct FrameState
{
    Time showTime;
    Id activeSceneId = kInvalidId;
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

    // The scene id active at show time t (cue-driven; preview scene when no
    // PlayScene cue has fired yet). A scene set with setSceneNow() takes
    // precedence for the live time.
    Id activeSceneAt(const Time& t) const;

    // The show time at which the scene active at t became active.
    Time sceneStartAt(const Time& t) const;

    // --- Manual overrides (via control interface) ---------------------------
    // These apply immediately and are re-evaluated on top of the timeline.

    void setPreviewScene(int sceneIndex);
    int previewScene() const { return m_PreviewScene; }

    // Switches the active scene immediately (live operation from the control
    // interface). The media in the new scene start from the current show
    // time. clearSceneNow() returns to cue-driven scene selection.
    bool setSceneNow(Id sceneId);
    void clearSceneNow();
    bool hasSceneOverride() const { return m_ForceScene.has_value(); }

    bool setLayerOpacity(Id layerId, float value);
    bool setLayerVisible(Id layerId, bool value);
    bool setLayerBlend(Id layerId, BlendMode mode);
    bool setLayerPosition(Id layerId, float x, float y);

    // Clears one layer's overrides; returns false if it had none.
    bool clearLayerOverride(Id layerId);
    // Clears all layer overrides; returns how many were removed.
    int clearLayerOverrides();

    // --- Info --------------------------------------------------------------

    static std::string stateToString(PlayState s);

private:
    const Scene* activeScene(const Time& t, Time& sceneStart) const;
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
    std::optional<Id> m_ForceScene;
    Time m_ForceSceneTime;

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
