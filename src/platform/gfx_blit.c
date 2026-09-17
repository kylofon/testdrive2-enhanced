/* Sprite blitters of TD2EGA (port/spec/platform.md §4.2): 24 entry points, clipped body 06c9:9189 /
 * 91c1 / 91f3, unclipped body 06c9:9c75 / 9cad / 9cdf, the per-shift routine tables and the
 * constant-plane helpers (RAM 06c9:82d0 / 8336 / 839c, EGA 06c9:ad22 / ad8b / adf4).
 *
 * Transcribed from the disassembly. The eight shift routines of every table differ only in the shift
 * count, so each (RAM / EGA) x (op) x (clipped / unclipped) table is one parameterised routine here; the
 * per-table differences found in the listing are reproduced explicitly:
 *   - edge flags [bp-3Eh]: 3 = both edges visible, bit 0 cleared = right edge clipped (spill byte not
 *     written), bit 1 cleared = left edge clipped (carry seeded from the source byte left of the clip);
 *   - clipped RAM XOR, shift 1 only (06c9:bb7d): the spill byte is XORed with AH, which is 0 when the
 *     visible width is 0, so nothing is drawn there (all other shifts XOR the seeded carry);
 *   - RAM XOR constant-plane helper 06c9:839c swaps the left/right edge masks when an edge is clipped;
 *   - EGA constant-plane helpers: TD1's quirks (middle bytes written back unchanged / XORed);
 *   - clipped EGA path with an empty stored-plane list returns without resetting the GC function;
 *   - `loop` with CX = 0 runs 65536 times: unclipped sprites of width 0, and the constant-plane helpers
 *     when a clipped sprite ends exactly at the left clip edge with x & 7 == 0 (visible width 0; the
 *     helpers run before the width check): RAM XOR then inverts 64 KB per row from the destination,
 *     the EGA helpers write 64 KB per row (whole planes cleared / set / inverted). Kept as is.
 * Blocks beyond the 4 destination slots of the original's stack frame are not emulated (see TODO). */
#include "gfx_ega.h"

enum { OP_REPLACE, OP_OR, OP_AND, OP_XOR };
enum { RECT_CLEAR, RECT_SET, RECT_XOR };

static const u8 op_gcfunc[4] = { 0x00, 0x10, 0x08, 0x18 };   /* [bp-42h] */
static const u8 op_flags[4] = { 3, 2, 1, 4 };                /* [bp-41h] */

/* CS tables (clipped body 06c9:9144.., unclipped body 06c9:9c30.. hold identical copies) */
#define T_BIT_INDEX(b)  CSB(0x9144 + (b))    /* lowest set bit index */
#define T_BIT_VALUE(b)  CSB(0x9154 + (b))    /* lowest set bit value */
#define T_BIT_CLEAR(i)  CSB(0x9164 + (i))    /* 0E 0D 0B 07 */
#define T_PLANE_MASK(i) CSB(0x9168 + (i))    /* 01 02 04 08 */

/* ------------------------------------------------------------------------------------------------ */
/* Constant-plane helpers. asm: ES:DI dst, DX n, SI rows, CX row skip, BX shift, AL edge flags.      */

