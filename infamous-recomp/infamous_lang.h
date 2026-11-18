/* ============================================================================================================
 * v243K (from PR #25): the game language behind the in-game Settings menu's Language row (included once from stubs.cpp; inert unless PS3_LANG
 * names a language the game would not pick by itself).
 *
 * RE (EU EBOOT, BCES00220): at boot the game (func_00089F80) works out which release it is by matching the title id
 * cellDiscGameGetBootDiscInfo reports against the 7 it knows: BCUS98119 (US), BCES00220, BCES00591, BCES00609,
 * BCAS20074, BCET00010 and BCET00220. Each release has the list of languages it ships, at 0x00720868: 7 x
 * { u32 count, u32 list }, in the game's own numbers (1 English, 2 French, 3 Spanish, 4 German, 5 Italian, 6 Dutch,
 * 7 Portuguese, 8 Russian, 9 Korean, 11 Chinese, 16 Polish, 17 English (UK)). The PS3 system language picks from that
 * list, and one that isn't in it gets the list's first language (func_00089818).
 *
 * The port reports the title id BLES00000, which is none of them, so the game takes the first release, the US one:
 * English, French and Spanish only. Every other system language came out in English, although the disc has every
 * European language (dialog/archive/<language>.psarc, movies/subtitles/<language>). Reporting a real release id
 * would move the save folder (the game names it after that id), so instead, when the game reads the system language,
 * the US release's list is pointed at the entry of another release's list that holds the chosen language: a list of
 * one, which the game then takes. English, French and Spanish need nothing.
 * ============================================================================================================ */
#include <cstdint>
#include <cstdio>

/* Called by cellSysutil.c when the game reads the PS3 system language (selectany: cellSysutil.c defines it too). */
#if defined(_MSC_VER)
extern "C" { __declspec(selectany) void (*g_sysutil_lang_hook)(int) = nullptr; }
#else
extern "C" { __attribute__((weak)) void (*g_sysutil_lang_hook)(int) = nullptr; }
#endif

namespace infamous_lang {
static const uint32_t LISTS = 0x00720868u, RELEASES = 7u;
static const uint32_t SHIPPED_COUNT[RELEASES] = { 3, 5, 1, 3, 3, 8, 9 };

static void on_system_language(int cell)
{
    static bool done = false;
    if (done) return;
    done = true;
    /* CELL_SYSUTIL_LANG_ -> the game's number: the same up to Polish (16), except Chinese (one, 11); Portuguese (Brazil)
     * 17 -> 7, English (UK) 18 -> 17 */
    const int want = cell == 18 ? 17 : cell == 17 ? 7 : cell == 10 ? 11 : cell;
    if (want == 1 || want == 2 || want == 3) return;   /* the US list has these */
    for (uint32_t r = 0; r < RELEASES; r++)
        if (vm_read32(LISTS + 8u * r) != SHIPPED_COUNT[r]) {
            std::fprintf(stderr, "[LANG] the language lists aren't at 0x%08X in this build of the game -> language %d not set\n", LISTS, cell);
            return;
        }
    for (uint32_t r = 1; r < RELEASES; r++) {
        const uint32_t list = vm_read32(LISTS + 8u * r + 4u);
        for (uint32_t i = 0; i < SHIPPED_COUNT[r]; i++)
            if ((int)vm_read32(list + 4u * i) == want) {
                vm_write32(LISTS + 4u, list + 4u * i);
                vm_write32(LISTS, 1u);
                std::fprintf(stderr, "[LANG] game language %d (system language %d)\n", want, cell);
                return;
            }
    }
    std::fprintf(stderr, "[LANG] the game has no language %d -> English\n", cell);
}

static struct Hook {
    Hook() { g_sysutil_lang_hook = on_system_language; }
} s_hook;
} // namespace infamous_lang
