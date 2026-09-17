#pragma once
/* Driving simulation — port of TD2EGA 06c9:3f40..06c9:5c97 and 06b3:0008 (port/spec/simulation.md).
 *
 * The far entry points (sim_stage_start, sim_restart_reset, sim_timer_routine, traffic_resync,
 * police_reset, steer_update) are declared in game.h. This header lists the near routines of the
 * hand-written assembly as C functions (register arguments become parameters / results, named after the
 * registers they model). All state is in mem[] (symbols.h offsets); nothing here keeps C state.
 *
 * Timing: sim_timer_routine is a timer routine (registered with codeptr and added to the routine list
 * DS:5F12 by sim_stage_start, after sfx_tick). It runs sound_tick on every 99.9985 Hz call and the
 * simulation tick on every 10th call. */
#include "game.h"

/* ---- 100 Hz part, input and controls (sim.c) */
void sound_tick(void);                          /* 06c9:412c rpm needle slew, engine / rumble / effect tones */
void read_input(void);                          /* 06c9:420a controls, idle timeout, hotkeys */
void decode_controls(void);                     /* 06c9:4281 autopilot, automatic, throttle, gear selection */
void shift_knob_anim(void);                     /* 06c9:43c3 knob animation, shift completion */
void clutch_check(u16 cx_old, u16 dx_new);      /* 06c9:4441 downshift lurch, grind tone */
void curve_sum(void);                           /* 06c9:448a DS:30E4 = sum of the curve ring, DS:30E6 = speed */
void hotkeys(u16 ax_key);                       /* 06c9:44a6 Esc / o O / d D through table DS:33DC */
void key_esc(void);                             /* 06c9:44bf */
void key_gate_mode(void);                       /* 06c9:44c5 */
void key_dash_toggle(void);                     /* 06c9:44d2 */
void brake_1600(void);                          /* 06c9:44d8 */
void brake_800(void);                           /* 06c9:44de */
void brake_400(void);                           /* 06c9:44e4 */
void engine(void);                              /* 06c9:44ea throttle, brake, skid, clutch, torque */
void free_rev(void);                            /* 06c9:4588 */
void drag_apply(u16 dx_force);                  /* 06c9:45bf speed += force - drag */
void brake(u16 dx_amount);                      /* 06c9:45fc speed -= amount, floor 0, then update_rpm */
/* 06c9:4612: rpm = ratio * speed >> 16 (min 800). Returns false in neutral, where the original leaves
 * CX / DX unchanged; otherwise *cx_old = previous rpm and *dx_new = new rpm (either pointer may be NULL). */
bool update_rpm(u16 *cx_old, u16 *dx_new);
void overrev_check(void);                       /* 06c9:4639 redline counter, blown engine */
void fall_anim(void);                           /* 06c9:5a6f car falling off / sinking */

/* ---- motion and the road (sim_motion.c) */
void motion(void);                              /* 06c9:4674 pull-over, sub-unit step, per-unit loop */
void obj_station_zone(void);                    /* 06c9:4929 object 0x0A */
void obj_past_station(void);                    /* 06c9:4947 object 0x0C */
void obj_parked_cop(u16 bp_pos);                /* 06c9:494d object 0x15 */
void obj_roadblock(u16 bp_pos);                 /* 06c9:495d object 0x16 */
void obj_place_roadside(u16 bx_code2);          /* 06c9:499a objects 0x1C..0x2F */
void obj_toggle_37f7(void);                     /* 06c9:49c9 object 0x17 */
void obj_toggle_median(void);                   /* 06c9:49cf object 0x18 */
void obj_toggle_37f8(void);                     /* 06c9:49d5 object 0x19 */
void obj_density_up(void);                      /* 06c9:49db object 0x1A */
void obj_density_down(void);                    /* 06c9:49e1 object 0x1B */
void obj_sign_posts(void);                      /* 06c9:49e7 objects 0x01..0x09 */
void obj_none(void);                            /* 06c9:4a18 objects 0x00, 0x0B */
void obj_lane_obstacle(u16 bx_code2);           /* 06c9:4a19 objects 0x0D..0x14 */
void hit_object(void);                          /* 06c9:4a2e bump: brake, sound, random damage */
void car_edges(void);                           /* 06c9:4afa car edges, station / out-of-gas stop checks */
void median_posts(void);                        /* 06c9:4b5d */
void road_edges(void);                          /* 06c9:4b9a walls, drop-offs, shoulder, zones */
void roadside_hit(void);                        /* 06c9:4c85 scenery collision */
bool overlap(s16 ax_centre, s16 bx_half);       /* 06c9:4d26 true = ZF (overlaps the car) */

