/* Driving simulation: traffic lists, meeting / pushing cars, collisions between the player, the opponent
 * and the police, and the traffic resync after a (re)start — port/spec/simulation.md §4.13, §4.14, §4.16
 * (06c9:4d3f..06c9:50a0, 06c9:595f..06c9:5a6e, 06c9:5ada..06c9:5c59), checked against work/sim/rdis.txt. */
#define SIM_INTERNAL
#include "sim.h"

static u16 car_w(u16 list, u16 idx, u16 field) { return DSW((u16)(list + idx + field)); }
static void car_set(u16 list, u16 idx, u16 sub, u16 pos)
{
    DSW((u16)(list + idx + CAR_SUB)) = sub;
    DSW((u16)(list + idx + CAR_POS)) = pos;
}
static SimDiff car_diff(u16 list, u16 idx, u16 dx_pos, u16 ax_sub)
{
    return sim_diff(car_w(list, idx, CAR_POS), car_w(list, idx, CAR_SUB), dx_pos, ax_sub);
}

/* 06c9:4d3f traffic — simulation.md §4.13 */
void traffic(void)
{
    u16 len = DSW(DS_stage_len);
    u16 si = 0;
    do {                                                /* oncoming cars move towards the player */
        u16 e = (u16)(ONC_BASE + si);
        u16 cx = DSW(e + CAR_SUB), bx = DSW(e + CAR_POS);
        u16 t = DSW(DS_traffic_speed);
        bool borrow = cx < t;
        cx = (u16)(cx - t);
        if (borrow) {
            cx = (u16)((cx & 0x00FF) | (u8)((cx >> 8) + 1) << 8);   /* inc ch */
            bx--;
            if (bx < DS_ROAD0) bx = (u16)(bx + len);
        }
        DSW(e + CAR_SUB) = cx;
        DSW(e + CAR_POS) = bx;
        s16 ax = DSS(e + CAR_X);
        if (DSW(e + CAR_TYPE) != 6 && (ROAD((u16)(bx - 8)) & 0x80) && DSB(DS_median) != 0) {
            ax = (s16)(ax - 20);
            if (!(ax > -600)) ax = -600;
        } else {
            ax = (s16)(ax + 20);
            if (!(ax < -200)) ax = -200;
        }
        DSS(e + CAR_X) = ax;
        si = (u16)(si + 8);
    } while (si != DSW(DS_oncoming_count8));            /* do-while as in the original */

    si = 0;
    do {                                                /* same-direction cars */
        u16 e = (u16)(SAME_BASE + si);
        u16 cx = (u16)(DSW(e + CAR_SUB) + DSW(DS_traffic_speed));
        u16 bx = DSW(e + CAR_POS);
        if ((cx >> 8) != 0) {
            cx = (u16)(cx - 0x100);
            bx++;
            if (bx >= DSW(DS_stage_end)) bx = (u16)(bx - len);
        }
        DSW(e + CAR_SUB) = cx;
        DSW(e + CAR_POS) = bx;
        s16 ax = DSS(e + CAR_X);
        if (DSW(e + CAR_TYPE) != 6 && (ROAD(bx) & 0x80)) {
            ax = (s16)(ax + 20);
            if (!(ax < 600)) ax = 600;
        } else {
            ax = (s16)(ax - 20);
            if (!(ax > 200)) ax = 200;
        }
        DSS(e + CAR_X) = ax;
        si = (u16)(si + 8);
    } while (si != DSW(DS_same_count8));

    DSB(DS_pass_mode) = 0;
    DSB(DS_force_meet) = 0;

    /* ---- player */
    u16 n_onc = DSW(DS_oncoming_count8), n_same = DSW(DS_same_count8);
    si = DSW(DS_onc_next);
    bool hit = meet_check(&si, ONC_BASE, n_onc, DSW(DS_player_sub), DSW(DS_player_pos), DSS(DS_car_centre));
    u16 old = DSW(DS_onc_next);
    DSW(DS_onc_next) = si;
    if (old != si) {
        if (si == 0) si = n_onc;
        si = (u16)(si - 8);                             /* the car just met */
        if (DSW(ONC_BASE + si + CAR_TYPE) == 4 && DSW(DS_cop_pos) == 0 && DSB(DS_speed_hi) > 0x46) {
            DSW(DS_chase_speed) = DSW(DS_speed);
            DSB(DS_cop_state) = 1;
            police_start();
        }
    }
    if (hit) DSB(DS_run_state) = 2;
    if (DSB(DS_cop_state) == 7) DSB(DS_force_meet) = 1;
    si = DSW(DS_same_next);
    push_behind(&si, DSW(DS_player_sub), DSW(DS_player_pos), DSS(DS_car_centre));
    DSW(DS_same_next) = si;
    DSB(DS_force_meet) = 0;
    si = DSW(DS_same_next);
    hit = meet_check(&si, SAME_BASE, n_same, DSW(DS_player_sub), DSW(DS_player_pos), DSS(DS_car_centre));
    DSW(DS_same_next) = si;
    if (hit) DSB(DS_run_state) = 2;

    /* ---- opponent */
    if (DSB(DS_opp_enabled) != 0) {
        DSB(DS_pass_mode) = DSB(DS_opp_pass_mode);
        si = DSW(DS_opp_onc_next);
        hit = meet_check(&si, ONC_BASE, n_onc, DSW(DS_opp_sub), DSW(DS_opp_pos), DSS(DS_opp_x));
        DSW(DS_opp_onc_next) = si;
        if (hit && DSB(DS_opp_crash_timer) == 0) {
            DSB(DS_opp_crash_timer) = 30;
            DSW(DS_opp_crashes)++;
        }
        si = DSW(DS_opp_same_next);
        push_behind(&si, DSW(DS_opp_sub), DSW(DS_opp_pos), DSS(DS_opp_x));
        DSW(DS_opp_same_next) = si;
        si = DSW(DS_opp_same_next);
        hit = meet_check(&si, SAME_BASE, n_same, DSW(DS_opp_sub), DSW(DS_opp_pos), DSS(DS_opp_x));
        DSW(DS_opp_same_next) = si;
        if (hit && DSB(DS_opp_crash_timer) == 0) {
            DSB(DS_opp_crash_timer) = 30;
            DSW(DS_opp_crashes)++;
        }
    }

    /* ---- police: pushes cars, never crashes */
    if (DSB(DS_cop_active) == 0) return;
    DSB(DS_pass_mode) = 0;
    si = DSW(DS_cop_onc_next);
    meet_check(&si, ONC_BASE, n_onc, DSW(DS_cop_sub), DSW(DS_cop_pos), DSS(DS_cop_x));
    DSW(DS_cop_onc_next) = si;
    DSB(DS_force_meet) = DSB(DS_cop_force_meet);
    si = DSW(DS_cop_same_next);
    push_behind(&si, DSW(DS_cop_sub), DSW(DS_cop_pos), DSS(DS_cop_x));
    DSW(DS_cop_same_next) = si;
    si = DSW(DS_cop_same_next);
    meet_check(&si, SAME_BASE, n_same, DSW(DS_cop_sub), DSW(DS_cop_pos), DSS(DS_cop_x));
    DSW(DS_cop_same_next) = si;
}

