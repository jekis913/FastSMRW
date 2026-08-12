#include "keymap_manager_dialog.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

using nlohmann::json;

namespace fastsmgtk {

namespace {

bool valid_keymap_name(const std::string& name) {
    if (name.empty())
        return false;
    for (char c : name)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_')
            return false;
    return true;
}

void message_box(GtkWindow* parent, const char* title, const std::string& text) {
    GtkWidget* d = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO,
                                          GTK_BUTTONS_OK, "%s", text.c_str());
    gtk_window_set_title(GTK_WINDOW(d), title);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

bool yes_no(GtkWindow* parent, const char* title, const std::string& text) {
    GtkWidget* d = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
                                          GTK_BUTTONS_YES_NO, "%s", text.c_str());
    gtk_window_set_title(GTK_WINDOW(d), title);
    const int r = gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    return r == GTK_RESPONSE_YES;
}

} // namespace

std::string format_key_display(const std::string& key) {
    if (key.empty())
        return "(unbound)";
    std::string out;
    std::stringstream ss(key);
    std::string chunk;
    while (std::getline(ss, chunk, '+')) {
        if (!out.empty())
            out += "+";
        if (chunk == "control")
            out += "Ctrl";
        else if (chunk == "alt")
            out += "Alt";
        else if (chunk == "shift")
            out += "Shift";
        else if (chunk == "win")
            out += "Win";
        else if (chunk.size() == 1)
            out += static_cast<char>(std::toupper(static_cast<unsigned char>(chunk[0])));
        else {
            chunk[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(chunk[0])));
            out += chunk;
        }
    }
    return out;
}

std::string capture_key_binding(GtkWindow* parent, const std::string& current_key) {
    GtkWidget* dialog = gtk_dialog_new_with_buttons(
        "Set binding", parent,
        static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT), "_Cancel",
        GTK_RESPONSE_CANCEL, "_OK", GTK_RESPONSE_OK, nullptr);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);

    GtkWidget* current = gtk_label_new(("Current: " + format_key_display(current_key)).c_str());
    gtk_label_set_xalign(GTK_LABEL(current), 0.0f);
    gtk_box_pack_start(GTK_BOX(box), current, FALSE, FALSE, 0);

    // Parse the current canonical string to prefill.
    bool ctrl = false, alt = false, shift = false, win = false;
    std::string base;
    {
        std::stringstream ss(current_key);
        std::string chunk;
        while (std::getline(ss, chunk, '+')) {
            if (chunk == "control")
                ctrl = true;
            else if (chunk == "alt")
                alt = true;
            else if (chunk == "shift")
                shift = true;
            else if (chunk == "win")
                win = true;
            else
                base = chunk;
        }
    }
    GtkWidget* mod_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget* ctrl_check = gtk_check_button_new_with_mnemonic("_Ctrl");
    GtkWidget* alt_check = gtk_check_button_new_with_mnemonic("_Alt");
    GtkWidget* shift_check = gtk_check_button_new_with_mnemonic("_Shift");
    GtkWidget* win_check = gtk_check_button_new_with_mnemonic("_Win");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ctrl_check), ctrl);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(alt_check), alt);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(shift_check), shift);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(win_check), win);
    for (GtkWidget* w : {ctrl_check, alt_check, shift_check, win_check})
        gtk_box_pack_start(GTK_BOX(mod_row), w, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), mod_row, FALSE, FALSE, 0);

    GtkWidget* key_label =
        gtk_label_new_with_mnemonic("_Key (e.g. t, /, up, return, delete, f5):");
    gtk_label_set_xalign(GTK_LABEL(key_label), 0.0f);
    GtkWidget* key_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(key_entry), base.c_str());
    gtk_entry_set_activates_default(GTK_ENTRY(key_entry), TRUE);
    gtk_label_set_mnemonic_widget(GTK_LABEL(key_label), key_entry);
    gtk_box_pack_start(GTK_BOX(box), key_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), key_entry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), box, TRUE, TRUE,
                       0);
    gtk_widget_show_all(dialog);
    gtk_widget_grab_focus(key_entry);

    std::string result;
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        std::string key = gtk_entry_get_text(GTK_ENTRY(key_entry));
        // lowercase + trim
        std::string clean;
        for (char c : key)
            if (!std::isspace(static_cast<unsigned char>(c)))
                clean += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (!clean.empty()) {
            if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ctrl_check)))
                result += "control+";
            if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(alt_check)))
                result += "alt+";
            if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(shift_check)))
                result += "shift+";
            if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(win_check)))
                result += "win+";
            result += clean;
        }
    }
    gtk_widget_destroy(dialog);
    return result;
}

