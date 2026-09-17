/* Driving simulation: stage / life resets, the timer entry, sound tones, input, gearbox, engine,
 * steering (06b3:0008) and the fall animation — port/spec/simulation.md §4.1–§4.8, §4.12, §4.19, §4.21.
 * Checked against the disassembly (work/sim/rdis.txt). All state is in mem[]. */
#define SIM_INTERNAL
#include "sim.h"

#include "../codeptr.h"
#include "../platform/input.h"
#include "../platform/res.h"
#include "../platform/timer.h"

/* ---------------------------------------------------------------------------------------------- */
/* 06c9:3f40 sim_stage_start — simulation.md §4.19 */
void sim_stage_start(void)
{
    for (u16 i = 0; i < 8; i++) DSB(DS_curve_ring + i) = 0;
    DSW(DS_heading) = 0;
    DSW(DS_cloud_scroll) = 0;
    DSW(DS_road_curve) = 0;
    DSW(DS_clock_seconds) = 0;
    DSW(DS_opp_crashes) = 0;
    DSW(DS_opp_time) = 0;
    DSW(DS_race_time) = 0;
    DSB(DS_clock_started) = 0;
    DSB(DS_station_zone) = 0;
    DSW(DS_ring_counter) = 0;
    DSW(DS_onc_next) = 0;
    DSW(DS_same_next) = 0;
    DSW(DS_opp_onc_next) = 0;
    DSW(DS_opp_same_next) = 0;
    DSW(DS_idle_ticks) = 0;
    DSB(DS_cop_vs_opp) = 0;
    DSB(DS_cop_state) = 0;
    DSB(DS_radar_beep) = 0;
    DSB(DS_siren_on) = 0;
    DSB(DS_opp_crash_timer) = 0;
    DSW(DS_cop_sub) = 0;
    DSW(DS_cop_pos) = 0;
    DSW(DS_opp_speed) = 0;
    DSB(DS_demo_at_finish) = 0;
    DSB(DS_opp_at_finish) = 0;
    DSB(DS_opp_finished) = 0;
    DSB(DS_finished) = 0;
    if (DSB(DS_auto_trans) != 0) DSB(DS_gate_mode) = 0;
    DSB(DS_opp_enabled) = DSB(DS_opponent_option);
    DSB(DS_scenery_density) = 0x20;

    /* region flags 70 units ahead: XOR of the record flags of the first 0x47 road bytes */
    u8 al = 0;
    for (u16 i = 0; i < 0x47; i++) al ^= DSB(REC(ROAD(DS_ROAD0 + i)));
    DSB(DS_lookahead_flags) = al;
    DSW(DS_stage_end) = (u16)(DSW(DS_stage_end) + 0x3B33);

    codeptr_register(FN_sim_timer_routine, sim_timer_routine);
    timer_install_drive();                              /* 06c9:601c clears the routine list */
    timer_add_routine(codeptr_far(FN_sfx_tick));
    timer_add_routine(codeptr_far(FN_sim_timer_routine));
}

/* 06c9:3ff9 sim_restart_reset — simulation.md §4.19 */
void sim_restart_reset(void)
{
    DSB(DS_knob_anim) = 0;
    DSB(DS_shift_latch) = 0;
    DSW(DS_speed) = 0;
    DSW(DS_rpm) = 0;
    DSW(DS_rpm_display) = 0;
    DSB(DS_clock_started) = 0;
    DSB(DS_grind_timer) = 0;
    DSB(DS_damage_hits) = 0;
    DSB(DS_overrev_ticks) = 0;
    for (u16 i = 0; i < 7; i++) DSB(DS_gear_broken + i) = 0;
    DSB(DS_engine_damage) = 0;
    DSW(DS_suspension_damage) = 0;
    DSW(DS_alignment) = 0;
    DSW(DS_steering_health) = 250;
    DSB(DS_run_state) = 0;
    DSB(DS_fall_mode) = 0;
    police_reset();
}

