/* Driving simulation: the computer opponent, the lane-choosing AI helpers, the police car and the demo
 * autopilot — port/spec/simulation.md §4.15, §4.17, §4.18 (06c9:50a1..06c9:595e, 06c9:5c5a..06c9:5c97),
 * checked against work/sim/rdis.txt. */
#define SIM_INTERNAL
#include "sim.h"

#include "../platform/res.h"

#define LANE_GAP(i) DSW(DS_lane_gap + 2 * (i))

/* x slews toward target by rate (the tails of 50a1 and 56ed) */
static s16 slew(s16 x, s16 target, u16 rate)
{
    if (x == target) return x;
    if (x > target) {
        x = (s16)(x - rate);
        if (!(x > target)) x = target;
    } else {
        x = (s16)(x + rate);
        if (!(x < target)) x = target;
    }
    return x;
}

/* speed step of an AI car: sub low byte += 3 * mph, pos += carry (no wrap) */
static void ai_advance(u16 drv, u16 v)
{
    DSW(drv + 6) = v;
    u16 st = (u16)((v >> 8) * 3 + DSW(drv));
    DSB(drv) = (u8)st;                                  /* only the low byte of sub is written */
    DSW(drv + 2) = (u16)(DSW(drv + 2) + (u8)(st >> 8));
}

/* brake step: v -= (vmax >> 8) * 4, floor 0; returns true when it hit 0 */
static bool ai_brake(u16 *v, u16 vmax)
{
    u16 b = (u16)((vmax >> 8) << 2);
    if (*v < b) {
        *v = 0;
        return true;
    }
    *v = (u16)(*v - b);
    return false;
}

/* acceleration table entry for speed v: index (v >> 11) & 0xFE as a byte offset */
static u16 accel_entry(u16 table, u16 v, u16 mult)
{
    u16 bx = (u8)(((v >> 3) >> 8) & 0xFE);
    return (u16)(((u32)DSW(table + bx) * mult) >> 16) >> 1;
}

/* ---------------------------------------------------------------------------------------------- */
/* 06c9:50a1 opponent_ai — simulation.md §4.15 */
void opponent_ai(void)
{
    LANE_GAP(1) = 100;
    if (DSB(DS_opp_enabled) == 0) {
        DSW(DS_opp_pos) = 0;
        return;
    }
    if (DSB(DS_opp_crash_timer) != 0) {
        if (--DSB(DS_opp_crash_timer) != 0) return;
        SimDiff d = sim_diff(DSW(DS_opp_pos), DSW(DS_opp_sub), DSW(DS_player_pos), DSW(DS_player_sub));
        /* restart in the left lane when next to the player */
        DSS(DS_opp_x) = (d.k >= 4 || d.k <= -4) ? 200 : -200;
        DSW(DS_opp_speed) = 0;
        return;
    }
    u16 pos = DSW(DS_opp_pos);
    u16 mph = DSB(DS_opp_speed_hi);
    DSB(DS_ai_road_byte) = ai_road_scan(pos, DSW(DS_opp_curve_factor));
    DSB(DS_opp_at_finish) = at_finish(DSW(DS_opp_pos), (u16)(DSB(DS_opp_vmax + 1) << 2), DSW(DS_opp_speed));
    u16 di = DSW(DS_opp_onc_next);
    LANE_GAP(0) = ai_gap(pos, DSW(ONC_BASE + CAR_POS + di), mph, 0xFFCE);
    di = DSW(DS_opp_same_next);
    u16 ax = ai_gap(pos, DSW(SAME_BASE + CAR_POS + di), mph, 50);
    if (DSB(DS_opp_at_finish) == 0) ai_lane_min(ax, DSS(SAME_BASE + CAR_X + di));
    ax = ai_gap(pos, DSW(DS_player_pos), mph, DSB(DS_speed_hi));
    if (DSB(DS_opp_at_finish) == 0) ai_lane_min(ax, DSS(DS_car_centre));
    if (DSB(DS_cop_vs_opp) != 0 || DSB(DS_cop_state) != 0) {
        ax = ai_gap(pos, DSW(DS_cop_pos), mph, DSB(DS_cop_speed_hi));
        if (DSB(DS_opp_at_finish) == 0) ai_lane_min(ax, DSS(DS_cop_x));
    }
    u16 gap;
    s16 target = ai_pick_lane(DSS(DS_opp_x), &gap);
    u8 bl = DSB(DS_cop_vs_opp);
    if (bl >= 2) target = 200;
    if (DSB(DS_opp_at_finish) != 0) target = 1000;
    u16 v = DSW(DS_opp_speed), vmax = DSW(DS_opp_vmax);
    if (bl >= 2 || v >= DSW(DS_curve_speed_limit) || gap < DSW(DS_opp_min_gap)) {
        DSB(DS_opp_braking) = 1;
        if (ai_brake(&v, vmax) && DSB(DS_opp_at_finish) != 0) DSB(DS_opp_finished) = 1;
    } else {
        DSB(DS_opp_braking) = 0;
        u16 a = accel_entry(DS_opp_accel, v, DSW(DS_opp_accel_mult));
        u16 dr = DSB(DS_DRAG + ((v >> 8) >> 2));
        a = (a < dr) ? 0 : (u16)(a - dr);
        v = (u16)(v + a);
        if (v >= vmax) v = vmax;
    }
    ai_advance(DRV_OPP, v);
    DSB(DS_opp_pass_mode) = 0;
    DSS(DS_opp_x) = slew(DSS(DS_opp_x), target, DSW(DS_opp_lat_rate));
}

