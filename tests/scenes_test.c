/* SPDX-License-Identifier: GPL-3.0-only */
/* SLOOP 2.7: the scenes E..H (firmware/src/scenes.c) on the real project, storage and arranger code, over a simulated
 * NOR flash (erase -> 0xFF, program only clears bits, torn writes): save, play in place from flash, live store while
 * playing (waits in RAM, one at a time), erase, a restart, a torn save, the song with scenes E..H. */
#define FELUCCA_ARRANGER 1
#define FELUCCA_FLASH 1
#define main hostsim_main
#include "hostsim.c"
#undef main
#include <assert.h>
#include <stdlib.h>

static uint8_t nor[0x100000] __attribute__((aligned(4096)));
static int fail_after = -1;
static int st_read(uint32_t off, void *dst, uint32_t n) { memcpy(dst, nor + off, n); return 0; }
static int st_erase(uint32_t off) { memset(nor + off, 0xFF, 4096); return 0; }
static int st_prog(uint32_t off, const void *src, uint32_t n)
{
    const uint8_t *s = src;
    uint32_t i;
    if (fail_after == 0)
        return -9;
    if (fail_after > 0)
        fail_after--;
    for (i = 0; i < n; i++)
        nor[off + i] &= s[i];
    return 0;
}
static const uint8_t *st_map(uint32_t off) { return nor + off; }
#include "../firmware/src/storage.c"

#define PROJ_HOST 1
static uint32_t trk_def_engine(uint32_t i) { return i < NPART ? i : 0; }
#include "../firmware/src/project.c"
static struct { uint8_t force; } ui;
static uint8_t sync_reload;
static uint8_t flash_ok = 1;
static uint8_t sec_dirty, song_dirty;
#include "../firmware/src/arranger_scene.c"
#include "../firmware/src/scenes.c"