/* 06c9:82d0 / 8336 / 839c (RAM), mask table CS:82C8 */
static void rect_ram(int op, u16 seg, u16 di, u16 dx, u16 si, u16 cx, u16 bx, u8 al)
{
    if (bx == 0) {
        u16 skip = cx;
        if (op == RECT_XOR) {
            do {
                u16 n = dx;                                        /* loop: 0 -> 65536 */
                do { wr8(seg, di, (u8)(rd8(seg, di) ^ 0xFF)); di++; } while (--n);
                di = (u16)(di + skip);
            } while (dec_jg(&si));
        } else {
            u8 v = op == RECT_SET ? 0xFF : 0x00;
            do {
                for (u16 n = dx; n; n--) wr8(seg, di++, v);         /* rep stosb */
                di = (u16)(di + skip);
            } while (dec_jg(&si));
        }
        return;
    }
    u8 ah = CSB(CS_rect_edge_mask + bx);                           /* destination pixels left of the sprite */
    u8 dh = (u8)~ah;
    u8 dl = (u8)dx;
    switch (op) {
    case RECT_CLEAR:
        if (!(al & 2)) ah = 0;
        if (!(al & 1)) { dh = 0; dl--; cx++; }
        break;
    case RECT_SET:
        if (!(al & 2)) dh = 0xFF;
        if (!(al & 1)) { ah = 0xFF; dl--; cx++; }
        break;
    default:                                                       /* 839c: masks swapped (original bug) */
        if (!(al & 2)) ah = 0xFF;
        if (!(al & 1)) { dh = 0xFF; dl--; cx++; }
        break;
    }
    dl--;
    u16 skip = cx;
    if (dl == 0) {                                                 /* two partial bytes */
        u16 n = si;
        do {
            switch (op) {
            case RECT_CLEAR: wr8(seg, di, (u8)(rd8(seg, di) & ah)); di++; wr8(seg, di, (u8)(rd8(seg, di) & dh)); break;
            case RECT_SET:   wr8(seg, di, (u8)(rd8(seg, di) | dh)); di++; wr8(seg, di, (u8)(rd8(seg, di) | ah)); break;
            default:         wr8(seg, di, (u8)(rd8(seg, di) ^ dh)); di++; wr8(seg, di, (u8)(rd8(seg, di) ^ ah)); break;
            }
            di = (u16)(di + skip);
        } while (--n);
        return;
    }
    if ((s8)dl < 0) {                                              /* one byte */
        u8 m = op == RECT_CLEAR ? (u8)(ah | dh) : (u8)(ah & dh);
        u16 n = si;
        do {
            switch (op) {
            case RECT_CLEAR: wr8(seg, di, (u8)(rd8(seg, di) & m)); break;
            case RECT_SET:   wr8(seg, di, (u8)(rd8(seg, di) | m)); break;
            default:         wr8(seg, di, (u8)(rd8(seg, di) ^ m)); break;
            }
            di = (u16)(di + skip);
        } while (--n);
        return;
    }
    do {
        switch (op) {
        case RECT_CLEAR:
            wr8(seg, di, (u8)(rd8(seg, di) & ah)); di++;
            for (u16 n = dl; n; n--) wr8(seg, di++, 0x00);
            wr8(seg, di, (u8)(rd8(seg, di) & dh));
            break;
        case RECT_SET:
            wr8(seg, di, (u8)(rd8(seg, di) | dh)); di++;
            for (u16 n = dl; n; n--) wr8(seg, di++, 0xFF);
            wr8(seg, di, (u8)(rd8(seg, di) | ah));
            break;
        default: {
            wr8(seg, di, (u8)(rd8(seg, di) ^ dh)); di++;
            u16 n = dl;
            do { wr8(seg, di, (u8)(rd8(seg, di) ^ 0xFF)); di++; } while (--n);
            wr8(seg, di, (u8)(rd8(seg, di) ^ ah));
            break;
        }
        }
        di = (u16)(di + skip);
    } while (dec_jg(&si));
}

/* 06c9:ad22 / ad8b / adf4 (EGA; read map / map mask set by the caller, GC function = the blit's op),
 * mask table CS:AD1A. The edge flags are ignored. The shift != 0, n >= 2 paths write the middle bytes
 * back with their own value (XOR: value ^ FF) and thereby clobber AH, which the next row's first byte
 * (clear) or every row's last byte (set / xor) then uses as its mask. */