/* 06c9:5299 ai_road_scan: lane availability and the curve speed limit of the next 40 units */
u8 ai_road_scan(u16 bx_pos, u16 di_factor)
{
    u8 b = ROAD(bx_pos);
    LANE_GAP(1) = 100;
    LANE_GAP(2) = ((s8)b < 0) ? 100 : 0;                /* outer lane only on wide road */
    u8 dl = 0;
    u16 bx = bx_pos;
    for (int i = 0; i < 4; i++) {
        u8 c = DSB(REC(ROAD(bx)) + 1);
        bx = (u16)(bx + 10);
        /* faithful-quirk (simulation.md Q5): the `jl` before `neg al` tests the flags of the preceding
         * `shl ax, 1`, which never says "less", so every curve is negated before the unsigned max. */
        c = (u8)-c;
        if (!(dl > c)) dl = c;
    }
    if ((s8)dl < 0) dl = (u8)-dl;
    u16 ax = DSW(DS_CURVE_SPEED + ((u16)dl << 1));
    DSW(DS_curve_speed_limit) = (u16)((u8)ax * (u8)di_factor);
    return b;
}

/* 06c9:52fd ai_gap: time-to-reach score, 100 = free */
u16 ai_gap(u16 dx_self, u16 cx_other, u16 ax_self_mph, u16 bx_other_mph)
{
    u16 d = (u16)(cx_other - dx_self);
    if ((s16)cx_other <= (s16)dx_self) return ((s16)d < -2) ? 100 : 90;
    if (d > 60) return 100;
    if (d < 3) return 1;
    if ((s16)ax_self_mph < (s16)bx_other_mph) return 100;
    u16 cl = (u16)(ax_self_mph - bx_other_mph);
    u16 cx = (u16)(((d >> 8) | (d << 8)) & 0xFFFF) >> 1;   /* xchg ch, cl; shr cx, 1 */
    cl = (u16)(cl + 5);
    u8 c = (cl >> 8) ? 0xFF : (u8)cl;
    if ((u8)(cx >> 8) >= c) return 100;
    return div16_8(cx, c) & 0xFF;
}

/* 06c9:5334 ai_lane_min */
void ai_lane_min(u16 ax_gap, s16 bx_x)
{
    int s;
    if (bx_x < -400) return;
    if (bx_x < 0) s = 0;
    else if (bx_x < 400) s = 1;
    else if (!(bx_x > 800)) s = 2;
    else return;
    if ((s16)ax_gap < (s16)LANE_GAP(s)) LANE_GAP(s) = ax_gap;
}

