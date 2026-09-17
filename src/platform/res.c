/* Resources, memory manager, file loading, unpacker, UNFLIP, DOS helpers, rand8, fatal, trig helpers —
 * port of TD2EGA 06c9:5d01..5f4e, 06c9:6aa2..7874, 06c9:780e, 06c9:7c16, 06c9:c6f0..c838, 13a3, 16a7,
 * 16b8, 16eb, 1748 (port/spec/platform.md §4.17-4.24, §4.26, §5.2-5.4; checked against the
 * disassembly).
 *
 * All state is in mem[]: the record table DS:68E2 and its pointers DS:6C66..6C6C, the heap bounds
 * DS:68DA/68DC, the Huffman tables in the code segment (CS:7AD6/7AF6/7B16), find_first's DS:5D88.
 * Files are read and written with SDL_IOStream through host_game_path (case-insensitive lookup). */
#include "res.h"

#include <SDL3/SDL_iostream.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../host.h"
#include "../symbols.h"
#include "gfx.h"
#include "input.h"
#include "timer.h"

/* Memory record layout (18 bytes, DS:68E2[50]) */
#define REC_PARAS 0x0C
#define REC_SEG   0x0E
#define REC_FLAGS 0x10
#define REC_SIZE  0x12

#define MM_FIRST  DSW(DS_g_mem_first)      /* constant DS:68E2 */
#define MM_LOW    DSW(DS_g_mem_low)
#define MM_CACHE  DSW(DS_g_mem_cache)
#define MM_LAST   DSW(DS_g_mem_last)       /* constant DS:6C54 */

#define R_PARAS(r) DSW((u16)((r) + REC_PARAS))
#define R_SEG(r)   DSW((u16)((r) + REC_SEG))
#define R_FLAGS(r) DSW((u16)((r) + REC_FLAGS))

/* Huffman tables in the code segment */
#define CS_huff_off   0x7AD6
#define CS_huff_lim   0x7AF6
#define CS_huff_alpha 0x7B16

#define DS_tick_id    0x52C2               /* g_tick_id (char[4]) */
#define DS_dta_name   0x5E0C               /* file name inside the DTA DS:5DEE */
#define DS_sin_tab    0x54AA               /* u16[91] 8.8 sine 0..90 degrees */
#define DS_tan_tab    0x5560               /* u16[] 8.8 tangent */

static const char *ds_str(u16 off) { return (const char *)mp(DGROUP, off); }

/* A C string copied into a zero-filled buffer, so the 12-byte name copies of the original can read
 * past the terminator without leaving the buffer. */
#define NAMEBUF 128
static const char *name_buf(char *buf, const char *name)
{
    size_t i = 0;
    for (; i < NAMEBUF - 1 && name[i]; i++) buf[i] = name[i];
    memset(buf + i, 0, NAMEBUF - i);
    return buf;
}

static u16 paras_of(u32 size)
{
    return (u16)((u16)(size >> 4) + ((size & 0x0F) != 0));
}

static SDL_IOStream *open_game_file(const char *name, const char *mode)
{
    char *path = host_game_path(name, mode[0] == 'w');
    if (!path) return NULL;
    SDL_IOStream *io = SDL_IOFromFile(path, mode);
    host_free(path);
    return io;
}

/* Reads up to n bytes to seg:off (clamped to mem[]); returns the count, -1 on an error. */
static long read_to_mem(SDL_IOStream *io, u16 seg, u16 off, u16 n)
{
    u32 l = lin(seg, off);
    u32 room = l < MEM_SIZE ? MEM_SIZE - l : 0;
    size_t want = n < room ? n : room;
    size_t got = want ? SDL_ReadIO(io, mem + l, want) : 0;
    if (got < want && SDL_GetIOStatus(io) == SDL_IO_STATUS_ERROR) return -1;
    return (long)got;
}

/* ============================================================================================ 13a3 */

/* 13a3:0006 cos_deg8: AL += 90 (8-bit), BX = AL zero-extended, then the sine body with DX = BX >= 0 */
s16 cos_deg8(u8 al)
{
    u16 bx = (u8)(al + 90);
    u16 dx = bx;
    if ((s8)(u8)bx > 90) bx = (bx & 0xFF00) | (u8)(180 - (u8)bx);   /* cmp bl,5Ah ; jle (signed) */
    u16 ax = DSW((u16)(DS_sin_tab + (u16)(bx << 1)));
    if ((s16)dx < 0) ax = (u16)-ax;
    return (s16)ax;
}

/* 13a3:0012 sin_deg8: BX = |cbw(AL)|, fold 91..127 to 180 - x; 0x80 stays (reads past the table) */
s16 sin_deg8(u8 al)
{
    u16 bx = (u16)(s16)(s8)al;
    u16 dx = bx;
    if ((s16)bx < 0) bx = (u16)-bx;
    if ((s8)(u8)bx > 90) bx = (bx & 0xFF00) | (u8)(180 - (u8)bx);
    u16 ax = DSW((u16)(DS_sin_tab + (u16)(bx << 1)));
    if ((s16)dx < 0) ax = (u16)-ax;
    return (s16)ax;
}

/* 13a3:0038 tan_deg8: table DS:5560[|AL|], negated for negative angles (no folding) */
s16 tan_deg8(u8 al)
{
    u16 bx = (u16)(s16)(s8)al;
    u16 dx = bx;
    if ((s16)bx < 0) bx = (u16)-bx;
    u16 ax = DSW((u16)(DS_tan_tab + (u16)(bx << 1)));
    if ((s16)dx < 0) ax = (u16)-ax;
    return (s16)ax;
}

/* ============================================================================================ 16a7 */

/* 16a7:0002 fatal */
_Noreturn void fatal(const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    /* PORT: printf + abort() -> host_fatal (message box, exit code 3); the message's trailing CR and
     * abort()'s "Abnormal program termination" banner are dropped. */
    size_t n = strlen(msg);
    while (n && (msg[n - 1] == '\r' || msg[n - 1] == '\n')) msg[--n] = 0;
    gfx_shutdown();
    kbd_restore();
    timer_restore();
    host_fatal("%s", msg);
}

