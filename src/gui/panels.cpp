#include "panels.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"

void p2500_panels_log(P2500Panels &p, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p.log[p.log_head], P2500Panels::LOG_LEN, fmt, ap);
    va_end(ap);
    p.log_head = (p.log_head + 1) % P2500Panels::LOG_CAP;
    if (p.log_count < P2500Panels::LOG_CAP) p.log_count++;
}

void p2500_panels_menu(P2500Panels &p)
{
    ImGui::MenuItem("Device state", "F1", &p.show_devices);
    ImGui::MenuItem("Memory", "F2", &p.show_memory);
    ImGui::MenuItem("Disassembly", "F3", &p.show_disasm);
    ImGui::MenuItem("Event log", "F4", &p.show_log);
}

/* A hex address entry that commits on Enter. Returns true, with *out set,
 * on the frame it commits. */
static bool addr_input(const char *label, char *buf, size_t buflen, uint16_t *out)
{
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.0f);
    bool done = ImGui::InputText(label, buf, buflen,
                                ImGuiInputTextFlags_CharsHexadecimal |
                                ImGuiInputTextFlags_EnterReturnsTrue);
    if (!done || buf[0] == '\0') return false;
    *out = (uint16_t)strtoul(buf, NULL, 16);
    buf[0] = '\0';
    return true;
}

/* ------------------------------------------------------------------------ *
 *  Device state
 *
 *  Every topic in core/debug.h, one collapsible section each, in daisy-chain
 *  and bus order rather than alphabetically: the order the questions get
 *  asked in. "Copy all" exists because the answer usually belongs in
 *  TODO.md, and retyping a register dump is how transcription errors get
 *  into notes.
 * ------------------------------------------------------------------------ */
