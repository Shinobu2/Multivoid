// ui/bug_report_pane.h -- the F1 > Report a bug content pane (every peer, not dev-gated).
//
// The form (what happened, what was expected, an optional contact), the files the report will
// hold with their sizes on disk, "Save report", the status of the bundle and "Show in folder".
// Save report validates the form (coop::bug_report::ValidateForm) and then asks the bundle for a
// zip (coop::bug_report::Request); the zip lands in the multivoid_reports folder beside the
// game's executable. Nothing is sent. The file list is read when the pane starts being drawn,
// never every frame.
//
// Render thread.

#pragma once

namespace ui::bug_report_pane {

// Draw the pane content (called from dev_menu's content child).
void Render();

}  // namespace ui::bug_report_pane