/* ============================================================================================ DOS helpers */

/* 06c9:5d01 far_memcpy: rep movsb (forward, byte by byte, 16-bit offsets) */
void far_memcpy(FarPtr src, FarPtr dst, u16 n)
{
    u16 si = src.off, di = dst.off;
    while (n--) wr8(dst.seg, di++, rd8(src.seg, si++));
}

/* 06c9:5d2e make_tick_id */
u16 make_tick_id(void)
{
    /* INT 1Ah AH=00h: CX:DX = BIOS tick count (0040:006C, kept by the timer module's BIOS clock
     * model), the midnight flag is cleared. */
    u16 cx = rd16(0x40, 0x6E), dx = rd16(0x40, 0x6C);
    wr8(0x40, 0x70, 0);
    DSW(DS_tick_id) = (u16)((cx & 0x3F3F) | 0x8080);
    DSW(DS_tick_id + 2) = (u16)((dx & 0x3F3F) | 0x8080);
    return DS_tick_id;
}

/* 06c9:5d4e dos_num_drives */
s16 dos_num_drives(void)
{
    return 1;                               /* PORT: disk handling dropped (one logical drive) */
}

/* 06c9:5d5b set_file_hidden */
s16 set_file_hidden(u16 name_ds)
{
    return 0;                               /* PORT: Play Disk creation dropped; reports success */
}

/* 06c9:5d74 bios_floppy_count */
s16 bios_floppy_count(void)
{
    return 2;                               /* PORT: INT 11h dropped (0 or 1 drives -> 2) */
}

/* 06c9:5e1c file_paras */
u16 file_paras_c(const char *name)
{
    SDL_IOStream *io = open_game_file(name, "rb");
    if (!io) fatal(ds_str(DS_s_file_error1), name);           /* "%s FILE ERROR" */
    Sint64 size = SDL_GetIOSize(io);
    SDL_CloseIO(io);
    if (size < 0) size = 0;
    return paras_of((u32)size);
}

u16 file_paras(u16 name_ds) { return file_paras_c(ds_str(name_ds)); }

/* 06c9:5e8a unpacked_paras */
u16 unpacked_paras_c(const char *name)
{
    u8 h[4];
    SDL_IOStream *io = open_game_file(name, "rb");
    if (!io) fatal(ds_str(DS_s_file_error1), name);
    size_t n = SDL_ReadIO(io, h, 4);
    SDL_CloseIO(io);
    if (n != 4) fatal(ds_str(DS_s_file_error1), name);
    u32 size = h[1] | h[2] << 8 | (u32)h[3] << 16;             /* the type byte is ignored */
    return paras_of(size);
}

u16 unpacked_paras(u16 name_ds) { return unpacked_paras_c(ds_str(name_ds)); }

/* 06c9:5ef4 find_first */
u16 find_first_c(const char *spec)
{
    /* PORT: INT 21h 1Ah/4Eh (attributes 6) -> case-insensitive lookup of one file in the game
     * directory. Wildcards are not supported (only the dropped install code uses them); the path part
     * of spec is ignored for the lookup but copied into the result like the original. */
    const char *base = spec;
    for (const char *p = spec; *p; p++)
        if (*p == ':' || *p == '\\' || *p == '/') base = p + 1;
    if (strpbrk(base, "*?")) return 0;
    char *path = host_game_path(base, false);
    if (!path) return 0;
    host_free(path);

    /* DTA name: the directory entry name as DOS reports it (upper case 8.3, NUL-terminated) */
    char dta[13] = { 0 };
    for (int i = 0; i < 12 && base[i]; i++) {
        char c = base[i];
        dta[i] = (c >= 'a' && c <= 'z') ? (char)(c - 0x20) : c;
    }
    memcpy(mp(DGROUP, DS_dta_name), dta, 13);

    u16 di = DS_g_find_path;
    DSW(DS_g_find_name_ptr) = di;
    for (u16 cx = 0x57; cx; cx--) {                          /* copy spec, remember the name position */
        u8 al = (u8)*spec++;
        DSB(di) = al;
        di++;
        if (al == 0) break;
        if (al == ':' || al == '\\') DSW(DS_g_find_name_ptr) = di;
    }
    memcpy(mp(DGROUP, DSW(DS_g_find_name_ptr)), mp(DGROUP, DS_dta_name), 13);
    return DS_g_find_path;
}

u16 find_first(u16 spec_ds)
{
    char buf[NAMEBUF];
    return find_first_c(name_buf(buf, ds_str(spec_ds)));
}

/* 06c9:5f4e find_next */
u16 find_next(void)
{
    return 0;                               /* PORT: no wildcard searches, so there is never a next match */
}

/* 16b8:0000 find_nth_file */
u16 find_nth_file(u16 spec_ds, s16 n)
{
    u16 r = find_first(spec_ds);
    for (s16 i = 1; i < n && r; i++) r = find_next();
    return r;
}

/* ============================================================================================ file loading */

/* 06c9:6aa2 load_file_at */
FarPtr load_file_at_c(const char *name, FarPtr at)
{
    SDL_IOStream *io = open_game_file(name, "rb");
    if (!io) goto err;
    for (u16 seg = at.seg;; seg = (u16)(seg + 0x400)) {
        long n = read_to_mem(io, seg, at.off, 0x4000);
        if (n < 0) goto err;                                  /* the handle is not closed, as the original */
        if (n != 0x4000) break;
    }
    SDL_CloseIO(io);
    return at;
err:
    fatal(ds_str(DS_s_file_error2), name);                    /* "%s FILE ERROR\r" */
}

FarPtr load_file_at(u16 name_ds, FarPtr at) { return load_file_at_c(ds_str(name_ds), at); }

/* 06c9:6d18 load_raw */
static FarPtr load_raw_core(const char *name)
{
    FarPtr p = mem_reclaim_cached_c(name);
    if (p.seg != 0) return p;                                 /* only DX is tested */
    u16 n = file_paras_c(name);
    p = mem_reserve_c(name, n);
    return load_file_at_c(name, p);
}