KeymapManagerDialog::KeymapManagerDialog(std::vector<KmAction> catalog, std::string active_keymap,
                                         std::function<void(const json&)> dispatch)
    : catalog_(std::move(catalog)), dispatch_(std::move(dispatch)),
      current_name_(std::move(active_keymap)) {
    for (const auto& a : catalog_)
        if (!a.default_key.empty())
            default_key_[a.id] = a.default_key;
    requested_name_ = current_name_;
}

std::pair<std::string, std::string>
KeymapManagerDialog::effective(const std::string& action) const {
    if (auto it = overrides_.find(action); it != overrides_.end())
        return {it->second, "custom"};
    if (unbinds_.count(action))
        return {"", "unbound"};
    if (auto it = default_key_.find(action); it != default_key_.end())
        return {it->second, "default"};
    return {"", "unbound"};
}

std::string KeymapManagerDialog::selected_action() const {
    GtkTreePath* path = nullptr;
    gtk_tree_view_get_cursor(GTK_TREE_VIEW(view_), &path, nullptr);
    if (!path)
        return {};
    const int row = gtk_tree_path_get_indices(path)[0];
    gtk_tree_path_free(path);
    if (row < 0 || row >= static_cast<int>(catalog_.size()))
        return {};
    return catalog_[static_cast<size_t>(row)].id;
}

void KeymapManagerDialog::populate_keymap_combo() {
    suppress_combo_ = true;
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(combo_));
    int sel = 0;
    for (size_t i = 0; i < keymaps_.size(); ++i) {
        std::string label = keymaps_[i];
        if (is_builtin(keymaps_[i]))
            label += " (built-in)";
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_), label.c_str());
        if (keymaps_[i] == current_name_)
            sel = static_cast<int>(i);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo_), sel);
    suppress_combo_ = false;
}

void KeymapManagerDialog::refresh_list() {
    GtkTreePath* prev_path = nullptr;
    gtk_tree_view_get_cursor(GTK_TREE_VIEW(view_), &prev_path, nullptr);
    int prev = -1;
    if (prev_path) {
        prev = gtk_tree_path_get_indices(prev_path)[0];
        gtk_tree_path_free(prev_path);
    }
    gtk_list_store_clear(store_);
    for (const auto& a : catalog_) {
        auto [key, source] = effective(a.id);
        GtkTreeIter iter;
        gtk_list_store_append(store_, &iter);
        gtk_list_store_set(store_, &iter, 0, a.label.c_str(), 1,
                           format_key_display(key).c_str(), 2, source.c_str(), -1);
    }
    if (!catalog_.empty()) {
        const int want = prev >= 0 && prev < static_cast<int>(catalog_.size()) ? prev : 0;
        GtkTreePath* path = gtk_tree_path_new_from_indices(want, -1);
        gtk_tree_view_set_cursor(GTK_TREE_VIEW(view_), path, nullptr, FALSE);
        gtk_tree_path_free(path);
    }
}

void KeymapManagerDialog::update_enabled() {
    // Set binding / Unbind / Duplicate stay available even on a read-only
    // keymap: editing one auto-creates an editable copy.
    const gboolean on = editable() ? TRUE : FALSE;
    gtk_dialog_set_response_sensitive(GTK_DIALOG(dialog_), 102, on); // Reset
    gtk_dialog_set_response_sensitive(GTK_DIALOG(dialog_), 106, on); // Save
    gtk_dialog_set_response_sensitive(GTK_DIALOG(dialog_), 104, on); // Delete
}

void KeymapManagerDialog::set_status() {
    std::string msg;
    if (is_builtin(current_name_))
        msg = current_name_ + " is built-in (read-only). Changing a binding creates an "
                              "editable copy automatically.";
    else
        msg = "Editing " + current_name_ + (dirty_ ? " — unsaved changes." : ".");
    gtk_label_set_text(GTK_LABEL(status_), msg.c_str());
}