static void draw_devices(P2500Panels &p, const P2500Machine &m)
{
    ImGui::SetNextWindowPos(ImVec2(24, 60), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(620, 560), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Device state", &p.show_devices)) { ImGui::End(); return; }

    if (ImGui::SmallButton("Copy all")) {
        /* Deliberately the same layout p2500-emu --state prints, so a dump
         * pasted from either is diffable against the other. */
        static char dump[16384];
        size_t at = 0;
        for (int t = 0; t < P2500_DBG_TOPICS && at + 1 < sizeof dump; t++) {
            at += (size_t)snprintf(dump + at, sizeof dump - at, "\n[%s]\n",
                                   p2500_debug_topic_name((P2500DebugTopic)t));
            int n = p2500_debug_topic_lines(&m, (P2500DebugTopic)t);
            for (int l = 0; l < n && at + 1 < sizeof dump; l++) {
                char label[48], value[192];
                p2500_debug_line(&m, (P2500DebugTopic)t, l,
                                 label, sizeof label, value, sizeof value);
                at += (size_t)snprintf(dump + at, sizeof dump - at,
                                       "  %-22s %s\n", label, value);
            }
        }
        ImGui::SetClipboardText(dump);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("same lines as p2500-emu --state");
    ImGui::Separator();

    for (int t = 0; t < P2500_DBG_TOPICS; t++) {
        const P2500DebugTopic topic = (P2500DebugTopic)t;
        /* Open by default: a panel that has to be unfolded before it says
         * anything is a panel nobody opens during a chase. */
        if (!ImGui::CollapsingHeader(p2500_debug_topic_name(topic),
                                     ImGuiTreeNodeFlags_DefaultOpen))
            continue;
        char table_id[32];
        snprintf(table_id, sizeof table_id, "tbl%d", t);
        if (!ImGui::BeginTable(table_id, 2,
                               ImGuiTableFlags_SizingFixedFit |
                               ImGuiTableFlags_RowBg))
            continue;
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed,
                                ImGui::GetFontSize() * 11.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
        int n = p2500_debug_topic_lines(&m, topic);
        for (int l = 0; l < n; l++) {
            char label[48], value[192];
            p2500_debug_line(&m, topic, l, label, sizeof label, value, sizeof value);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", label);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(value);
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

/* ------------------------------------------------------------------------ *
 *  Memory
 * ------------------------------------------------------------------------ */
static uint8_t mem_read(const P2500Machine &m, int plane, uint16_t addr)
{
    if (plane == 1) return m.vram[addr & (P2500_VRAM_SIZE - 1)];
    if (plane == 2) return m.vram_attr[addr & (P2500_VRAM_SIZE - 1)];
    return p2500_peek(&m, addr);
}

static void draw_memory(P2500Panels &p, const P2500Machine &m, P2500Debug &dbg)
{
    ImGui::SetNextWindowPos(ImVec2(360, 120), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(640, 480), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Memory", &p.show_memory)) { ImGui::End(); return; }

    const char *planes[] = { "CPU view (bank-aware)", "Video characters", "Video attributes" };
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    ImGui::Combo("plane", &p.mem_plane, planes, 3);
    ImGui::SameLine();
    uint16_t want = 0;
    if (addr_input("go to", p.mem_entry, sizeof p.mem_entry, &want)) p.mem_goto = want;
    ImGui::SameLine();
    ImGui::Checkbox("follow PC", &p.mem_follow_pc);
    if (p.mem_follow_pc) p.mem_goto = m.cpu.pc;

    const int rows = (p.mem_plane == 0 ? 0x10000 : P2500_VRAM_SIZE) / 16;
    ImGui::Separator();

    if (ImGui::BeginChild("hex", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 4.2f))) {
        const float line_h = ImGui::GetTextLineHeightWithSpacing();
        if (p.mem_goto >= 0) {
            ImGui::SetScrollY((float)(p.mem_goto / 16) * line_h - line_h * 4.0f);
            p.mem_goto = -1;
        }
        /* Clipped: 4096 rows of formatted hex per frame would cost more than
         * the emulation does. */
        ImGuiListClipper clip;
        clip.Begin(rows, line_h);
        while (clip.Step()) {
            for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
                const uint16_t base = (uint16_t)(r * 16);
                char hex[64], ascii[20];
                size_t at = 0;
                for (int c = 0; c < 16; c++) {
                    uint8_t b = mem_read(m, p.mem_plane, (uint16_t)(base + c));
                    at += (size_t)snprintf(hex + at, sizeof hex - at, "%02X%s",
                                           b, c == 7 ? "  " : " ");
                    ascii[c] = (b >= 0x20 && b < 0x7F) ? (char)b : '.';
                }
                ascii[16] = '\0';
                const bool at_pc = p.mem_plane == 0 && m.cpu.pc >= base && m.cpu.pc < base + 16;
                if (at_pc) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_Text));
                else ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::Text("%04X", base);
                ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::Text("%s |%s|", hex, ascii);
            }
        }
        clip.End();
    }
    ImGui::EndChild();

    ImGui::Separator();
    ImGui::TextDisabled("Watches - reported to the event log when they change");
    uint16_t addr = 0;
    if (addr_input("add watch", p.watch_entry, sizeof p.watch_entry, &addr)) {
        p2500_debug_add_watch(&dbg, addr, 1);
        p2500_debug_baseline(&dbg, &m);
    }
    for (int i = 0; i < dbg.watches; i++) {
        ImGui::PushID(i);
        ImGui::Checkbox("", &dbg.watch[i].enabled);
        ImGui::SameLine();
        ImGui::Text("$%04X:%u  = %02X", dbg.watch[i].addr, dbg.watch[i].len,
                    dbg.watch[i].last[0]);
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) { p2500_debug_remove_watch(&dbg, i); ImGui::PopID(); break; }
        ImGui::PopID();
    }
    ImGui::End();
}

/* ------------------------------------------------------------------------ *
 *  Disassembly
 *
 *  Listing forwards from an anchor only. A Z80 instruction stream cannot be
 *  decoded backwards - the byte before PC may be an operand - so guessing at
 *  preceding instructions would show confident nonsense. The anchor is
 *  adjustable instead.
 * ------------------------------------------------------------------------ */
static void draw_disasm(P2500Panels &p, const P2500Machine &m, P2500Debug &dbg,
                        bool paused, bool *toggle_pause, P2500PanelActions &act)
{
    ImGui::SetNextWindowPos(ImVec2(680, 60), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(560, 520), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Disassembly", &p.show_disasm)) { ImGui::End(); return; }

    if (ImGui::Button(paused ? "Run" : "Pause")) *toggle_pause = true;
    ImGui::SameLine();
    if (ImGui::Button("Step")) act.steps = 1;
    ImGui::SameLine();
    if (ImGui::Button("Step 100")) act.steps = 100;
    ImGui::SameLine();
    if (ImGui::Button("Step field")) act.steps = ~0UL; /* one 50 Hz field */
    ImGui::SameLine();
    ImGui::TextDisabled("PC $%04X", m.cpu.pc);

    ImGui::Checkbox("follow PC", &p.disasm_follow_pc);
    ImGui::SameLine();
    uint16_t want = 0;
    if (addr_input("from", p.disasm_entry, sizeof p.disasm_entry, &want)) {
        p.disasm_addr = want;
        p.disasm_follow_pc = false;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("-16")) { p.disasm_addr = (uint16_t)(p.disasm_addr - 16); p.disasm_follow_pc = false; }
    ImGui::SameLine();
    if (ImGui::SmallButton("+16")) { p.disasm_addr = (uint16_t)(p.disasm_addr + 16); p.disasm_follow_pc = false; }

    if (p.disasm_follow_pc) p.disasm_addr = m.cpu.pc;
    ImGui::Separator();

    if (ImGui::BeginChild("listing", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 3.4f))) {
        uint16_t at = p.disasm_addr;
        const int lines = 48;
        for (int i = 0; i < lines; i++) {
            char text[80];
            int len = p2500_disasm(&m, at, text, sizeof text);
            char bytes[16] = "";
            size_t bo = 0;
            for (int b = 0; b < len && b < 4; b++)
                bo += (size_t)snprintf(bytes + bo, sizeof bytes - bo, "%02X ",
                                       p2500_peek(&m, (uint16_t)(at + b)));

            /* The gutter is the breakpoint control - clicking the marker is
             * what every debugger does - drawn rather than buttoned, because
             * a column of 48 framed buttons reads as a wall, not a margin. */
            const bool has_break = p2500_debug_find_break(&dbg, at) >= 0;
            const float dot = ImGui::GetTextLineHeight();
            ImGui::PushID(i);
            const ImVec2 gutter = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("bp", ImVec2(dot, dot)))
                p2500_debug_toggle_break(&dbg, at);
            const bool hot = ImGui::IsItemHovered();
            ImDrawList *dl = ImGui::GetWindowDrawList();
            const ImVec2 centre(gutter.x + dot * 0.5f, gutter.y + dot * 0.5f);
            if (has_break)
                dl->AddCircleFilled(centre, dot * 0.28f, IM_COL32(255, 70, 50, 255));
            else if (hot)
                dl->AddCircle(centre, dot * 0.28f, ImGui::GetColorU32(ImGuiCol_TextDisabled), 0, 1.2f);
            ImGui::PopID();
            ImGui::SameLine();
            if (at == m.cpu.pc) ImGui::Text(">$%04X  %-12s %s", at, bytes, text);
            else ImGui::TextDisabled(" $%04X  %-12s %s", at, bytes, text);
            at = (uint16_t)(at + len);
        }
    }
    ImGui::EndChild();

    ImGui::Separator();
    uint16_t addr = 0;
    if (addr_input("add breakpoint", p.break_entry, sizeof p.break_entry, &addr))
        p2500_debug_add_break(&dbg, addr);
    for (int i = 0; i < dbg.breaks; i++) {
        ImGui::PushID(1000 + i);
        ImGui::Checkbox("", &dbg.brk[i].enabled);
        ImGui::SameLine();
        ImGui::Text("$%04X", dbg.brk[i].addr);
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) { p2500_debug_remove_break(&dbg, i); ImGui::PopID(); break; }
        ImGui::PopID();
        if (i % 4 != 3 && i + 1 < dbg.breaks) ImGui::SameLine();
    }
    ImGui::End();
}