/* 06c9:4f92 meet_check */
bool meet_check(u16 *si_idx, u16 bp_list, u16 cx_n8, u16 ax_sub, u16 dx_pos, s16 di_x)
{
    u16 len = DSW(DS_stage_len);
    u16 si = *si_idx;
    SimDiff d = car_diff(bp_list, si, dx_pos, ax_sub);
    s16 k = d.k;
    if (!diff_le(d)) k = (s16)(k - len);                /* ahead: not yet */
    if (k < -4) return false;
    if (DSB(DS_force_meet) == 0) {
        if (DSC(DS_pass_mode) > 0) goto pass;
        s16 x = DSS(bp_list + si + CAR_X);
        s16 di = (s16)(di_x - 0xB4);
        if (x < di) goto pass;
        di = (s16)(di + 0x168);
        if (x > di) goto pass;
    }
    /* collision: shove this car and the ones right behind it in front of the driver (simulation.md Q9) */
    {
        u16 i0 = si;
        u16 dx = wrap_up((u16)(dx_pos + 1));
        for (;;) {
            car_set(bp_list, si, ax_sub, dx);
            dx = wrap_up((u16)(dx + 3));
            si = (u16)(si + 8);
            if (si == cx_n8) si = 0;
            if (si == i0) break;
            d = car_diff(bp_list, si, dx, ax_sub);
            k = d.k;
            if (!d.lt) k = (s16)(k - len);
            if (k < -4) break;
        }
        return true;                                    /* *si_idx unchanged (pop si) */
    }
pass:
    si = (u16)(si + 8);
    if (si == cx_n8) si = 0;
    *si_idx = si;
    return false;
}

