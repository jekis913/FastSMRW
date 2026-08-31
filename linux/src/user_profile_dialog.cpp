#include "user_profile_dialog.hpp"

#include <vector>

using nlohmann::json;

namespace fastsmgtk {

namespace {
constexpr int kActionBase = 100;
}

std::optional<std::string> show_user_profile_dialog(GtkWindow* parent, const json& e) {
    const bool known = e.value("has_relationship", false);
    const bool following = e.value("following", false);
    const bool requested = e.value("requested", false);
    const bool muting = e.value("muting", false);
    const bool blocking = e.value("blocking", false);
    const bool showing_reblogs = e.value("showing_reblogs", true);
    const bool can_hide_boosts = e.value("can_hide_boosts", false);
    const bool has_url = !e.value("url", std::string{}).empty();

    GtkWidget* dialog = gtk_dialog_new_with_buttons(
        "User Profile", parent,
        static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT), "_Close",
        GTK_RESPONSE_CANCEL, nullptr);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 560, 480);

    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), box, TRUE, TRUE,
                       0);

    GtkWidget* text_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(text_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text_view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_accepts_tab(GTK_TEXT_VIEW(text_view), FALSE);
    atk_object_set_name(gtk_widget_get_accessible(text_view), "Profile");
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view)),
                             e.value("text", std::string{}).c_str(), -1);
    GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(scroll), text_view);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);

    std::vector<std::string> actions;
    // A plain grid, NOT GtkFlowBox: the flow box wraps each button in its own
    // focusable child accessible, so screen readers announce every button twice.
    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 6);
    auto add_action = [&](const std::string& action, const std::string& label) {
        GtkWidget* button = gtk_button_new_with_mnemonic(label.c_str());
        const int response = kActionBase + static_cast<int>(actions.size());
        g_object_set_data(G_OBJECT(button), "fastsm-response", GINT_TO_POINTER(response));
        g_signal_connect(button, "clicked",
                         G_CALLBACK(+[](GtkButton* b, gpointer d) {
                             gtk_dialog_response(GTK_DIALOG(d),
                                                 GPOINTER_TO_INT(g_object_get_data(
                                                     G_OBJECT(b), "fastsm-response")));
                         }),
                         dialog);
        actions.push_back(action);
        const int index = static_cast<int>(actions.size()) - 1;
        gtk_grid_attach(GTK_GRID(grid), button, index % 3, index / 3, 1, 1);
    };

    add_action("view_posts", "View _Posts");
    // Direct messages need the direct visibility level (Mastodon only).
    if (e.value("can_message", false))
        add_action("message", "Send a Messa_ge…");
    add_action("followers", "F_ollowers");
    add_action("following", "Follo_wing");
    if (has_url)
        add_action("browser", "Open in Bro_wser");
    if (known) {
        add_action("follow", requested    ? "Cancel Follow _Request"
                             : following  ? "Un_follow"
                                          : "_Follow");
        add_action("mute", muting ? "Un_mute" : "_Mute");
        add_action("block", blocking ? "Un_block" : "_Block");
        if (can_hide_boosts)
            add_action("boosts", showing_reblogs ? "_Hide Their Boosts" : "S_how Their Boosts");
        if (e.value("can_use_lists", false))
            add_action("lists", "_Lists…");
    }
    add_action("report", "_Report…");
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);

    gtk_widget_show_all(dialog);
    gtk_widget_grab_focus(text_view);

    std::optional<std::string> result;
    const int response = gtk_dialog_run(GTK_DIALOG(dialog));
    const int index = response - kActionBase;
    if (index >= 0 && index < static_cast<int>(actions.size()))
        result = actions[static_cast<size_t>(index)];
    gtk_widget_destroy(dialog);
    return result;
}

} // namespace fastsmgtk