/* ------------------------------------------------------------------------ *
 *  Event log
 * ------------------------------------------------------------------------ */
static void draw_log(P2500Panels &p)
{
    ImGui::SetNextWindowPos(ImVec2(120, 300), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(720, 300), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Event log", &p.show_log)) { ImGui::End(); return; }
    ImGui::Checkbox("auto-scroll", &p.log_autoscroll);
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) { p.log_count = 0; p.log_head = 0; }
    ImGui::SameLine();
    ImGui::TextDisabled("%d of %d", p.log_count, P2500Panels::LOG_CAP);
    ImGui::Separator();
    if (ImGui::BeginChild("lines")) {
        const int first = (p.log_head - p.log_count + P2500Panels::LOG_CAP) % P2500Panels::LOG_CAP;
        for (int i = 0; i < p.log_count; i++)
            ImGui::TextUnformatted(p.log[(first + i) % P2500Panels::LOG_CAP]);
        if (p.log_autoscroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f)
            ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::End();
}

P2500PanelActions p2500_panels_draw(P2500Panels &p, P2500Machine &m,
                                    P2500Debug &dbg, bool paused,
                                    bool *toggle_pause)
{
    P2500PanelActions act;
    if (p.show_devices) draw_devices(p, m);
    if (p.show_memory) draw_memory(p, m, dbg);
    if (p.show_disasm) draw_disasm(p, m, dbg, paused, toggle_pause, act);
    if (p.show_log) draw_log(p);
    return act;
}