void KeymapManagerDialog::on_keymap(const json& e) {
    const std::string name = e.value("name", std::string{});
    if (e.contains("builtins")) {
        builtins_.clear();
        for (const auto& n : e["builtins"])
            builtins_.insert(n.get<std::string>());
    }
    if (e.contains("keymaps")) {
        keymaps_.clear();
        for (const auto& n : e["keymaps"])
            keymaps_.push_back(n.get<std::string>());
        populate_keymap_combo();
    }
    if (name != requested_name_)
        return; // unrelated keymap event (e.g. driver reload)
    current_name_ = name;
    overrides_.clear();
    unbinds_.clear();
    // Bind to a named object first: iterating .items() on the temporary returned
    // by value() would dangle (the proxy outlives the temporary).
    const json ov = e.value("overrides", json::object());
    for (const auto& [action, key] : ov.items())
        overrides_[action] = key.get<std::string>();
    for (const auto& a : e.value("unbinds", json::array()))
        unbinds_.insert(a.get<std::string>());
    dirty_ = false;
    populate_keymap_combo();
    refresh_list();
    update_enabled();
    set_status();
}

void KeymapManagerDialog::switch_keymap(const std::string& name) {
    requested_name_ = name;
    current_name_ = name;
    overrides_.clear();
    unbinds_.clear();
    dirty_ = false;
    dispatch_({{"cmd", "get_keymap"}, {"name", name}});       // on_keymap fills in
    dispatch_({{"cmd", "set_active_keymap"}, {"name", name}}); // selecting activates
    refresh_list();
    update_enabled();
    set_status();
}

bool KeymapManagerDialog::confirm_discard() {
    if (!dirty_)
        return true;
    return yes_no(GTK_WINDOW(dialog_), "Unsaved changes",
                  "You have unsaved changes to '" + current_name_ + "'. Discard them?");
}

std::string KeymapManagerDialog::unique_fork_name() const {
    auto taken = [&](const std::string& n) {
        return n == "default" || std::find(keymaps_.begin(), keymaps_.end(), n) != keymaps_.end();
    };
    const std::string base = "My-Keymap";
    if (!taken(base))
        return base;
    for (int i = 2; i < 1000; ++i) {
        const std::string n = base + "-" + std::to_string(i);
        if (!taken(n))
            return n;
    }
    return base;
}

bool KeymapManagerDialog::commit_edit_forking_if_needed() {
    if (editable())
        return false;
    const std::string name = unique_fork_name();
    json ov = json::object();
    for (const auto& [action, key] : overrides_)
        ov[action] = key;
    json ub = json::array();
    for (const auto& a : unbinds_)
        ub.push_back(a);
    dispatch_({{"cmd", "save_keymap"}, {"name", name}, {"overrides", ov}, {"unbinds", ub}});
    switch_keymap(name);
    gtk_label_set_text(GTK_LABEL(status_),
                       ("Created editable copy '" + name + "' with your change, now active.")
                           .c_str());
    return true;
}

void KeymapManagerDialog::do_set_binding() {
    const std::string action = selected_action();
    if (action.empty())
        return;
    const std::string key =
        capture_key_binding(GTK_WINDOW(dialog_), effective(action).first);
    if (key.empty())
        return;
    // Collision: if another action already uses this key, offer to reassign.
    for (const auto& a : catalog_) {
        if (a.id == action)
            continue;
        if (effective(a.id).first == key) {
            if (!yes_no(GTK_WINDOW(dialog_), "Key in use",
                        "'" + format_key_display(key) + "' is already bound to '" + a.label +
                            "'. Reassign it to this action?"))
                return;
            if (overrides_.count(a.id))
                overrides_.erase(a.id); // was custom -> just drop it
            else
                unbinds_.insert(a.id); // shadowed a default -> unbind it
            break;
        }
    }
    overrides_[action] = key;
    unbinds_.erase(action);
    dirty_ = true;
    refresh_list();
    if (!commit_edit_forking_if_needed())
        set_status();
}

void KeymapManagerDialog::do_unbind() {
    const std::string action = selected_action();
    if (action.empty())
        return;
    overrides_.erase(action);
    if (default_key_.count(action))
        unbinds_.insert(action); // shadow the inherited default
    dirty_ = true;
    refresh_list();
    if (!commit_edit_forking_if_needed())
        set_status();
}

void KeymapManagerDialog::do_reset() {
    const std::string action = selected_action();
    if (action.empty() || !editable())
        return;
    overrides_.erase(action);
    unbinds_.erase(action);
    dirty_ = true;
    refresh_list();
    set_status();
}