/* 06c9:5022 push_behind: same-direction cars cannot drive through a driver */
void push_behind(u16 *si_idx, u16 ax_sub, u16 dx_pos, s16 di_x)
{
    u16 len = DSW(DS_stage_len);
    u16 cx = *si_idx, si = cx, bp = 0;
    u16 dx = dx_pos;
    s16 k;
    for (;;) {
        if (si == 0) si = DSW(DS_same_count8);
        si = (u16)(si - 8);
        SimDiff d = car_diff(SAME_BASE, si, dx, ax_sub);
        k = d.k;
        if (!d.lt) k = (s16)(k - len);
        if (k < -3) break;
        if (bp == 0) {
            if (DSC(DS_pass_mode) < 0) break;
            if (DSB(DS_force_meet) == 0) {
                s16 x = DSS(SAME_BASE + si + CAR_X);
                s16 di = (s16)(di_x - 0xB4);
                if (x < di) break;
                di = (s16)(di + 0x168);
                if (x > di) break;
            }
        }
        dx = wrap_down((u16)(dx - 3));
        car_set(SAME_BASE, si, ax_sub, dx);
        bp++;
    }
    si = cx;
    if (bp == 0 && !((s16)(k + len) > 1)) {             /* the car behind is the next one again */
        if (si == 0) si = DSW(DS_same_count8);
        si = (u16)(si - 8);
    }
    *si_idx = si;
}

/* ---------------------------------------------------------------------------------------------- */
/* 06c9:5a25 pass_check — driver blocks {sub, pos, x, speed} */
bool pass_check(u16 si_a, u16 di_b, s16 bx_old, s16 dx_ax, u16 *cx)
{
    if (DSW(si_a + 2) == 0) return false;
    if (DSW(di_b + 2) == 0) return false;
    SimDiff d = sim_diff(DSW(di_b + 2), DSW(di_b), DSW(si_a + 2), DSW(si_a));
    u16 c = (u16)(d.k - 1);                             /* >= 0: b is at least one unit ahead */
    *cx = c;
    if ((s16)((u16)bx_old ^ c) >= 0) return false;
    s16 ax = (s16)(dx_ax + 0xB4);
    if (ax < DSS(di_b + 4)) return false;
    ax = (s16)(ax - 0x168);
    if (ax > DSS(di_b + 4)) return false;               /* passed side by side */
    u16 p = (u16)(DSW(si_a + 2) + 1);
    c = 1;
    if (bx_old < 0) {
        p = (u16)(p - 2);
        c = 0xFFFF;
    }
    DSW(di_b + 2) = p;
    DSW(di_b) = DSW(si_a);
    *cx = c;
    return true;
}

/* 06c9:595f pass_collisions — simulation.md §4.16 */
void pass_collisions(void)
{
    /* CX on entry: traffic()'s last meet_check leaves CX = same_count8 on every path (simulation.md Q6;
     * it is only stored back when a position is 0, which the state handling never produces). */
    u16 cx = DSW(DS_same_count8);
    s16 bx = DSS(DS_rel_player_opp);
    s16 dx = DSS(DS_car_centre);
    if (DSB(DS_opp_enabled) != 0) {
        bool hit = pass_check(DRV_PLAYER, DRV_OPP, bx, dx, &cx);
        DSW(DS_rel_player_opp) = cx;
        if (hit) {
            if (bx >= 0) {                              /* you hit it from behind */
                DSB(DS_run_state) = 2;
                DSB(DS_opp_crash_timer) = 10;
                DSW(DS_opp_crashes)++;
            } else {                                    /* it hit you */
                DSW(DS_opp_speed) = DSW(DS_speed);
            }
            return;
        }
    }
    if (DSB(DS_cop_active) == 0) return;
    bx = DSS(DS_rel_player_cop);
    bool hit = pass_check(DRV_PLAYER, DRV_COP, bx, dx, &cx);
    DSW(DS_rel_player_cop) = cx;
    if (hit) {
        if ((s16)cx >= 0) {                             /* rammed the police */
            DSW(DS_lives) = 1;
            DSB(DS_run_state) = 2;
            return;
        }
        DSW(DS_cop_pos) = (u16)(DSW(DS_cop_pos) - 2);   /* the police hit you */
        DSW(DS_cop_speed) = DSW(DS_speed);
        DSB(DS_cop_state) = 1;
        DSB(DS_cop_active) = 1;
        DSB(DS_cop_force_meet) = 0;
        return;
    }
    dx = DSS(DS_opp_x);
    bx = DSS(DS_rel_opp_cop);
    if (DSB(DS_opp_enabled) == 0) return;
    hit = pass_check(DRV_OPP, DRV_COP, bx, dx, &cx);
    DSW(DS_rel_opp_cop) = cx;
    if (!hit) return;
    if ((s16)cx >= 0) {
        DSB(DS_opp_crash_timer) = 30;
        DSW(DS_opp_crashes)++;
        return;
    }
    DSW(DS_cop_pos) = (u16)(DSW(DS_cop_pos) - 2);
    DSW(DS_cop_speed) = (u16)(DSW(DS_opp_speed) + 0x500);
    DSB(DS_cop_vs_opp) = 1;
    DSB(DS_cop_active) = 1;
    DSB(DS_cop_force_meet) = 0;
}