FarPtr load_raw_c(const char *name)
{
    char buf[NAMEBUF];
    return load_raw_core(name_buf(buf, name));
}

FarPtr load_raw(u16 name_ds) { return load_raw_core(ds_str(name_ds)); }

/* 06c9:7866 / 06c9:7874 */
static s16 write_file_core(const char *name, FarPtr buf, u32 len, bool quiet)
{
    /* PORT: the "quiet" flag is stored below SP in the original (an interrupt can overwrite it);
     * the port keeps it in a normal variable. */
    SDL_IOStream *io = open_game_file(name, "wb");
    if (!io) goto err;
    u16 seg = buf.seg;
    while (len) {
        u16 n = 0x4000;
        if (len < 0x4000) { n = (u16)len; len = 0; }
        else len -= 0x4000;
        u32 l = lin(seg, buf.off);
        u32 room = l < MEM_SIZE ? MEM_SIZE - l : 0;
        size_t want = n < room ? n : room;
        if (want && SDL_WriteIO(io, mem + l, want) < want &&
            SDL_GetIOStatus(io) == SDL_IO_STATUS_ERROR) goto err;
        seg = (u16)(seg + 0x400);
    }
    SDL_CloseIO(io);
    return 0;
err:
    if (!quiet) fatal(ds_str(DS_s_file_error3), name);        /* "%s FILE ERROR\r" */
    if (io) SDL_CloseIO(io);
    return 0;
}

s16 write_file_or_die_c(const char *name, FarPtr buf, u32 len) { return write_file_core(name, buf, len, false); }
s16 write_file_c(const char *name, FarPtr buf, u32 len)        { return write_file_core(name, buf, len, true); }
s16 write_file_or_die(u16 name_ds, FarPtr buf, u32 len)        { return write_file_core(ds_str(name_ds), buf, len, false); }
s16 write_file(u16 name_ds, FarPtr buf, u32 len)               { return write_file_core(ds_str(name_ds), buf, len, true); }

/* ============================================================================================ unpacker */

/* 06c9:6c6e rle_seq_pass (frame of 6b02). Returns DX:AX = output length. */
static u32 rle_seq_pass(u16 ds, u16 si, u16 oseg, u32 count, u8 seq, u8 ec, u16 ax_in)
{
    if (ec == 1) return 0x10000u | ax_in;     /* returns immediately: DX = ec & 7Fh = 1, AX = in[3] */
    u16 es = oseg, di = 0;
    u16 bx = (u16)count, hi = (u16)(count >> 16);
    for (;;) {
        if (si > 0x8000) { si = (u16)(si - 0x8000); ds = (u16)(ds + 0x800); }
        if (di > 0x8000) { di = (u16)(di - 0x8000); es = (u16)(es + 0x800); }
        u8 al = rd8(ds, si++);
        if (al != seq) {
            wr8(es, di++, al);
            if (--bx != 0) continue;
            if (hi == 0) break;
            hi--;
            continue;
        }
        u16 body = di;                        /* no normalization inside the body */
        while ((al = rd8(ds, si++)) != seq) {
            wr8(es, di++, al);
            if (--bx == 0) hi--;
        }
        al = rd8(ds, si++);
        u16 len = (u16)(di - body);
        u8 dl = (u8)(al - 1);
        do {                                  /* n copies in total; n == 1 -> 257, n == 0 -> 256 */
            u16 s = body;
            for (u16 c = len; c; c--) wr8(es, di++, rd8(es, s++));
        } while (--dl != 0);
        u16 borrow = bx < 3;
        bx = (u16)(bx - 3);
        hi = (u16)(hi - borrow);
        if ((hi | bx) == 0) break;
    }
    return (u32)(u16)(es - oseg) * 16u + di;
}

/* 06c9:6b97 rle_run_pass (frame of 6b02) */
static void rle_run_pass(u16 ds, u16 si, u16 es, u32 count, const u8 *tab)
{
    u16 di = 0;
    u16 dx = (u16)count, hi = (u16)(count >> 16);
    for (;;) {
        if (si > 0x8000) { si = (u16)(si - 0x8000); ds = (u16)(ds + 0x800); }
        if (di > 0x8000) { di = (u16)(di - 0x8000); es = (u16)(es + 0x800); }
        u8 al = rd8(ds, si++);
        u8 cl = tab[al];
        if (cl == 0) {
            wr8(es, di++, al);
            if (--dx != 0) continue;
            if (hi == 0) return;
            hi--;
            continue;
        }
        u16 cx, used;
        if (cl == 1) {
            cx = rd8(ds, si++);
            al = rd8(ds, si++);
            used = 3;
        } else if (cl == 3) {
            cx = rd8(ds, si++);
            cx |= (u16)(rd8(ds, si++) << 8);
            al = rd8(ds, si++);
            used = 4;
        } else {
            cx = (u16)(cl - 1);
            al = rd8(ds, si++);
            used = 2;
        }
        for (; cx; cx--) wr8(es, di++, al);   /* rep stosb: DI wraps inside the segment */
        u16 borrow = dx < used;
        dx = (u16)(dx - used);
        hi = (u16)(hi - borrow);
        if ((hi | dx) == 0) return;           /* exact zero only */
    }
}

