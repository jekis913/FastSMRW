#pragma once

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <gtk/gtk.h>
#include <nlohmann/json.hpp>

namespace fastsmgtk {

// One bindable action, as sent by the core's action_catalog event.
struct KmAction {
    std::string id;
    std::string label;
    std::string default_key;
};

// The Keyboard Manager: create/edit/delete keymaps for the invisible
// interface — the GTK mirror of windows/src/keymap_manager_dialog.cpp
// (inheritance from the read-only default, unbind support, collision
// detection, auto-fork when editing a built-in). Modal, but the core is
// async: commands go through `dispatch` and keymap events arrive via
// on_keymap() while the modal loop pumps.
class KeymapManagerDialog {
public:
    KeymapManagerDialog(std::vector<KmAction> catalog, std::string active_keymap,
                        std::function<void(const nlohmann::json&)> dispatch);

    void run(GtkWindow* parent);
    void on_keymap(const nlohmann::json& e);

private:
    void populate_keymap_combo();
    void refresh_list();
    void update_enabled();
    void set_status();
    std::pair<std::string, std::string> effective(const std::string& action) const;
    std::string selected_action() const;
    bool is_builtin(const std::string& name) const {
        return name == "default" || builtins_.count(name) > 0;
    }
    bool editable() const { return !current_name_.empty() && !is_builtin(current_name_); }
    void switch_keymap(const std::string& name);
    void do_set_binding();
    void do_unbind();
    void do_reset();
    void do_new();
    void do_delete();
    void do_save();
    void do_duplicate();
    void do_import();
    bool confirm_discard();
    bool commit_edit_forking_if_needed();
    std::string unique_fork_name() const;
    std::string prompt_name(); // the new-keymap name prompt ("" = cancelled)

    std::vector<KmAction> catalog_;
    std::map<std::string, std::string> default_key_;
    std::function<void(const nlohmann::json&)> dispatch_;

    GtkWidget* dialog_ = nullptr;
    GtkWidget* combo_ = nullptr;
    GtkWidget* status_ = nullptr;
    GtkListStore* store_ = nullptr;
    GtkWidget* view_ = nullptr;
    std::vector<std::string> keymaps_{"default"};
    std::set<std::string> builtins_;
    std::string current_name_;
    std::string requested_name_;
    std::map<std::string, std::string> overrides_;
    std::set<std::string> unbinds_;
    bool dirty_ = false;
    bool suppress_combo_ = false;
};

// Render a canonical key-string ("control+shift+win+r") for display
// ("Ctrl+Shift+Win+R"). Empty string -> "(unbound)".
std::string format_key_display(const std::string& key);

// The modal key-capture dialog (modifier checkboxes + a base key), prefilled
// from `current_key`. Returns the canonical key-string, or "" if cancelled.
std::string capture_key_binding(GtkWindow* parent, const std::string& current_key);

} // namespace fastsmgtk
