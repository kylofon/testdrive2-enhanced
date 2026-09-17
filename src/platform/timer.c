/* Timer ISR, routine list and tick helpers — port of TD2EGA 06c9:5d83..5d8d, 06c9:601c..6230,
 * 06c9:79c8..7a55, 06c9:c692..c6c5 (port/spec/platform.md §4.13, §4.14, §4.27, §6, §7; checked against
 * the disassembly).
 *
 * The real-mode interrupt vector 8 is kept in mem[] (0000:0020): the host tick runs the ISR body only
 * while it points at 06c9:61bf, exactly when the original's ISR would run. The BIOS timer handler that
 * the original chains to (and that runs at 18.2 Hz while the game's ISR is not installed) is reduced to
 * its tick counter at 0040:006C, which bios_ticks() reads. */
#include "timer.h"

#include "../codeptr.h"
#include "../host.h"
#include "../symbols.h"
#include "input.h"
#include "res.h"
#include "sound.h"

#define ISR_OFF        0x61BF
#define IVT8_OFF       0x0020               /* 0000:0020 offset word, 0000:0022 segment word */
#define BIOS_TICKS     0x046C               /* 0040:006C u32 ticks since midnight (linear 0x46C) */
#define BIOS_MIDNIGHT  0x0470               /* 0040:0070 rollover flag */
#define DS_s_no_room   0x5FE8               /* "NO ROOM LEFT ON TIMER INTERRUPT ROUTINE LIST\r" */

/* Host-side model of PIT channel 0 feeding the BIOS clock while INT 8 is not ours (hardware state). */
static u32 bios_pit_acc;

static bool isr_installed(void)
{
    return rd16(0, IVT8_OFF) == ISR_OFF && rd16(0, IVT8_OFF + 2) == ASM_SEG;
}

/* The BIOS INT 8 handler, reduced to the time-of-day counter (floppy motor and INT 1Ch dropped). */
static void bios_int8(void)
{
    u32 t = rd32(0, BIOS_TICKS) + 1;
    if (t >= 0x1800B0u) {
        t = 0;
        wr8(0, BIOS_MIDNIGHT, 1);
    }
    wr32(0, BIOS_TICKS, t);
}

/* Host tick (99.9985 Hz): IRQ 0 */
static void timer_host_tick(void)
{
    if (isr_installed()) {
        timer_isr();
        return;
    }
    /* PORT: while the game's ISR is not hooked, the BIOS handler runs at the BIOS rate (divisor 65536). */
    bios_pit_acc += PIT_DIV_GAME;
    if (bios_pit_acc >= 0x10000u) {
        bios_pit_acc -= 0x10000u;
        bios_int8();
    }
}

void timer_init(void)
{
    codeptr_register(FN_music_tick, music_tick);
    codeptr_register(FN_sfx_tick, sfx_tick);
    host_set_tick_handler(timer_host_tick);
}

/* 06c9:601c timer_install_drive */
void timer_install_drive(void)
{
    DSW(DS_chain_reload) = 5;
    DSW(DS_chain_count) = 5;
    DSW(DS_chain_budget) = 100;
    DSB(DS_chain_countdown_on) = 1;           /* chain_enable left as it is */
    timer_install_common(PIT_DIV_GAME);
}

/* 06c9:603c timer_install_menu (no callers) */
void timer_install_menu(void)
{
    DSW(DS_chain_reload) = 5;
    DSW(DS_chain_count) = 5;
    DSB(DS_chain_countdown_on) = 0;
    DSB(DS_chain_enable) = 1;
    timer_install_common(PIT_DIV_GAME);
}

/* 06c9:6059 timer_install_div */
void timer_install_div(s16 div)
{
    s16 q = idiv32_16(0x10000, div, NULL);    /* DX:AX = 1:0000 */
    DSW(DS_chain_reload) = (u16)q;
    DSW(DS_chain_count) = (u16)q;
    DSB(DS_chain_countdown_on) = 0;
    DSB(DS_chain_enable) = 1;
    timer_install_common((u16)div);
}

