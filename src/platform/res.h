#pragma once
/* Resources, memory manager, file loading, unpacker, UNFLIP, DOS helpers, rand8, fatal and the DSI trig
 * helpers of segment 13a3 — port of TD2EGA 06c9:5d01..5f4e, 06c9:6aa2..7874, 06c9:7c16, 06c9:c6f0..c838,
 * 13a3, 16a7, 16b8, 16eb, 1748 (port/spec/platform.md §2.4, §3.4, §4.17-4.24, §4.26, §5.2-5.4).
 *
 * Memory: the game's memory manager is kept as in the original. Its 50 records live at DS:68E2
 * (DS_g_mem_rec) and manage the "DOS block" HEAP_BOTTOM..HEAP_TOP of mem[]. Archives, sound files,
 * stage files and WINDOW buffers are FarPtrs into mem[] with offset 0 (res_find returns normalized
 * pointers, offset 0..15).
 *
 * Pointer arguments follow PORTING.md: a near data pointer (`char *` in the spec) is a DGROUP offset
 * (`u16 x_ds`), a far pointer a FarPtr. Functions that only READ a file / block name also have a `_c`
 * twin taking a host C string, for names the port builds outside DGROUP (e.g. sprintf into a C local);
 * both behave identically. Names are looked up case-insensitively in the game directory
 * (host_game_path). */
#include "../mem.h"

/* ---- 13a3: DSI 8.8 trigonometry (256 = 1.0). AL argument, AX result, BX/DX preserved. */
s16 cos_deg8(u8 al);                                          /* 13a3:0006 sin(al + 90), 8-bit add */
s16 sin_deg8(u8 al);                                          /* 13a3:0012 table DS:54AA, al signed degrees */
s16 tan_deg8(u8 al);                                          /* 13a3:0038 table DS:5560, al signed degrees */

/* ---- 16a7: fatal error. The original passes a DGROUP format string and up to 4 stack words to printf;
 * the port takes a C format string and C varargs (convert DGROUP strings with mp(DGROUP, off)).
 * Restores keyboard and timer, then host_fatal (message box, exit code 3). */
_Noreturn void fatal(const char *fmt, ...);                   /* 16a7:0002 */

/* ---- small DOS / BIOS helpers (06c9:5d01..5f4e, 16b8) */
void far_memcpy(FarPtr src, FarPtr dst, u16 n);               /* 06c9:5d01 (soff, sseg, doff, dseg, n); n == 0 copies nothing */
u16  make_tick_id(void);                                      /* 06c9:5d2e returns DS:52C2 (4 bytes, no NUL) */
s16  dos_num_drives(void);                                    /* 06c9:5d4e PORT: returns 1 */
s16  set_file_hidden(u16 name_ds);                            /* 06c9:5d5b PORT: no-op, returns 0 */
s16  bios_floppy_count(void);                                 /* 06c9:5d74 PORT: returns 2 */
u16  file_paras(u16 name_ds);                                 /* 06c9:5e1c size in paragraphs; fatal "%s FILE ERROR" */
u16  file_paras_c(const char *name);
u16  unpacked_paras(u16 name_ds);                             /* 06c9:5e8a u24 at +1 in paragraphs */
u16  unpacked_paras_c(const char *name);
u16  find_first(u16 spec_ds);                                 /* 06c9:5ef4 -> DS:5D88 (dir part + name) or 0 */
u16  find_first_c(const char *spec);
u16  find_next(void);                                         /* 06c9:5f4e -> DS:5D88 or 0 */
u16  find_nth_file(u16 spec_ds, s16 n);                       /* 16b8:0000 */

/* ---- file loading / writing (06c9:6aa2, 6d18, 7866, 7874) */
FarPtr load_file_at(u16 name_ds, FarPtr at);                  /* 06c9:6aa2 whole file to at (0x4000 chunks, seg += 0x400) */
FarPtr load_file_at_c(const char *name, FarPtr at);
FarPtr load_raw(u16 name_ds);                                 /* 06c9:6d18 cached or reserve + load unchanged */
FarPtr load_raw_c(const char *name);
s16    write_file_or_die(u16 name_ds, FarPtr buf, u32 len);   /* 06c9:7866 fatal on error, returns 0 */
s16    write_file_or_die_c(const char *name, FarPtr buf, u32 len);
s16    write_file(u16 name_ds, FarPtr buf, u32 len);          /* 06c9:7874 errors ignored, returns 0 */
s16    write_file_c(const char *name, FarPtr buf, u32 len);

/* ---- unpacker (06c9:6b02, 6d50, 7c16). Streams are decoded in place inside the reserved block. */
FarPtr unpack_file(u16 name_ds);                              /* 06c9:6d50 cached or reserve + load + 1..k passes */
FarPtr unpack_file_c(const char *name);
FarPtr rle_decode(FarPtr in, FarPtr blk, u16 total_paras);    /* 06c9:6b02 returns blk.seg:0000 */
u32    huff_decode(FarPtr in, FarPtr blk, u16 total_paras);   /* 06c9:7c16 returns the u24 size of its header */