/* 06c9:6b02 rle_decode */
FarPtr rle_decode(FarPtr in, FarPtr blk, u16 total_paras)
{
    u8 hdr[16];
    u16 ax = rd8(in.seg, (u16)(in.off + 3));  /* AX after the header copy (used by the ec == 1 case) */
    for (int i = 0; i < 16; i++) hdr[i] = rd8(in.seg, (u16)(in.off + 4 + i));
    u32 count = hdr[0] | hdr[1] << 8 | hdr[2] << 16 | (u32)hdr[3] << 24;
    u8 ec = hdr[4];
    u16 ds = in.seg;
    u16 si = (u16)(in.off + 9 + (ec & 0x7F));
    if (ec <= 0x80) {
        count = rle_seq_pass(ds, si, blk.seg, count, hdr[6], ec, ax);
        u16 n = paras_of(count);
        u16 nseg = (u16)(total_paras - n + blk.seg);
        far_move_up(blk.seg, nseg, n);
        ds = nseg;
        si = 0;
    }
    u8 tab[256] = { 0 };
    int nesc = ec & 0x7F;
    /* PORT: ec & 7Fh == 0 makes the original's LOOP run 65536 times over stack bytes; the port uses no
     * escapes. Escapes beyond the 11 copied header bytes are read from the stream instead of the stack. */
    for (int i = 0; i < nesc; i++) {
        u8 code = i < 11 ? hdr[5 + i] : rd8(in.seg, (u16)(in.off + 9 + i));
        tab[code] = (u8)(i + 1);
    }
    rle_run_pass(ds, si, blk.seg, count, tab);
    return far_make(blk.seg, 0);
}

/* 06c9:7c16 huff_decode */
u32 huff_decode(FarPtr in, FarPtr blk, u16 total_paras)
{
    u16 ds = in.seg, si = in.off;
    u16 lo = rd16(ds, (u16)(si + 1));
    u16 hi = rd8(ds, (u16)(si + 3));
    u32 result = (u32)hi << 16 | lo;
    u16 nb = rd8(ds, (u16)(si + 4));
    u16 nlen2 = (u16)((nb & 0x7F) << 1);
    si = (u16)(si + 5);

    u16 cx = CS_huff_alpha, bx = 0, dx = 0, di = 0;
    do {                                      /* per code length: bias (absolute CS offset) and limit */
        CSW((u16)(CS_huff_off + bx)) = cx;
        dx = (u16)(dx << 1);
        CSW((u16)(CS_huff_off + bx)) = (u16)(CSW((u16)(CS_huff_off + bx)) - dx);
        u8 c = rd8(ds, si++);
        dx = (u16)(dx + c);
        cx = (u16)(cx + c);
        if (c) di = dx;
        CSW((u16)(CS_huff_lim + bx)) = di;
        bx = (u16)(bx + 2);
    } while ((s16)bx < (s16)nlen2);
    cx = (u16)(cx - CS_huff_alpha);
    for (u16 d = CS_huff_alpha; cx; cx--) CSB(d++) = rd8(ds, si++);   /* alphabet (may overrun) */

    u16 es = blk.seg;
    di = blk.off;
    u8 cl = 1, ch = 0, ah = 0;
    bool delta = (nb & 0x80) != 0;
    for (;;) {
        bx = 0;
        dx = 0;
        for (;;) {
            if (--cl == 0) {
                ah = rd8(ds, si);
                if (++si == 0) ds = (u16)(ds + 0x1000);
                cl = 8;
            }
            u16 bit = ah & 1;
            ah >>= 1;
            dx = (u16)(dx << 1 | bit);
            if (dx < CSW((u16)(CS_huff_lim + bx))) break;
            bx = (u16)(bx + 2);
        }
        dx = (u16)(dx + CSW((u16)(CS_huff_off + bx)));
        u8 al = CSB(dx);
        if (delta) al = (u8)(al + ch);
        wr8(es, di, al);
        if (++di == 0) { es = (u16)(es + 0x1000); }
        if (delta) ch = al;
        if (--lo != 0) continue;
        if ((s16)--hi >= 0) continue;
        break;
    }
    return result;
}

/* 06c9:6d50 unpack_file */
static FarPtr unpack_core(const char *name)
{
    FarPtr p = mem_reclaim_cached_c(name);
    if (p.seg != 0) return p;
    u16 total = (u16)(unpacked_paras_c(name) + 1);
    FarPtr blk = mem_reserve_c(name, total);
    u16 fparas = file_paras_c(name);
    FarPtr in = load_file_at_c(name, far_make((u16)(total - fparas + blk.seg), blk.off));
    s16 passes = 1;
    u8 t0 = rd8(in.seg, in.off);
    if (t0 & 0x80) {
        passes = t0 & 0x7F;
        in.off = (u16)(in.off + 4);
    }
    for (;;) {
        u8 t = rd8(in.seg, in.off);
        if (t != 1 && t != 2) fatal(ds_str(DS_s_bad_pack), name);   /* "%s INVALID PACK TYPE\r" */
        u32 r;
        if (t == 1) {                                               /* jump table CS:6E2E */
            FarPtr q = rle_decode(in, blk, total);
            r = (u32)q.seg << 16 | q.off;
        } else {
            r = huff_decode(in, blk, total);
        }
        if (--passes <= 0) return far_make((u16)(r >> 16), (u16)r);  /* Huffman last: its size (bug) */
        u16 n = paras_of(r);
        u16 nseg = (u16)(total - n + blk.seg);
        far_move_up(blk.seg, nseg, n);
        in = far_make(nseg, 0);
    }
}

FarPtr unpack_file_c(const char *name)
{
    char buf[NAMEBUF];
    return unpack_core(name_buf(buf, name));
}

FarPtr unpack_file(u16 name_ds) { return unpack_core(ds_str(name_ds)); }

/* ============================================================================================ archives */

static FarPtr lin_to_far(u32 l)
{
    return far_make((u16)(l >> 4), (u16)(l & 0x0F));
}

