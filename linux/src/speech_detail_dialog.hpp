#pragma once

#include <string>
#include <vector>

#include <gtk/gtk.h>

namespace fastsmgtk {

// One row of the reorderable checked-list modal (speech fields, movement
// units) — the GTK mirror of the Windows IDD_SPEECH_DETAIL/IDD_MOVEMENT_UNITS
// shared dialog. Space toggles the focused row, Ctrl+Up/Ctrl+Down (or the Move
// buttons) reorder it; with_wrap adds the per-row "spoken before/after" fields
// and the no-separator checkbox.
struct SpeechDetailRow {
    int id = 0;
    std::string label;
    bool enabled = true;
    std::string before;
    std::string after;
    bool no_sep_after = false;
};

bool run_speech_detail(GtkWindow* parent, const std::string& title,
                       std::vector<SpeechDetailRow>& rows, bool with_wrap);

} // namespace fastsmgtk
