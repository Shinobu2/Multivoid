// ui/voice_panel.h -- the voice-chat settings window. Opened with the V key
// (game-focus edge in the overlay WndProc). V opens voice settings and is
// independent of the tilde scoreboard. Rendered by imgui_overlay as its own
// interactive surface (it joins the input-capture set); the open state latches
// until V / the window's X closes it.
//
// The panel sets a row and nothing else: a device/mode change is applied by the row's subscriber
// (voice_chat::SubscribeRows), which reopens the devices on the next game tick, never here. The
// three sliders preview a drag live through voice_chat's atomic setters and commit their row on
// release. Render thread only.

#pragma once

namespace ui::voice_panel {

void Toggle();
void Close();
bool IsOpen();
void Render();

// Render thread, once per drawn frame: a slider that was being dragged when the panel closed
// commits its last previewed value to its row.
void CommitAbandonedDrag();

}  // namespace ui::voice_panel