/* 06c9:6e59 / 06c9:6e4e. name points to 4 bytes that are space-padded in place. */
static FarPtr res_find_core(FarPtr arc, u8 *name, bool must_exist)
{
    for (int bx = 0; bx < 4; bx++) {
        if (name[bx] == 0) {
            for (; bx < 4; bx++) name[bx] = ' ';
            break;
        }
    }
    u16 ds = arc.seg;
    u16 ax = rd16(ds, (u16)(arc.off + 4));
    u16 bx = (u16)(arc.off + 6);
    do {
        int j;
        for (j = 0; j < 4; j++)
            if (rd8(ds, (u16)(bx + j)) != name[j]) break;
        if (j == 4 || (rd8(ds, (u16)(bx + j)) == 0 && name[j] == 0x20)) {
            u16 cnt = rd16(ds, (u16)(arc.off + 4));
            bx = (u16)(bx + (u16)(cnt << 2));
            u16 hdr = (u16)((u16)(cnt << 3) + 6);
            u32 l = ((u32)ds << 4) + hdr + rd32(ds, bx);         /* 32-bit add: FP_OFF(arc) not added */
            return lin_to_far(l);
        }
        bx = (u16)(bx + 4);
    } while ((s16)--ax >= 0);                                     /* count + 1 entries */
    if (must_exist) {
        char n4[5];
        memcpy(n4, name, 4);
        n4[4] = 0;
        fatal(ds_str(DS_s_locateshape), n4);      /* "locateshape - %-4.4s SHAPE OR SOUND NOT FOUND\r" */
    }
    return far_make(0, 0);
}

FarPtr res_find(FarPtr arc, u16 name4_ds)     { return res_find_core(arc, mp(DGROUP, name4_ds), true); }
FarPtr res_find_opt(FarPtr arc, u16 name4_ds) { return res_find_core(arc, mp(DGROUP, name4_ds), false); }

/* PORT: the C-string variants pad a copy, not the caller's string (platform.md §4.20 "Port"). */
FarPtr res_find_c(FarPtr arc, const char *name4)
{
    u8 n[5] = { 0 };
    strncpy((char *)n, name4, 4);
    return res_find_core(arc, n, true);
}

FarPtr res_find_opt_c(FarPtr arc, const char *name4)
{
    u8 n[5] = { 0 };
    strncpy((char *)n, name4, 4);
    return res_find_core(arc, n, false);
}

/* 06c9:c6f0 */
u16 res_count(FarPtr arc)
{
    return rd16(arc.seg, (u16)(arc.off + 4));
}

/* 06c9:c701 res_ptr (res_by_index) */
FarPtr res_ptr(FarPtr arc, u16 index)
{
    u16 ax = (u16)(rd16(arc.seg, (u16)(arc.off + 4)) << 2);
    u16 bx = (u16)((u16)(index << 2) + ax + 6);                  /* FP_OFF(arc) not added */
    ax = (u16)((u16)(ax << 1) + 6);
    u32 l = ((u32)arc.seg << 4) + ax + rd32(arc.seg, bx);
    return lin_to_far(l);
}

/* 06c9:c760 res_name */
void res_name(FarPtr arc, u16 index, u16 out_ds)
{
    u16 bx = (u16)(index << 2);
    DSW(out_ds) = rd16(arc.seg, (u16)(bx + arc.off + 6));
    DSW((u16)(out_ds + 2)) = rd16(arc.seg, (u16)(bx + arc.off + 8));
}

/* 06c9:c838 res_unflip_archive */
void res_unflip_archive(FarPtr arc, FarPtr scratch)
{
    s16 left = (s16)rd16(arc.seg, (u16)(arc.off + 4));
    u16 k = 0;
    do {
        FarPtr r = res_ptr(arc, k);
        u16 rs = r.seg, ro = r.off;
        if ((rd8(rs, (u16)(ro + 15)) & 0xF0) == 0) {
            u8 flags = (u8)(rd8(rs, (u16)(ro + 14)) >> 4);
            if (flags) {
                u16 w = rd16(rs, ro), h = rd16(rs, (u16)(ro + 2));
                u16 sz = (u16)(w * h);
                u16 blk = (u16)(ro + 16);
                for (int b = 0; b < 4; b++) {
                    bool conv = flags & 1;
                    flags >>= 1;
                    if (conv) {
                        /* PORT: w == 0 or h == 0 makes the original's loops run 65536 times; skipped
                         * (no shipped sprite has an empty dimension). */
                        if (w != 0 && h != 0) {
                            u16 di = scratch.off;
                            for (u16 y = 0; y < h; y++) {
                                u16 si = (u16)(blk + y);
                                for (u16 x = 0; x < w; x++) {
                                    wr8(scratch.seg, di++, rd8(rs, si));
                                    si = (u16)(si + h);
                                }
                            }
                        }
                        u16 si = scratch.off, di = blk;
                        for (u16 c = sz; c; c--) wr8(rs, di++, rd8(scratch.seg, si++));
                    }
                    blk = (u16)(blk + sz);
                }
            }
        }
        k++;
    } while (--left > 0);
}

/* 16eb:000a / 004c / 008e / 00d0 */
static void res_find_list_core(FarPtr arc, u16 names_ds, u16 out_ds, bool must_exist)
{
    u16 i = 0;
    while (DSB(names_ds) != 0) {
        FarPtr r = res_find_core(arc, mp(DGROUP, names_ds), must_exist);
        ds_far_wr((u16)(out_ds + (u16)(i << 2)), r);
        i++;
        names_ds = (u16)(names_ds + 4);
    }
}

void res_find_list(FarPtr arc, u16 names_ds, u16 out_ds)         { res_find_list_core(arc, names_ds, out_ds, true); }
void res_find_list_opt(FarPtr arc, u16 names_ds, u16 out_ds)     { res_find_list_core(arc, names_ds, out_ds, false); }
void res_find_list_dup(FarPtr arc, u16 names_ds, u16 out_ds)     { res_find_list_core(arc, names_ds, out_ds, true); }
void res_find_list_opt_dup(FarPtr arc, u16 names_ds, u16 out_ds) { res_find_list_core(arc, names_ds, out_ds, false); }