std::string KeymapManagerDialog::prompt_name() {
    GtkWidget* d = gtk_dialog_new_with_buttons(
        "New keymap", GTK_WINDOW(dialog_),
        static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT), "_Cancel",
        GTK_RESPONSE_CANCEL, "_OK", GTK_RESPONSE_OK, nullptr);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_OK);
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    GtkWidget* label =
        gtk_label_new_with_mnemonic("_Name (letters, digits, dash, underscore):");
    GtkWidget* entry = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), entry);
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))), box, TRUE, TRUE, 0);
    gtk_widget_show_all(d);
    std::string name;
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK)
        name = gtk_entry_get_text(GTK_ENTRY(entry));
    gtk_widget_destroy(d);
    return name;
}

void KeymapManagerDialog::do_new() {
    const std::string name = prompt_name();
    if (name.empty())
        return;
    if (!valid_keymap_name(name)) {
        message_box(GTK_WINDOW(dialog_), "Invalid name",
                    "Use letters, digits, dashes, and underscores only.");
        return;
    }
    if (name == "default" || std::find(keymaps_.begin(), keymaps_.end(), name) != keymaps_.end()) {
        message_box(GTK_WINDOW(dialog_), "Name in use", "A keymap with that name already exists.");
        return;
    }
    // An empty keymap (inherits everything from default); then edit it.
    dispatch_({{"cmd", "save_keymap"},
               {"name", name},
               {"overrides", json::object()},
               {"unbinds", json::array()}});
    switch_keymap(name);
}

void KeymapManagerDialog::do_delete() {
    if (!editable())
        return;
    if (!yes_no(GTK_WINDOW(dialog_), "Delete keymap",
                "Delete keymap '" + current_name_ + "'? This cannot be undone."))
        return;
    dispatch_({{"cmd", "delete_keymap"}, {"name", current_name_}});
    dirty_ = false;
    switch_keymap("default");
}

void KeymapManagerDialog::do_save() {
    if (!editable())
        return;
    json ov = json::object();
    for (const auto& [action, key] : overrides_)
        ov[action] = key;
    json ub = json::array();
    for (const auto& a : unbinds_)
        ub.push_back(a);
    dispatch_({{"cmd", "save_keymap"}, {"name", current_name_}, {"overrides", ov},
               {"unbinds", ub}});
    dispatch_({{"cmd", "set_active_keymap"}, {"name", current_name_}}); // save activates it
    dirty_ = false;
    set_status();
    message_box(GTK_WINDOW(dialog_), "Keyboard Manager", "Saved and made active.");
}

void KeymapManagerDialog::do_duplicate() {
    const std::string name = prompt_name();
    if (name.empty())
        return;
    if (!valid_keymap_name(name)) {
        message_box(GTK_WINDOW(dialog_), "Invalid name",
                    "Use letters, digits, dashes, and underscores only.");
        return;
    }
    if (name == "default" || std::find(keymaps_.begin(), keymaps_.end(), name) != keymaps_.end()) {
        message_box(GTK_WINDOW(dialog_), "Name in use", "A keymap with that name already exists.");
        return;
    }
    json ov = json::object();
    for (const auto& [action, key] : overrides_)
        ov[action] = key;
    json ub = json::array();
    for (const auto& a : unbinds_)
        ub.push_back(a);
    dispatch_({{"cmd", "save_keymap"}, {"name", name}, {"overrides", ov}, {"unbinds", ub}});
    switch_keymap(name);
}

void KeymapManagerDialog::do_import() {
    GtkWidget* chooser = gtk_file_chooser_dialog_new(
        "Import Keymap", GTK_WINDOW(dialog_), GTK_FILE_CHOOSER_ACTION_OPEN, "_Cancel",
        GTK_RESPONSE_CANCEL, "_Import", GTK_RESPONSE_ACCEPT, nullptr);
    GtkFileFilter* filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Keymap files (*.keymap)");
    gtk_file_filter_add_pattern(filter, "*.keymap");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), filter);
    std::string text;
    bool have_file = false;
    if (gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT) {
        gchar* path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
        if (path) {
            std::ifstream in(path, std::ios::binary);
            if (in) {
                text.assign(std::istreambuf_iterator<char>(in),
                            std::istreambuf_iterator<char>());
                have_file = true;
            }
        }
        g_free(path);
    }
    gtk_widget_destroy(chooser);
    if (!have_file) {
        return;
    }
    const std::string name = prompt_name();
    if (name.empty())
        return;
    if (!valid_keymap_name(name)) {
        message_box(GTK_WINDOW(dialog_), "Invalid name",
                    "Use letters, digits, dashes, and underscores only.");
        return;
    }
    dispatch_({{"cmd", "import_keymap"}, {"name", name}, {"text", text}});
    switch_keymap(name);
}

