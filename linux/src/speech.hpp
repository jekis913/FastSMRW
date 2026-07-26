#pragma once

#include <string>
#include <vector>

#include <gio/gio.h>

#include "fastsm/speech/speaker.hpp"

namespace fastsmgtk {

// Speaker for Linux — the analogue of the Windows UniversalSpeech backend.
// When Orca is running, announcements are routed through Orca's D-Bus remote
// controller (org.gnome.Orca.Service PresentMessage) so they come out in the
// screen reader's voice, interleaved correctly with what Orca is reading.
// Without Orca it falls back to speaking directly through Speech Dispatcher
// (libspeechd), so the app still talks for users running it standalone.
// Degrades to a no-op when neither is available.
class LinuxSpeaker : public fastsm::speech::Speaker {
public:
    LinuxSpeaker();
    ~LinuxSpeaker() override;

    void speak(const std::string& utf8, bool interrupt) override;
    void stop() override;

private:
    void on_orca_appeared();
    // The module that owns InterruptSpeech has been renamed across Orca
    // versions (SpeechAndVerbosityManager -> SpeechManager), so it's resolved
    // at runtime: ListModules, then probe each Speech* module's ListCommands
    // for InterruptSpeech. Until resolution lands, speaks go out un-interrupted.
    void resolve_speech_module();
    void probe_next_candidate();
    void orca_interrupt();

    void* spd_ = nullptr; // SPDConnection*; null when speechd is unavailable
    GDBusConnection* bus_ = nullptr;
    guint watch_id_ = 0;
    bool orca_present_ = false;      // maintained by a D-Bus name watch (main thread)
    std::string speech_module_path_; // resolved object path; empty until known
    std::vector<std::string> candidates_; // module paths still to probe
};

} // namespace fastsmgtk
