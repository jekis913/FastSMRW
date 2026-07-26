#pragma once

#include <optional>
#include <string>
#include <vector>

#include <gtk/gtk.h>
#include <nlohmann/json.hpp>

namespace fastsmgtk {

// What the Post Info dialog returned: an action name, plus the chosen option
// indexes when the action is "vote". Action names: reply, boost, favorite,
// quote, browser, links, thread, author, delete, mute_conv, favorited_by,
// boosted_by, vote. The caller dispatches the matching command.
struct PostInfoResult {
    std::string action;
    std::vector<int> choices; // set only for "vote"
};

// Modal Post Info dialog driven by a post_info event from the core: a
// read-only review of the post plus action buttons, and (when the poll is
// votable) voting controls. The GTK mirror of windows/src/post_info_dialog.cpp
// (no Report yet).
std::optional<PostInfoResult> show_post_info_dialog(GtkWindow* parent, const nlohmann::json& e);

} // namespace fastsmgtk
