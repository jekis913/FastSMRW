#pragma once

#include <functional>
#include <string>
#include <vector>

#include <gtk/gtk.h>

namespace fastsmgtk {

// A streaming audio player over GStreamer playbin (nothing downloaded to
// disk) — the Linux analogue of the Windows DirectShow MediaPlayback.
// Reusable both by the pop-up player window and for windowless "background"
// playback owned by the main window. Not copyable.
class MediaPlayback {
public:
    MediaPlayback();
    ~MediaPlayback();
    MediaPlayback(const MediaPlayback&) = delete;
    MediaPlayback& operator=(const MediaPlayback&) = delete;

    bool play(const std::string& url); // start streaming; false if it can't render
    void stop();
    bool active() const;
    void seek(double delta_seconds);
    void set_volume(int pct);         // absolute, 0-100
    int adjust_volume(int delta_pct); // returns the resulting level
    bool toggle_pause(); // returns true if now playing
    bool completed();    // playback reached the end (or errored out)
    int volume_pct() const;

    // Send playback to a specific output device, by the name reported by
    // media_output_devices() ("" = whatever the system is using). Takes effect
    // on the next play(); a stream already running keeps the device it opened on.
    void set_output_device(std::string name);

private:
    struct Impl;
    Impl* impl_;
};

// The output devices media can play through, by display name. The system
// default isn't in the list — the settings page offers that as its own choice.
std::vector<std::string> media_output_devices();

// Where a player starts out, and how it reports a volume change back so the
// level survives to the next thing you play.
struct MediaPlayerOptions {
    std::string device;                 // "" = the system's own output device
    int volume = 100;                   // 0-100
    std::function<void(int)> on_volume; // the user pressed Up/Down: persist this
};

// A keys-only pop-up player (owns its own MediaPlayback): Space play/pause,
// Left/Right seek, Up/Down volume, Escape stop+close. `speak` announces
// feedback. Returns false if the stream couldn't be rendered (caller opens the
// system player instead).
bool show_media_player(GtkWindow* parent, const std::string& title, const std::string& url,
                       std::function<void(const std::string&)> speak,
                       MediaPlayerOptions options = {});

} // namespace fastsmgtk
