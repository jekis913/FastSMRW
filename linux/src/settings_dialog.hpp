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
// The lists the Audio page offers, gathered by the caller: soundpacks and the
// sound-effect devices come from the core, the media devices from GStreamer.
// An empty device list just leaves that picker at "System default".
struct AudioChoices {
    std::vector<std::string> soundpacks;
    std::vector<std::string> sound_devices;
    std::vector<std::string> media_devices;
};

std::optional<fastsm::store::AppSettings>
show_settings_dialog(GtkWindow* parent, fastsm::store::AppSettings settings,
                     const AudioChoices& audio);

} // namespace fastsmgtk