/* 06c9:607a timer_install_div_countdown (no callers) */
void timer_install_div_countdown(s16 div)
{
    s16 q = idiv32_16(0x10000, div, NULL);
    DSW(DS_chain_reload) = (u16)q;
    DSW(DS_chain_count) = (u16)q;
    DSW(DS_chain_budget) = 100;
    DSB(DS_chain_countdown_on) = 1;
    timer_install_common((u16)div);
}

/* 06c9:6099 timer_install_common (DX = divisor) */
void timer_install_common(u16 dx_div)
{
    DSW(DS_timer_routines) = 0;               /* list empty */
    DSW(DS_timer_routines + 2) = 0;
    spk_port61_and(0xFC);                     /* speaker off */
    /* PORT: out 43h,B6h and the PIC mask writes are dropped. */
    DSW(DS_prof_index) = 0;
    for (u16 bx = 0; bx < 40 * 2; bx += 2) DSW((u16)(DS_prof_counts + bx)) = 0;
    u16 off = rd16(0, IVT8_OFF);
    if (off != ISR_OFF) DSW(DS_old_int8) = off;
    u16 seg = rd16(0, IVT8_OFF + 2);
    if (seg != ASM_SEG) {
        DSW(DS_old_int8 + 2) = seg;
        wr16(0, IVT8_OFF, ISR_OFF);
        wr16(0, IVT8_OFF + 2, ASM_SEG);
    }
    /* PORT: PIT channel 0 (out 40h, DX) is not reprogrammed: the host tick runs at 1193182 / 0x2E9C Hz,
     * the only divisor the game passes. */
    (void)dx_div;
}

/* 06c9:610a timer_restore */
void timer_restore(void)
{
    if (rd16(0, IVT8_OFF + 2) != ASM_SEG) return;
    if (rd16(0, IVT8_OFF) != ISR_OFF) return;
    wr16(0, IVT8_OFF, DSW(DS_old_int8));
    wr16(0, IVT8_OFF + 2, DSW(DS_old_int8 + 2));
    /* PORT: PIT channel 0 = 65536 is modelled by the BIOS-rate accumulator in timer_host_tick. */
    spk_port61_and(0xFC);
}

/* 06c9:614c timer_add_routine */
void timer_add_routine(FarPtr fn)
{
    u16 bx = DS_timer_routines;
    u16 cx;
    for (cx = 5; cx; cx--, bx = (u16)(bx + 4))
        if (DSW((u16)(bx + 2)) == 0) break;
    if (cx == 0) fatal("%s", (const char *)mp(DGROUP, DS_s_no_room));
    DSW(bx) = fn.off;
    DSW((u16)(bx + 2)) = 0;
    DSW((u16)(bx + 2)) = fn.seg;
    DSW((u16)(bx + 6)) = 0;                   /* terminator (slot 5 when bx is slot 4) */
}

/* 06c9:6180 timer_remove_routine */
void timer_remove_routine(FarPtr fn)
{
    u16 bx = DS_timer_routines;
    u16 cx;
    for (cx = 5; cx; cx--, bx = (u16)(bx + 4))
        if (DSW(bx) == fn.off && DSW((u16)(bx + 2)) == fn.seg) break;
    if (cx == 0) return;
    for (--cx; cx; cx--, bx = (u16)(bx + 4)) {
        DSW(bx) = DSW((u16)(bx + 4));
        DSW((u16)(bx + 2)) = DSW((u16)(bx + 6));
    }
    DSW(bx) = 0;
    DSW((u16)(bx + 2)) = 0;
}