/* 1748:000e load_shapes */
FarPtr load_shapes_c(const char *name)
{
    char buf[NAMEBUF];
    name_buf(buf, name);                                  /* strcpy(buf, name) (stack buffer in the original) */
    char *dot = buf;
    while (*dot && *dot != '.') dot++;
    FarPtr p;
    if (*dot == 0) {
        for (u16 i = 0;; i++) {
            const char *ext = ds_str(DSW((u16)(DS_g_shape_ext + (u16)(i << 1))));
            if (*ext == 0) break;
            strcpy(dot, ext);
            p = mem_reclaim_cached_c(buf);
            if (p.seg | p.off) return p;
        }
        for (u16 i = 0;; i++) {
            const char *ext = ds_str(DSW((u16)(DS_g_shape_ext + (u16)(i << 1))));
            if (*ext == 0) break;
            strcpy(dot, ext);
            if (find_first_c(buf)) break;
        }
    } else {
        p = mem_reclaim_cached_c(buf);
        if (p.seg | p.off) return p;
    }
    /* strcpy(ext, dot) into a 6-byte stack buffer, then stricmp(ext, ".PES") */
    if (SDL_strcasecmp(dot, ds_str(DS_s_pes_cmp)) == 0) {
        p = unpack_file_c(buf);
        FarPtr tmp = mem_reserve(DS_s_unflip, 0x1F6);      /* "UNFLIP", 8032 bytes of scratch */
        res_unflip_archive(p, tmp);
        mem_free(tmp);
    } else {
        p = load_raw_c(buf);                              /* .ESH: unpacked and already row-major */
    }
    return p;
}

FarPtr load_shapes(u16 name_ds) { return load_shapes_c(ds_str(name_ds)); }

/* 1748:016a shapes_paras */
u16 shapes_paras(u16 name_ds)
{
    char buf[NAMEBUF];
    name_buf(buf, ds_str(name_ds));
    char *dot = buf;
    while (*dot && *dot != '.') dot++;
    if (*dot == 0) {
        for (u16 i = 0;; i++) {
            const char *ext = ds_str(DSW((u16)(DS_g_shape_ext + (u16)(i << 1))));
            if (*ext == 0) break;
            strcpy(dot, ext);
            if (find_first_c(buf)) break;
        }
    }
    if (SDL_strcasecmp(dot, ds_str(DS_s_pes_cmp2)) == 0) return unpacked_paras_c(buf);
    return file_paras_c(buf);
}

/* ============================================================================================ memory manager */

/* 06c9:6f1e mem_init */
void mem_init(u16 top_seg)
{
    DSW(DS_g_mem_cs) = ASM_SEG;
    DSW(DS_g_mem_ds) = DGROUP;
    if (DSW(DS_g_heap_top) == 0) {
        /* PORT: INT 21h 48h (100 paragraphs) and the two 4Ah resizes are replaced by the fixed DOS
         * block HEAP_BOTTOM..HEAP_TOP of mem[]: BX = the requested size or the maximum available. */
        u16 base = HEAP_BOTTOM;
        R_SEG(MM_FIRST) = base;
        DSW(DS_g_heap_base) = base;
        u16 bx = (u16)(top_seg - R_SEG(MM_FIRST));
        u16 max = (u16)(HEAP_TOP - base);
        if (bx > max) bx = max;
        u16 top = (u16)(DSW(DS_g_heap_base) + bx);
        R_SEG(MM_LAST) = top;
        DSW(DS_g_heap_top) = top;
    }
    MM_CACHE = MM_LAST;
    MM_LOW = MM_FIRST;
    for (u16 si = (u16)(MM_FIRST + REC_SIZE); si != MM_LAST; si = (u16)(si + REC_SIZE))
        R_FLAGS(si) = 0;
}

/* 06c9:6f86 */
void mem_init_default(void)
{
    mem_init(0xA000);
}

/* 06c9:6f93 */
void mem_init_reserve(u16 paras)
{
    mem_init(0xA000);
    R_SEG(MM_LAST) = (u16)(R_SEG(MM_LAST) - paras);
    DSW(DS_g_heap_top) = (u16)(DSW(DS_g_heap_top) - paras);
}

/* 06c9:6fb2 */
u16 mem_gap(void)
{
    return (u16)(R_SEG(MM_CACHE) - R_SEG(MM_LOW) - R_PARAS(MM_LOW));
}

/* 06c9:6fc4 */
u16 mem_low_used(void)
{
    return (u16)(R_SEG(MM_LOW) + R_PARAS(MM_LOW) - R_SEG(MM_FIRST));
}

/* 06c9:6fd6 */
u16 mem_free_total(void)
{
    return (u16)(R_SEG(MM_LAST) - R_SEG(MM_LOW) - R_PARAS(MM_LOW));
}

/* 06c9:6fe8 far_copy_down: forward copy of paras * 16 bytes (dst below src) */
void far_copy_down(u16 src_seg, u16 dst_seg, u16 paras)
{
    u32 s = lin(src_seg, 0), d = lin(dst_seg, 0);
    u32 n = (u32)paras * 16;
    for (u32 i = 0; i < n; i++) {
        if (s + i >= MEM_SIZE || d + i >= MEM_SIZE) break;
        mem[d + i] = mem[s + i];
    }
}

/* 06c9:7030 far_move_up: backward copy of paras * 16 bytes (dst above src) */
void far_move_up(u16 src_seg, u16 dst_seg, u16 paras)
{
    u32 s = lin(src_seg, 0), d = lin(dst_seg, 0);
    u32 n = (u32)paras * 16;
    while (n--) {
        if (s + n < MEM_SIZE && d + n < MEM_SIZE) mem[d + n] = mem[s + n];
    }
}

/* 06c9:7083 path_basename */
static const char *basename_c(const char *path)
{
    const char *r = path;
    for (const char *p = path; *p;) {
        char c = *p++;
        if (c == ':' || c == '\\') r = p;
    }
    return r;
}

u16 path_basename(u16 path_ds)
{
    const char *p = ds_str(path_ds);
    return (u16)(path_ds + (basename_c(p) - p));
}