/* ---------------------------------------------------------------------------------------------- */
/* 06c9:403b sim_timer_routine — simulation.md §4.1. Timer routine (every PIT tick, 99.9985 Hz). */
void sim_timer_routine(void)
{
    sound_tick();
    if (--DSB(DS_div_100hz) != 0) return;
    DSB(DS_div_100hz) = 10;
    DSW(DS_tick_count10)++;
    if (DSB(DS_run_state) >= 2) return;                 /* unsigned: also 0xFF (quit) */
    if (DSB(DS_clock_started) != 0) DSW(DS_race_time)++;
    if (DSB(DS_finished) != 0) DSW(DS_race_time) = DSW(DS_final_time);
    if (DSB(DS_opp_finished) == 0 && DSB(DS_opp_crash_timer) == 0 && DSW(DS_race_time) != 0)
        DSW(DS_opp_time)++;
    if (--DSB(DS_div_10hz) == 0) {                      /* 1 Hz */
        DSB(DS_div_10hz) = 10;
        DSB(DS_clock_dirty) = 10;
        DSW(DS_cloud_scroll)++;
        if (DSB(DS_finished) == 0) {
            DSW(DS_clock_seconds)++;
            if (DSW(DS_clock_seconds) >= 1800) {        /* 30 minutes: game over */
                DSW(DS_lives) = 1;
                DSB(DS_run_state) = 2;
            }
            if (DSB(DS_clock_started) == 0) {
                DSB(DS_div_10hz) = 10;
                DSW(DS_clock_seconds)--;
            }
        }
    }
    DSB(DS_throttle) = 0;
    DSB(DS_steer_in) = 0;
    read_input();
    if (DSB(DS_fall_mode) == 0) {
        DSW(DS_speed_delta) = DSW(DS_speed);
        if (DSW(DS_demo_mode) == 1) demo_steer();
        engine();
        u16 d = (u16)(DSW(DS_speed) - DSW(DS_speed_delta));
        if (DSW(DS_speed) < DSW(DS_speed_delta)) d = (u16)-d;
        DSW(DS_speed_delta) = d;
        overrev_check();
        motion();
        if (DSW(DS_clock_seconds) == 0) return;         /* nothing else moves before the clock runs */
        opponent_ai();
        police();
        traffic();
        pass_collisions();
        if (DSB(DS_fall_mode) == 0) return;
    }
    fall_anim();
}

/* 06c9:412c sound_tick — simulation.md §4.2 */
void sound_tick(void)
{
    DSB(DS_sound_frame)++;
    DSW(DS_tone_rumble) = DSW(DS_RUMBLE + ((DSB(DS_sound_frame) & 0x1F) << 1));
    s16 r = DSS(DS_rpm_display), t = DSS(DS_rpm);
    if (r <= t) {
        r = (s16)(r + 0x60);
        if (r >= t) r = t;
    } else {
        r = (s16)(r - 0x60);
        if (r <= t) r = t;
    }
    DSS(DS_rpm_display) = r;
    DSW(DS_tone_engine) = DSW(DS_ENGINE_NOTE + (((u16)r >> 5) & 0xFFFE));
    if (DSB(DS_siren_on) != 0) {
        if (DSW(DS_clock_seconds) & 8) {                /* wail */
            u16 p = DSW(DS_siren_pitch);
            if (DSC(DS_siren_dir) >= 0) {
                p = (u16)(p + 2);
                if ((s16)p >= 0x708) DSB(DS_siren_dir) = 0xFF;
            } else {
                p = (u16)(p - 2);
                if (!((s16)p > 0x5DC)) DSB(DS_siren_dir) = 1;
            }
            DSW(DS_siren_pitch) = p;
            DSW(DS_tone_effect) = p;
        } else {                                        /* hi-lo */
            DSW(DS_tone_effect) = (DSW(DS_tick_count10) & 4) ? 0x5DC : 0x7D0;
        }
        return;
    }
    if (DSB(DS_radar_beep) != 0) {
        DSW(DS_tone_effect) = 0x384;
        return;
    }
    if (DSB(DS_div_100hz) == 1) {                       /* the call that also runs the 10 Hz tick */
        u8 g = DSB(DS_grind_timer);
        if (g != 0) {
            DSB(DS_grind_timer) = (u8)(g - 1);
            DSW(DS_tone_effect) = 0x474;
        } else {
            DSW(DS_tone_effect) = 0xFFFF;
        }
    }
    if (DSB(DS_skidding) != 0) DSW(DS_tone_effect) = 0x8E8;
}