/* ---- traffic and collisions between drivers (sim_traffic.c)
 * A traffic list is 50 entries of {u16 type, pos, sub; i16 x} at DS:3813 (oncoming) / DS:39A3 (same
 * direction); indices are entry * 8. A driver position is (pos, sub) = DX:AX. */
void traffic(void);                             /* 06c9:4d3f */
/* 06c9:4f92: SI = *si_idx (updated), BP = list base, CX = count * 8, DX:AX = driver, DI = driver x.
 * Returns true for a collision (ZF). */
bool meet_check(u16 *si_idx, u16 bp_list, u16 cx_n8, u16 ax_sub, u16 dx_pos, s16 di_x);
void push_behind(u16 *si_idx, u16 ax_sub, u16 dx_pos, s16 di_x);   /* 06c9:5022 (same-direction list) */
void pass_collisions(void);                     /* 06c9:595f */
/* 06c9:5a25: SI / DI = DS addresses of the {sub, pos, x, speed} blocks of a and b, BX = old relation,
 * DX = a.x. Returns CF; *cx is the new relation (left unchanged when a position is 0). */
bool pass_check(u16 si_a, u16 di_b, s16 bx_old, s16 dx_ax, u16 *cx);
void resync_list(u16 si_idx, u16 bp_list, u16 cx_n8);              /* 06c9:5b5a */
u16  nearest_ahead(u16 si_list, u16 cx_n8, u16 ax_sub, u16 dx_pos); /* 06c9:5c30 -> SI (DS address) */

/* ---- opponent, police, demo autopilot (sim_ai.c) */
void opponent_ai(void);                         /* 06c9:50a1 */
u8   ai_road_scan(u16 bx_pos, u16 di_factor);   /* 06c9:5299 -> BL road byte; sets DS:3344 */
u16  ai_gap(u16 dx_self, u16 cx_other, u16 ax_self_mph, u16 bx_other_mph);  /* 06c9:52fd -> AX */
void ai_lane_min(u16 ax_gap, s16 bx_x);         /* 06c9:5334 */
s16  ai_pick_lane(s16 ax_own_x, u16 *dx_gap);   /* 06c9:535b -> CX target x, DX gap */
void police(void);                              /* 06c9:53a6 */
void police_start(void);                        /* 06c9:5446 */
void police_chase_opp(void);                    /* 06c9:54f4 (jumped to from 53a6) */
void police_chase_player(void);                 /* 06c9:557a (jumped to from 53a6) */
/* 06c9:56ed. Returns true when the police gave up: the original then pops its return address and
 * returns from police() directly. */
bool police_drive(void);
void demo_steer(void);                          /* 06c9:5880 */
u8   at_finish(u16 bx_pos, u16 di_decel, u16 si_speed);   /* 06c9:5c5a -> AL */