/* 06c9:6230 timer_bios_chain */
void timer_bios_chain(void)
{
    if (DSB(DS_chain_countdown_on) != 0) {
        DSW(DS_chain_budget)--;
        if ((s16)DSW(DS_chain_budget) <= 0) {
            DSB(DS_chain_countdown_on) = 0;
            DSB(DS_chain_enable) = 0;
        }
    }
    /* IVT[8] = old vector; int 8; IVT[8] = ours. The old handler is the BIOS clock. */
    bios_int8();
}

/* 06c9:61bf timer_isr (body; EOI and register saving dropped) */
void timer_isr(void)
{
    DSW(DS_chain_count)--;
    if ((s16)DSW(DS_chain_count) <= 0) {      /* dec / jg */
        DSL(DS_slow_count)++;
        DSW(DS_chain_count) = DSW(DS_chain_reload);
        if (DSB(DS_chain_enable) != 0) timer_bios_chain();
    }
    if (DSB(DS_timer_paused) != 0) {
        spk_port61_and(0xFC);
        return;
    }
    DSW((u16)(DS_prof_counts + DSW(DS_prof_index)))++;
    DSL(DS_tick_count)++;
    for (u16 di = 0; DSW((u16)(DS_timer_routines + 2 + di)) != 0; di = (u16)(di + 4)) {
        CodeFn fn = codeptr_lookup(ds_far((u16)(DS_timer_routines + di)));
        fn();                                 /* far call with interrupts disabled */
    }
}

/* ---- BIOS clock helpers */

/* 06c9:5d83 bios_ticks */
u16 bios_ticks(void)
{
    return rd16(0x40, 0x6C);
}

/* 06c9:5d8d bios_ticks_since */
u16 bios_ticks_since(u16 t0)
{
    return (u16)(rd16(0x40, 0x6C) - t0);
}

/* ---- game tick helpers (DS:5E8C) */

/* 06c9:79c8 */
u32 ticks_get(void)
{
    return DSL(DS_tick_count);
}

/* 06c9:79d2 */
u32 ticks_since(u32 t)
{
    return ticks_get() - t;
}

/* 06c9:79ea */
u32 ticks_lap(void)
{
    u32 last = DSL(DS_lap_start);
    u32 now = ticks_get();
    DSL(DS_lap_start) = now;
    return now - last;
}

/* 06c9:7a07 */
void ticks_reset(void)
{
    DSL(DS_tick_count) = 0;
}

/* 06c9:7a10 */
void deadline_set(u32 n)
{
    DSL(DS_deadline) = ticks_get() + n;
}

/* 06c9:7a27 */
void deadline_wait(void)
{
    while (ticks_get() < DSL(DS_deadline)) host_pump();
}

/* 06c9:7a3b */
s16 deadline_passed(void)
{
    return ticks_get() >= DSL(DS_deadline) ? 1 : 0;
}

/* 06c9:7a55 */
void delay_ticks(u32 n)
{
    u32 end = ticks_get() + n;
    while (ticks_get() < end) host_pump();
}

/* ---- slow clock helpers (DS:5E90, 20 Hz, also while paused) */

/* The slow-clock waits compare "hi < end_hi || lo < end_lo" (no ja after the high-word compare), which
 * keeps waiting up to 65536 more slow ticks when the deadline's low word is above the current low word
 * after the high word has passed. PORT: compared as u32 (platform.md §4.7 / §7 recommendation). */
static bool slow_before(u32 t, u32 end)
{
    return t < end;
}

/* 06c9:c692 */
u32 slow_ticks_get(void)
{
    return DSL(DS_slow_count);
}

/* 06c9:c69c */
void slow_deadline_set(u32 n)
{
    DSL(DS_slow_deadline) = slow_ticks_get() + n;
}

/* 06c9:c6b3 */
void slow_deadline_wait(void)
{
    while (slow_before(slow_ticks_get(), DSL(DS_slow_deadline))) host_pump();
}

/* 06c9:c6c5 */
void delay_slow(u32 n)
{
    u32 end = slow_ticks_get() + n;
    while (slow_before(slow_ticks_get(), end)) host_pump();
}
