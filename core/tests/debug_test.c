/*
 * debug_test -- the debugger contract against a running machine: attach
 * stops it, registers, step / run-to / scanline / frame, disassembly with
 * the PC line and port names, breakpoints of every class (execute, memory
 * read and write, INP and OUT) with conditions, memory, the video views,
 * sound and I/O, the cartridge tab, built-in and loaded symbols in every
 * format, the trace ring, Save, the prompt and its completion, and detach
 * lets it run again. The breakpoints survive a power cycle. Also: the old
 * byte of a write (`old`, "was $xx"), Access breakpoints, the grey (disabled)
 * dot, hit counts and Clear Hits, search, the memory map, the symbol list,
 * the 65536-entry trace ring with its Branches filter, the stop reason's
 * address and the stop serial. The program is the tests' counter app
 * (test_rom.h), on the stand-in.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "studio2session.h"
#include "studio2debug.h"
#include "test_tmpdir.h"
#include "test_rom.h"

/* the loop, as counter.asm assembles it */
#define A_LDN   TEST_COUNTER_LOOP               /* LDN R8 */
#define A_ADI   (TEST_COUNTER_LOOP + 1)         /* ADI #01 */
#define A_STR   (TEST_COUNTER_LOOP + 3)         /* STR R8 */
#define A_SEX3  (TEST_COUNTER_LOOP + 4)         /* SEX R3 */
#define A_OUT   (TEST_COUNTER_LOOP + 5)         /* OUT 2; DB 01 */
#define A_SEX2  (TEST_COUNTER_LOOP + 7)         /* SEX R2 */
#define A_BR    (TEST_COUNTER_UP + 1)           /* BR loop */

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static int wait_stopped(studio2debug *d, int want, int timeout_ms)
{
    int waited = 0;
    while (waited < timeout_ms) {
        if (!!studio2debug_is_stopped(d) == want) return 1;
        sleep_ms(5); waited += 5;
    }
    return 0;
}

/* Resume and wait for the next stop. */
static int run_until_stop(studio2debug *d, int timeout_ms)
{
    unsigned g = studio2debug_generation(d);
    int waited = 0;
    studio2debug_resume(d);
    while (waited < timeout_ms) {
        if (studio2debug_is_stopped(d) && studio2debug_generation(d) != g) return 1;
        sleep_ms(2); waited += 2;
    }
    return 0;
}

static long file_size(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 ? (long)st.st_size : -1;
}

static void write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    fputs(text, f);
    fclose(f);
}

static int cpu_pc(studio2debug *d)
{
    studio2debug_cpu c;
    studio2debug_cpu_get(d, &c);
    return c.pc;
}

static int in_program(int pc)
{
    return pc >= TEST_COUNTER_LOOP && pc <= TEST_COUNTER_ISRW + 2;
}

