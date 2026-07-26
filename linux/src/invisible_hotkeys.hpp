#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fastsmgtk {

// Invisible-interface driver: system-wide hotkeys via raw evdev, the same
// mechanism as the original FastSM's linux_shortcuts.py. Reads every keyboard
// in /dev/input (found via /proc/bus/input/devices), tracks modifier state,
// and fires the bound action for canonical core key-strings
// ("control+win+r"). Caveats inherited from the original: the user must be in
// the `input` group, and keycodes are physical QWERTY positions. Note: WSLg
// exposes no evdev keyboards, so this only works on real Linux systems.
class InvisibleHotkeys {
public:
    using ActionFn = std::function<void(const std::string& action)>; // main thread

    ~InvisibleHotkeys() { stop(); }

    // Starts listening with the given canonical-keystring -> action bindings.
    // Returns false when no keyboard device could be opened (not installed /
    // no permission); `error` then says why, for an announcement.
    bool start(std::unordered_map<std::string, std::string> bindings, ActionFn on_action,
               std::string& error);
    void stop();
    bool running() const { return running_.load(); }

private:
    void run();

    std::unordered_map<std::string, std::string> bindings_;
    ActionFn on_action_;
    std::vector<int> fds_;
    int wake_pipe_[2] = {-1, -1};
    std::thread thread_;
    std::atomic<bool> running_{false};
};

} // namespace fastsmgtk