/* 06c9:535b ai_pick_lane -> CX target x, DX gap */
s16 ai_pick_lane(s16 ax_own_x, u16 *dx_gap)
{
    u16 dx = LANE_GAP(0);
    s16 cx = -200;
    if (!((s16)dx > (s16)LANE_GAP(1))) {
        dx = LANE_GAP(1);
        cx = 200;
    }
    if ((DSB(DS_ai_road_byte) & 0x80) && (s16)dx < (s16)LANE_GAP(2)) cx = 600;
    if (ax_own_x < -40) dx = LANE_GAP(0);
    else if (!(ax_own_x > 40)) dx = 100;
    else if (ax_own_x > 440) dx = LANE_GAP(2);
    else if (ax_own_x < 360) dx = 100;
    else dx = LANE_GAP(1);
    *dx_gap = dx;
    return cx;
}

/* 06c9:5c5a at_finish: braking distance to 15 units before the finish */
u8 at_finish(u16 bx_pos, u16 di_decel, u16 si_speed)
{
    u16 fin = (u16)(DSW(DS_finish_unit) + 0x3B42);
    if (bx_pos < fin) {
        u16 v2 = (u16)(((u32)si_speed * si_speed) >> 16);
        u16 dist = div32_16((u32)v2 * 3, di_decel, NULL) >> 1;
        if ((u16)(fin - bx_pos) > dist) return 0;
    }
    DSW(DS_curve_speed_limit) = 0;
    return 1;
}

/* ---------------------------------------------------------------------------------------------- */
/* 06c9:53a6 police — simulation.md §4.17 */
void police(void)
{
    DSB(DS_radar_beep) = 0;
    DSB(DS_siren_on) = 0;
    DSW(DS_radar_level) = 0;
    DSB(DS_cop_active) = (u8)(DSB(DS_cop_vs_opp) | DSB(DS_cop_state));
    if (DSW(DS_cop_pos) == 0) return;
    if (DSB(DS_cop_vs_opp) != 0) { police_chase_opp(); return; }
    if (DSB(DS_cop_state) != 0) { police_chase_player(); return; }
    /* parked police car ahead: radar trap */
    s16 ax = sim_diff(DSW(DS_cop_pos), DSW(DS_cop_sub), DSW(DS_player_pos), DSW(DS_player_sub)).k;
    if (ax < 0) { police_reset(); return; }             /* passed without being caught */
    if (DSW(DS_tick_count10) & 4) {
        DSB(DS_radar_beep) = 1;
        u16 bx = (u16)(((u16)ax >> 4) + 1);
        if (!((s16)bx < 5)) bx = 5;
        DSW(DS_radar_level) = bx;
    }
    if (ax > 16) return;
    if (DSW(DS_speed) < DSW(DS_opp_speed)) {            /* the opponent is the faster one */
        if (DSB(DS_opp_speed_hi) <= 0x32) return;
        DSB(DS_cop_vs_opp) = 1;
        police_start();
        return;
    }
    if (DSB(DS_speed_hi) <= 0x32) return;
    DSW(DS_chase_speed) = DSW(DS_speed);
    DSB(DS_cop_state) = 1;
    police_start();
}

/* 06c9:5427 police_reset */
void police_reset(void)
{
    DSW(DS_cop_pos) = 0;
    DSB(DS_cop_vs_opp) = 0;
    DSB(DS_cop_state) = 0;
    DSB(DS_cop_active) = 0;
    DSB(DS_cop_force_meet) = 0;
}