/* ---------------------------------------------------------------------------------------------- */
/* 06c9:420a read_input — simulation.md §4.3, §4.21 */
void read_input(void)
{
    if (DSB(DS_fall_mode) == 0) {
        u16 bits = input_drive_bits();                  /* 06c9:6620 */
        DSB(DS_input_bits) = (u8)bits;
        u16 ax = joy_dir_index(bits);                   /* 06c9:6828 */
        bits &= 0x30;
        if (DSB(DS_auto_trans) == 0) ax |= bits;        /* no manual shifting with the automatic */
        DSB(DS_input) = (u8)ax;
        DSW(DS_idle_ticks)++;
        if (DSW(DS_idle_ticks) > 0x960) {               /* 4 minutes without input: quit */
            DSB(DS_run_state) = 0xFF;
        } else if (ax != 0) {
            DSW(DS_idle_ticks) = 0;
            /* PORT: 06c9:424a is the copy protection's patched branch; the "passed" opcode is JNE, so
             * joystick / keypad input quits only the attract demo (DS:8A9A == 1). */
            if (DSW(DS_demo_mode) == 1) DSB(DS_run_state) = 0xFF;
        }
        decode_controls();
        if (DSB(DS_knob_anim) != 0) shift_knob_anim();
        curve_sum();
        steer_update();                                 /* 06b3:0008 */
    }
    u16 key = getkey_drive();                           /* 06c9:6601 */
    /* PORT: 06c9:4275 patched branch, "passed" = JNE: a BIOS key quits only the demo. */
    if (key != 0 && DSW(DS_demo_mode) == 1) DSB(DS_run_state) = 0xFF;
    hotkeys(key);
}