/* 06c9:70a3 mem_reserve. name must be readable for 12 bytes past its basename. */
static FarPtr mem_reserve_core(const char *name, u16 paras)
{
    u16 di = MM_LOW;
    u16 si = MM_CACHE;
    u16 dx = (u16)(R_SEG(di) + R_PARAS(di));
    di = (u16)(di + REC_SIZE);
    if (si <= di) {                                       /* record table full: drop the lowest cache record */
        if (si == MM_LAST) fatal(ds_str(DS_s_reserve_oob), name);   /* OUT OF MEMORY BLOCKS */
        MM_CACHE = (u16)(si + REC_SIZE);
    }
    MM_LOW = di;
    memcpy(mp(DGROUP, di), basename_c(name), 12);
    si = MM_CACHE;
    R_SEG(di) = dx;
    R_PARAS(di) = paras;
    R_FLAGS(di) = 2;
    if ((u16)(paras + dx) > R_SEG(si)) {                  /* evict the cached blocks we overlap */
        si = MM_CACHE;
        di = MM_LOW;
        u16 ax = (u16)(R_SEG(di) + R_PARAS(di));
        while (ax > R_SEG(si)) {
            if (si == MM_LAST) fatal(ds_str(DS_s_reserve_oom), name);  /* OUT OF MEMORY LOADING */
            R_FLAGS(si) = 0;
            si = (u16)(si + REC_SIZE);
            MM_CACHE = si;
        }
    }
    return far_make(dx, 0);
}

FarPtr mem_reserve_c(const char *name, u16 paras)
{
    char buf[NAMEBUF];
    return mem_reserve_core(name_buf(buf, name), paras);
}

FarPtr mem_reserve(u16 name_ds, u16 paras) { return mem_reserve_core(ds_str(name_ds), paras); }

/* 06c9:7147 cache_compact */
void cache_compact(void)
{
    u16 si = MM_LAST, di = MM_LAST;
    u16 dx = 0;
    for (;;) {
        if (!(R_FLAGS(si) & 1)) {
            dx = (u16)(dx + R_PARAS(si));             /* dropped record: a hole */
        } else {
            if (dx != 0) {                            /* move si up under the record above di */
                u16 bx = R_PARAS(si);
                u16 ax = (u16)(R_SEG((u16)(di + REC_SIZE)) - bx);
                u16 src = R_SEG(si);
                R_PARAS(di) = bx;
                R_SEG(di) = ax;
                u16 cx = R_FLAGS(si);
                R_FLAGS(si) = 0;
                R_FLAGS(di) = cx;
                memmove(mp(DGROUP, di), mp(DGROUP, si), 12);
                far_move_up(src, ax, bx);
            }
            di = (u16)(di - REC_SIZE);
        }
        si = (u16)(si - REC_SIZE);
        if (si < MM_CACHE) break;                     /* jae: loop while si >= cache */
    }
    MM_CACHE = (u16)(di + REC_SIZE);
}

/* Name compare shared by 71b4 and 7273: key ends at NUL, which matches a '.' or NUL in the record. */
static bool name_match(const char *key, u16 rec)
{
    for (u16 bx = 0; bx < 12; bx++) {
        u8 al = (u8)key[bx];
        if (al == 0) return DSB((u16)(rec + bx)) == '.' || DSB((u16)(rec + bx)) == 0;
        if (DSB((u16)(rec + bx)) != al) return false;
    }
    return true;
}

/* Search of 71b4/7273: the matching cache record, or 0 */
static u16 cache_search(const char *name)
{
    const char *key = basename_c(name);
    u16 si = MM_CACHE;
    do {
        if (R_FLAGS(si) == 0) return 0;               /* stops at the first free record */
        if (name_match(key, si)) return si;
        si = (u16)(si + REC_SIZE);
    } while (si < MM_LAST);
    return 0;
}

/* 06c9:71b4 mem_reclaim_cached */
static FarPtr mem_reclaim_core(const char *name)
{
    u16 si = cache_search(name);
    if (!si) return far_make(0, 0);
    u16 di = MM_LOW;
    u16 dx = (u16)(R_SEG(di) + R_PARAS(di));
    di = (u16)(di + REC_SIZE);
    MM_LOW = di;
    u16 n = R_PARAS(si), src = R_SEG(si);
    R_FLAGS(si) = 0;
    R_SEG(di) = dx;
    R_PARAS(di) = n;
    R_FLAGS(di) = 2;
    memmove(mp(DGROUP, di), mp(DGROUP, si), 12);
    if (di == MM_CACHE) MM_CACHE = (u16)(MM_CACHE + REC_SIZE);
    far_copy_down(src, dx, n);
    si = MM_CACHE;
    di = MM_LOW;
    while ((u16)(R_SEG(di) + R_PARAS(di)) > R_SEG(si)) {   /* no LAST check here */
        R_FLAGS(si) = 0;
        si = (u16)(si + REC_SIZE);
        MM_CACHE = si;
    }
    cache_compact();
    return far_make(R_SEG(di), 0);
}

FarPtr mem_reclaim_cached_c(const char *name)
{
    char buf[NAMEBUF];
    return mem_reclaim_core(name_buf(buf, name));
}

FarPtr mem_reclaim_cached(u16 name_ds) { return mem_reclaim_core(ds_str(name_ds)); }

/* 06c9:7273 mem_is_cached */
s16 mem_is_cached_c(const char *name)
{
    char buf[NAMEBUF];
    return cache_search(name_buf(buf, name)) ? 1 : 0;
}

s16 mem_is_cached(u16 name_ds) { return cache_search(ds_str(name_ds)) ? 1 : 0; }

/* Inlined search of 72c6/736b/7490/74d4/7501: the low-stack record of a block */
static u16 mem_find_low(u16 seg)
{
    for (u16 si = MM_LOW; si != MM_FIRST; si = (u16)(si - REC_SIZE))
        if (R_SEG(si) == seg) return si;
    fatal(ds_str(DS_s_block_not_found), seg);             /* "memory manager - BLOCK NOT FOUND at SEG= %x\r" */
}

static void pop_low(u16 si)
{
    if (si != MM_LOW) return;
    do si = (u16)(si - REC_SIZE); while (R_FLAGS(si) == 0);
    MM_LOW = si;
}

