#pragma once

#include <optional>
#include <string>

#include <gtk/gtk.h>
#include <nlohmann/json.hpp>

namespace fastsmgtk {

// Modal Open User Profile dialog driven by a user_profile event: a read-only
// review of the profile plus navigation and follow/mute/block buttons. Returns
// the chosen action name (view_posts, followers, following, browser, follow,
// mute, block, boosts) or nullopt if dismissed; the caller dispatches the
// command. The GTK mirror of windows/src/user_profile_dialog.cpp (no Lists /
// Report yet).
std::optional<std::string> show_user_profile_dialog(GtkWindow* parent, const nlohmann::json& e);

} // namespace fastsmgtk