/* 06c9:4281 decode_controls — simulation.md §4.4 */
void decode_controls(void)
{
    u8 dl = DSB(DS_knob_anim);                          /* anim */
    u8 dh = DSB(DS_fire_held);                          /* held */
    u8 al = DSB(DS_input);
    u16 bp = DSW(DS_rpm);
    u16 bx;

    if (DSB(DS_finished) != 0) {                        /* last stage past the line: brake, steer only */
        bx = al & 0x0F;
        DSB(DS_steer_in) = DSB(DS_STEER_DIR + bx);
        al = 5;
        goto automatic;                                 /* also with the manual gearbox */
    }
    /* PORT: 06c9:42ad is the copy protection's patched branch. The "passed" opcode is JNE: the autopilot
     * drives only the attract demo (DS:8A9A == 1). The shipped JA would drive for every demo_mode <= 1. */
    if (DSW(DS_demo_mode) == 1) {
        DSW(DS_idle_ticks) = 0;
        al = 1;
        DSB(DS_gearbox_timer) = 10;
        if (dl == 0) {
            if (bp > DSW(DS_car_rpm_redline)) {
                al |= 0x10;                             /* upshift: 0x11 */
            } else if (bp > DSW(DS_car_rpm_downshift)) {
                /* keep */
            } else if (DSB(DS_gear) > 1) {
                al = 5 | 0x10;                          /* downshift: 0x15 */
            }
        }
    }
    bx = al & 0x0F;
    DSB(DS_steer_in) = DSB(DS_STEER_DIR + bx);
    {
        s8 cl = DSC(DS_THROTTLE_DIR + bx);
        if (cl < 0) DSC(DS_throttle) = cl;              /* brake at once */
    }
    if (DSB(DS_auto_trans) == 0) goto fire;
automatic:
    if (dl == 0) {
        if (bp >= DSW(DS_car_rpm_upshift)) {
            if (DSB(DS_gear) != DSB(DS_car_num_gears)) {
                al = 0x11;
                bx = 1;
            }
        } else if (DSB(DS_gear) > 1 && bp <= DSW(DS_car_rpm_downshift)) {
            al = 0x15;
            bx = 5;
        }
    }
fire:
    if (!(al & 0x30) && dh != 0 && dl == 0) {           /* fire released after the knob arrived */
        DSB(DS_fire_held) = dl;                         /* = 0 */
        u16 cx, dx;
        if (update_rpm(&cx, &dx)) {
            clutch_check(cx, dx);
        } else {
            /* PORT: simulation.md Q3. In neutral 06c9:4612 leaves CX / DX unchanged and 4441 compares
             * register leftovers (CX = throttle byte / joystick garbage, DX = 0x0100), which can only
             * start the grind tone. The check is skipped in neutral. */
        }
        return;
    }
    if (!(al & 0x30)) {
        DSB(DS_shift_latch) = 0;
        if (dh == 0) DSB(DS_throttle) = DSB(DS_THROTTLE_DIR + bx);
        return;
    }
    DSB(DS_fire_held) = 1;
    DSB(DS_gearbox_dirty) = 1;
    DSB(DS_gearbox_timer) = 10;
    u8 cl = 0, g;
    if (DSW(DS_demo_mode) != 1 && DSB(DS_gate_mode) != 0 && DSB(DS_run_state) == 0) {   /* gate shifting */
        if ((u8)bx == cl) goto done;
        cl++;
        u8 ch = DSB(DS_car_gate_slot + bx);
        g = DSB(DS_car_gate_gear + bx);
        bx = (u8)(ch + 7);
    } else {                                            /* sequential shifting */
        u8 ah = DSB(DS_GEAR_DELTA + bx);
        if (ah == cl) goto done;
        g = (u8)(DSB(DS_gear) + ah);
        if (g > DSB(DS_car_num_gears)) goto done;       /* unsigned: 0 - 1 fails too */
        cl++;
        if (DSB(DS_shift_latch) == 1) goto done;        /* one shift per press */
        bx = g;
    }
    DSB(DS_gear) = g;                                   /* the gear changes before the knob moves */
    DSW(DS_knob_target_x) = DSW(DS_car_knob_xy + (bx << 2));
    DSW(DS_knob_target_y) = DSW(DS_car_knob_xy + 2 + (bx << 2));
    DSB(DS_knob_anim) = cl;
done:
    DSB(DS_shift_latch) = cl;
}

/* 06c9:43c3 shift_knob_anim — simulation.md §4.5 */
void shift_knob_anim(void)
{
    DSB(DS_gearbox_dirty) = 1;
    DSB(DS_clock_started) = 1;                          /* the clock starts at the first shift */
    s16 x = DSS(DS_knob_x), y = DSS(DS_knob_y);
    s16 tx = DSS(DS_knob_target_x), ty = DSS(DS_knob_target_y);
    s16 ny = DSS(DS_car_knob_xy + 2);                   /* neutral row */
    if (x == tx) {
        if (y < ty) y = (s16)(y + 6);
        else if (y != ty) y = (s16)(y - 6);
    } else if (y == ny) {
        x = (x < tx) ? (s16)(x + 6) : (s16)(x - 6);
    } else {
        y = (y > ny) ? (s16)(y - 6) : (s16)(y + 6);     /* back to the neutral row first */
    }
    DSS(DS_knob_x) = x;
    DSS(DS_knob_y) = y;
    if (x != DSS(DS_knob_target_x) || y != DSS(DS_knob_target_y)) return;
    if (DSB(DS_input) & 0x30) return;                   /* wait for fire release */
    u8 bh = (u8)((u16)y >> 8);                          /* 0 for every knob position */
    DSB(DS_knob_anim) = bh;
    DSB(DS_fire_held) = bh;
    DSB(DS_throttle) = 1;                               /* this tick accelerates */
    u8 gear = DSB(DS_gear);
    if (gear == bh) return;
    DSW(DS_gear_ratio) = DSW(DS_car_gear_ratio + (((u16)bh << 8 | gear) << 1));
    u16 cx, dx;
    if (update_rpm(&cx, &dx)) clutch_check(cx, dx);
}

