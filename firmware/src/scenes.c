/* SPDX-License-Identifier: GPL-3.0-only */
/* SLOOP 2.7: the scenes E..H (the declarations are in project.c, after proj_ok: proj_stage, fsec_ptr, fsec_used, sec_ok,
 * proj_view). A..D are RAM slots with a flash copy, as ever; E..H are in flash only, played in place through the XIP
 * window, with at most one of them waiting in RAM (proj_stage) until the flash can be written. Included by project.c's
 * firmware part and by tests/scenes_test.c, which needs: storage.c (st_*), st_map, flash_ok, sec_dirty, live_sec,
 * proj_capture, song, transport_req. */

#if FELUCCA_FLASH
static uint32_t sec_obj(uint32_t s) { return s < NSEC_RAM ? OBJ_PROJECT0 + s : OBJ_PROJECT4 + (s - NSEC_RAM); }
/* scene s (4..7) as flash has it: the newer valid copy of its object, read where it lies (no copy in RAM). An object
 * of no bytes (erased) is no scene; a copy whose project does not check goes to the older one. Main loop, not the
 * audio ISR: the headers are read by SPI */
static void fsec_refresh(uint32_t s)
{
    uint32_t obj = sec_obj(s), c, first;
    st_hdr_t h[2];
    int v[2];
    fsec_ptr[s - NSEC_RAM] = 0;
    fsec_used &= (uint8_t)~(1u << (s - NSEC_RAM));
    v[0] = st_head(obj, 0, &h[0]) == 0;
    v[1] = st_head(obj, 1, &h[1]) == 0;
    first = v[1] && (!v[0] || h[1].seq > h[0].seq) ? 1u : 0u;      /* the newer first (A on a tie) */
    for (c = 0; c < 2u; c++) {
        uint32_t k = c ? 1u - first : first;
        const project_t *p;
        if (!v[k])
            continue;
        if (!h[k].len)
            return;                                 /* erased */
        if (h[k].len != sizeof(project_t))
            continue;
        p = (const project_t *)st_map(st_sector(obj, k) + ST_PAYLOAD_OFF);
        if (proj_ok(p)) {
            fsec_ptr[s - NSEC_RAM] = p;
            fsec_used |= (uint8_t)(1u << (s - NSEC_RAM));
            return;
        }
    }
}
/* the staged scene (proj_stage) into flash; stopped and quiet only (the erase stops the audio ~50 ms). 0: done, or
 * nothing staged; -1: the write failed (it stays staged) */
static int fsec_flush(void)
{
    uint32_t s;
    if (stage_slot < 0 || !flash_ok)
        return 0;
    s = (uint32_t)stage_slot;
    if (st_save(sec_obj(s), &proj_stage, sizeof proj_stage))
        return -1;
    fsec_refresh(s);                                  /* (now from flash: it is what plays from here on) */
    stage_slot = -1;
    sec_dirty &= (uint8_t)~(1u << s);
    return 0;
}
#endif

/* SAVE > PROJECT > SAVE (stopped) on a scene E..H: the loop captured, then written. 0: in flash; 1: in RAM only (no
 * flash: it stays staged); -1: the write failed (it stays staged, tried again when quiet: sections_flush) */
static int scene_save(uint32_t slot)
{
#if FELUCCA_FLASH
    if (stage_slot >= 0 && stage_slot != (int8_t)slot && fsec_flush())
        return -1;                                      /* (another one still waits, and cannot be written) */
#endif
    proj_capture(&proj_stage);
    stage_slot = (int8_t)slot;
    sec_dirty |= (uint8_t)(1u << slot);
#if FELUCCA_FLASH
    if (flash_ok)
        return fsec_flush() ? -1 : 0;
#endif
    return 1;
}
/* UNDELETE (SLOOP 2.8.1+8S.1): a scene E..H that is empty because it was erased keeps its last project in the object's
 * older copy until the next save writes over it (an erase only writes a newer, empty copy). Found here, copied to
 * proj_stage and written back as the newest copy. 1: restored; 0: nothing to restore (not empty, no older copy that
 * checks); -1: the flash could not be written (another scene still waits, or an error: nothing was lost yet unless the
 * power fails during the write, a few tenths of a second) */
static int scene_undelete(uint32_t s)
{
#if FELUCCA_FLASH
    uint32_t obj, c, best = 2u, bseq = 0;
    st_hdr_t h[2];
    if (s < NSEC_RAM || s >= NSEC || !flash_ok || sec_ok(s))
        return 0;
    if (stage_slot >= 0 && fsec_flush())
        return -1;                                      /* (another one still waits, and cannot be written) */
    obj = sec_obj(s);
    for (c = 0; c < 2u; c++) {
        const project_t *p;
        if (st_head(obj, c, &h[c]) || h[c].len != sizeof(project_t))
            continue;
        p = (const project_t *)st_map(st_sector(obj, c) + ST_PAYLOAD_OFF);
        if (!proj_ok(p) || st_crc32(p, h[c].len) != h[c].crc)
            continue;
        if (best > 1u || h[c].seq > bseq) {
            best = c;
            bseq = h[c].seq;
        }
    }
    if (best > 1u)
        return 0;
    memcpy(&proj_stage, st_map(st_sector(obj, best) + ST_PAYLOAD_OFF), sizeof proj_stage);   /* (its sector is erased next) */
    stage_slot = (int8_t)s;
    sec_dirty |= (uint8_t)(1u << s);
    return fsec_flush() ? -1 : 1;
#else
    (void)s;
    return 0;
#endif
}
/* erased: nothing of it stays, staged or in flash (the object itself was emptied by the caller) */
static void scene_forget(uint32_t slot)
{
    if (stage_slot == (int8_t)slot)
        stage_slot = -1;
    fsec_ptr[slot - NSEC_RAM] = 0;
    fsec_used &= (uint8_t)~(1u << (slot - NSEC_RAM));
}

#if FELUCCA_ARRANGER
/* SAVE + key (ui_layers.c), also while playing: stored into RAM at once. A..D as ever; E..H wait in proj_stage (one of
 * them at a time) for the flash. 1: stored. 0: not: another of E..H still waits and the flash cannot be written now
 * (the transport runs): the UI says so */
static int section_store(uint32_t s)
{
    s &= NSEC - 1u;
    if (s >= NSEC_RAM) {
        if (stage_slot >= 0 && stage_slot != (int8_t)s) {
#if FELUCCA_FLASH
            if (song.playing || transport_req || !flash_ok || fsec_flush())
                return 0;
#else
            return 0;
#endif
        }
        fm1_irq_off();                                  /* (the audio ISR may be applying this scene) */
        proj_capture(&proj_stage);
        stage_slot = (int8_t)s;
        live_sec = (int8_t)s;
        fm1_irq_on();
        sec_dirty |= (uint8_t)(1u << s);
        return 1;
    }
    fm1_irq_off();                                      /* (the audio ISR may be applying a section) */
    proj_capture(&proj_slot[s]);
    live_sec = (int8_t)s;
    fm1_irq_on();
    sec_dirty |= (uint8_t)(1u << s);
    return 1;
}
#endif
