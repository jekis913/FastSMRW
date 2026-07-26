#include "media_player.hpp"

#include <algorithm>

#include <gst/gst.h>

namespace fastsmgtk {

struct MediaPlayback::Impl {
    GstElement* playbin = nullptr;
    bool done = false;
    int volume = 100;

    // Drain the bus; EOS or an error (bad codec, dead stream) marks us done.
    void poll_bus() {
        if (!playbin)
            return;
        GstBus* bus = gst_element_get_bus(playbin);
        while (GstMessage* msg = gst_bus_pop_filtered(
                   bus, static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR))) {
            done = true;
            gst_message_unref(msg);
        }
        gst_object_unref(bus);
    }
};

MediaPlayback::MediaPlayback() : impl_(new Impl) {
    static bool inited = false;
    if (!inited) {
        gst_init(nullptr, nullptr);
        inited = true;
    }
}

MediaPlayback::~MediaPlayback() {
    stop();
    delete impl_;
}

bool MediaPlayback::play(const std::string& url) {
    stop();
    impl_->done = false;
    impl_->playbin = gst_element_factory_make("playbin", nullptr);
    if (!impl_->playbin)
        return false;
    g_object_set(impl_->playbin, "uri", url.c_str(), nullptr);
    g_object_set(impl_->playbin, "volume", impl_->volume / 100.0, nullptr);
    if (gst_element_set_state(impl_->playbin, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        stop();
        return false;
    }
    return true;
}

void MediaPlayback::stop() {
    if (!impl_->playbin)
        return;
    gst_element_set_state(impl_->playbin, GST_STATE_NULL);
    gst_object_unref(impl_->playbin);
    impl_->playbin = nullptr;
}

bool MediaPlayback::active() const { return impl_->playbin != nullptr && !impl_->done; }

void MediaPlayback::seek(double delta_seconds) {
    if (!impl_->playbin)
        return;
    gint64 pos = 0;
    if (!gst_element_query_position(impl_->playbin, GST_FORMAT_TIME, &pos))
        return;
    gint64 target = pos + static_cast<gint64>(delta_seconds * GST_SECOND);
    if (target < 0)
        target = 0;
    gst_element_seek_simple(impl_->playbin, GST_FORMAT_TIME,
                            static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH |
                                                      GST_SEEK_FLAG_KEY_UNIT),
                            target);
}

void MediaPlayback::adjust_volume(int delta_pct) {
    impl_->volume = std::clamp(impl_->volume + delta_pct, 0, 100);
    if (impl_->playbin)
        g_object_set(impl_->playbin, "volume", impl_->volume / 100.0, nullptr);
}

bool MediaPlayback::toggle_pause() {
    if (!impl_->playbin)
        return false;
    GstState state = GST_STATE_NULL;
    gst_element_get_state(impl_->playbin, &state, nullptr, 0);
    const bool now_playing = state != GST_STATE_PLAYING;
    gst_element_set_state(impl_->playbin,
                          now_playing ? GST_STATE_PLAYING : GST_STATE_PAUSED);
    return now_playing;
}

bool MediaPlayback::completed() {
    impl_->poll_bus();
    return impl_->done;
}

int MediaPlayback::volume_pct() const { return impl_->volume; }

namespace {

struct PlayerCtx {
    MediaPlayback* player = nullptr;
    std::function<void(const std::string&)>* speak = nullptr;
    GtkWidget* dialog = nullptr;
};
PlayerCtx player_ctx;

} // namespace

bool show_media_player(GtkWindow* parent, const std::string& title, const std::string& url,
                       std::function<void(const std::string&)> speak) {
    MediaPlayback player;
    if (!player.play(url))
        return false;

    GtkWidget* dialog = gtk_dialog_new_with_buttons(
        title.empty() ? "Media Player" : title.c_str(), parent,
        static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT), "_Close",
        GTK_RESPONSE_CANCEL, nullptr);
    GtkWidget* label = gtk_label_new(
        "Playing. Space pauses, Left and Right seek, Up and Down change the volume, "
        "Escape closes.");
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_container_set_border_width(
        GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), 12);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), label, TRUE,
                       TRUE, 0);

    player_ctx = {&player, &speak, dialog};
    g_signal_connect(
        dialog, "key-press-event",
        G_CALLBACK(+[](GtkWidget*, GdkEventKey* event, gpointer) -> gboolean {
            switch (event->keyval) {
            case GDK_KEY_space:
                (*player_ctx.speak)(player_ctx.player->toggle_pause() ? "Playing" : "Paused");
                return TRUE;
            case GDK_KEY_Left:
                player_ctx.player->seek(-5);
                return TRUE;
            case GDK_KEY_Right:
                player_ctx.player->seek(5);
                return TRUE;
            case GDK_KEY_Up:
                player_ctx.player->adjust_volume(5);
                (*player_ctx.speak)(std::to_string(player_ctx.player->volume_pct()) +
                                    " percent");
                return TRUE;
            case GDK_KEY_Down:
                player_ctx.player->adjust_volume(-5);
                (*player_ctx.speak)(std::to_string(player_ctx.player->volume_pct()) +
                                    " percent");
                return TRUE;
            default:
                return FALSE;
            }
        }),
        nullptr);
    // Close on our own when the stream finishes.
    const guint timer = g_timeout_add(1000, +[](gpointer) -> gboolean {
        if (player_ctx.player && player_ctx.player->completed())
            gtk_dialog_response(GTK_DIALOG(player_ctx.dialog), GTK_RESPONSE_CANCEL);
        return G_SOURCE_CONTINUE; // removed once, after the dialog closes
    }, nullptr);

    gtk_widget_show_all(dialog);
    gtk_dialog_run(GTK_DIALOG(dialog));
    g_source_remove(timer);
    player.stop();
    gtk_widget_destroy(dialog);
    player_ctx = PlayerCtx{};
    return true;
}

} // namespace fastsmgtk
