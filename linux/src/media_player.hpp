#pragma once

#include <functional>
#include <string>

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
    void adjust_volume(int delta_pct);
    bool toggle_pause(); // returns true if now playing
    bool completed();    // playback reached the end (or errored out)
    int volume_pct() const;

private:
    struct Impl;
    Impl* impl_;
};

// A keys-only pop-up player (owns its own MediaPlayback): Space play/pause,
// Left/Right seek, Up/Down volume, Escape stop+close. `speak` announces
// feedback. Returns false if the stream couldn't be rendered (caller opens the
// system player instead).
bool show_media_player(GtkWindow* parent, const std::string& title, const std::string& url,
                       std::function<void(const std::string&)> speak);

} // namespace fastsmgtk
