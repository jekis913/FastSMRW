#include "report_dialog.hpp"

#include <vector>

namespace fastsmgtk {

std::optional<ReportInput> show_report_dialog(GtkWindow* parent, bool remote) {
    GtkWidget* dialog = gtk_dialog_new_with_buttons(
        "Report", parent,
        static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT), "_Cancel",
        GTK_RESPONSE_CANCEL, "_Report", GTK_RESPONSE_OK, nullptr);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 440, 360);

    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), box, TRUE, TRUE,
                       0);

    const std::vector<std::pair<std::string, std::string>> categories = {
        {"spam", "It's spam"},
        {"violation", "It violates server rules"},
        {"legal", "It's illegal"},
        {"other", "Something else"}};
    GtkWidget* cat_label = gtk_label_new_with_mnemonic("_Reason:");
    gtk_label_set_xalign(GTK_LABEL(cat_label), 0.0f);
    GtkWidget* combo = gtk_combo_box_text_new();
    for (const auto& [token, label] : categories)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), label.c_str());
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
    gtk_label_set_mnemonic_widget(GTK_LABEL(cat_label), combo);
    gtk_box_pack_start(GTK_BOX(box), cat_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), combo, FALSE, FALSE, 0);

    GtkWidget* comment_label = gtk_label_new_with_mnemonic("Additional _comments:");
    gtk_label_set_xalign(GTK_LABEL(comment_label), 0.0f);
    GtkWidget* text_view = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text_view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_accepts_tab(GTK_TEXT_VIEW(text_view), FALSE);
    gtk_label_set_mnemonic_widget(GTK_LABEL(comment_label), text_view);
    atk_object_set_name(gtk_widget_get_accessible(text_view), "Additional comments");
    GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(scroll), text_view);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_pack_start(GTK_BOX(box), comment_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);

    GtkWidget* forward_check =
        gtk_check_button_new_with_mnemonic("_Forward the report to their server too");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(forward_check), remote);
    gtk_widget_set_sensitive(forward_check, remote);
    gtk_box_pack_start(GTK_BOX(box), forward_check, FALSE, FALSE, 0);

    gtk_widget_show_all(dialog);
    gtk_widget_grab_focus(combo);

    std::optional<ReportInput> result;
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        ReportInput r;
        const int sel = gtk_combo_box_get_active(GTK_COMBO_BOX(combo));
        r.category = categories[static_cast<size_t>(sel < 0 ? 0 : sel)].first;
        GtkTextBuffer* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view));
        GtkTextIter start, end;
        gtk_text_buffer_get_bounds(buffer, &start, &end);
        gchar* text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
        r.comment = text ? text : "";
        g_free(text);
        r.forward = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(forward_check));
        result = std::move(r);
    }
    gtk_widget_destroy(dialog);
    return result;
}

} // namespace fastsmgtk