/* 06c9:5446 police_start: the police car appears 26 units behind the player */
void police_start(void)
{
    u16 ax = DSW(DS_player_sub), dx = DSW(DS_player_pos);
    DSS(DS_rel_player_cop) = -100;
    DSB(DS_cop_active) = 1;
    u16 bp = DSW(DS_same_next), di = DSW(DS_onc_next);
    dx = (u16)(dx - 26);                                /* no wrap */
    DSW(DS_rel_opp_cop) = (u16)-(u16)sim_diff(DSW(DS_opp_pos), DSW(DS_opp_sub), dx, ax).k;

    u16 n = DSW(DS_same_count8), si = bp;
    for (;;) {
        if (si == 0) si = n;
        si = (u16)(si - 8);
        if (si == bp) break;
        SimDiff d = sim_diff(DSW(SAME_BASE + CAR_POS + si), DSW(SAME_BASE + CAR_SUB + si), dx, ax);
        if (d.lt) break;
        if (!(d.k < 26)) break;
    }
    si = (u16)(si + 8);
    if (si == n) si = 0;
    DSW(DS_cop_same_next) = si;

    n = DSW(DS_oncoming_count8);
    si = di;
    for (;;) {
        if (si == 0) si = n;
        si = (u16)(si - 8);
        if (si == di) break;
        SimDiff d = sim_diff(DSW(ONC_BASE + CAR_POS + si), DSW(ONC_BASE + CAR_SUB + si), dx, ax);
        if (d.lt) break;
        if (!(d.k < 26)) break;
    }
    si = (u16)(si + 8);
    if (si == n) si = 0;
    DSW(DS_cop_onc_next) = si;

    DSW(DS_cop_speed) = 0x3200;
    DSW(DS_cop_x) = 200;
    DSW(DS_cop_sub) = ax;
    DSW(DS_cop_pos) = dx;
    DSB(DS_cop_force_meet) = 0;
}

static s16 cop_distance(u16 drv)                        /* DIFF(cop, drv), + LEN when negative */
{
    SimDiff d = sim_diff(DSW(DS_cop_pos), DSW(DS_cop_sub), DSW(drv + 2), DSW(drv));
    s16 ax = d.k;
    if (d.lt) ax = (s16)(ax + DSW(DS_stage_len));
    return ax;
}

/* 06c9:54f4 police_chase_opp: state machine DS:33B6 through table DS:3456 */
void police_chase_opp(void)
{
    if (police_drive()) return;
    s16 ax = cop_distance(DRV_OPP);
    u16 bx = (u16)((u16)(DSB(DS_cop_vs_opp) - 1) << 1);
    u16 target = DSW((u16)(DS_COP_OPP_JT + bx));
    switch (target) {
    case 0x5518:                                        /* 1: catching up */
        if (!(ax > 300)) {
            DSB(DS_cop_vs_opp)++;
            DSB(DS_cop_force_meet) = 1;
        }
        break;
    case 0x5527:                                        /* 2 */
        if (!(ax < 10)) DSB(DS_cop_vs_opp)++;
        break;
    case 0x5531:                                        /* 3: hold 10 ahead of the opponent */
        if (!(ax < 10)) {
            DSW(DS_cop_sub) = DSW(DS_opp_sub);
            DSW(DS_cop_pos) = (u16)(DSW(DS_opp_pos) + 10);   /* `add si, 8` is dead code */
        }
        if (DSW(DS_cop_speed) != 0) break;
        DSB(DS_cop_vs_opp)++;
        DSB(DS_cop_timer) = 20;
        break;
    case 0x5561:                                        /* 4 */
        if (--DSB(DS_cop_timer) == 0) DSB(DS_cop_vs_opp)++;
        break;
    case 0x556C:                                        /* 5: drive off, no ticket */
        DSW(DS_cop_speed) = (u16)(DSW(DS_cop_speed) + 0x400);
        if (!(ax < 60)) police_reset();
        break;
    default:
        fatal("sim: police vs opponent state %u", DSB(DS_cop_vs_opp));
    }
}