#ifdef SIM_INTERNAL
/* Simulation names for DGROUP offsets that symbols.h names after another module (symbol_conflicts.txt). */
#define DS_player_sub       DS_player_pos_lo        /* 52C8 */
#define DS_player_x         DS_player_lateral       /* 52CC */
#define DS_speed_hi         (DS_speed + 1)          /* 52CF mph */
#define DS_opp_sub          DS_opp_pos_lo           /* 52D0 */
#define DS_opp_x            DS_opp_lateral          /* 52D4 */
#define DS_opp_speed_hi     (DS_opp_speed + 1)      /* 52D7 */
#define DS_cop_sub          DS_cop_pos_lo           /* 52D8 */
#define DS_cop_x            DS_cop_lateral          /* 52DC */
#define DS_cop_speed_hi     (DS_cop_speed + 1)      /* 52DF */
#define DS_tick_count10     DS_sim_tick10           /* 3346 10 Hz counter */
#define DS_opp_crashes      DS_opp_penalties        /* 331C */
#define DS_gearbox_timer    DS_gate_close_delay     /* 2F59 */
#define DS_gearbox_dirty    DS_redraw_gate          /* 2F5A */
#define DS_gauges_dirty     DS_redraw_inst          /* 2F5B */
#define DS_clock_dirty      DS_redraw_hud           /* 2F5C */
#define DS_opp_enabled      DS_opponent_enabled     /* 3379 */
#define DS_fall_depth       DS_fall_scroll          /* 33CE */
#define DS_roadside_type    DS_dat_scenery_type     /* 368A */
#define DS_roadside_side    DS_dat_scenery_offset   /* 370A */
#define DS_stage_len        DS_dat_road_units       /* 380D */
#define DS_finish_unit      DS_stage_length         /* 380F */
#define DS_units_to_finish  DS_distance_left        /* 5342 */
#define DS_region_flags     DS_start_flags          /* 5491 */
#define DS_SND_BUMP         DS_stream_noise_short   /* 5494 */
#define DS_opp_accel        DS_opp_bin              /* 5616 */
#define DS_RUMBLE           DS_noise_div            /* 32AE */
#define DS_ROAD0            DS_road_bytes           /* 3B51 first road byte */
#define DS_traffic_speed    DS_diff_c               /* 8432 */
#define DS_opponent_option  DS_game_mode            /* 843E */
#define DS_auto_trans       DS_diff_easy            /* 90A8 (byte) */
#define DS_escaped          DS_outran_police        /* 9258 */
#define DS_sgn_seg          DS_sgn_segment          /* 940E */
#define DS_clock_seconds    DS_stage_time           /* 942E */
#define DS_car_num_gears    DS_car                  /* 23A6 */

/* Traffic lists */
#define ONC_BASE            DS_oncoming             /* 3813 */
#define SAME_BASE           DS_same_dir             /* 39A3 */
#define CAR_TYPE 0
#define CAR_POS  2
#define CAR_SUB  4
#define CAR_X    6

/* Driver blocks {sub, pos, x, speed} */
#define DRV_PLAYER          DS_player_pos_lo        /* 52C8 */
#define DRV_OPP             DS_opp_pos_lo           /* 52D0 */
#define DRV_COP             DS_cop_pos_lo           /* 52D8 */

#define ROAD(p)             DSB(p)                  /* road byte at DS address p */
#define REC(b)              ((u16)(DS_road_records + ((b) & 0x7F) * 4))   /* {flags, curve, pitch, object} */

/* DIFF(e, d): high word of the 32-bit subtraction (e.pos:e.sub) - (d.pos:d.sub). `lt` is the signed
 * "less" result of that sbb (SF != OF), `zf` its zero flag (high word only), as the original's jl / jle /
 * jge / jg see them. */
typedef struct { s16 k; bool lt, zf; } SimDiff;
static inline SimDiff sim_diff(u16 e_pos, u16 e_sub, u16 d_pos, u16 d_sub)
{
    s32 hi = (s32)(s16)e_pos - (s32)(s16)d_pos - (e_sub < d_sub ? 1 : 0);
    SimDiff r = { (s16)hi, hi < 0, (u16)hi == 0 };
    return r;
}
static inline bool diff_le(SimDiff d) { return d.lt || d.zf; }   /* jle after the sbb */

static inline u16 wrap_up(u16 p)   { return p >= DSW(DS_stage_end) ? (u16)(p - DSW(DS_stage_len)) : p; }
static inline u16 wrap_down(u16 p) { return p < DS_ROAD0 ? (u16)(p + DSW(DS_stage_len)) : p; }
#endif