static void rect_ega(int op, u16 di, u16 dx, u16 si, u16 cx, u16 bx)
{
    if (bx == 0) {
        u8 al = op == RECT_CLEAR ? 0x00 : 0xFF;
        do {
            u16 n = dx;                                            /* loop: 0 -> 65536 */
            do { (void)ega_read(di); ega_write(di, al); di++; } while (--n);
            di = (u16)(di + cx);
        } while (dec_jg(&si));
        return;
    }
    u8 ah = CSB(0xAD1A + bx), dh = (u8)~ah, dl = (u8)dx;
    if (dl == 1) {
        u16 n = si;
        do {
            u8 v = ega_read(di);
            v = op == RECT_CLEAR ? (u8)(v & ah) : op == RECT_SET ? (u8)(v | dh) : (u8)(v ^ dh);
            ega_write(di, v); di++;
            v = ega_read(di);
            v = op == RECT_CLEAR ? (u8)(v & dh) : op == RECT_SET ? (u8)(v | ah) : (u8)(v ^ ah);
            ega_write(di, v);
            di = (u16)(di + cx);
        } while (--n);
        return;
    }
    dl--;
    do {
        u8 v = ega_read(di);
        v = op == RECT_CLEAR ? (u8)(v & ah) : op == RECT_SET ? (u8)(v | dh) : (u8)(v ^ dh);
        ega_write(di, v); di++;
        u16 n = dl;                                                /* cl = dl, ch = 0 */
        do {
            ah = ega_read(di);
            if (op == RECT_XOR) ah ^= 0xFF;
            ega_write(di, ah); di++;
        } while (--n);
        v = ega_read(di);
        v = op == RECT_CLEAR ? (u8)(v & dh) : op == RECT_SET ? (u8)(v | ah) : (u8)(v ^ ah);
        ega_write(di, v);
        di = (u16)(di + cx);
    } while (dec_jg(&si));
}

/* ------------------------------------------------------------------------------------------------ */
/* Blit body                                                                                        */

static inline u8 op_apply(int op, u8 d, u8 s)
{
    switch (op) {
    case OP_OR:  return (u8)(d | s);
    case OP_AND: return (u8)(d & s);
    case OP_XOR: return (u8)(d ^ s);
    default:     return s;
    }
}