/* 06c9:72c6 mem_release_cache_old */
FarPtr mem_release_cache_old(FarPtr blk)
{
    u16 si = mem_find_low(blk.seg);
    u16 ret = 0;
    R_FLAGS(si) = 0;
    bool keep = si == MM_LOW;
    if (!keep) {
        u16 bx = MM_LOW;
        u16 gap = (u16)(R_SEG(MM_CACHE) - R_SEG(bx) - R_PARAS(bx));
        keep = gap >= R_PARAS(si);
    }
    if (keep) {
        u16 bx = R_PARAS(si);
        u16 di = MM_CACHE;
        u16 ax = (u16)(R_SEG(di) - bx);
        ret = ax;
        di = (u16)(di - REC_SIZE);                        /* not checked against the low stack */
        MM_CACHE = di;
        R_SEG(di) = ax;
        R_PARAS(di) = bx;
        R_FLAGS(di) = 1;
        memmove(mp(DGROUP, di), mp(DGROUP, si), 12);
        far_move_up(R_SEG(si), ax, bx);
    }
    pop_low(si);
    return far_make(ret, blk.off);
}

/* 06c9:736b mem_release_cache */
FarPtr mem_release_cache(FarPtr blk)
{
    u16 si = mem_find_low(blk.seg);
    R_FLAGS(si) = 0;
    bool keep = si == MM_LOW;
    if (!keep) {
        u16 bx = MM_LOW;
        u16 room = (u16)(R_SEG(MM_LAST) - R_SEG(bx) - R_PARAS(bx));
        keep = room > R_PARAS(si);
    }
    if (keep) {
        u16 lowend = (u16)(R_SEG(MM_LOW) + R_PARAS(MM_LOW));   /* g_low not popped yet */
        bool moved = false;
        u16 di;
        for (di = MM_CACHE; di != MM_LAST; di = (u16)(di + REC_SIZE)) {
            u16 ax = (u16)(R_SEG(di) - R_PARAS(si));
            if (lowend > ax) continue;                    /* would hit the low stack: left in place */
            u16 bx = (u16)(di - REC_SIZE);
            if (bx == MM_LOW) continue;
            if (!moved) { MM_CACHE = bx; moved = true; }
            R_FLAGS(bx) = R_FLAGS(di);
            u16 n = R_PARAS(di);
            R_PARAS(bx) = n;
            R_SEG(bx) = ax;
            u16 src = R_SEG(di);
            memmove(mp(DGROUP, bx), mp(DGROUP, di), 12);
            far_copy_down(src, ax, n);
        }
        u16 ax = R_SEG(di);                               /* LAST.seg */
        di = (u16)(di - REC_SIZE);                        /* new block on top of the cache */
        if (!moved) MM_CACHE = di;
        u16 bx = R_PARAS(si);
        R_PARAS(di) = bx;
        ax = (u16)(ax - bx);
        R_SEG(di) = ax;
        u16 src = R_SEG(si);
        R_FLAGS(di) = 1;
        memmove(mp(DGROUP, di), mp(DGROUP, si), 12);
        far_move_up(src, ax, bx);
    }
    pop_low(si);
    /* Verified: unlike 72c6 this never stores the new segment, so it returns 0:off in every case
     * (platform.md §4.24 says nseg). */
    return far_make(0, blk.off);
}

/* 06c9:7490 mem_free */
void mem_free(FarPtr blk)
{
    u16 si = mem_find_low(blk.seg);
    R_FLAGS(si) = 0;
    pop_low(si);
}

/* 06c9:74d4 mem_size */
u16 mem_size(FarPtr blk)
{
    return R_PARAS(mem_find_low(blk.seg));
}

/* 06c9:7501 mem_slide_down */
FarPtr mem_slide_down(FarPtr blk)
{
    u16 si = mem_find_low(blk.seg);
    u16 di = (u16)(si - REC_SIZE);
    if (R_FLAGS(di) != 0) return far_make(R_SEG(si), 0);
    do di = (u16)(di - REC_SIZE); while (R_FLAGS(di) == 0);
    R_FLAGS(si) = 0;
    u16 bx = R_PARAS(si);
    u16 ax = (u16)(R_SEG(di) + R_PARAS(di));
    u16 src = R_SEG(si);
    di = (u16)(di + REC_SIZE);
    if (si == MM_LOW) MM_LOW = di;
    R_SEG(di) = ax;
    R_PARAS(di) = bx;
    R_FLAGS(di) = 2;
    memmove(mp(DGROUP, di), mp(DGROUP, si), 12);
    far_copy_down(src, ax, bx);
    return far_make(R_SEG(di), 0);
}

/* ============================================================================================ rand8 */

/* 06c9:780e rand8 */
u8 rand8(void)
{
    DSW(DS_rand8_idx)--;
    u16 si = DSW(DS_rand8_idx) & 0xFF;
    u8 al = DSB((u16)(DS_rand8_tab + si));
    u8 carry = al < 0x80;                     /* cmp al,80h ; rcl al,1 */
    al = (u8)(al << 1 | carry);
    DSB((u16)(DS_rand8_tab + si)) = al;
    return al;
}

/* ============================================================================================ PORT: DGROUP stack locals */

static u16 port_sp = DS_STACK_TOP;          /* emulated SP for near-pointer locals (host-side) */

/* Block layout: [u16 size][size bytes]; the returned offset is the first data byte. */
u16 ds_stack_alloc(u16 nbytes)
{
    u16 n = (u16)((nbytes + 1) & ~1u);
    if ((u16)(port_sp - DS_STACK_LIMIT) < (u16)(n + 2))
        host_fatal("DGROUP stack overflow (%u bytes)", (unsigned)nbytes);
    port_sp = (u16)(port_sp - n);
    memset(mp(DGROUP, port_sp), 0, n);
    port_sp = (u16)(port_sp - 2);
    DSW(port_sp) = n;
    return (u16)(port_sp + 2);
}

void ds_stack_release(u16 ds_off)
{
    if (ds_off < (u16)(port_sp + 2) || ds_off > DS_STACK_TOP) return;   /* not a live block: ignored */
    port_sp = (u16)(ds_off + DSW((u16)(ds_off - 2)));
}