/* 06c9:4441 clutch_check — simulation.md §4.5 */
void clutch_check(u16 cx_old, u16 dx_new)
{
    u16 ax = (u16)(dx_new - cx_old);
    if (!((s16)cx_old > (s16)dx_new)) {                 /* rpm rose: downshift */
        if (ax < 0xA8C) return;
        if (DSB(DS_gear) == 0) return;
        DSB(DS_grind_timer) = 3;
        DSB(DS_speed_hi) = (u8)(DSB(DS_speed_hi) - 5);
        return;
    }
    ax = (u16)-ax;                                      /* upshift: rpm drop */
    if (ax < 0xBB8) return;
    u16 drop = ax;
    u8 st = DSB(DS_car_engine_strength), dm = DSB(DS_engine_damage);
    u8 e = (st < dm) ? 0 : (u8)(st - dm);
    u16 p = (u16)e * DSB(DS_gear_ratio + 1);
    if (p < 0x1B58) return;
    DSB(DS_grind_timer) = (u8)(drop >> 8);              /* sound only */
}

/* 06c9:448a curve_sum */
void curve_sum(void)
{
    s16 s = 0;
    for (u16 i = 0; i < 8; i++) s = (s16)(s + DSC(DS_curve_ring + i));
    DSS(DS_curve_sum) = s;
    DSW(DS_speed_copy) = DSW(DS_speed);
}

/* 06c9:44a6 hotkeys: repne scasb (std) over DS:33DA..33D6, handler = DS:33DC[remaining count] */
void hotkeys(u16 ax_key)
{
    if (ax_key == 0) return;
    for (int cx = 4; cx >= 0; cx--) {
        if (DSB(DS_HOTKEY_CHARS + cx) != (u8)ax_key) continue;
        switch (DSW(DS_HOTKEY_JT + (cx << 1))) {
        case 0x44BF: key_esc(); break;
        case 0x44C5: key_gate_mode(); break;
        case 0x44D2: key_dash_toggle(); break;
        default: fatal("sim: bad hotkey handler %04X", DSW(DS_HOTKEY_JT + (cx << 1)));
        }
        return;
    }
}

void key_esc(void) { DSB(DS_run_state) = 0xFF; }                                    /* 06c9:44bf */
void key_gate_mode(void) { if (DSB(DS_auto_trans) == 0) DSB(DS_gate_mode) ^= 1; }   /* 06c9:44c5 */
void key_dash_toggle(void) { DSB(DS_dash_toggle) ^= 1; }                            /* 06c9:44d2 */

/* ---------------------------------------------------------------------------------------------- */
void brake_1600(void) { brake(0x640); }                 /* 06c9:44d8 */
void brake_800(void) { brake(0x320); }                  /* 06c9:44de */
void brake_400(void) { brake(0x190); }                  /* 06c9:44e4 */

