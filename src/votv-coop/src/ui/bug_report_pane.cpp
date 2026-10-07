// ui/bug_report_pane.cpp -- see ui/bug_report_pane.h.
//
// The pane keeps the form's text in fixed char buffers (ImGui edits them in place) and the file
// list the bundle would hold. The list is read by coop::bug_report::ListEntries on the first frame
// the pane is drawn after a frame it was not drawn, so a pane left open does not stat the files
// again. The save button is disabled while a bundle is being built; a press goes through
// ValidateForm first and shows its sentence in red when the form is refused.

#include "ui/bug_report_pane.h"

#include "coop/bug_report/report_bundle.h"
#include "coop/bug_report/report_core.h"
#include "coop/text/utf8_codec.h"
#include "ue_wrap/core/log.h"
#include "ui/scale.h"

#include "imgui.h"

#include <windows.h>
#include <shellapi.h>

#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace ui::bug_report_pane {
namespace {

using ui::scale::S;
namespace BR = coop::bug_report;

constexpr int kFieldLines = 6;

// Render thread only.
char g_happened[BR::kFieldMaxBytes + 1] = {};
char g_expected[BR::kFieldMaxBytes + 1] = {};
char g_contact[BR::kContactMaxBytes + 1] = {};
std::vector<BR::Entry> g_entries;   // the bundle's file list, as of the pane's last opening
int g_lastFrame = -1;               // the ImGui frame the pane was last drawn on
const char* g_formError = nullptr;  // ValidateForm's sentence for the last press, else nullptr

unsigned long long Kb(uint64_t bytes) { return (bytes + 1023) / 1024; }

void DrawField(const char* label, const char* id, char* buf, size_t size) {
    ImGui::TextUnformatted(label);
    const float h = ImGui::GetTextLineHeight() * kFieldLines + ImGui::GetStyle().FramePadding.y * 2.0f;
    ImGui::InputTextMultiline(id, buf, size, ImVec2(-FLT_MIN, h));
}

void DrawFiles() {
    ImGui::TextUnformatted("Files that will be included");
    ImGui::Indent(S(12.0f));
    for (const BR::Entry& e : g_entries) {
        if (!e.leftOut.empty()) {
            ImGui::TextDisabled("%s -- left out: %s", e.name, e.leftOut.c_str());
        } else if (e.tailOnly && e.bytesOnDisk > BR::kUe4ssTailBytes) {
            ImGui::Text("%s (last %llu KB of %llu KB)", e.name, Kb(BR::kUe4ssTailBytes), Kb(e.bytesOnDisk));
        } else {
            ImGui::Text("%s (%llu KB)", e.name, Kb(e.bytesOnDisk));
        }
    }
    for (const char* name : BR::kMadeEntries) {
        if (std::strcmp(name, "multivoid.ini") == 0) ImGui::Text("%s (without passwords)", name);
        else ImGui::TextUnformatted(name);
    }
    ImGui::Unindent(S(12.0f));
}

void ShowInFolder(const std::wstring& zipPath) {
    const std::wstring args = L"/select,\"" + zipPath + L"\"";
    const INT_PTR r = reinterpret_cast<INT_PTR>(
        ::ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL));
    if (r <= 32) UE_LOGW("bug_report_pane: Show in folder failed (ShellExecuteW %lld)", static_cast<long long>(r));
}

void OnSavePressed() {
    BR::Form form{g_happened, g_expected, g_contact};
    g_formError = BR::ValidateForm(form);
    if (g_formError) return;
    BR::Request(std::move(form));
}

void DrawStatus(const BR::Status& st) {
    switch (st.phase) {
        case BR::Phase::Idle:
            break;
        case BR::Phase::Building:
            ImGui::TextUnformatted("Saving the report...");
            break;
        case BR::Phase::Done: {
            const std::string name = coop::text::ToUtf8(std::filesystem::path(st.zipPath).filename().wstring());
            ImGui::TextWrapped("Saved: %s (%llu KB)", name.c_str(), Kb(st.zipBytes));
            if (ImGui::Button("Show in folder")) ShowInFolder(st.zipPath);
            break;
        }
        case BR::Phase::Failed:
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 0.45f, 0.42f, 1.0f));
            ImGui::TextWrapped("%s", st.error.c_str());
            ImGui::PopStyleColor();
            break;
    }
}

}  // namespace

void Render() {
    const int frame = ImGui::GetFrameCount();
    if (g_lastFrame != frame - 1) g_entries = BR::ListEntries();
    g_lastFrame = frame;

    ImGui::TextWrapped("Saves a report file on this PC. Nothing is sent.");
    ImGui::Spacing();
    DrawField("What happened", "##happened", g_happened, sizeof(g_happened));
    DrawField("What you expected", "##expected", g_expected, sizeof(g_expected));
    ImGui::TextUnformatted("Contact (optional, e.g. a Discord name)");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText("##contact", g_contact, sizeof(g_contact));
    ImGui::Spacing();
    DrawFiles();
    ImGui::Spacing();

    const BR::Status st = BR::GetStatus();
    ImGui::BeginDisabled(st.phase == BR::Phase::Building);
    if (ImGui::Button("Save report")) OnSavePressed();
    ImGui::EndDisabled();
    if (g_formError) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 0.45f, 0.42f, 1.0f));
        ImGui::TextWrapped("%s", g_formError);
        ImGui::PopStyleColor();
    }
    DrawStatus(st);
}

}  // namespace ui::bug_report_pane
