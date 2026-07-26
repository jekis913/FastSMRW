#include "speech.hpp"

#include <speech-dispatcher/libspeechd.h>

#include <algorithm>
#include <cstring>

namespace fastsmgtk {

namespace {

constexpr const char* kOrcaName = "org.gnome.Orca.Service";
constexpr const char* kOrcaPath = "/org/gnome/Orca/Service";
constexpr const char* kOrcaIface = "org.gnome.Orca.Service";
constexpr const char* kOrcaModuleIface = "org.gnome.Orca.Module";

} // namespace

LinuxSpeaker::LinuxSpeaker() {
    spd_ = spd_open("FastSMRW", "main", nullptr, SPD_MODE_SINGLE);

    bus_ = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (!bus_)
        return;
    // Synchronous presence check so the very first announcement (which arrives
    // right after start) already picks the right backend; the watch keeps the
    // flag current if Orca starts or quits later.
    if (GVariant* r = g_dbus_connection_call_sync(
            bus_, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
            "NameHasOwner", g_variant_new("(s)", kOrcaName), G_VARIANT_TYPE("(b)"),
            G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, nullptr)) {
        gboolean present = FALSE;
        g_variant_get(r, "(b)", &present);
        g_variant_unref(r);
        if (present)
            on_orca_appeared();
    }
    watch_id_ = g_bus_watch_name(
        G_BUS_TYPE_SESSION, kOrcaName, G_BUS_NAME_WATCHER_FLAGS_NONE,
        +[](GDBusConnection*, const gchar*, const gchar*, gpointer user) {
            static_cast<LinuxSpeaker*>(user)->on_orca_appeared();
        },
        +[](GDBusConnection*, const gchar*, gpointer user) {
            auto* self = static_cast<LinuxSpeaker*>(user);
            self->orca_present_ = false;
            self->speech_module_path_.clear();
            self->candidates_.clear();
        },
        this, nullptr);
}

LinuxSpeaker::~LinuxSpeaker() {
    if (watch_id_)
        g_bus_unwatch_name(watch_id_);
    if (bus_)
        g_object_unref(bus_);
    if (spd_)
        spd_close(static_cast<SPDConnection*>(spd_));
}

void LinuxSpeaker::on_orca_appeared() {
    orca_present_ = true;
    speech_module_path_.clear(); // a different Orca version may have restarted
    resolve_speech_module();
}

void LinuxSpeaker::resolve_speech_module() {
    g_dbus_connection_call(
        bus_, kOrcaName, kOrcaPath, kOrcaIface, "ListModules", nullptr, G_VARIANT_TYPE("(as)"),
        G_DBUS_CALL_FLAGS_NONE, 2000, nullptr,
        +[](GObject* source, GAsyncResult* res, gpointer user) {
            auto* self = static_cast<LinuxSpeaker*>(user);
            GVariant* r =
                g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, nullptr);
            if (!r)
                return;
            std::vector<std::string> speechy;
            GVariantIter* it = nullptr;
            g_variant_get(r, "(as)", &it);
            const gchar* name = nullptr;
            while (g_variant_iter_loop(it, "&s", &name))
                if (strstr(name, "Speech"))
                    speechy.emplace_back(name);
            g_variant_iter_free(it);
            g_variant_unref(r);
            // Probe likely owners of InterruptSpeech first (current name, then
            // the pre-rename one), but fall back to every Speech* module.
            std::stable_sort(speechy.begin(), speechy.end(),
                             [](const std::string& a, const std::string& b) {
                                 auto rank = [](const std::string& s) {
                                     if (s == "SpeechManager")
                                         return 0;
                                     if (s == "SpeechAndVerbosityManager")
                                         return 1;
                                     return 2;
                                 };
                                 return rank(a) < rank(b);
                             });
            self->candidates_.clear();
            for (const auto& module : speechy)
                self->candidates_.push_back(std::string(kOrcaPath) + "/" + module);
            self->probe_next_candidate();
        },
        this);
}

void LinuxSpeaker::probe_next_candidate() {
    if (candidates_.empty())
        return;
    const std::string path = candidates_.front();
    candidates_.erase(candidates_.begin());
    struct Probe {
        LinuxSpeaker* self;
        std::string path;
    };
    g_dbus_connection_call(
        bus_, kOrcaName, path.c_str(), kOrcaModuleIface, "ListCommands", nullptr,
        G_VARIANT_TYPE("(a(ss))"), G_DBUS_CALL_FLAGS_NONE, 2000, nullptr,
        +[](GObject* source, GAsyncResult* res, gpointer user) {
            auto* probe = static_cast<Probe*>(user);
            LinuxSpeaker* self = probe->self;
            bool found = false;
            if (GVariant* r =
                    g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, nullptr)) {
                GVariantIter* it = nullptr;
                g_variant_get(r, "(a(ss))", &it);
                const gchar* cmd = nullptr;
                const gchar* desc = nullptr;
                while (g_variant_iter_loop(it, "(&s&s)", &cmd, &desc))
                    if (g_strcmp0(cmd, "InterruptSpeech") == 0)
                        found = true;
                g_variant_iter_free(it);
                g_variant_unref(r);
            }
            if (found) {
                self->speech_module_path_ = probe->path;
                self->candidates_.clear();
            } else {
                self->probe_next_candidate();
            }
            delete probe;
        },
        new Probe{this, path});
}

void LinuxSpeaker::orca_interrupt() {
    if (speech_module_path_.empty())
        return; // not resolved (yet): speak un-interrupted rather than guess
    g_dbus_connection_call(bus_, kOrcaName, speech_module_path_.c_str(), kOrcaModuleIface,
                           "ExecuteCommand", g_variant_new("(sb)", "InterruptSpeech", FALSE),
                           G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr,
                           nullptr);
}

void LinuxSpeaker::speak(const std::string& utf8, bool interrupt) {
    if (utf8.empty())
        return;
    if (bus_ && orca_present_) {
        // Async calls; D-Bus preserves ordering on a connection, so the
        // interrupt lands before the message.
        if (interrupt)
            orca_interrupt();
        g_dbus_connection_call(bus_, kOrcaName, kOrcaPath, kOrcaIface, "PresentMessage",
                               g_variant_new("(s)", utf8.c_str()), G_VARIANT_TYPE("(b)"),
                               G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
        return;
    }
    if (!spd_)
        return;
    auto* conn = static_cast<SPDConnection*>(spd_);
    if (interrupt)
        spd_cancel(conn); // cancels only this connection's speech
    spd_say(conn, SPD_TEXT, utf8.c_str());
}

void LinuxSpeaker::stop() {
    if (bus_ && orca_present_) {
        orca_interrupt();
        return;
    }
    if (spd_)
        spd_cancel(static_cast<SPDConnection*>(spd_));
}

} // namespace fastsmgtk