int main(void)
{
    char cfg[512], data[512], rom[700], out[8192], path[800], msg[256];
    studio2session_paths p;
    studio2session *s;
    studio2session_start_opts o;
    studio2debug *d;
    studio2debug_cpu c, c2;
    studio2debug_line lines[48];
    int pc_line = -1, n, i, w, h;
    static uint32_t img[STUDIO2DEBUG_VIEW_MAX_PIXELS];

    test_tmpdir(cfg, sizeof cfg, "dcfg");
    test_tmpdir(data, sizeof data, "ddata");
    memset(&p, 0, sizeof p);
    p.config_dir = cfg; p.data_dir = data; p.fujinet_lib = "";
    snprintf(rom, sizeof rom, "%s/counter.bin", cfg);
    test_rom_write(rom, test_counter_app, sizeof test_counter_app);

    s = studio2session_new(&p);
    if (!s) return 1;
    studio2session_default_opts(s, &o);
    o.enable_fujinet = 0; o.enable_audio = 0; o.enable_gamepad = 0;
    o.cart_path = rom;
    check(studio2session_start(s, &o) == 0, "session starts with the counter app");
    sleep_ms(300);

    d = studio2session_debugger(s);
    check(d != NULL, "debugger handle");
    studio2debug_attach(d);
    check(studio2debug_is_attached(d), "attached");
    check(wait_stopped(d, 1, 3000), "attaching stops the machine");

    {
        char why[128]; int addr = 0;
        studio2debug_stop_reason(d, why, sizeof why, &addr);
        check(strncmp(why, "stopped at $", 12) == 0 && addr >= 0, "attach: the reason carries its address");
        check(studio2debug_stop_serial(d) >= 1, "and the stop serial moved");
    }
    studio2debug_cpu_get(d, &c);
    check(in_program(c.pc), "the PC is in the counter's loop or its interrupt");
    check(c.p == 3 || c.p == 1, "P is the program's (3) or the interrupt's (1)");
    check(c.r[2] >= 0x09f0 && c.r[2] <= 0x09ff, "the stack is the program's own, at RAM's top");
    check(c.r[8] == TEST_RAM_COUNTER, "R8 points at the counter");
    check(c.vpos >= 0 && c.vpos < 262 && c.hpos >= 0 && c.hpos < 112, "the beam position is reported");
    check(strcmp(c.mode_name, "run") == 0, "the CPU is in RUN");

    studio2debug_step(d);
    check(wait_stopped(d, 1, 2000), "stopped after a step");
    {
        int waited = 0;
        studio2debug_cpu_get(d, &c2);
        while (c2.pc == c.pc && waited < 2000) {
            sleep_ms(5); waited += 5;
            studio2debug_cpu_get(d, &c2);
        }
    }
    check(c2.pc != c.pc, "a step moves the PC");
    {
        char why[128];
        studio2debug_stop_reason(d, why, sizeof why, NULL);
        check(strncmp(why, "step at $", 9) == 0, "the step's reason is \"step at $xxxx\"");
    }
    check(c2.cycles > c.cycles, "and the cycle count");

    /* run to an address */
    studio2debug_run_to(d, A_LDN);
    check(wait_stopped(d, 1, 3000) && cpu_pc(d) == A_LDN, "run to the loop's top");

    /* disassembly around the PC */
    n = studio2debug_disassemble(d, (uint16_t)studio2debug_row_address(d, A_LDN, -4), lines, 48, &pc_line);
    check(n > 8, "disassembly lines");
    check(pc_line >= 0 && lines[pc_line].address == A_LDN && lines[pc_line].is_pc, "the PC line is marked");
    n = studio2debug_disassemble(d, A_LDN, lines, 5, NULL);
    check(n == 5 && strcmp(lines[0].disasm, "LDN R8") == 0 && strcmp(lines[1].disasm, "ADI #01") == 0,
          "the loop starts LDN R8 / ADI #01");
    check(strcmp(lines[2].disasm, "STR R8") == 0 && strcmp(lines[4].disasm, "OUT 2") == 0,
          "then STR R8 ... OUT 2");
    check(strcmp(lines[4].comment, "KEYLATCH") == 0, "and OUT 2 is named");
    n = studio2debug_disassemble(d, 0x0402, lines, 1, NULL);
    check(n == 1 && strcmp(lines[0].label, "s2_entry") == 0, "the clients' native entry is labelled");

    /* an execute breakpoint hits */
    check(studio2debug_breakpoint_toggle(d, A_BR) == 1, "breakpoint set on the BR");
    check(studio2debug_breakpoint_check(d, A_BR), "and reported");
    check(run_until_stop(d, 3000), "the breakpoint hit");
    check(cpu_pc(d) == A_BR, "stopped at the breakpoint's address");
    {
        char why[128]; int addr;
        studio2debug_stop_reason(d, why, sizeof why, &addr);
        check(strstr(why, "reakpoint") != NULL && addr == A_BR, "the stop reason says breakpoint");
    }
    n = studio2debug_disassemble(d, TEST_COUNTER_UP, lines, 2, NULL);
    check(n == 2 && lines[1].address == A_BR && lines[1].has_breakpoint, "the line shows the breakpoint");
    {
        int id = studio2debug_breakpoint_add(d, STUDIO2DEBUG_BP_EXEC, A_LDN, A_LDN, "");
        studio2debug_breakpoint hit_list[4];
        studio2debug_breakpoint_enable(d, id, 0);
        n = studio2debug_disassemble(d, A_LDN, lines, 1, NULL);
        check(n == 1 && !lines[0].has_breakpoint && lines[0].bp_disabled, "a disabled breakpoint is the grey dot");
        studio2debug_breakpoint_enable(d, id, 1);
        n = studio2debug_disassemble(d, A_LDN, lines, 1, NULL);
        check(n == 1 && lines[0].has_breakpoint && !lines[0].bp_disabled, "enabled again: the red dot");
        studio2debug_breakpoint_remove(d, id);
        check(studio2debug_breakpoint_list(d, hit_list, 4) == 1, "only the BR breakpoint is left");
        check(hit_list[0].hits >= 1, "it counted its hit");
        studio2debug_breakpoint_clear_hits(d);
        check(studio2debug_breakpoint_list(d, hit_list, 4) == 1 && hit_list[0].hits == 0, "Clear Hits zeroes them");
    }
    check(studio2debug_breakpoint_toggle(d, A_BR) == 0, "toggled off");

    /* conditions */
    check(studio2debug_breakpoint_add(d, STUDIO2DEBUG_BP_EXEC, A_STR, A_STR, "this bogus ((") == -1,
          "an unparsable condition is refused");
    {
        int id = studio2debug_breakpoint_add(d, STUDIO2DEBUG_BP_EXEC, A_STR, A_STR, "r8 == $1234");
        check(id > 0, "a conditional breakpoint is added");
        studio2debug_resume(d);
        sleep_ms(200);
        check(!studio2debug_is_stopped(d), "a false condition never stops");
        studio2debug_stop(d);
        check(wait_stopped(d, 1, 3000), "stop");
        studio2debug_breakpoint_remove(d, id);
        id = studio2debug_breakpoint_add(d, STUDIO2DEBUG_BP_EXEC, A_STR, A_STR,
                                         "r8 == $0810 && [$0810] == $40 && r8.1 == 8");
        check(id > 0 && run_until_stop(d, 5000), "a true condition stops");
        {
            uint8_t v = 0;
            studio2debug_read(d, TEST_RAM_COUNTER, &v, 1);
            check(cpu_pc(d) == A_STR && v == 0x40, "with the memory the condition named");
        }
        studio2debug_breakpoint_clear(d);
    }

    /* memory and I/O breakpoints */
    {
        studio2debug_breakpoint list[8];
        int id = studio2debug_breakpoint_add(d, STUDIO2DEBUG_BP_WRITE, TEST_RAM_COUNTER, TEST_RAM_COUNTER, "");
        check(id > 0, "write breakpoint added");
        check(studio2debug_breakpoint_list(d, list, 8) == 1 && list[0].type == STUDIO2DEBUG_BP_WRITE, "and listed");
        check(run_until_stop(d, 3000) && cpu_pc(d) == A_SEX3, "the write breakpoint hits after STR R8");
        studio2debug_breakpoint_enable(d, id, 0);
        check(studio2debug_breakpoint_list(d, list, 8) == 1 && !list[0].enabled && list[0].hits >= 1,
              "disabled, with its hit count");
        studio2debug_breakpoint_clear(d);

        studio2debug_breakpoint_add(d, STUDIO2DEBUG_BP_READ, TEST_RAM_COUNTER, TEST_RAM_COUNTER, "");
        check(run_until_stop(d, 3000) && cpu_pc(d) == A_ADI, "the read breakpoint hits after LDN R8");
        studio2debug_breakpoint_clear(d);

        studio2debug_breakpoint_add(d, STUDIO2DEBUG_BP_OUT, 2, 2, "value == 1");
        check(run_until_stop(d, 3000) && cpu_pc(d) == A_SEX2, "the OUT 2 breakpoint hits past its inline byte");
        studio2debug_breakpoint_clear(d);

        studio2debug_breakpoint_add(d, STUDIO2DEBUG_BP_IN, 1, 7, "");
        studio2debug_resume(d);
        sleep_ms(150);
        check(!studio2debug_is_stopped(d), "the program never INPs: the INP breakpoint stays quiet");
        studio2debug_stop(d);
        wait_stopped(d, 1, 3000);
        studio2debug_breakpoint_clear(d);
        check(studio2debug_breakpoint_list(d, list, 8) == 0, "cleared");
        check(studio2debug_breakpoint_add(d, STUDIO2DEBUG_BP_IN, 0, 0, "") > 0, "the API takes any range");
        studio2debug_breakpoint_clear(d);
    }

    /* the old byte of a write, Access breakpoints, the stop reason's text */
    {
        studio2debug_breakpoint list[8];
        char why[128];
        unsigned val = 0, was = 0;
        int id, addr = -1;
        uint8_t now = 0;

        id = studio2debug_breakpoint_add(d, STUDIO2DEBUG_BP_WRITE, TEST_RAM_COUNTER, TEST_RAM_COUNTER,
                                         "value == old + 1");
        check(id > 0 && run_until_stop(d, 3000) && cpu_pc(d) == A_SEX3, "a write breakpoint on `old` hits");
        studio2debug_stop_reason(d, why, sizeof why, &addr);
        check(sscanf(why, "wrote $0810 = $%x (was $%x)", &val, &was) == 2, "the reason reads \"wrote $0810 = $VV (was $OO)\"");
        check(((was + 1) & 0xff) == val, "and the old byte is the one before the store");
        studio2debug_read(d, TEST_RAM_COUNTER, &now, 1);
        check(now == val, "memory holds the new value");
        check(addr == A_SEX3, "the reason's address is the PC");
        studio2debug_breakpoint_clear(d);

        id = studio2debug_breakpoint_add(d, STUDIO2DEBUG_BP_WRITE, TEST_RAM_COUNTER, TEST_RAM_COUNTER,
                                         "value == old");
        studio2debug_resume(d);
        sleep_ms(150);
        check(!studio2debug_is_stopped(d), "value == old never holds for the counter");
        studio2debug_stop(d);
        check(wait_stopped(d, 1, 3000), "stop");
        studio2debug_breakpoint_clear(d);

        id = studio2debug_breakpoint_add(d, STUDIO2DEBUG_BP_EXEC, A_STR, A_STR, "old == value && addr == pc");
        check(id > 0 && run_until_stop(d, 3000) && cpu_pc(d) == A_STR, "an exec condition sees value, old and addr");
        studio2debug_breakpoint_clear(d);

        studio2debug_run_to(d, A_LDN);
        check(wait_stopped(d, 1, 3000), "back at the loop's top");
        id = studio2debug_breakpoint_add(d, STUDIO2DEBUG_BP_ACCESS, TEST_RAM_COUNTER, TEST_RAM_COUNTER, "");
        check(id > 0 && studio2debug_breakpoint_list(d, list, 8) == 1 &&
              list[0].type == STUDIO2DEBUG_BP_ACCESS, "an Access breakpoint is read | write");
        check(run_until_stop(d, 3000) && cpu_pc(d) == A_ADI, "it hits on the read");
        studio2debug_stop_reason(d, why, sizeof why, NULL);
        check(strncmp(why, "read $0810 = $", 14) == 0, "reading as \"read $0810 = $VV\"");
        check(run_until_stop(d, 3000) && cpu_pc(d) == A_SEX3, "and on the write");
        studio2debug_stop_reason(d, why, sizeof why, NULL);
        check(strncmp(why, "wrote $0810 = $", 15) == 0, "writing as \"wrote ...\"");
        studio2debug_command(d, "breaks", out, sizeof out);
        check(strstr(out, " a   $0810") != NULL, "the prompt lists it as an Access entry");
        studio2debug_breakpoint_clear(d);

        studio2debug_run_to(d, A_LDN);
        check(wait_stopped(d, 1, 3000), "run to");
        studio2debug_stop_reason(d, why, sizeof why, &addr);
        check(strncmp(why, "run to at $", 11) == 0 && addr == A_LDN, "a run-to reason gets \" at $xxxx\"");
    }

    /* the stop reason is for THIS stop: alternate step and run-to, reading
     * the reason the moment the machine reads as stopped */
    {
        int bad = 0;
        for (i = 0; i < 100; i++) {
            char why[128];
            uint64_t before = studio2debug_stop_serial(d);
            if (i & 1) studio2debug_step(d);
            else       studio2debug_run_to(d, i & 2 ? A_STR : A_LDN);
            if (!wait_stopped(d, 1, 3000)) { bad++; break; }
            studio2debug_stop_reason(d, why, sizeof why, NULL);
            if (studio2debug_stop_serial(d) <= before) bad++;
            if (strncmp(why, i & 1 ? "step at $" : "run to at $", i & 1 ? 9 : 11) != 0) bad++;
        }
        check(bad == 0, "a poller that sees the stop reads its own reason and serial (100 stops)");
    }

    /* memory: read, and debugger writes */
    {
        uint8_t ram[512], v = 0, claim[4];
        studio2debug_write(d, 0x0900, 0x5A);
        studio2debug_ram_get(d, ram);
        check(ram[0x100] == 0x5A, "a RAM write lands");
        studio2debug_read(d, 0x07FC, claim, 4);
        check(memcmp(claim, "FUJI", 4) == 0, "the claim reads through the cartridge");
        studio2debug_read(d, 0x0420, &v, 1);
        studio2debug_write(d, 0x0420, 0xEA);
        {
            uint8_t back = 0;
            studio2debug_read(d, 0x0420, &back, 1);
            check(back == 0xEA, "a debugger write reaches the cartridge's image");
        }
        studio2debug_write(d, 0x0420, v);
    }

    /* memory search: `to` bounds the START of a match */
    {
        static const uint8_t pat[3] = { 0xA5, 0x5A, 0xC3 };
        static const uint8_t ins[3] = { 0xa5, 0x5a, 0xc3 };
        uint16_t hits[8], next = 0;
        int cnt;

        studio2debug_write(d, 0x0900, ins[0]);
        studio2debug_write(d, 0x0901, ins[1]);
        studio2debug_write(d, 0x0902, ins[2]);
        cnt = studio2debug_search(d, pat, 3, 0xff, 0, 0x0900, 0x0900, hits, 8);
        check(cnt == 1 && hits[0] == 0x0900, "search finds the bytes at $0900");
        cnt = studio2debug_search(d, pat, 3, 0xff, 0, 0x0000, 0x08ff, hits, 8);
        check(cnt == 0, "and not below it");
        cnt = studio2debug_search(d, pat + 1, 2, 0xff, 0, 0x08ff, 0x0900, hits, 8);
        check(cnt == 0, "`to` bounds the start: a match starting at $0901 is out");
        cnt = studio2debug_search(d, pat, 3, 0xff, 0, 0x0900, 0x0901, hits, 8);
        check(cnt == 1, "a match may run past `to`");
        cnt = studio2debug_search(d, pat, 3, 0xf0, 0, 0x0900, 0x0900, hits, 8);
        check(cnt == 1, "a masked search matches on the mask");
        {
            static const uint8_t lower[3] = { 'f', 'U', 'j' };
            studio2debug_write(d, 0x0903, 'F');
            studio2debug_write(d, 0x0904, 'u');
            studio2debug_write(d, 0x0905, 'J');
            cnt = studio2debug_search(d, lower, 3, 0xff, STUDIO2DEBUG_SEARCH_NOCASE, 0x0903, 0x0903, hits, 8);
            check(cnt == 1 && hits[0] == 0x0903, "a text search ignores case");
            cnt = studio2debug_search(d, lower, 3, 0xff, 0, 0x0903, 0x0903, hits, 8);
            check(cnt == 0, "unless asked not to");
        }
        check(studio2debug_search_next(d, pat, 3, 0xff, 0, 0x0900, 0, &next) == 1 && next != 0x0900,
              "search next wraps to the mirror of the RAM");
        check(studio2debug_search_next(d, pat, 3, 0xff, 0, 0x0000, 0, &next) == 1 && next == 0x0900,
              "search next from $0000 lands on $0900");
        check(studio2debug_search_next(d, pat, 3, 0xff, 0, 0x0901, 1, &next) == 1 && next == 0x0900,
              "search previous");
        studio2debug_resume(d);
        sleep_ms(30);
        check(studio2debug_search(d, pat, 3, 0xff, 0, 0, 0xffff, hits, 8) == -1, "searching a running machine says so");
        check(studio2debug_search_next(d, pat, 3, 0xff, 0, 0, 0, &next) == -1, "and so does search next");
        studio2debug_stop(d);
        check(wait_stopped(d, 1, 3000), "stop");
    }

    /* the memory map: ordered, gap-free, named */
    {
        studio2debug_region map[64];
        int cnt = studio2debug_memory_map(d, map, 64), gap = 0, empty = 0, ram = 0, raster = 0, bios = 0;
        for (i = 0; i < cnt; i++) {
            if (i == 0 ? map[i].lo != 0 : map[i].lo != map[i - 1].hi + 1) gap++;
            if (map[i].hi < map[i].lo || !map[i].name[0]) empty++;
            if (0x0900 >= map[i].lo && 0x0900 <= map[i].hi && !strncmp(map[i].name, "Console RAM", 11)) ram++;
            if (0xF800 >= map[i].lo && 0xF800 <= map[i].hi && strstr(map[i].name, "raster")) raster++;
            if (0x0000 >= map[i].lo && 0x0000 <= map[i].hi && strstr(map[i].name, "ROM")) bios++;
        }
        check(cnt > 4 && cnt < 64, "the map has regions");
        check(cnt > 0 && map[0].lo == 0 && map[cnt - 1].hi == 0xffff && gap == 0, "gap-free from $0000 to $FFFF");
        check(empty == 0, "each region is named and ordered");
        check(ram == 1 && raster == 1 && bios == 1, "console RAM, the raster and the BIOS are where they belong");
        check(studio2debug_memory_map(d, map, 3) == 3, "a short buffer is not overrun");
    }

    /* registers */
    studio2debug_cpu_set(d, STUDIO2_REG_D, 0x42);
    studio2debug_cpu_set(d, STUDIO2_REG_DF, 1);
    studio2debug_cpu_set(d, STUDIO2_REG_R0 + 5, 0x1234);
    studio2debug_cpu_get(d, &c);
    check(c.d == 0x42 && c.df == 1, "D and DF are editable");
    check(c.r[5] == 0x1234, "the scratchpad registers too");

    /* video */
    {
        studio2debug_video v;
        studio2debug_video_get(d, &v);
        check(v.display_on, "the 1861 is on");
        check(v.dma_bytes > 0, "and has fetched by DMA");
        check(v.next_int_line == 78 || v.next_int_line == 80, "its next INT event falls on line 78 or 80");
        check(studio2debug_video_view(d, STUDIO2DEBUG_VIEW_SCREEN, img, &w, &h) && w == 112 && h == 262,
              "the whole-raster view");
        check(studio2debug_video_view(d, STUDIO2DEBUG_VIEW_RASTER, img, &w, &h) && w == 64 && h == 128,
              "the cartridge's raster view");
        check(studio2debug_video_view(d, STUDIO2DEBUG_VIEW_RAM, img, &w, &h) && w == 64 && h == 32,
              "the BIOS display page view");
    }

    /* sound and I/O */
    {
        studio2debug_io io;
        studio2debug_io_get(d, &io);
        check(!io.have_bios && !io.stub_parked, "the stand-in, not parked");
        check(io.keylatch == 1, "the program latched key 1");
        check(io.last_out[2] == 1, "OUT 2's last byte");
        check(io.beep_hz == 300 && !io.q, "the 300 Hz beeper, off");
        check(!io.clear_held && io.keys[0] == 0 && io.keys[1] == 0, "nothing held");
    }

    /* the cartridge */
    {
        studio2debug_cart cart;
        studio2debug_cart_get(d, &cart);
        check(cart.present && cart.direct && cart.booted_game, "cartridge tab: an opened image");
        check(cart.mailbox_mode && cart.live_mailbox, "an app: the mailbox is live");
        check(!cart.live_st2 && cart.live_size == 1024, "a raw 1K image");
        check(cart.page_type[0x04] == 1 && cart.page_type[0xF4] == 3 && cart.page_type[0x09] == 0,
              "the page map: the image, the raster, and console RAM left alone");
        check(studio2debug_cart_info(d, out, sizeof out) > 0 && strstr(out, "FujiNet"), "cart info line");
        check(studio2debug_fujinet_state(d, out, sizeof out) > 0 && strstr(out, "Link"), "the FujiNet state text");
    }

    /* symbols: every format, and the built-in tables */
    check(studio2debug_label_address(d, "FN_ACKSEQ") == 0xE800, "built-in mailbox labels (from fuji_mailbox.h)");
    check(studio2debug_label_address(d, "FN_RASTER") == 0xF400, "and the raster");
    check(studio2debug_label_address(d, "S2T_PUTC") == 0xED00, "and the text engine (from s2_text.h)");
    snprintf(path, sizeof path, "%s/t.map", cfg);
    write_text(path, "Symbols in Segment NOTHING\nR8                 Int    8   -1  1 0\n\n"
                     "Symbols in Segment CODE\nloop               Int    42D -1  1 0\n");
    check(studio2debug_load_symbols(d, path, msg, sizeof msg) == 1, "an AS -g MAP .map loads (code only)");
    check(studio2debug_label_address(d, "loop") == A_LDN, "and its labels resolve");
    snprintf(path, sizeof path, "%s/t.lst", cfg);
    write_text(path, "  Symbol Table (* = unused):\n  --------------------------\n\n"
                     " RA :                            A - | *isrw :                     451 C |\n"
                     " up :                          43B C |\n");
    check(studio2debug_load_symbols(d, path, msg, sizeof msg) == 2, "an AS listing's symbol table loads");
    check(studio2debug_label_address(d, "isrw") == TEST_COUNTER_ISRW, "unused labels too");
    snprintf(path, sizeof path, "%s/t.sym", cfg);
    write_text(path, "; WLA-DX\n[labels]\n00:0430 bump\n");
    check(studio2debug_load_symbols(d, path, msg, sizeof msg) == 1, "a WLA-DX .sym loads");
    check(studio2debug_label_address(d, "bump") == A_STR, "and its labels resolve");
    snprintf(path, sizeof path, "%s/t.txt", cfg);
    write_text(path, "0811 key1\n");
    check(studio2debug_load_symbols(d, path, msg, sizeof msg) == 1, "a plain list loads");
    studio2debug_address_label(d, TEST_RAM_KEY1, out, sizeof out);
    check(strcmp(out, "key1") == 0, "and the address finds its label");
    n = studio2debug_disassemble(d, A_STR, lines, 1, NULL);
    check(n == 1 && strcmp(lines[0].label, "bump") == 0, "loaded labels show in the disassembly");
    n = studio2debug_disassemble(d, A_BR, lines, 1, NULL);
    check(n == 1 && strcmp(lines[0].comment, "loop") == 0, "a branch names its target");

    /* the symbol list: built-ins and loaded, filtered, cleared */
    {
        int total = studio2debug_symbols_count(d), idx[64], hits_n, sorted = 1, found = 0;
        uint16_t symaddr = 0, prev = 0;
        char sname[64];
        int sbank = 0;

        check(total > 20, "the symbol list holds the built-ins and the loaded");
        check(studio2debug_symbols_get(d, 0, &symaddr, sname, sizeof sname, &sbank) == 1 && sname[0], "an entry reads back");
        check(studio2debug_symbols_get(d, total, &symaddr, sname, sizeof sname, &sbank) == 0, "past the end is 0");
        hits_n = studio2debug_symbols_filter(d, "fn_ack", idx, 64);
        check(hits_n >= 1, "a filter matches case-insensitively");
        for (i = 0; i < hits_n; i++) {
            studio2debug_symbols_get(d, idx[i], &symaddr, sname, sizeof sname, &sbank);
            if (i && symaddr < prev) sorted = 0;
            prev = symaddr;
            if (!strcmp(sname, "FN_ACKSEQ")) found = 1;
        }
        check(found && sorted, "FN_ACKSEQ is among them, ordered by address");
        check(studio2debug_symbols_filter(d, "", idx, 5) == 5, "an empty filter matches all, up to max");
        check(studio2debug_symbols_filter(d, "zzzz_nothing", idx, 64) == 0, "no match");
        studio2debug_clear_symbols(d);
        check(studio2debug_label_address(d, "loop") < 0 && studio2debug_label_address(d, "bump") < 0,
              "clearing drops the loaded labels");
        check(studio2debug_label_address(d, "FN_ACKSEQ") == 0xE800, "and keeps the built-ins");
        check(studio2debug_symbols_count(d) < total, "the list shrank");
        snprintf(path, sizeof path, "%s/t2.txt", cfg);
        write_text(path, "0430 bump\n042D loop\n");
        check(studio2debug_load_symbols(d, path, msg, sizeof msg) == 2, "and loading again works");
    }

    /* the trace ring */
    {
        studio2debug_trace tr[16];
        studio2debug_trace_enable(d, 1);
        for (i = 0; i < 10; i++) {
            studio2debug_step(d);
            wait_stopped(d, 1, 2000);
        }
        n = studio2debug_trace_read(d, tr, 16);
        check(n >= 10, "the trace holds the stepped instructions");
        check(n > 0 && in_program(tr[0].pc), "newest first, in the program");
        {
            char line[160];
            check(studio2debug_trace_count(d) >= 10, "the trace counts its entries");
            check(tr[0].cycles > 0, "an entry carries its cycle count");
            for (i = 0; i < n && tr[i].pc != A_LDN; i++) ;
            if (i < n) {
                studio2debug_trace_format(&tr[i], line, sizeof line);
                check(strncmp(line, "042D  LDN R8", 12) == 0 && strstr(line, "R(X)=") != NULL,
                      "an entry formats as the instruction and its registers");
            }
        }
        studio2debug_trace_enable(d, 0);
        studio2debug_trace_clear(d);
        check(studio2debug_trace_count(d) == 0, "clear empties it");
    }

    /* the ring keeps 65536; the Branches filter keeps only flow changes */
    {
        static studio2debug_trace big[70000];
        int held = 0, waited = 0, branching = 0, other = 0, bad = 0, loops = 0;

        studio2debug_trace_enable(d, 1);
        studio2debug_resume(d);
        while (waited < 20000 && (held = studio2debug_trace_count(d)) < 65536) {
            sleep_ms(20); waited += 20;
        }
        studio2debug_stop(d);
        check(wait_stopped(d, 1, 3000), "stopped with the big trace on");
        held = studio2debug_trace_read(d, big, 70000);
        check(held == 65536 && studio2debug_trace_count(d) == 65536, "the ring holds 65536 entries");
        for (i = 0; i < held; i++) {
            const uint8_t op = big[i].bytes[0];
            const int flow = (op & 0xf0) == 0x30 || ((op & 0xf0) == 0xc0 && op != 0xc4) ||
                             (op & 0xf0) == 0xd0 || op == 0x70 || op == 0x71;
            const int got = studio2debug_trace_match(&big[i], STUDIO2DEBUG_TRACE_BRANCHES, 0, 0);
            if (got) branching++; else other++;
            if (got != flow) bad++;
            if (op == 0x30) loops++;
        }
        check(bad == 0, "Branches keeps exactly $3x, $Cx (not NOP), $Dx, RET and DIS");
        check(branching > 0 && other > branching && loops > 0, "and the BR loop shows among them");
        {
            int in_range = 0, want = 0;
            for (i = 0; i < held; i++) {
                if (studio2debug_trace_match(&big[i], STUDIO2DEBUG_TRACE_RANGE, A_BR, A_LDN)) in_range++;
                if (big[i].pc >= A_LDN && big[i].pc <= A_BR) want++;
            }
            check(in_range == want && want > 0, "Range filters by PC (bounds in either order)");
        }
        studio2debug_trace_enable(d, 0);
        studio2debug_trace_clear(d);
    }

    /* the beam: a scanline, a frame */
    {
        studio2debug_cpu a, b;
        studio2debug_cpu_get(d, &a);
        studio2debug_scanline(d, 1);
        wait_stopped(d, 1, 3000);
        sleep_ms(20);
        studio2debug_cpu_get(d, &b);
        check(b.vpos == (a.vpos + 1) % 262, "scanline+1 advances the beam one line");
        studio2debug_frame(d, 1);
        wait_stopped(d, 1, 3000);
        sleep_ms(20);
        studio2debug_cpu_get(d, &a);
        check(a.frame == b.frame + 1, "frame+1 advances one frame");
    }

    /* Save */
    {
        static const struct { const char *kind; long size; } kinds[] = {
            { "ram", 512 }, { "arena", 2048 }, { "raster", 1024 },
        };
        for (i = 0; i < (int)(sizeof kinds / sizeof kinds[0]); i++) {
            char what[96];
            snprintf(path, sizeof path, "%s/save.%s", cfg, kinds[i].kind);
            snprintf(what, sizeof what, "save %s writes %ld bytes", kinds[i].kind, kinds[i].size);
            check(studio2debug_save(d, kinds[i].kind, path, msg, sizeof msg) == 0 &&
                  file_size(path) == kinds[i].size, what);
        }
        snprintf(path, sizeof path, "%s/save.ppm", cfg);
        check(studio2debug_save(d, "screen", path, msg, sizeof msg) == 0 &&
              file_size(path) > 100 * 246 * 3, "save screen (a PPM of the picture)");
        snprintf(path, sizeof path, "%s/save.regs", cfg);
        check(studio2debug_save(d, "regs", path, msg, sizeof msg) == 0 && file_size(path) > 100, "save regs");
        snprintf(path, sizeof path, "%s/save.dis", cfg);
        check(studio2debug_save(d, "dis", path, msg, sizeof msg) == 0 && file_size(path) > 1000, "save dis");
        check(studio2debug_save(d, "nonsense", path, msg, sizeof msg) == -1, "an unknown kind is refused");
    }

    /* the prompt */
    studio2debug_cpu_set(d, STUDIO2_REG_D, 0x66);
    studio2debug_command(d, "print d", out, sizeof out);
    check(strstr(out, "66") != NULL, "print evaluates an expression");
    studio2debug_command(d, "print {$07FC}", out, sizeof out);
    check(strstr(out, "4655") != NULL, "{addr} reads high byte first, as the 1802 keeps them");
    snprintf(path, sizeof path, "mem $%04X 4", A_LDN);
    studio2debug_command(d, path, out, sizeof out);
    check(strstr(out, "08 FC 01 58") != NULL, "mem dumps CPU memory");
    studio2debug_command(d, "label $0431 latch", out, sizeof out);
    check(studio2debug_label_address(d, "latch") == A_SEX3, "label sets one");
    snprintf(path, sizeof path, "disasm $%04X 2", A_LDN);
    studio2debug_command(d, path, out, sizeof out);
    check(strstr(out, "LDN R8") != NULL, "disasm");
    studio2debug_command(d, "regs", out, sizeof out);
    check(strstr(out, "R8 =0810") != NULL && strstr(out, "P=") != NULL, "regs");
    studio2debug_command(d, "video", out, sizeof out);
    check(strstr(out, "INT") != NULL && strstr(out, "R0") != NULL && strstr(out, "INP1") != NULL, "video: the Video tab's registers");
    studio2debug_command(d, "video timing", out, sizeof out);
    check(strstr(out, "not available") == NULL && strstr(out, "unknown") == NULL, "video timing");
    studio2debug_command(d, "video bogus", out, sizeof out);
    check(strstr(out, "pixel, registers") != NULL, "video: a bad section is explained");
    studio2debug_command(d, "io", out, sizeof out);
    check(strstr(out, "key latch 1") != NULL, "io shows the key latch");
    studio2debug_command(d, "keys", out, sizeof out);
    check(strstr(out, "keypad A") != NULL, "keys");
    studio2debug_command(d, "cart", out, sizeof out);
    check(strstr(out, "FujiNet") != NULL && strstr(out, "pages") != NULL, "cart, with its page map");
    studio2debug_command(d, "text", out, sizeof out);
    check(strstr(out, "cursor") != NULL, "text");
    studio2debug_command(d, "set r9 $0811", out, sizeof out);
    check(strstr(out, "R9 =0811") != NULL, "set a register by name");
    snprintf(path, sizeof path, "break $%04X", A_STR);
    studio2debug_command(d, path, out, sizeof out);
    check(studio2debug_breakpoint_check(d, A_STR), "break sets a breakpoint");
    studio2debug_command(d, "bpout 9", out, sizeof out);
    check(strstr(out, "1-7") != NULL, "an INP/OUT breakpoint outside ports 1-7 is refused");
    studio2debug_command(d, "clear", out, sizeof out);
    check(!studio2debug_breakpoint_check(d, A_STR), "clear removes them");
    studio2debug_command(d, "bpa $0810", out, sizeof out);
    studio2debug_command(d, "breaks", out, sizeof out);
    check(strstr(out, "#") != NULL && strstr(out, "$0810") != NULL, "bpa adds an Access breakpoint");
    studio2debug_command(d, "clear", out, sizeof out);
    studio2debug_command(d, "unwatch all", out, sizeof out);
    studio2debug_command(d, "watch $0900", out, sizeof out);
    check(strstr(out, "$0900") != NULL && strstr(out, "A5") != NULL, "watch adds one and shows its value");
    studio2debug_command(d, "watch", out, sizeof out);
    check(strstr(out, " 0  ") != NULL, "watch lists them");
    studio2debug_command(d, "unwatch 0", out, sizeof out);
    studio2debug_command(d, "watch", out, sizeof out);
    check(strstr(out, "no watches") != NULL, "unwatch removes one");
    studio2debug_command(d, "find A5 5A C3", out, sizeof out);
    check(strstr(out, "$0900") != NULL, "find searches for hex bytes");
    studio2debug_command(d, "find text fuj", out, sizeof out);
    check(strstr(out, "$07FC") != NULL || strstr(out, "$07fc") != NULL, "find text is case-insensitive (the claim at $07FC)");
    studio2debug_command(d, "find hex ZZ", out, sizeof out);
    check(strstr(out, "find") != NULL, "bad bytes print the usage");
    studio2debug_command(d, "trace on", out, sizeof out);
    studio2debug_command(d, "step", out, sizeof out);
    wait_stopped(d, 1, 2000);
    studio2debug_command(d, "trace 4", out, sizeof out);
    studio2debug_command(d, "trace off", out, sizeof out);
    studio2debug_command(d, "trace clear", out, sizeof out);
    studio2debug_command(d, "io", out, sizeof out);
    check(strstr(out, "Memory map") != NULL && strstr(out, "Console RAM") != NULL, "io prints the memory map");
    studio2debug_command(d, "help", out, sizeof out);
    check(strstr(out, "runto") != NULL && strstr(out, "bpa") != NULL && strstr(out, "find") != NULL,
          "help lists the commands");
    studio2debug_completions(d, "ru", out, sizeof out);
    check(strstr(out, "run") != NULL && strstr(out, "runto") != NULL, "Tab completion");
    studio2debug_completions(d, "print r1", out, sizeof out);
    check(strstr(out, "r10") != NULL, "registers complete");
    studio2debug_command(d, "frame", out, sizeof out);
    check(wait_stopped(d, 1, 3000), "frame stops again");

    /* step over / out do not wedge the machine */
    studio2debug_step_over(d);
    check(wait_stopped(d, 1, 3000), "step over");

    /* detach: running again */
    studio2debug_detach(d);
    check(!studio2debug_is_attached(d), "detached");
    {
        uint8_t a = 0, b = 0;
        sleep_ms(100);
        studio2debug_attach(d);
        wait_stopped(d, 1, 3000);
        studio2debug_read(d, TEST_RAM_COUNTER, &a, 1);
        studio2debug_resume(d);
        sleep_ms(200);
        studio2debug_read(d, TEST_RAM_COUNTER, &b, 1);
        check(a != b, "the machine runs after resume");
        studio2debug_detach(d);
    }

    /* the debugger survives a power cycle */
    studio2debug_attach(d);
    studio2debug_breakpoint_toggle(d, A_STR);
    check(studio2session_reset_to_config(s) == 0, "reset to CONFIG with the debugger attached");
    check(studio2session_load_cart(s, rom) == 0, "reopen the cartridge");
    /* the power cycle re-attaches: it stops on the new machine's first
     * instruction, and from there the breakpoint must still be set */
    check(wait_stopped(d, 1, 3000), "the new machine stops on attach");
    check(cpu_pc(d) == 0x0000, "at the stand-in's first instruction");
    check(run_until_stop(d, 3000) && cpu_pc(d) == A_STR, "the breakpoint survives the power cycle");
    studio2debug_breakpoint_clear(d);
    studio2debug_detach(d);

    /* attached before the session starts (a window opened first): the
     * machine stops as soon as it exists */
    studio2session_stop(s);
    studio2debug_attach(d);
    o.cart_path = rom;
    check(studio2session_start(s, &o) == 0, "restart with the debugger already attached");
    check(wait_stopped(d, 1, 3000), "the new machine stops in the debugger at once");
    studio2debug_detach(d);
    check(!studio2debug_is_stopped(d), "and runs when it detaches");

    studio2session_stop(s);
    studio2session_free(s);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