/* 06c9:44ea engine — simulation.md §4.7 */
void engine(void)
{
    if (DSB(DS_finished) != 0) { brake_1600(); return; }
    s8 t = DSC(DS_throttle);
    if (t < 0) { brake_1600(); return; }
    if (DSB(DS_skidding) != 0) { brake_800(); return; }
    u8 gear = DSB(DS_gear);
    if (DSB(DS_knob_anim) != 0 || DSB(DS_fire_held) != 0 || gear == 0) {   /* clutch in or neutral */
        free_rev();
        if (DSC(DS_throttle) < 0) { brake_1600(); return; }                 /* dead: t >= 0 here */
        drag_apply(0);
        return;
    }
    if (t <= 0) {                                       /* in gear, off throttle: speed held */
        DSB(DS_grind_timer) = 0;
        return;
    }
    if (DSB(DS_gear_broken + gear) != 0) { brake_400(); return; }
    u8 cs = DSB(DS_cop_state);
    if (cs != 8 && cs >= 2) return;                     /* being pulled over */
    DSB(DS_gauges_dirty) = 1;
    u16 i = (u16)(DSW(DS_rpm) << 1) >> 8;
    if (i >= 0x50) i = 0x50;
    u16 f = (u16)((u16)DSB(DS_car_torque + i) * DSB(DS_gear_ratio + 1)) >> 5;
    if (DSB(DS_skidding) != 0) f >>= 1;                 /* dead */
    drag_apply(f);
    update_rpm(NULL, NULL);
}

/* 06c9:4588 free_rev */
void free_rev(void)
{
    u16 ax = DSW(DS_rpm);
    if (DSB(DS_gear) == 0 && DSC(DS_throttle) > 0) {
        DSB(DS_gauges_dirty) = 1;
        DSW(DS_rpm) = (u16)(ax + 600);                  /* no limit here */
        return;
    }
    u16 dx = ax;
    ax = (u16)(ax - 200);
    if (!((s16)ax > 800)) ax = 800;
    if (ax != dx) {
        DSW(DS_rpm) = ax;
        DSB(DS_gauges_dirty) = 1;
    }
}

/* 06c9:45bf drag_apply */
void drag_apply(u16 dx_force)
{
    if (DSB(DS_throttle) == 0xFF) { brake(dx_force); return; }   /* dead in practice */
    u16 ax = DSW(DS_speed);
    u16 cx = DSB(DS_DRAG + (DSB(DS_speed_hi) >> 2));
    if (dx_force == 0) cx <<= 1;
    u16 dx = (u16)(dx_force - cx);
    if (dx == 0) return;
    if ((s16)dx < 0) {
        dx = (u16)-dx;
        if (ax < dx) return;                            /* would go below 0: speed unchanged */
        ax = (u16)(ax - dx);
    } else {
        ax = (u16)(ax + dx);                            /* wraps at 0xFFFF */
    }
    DSW(DS_speed) = ax;
    DSB(DS_gauges_dirty) = 1;
}

/* 06c9:45fc brake */
void brake(u16 dx_amount)
{
    u16 v = DSW(DS_speed);
    u16 ax = (v < dx_amount) ? 0 : (u16)(v - dx_amount);
    if (ax == v) return;
    DSW(DS_speed) = ax;
    update_rpm(NULL, NULL);
}

/* 06c9:4612 update_rpm */
bool update_rpm(u16 *cx_old, u16 *dx_new)
{
    if (DSB(DS_gear) == 0) return false;
    if (cx_old) *cx_old = DSW(DS_rpm);
    DSB(DS_gauges_dirty) = 1;
    u16 dx = (u16)(((u32)DSW(DS_gear_ratio) * DSW(DS_speed)) >> 16);
    if (dx < 800) dx = 800;
    DSW(DS_rpm) = dx;
    if (dx_new) *dx_new = dx;
    return true;
}

/* 06c9:4639 overrev_check — simulation.md §4.8 */
void overrev_check(void)
{
    u8 bl = DSB(DS_overrev_ticks);
    u16 ax = DSW(DS_rpm);
    if (ax < DSW(DS_car_rpm_redline)) {
        if ((s8)bl > 0) DSB(DS_overrev_ticks) = (u8)(bl - 1);   /* dec bl; jl */
        return;
    }
    if (ax <= DSW(DS_car_rpm_max)) {
        bl++;
        if (bl < 0x1E) {
            DSB(DS_overrev_ticks) = bl;
            return;
        }
    }
    if (ax >= DSW(DS_car_rpm_max)) ax = DSW(DS_car_rpm_max);
    DSW(DS_rpm) = ax;
    DSB(DS_run_state) = 3;                              /* blown engine */
}

