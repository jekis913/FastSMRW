#pragma once

#include <functional>
#include <optional>
#include <string>

#include <gtk/gtk.h>
#include <nlohmann/json.hpp>

namespace fastsmgtk {

// Alt+A @-mention autocomplete hook: given the partial handle under the caret,
// returns the chosen handle (without '@') to insert, or nullopt if the user
// cancelled. Supplied by the main window so the picker can reach the core;
// when unset the Autocomplete button is hidden.
using MentionPicker = std::function<std::optional<std::string>(const std::string& partial)>;

// Modal compose dialog (new/reply/quote/edit), driven by a compose_context
// event from the core. Returns the complete {"cmd":"post", ...} command to
// dispatch, or nullopt if the user cancelled. The GTK mirror of
// windows/src/compose_dialog.cpp: content warning, recipients, visibility,
// language, polls, scheduling, attachments (with alt text), a title-bar
// character counter, and mention autocomplete.
std::optional<nlohmann::json> show_compose_dialog(GtkWindow* parent, const nlohmann::json& ctx,
                                                  const MentionPicker& pick_mention = {});

} // namespace fastsmgtk
