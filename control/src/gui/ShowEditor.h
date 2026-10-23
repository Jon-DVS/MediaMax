#pragma once

// Show-file editing state for the control GUI.
//
// Wraps the core Show model plus file/dirty bookkeeping. The UI edits the
// Show directly; saving serializes through Show::toJson (the same codec the
// engine uses), so the GUI cannot emit an invalid document.

#include <string>

#include "mediamax/show/Show.h"

namespace mediamax::gui
{

class ShowEditor
{
public:
    ShowEditor(); // a fresh "Untitled Show" (30 fps, empty)

    // Reads and parses a show file. On failure the editor is left untouched.
    bool loadFile(const std::string& path, std::string& error);

    // Serializes the current show to `path` (pretty JSON) and marks it saved.
    bool saveFile(const std::string& path, std::string& error);

    // Replaces the content with an empty show.
    void newShow();

    Show& show() { return m_Show; }
    const Show& show() const { return m_Show; }

    const std::string& filePath() const { return m_Path; }
    bool dirty() const { return m_Dirty; }

    // Marks the content as edited (call after any Show mutation).
    void touch();

    // The current document as JSON (for show.loadJson) / pretty text.
    nlohmann::json documentJson() const { return m_Show.toJson(); }
    std::string documentText() const { return m_Show.toJson().dump(2); }

    // The next free (unique-within-file) id of each kind.
    static Id nextSceneId(const Show& show);
    static Id nextLayerId(const Show& show);
    static Id nextMediaId(const Show& show);

private:
    Show m_Show;
    std::string m_Path;
    bool m_Dirty = false;
};

} // namespace mediamax::gui