/* ---------------------------------------------------------------------------------------------- */
/* 06b3:0008 steer_update (C code) — simulation.md §4.6 */
void steer_update(void)
{
    DSW(DS_speed_sq) >>= 9;
    s16 target = (s16)(-(s16)((s16)((u16)DSB(DS_curve_sum) << 8) >> 5) - (DSS(DS_yaw) >> 2));
    s16 delta = (s16)(DSC(DS_steer_in) * 120);
    s8 sin_ = DSC(DS_steer_in);

    if (sin_ == 0 && DSB(DS_unit_advanced) != 0 && DSW(DS_road_curve) == 0) {
        s16 a = DSS(DS_steer_angle);
        if (a < 0) a = (s16)-(u16)a;
        if (a < 0x600) {
            s16 y = DSS(DS_yaw);
            if (y < 0) y = (s16)-(u16)y;
            if (y < 0xC00) {                            /* self-centring on straights */
                DSS(DS_yaw) = (s16)(DSS(DS_yaw) - (DSS(DS_yaw) >> 3));
                s16 s = (s16)(DSS(DS_steer_angle) - (DSS(DS_steer_angle) >> 2));
                DSS(DS_steer_angle) = s;
                if (s < 0) s = (s16)-(u16)s;            /* jns on the subtraction's result */
                if (s < 0x100) DSS(DS_steer_angle) = 0;
                return;
            }
        }
    }
    if (DSB(DS_unit_advanced) != 0 && sin_ == 0 && DSS(DS_steer_angle) != 0)
        delta = (s16)(delta - (u16)((u32)(u16)(DSS(DS_steer_angle) >> 7) * DSW(DS_speed_sq)));
    if ((sin_ > 0 && target > DSS(DS_steer_angle)) || (sin_ < 0 && target < DSS(DS_steer_angle)))
        delta = (s16)(delta - ((s16)(DSS(DS_steer_angle) - target) >> 3));
    s32 k = 256 - (s32)(DSW(DS_speed_copy) >> 10);
    delta = (s16)(u16)(((u32)((s32)delta * k)) >> 8);   /* bytes 1-2 of the long product */
    if (delta > 0xA00) delta = 0xA00;
    if (delta < -0xA00) delta = -0xA00;
    s16 s = (s16)(DSS(DS_steer_angle) + delta);
    if (s > 0xE80) s = 0xE80;
    if (s < -0xE80) s = -0xE80;
    DSS(DS_steer_angle) = s;
}

/* ---------------------------------------------------------------------------------------------- */
/* 06c9:5a6f fall_anim — simulation.md §4.12 */
void fall_anim(void)
{
    DSB(DS_gear) = 0;
    DSB(DS_grind_timer) = 0;
    free_rev();
    brake_1600();
    u8 m = DSB(DS_fall_mode);
    if (m == 1) {
        DSS(DS_player_x) = (s16)(DSS(DS_player_x) - 25);
        if (!(DSS(DS_player_x) < DSS(DS_fall_edge_x))) return;
    } else if (m == 4) {
        u16 d = (u16)(DSW(DS_fall_depth) + 3);
        DSW(DS_fall_depth) = d;
        if (d >= 100) DSB(DS_run_state) = 2;
        return;
    } else {
        DSS(DS_player_x) = (s16)(DSS(DS_player_x) + 25);
        if (DSS(DS_player_x) <= DSS(DS_fall_edge_x)) return;
    }
    u16 bx = DSW(DS_fall_speed);
    u16 ax = (u16)(DSW(DS_fall_depth) + bx);
    DSW(DS_fall_depth) = ax;
    DSW(DS_fall_speed) = (u16)(bx + 2);
    if (ax >= 0x118) DSB(DS_run_state) = 2;
}
