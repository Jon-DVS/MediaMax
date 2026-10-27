#include "mediamax/playback/PlaybackEngine.h"

#include <algorithm>
#include <cmath>

namespace mediamax
{

namespace
{

// True when any scene of the show contains the layer (a layer override is
// only meaningful for a layer that still exists).
bool layerExistsInShow(const Show& show, Id layerId)
{
    for (const auto& scene : show.scenes)
    {
        for (const auto& layer : scene.layers)
        {
            if (layer.id == layerId)
            {
                return true;
            }
        }
    }
    return false;
}

} // namespace

void PlaybackEngine::loadShow(Show show, bool preserveState)
{
    if (!preserveState)
    {
        m_Show = std::move(show);
        m_State = PlayState::Stopped;
        m_Time = Time::zero();
        m_PreviewScene = 0;
        m_ForceScenes.reset();
        m_Overrides.clear();
        return;
    }

    // Live edit: carry the playback state over to the new show wherever it
    // still makes sense.
    const PlayState state = m_State;
    const Time time = m_Time;
    m_Show = std::move(show);
    m_State = state;
    m_Time = time;
    if (m_Show.duration.isValid() && m_Time > m_Show.duration)
    {
        m_Time = m_Show.duration;
    }
    if (m_PreviewScene >= static_cast<int>(m_Show.scenes.size()))
    {
        m_PreviewScene = 0;
    }
    if (m_ForceScenes.has_value())
    {
        for (auto it = m_ForceScenes->begin(); it != m_ForceScenes->end();)
        {
            it = m_Show.findScene(it->first) ? std::next(it) : m_ForceScenes->erase(it);
        }
        if (m_ForceScenes->empty())
        {
            // Every manually active scene was removed by the edit: release
            // the override back to the timeline. (A deliberate empty set is
            // only ever created live, by toggling the last scene off.)
            m_ForceScenes.reset();
        }
    }
    for (auto it = m_Overrides.begin(); it != m_Overrides.end();)
    {
        const bool kept = layerExistsInShow(m_Show, it->first);
        it = kept ? std::next(it) : m_Overrides.erase(it);
    }
}

void PlaybackEngine::play()
{
    // Restart from the beginning when playing from the end of a finite show.
    if (m_State == PlayState::Stopped && m_Show.duration.isValid() && m_Show.duration > Time::zero() &&
        m_Time >= m_Show.duration)
    {
        m_Time = Time::zero();
    }
    m_State = PlayState::Playing;
}

void PlaybackEngine::pause()
{
    if (m_State == PlayState::Playing)
    {
        m_State = PlayState::Paused;
    }
}

void PlaybackEngine::stop()
{
    m_State = PlayState::Stopped;
    m_Time = Time::zero();
}

void PlaybackEngine::seek(const Time& t)
{
    Time target = t;
    if (m_Show.duration.isValid() && m_Show.duration > Time::zero())
    {
        target = target.clamp(Time::zero(), m_Show.duration);
    }
    else if (target.isNegative())
    {
        target = Time::zero();
    }
    m_Time = target;
}

void PlaybackEngine::tick(const Time& dt)
{
    if (m_State != PlayState::Playing)
    {
        return;
    }
    const Time oldTime = m_Time;
    m_Time = m_Time + dt;

    // Fire transport cues whose time lies in (oldTime, m_Time]. The cue list
    // is sorted by time, so a single scan suffices. Layer cues are not fired
    // here: they are re-derived from time in frameStateAt(), so they work
    // identically when seeking. PlayScene cues are fired only to release the
    // operator's manual scene set: the timeline takes back scene control at
    // the next PlayScene cue it crosses.
    for (const auto& cue : m_Show.cues)
    {
        if (cue.time <= oldTime)
        {
            continue;
        }
        if (cue.time > m_Time)
        {
            break;
        }
        if (std::holds_alternative<StopPlayback>(cue.action))
        {
            m_Time = cue.time;
            m_State = PlayState::Stopped;
            return;
        }
        if (std::holds_alternative<PausePlayback>(cue.action))
        {
            m_Time = cue.time;
            m_State = PlayState::Paused;
            return;
        }
        if (std::holds_alternative<PlayScene>(cue.action))
        {
            m_ForceScenes.reset();
        }
        if (const auto* seek = std::get_if<SeekTo>(&cue.action))
        {
            m_Time = seek->time;
            break; // at most one seek per tick; no cue re-firing
        }
    }

    if (m_Show.duration.isValid() && m_Show.duration > Time::zero() && m_Time >= m_Show.duration)
    {
        m_Time = m_Show.duration;
        m_State = PlayState::Stopped;
    }
}

FrameState PlaybackEngine::frameState() const
{
    return frameStateAt(m_Time);
}

Id PlaybackEngine::timelineSceneAt(const Time& t) const
{
    // The most recent PlayScene cue at or before t wins; otherwise the
    // preview scene.
    Id sceneId = kInvalidId;
    for (const auto& cue : m_Show.cues)
    {
        if (cue.time > t)
        {
            break; // sorted by time
        }
        if (const auto* playScene = std::get_if<PlayScene>(&cue.action))
        {
            if (m_Show.findScene(playScene->scene))
            {
                sceneId = playScene->scene;
            }
        }
    }
    if (sceneId == kInvalidId)
    {
        if (m_PreviewScene >= 0 && m_PreviewScene < static_cast<int>(m_Show.scenes.size()))
        {
            sceneId = m_Show.scenes[static_cast<size_t>(m_PreviewScene)].id;
        }
    }
    return sceneId;
}

std::vector<Id> PlaybackEngine::activeScenesAt(const Time& t) const
{
    std::vector<Id> result;
    // The live manual set (setSceneNow/toggleSceneNow) wins for the current
    // time - including a deliberate empty set (no scene active); reported in
    // show-list order, which is the compositing order.
    if (t == m_Time && m_ForceScenes.has_value())
    {
        for (const auto& scene : m_Show.scenes)
        {
            if (m_ForceScenes->count(scene.id) > 0)
            {
                result.push_back(scene.id);
            }
        }
        return result;
    }
    const Id sceneId = timelineSceneAt(t);
    if (sceneId != kInvalidId)
    {
        result.push_back(sceneId);
    }
    return result;
}

Id PlaybackEngine::activeSceneAt(const Time& t) const
{
    const std::vector<Id> active = activeScenesAt(t);
    return active.empty() ? kInvalidId : active.front();
}

Time PlaybackEngine::sceneStartFor(const Time& t, Id sceneId) const
{
    // A manually activated scene starts (its media) from its activation
    // time.
    if (t == m_Time && m_ForceScenes.has_value())
    {
        if (const auto it = m_ForceScenes->find(sceneId); it != m_ForceScenes->end())
        {
            return it->second;
        }
    }
    // Otherwise: the most recent PlayScene cue targeting the scene.
    Time start = Time::zero();
    for (const auto& cue : m_Show.cues)
    {
        if (cue.time > t)
        {
            break;
        }
        if (const auto* playScene = std::get_if<PlayScene>(&cue.action))
        {
            if (playScene->scene == sceneId)
            {
                start = cue.time;
            }
        }
    }
    return start;
}

Time PlaybackEngine::sceneStartAt(const Time& t) const
{
    return sceneStartFor(t, activeSceneAt(t));
}

FrameState PlaybackEngine::frameStateAt(const Time& t) const
{
    FrameState state;
    state.showTime = t;
    state.playing = m_State == PlayState::Playing;

    // Every active scene is composited, in show-list order (bottom to top);
    // within a scene, layers draw by zIndex ascending, stable. The first
    // scene is the primary one (activeSceneId).
    for (const Id sceneId : activeScenesAt(t))
    {
        const Scene* scene = m_Show.findScene(sceneId);
        if (!scene)
        {
            continue;
        }
        state.activeSceneIds.push_back(scene->id);
        if (state.activeSceneId == kInvalidId)
        {
            state.activeSceneId = scene->id;
        }
        const Time sceneStart = sceneStartFor(t, scene->id);

        std::vector<const Layer*> ordered;
        ordered.reserve(scene->layers.size());
        for (const auto& layer : scene->layers)
        {
            ordered.push_back(&layer);
        }
        std::stable_sort(ordered.begin(), ordered.end(),
                         [](const Layer* a, const Layer* b) { return a->zIndex < b->zIndex; });

        for (const Layer* layer : ordered)
        {
            LayerFrame lf;
            resolveLayer(*layer, t, sceneStart, lf);
            if (lf.visible && lf.opacity > 0.0f)
            {
                state.layers.push_back(std::move(lf));
            }
        }
    }
    return state;
}

void PlaybackEngine::resolveLayer(const Layer& layer, const Time& showTime, const Time& sceneStart,
                                  LayerFrame& out) const
{
    out.layerId = layer.id;
    out.x = layer.transform.x;
    out.y = layer.transform.y;
    out.rotation = layer.transform.rotation;
    out.scaleX = layer.transform.scaleX;
    out.scaleY = layer.transform.scaleY;
    out.anchorX = layer.transform.anchorX;
    out.anchorY = layer.transform.anchorY;
    out.opacity = layer.opacity;
    out.blend = layer.blend;
    out.visible = layer.visible;
    out.zIndex = layer.zIndex;
    out.mediaId = layer.mediaId;
    out.color = layer.color;
    out.mediaAtEnd = false;
    out.mediaTime = layer.inPoint;

    // Keyframe tracks override the static values.
    if (!layer.xTrack.empty())
    {
        out.x = layer.xTrack.valueAt(showTime);
    }
    if (!layer.yTrack.empty())
    {
        out.y = layer.yTrack.valueAt(showTime);
    }
    if (!layer.rotationTrack.empty())
    {
        out.rotation = layer.rotationTrack.valueAt(showTime);
    }
    if (!layer.scaleTrack.empty())
    {
        const float s = layer.scaleTrack.valueAt(showTime);
        out.scaleX = s;
        out.scaleY = s;
    }
    if (!layer.opacityTrack.empty())
    {
        out.opacity = layer.opacityTrack.valueAt(showTime);
    }

    // Timeline cues: the last cue targeting this layer at or before showTime
    // wins (pure function of t, like the tracks).
    applyLayerCues(layer.id, showTime, out);

    // Manual overrides (control interface) win over the timeline.
    if (const auto it = m_Overrides.find(layer.id); it != m_Overrides.end())
    {
        if (it->second.opacity) out.opacity = *it->second.opacity;
        if (it->second.visible) out.visible = *it->second.visible;
        if (it->second.blend) out.blend = *it->second.blend;
        if (it->second.x) out.x = *it->second.x;
        if (it->second.y) out.y = *it->second.y;
    }

    // Media scheduling.
    if (layer.mediaId != kInvalidId)
    {
        out.mediaTime = mediaTimeFor(layer, showTime, sceneStart, out.mediaAtEnd);
    }
}

void PlaybackEngine::applyLayerCues(Id layerId, const Time& showTime, LayerFrame& out) const
{
    for (const auto& cue : m_Show.cues)
    {
        if (cue.time > showTime)
        {
            break; // sorted by time
        }
        if (const auto* a = std::get_if<SetLayerOpacity>(&cue.action))
        {
            if (a->layer == layerId)
            {
                out.opacity = a->value;
            }
        }
        else if (const auto* a = std::get_if<SetLayerVisible>(&cue.action))
        {
            if (a->layer == layerId)
            {
                out.visible = a->value;
            }
        }
        else if (const auto* a = std::get_if<SetLayerBlend>(&cue.action))
        {
            if (a->layer == layerId)
            {
                out.blend = a->mode;
            }
        }
        else if (const auto* a = std::get_if<SetLayerPosition>(&cue.action))
        {
            if (a->layer == layerId)
            {
                out.x = a->x;
                out.y = a->y;
            }
        }
    }
}

Time PlaybackEngine::mediaTimeFor(const Layer& layer, const Time& showTime,
                                  const Time& sceneStart, bool& atEnd) const
{
    atEnd = false;
    if (layer.mediaId == kInvalidId)
    {
        return layer.inPoint;
    }

    // The media plays from its in-point, starting when the layer's scene
    // became active, at `rate` times show speed.
    const float rate = layer.rate > 0.0f ? layer.rate : 1.0f;
    Time elapsed = (showTime - sceneStart) * static_cast<double>(rate);
    if (elapsed.isNegative())
    {
        elapsed = Time::zero();
    }
    Time mediaTime = layer.inPoint + elapsed;

    // End bound: outPoint if set (zero = "to end of media", see Layer.h),
    // else the media's duration.
    Time duration;
    if (layer.outPoint > Time::zero())
    {
        duration = layer.outPoint;
    }
    else if (const auto asset = m_Media.find(layer.mediaId))
    {
        duration = (*asset)->metadata.duration;
    }

    if (duration.isValid() && duration > layer.inPoint)
    {
        const Time span = duration - layer.inPoint;
        if (mediaTime >= duration)
        {
            if (layer.loop)
            {
                // Wrap within [inPoint, duration).
                const Time overflow = mediaTime - duration;
                mediaTime = layer.inPoint + (overflow % span);
            }
            else
            {
                mediaTime = duration;
                atEnd = true;
            }
        }
    }
    return mediaTime;
}

void PlaybackEngine::setPreviewScene(int sceneIndex)
{
    if (sceneIndex >= 0 && sceneIndex < static_cast<int>(m_Show.scenes.size()))
    {
        m_PreviewScene = sceneIndex;
    }
}

bool PlaybackEngine::setSceneNow(Id sceneId)
{
    if (!m_Show.findScene(sceneId))
    {
        return false;
    }
    m_ForceScenes = std::map<Id, Time>{{sceneId, m_Time}};
    return true;
}

bool PlaybackEngine::toggleSceneNow(Id sceneId)
{
    if (!m_Show.findScene(sceneId))
    {
        return false;
    }
    if (!m_ForceScenes.has_value())
    {
        // Taking manual control from a timeline state: the toggle operates
        // on what the timeline shows. Toggling the timeline scene itself off
        // takes control with an empty set (no scene active); otherwise the
        // timeline scene stays active (starting where the timeline started
        // it) and the toggle adds (or removes) the given scene.
        m_ForceScenes = std::map<Id, Time>{};
        const Id timelineId = timelineSceneAt(m_Time);
        if (timelineId == sceneId)
        {
            return true; // empty manual set: nothing is active
        }
        if (timelineId != kInvalidId)
        {
            (*m_ForceScenes)[timelineId] = sceneStartFor(m_Time, timelineId);
        }
    }
    if (const auto it = m_ForceScenes->find(sceneId); it != m_ForceScenes->end())
    {
        m_ForceScenes->erase(it);
    }
    else
    {
        (*m_ForceScenes)[sceneId] = m_Time;
    }
    return true;
}

int PlaybackEngine::clearSceneNow()
{
    const int count = m_ForceScenes.has_value() ? static_cast<int>(m_ForceScenes->size()) : 0;
    m_ForceScenes.reset();
    return count;
}

bool PlaybackEngine::setLayerOpacity(Id layerId, float value)
{
    if (value < 0.0f) value = 0.f;
    if (value > 1.0f) value = 1.f;
    m_Overrides[layerId].opacity = value;
    return true;
}

bool PlaybackEngine::setLayerVisible(Id layerId, bool value)
{
    m_Overrides[layerId].visible = value;
    return true;
}

bool PlaybackEngine::setLayerBlend(Id layerId, BlendMode mode)
{
    m_Overrides[layerId].blend = mode;
    return true;
}

bool PlaybackEngine::setLayerPosition(Id layerId, float x, float y)
{
    m_Overrides[layerId].x = x;
    m_Overrides[layerId].y = y;
    return true;
}

bool PlaybackEngine::hasLayerOverride(Id layerId) const
{
    return m_Overrides.find(layerId) != m_Overrides.end();
}

bool PlaybackEngine::clearLayerOverride(Id layerId)
{
    return m_Overrides.erase(layerId) > 0;
}

int PlaybackEngine::clearLayerOverrides()
{
    const int count = static_cast<int>(m_Overrides.size());
    m_Overrides.clear();
    return count;
}

std::string PlaybackEngine::stateToString(PlayState s)
{
    switch (s)
    {
        case PlayState::Stopped: return "stopped";
        case PlayState::Playing: return "playing";
        case PlayState::Paused: return "paused";
    }
    return "unknown";
}

} // namespace mediamax
