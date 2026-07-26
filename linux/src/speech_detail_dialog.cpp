#include "speech_detail_dialog.hpp"

#include <cstdlib>
#include <utility>

namespace fastsmgtk {

namespace {

// Modal + single-threaded, so one static context (like the Windows DetailCtx).
struct Ctx {
    std::vector<SpeechDetailRow>* rows = nullptr;
    GtkListStore* store = nullptr;
    GtkTreeView* view = nullptr;
    GtkWidget* before_entry = nullptr;
    GtkWidget* after_entry = nullptr;
    GtkWidget* no_sep_check = nullptr;
    int loaded = -1; // row whose wrap fields are currently shown
};
Ctx ctx;

int cursor_row() {
    GtkTreePath* path = nullptr;
    gtk_tree_view_get_cursor(ctx.view, &path, nullptr);
    if (!path)
        return -1;
    const int row = gtk_tree_path_get_indices(path)[0];
    gtk_tree_path_free(path);
    return row;
}

// Stash the currently-shown wrap edits back into their row.
void save_loaded_wrap() {
    if (!ctx.before_entry || ctx.loaded < 0 ||
        ctx.loaded >= static_cast<int>(ctx.rows->size()))
        return;
    auto& r = (*ctx.rows)[static_cast<size_t>(ctx.loaded)];
    r.before = gtk_entry_get_text(GTK_ENTRY(ctx.before_entry));
    r.after = gtk_entry_get_text(GTK_ENTRY(ctx.after_entry));
    r.no_sep_after = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ctx.no_sep_check));
}

void load_sel_wrap() {
    if (!ctx.before_entry)
        return;
    const int row = cursor_row();
    ctx.loaded = row;
    if (row < 0 || row >= static_cast<int>(ctx.rows->size()))
        return;
    const auto& r = (*ctx.rows)[static_cast<size_t>(row)];
    gtk_entry_set_text(GTK_ENTRY(ctx.before_entry), r.before.c_str());
    gtk_entry_set_text(GTK_ENTRY(ctx.after_entry), r.after.c_str());
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ctx.no_sep_check), r.no_sep_after);
}

void refill(int cursor) {
    gtk_list_store_clear(ctx.store);
    for (const auto& r : *ctx.rows) {
        GtkTreeIter iter;
        gtk_list_store_append(ctx.store, &iter);
        gtk_list_store_set(ctx.store, &iter, 0, r.enabled ? TRUE : FALSE, 1, r.label.c_str(),
                           -1);
    }
    if (cursor >= 0 && cursor < static_cast<int>(ctx.rows->size())) {
        ctx.loaded = -1; // the cursor move below reloads the wrap fields
        GtkTreePath* path = gtk_tree_path_new_from_indices(cursor, -1);
        gtk_tree_view_set_cursor(ctx.view, path, nullptr, FALSE);
        gtk_tree_path_free(path);
    }
}

void toggle_row(int row) {
    if (row < 0 || row >= static_cast<int>(ctx.rows->size()))
        return;
    auto& r = (*ctx.rows)[static_cast<size_t>(row)];
    r.enabled = !r.enabled;
    GtkTreeIter iter;
    if (gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(ctx.store), &iter, nullptr, row))
        gtk_list_store_set(ctx.store, &iter, 0, r.enabled ? TRUE : FALSE, -1);
}

void move_row(int delta) {
    const int row = cursor_row();
    const int to = row + delta;
    if (row < 0 || to < 0 || to >= static_cast<int>(ctx.rows->size()))
        return;
    save_loaded_wrap();
    std::swap((*ctx.rows)[static_cast<size_t>(row)], (*ctx.rows)[static_cast<size_t>(to)]);
    refill(to);
    gtk_widget_grab_focus(GTK_WIDGET(ctx.view));
}

} // namespace