/* ---------------------------------------------------------------------------------------------- */
/* 06c9:5ada traffic_resync — simulation.md §4.14 */
void traffic_resync(void)
{
    resync_list(DSW(DS_same_next), SAME_BASE, DSW(DS_same_count8));
    resync_list(DSW(DS_onc_next), ONC_BASE, DSW(DS_oncoming_count8));
    u16 ax = DSW(DS_player_sub), dx = DSW(DS_player_pos);
    DSW(DS_same_next) = (u16)(nearest_ahead(SAME_BASE, DSW(DS_same_count8), ax, dx) - SAME_BASE);
    DSW(DS_onc_next) = (u16)(nearest_ahead(ONC_BASE, DSW(DS_oncoming_count8), ax, dx) - ONC_BASE);
    if (DSB(DS_opp_enabled) == 0) return;
    ax = DSW(DS_opp_sub);
    dx = DSW(DS_opp_pos);
    DSW(DS_opp_same_next) = (u16)(nearest_ahead(SAME_BASE, DSW(DS_same_count8), ax, dx) - SAME_BASE);
    DSW(DS_opp_onc_next) = (u16)(nearest_ahead(ONC_BASE, DSW(DS_oncoming_count8), ax, dx) - ONC_BASE);
}

/* 06c9:5b5a resync_list: clears the list around the player */
void resync_list(u16 si_idx, u16 bp_list, u16 cx_n8)
{
    u16 len = DSW(DS_stage_len);
    u16 ax = DSW(DS_player_sub), dx = DSW(DS_player_pos);
    u16 di = si_idx, si = si_idx;
    SimDiff d = car_diff(bp_list, si, dx, ax);
    s16 k = d.k;
    if (d.lt) k = (s16)(k + len);
    if (!((u16)k > 60)) {                               /* cars within 60 ahead go to +61, +64, ... */
        dx = wrap_up((u16)(dx + 61));
        for (;;) {
            car_set(bp_list, si, ax, dx);
            dx = wrap_up((u16)(dx + 3));
            si = (u16)(si + 8);
            if (si == cx_n8) si = 0;
            if (si == di) break;
            d = car_diff(bp_list, si, dx, ax);
            k = d.k;
            if (!diff_le(d)) k = (s16)(k - len);
            if (k < -62) break;
        }
    }
    ax = DSW(DS_player_sub);
    dx = DSW(DS_player_pos);
    si = di;
    if (si == 0) si = cx_n8;
    si = (u16)(si - 8);
    d = car_diff(bp_list, si, dx, ax);
    k = d.k;
    if (!diff_le(d)) k = (s16)(k - len);
    if (k < -10) return;
    dx = wrap_down((u16)(dx - 10));                     /* cars within 10 behind go to -10, -13, ... */
    for (;;) {
        car_set(bp_list, si, ax, dx);
        dx = wrap_down((u16)(dx - 3));
        if (si == 0) si = cx_n8;
        si = (u16)(si - 8);
        if (si == di) break;
        d = car_diff(bp_list, si, dx, ax);
        k = d.k;
        if (d.lt) k = (s16)(k + len);
        if (k > 30) break;
    }
}

/* 06c9:5c30 nearest_ahead: SI = address of the entry with the smallest (DIFF mod LEN), last one on ties */
u16 nearest_ahead(u16 si_list, u16 cx_n8, u16 ax_sub, u16 dx_pos)
{
    u16 len = DSW(DS_stage_len);
    u32 n = (u16)(cx_n8 >> 3);
    if (n == 0) n = 0x10000;                            /* `loop` with CX = 0 (simulation.md Q11) */
    u16 bp = len;
    u16 di = si_list;                                   /* PORT: DI is not initialised by the original */
    u16 si = si_list;
    while (n--) {
        SimDiff d = sim_diff(DSW((u16)(si + CAR_POS)), DSW((u16)(si + CAR_SUB)), dx_pos, ax_sub);
        u16 k = (u16)d.k;
        if (d.lt) k = (u16)(k + len);
        if (!(k > bp)) {
            di = si;
            bp = k;
        }
        si = (u16)(si + 8);
    }
    return di;
}