/* 06c9:557a police_chase_player: state machine DS:33B7 through table DS:3446 */
void police_chase_player(void)
{
    if (DSB(DS_cop_state) < 6) {
        if (police_drive()) return;
    }
    s16 ax = cop_distance(DRV_PLAYER);
    u16 bx = (u16)((u16)(DSB(DS_cop_state) - 1) << 1);
    u16 target = DSW((u16)(DS_COP_STATE_JT + bx));
    switch (target) {
    case 0x55A5:                                        /* 1: the police car catches up */
        if (!(ax > 300)) {
            DSB(DS_cop_state)++;
            DSB(DS_cop_force_meet) = 1;
        }
        break;
    case 0x55B4:                                        /* 2 */
        if (!(ax < 10)) DSB(DS_cop_state)++;
        break;
    case 0x55BE:                                        /* 3: hold 10 ahead */
        if (!(ax < 10)) {
            DSW(DS_cop_sub) = DSW(DS_player_sub);
            DSW(DS_cop_pos) = (u16)(DSW(DS_player_pos) + 10);   /* no wrap; `add si, 8` is dead code */
            if (DSW(DS_speed) == 0) {
                DSW(DS_cop_speed) = 0;
                goto stop;
            }
        }
        if (DSW(DS_cop_speed) != 0) break;
    stop:
        DSW(DS_speed) = 0;                              /* both stopped */
        DSB(DS_cop_state)++;
        DSB(DS_cop_timer) = 20;
        break;
    case 0x5603:                                        /* 4: 2 s */
        if (--DSB(DS_cop_timer) == 0) DSB(DS_cop_state)++;
        break;
    case 0x560E:                                        /* 5: drive off, ticket */
        DSW(DS_cop_speed) = (u16)(DSW(DS_cop_speed) + 0x400);
        if (!(ax < 60)) {
            DSW(DS_tickets)++;
            police_reset();
        }
        break;
    case 0x5620:                                        /* 6: at the roadblock */
        if (--DSB(DS_cop_timer) != 0) break;
        DSB(DS_cop_state)--;
        DSW(DS_cop_same_next) = DSW(DS_same_next);
        DSW(DS_cop_onc_next) = DSW(DS_onc_next);
        break;
    case 0x5637:                                        /* 7: rolling up to the roadblock at 30 mph */
        if (DSW(DS_tick_count10) & 4) {
            DSB(DS_radar_beep) = 1;
            DSW(DS_radar_level) = 1;
        }
        DSB(DS_speed_hi) = 0x1E;
        update_rpm(NULL, NULL);
        DSB(DS_gauges_dirty) = 1;
        if (ax > 2) break;
        DSB(DS_cop_timer) = 30;
        DSB(DS_cop_state)--;
        DSW(DS_speed) = 0;
        DSW(DS_rpm) = 800;
        DSB(DS_gauges_dirty) = 1;
        break;
    case 0x5679:                                        /* 8: roadblock ahead */
        if (DSW(DS_tick_count10) & 4) {
            DSB(DS_radar_beep) = 1;
            DSW(DS_radar_level) = 1;
        }
        if (!(ax > 300) && ax > 2) {
            if (DSW(DS_speed) < 0x1E00) DSB(DS_cop_state)--;   /* slowed below 30 mph */
            break;
        }
        {
            s16 c = (s16)(DSS(DS_car_centre) + 0xB4);
            s16 cx = DSS(DS_cop_x);
            if (!(c < cx) && !((s16)(c - 0x168) > cx)) {    /* 56a3: hit the roadblock */
                DSW(DS_cop_sub) = DSW(DS_player_sub);
                DSW(DS_cop_pos) = wrap_up((u16)(DSW(DS_player_pos) + 2));
                DSW(DS_lives) = 1;
                DSB(DS_run_state) = 2;
                break;
            }
        }
        DSB(DS_cop_state) = 1;                          /* 56df: drove past */
        DSW(DS_chase_speed) = DSW(DS_speed);
        police_start();
        break;
    default:
        fatal("sim: police state %u", DSB(DS_cop_state));
    }
}

