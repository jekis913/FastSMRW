#pragma once

#include <optional>
#include <string>
#include <vector>

#include <gtk/gtk.h>

#include "fastsm/store/app_settings.hpp"

namespace fastsmgtk {

// Modal Settings dialog — a GtkNotebook mirroring the Windows property sheet's
// pages (General, Timelines, Audio, Earcons, Speech, Advanced, Confirmations,
// Behavior, Updates). The invisible-interface page is Windows-only, and the
// speech-template / movement-unit editors aren't built yet. Edits a copy of
// AppSettings; returns it if the user accepts (the caller serializes and sends
// update_settings), nullopt on cancel.
std::optional<fastsm::store::AppSettings>
show_settings_dialog(GtkWindow* parent, fastsm::store::AppSettings settings,
                     const std::vector<std::string>& soundpacks);

} // namespace fastsmgtk