void KeymapManagerDialog::run(GtkWindow* parent) {
    dialog_ = gtk_dialog_new_with_buttons(
        "Keyboard Manager", parent,
        static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
        "_Set binding…", 100, "_Unbind", 101, "_Reset to default", 102, "_New…", 103, "_Delete",
        104, "_Import…", 105, "D_uplicate…", 107, "Sa_ve", 106, "_Close", GTK_RESPONSE_CANCEL,
        nullptr);
    gtk_window_set_default_size(GTK_WINDOW(dialog_), 640, 520);

    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);

    GtkWidget* combo_label = gtk_label_new_with_mnemonic("Active _keymap:");
    gtk_label_set_xalign(GTK_LABEL(combo_label), 0.0f);
    combo_ = gtk_combo_box_text_new();
    gtk_label_set_mnemonic_widget(GTK_LABEL(combo_label), combo_);
    gtk_box_pack_start(GTK_BOX(box), combo_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), combo_, FALSE, FALSE, 0);

    status_ = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(status_), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(status_), TRUE);
    gtk_box_pack_start(GTK_BOX(box), status_, FALSE, FALSE, 0);

    store_ = gtk_list_store_new(3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    view_ = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store_));
    g_object_unref(store_);
    const char* headers[] = {"Action", "Key", "Source"};
    for (int i = 0; i < 3; ++i)
        gtk_tree_view_append_column(
            GTK_TREE_VIEW(view_),
            gtk_tree_view_column_new_with_attributes(headers[i], gtk_cell_renderer_text_new(),
                                                     "text", i, nullptr));
    atk_object_set_name(gtk_widget_get_accessible(view_), "Actions");
    GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(scroll), view_);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog_))), box, TRUE, TRUE,
                       0);

    // Enter on a row = Set binding.
    g_signal_connect_swapped(view_, "row-activated",
                             G_CALLBACK(+[](gpointer d, GtkTreePath*, GtkTreeViewColumn*) {
                                 gtk_dialog_response(GTK_DIALOG(d), 100);
                             }),
                             dialog_);
    struct ComboCtx {
        KeymapManagerDialog* self;
    };
    static ComboCtx combo_ctx;
    combo_ctx = {this};
    g_signal_connect(combo_, "changed", G_CALLBACK(+[](GtkComboBox* c, gpointer) {
                         KeymapManagerDialog* self = combo_ctx.self;
                         if (self->suppress_combo_)
                             return;
                         const int sel = gtk_combo_box_get_active(c);
                         if (sel < 0 || sel >= static_cast<int>(self->keymaps_.size()))
                             return;
                         const std::string& name = self->keymaps_[static_cast<size_t>(sel)];
                         if (name == self->current_name_)
                             return;
                         if (!self->confirm_discard()) {
                             self->populate_keymap_combo(); // snap back
                             return;
                         }
                         self->switch_keymap(name);
                     }),
                     nullptr);

    gtk_widget_show_all(dialog_);
    switch_keymap(current_name_); // request the active keymap's contents
    gtk_widget_grab_focus(view_);

    int response;
    while ((response = gtk_dialog_run(GTK_DIALOG(dialog_))) >= 100 && response <= 107) {
        switch (response) {
        case 100: do_set_binding(); break;
        case 101: do_unbind(); break;
        case 102: do_reset(); break;
        case 103: do_new(); break;
        case 104: do_delete(); break;
        case 105: do_import(); break;
        case 106: do_save(); break;
        case 107: do_duplicate(); break;
        }
        gtk_widget_grab_focus(view_);
    }
    if (dirty_ && !confirm_discard()) {
        // The user chose to keep editing... but the dialog is closing either
        // way; save instead so nothing is lost.
        if (editable())
            do_save();
    }
    gtk_widget_destroy(dialog_);
    dialog_ = nullptr;
}

} // namespace fastsmgtk
