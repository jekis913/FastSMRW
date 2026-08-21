#pragma once

#include <functional>
#include <string>
#include <vector>

#include <windows.h>

namespace fastsmui {

// A streaming audio player over DirectShow: it renders an HTTP URL progressively
// (nothing is downloaded to disk). Reusable both by the pop-up player window and
// for windowless "background" playback owned by the main window. Manages its own
// COM apartment for the graph's lifetime. Not copyable.
class MediaPlayback {
public:
    MediaPlayback();
    ~MediaPlayback();
    MediaPlayback(const MediaPlayback&) = delete;
    MediaPlayback& operator=(const MediaPlayback&) = delete;

    bool play(const std::wstring& url); // start streaming; false if it can't render
    void stop();
    bool active() const;
    void seek(double delta_seconds);
    void set_volume(int pct);         // absolute, 0-100
    int adjust_volume(int delta_pct); // returns the resulting level
    bool toggle_pause(); // returns true if now playing
    bool completed();    // playback reached the end (or aborted)
    double position() const;
    double duration() const;
    int volume_pct() const;

    // Send playback to a specific output device, by the name reported by
    // media_output_devices() ("" = whatever the system is using). Takes effect
    // on the next play(); a stream already running keeps the device it opened on.
    void set_output_device(std::wstring name);

private:
    struct Impl;
    Impl* impl_;
};

// The output devices media can play through, by friendly name. The system
// default isn't in the list — the settings page offers that as its own choice.
std::vector<std::wstring> media_output_devices();

// Where a player starts out, and how it reports a volume change back so the
// level survives to the next thing you play.
struct MediaPlayerOptions {
    std::wstring device;                // "" = the system's own output device
    int volume = 100;                   // 0-100
    std::function<void(int)> on_volume; // the user pressed Up/Down: persist this
};

// A keys-only pop-up player (owns its own MediaPlayback): Space play/pause,
// Left/Right seek, Up/Down volume, Escape stop+close. `speak` announces feedback.
// Returns false if the stream couldn't be rendered (caller opens the system
// player instead).
bool show_media_player(HWND parent, HINSTANCE inst, const std::wstring& title,
                       const std::wstring& url, std::function<void(const std::wstring&)> speak,
                       MediaPlayerOptions options = {});

} // namespace fastsmui