static void blit_body(FarPtr spr, u16 px, u16 py, int op, bool clip)
{
    u16 ds = spr.seg, so = spr.off;
    u8 flags = op_flags[op];
    u16 w = rd16(ds, so), h = rd16(ds, (u16)(so + 2));
    u16 src = (u16)(so + 0x10);                                    /* bp-28 */
    u16 rows, vis_w, row_skip = 0, col;                            /* bp-0E, bp-0C, bp-36, bp-08 */
    u8 edge = 3;                                                   /* bp-3E */
    u16 shift = px & 7;                                            /* bp-34 */

    if (clip) {                                                    /* 06c9:9205 */
        u16 bx = py, cx = h, dx = src;
        u16 y0 = CUR_Y0, y1 = CUR_Y1;
        if (!slt(bx, y0)) {
            if (!sle((u16)(bx + cx), y1)) {
                u16 ax = (u16)(bx + cx - y1);
                if (!sgt(cx, ax)) return;
                cx = (u16)(cx - ax);
            }
        } else {
            u16 ax = py;
            bx = y0;
            if (sle((u16)(ax + cx), bx)) return;
            ax = (u16)(ax + cx - bx);                              /* visible rows */
            u16 skip = (u16)(cx - ax);                             /* xchg cx,ax ; sub ax,cx */
            cx = ax;
            dx = (u16)(dx + (u8)skip * rd8(ds, so));               /* mul byte ptr [si] */
            if (!sle((u16)(bx + cx), y1)) {
                ax = (u16)(bx + cx - y1);
                if (sle(cx, ax)) return;
                cx = (u16)(cx - ax);
            }
        }
        src = dx;
        rows = cx;
        py = bx;

        cx = w;
        dx = 0;
        bx = (u16)((s16)px >> 3);
        u16 x0 = CUR_X0, x1 = CUR_X1;
        if (!slt(bx, x0)) {
            u16 ax = (u16)(bx + cx);
            if (!slt(ax, x1)) {                                    /* right edge at or past the clip */
                edge = 2;
                ax = (u16)(ax - x1);
                if (sle(cx, ax)) return;
                cx = (u16)(cx - ax);
                dx = ax;
            }
        } else {
            u16 ax = (u16)(bx + cx);
            bx = x0;
            if (slt(ax, bx)) return;
            ax = (u16)(ax - bx);                                   /* visible columns (0 allowed) */
            src = (u16)(src + (u16)(cx - ax));
            edge = 1;
            u16 room = (u16)(x1 - bx);
            if (!slt(ax, room)) { ax = room; edge = 0; }
            dx = (u16)(cx - ax);                                   /* xchg dx,cx ; sub dx,ax ; add cx,ax */
            cx = ax;
        }
        vis_w = cx;
        row_skip = dx;
        col = bx;
    } else {                                                       /* 06c9:9cf1 */
        rows = h;
        vis_w = w;
        col = (u16)((s16)px >> 3);
    }

    u16 dst0 = (u16)(col + row_at(py));                            /* bp-2A */
    u8 pm0 = rd8(ds, (u16)(so + 0x0C)), pm1 = rd8(ds, (u16)(so + 0x0D)), pm3 = rd8(ds, (u16)(so + 0x0F));
    u16 blocksize = (u16)((u8)h * rd8(ds, so));                    /* bp-38: mul byte ptr [si] */
    if (pm3 & 0xF0) blocksize = (u16)(blocksize + (pm3 >> 4));
    u16 dstep = (u16)(CUR_STRIDE - vis_w);                         /* bp-2C */
    u8 rect_al = clip ? edge : 3;                                  /* unclipped body passes AL = 3 */
    unsigned n_inv = 8 - shift;
    u8 keep_right = (u8)(0xFF >> shift), keep_left = (u8)~keep_right;

    if (CUR_PLANE(0) != VRAM_SEG) {
        /* destination list (plane segment, source) — processed last to first */
        u16 eseg[4], esrc[4];
        int n = 0;
        u16 p = (u16)(so + 0x0C), dxp = src;
        for (;;) {
            u16 bx = rd8(ds, p) & 0x0F;
            if (!bx) break;
            while (bx) {
                u8 idx = T_BIT_INDEX(bx);
                bx &= T_BIT_CLEAR(idx);
                u16 seg = CUR_PLANE(idx);
                if (!seg) continue;
                /* TODO(verify): a 5th destination overwrites the original's frame (rows, width, ...);
                 * no sprite has more than 4 distinct colour bits, so extra entries are dropped. */
                if (n < 4) { eseg[n] = seg; esrc[n] = dxp; }
                n++;
            }
            if (n >= 4) break;                                     /* cmp di,8 ; jge */
            p++;
            dxp = (u16)(dxp + blocksize);
        }
        if (n > 4) n = 4;

        if ((pm0 & 0xF0) && (flags & 1))
            for (int b = 0; b < 4; b++)
                if ((pm0 & (0x10 << b)) && CUR_PLANE(b))
                    rect_ram(RECT_CLEAR, CUR_PLANE(b), dst0, vis_w, rows, dstep, shift, rect_al);
        if ((pm1 & 0xF0) && (flags & 6))
            for (int b = 0; b < 4; b++)
                if ((pm1 & (0x10 << b)) && CUR_PLANE(b))
                    rect_ram((flags & 4) ? RECT_XOR : RECT_SET, CUR_PLANE(b), dst0, vis_w, rows, dstep, shift, rect_al);

        if (n == 0) return;                                        /* [bp-3Ah] < 0 */
        if (clip && (u16)(vis_w + shift * 2) == 0) return;
        u8 ah_dispatch = (u8)((u16)(vis_w + shift * 2) >> 8);      /* AH on entry to the shift routine */

        for (int e = n - 1; e >= 0; e--) {
            u16 seg = eseg[e], di = dst0, si = esrc[e], r = rows;
            do {
                u16 cnt = vis_w;
                if (shift == 0) {
                    if (op == OP_REPLACE) {
                        for (; cnt; cnt--) wr8(seg, di++, rd8(ds, si++));      /* rep movsb */
                    } else {
                        do {                                                   /* loop */
                            wr8(seg, di, op_apply(op, rd8(seg, di), rd8(ds, si++)));
                            di++;
                        } while (--cnt);
                    }
                } else {
                    u8 dh, ah = ah_dispatch;
                    bool spill = !clip || (edge & 1);
                    switch (op) {
                    case OP_REPLACE: dh = rd8(seg, di); break;
                    case OP_AND:     dh = keep_left; break;
                    default:         dh = 0; break;
                    }
                    bool to_spill = false;
                    if (clip && !(edge & 2)) {
                        dh = (u8)(rd8(ds, (u16)(si - 1)) << n_inv);
                        if (cnt == 0) to_spill = true;
                    }
                    if (!to_spill) {
                        if (op == OP_REPLACE) dh &= keep_left;
                        do {
                            u8 s = rd8(ds, si++);
                            u8 al = (u8)(s >> shift | dh);
                            dh = (u8)(s << n_inv);
                            ah = dh;
                            wr8(seg, di, op_apply(op, rd8(seg, di), al));
                            di++;
                        } while (--cnt);
                    }
                    if (to_spill || spill) {
                        u8 d = rd8(seg, di);
                        switch (op) {
                        case OP_REPLACE: wr8(seg, di, (u8)((d & keep_right) | dh)); break;
                        case OP_OR:      wr8(seg, di, (u8)(d | dh)); break;
                        case OP_AND:     wr8(seg, di, (u8)(d & (dh | keep_right))); break;
                        default:         wr8(seg, di, (u8)(d ^ ((clip && shift == 1) ? ah : dh))); break;
                        }
                    }
                }
                di = (u16)(di + dstep);
                if (clip) si = (u16)(si + row_skip);
            } while (dec_jg(&r));
        }
        return;
    }

    /* EGA path (clipped 06c9:941c, unclipped 06c9:9e61) */
    u8 gcfunc = op_gcfunc[op];
    gc_out(3, gcfunc);
    u16 adv_last = (u16)(blocksize - vis_w);                       /* bp-30 */
    u8 emask[4], eidx[4];
    u16 eadv[4];
    int n = 0;
    u16 back = 0;                                                  /* bp-32 */
    u16 p = (u16)(so + 0x0C);
    for (int k = 0; k < 4; k++, p++) {
        u16 bx = rd8(ds, p) & 0x0F;
        if (!bx) break;
        u16 dx = (u16)-vis_w;
        do {
            u8 ah = T_BIT_INDEX(bx), al = T_BIT_VALUE(bx);
            /* TODO(verify): more than 4 entries overlap the advance slots in the original's frame;
             * not reachable with shipped data (no repeated colour bits). */
            if (n < 4) { emask[n] = al; eidx[n] = ah; eadv[n] = dx; }
            n++;
            bx &= (u16)~al;
        } while (bx);
        if (n <= 4) eadv[n - 1] = adv_last;
        back = (u16)(back + blocksize);
    }
    if (n > 4) n = 4;
    back = (u16)(back - w);

    if ((pm0 & 0xF0) && (flags & 1))
        for (int b = 0; b < 4; b++)
            if (pm0 & (0x10 << b)) {
                gc_out(4, (u8)b);
                seq_map_mask(T_PLANE_MASK(b));
                rect_ega(RECT_CLEAR, dst0, vis_w, rows, dstep, shift);
            }
    if ((pm1 & 0xF0) && (flags & 6))
        for (int b = 0; b < 4; b++)
            if (pm1 & (0x10 << b)) {
                gc_out(4, (u8)b);
                seq_map_mask(T_PLANE_MASK(b));
                rect_ega((flags & 4) ? RECT_XOR : RECT_SET, dst0, vis_w, rows, dstep, shift);
            }

    if (n == 0) {
        if (clip) return;                                          /* 06c9:95aa: GC function stays set */
        gc_out(3, 0);
        return;
    }
    if (clip && (u16)(vis_w + shift * 2) == 0) { gc_out(3, 0); return; }

    u16 si = src, di = dst0, r = rows;
    do {
        for (int e = 0; e < n; e++) {
            seq_map_mask(emask[e]);
            u16 cnt = vis_w;
            if (shift == 0) {
                if (gcfunc == 0 && op != OP_XOR) {
                    for (; cnt; cnt--) { ega_write(di, rd8(ds, si)); di++; si++; }   /* rep movsb / movsw */
                } else {
                    do { (void)ega_read(di); ega_write(di, rd8(ds, si)); di++; si++; } while (--cnt);
                }
            } else {
                gc_out(4, eidx[e]);
                u8 dh = ega_read(di);
                if (op == OP_XOR) dh = 0;
                bool to_spill = false;
                if (clip && !(edge & 2)) {
                    dh = (u8)(rd8(ds, (u16)(si - 1)) << n_inv);
                    if (cnt == 0) to_spill = true;
                }
                if (!to_spill) {
                    dh &= keep_left;
                    do {
                        u8 s = rd8(ds, si++);
                        u8 al = (u8)(s >> shift | dh);
                        dh = (u8)(s << n_inv);
                        ega_write(di, al);
                        di++;
                        (void)ega_read(di);
                    } while (--cnt);
                }
                if (to_spill || !clip || (edge & 1)) {
                    if (op == OP_XOR) ega_write(di, dh);
                    else ega_write(di, (u8)((ega_read(di) & keep_right) | dh));
                }
            }
            di = (u16)(di - vis_w);
            si = (u16)(si + eadv[e]);
        }
        di = (u16)(di + CUR_STRIDE);
        si = (u16)(si - back);
    } while (dec_jg(&r));
    gc_out(3, 0);
}

