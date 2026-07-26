#pragma once

#include <optional>
#include <string>

#include <gtk/gtk.h>

namespace fastsmgtk {

// What the report dialog collected. category is the Mastodon API token
// ("spam" | "violation" | "legal" | "other").
struct ReportInput {
    std::string category;
    std::string comment;
    bool forward = false;
};

// Modal "Report" dialog; `remote` pre-checks "forward to their server". The
// GTK mirror of windows/src/report_dialog.cpp.
std::optional<ReportInput> show_report_dialog(GtkWindow* parent, bool remote);

} // namespace fastsmgtk
