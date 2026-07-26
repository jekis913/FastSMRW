//
// Linux entry point. Boots GTK, the speaker (Orca via D-Bus, else Speech
// Dispatcher) and the core,
// wires the event sink onto the GTK main loop, and runs. The Linux mirror of
// windows/src/main.cpp.
//

#include <filesystem>
#include <fstream>
#include <string>

#include <gtk/gtk.h>
#include <nlohmann/json.hpp>
#include <unistd.h>

#include "fastsm/capi/fastsm_core.h"
#include "fastsm/fastsm.hpp"
#include "fastsm/store/paths.hpp"

#include "main_window.hpp"
#include "speech.hpp"

namespace {

// Whether to show the window at startup: honor the remembered ToggleWindow
// state, but never start hidden unless an invisible mode is on (else there'd
// be no way to bring it back). Mirrors windows/src/main.cpp.
bool should_show_at_startup() {
    try {
        std::ifstream in(fastsm::store::config_dir() / "config.json");
        if (!in)
            return true;
        nlohmann::json cfg;
        in >> cfg;
        const nlohmann::json s = cfg.value("settings", nlohmann::json::object());
        const bool shown = s.value("window_shown", true);
        const bool invisible = s.value("invisible_mode", std::string("off")) != "off";
        return shown || !invisible;
    } catch (...) {
        return true;
    }
}

// The folder that holds the running executable (bundled soundpacks live here).
std::filesystem::path exe_dir() {
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0)
        return {};
    buf[n] = '\0';
    return std::filesystem::path(buf).parent_path();
}

} // namespace

namespace {

constexpr const char* kBusName = "me.masonasons.FastSMRW";
constexpr const char* kBusPath = "/me/masonasons/FastSMRW";

// Single instance via the session bus (the Linux analogue of the Windows
// named mutex + window message): the first instance owns the name and listens
// for "Show"; a second launch emits it and exits, surfacing the first window.
bool claim_single_instance(GDBusConnection* bus) {
    if (!bus)
        return true; // no session bus (odd setups): just run
    GVariant* r = g_dbus_connection_call_sync(
        bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
        "RequestName", g_variant_new("(su)", kBusName, 4u /* DO_NOT_QUEUE */),
        G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, nullptr);
    if (!r)
        return true;
    guint32 reply = 0;
    g_variant_get(r, "(u)", &reply);
    g_variant_unref(r);
    if (reply == 1 /* PRIMARY_OWNER */)
        return true;
    g_dbus_connection_emit_signal(bus, nullptr, kBusPath, kBusName, "Show", nullptr, nullptr);
    g_dbus_connection_flush_sync(bus, nullptr, nullptr);
    return false;
}

} // namespace

int main(int argc, char** argv) {
    gtk_init(&argc, &argv);

    GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (!claim_single_instance(bus))
        return 0; // the running instance was asked to show itself

    fastsmgtk::LinuxSpeaker speaker;
    fastsmgtk::MainWindow window;
    window.set_speaker(&speaker);

    // Build the core: the data folder (config.json + cache + user soundpacks)
    // and where the bundled soundpacks ship (next to the executable).
    nlohmann::json cfg;
    cfg["config_dir"] = fastsm::store::config_dir().string();
    cfg["soundpacks_dir"] = (exe_dir() / "soundpacks").string();
    cfg["user_agent"] = std::string("FastSMRW-Linux/") + fastsm::version();
    const std::string cfg_str = cfg.dump();

    fastsm_core* core = fastsm_core_create(cfg_str.c_str());
    if (!core)
        return 1;
    window.set_core(core);
    fastsm_core_set_event_sink(core, &fastsmgtk::MainWindow::event_sink, &window);

    if (should_show_at_startup())
        gtk_widget_show_all(window.window());
    else
        gtk_widget_realize(window.window()); // hidden start: exists for the tray/hotkeys
    if (bus)
        g_dbus_connection_signal_subscribe(
            bus, nullptr, kBusName, "Show", kBusPath, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
            +[](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*,
                GVariant*, gpointer user) {
                gtk_window_present(GTK_WINDOW(static_cast<GtkWidget*>(user)));
            },
            window.window(), nullptr);

    const std::string start = R"({"cmd":"start"})";
    fastsm_core_dispatch(core, start.c_str(), start.size());

    gtk_main();

    fastsm_core_set_event_sink(core, nullptr, nullptr);
    fastsm_core_destroy(core); // stops the core threads
    return 0;
}