/* ------------------------------------------------------------------------------------------------ */
/* Entry points                                                                                     */

static inline u16 hdr_hot_x(FarPtr s) { return rd16(s.seg, (u16)(s.off + 4)); }
static inline u16 hdr_hot_y(FarPtr s) { return rd16(s.seg, (u16)(s.off + 6)); }
static inline u16 hdr_own_x(FarPtr s) { return rd16(s.seg, (u16)(s.off + 8)); }    /* TD2: not masked */
static inline u16 hdr_own_y(FarPtr s) { return rd16(s.seg, (u16)(s.off + 0x0A)); }

#define BLIT_FAMILY(name_hot, name_raw, name_own, op, clip)                                              \
    void name_hot(FarPtr spr, s16 x, s16 y)                                                              \
    { blit_body(spr, (u16)((u16)x - hdr_hot_x(spr)), (u16)((u16)y - hdr_hot_y(spr)), op, clip); }       \
    void name_raw(FarPtr spr, s16 x, s16 y) { blit_body(spr, (u16)x, (u16)y, op, clip); }               \
    void name_own(FarPtr spr) { blit_body(spr, hdr_own_x(spr), hdr_own_y(spr), op, clip); }

/* 06c9:916c/91a4/91d6 */ BLIT_FAMILY(blit_copy_clip_hot, blit_copy_clip_raw, blit_copy_clip_own, OP_REPLACE, true)
/* 06c9:a6d8/a6f8/a718 */ BLIT_FAMILY(blit_or_clip_hot, blit_or_clip_raw, blit_or_clip_own, OP_OR, true)
/* 06c9:7d7c/7d9c/7dbc */ BLIT_FAMILY(blit_and_clip_hot, blit_and_clip_raw, blit_and_clip_own, OP_AND, true)
/* 06c9:bac8/bae8/bb08 */ BLIT_FAMILY(blit_xor_clip_hot, blit_xor_clip_raw, blit_xor_clip_own, OP_XOR, true)
/* 06c9:9c58/9c90/9cc2 */ BLIT_FAMILY(blit_copy_hot, blit_copy_raw, blit_copy_own, OP_REPLACE, false)
/* 06c9:a9d2/a9f2/aa12 */ BLIT_FAMILY(blit_or_hot, blit_or_raw, blit_or_own, OP_OR, false)
/* 06c9:808a/80aa/80ca */ BLIT_FAMILY(blit_and_hot, blit_and_raw, blit_and_own, OP_AND, false)
/* 06c9:c132/c152/c172 */ BLIT_FAMILY(blit_xor_hot, blit_xor_raw, blit_xor_own, OP_XOR, false)

/* 06c9:c958 draw_cursor_glyph: XOR hotspot blit of the CS glyph CS:[CS:C8FE + idx*2] at (x & ~7, y) */
void draw_cursor_glyph(s16 x, s16 y, u16 idx)
{
    u16 off = CSW((u16)(CS_cursor_glyph_tab + (u16)(idx * 2)));
    blit_xor_hot(far_make(ASM_SEG, off), (s16)((u16)x & 0xFFF8), y);
}