static int bad;
static void check(const char *what, int ok)
{
    printf("%-78s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}
static uint32_t nor_sum(void) { return proj_hash(nor, sizeof nor); }
static void boot(void)                           /* a power-on: what project.c persist_boot does for E..H */
{
    uint32_t i;
    memset(fsec_ptr, 0, sizeof fsec_ptr);
    fsec_used = 0;
    stage_slot = -1;
    for (i = NSEC_RAM; i < NSEC; i++)
        fsec_refresh(i);
}
static void make_loop(uint32_t seed)             /* a distinct working project each time */
{
    uint32_t i;
    for (i = 0; i < NTRK; i++) {
        steps_clear(&trk[i]);
        trk[i].p[P_SLEN] = (int16_t)(8 + (seed % 5u));
        trk[i].p[P_SDIV] = 2;
        trk[i].p[P_LEVEL] = (int16_t)(60 + seed);
    }
    {
        uint8_t n[1] = {(uint8_t)(40 + seed)};
        put_step(&trk[0], 0, 1, n, ST_NOTE, 0);
    }
}
static uint32_t working_sum(void)                /* the checksum of what the tracks hold now */
{
    static project_t t;
    proj_capture(&t);
    return t.sum;
}

int main(void)
{
    uint32_t i, e_sum, f_sum, before;
    host_tracks_init();
    memset(nor, 0xFF, sizeof nor);
    arr_defaults(&arrangement);

    check("empty flash: no scene E..H, the song's ready mask has none of them", (boot(), 1) &&
          !sec_ok(4) && !sec_ok(5) && !sec_ok(6) && !sec_ok(7) && (arrangement_ready() & 0xF0u) == 0);
    check("sec_ok (project_used, the screens' question) answers for all eight", !sec_ok(4) && !sec_ok(7) && !sec_ok(0));

    /* --- E and F saved while stopped */
    make_loop(1); e_sum = working_sum();
    check("save E: in flash (0)", scene_save(4) == 0);
    check("... E is a scene, played in place from flash (not from RAM), the checksum of the loop it was",
          sec_ok(4) && proj_view(4) == fsec_ptr[0] && proj_view(4) != &proj_stage && proj_ok(proj_view(4)) &&
          proj_view(4)->sum == e_sum);
    check("... the staging area is free again, nothing waits to be written", stage_slot < 0 && !(sec_dirty & 0xF0u));
    make_loop(2); f_sum = working_sum();
    check("save F", scene_save(5) == 0 && sec_ok(5) && proj_view(5)->sum == f_sum && f_sum != e_sum);
    check("... and E is as it was", proj_view(4)->sum == e_sum);
    check("the song may use them: ready mask has E and F; a song E,F,E is valid, one with G is not",
          (arrangement_ready() & 0xF0u) == 0x30u && ({ arr_config_t c; arr_defaults(&c); c.count = 3;
                c.entry[0].scene = 4; c.entry[1].scene = 5; c.entry[2].scene = 4; arr_valid(&c, arrangement_ready() | 0x0Fu); }) &&
          !({ arr_config_t c; arr_defaults(&c); c.count = 1; c.entry[0].scene = 6; arr_valid(&c, arrangement_ready() | 0x0Fu); }));
    check("section_bars of a scene E..H: from its longest pattern (1..64)", section_bars(4) >= 1u && section_bars(4) <= 64u);

    /* --- loading E puts its loop back */
    make_loop(3);
    check("the working loop is not E's now", working_sum() != e_sum);
    proj_apply(proj_view(4), 1);
    check("proj_apply of E (as section_load does): the tracks hold E again", working_sum() == e_sum);
    arrangement_apply(5);
    check("arrangement_apply(5) (the song's step): the tracks hold F", working_sum() == f_sum);

    /* --- restart: E and F come back from flash, G and H are empty */
    boot();
    check("restart: E and F found in flash, G and H none; same projects",
          sec_ok(4) && sec_ok(5) && !sec_ok(6) && !sec_ok(7) && proj_view(4)->sum == e_sum && proj_view(5)->sum == f_sum);

    /* --- live store while playing: waits in RAM, flash untouched, one at a time */
    song.playing = 1;
    make_loop(4); {
        uint32_t g_sum = working_sum();
        before = nor_sum();
        check("live store G while playing: stored (1)", section_store(6) == 1);
        check("... it plays from the staging area; flash is untouched; it waits to be written",
              sec_ok(6) && proj_view(6) == &proj_stage && proj_view(6)->sum == g_sum && nor_sum() == before &&
              stage_slot == 6 && (sec_dirty & 0x40u));
        make_loop(5);
        check("live store H while G still waits and the transport runs: refused (0), G kept",
              section_store(7) == 0 && !sec_ok(7) && stage_slot == 6 && proj_view(6)->sum == g_sum);
        check("live store G again (the same scene, with the newer loop): allowed (1), it replaces the waiting one",
              section_store(6) == 1 && stage_slot == 6 && proj_view(6)->sum == working_sum() && proj_view(6)->sum != g_sum);
        g_sum = working_sum();
        check("A..D are live-stored as ever while E..H waits", section_store(2) == 1 && sec_ok(2) && stage_slot == 6);
        song.playing = 0;                              /* stopped: the flash can be written */
        check("stopped: H live-stored (1): G is written to flash first, then H waits",
              section_store(7) == 1 && stage_slot == 7 && sec_ok(6) && proj_view(6) == fsec_ptr[2] && nor_sum() != before);
        check("... G in flash has the loop it was stored with", proj_view(6)->sum == g_sum);
        check("the main loop, when quiet (sections_write): H into flash, nothing waits",
              fsec_flush() == 0 && stage_slot < 0 && sec_ok(7) && proj_view(7) == fsec_ptr[3]);
    }

    /* --- a restart keeps all four */
    boot();
    check("restart: E F G H all found", sec_ok(4) && sec_ok(5) && sec_ok(6) && sec_ok(7) && (arrangement_ready() & 0xF0u) == 0xF0u);

    /* --- a live store never written (the power goes while it waits): the old flash content stays */
    song.playing = 1;
    make_loop(6);
    check("live store E (replacing it) while playing", section_store(4) == 1 && stage_slot == 4 && proj_view(4)->sum != e_sum);
    boot();                                           /* power loss: the staging area is gone */
    check("power loss before it was written: E is its old self in flash", sec_ok(4) && proj_view(4)->sum == e_sum);
    song.playing = 0;

    /* --- a torn save: the last good scene stays */
    make_loop(7);
    fail_after = 3;
    check("save F with the flash failing mid-write: an error (-1)", scene_save(5) == -1);
    fail_after = -1;
    check("... F still plays (from the staging area, to be written again)", sec_ok(5) && stage_slot == 5);
    boot();
    check("... after a restart F is the last good F (flash was not damaged)", sec_ok(5) && proj_view(5)->sum == f_sum);
    make_loop(7);
    check("save F again, the flash works", scene_save(5) == 0 && proj_view(5)->sum == working_sum() && stage_slot < 0);

    /* --- erase (as project_erase does it: an object of no bytes, then forget) */
    {
        uint32_t g_before = proj_view(6)->sum;
        check("UNDELETE of a scene that is not empty: nothing to do (0)", scene_undelete(6) == 0);
        check("erase G: an object of no bytes", st_save(sec_obj(6), &proj_slot[0], 0) == 0);
        scene_forget(6);
        check("... G is empty, E F H are not touched", !sec_ok(6) && sec_ok(4) && sec_ok(5) && sec_ok(7));
        check("UNDELETE G (the erased scene's older copy): back (1), the same loop it was, nothing waits",
              scene_undelete(6) == 1 && sec_ok(6) && proj_view(6)->sum == g_before && stage_slot < 0);
        boot();
        check("... still G after a restart, E F H untouched", sec_ok(6) && proj_view(6)->sum == g_before &&
              sec_ok(4) && sec_ok(5) && sec_ok(7));
        check("UNDELETE of a scene A..D (RAM slots), or of one with nothing erased behind it: 0",
              scene_undelete(1) == 0 && scene_undelete(4) == 0);
        st_save(sec_obj(6), &proj_slot[0], 0);             /* erased twice: both copies are empty, nothing is behind */
        st_save(sec_obj(6), &proj_slot[0], 0);
        scene_forget(6);
        check("UNDELETE with both copies empty: nothing to restore (0), G stays empty", scene_undelete(6) == 0 && !sec_ok(6));
        make_loop(14); scene_save(6);
        {
            uint32_t g14 = proj_view(6)->sum;
            st_save(sec_obj(6), &proj_slot[0], 0); scene_forget(6);           /* erased once: its project is behind */
            fail_after = 0;                                                   /* ... but the flash cannot be written now */
            check("UNDELETE while the flash fails: an error (-1), the scene waits in RAM, it plays from there",
                  scene_undelete(6) == -1 && sec_ok(6) && stage_slot == 6 && proj_view(6) == &proj_stage &&
                  proj_view(6)->sum == g14);
            fail_after = -1;
            check("... written when the flash works again (the main loop's flush)", fsec_flush() == 0 && stage_slot < 0 &&
                  sec_ok(6) && proj_view(6) == fsec_ptr[2] && proj_view(6)->sum == g14);
            st_save(sec_obj(6), &proj_slot[0], 0); st_save(sec_obj(6), &proj_slot[0], 0);   /* erased twice: empty for the rest */
            scene_forget(6);
        }
    }
    boot();
    check("... also after a restart (an object of no bytes is no scene)", !sec_ok(6) && sec_ok(4) && sec_ok(5) && sec_ok(7));
    check("... and a new G can be saved over it", (make_loop(8), scene_save(6) == 0) && sec_ok(6));
    song.playing = 1;
    make_loop(9);
    check("a scene staged while playing, then erased: forgotten, nothing is written later",
          section_store(7) == 1 && stage_slot == 7 && (scene_forget(7), 1) && stage_slot < 0 && !sec_ok(7));
    song.playing = 0;
    check("... the main loop has nothing to write for it", fsec_flush() == 0 && !sec_ok(7));

    /* --- backups (editor.c objects 12..15): a scene read out, and written back with scene_restore */
    {
        static project_t raw;
        uint32_t h_sum, e_sum2;
        boot();
        make_loop(20); scene_save(7); h_sum = proj_view(7)->sum;
        make_loop(21); scene_save(4); e_sum2 = proj_view(4)->sum;
        memcpy(&raw, proj_view(7), sizeof raw);                 /* what BK_GET would hand out for H */
        check("backup: H read where it is (a project that checks)", proj_ok(&raw) && raw.sum == h_sum);
        scene_save(7);                                           /* H overwritten by the working loop (a different loop) */
        make_loop(22); scene_save(7);
        check("... H is another loop now", proj_view(7)->sum != h_sum);
        check("restore H from the backup: 0, the same loop it was", scene_restore(7, &raw, sizeof raw) == 0 &&
              sec_ok(7) && proj_view(7)->sum == h_sum && stage_slot < 0);
        check("... E untouched", proj_view(4)->sum == e_sum2);
        boot();
        check("... and after a restart", sec_ok(7) && proj_view(7)->sum == h_sum && proj_view(4)->sum == e_sum2);
        check("restore of garbage: 2, nothing changes", ({ static uint8_t junk[sizeof(project_t)]; memset(junk, 0x5A, sizeof junk);
              scene_restore(7, junk, sizeof junk); }) == 2 && proj_view(7)->sum == h_sum);
        check("restore of a scene A..D here: not this function (2)", scene_restore(1, &raw, sizeof raw) == 2);
        song.playing = 1;
        check("restore while the song plays: 3, nothing changes", scene_restore(7, &raw, sizeof raw) == 3 && proj_view(7)->sum == h_sum);
        song.playing = 0;
        fail_after = 0;
        check("restore with the flash failing: 4", scene_restore(6, &raw, sizeof raw) == 4);
        fail_after = -1;
        fsec_flush();
        check("... it waited in RAM and is written now (G is that project)", sec_ok(6) && proj_view(6)->sum == h_sum);
        check("restore of no bytes: the scene is erased (0)", scene_restore(6, &raw, 0) == 0 && !sec_ok(6) && sec_ok(7));
        boot();
        check("... also after a restart", !sec_ok(6) && sec_ok(7) && sec_ok(4));
        make_loop(8); scene_save(6);                             /* G is there again for the checks that follow */
    }

    /* --- a copy that does not check (bit rot in the newest copy) goes to the older one, as storage.c does */
    boot();
    {
        uint32_t k = 0, any = 0;
        st_hdr_t h[2];
        make_loop(10); scene_save(4); make_loop(11); scene_save(4);   /* both copies of E written, the second newer */
        st_head(sec_obj(4), 0, &h[0]); st_head(sec_obj(4), 1, &h[1]);
        k = h[1].seq > h[0].seq ? 1u : 0u;
        nor[st_sector(sec_obj(4), k) + ST_PAYLOAD_OFF + 500] ^= 0x04;
        fsec_refresh(4);
        any = sec_ok(4);
        check("rot in the newer copy of E: E is the older copy (a project that checks), not garbage", any &&
              proj_ok(proj_view(4)) && proj_view(4)->sum != working_sum());
        nor[st_sector(sec_obj(4), 1u - k) + ST_PAYLOAD_OFF + 500] ^= 0x04;
        fsec_refresh(4);
        check("rot in both copies: E is empty (the screens show EMPTY, no crash)", !sec_ok(4) && !proj_view(4));
    }

    /* --- the A..D scenes are as before */
    make_loop(12);
    proj_capture(&proj_slot[1]);
    check("A..D unchanged: proj_view / sec_ok of RAM slots", proj_view(1) == &proj_slot[1] && sec_ok(1) && !sec_ok(0));
    check("out of range scene: no project, not ok", proj_view(8) == 0 && !sec_ok(8));

    printf("scenes test %s\n", bad ? "FAILED" : "passed");
    return bad ? 1 : 0;
}