bool run_speech_detail(GtkWindow* parent, const std::string& title,
                       std::vector<SpeechDetailRow>& rows, bool with_wrap) {
    GtkWidget* dialog = gtk_dialog_new_with_buttons(
        title.c_str(), parent,
        static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT), "_Cancel",
        GTK_RESPONSE_CANCEL, "Move _Up", 100, "Move _Down", 101, "_OK", GTK_RESPONSE_OK, nullptr);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 420, with_wrap ? 560 : 420);

    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), box, TRUE, TRUE,
                       0);

    GtkListStore* store = gtk_list_store_new(2, G_TYPE_BOOLEAN, G_TYPE_STRING);
    GtkWidget* view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store);
    GtkCellRenderer* toggle = gtk_cell_renderer_toggle_new();
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), gtk_tree_view_column_new_with_attributes(
                                                         "", toggle, "active", 0, nullptr));
    gtk_tree_view_append_column(
        GTK_TREE_VIEW(view), gtk_tree_view_column_new_with_attributes(
                                 "", gtk_cell_renderer_text_new(), "text", 1, nullptr));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(view), FALSE);
    atk_object_set_name(gtk_widget_get_accessible(view), title.c_str());
    GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(scroll), view);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);

    ctx = Ctx{};
    ctx.rows = &rows;
    ctx.store = store;
    ctx.view = GTK_TREE_VIEW(view);

    if (with_wrap) {
        GtkWidget* before_label = gtk_label_new_with_mnemonic("Spoken _before this field:");
        gtk_label_set_xalign(GTK_LABEL(before_label), 0.0f);
        ctx.before_entry = gtk_entry_new();
        gtk_label_set_mnemonic_widget(GTK_LABEL(before_label), ctx.before_entry);
        GtkWidget* after_label = gtk_label_new_with_mnemonic("Spoken _after this field:");
        gtk_label_set_xalign(GTK_LABEL(after_label), 0.0f);
        ctx.after_entry = gtk_entry_new();
        gtk_label_set_mnemonic_widget(GTK_LABEL(after_label), ctx.after_entry);
        ctx.no_sep_check =
            gtk_check_button_new_with_mnemonic("_No separator after this field");
        gtk_box_pack_start(GTK_BOX(box), before_label, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), ctx.before_entry, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), after_label, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), ctx.after_entry, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), ctx.no_sep_check, FALSE, FALSE, 0);
    }

    g_signal_connect(toggle, "toggled",
                     G_CALLBACK(+[](GtkCellRendererToggle*, gchar* path_str, gpointer) {
                         toggle_row(atoi(path_str));
                     }),
                     nullptr);
    g_signal_connect(view, "cursor-changed", G_CALLBACK(+[](GtkTreeView*, gpointer) {
                         save_loaded_wrap();
                         load_sel_wrap();
                     }),
                     nullptr);
    g_signal_connect(view, "key-press-event",
                     G_CALLBACK(+[](GtkWidget*, GdkEventKey* event, gpointer) -> gboolean {
                         const guint state =
                             event->state & gtk_accelerator_get_default_mod_mask();
                         if (event->keyval == GDK_KEY_space && state == 0) {
                             toggle_row(cursor_row());
                             return TRUE;
                         }
                         if ((event->keyval == GDK_KEY_Up || event->keyval == GDK_KEY_Down) &&
                             state == GDK_CONTROL_MASK) {
                             move_row(event->keyval == GDK_KEY_Up ? -1 : +1);
                             return TRUE;
                         }
                         return FALSE;
                     }),
                     nullptr);

    refill(0);
    gtk_widget_show_all(dialog);
    load_sel_wrap();
    gtk_widget_grab_focus(view);

    int response;
    while ((response = gtk_dialog_run(GTK_DIALOG(dialog))) == 100 || response == 101)
        move_row(response == 100 ? -1 : +1);
    const bool ok = response == GTK_RESPONSE_OK;
    if (ok)
        save_loaded_wrap(); // capture the currently-shown edits
    gtk_widget_destroy(dialog);
    ctx = Ctx{};
    return ok;
}

} // namespace fastsmgtk
