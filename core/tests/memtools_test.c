/* memtools: search parsing, watch list, trace filters, breakpoint text (no ROMs needed). */
#include <stdio.h>
#include <string.h>

#include "studio2memtools.h"

static int fails;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

int main(void)
{
    uint8_t pat[STUDIO2SEARCH_MAX], mask;
    int count, flags;
    studio2debug_trace brz = { .pc = 0x0433, .len = 2, .p = 3, .x = 2, .bytes = { 0x32, 0x3B } };
    studio2debug_trace nop = { .pc = 0x0500, .len = 1, .p = 3, .x = 2, .bytes = { 0xC4 } };
    char buf[128];

    CHECK(studio2search_parse(STUDIO2SEARCH_HEX, "5A 3c,$FF 0x10", pat, &count, &mask, &flags), "hex parses");
    CHECK(count == 4 && pat[0] == 0x5A && pat[1] == 0x3C && pat[2] == 0xFF && pat[3] == 0x10, "hex bytes");
    CHECK(mask == 0xFF && flags == 0, "hex mask/flags");
    CHECK(studio2search_parse(STUDIO2SEARCH_HEX, "5", pat, &count, &mask, &flags) && count == 1 && pat[0] == 5,
          "lone digit");
    CHECK(!studio2search_parse(STUDIO2SEARCH_HEX, "zz", pat, &count, &mask, &flags), "bad hex");
    CHECK(!studio2search_parse(STUDIO2SEARCH_HEX, "  ", pat, &count, &mask, &flags), "empty hex");
    CHECK(studio2search_parse(STUDIO2SEARCH_TEXT, "Ab", pat, &count, &mask, &flags) && count == 2 &&
          pat[0] == 'A' && (flags & STUDIO2DEBUG_SEARCH_NOCASE), "text");

    studio2watch_clear();
    CHECK(studio2watch_add("  $0810 ") == 0 && studio2watch_add("score") == 1, "add");
    CHECK(studio2watch_add("   ") == -1, "empty add refused");
    CHECK(studio2watch_count() == 2 && !strcmp(studio2watch_text(0), "$0810"), "text trimmed");
    {
        unsigned gen = studio2watch_generation();
        studio2watch_remove(0);
        CHECK(studio2watch_count() == 1 && !strcmp(studio2watch_text(0), "score"), "remove");
        CHECK(studio2watch_generation() != gen, "generation bumps");
    }
    studio2watch_clear();
    CHECK(studio2watch_count() == 0, "clear");

    CHECK(studio2debug_trace_match(&brz, STUDIO2DEBUG_TRACE_ALL, 0, 0), "all");
    CHECK(studio2debug_trace_match(&brz, STUDIO2DEBUG_TRACE_BRANCHES, 0, 0), "BZ is a branch");
    CHECK(!studio2debug_trace_match(&nop, STUDIO2DEBUG_TRACE_BRANCHES, 0, 0), "NOP is not");
    CHECK(studio2debug_trace_match(&brz, STUDIO2DEBUG_TRACE_RANGE, 0x0400, 0x04FF), "in range");
    CHECK(!studio2debug_trace_match(&nop, STUDIO2DEBUG_TRACE_RANGE, 0x0400, 0x04FF), "out of range");
    CHECK(studio2debug_trace_match(&brz, STUDIO2DEBUG_TRACE_RANGE, 0x04FF, 0x0400), "range swaps");
    studio2debug_trace_format(&brz, buf, sizeof buf);
    CHECK(!strncmp(buf, "0433  BZ 043B", 13), "trace format");

    {
        /* every 1802 flow change, and plain instructions */
        static const struct { uint8_t byt[3]; int branch; const char *what; } cases[] = {
            { { 0x30, 0x10 }, 1, "BR" }, { { 0x3A, 0x10 }, 1, "BNZ" }, { { 0x3F, 0x10 }, 1, "BN4" },
            { { 0xC0, 0x04, 0x00 }, 1, "LBR" }, { { 0xCA, 0x04, 0x00 }, 1, "LBNZ" },
            { { 0xC8 }, 1, "LSKP" }, { { 0xCE }, 1, "LSZ" },
            { { 0xD4 }, 1, "SEP R4" }, { { 0xD3 }, 1, "SEP R3" },
            { { 0x70 }, 1, "RET" }, { { 0x71 }, 1, "DIS" },
            { { 0xC4 }, 0, "NOP" }, { { 0x00 }, 0, "IDL" }, { { 0xF8, 0x05 }, 0, "LDI" },
            { { 0x08 }, 0, "LDN R8" }, { { 0x62 }, 0, "OUT 2" }, { { 0xA1 }, 0, "PLO R1" },
            { { 0x22 }, 0, "DEC R2" }, { { 0xE3 }, 0, "SEX R3" }, { { 0x78 }, 0, "SAV" },
        };
        for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            studio2debug_trace ent = { .pc = 0x0100, .len = 1, .p = 3, .x = 2 };
            memcpy(ent.bytes, cases[i].byt, 3);
            CHECK(!!studio2debug_trace_match(&ent, STUDIO2DEBUG_TRACE_BRANCHES, 0, 0) == cases[i].branch,
                  cases[i].what);
        }
    }
    {
        studio2debug_breakpoint bp;
        memset(&bp, 0, sizeof bp);
        bp.type = STUDIO2DEBUG_BP_WRITE; bp.start = 0x0900; bp.end = 0x09FF;
        snprintf(bp.condition, sizeof bp.condition, "value != old");
        studio2bp_describe(NULL, &bp, buf, sizeof buf);
        CHECK(!strcmp(buf, "Write  $0900-$09FF  if value != old"), "write bp description");
        bp.type = STUDIO2DEBUG_BP_ACCESS; bp.start = bp.end = 0x0810; bp.condition[0] = 0;
        studio2bp_describe(NULL, &bp, buf, sizeof buf);
        CHECK(!strcmp(buf, "Access  $0810"), "access bp description");
        bp.type = STUDIO2DEBUG_BP_OUT | STUDIO2DEBUG_BP_IN; bp.start = bp.end = 2;
        studio2bp_describe(NULL, &bp, buf, sizeof buf);
        CHECK(!strcmp(buf, "In/Out  2"), "I/O bp description");
        bp.type = STUDIO2DEBUG_BP_EXEC | STUDIO2DEBUG_BP_WRITE; bp.start = bp.end = 0x0810;
        studio2bp_describe(NULL, &bp, buf, sizeof buf);
        CHECK(!strcmp(buf, "Exec/Write  $0810"), "memory bp description");
    }
    {
        static const studio2debug_region map[] = {
            { 0x0000, 0x03FF, 1, 0, "BIOS ROM" }, { 0x0400, 0x07FF, 1, 0, "Cartridge ROM" },
        };
        CHECK(studio2mem_region_name(map, 2, 0x0420, buf, sizeof buf) > 0 && !strcmp(buf, "Cartridge ROM"),
              "region name");
        CHECK(studio2mem_region_name(map, 2, 0x0900, buf, sizeof buf) == 0 && !buf[0], "no region");
    }

    printf(fails ? "memtools_test: %d failed\n" : "memtools_test: ok\n", fails);
    return fails != 0;
}