/* ---- archives (06c9:6e4e, 6e59, c6f0, c701, c760, c838, 16eb, 1748) */
FarPtr res_find(FarPtr arc, u16 name4_ds);                    /* 06c9:6e59 "locateshape"; fatal if missing */
FarPtr res_find_c(FarPtr arc, const char *name4);
FarPtr res_find_opt(FarPtr arc, u16 name4_ds);                /* 06c9:6e4e 0:0 if missing */
FarPtr res_find_opt_c(FarPtr arc, const char *name4);
u16    res_count(FarPtr arc);                                 /* 06c9:c6f0 word at arc+4 */
FarPtr res_ptr(FarPtr arc, u16 index);                        /* 06c9:c701 normalized pointer of resource k (res_by_index) */
void   res_name(FarPtr arc, u16 index, u16 out_ds);           /* 06c9:c760 4 name bytes -> DS:out */
void   res_unflip_archive(FarPtr arc, FarPtr scratch);        /* 06c9:c838 column-major blocks -> row-major */
void   res_find_list(FarPtr arc, u16 names_ds, u16 out_ds);   /* 16eb:000a far ptrs to DS:out[i] until a NUL name */
void   res_find_list_opt(FarPtr arc, u16 names_ds, u16 out_ds);   /* 16eb:004c */
void   res_find_list_dup(FarPtr arc, u16 names_ds, u16 out_ds);   /* 16eb:008e (copy of 000a, no callers) */
void   res_find_list_opt_dup(FarPtr arc, u16 names_ds, u16 out_ds); /* 16eb:00d0 (copy of 004c, no callers) */
FarPtr load_shapes(u16 name_ds);                              /* 1748:000e .PES/.ESH search, unpack + UNFLIP, cache */
FarPtr load_shapes_c(const char *name);
u16    shapes_paras(u16 name_ds);                             /* 1748:016a */

/* ---- memory manager (06c9:6f1e..7501). Blocks are named by the basename of the file (12 bytes). */
void   mem_init(u16 top_seg);                                 /* 06c9:6f1e PORT: DOS block = HEAP_BOTTOM..min(top, HEAP_TOP) */
void   mem_init_default(void);                                /* 06c9:6f86 mem_init(0xA000) */
void   mem_init_reserve(u16 paras);                           /* 06c9:6f93 */
u16    mem_gap(void);                                         /* 06c9:6fb2 */
u16    mem_low_used(void);                                    /* 06c9:6fc4 */
u16    mem_free_total(void);                                  /* 06c9:6fd6 */
void   far_copy_down(u16 src_seg, u16 dst_seg, u16 paras);    /* 06c9:6fe8 forward paragraph copy */
void   far_move_up(u16 src_seg, u16 dst_seg, u16 paras);      /* 06c9:7030 backward paragraph copy */
u16    path_basename(u16 path_ds);                            /* 06c9:7083 after the last ':' or '\' */
FarPtr mem_reserve(u16 name_ds, u16 paras);                   /* 06c9:70a3 "reservememory" (e.g. DS_s_window) */
FarPtr mem_reserve_c(const char *name, u16 paras);
void   cache_compact(void);                                   /* 06c9:7147 */
FarPtr mem_reclaim_cached(u16 name_ds);                       /* 06c9:71b4 0:0 if not cached */
FarPtr mem_reclaim_cached_c(const char *name);
s16    mem_is_cached(u16 name_ds);                            /* 06c9:7273 */
s16    mem_is_cached_c(const char *name);
FarPtr mem_release_cache_old(FarPtr blk);                     /* 06c9:72c6 free + cache at the bottom (evicted first) */
FarPtr mem_release_cache(FarPtr blk);                         /* 06c9:736b free + cache at the top */
void   mem_free(FarPtr blk);                                  /* 06c9:7490 */
u16    mem_size(FarPtr blk);                                  /* 06c9:74d4 */
FarPtr mem_slide_down(FarPtr blk);                            /* 06c9:7501 */

/* ---- rand8 (06c9:780e): table generator DS:66E6 / DS:66E8 */
u8 rand8(void);

/* ---- PORT: locals the original keeps on its stack. The stack is inside DGROUP (_astart sets SS = DS,
 * SP = DS:A3CE; the stack occupies about DS:9430..A3CE), so the game passes such locals as near
 * pointers (gfx_targets_save(&local), text_state_save, name buffers). The port does not run on that
 * stack, so these helpers hand out LIFO blocks from the same region: ds_stack_alloc(n) returns the DS
 * offset of n zeroed bytes, ds_stack_release(off) frees that block and every block allocated after it. */
#define DS_STACK_TOP   0xA3CE
#define DS_STACK_LIMIT 0x9430
u16  ds_stack_alloc(u16 nbytes);
void ds_stack_release(u16 ds_off);