/* 06c9:56ed police_drive */
bool police_drive(void)
{
    DSB(DS_siren_on) = 1;
    u16 pos = DSW(DS_cop_pos);
    u16 mph = DSB(DS_cop_speed_hi);
    DSB(DS_ai_road_byte) = ai_road_scan(pos, DSW(DS_cop_curve_factor));
    u16 di = DSW(DS_cop_onc_next);
    LANE_GAP(0) = ai_gap(pos, DSW(ONC_BASE + CAR_POS + di), mph, 0xFFCE);
    di = DSW(DS_cop_same_next);
    u16 ax = ai_gap(pos, DSW(SAME_BASE + CAR_POS + di), mph, 50);
    if (!(DSS(SAME_BASE + CAR_X + di) > 400)) LANE_GAP(1) = ax;     /* stored, not min */
    else LANE_GAP(2) = ax;
    ax = ai_gap(pos, DSW(DS_player_pos), mph, DSB(DS_speed_hi));
    ai_lane_min(ax, DSS(DS_car_centre));
    if (DSB(DS_opp_enabled) != 0) {
        ax = ai_gap(pos, DSW(DS_opp_pos), mph, DSB(DS_opp_speed_hi));
        ai_lane_min(ax, DSS(DS_opp_x));
    }
    u16 gap;
    s16 target = ai_pick_lane(DSS(DS_cop_x), &gap);
    u8 bl = DSB(DS_cop_active);
    if (bl >= 2 && bl != 5) target = 200;
    u16 v = DSW(DS_cop_speed), vmax = DSW(DS_cop_vmax);
    u16 jt = DSW((u16)(DS_COP_DRIVE_JT + (u16)((u16)(bl - 1) << 1)));
    bool br;
    switch (jt) {
    case 0x57AB: br = v >= DSW(DS_curve_speed_limit) || gap < DSW(DS_cop_min_gap); break;   /* 1 */
    case 0x57CF: br = false; break;                                                         /* 2, 5 */
    case 0x57B7: br = true; break;                                                          /* 3, 4 */
    default:
        /* PORT: cop_active outside 1..5 (e.g. a roadblock car, cop_state 8, hit by the opponent)
         * would jump through the bytes after table DS:3460 into arbitrary code. The port brakes. */
        br = true;
        break;
    }
    if (br) {
        DSB(DS_cop_braking) = 1;
        ai_brake(&v, vmax);
    } else {
        DSB(DS_cop_braking) = 0;
        v = (u16)(v + accel_entry(DS_POLICE_ACCEL, v, DSW(DS_cop_accel_mult)));   /* no drag */
        if (v >= vmax) v = vmax;
    }
    ai_advance(DRV_COP, v);
    if (DSB(DS_cop_state) != 0) {
        s16 d = sim_diff(DSW(DS_cop_pos), DSW(DS_cop_sub), DSW(DS_player_pos), DSW(DS_player_sub)).k;
        u16 len = DSW(DS_stage_len);
        if (!(d < 300)) d = (s16)(d - len);
        if (!(d > -300)) d = (s16)(d + len);
        if (d < -100) {                                 /* the police gave up */
            DSB(DS_escaped) = 1;
            police_reset();
            return true;                                /* `pop ax; ret`: leaves police() */
        }
    }
    DSS(DS_cop_x) = slew(DSS(DS_cop_x), target, DSW(DS_cop_lat_rate));
    return false;
}

/* ---------------------------------------------------------------------------------------------- */
/* 06c9:5880 demo_steer — simulation.md §4.18 */
void demo_steer(void)
{
    LANE_GAP(1) = 100;
    u16 pos = DSW(DS_player_pos);
    u16 mph = DSB(DS_speed_hi);
    DSB(DS_ai_road_byte) = ai_road_scan(pos, 0xFF);
    DSB(DS_demo_at_finish) = at_finish(DSW(DS_player_pos), 0x640, DSW(DS_speed));
    u16 di = DSW(DS_onc_next);
    LANE_GAP(0) = ai_gap(DSW(DS_player_pos), DSW(ONC_BASE + CAR_POS + di), mph, 0xFFCE);
    di = DSW(DS_same_next);
    u16 ax = ai_gap(DSW(DS_player_pos), DSW(SAME_BASE + CAR_POS + di), mph, 50);
    if (DSB(DS_demo_at_finish) == 0) ai_lane_min(ax, DSS(SAME_BASE + CAR_X + di));
    u16 dx;
    s16 target = ai_pick_lane((s16)(DSS(DS_player_x) + 40), &dx);
    u8 bl = DSB(DS_cop_state);
    if (bl >= 2) target = 200;
    if (DSB(DS_demo_at_finish) != 0) target = 200;
    u16 v = DSW(DS_speed), lim = DSW(DS_curve_speed_limit);
    if (bl >= 2) {
        DSB(DS_throttle) = 0xFF;
    } else if (v < lim) {
        if (dx < 20) DSB(DS_throttle) = 0xFF;
    } else if (dx < 20 || (u16)(v - lim) >= 0x640 || lim == 0) {
        DSB(DS_throttle) = 0xFF;
    } else {
        DSB(DS_throttle) = 0;
    }
    s16 c = slew((s16)(DSS(DS_player_x) + 40), target, 44);   /* the centre moves by 44 per tick */
    DSS(DS_player_x) = (s16)(c - 40);
}
