#include "gui/ShowEditor.h"

#include <algorithm>

#include "mediamax/util/File.h"

namespace mediamax::gui
{

ShowEditor::ShowEditor()
{
    newShow();
}

bool ShowEditor::loadFile(const std::string& path, std::string& error)
{
    auto file = readFile(path);
    if (!file)
    {
        error = "cannot read " + path;
        return false;
    }
    nlohmann::json parsed = nlohmann::json::parse(*file, nullptr, false);
    if (parsed.is_discarded())
    {
        error = "invalid JSON in " + path;
        return false;
    }
    auto show = Show::fromJson(parsed, error);
    if (!show)
    {
        // `error` is filled in by Show::fromJson.
        return false;
    }
    m_Show = std::move(*show);
    m_Path = path;
    m_Dirty = false;
    return true;
}

bool ShowEditor::saveFile(const std::string& path, std::string& error)
{
    if (!writeFile(path, documentText()))
    {
        error = "cannot write " + path;
        return false;
    }
    m_Path = path;
    m_Dirty = false;
    return true;
}

void ShowEditor::newShow()
{
    m_Show = Show{};
    m_Show.name = "Untitled Show";
    m_Path.clear();
    m_Dirty = false;
}

void ShowEditor::touch()
{
    m_Dirty = true;
    ++m_Generation;
    m_LastTouched = std::chrono::steady_clock::now();
}

Id ShowEditor::nextSceneId(const Show& show)
{
    Id next = 1;
    for (const auto& scene : show.scenes)
    {
        next = std::max(next, scene.id + 1);
    }
    return next;
}

Id ShowEditor::nextLayerId(const Show& show)
{
    Id next = 1;
    for (const auto& scene : show.scenes)
    {
        for (const auto& layer : scene.layers)
        {
            next = std::max(next, layer.id + 1);
        }
    }
    return next;
}

Id ShowEditor::nextMediaId(const Show& show)
{
    Id next = 1;
    for (const auto& ref : show.media)
    {
        next = std::max(next, ref.id + 1);
    }
    return next;
}

} // namespace mediamax::gui
