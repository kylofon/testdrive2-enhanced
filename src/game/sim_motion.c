/* Driving simulation: motion along the road, road objects, damage, road edges and roadside scenery —
 * port/spec/simulation.md §4.9–§4.11 (06c9:4674..06c9:4d3e), checked against work/sim/rdis.txt. */
#define SIM_INTERNAL
#include "sim.h"

#include "../platform/res.h"
#include "../platform/sound.h"

/* Pull-over / roadblock states of the police car restrict the player (cop_state 2..7). */
static bool pulled_over(void)
{
    u8 cs = DSB(DS_cop_state);
    return cs != 8 && cs >= 2;
}

/* 06c9:4713..4789: random roadside scenery 70 units ahead (ring slot di). */
static void spawn_scenery(u16 di, u8 r)
{
    if (r < DSB(DS_scenery_density)) {
        u8 al = (u8)(rand8() & 0x0F);
        if (al == 0x0F) goto none;
        if (al == 7) goto none;
        if (al > 7) {                                   /* right side */
            if (DSB(DS_lookahead_flags) & 0x8C) goto none;
            u16 u = (u16)(DSW(DS_player_pos) - 0x3AED);   /* unit + 100 */
            for (u16 si = 0; DSW(DS_right_zones + si) != 0; si = (u16)(si + 8)) {
                if (u <= DSW(DS_right_zones + 2 + si)) {
                    if (u >= DSW(DS_right_zones + si)) goto none;
                    break;
                }
            }
        } else {
            if (DSB(DS_lookahead_flags) & 0xE0) goto none;
        }
        al = (u8)(al - 7);                              /* side -7..-1, 1..7 */
        DSB(DS_roadside_side + di) = al;
        u8 k;
        do k = (u8)(rand8() & 7); while (k >= 6);
        u8 t = (u8)(k * 5);
        DSB(DS_roadside_type + di) = t;
        /* word at DS:1DB4 + type*4: the segment of the scenery sprite handle (null = no sprite) */
        if (DSW(DS_scenery_sprites + ((u16)t << 2)) != 0) return;
    }
none:
    DSB(DS_roadside_type + di) = 0xFF;
    DSB(DS_roadside_side + di) = 0xFF;
}

static s16 clamp_yaw(s16 v)
{
    s16 mn = DSS(DS_YAW_MIN), mx = DSS(DS_YAW_MAX);
    if (v > mx) return mx;
    if (v < mn) return mn;
    return v;
}

/* Object handler dispatch through DS:33E6 (BX = code * 2, BP = player_pos). */
static void object_dispatch(u8 code, u16 bp)
{
    u16 bx = (u16)code << 1;
    u16 target = DSW(DS_OBJECT_JT + bx);
    switch (target) {
    case 0x4929: obj_station_zone(); break;
    case 0x4947: obj_past_station(); break;
    case 0x494D: obj_parked_cop(bp); break;
    case 0x495D: obj_roadblock(bp); break;
    case 0x499A: obj_place_roadside(bx); break;
    case 0x49C9: obj_toggle_37f7(); break;
    case 0x49CF: obj_toggle_median(); break;
    case 0x49D5: obj_toggle_37f8(); break;
    case 0x49DB: obj_density_up(); break;
    case 0x49E1: obj_density_down(); break;
    case 0x49E7: obj_sign_posts(); break;
    case 0x4A18: obj_none(); break;
    case 0x4A19: obj_lane_obstacle(bx); break;
    default:
        /* Object codes above 0x2F would call through the tables that follow DS:33E6; no shipped stage
         * has one. */
        fatal("sim: road object %02X has no handler (%04X)", code, target);
    }
}

/* 06c9:4674 motion — simulation.md §4.9 */
void motion(void)
{
    if (pulled_over()) {                                /* pull over to x = 160 at 20 per tick */
        s16 x = DSS(DS_player_x);
        if (x != 160) {
            if (x > 160) {
                x = (s16)(x - 20);
                if (!(x > 160)) x = 160;
            } else {
                x = (s16)(x + 20);
                if (!(x < 160)) x = 160;
            }
            DSS(DS_player_x) = x;
        }
    }
    car_edges();
    DSB(DS_skidding) = 0;
    DSB(DS_unit_advanced) = 0;
    u16 ax = (u16)(DSB(DS_speed_hi) * 3 + DSW(DS_player_sub));
    for (;;) {
        if (DSB(DS_run_state) >= 2) return;
        DSW(DS_player_sub) = ax;
        if ((ax >> 8) == 0) return;

        /* ---- one road unit */
        DSB(DS_lookahead_flags) ^= DSB(REC(ROAD((u16)(DSW(DS_player_pos) + 0x47))));
        DSB(DS_unit_advanced) = 1;
        u8 r = rand8();
        u8 bl = (u8)(DSB(DS_ring_counter) + 1);         /* only the low byte is incremented */
        DSB(DS_ring_counter) = bl;
        u16 di = (u8)(bl + 0x46) & 0x7F;
        spawn_scenery(di, r);

        if (--DSW(DS_fuel) == 0) DSB(DS_run_state) = 4;             /* out of gas */
        if (--DSW(DS_units_to_finish) == 0) DSW(DS_units_to_finish)++;

        /* cornering grip */
        s16 st = DSS(DS_steer_angle);
        u8 mph = DSB(DS_speed_hi);
        u16 v2 = (u16)(mph * mph);
        DSW(DS_speed_sq) = v2;
        u16 dx = DSW(DS_car_grip + 2);
        u16 lim = DSW(DS_car_grip);
        u16 dx2 = (u16)(dx << 1);
        if (dx2 < v2) {                                 /* the division fits (also skips v2 == 0) */
            dx = dx2 >> 1;
            lim = div32_16((u32)dx << 16 | lim, v2, NULL);
            if (st >= 0) {
                if (st > (s16)lim) DSB(DS_skidding) = 1;
            } else {
                lim = (u16)-lim;
                if (st < (s16)lim) DSB(DS_skidding) = 1;
            }
        }
        DSW(DS_grip_limit) = lim;

        /* yaw */
        s16 cx = DSS(DS_yaw);
        s16 c = DSS(DS_road_curve);
        if (DSW(DS_demo_mode) == 1) {
            DSS(DS_yaw) = 0;
            DSS(DS_steer_angle) = (s16)-(u16)c;         /* view_yaw keeps its old value */
        } else {
            s16 add = st;
            if (DSB(DS_skidding) != 0) {
                u16 a = DSW(DS_grip_limit);
                u16 b = (u16)(-(u16)st + a);
                b = (u16)(b + (u16)(a << 1));
                add = (s16)b >> 1;
            }
            cx = (s16)(cx + c + add);
            DSS(DS_yaw) = cx;
            DSS(DS_view_yaw) = cx;
        }
        DSS(DS_view_yaw) = clamp_yaw(DSS(DS_view_yaw));
        DSS(DS_yaw) = clamp_yaw(DSS(DS_yaw));
        DSS(DS_view_yaw) = DSS(DS_view_yaw) >> 2;
        s16 px = DSS(DS_player_x);
        if (pulled_over()) {
            DSS(DS_steer_angle) = 0;
            DSS(DS_view_yaw) = 0;
            DSS(DS_yaw) = 0;
        }
        u8 a8 = (u8)((u16)(DSW(DS_yaw) << 1) >> 8);
        s32 p = (s32)sin_deg8(a8) * 36;                 /* 13a3:0012, imul */
        px = (s16)(px - (s8)(u8)((u32)p >> 8));         /* mov al, ah; cwde */
        DSS(DS_player_x) = px;
        car_edges();
        median_posts();
        road_edges();
        roadside_hit();

        /* advance */
        u16 pos = (u16)(DSW(DS_player_pos) + 1);
        DSW(DS_player_pos) = pos;
        u16 bp = pos;
        u8 b = ROAD(pos);
        DSB(DS_region_flags) = (u8)((DSB(DS_region_flags) & 0xFE) | (b >> 7));
        u16 rec = REC(b);
        u8 fl = DSB(rec), cv = DSB(rec + 1);
        DSB(DS_region_flags) ^= fl;
        DSS(DS_road_curve) = (s16)((u16)cv << 8) >> 2;              /* curve * 64 */
        s16 h = (s16)(s8)cv >> 1;
        DSW(DS_heading) = (u16)(DSW(DS_heading) + h);
        DSW(DS_cloud_scroll) = (u16)(DSW(DS_cloud_scroll) + (s16)((h >> 2) + h));
        DSB(DS_curve_ring + ((pos + 6) & 7)) = cv;
        object_dispatch(DSB(REC(ROAD((u16)(bp + 1))) + 3), bp);
        ax = (u16)(DSW(DS_player_sub) - 0x100);
    }
}

/* ---------------------------------------------------------------------------------------------- */
/* Object handlers — simulation.md §4.10 */

void obj_station_zone(void)                             /* 06c9:4929 */
{
    DSB(DS_station_zone) = 1;
    if (DSW(DS_last_stage) != 0) {
        DSW(DS_final_time) = DSW(DS_race_time);
        DSW(DS_final_seconds) = DSW(DS_clock_seconds);
        DSB(DS_finished) = 1;
    }
}

void obj_past_station(void) { DSB(DS_station_zone) = 2; }    /* 06c9:4947 */

void obj_parked_cop(u16 bp_pos)                         /* 06c9:494d */
{
    if (DSW(DS_cop_pos) != 0) return;
    DSW(DS_cop_pos) = (u16)(bp_pos + 0x50);
}

void obj_roadblock(u16 bp_pos)                          /* 06c9:495d */
{
    if (DSW(DS_cop_pos) != 0) return;
    u16 v = DSW(DS_speed);
    if (v <= 0x3200) return;
    DSW(DS_chase_speed) = v;
    u16 p = (u16)(bp_pos + 0x3C);
    DSW(DS_cop_pos) = p;
    DSW(DS_cop_speed) = 0;
    DSB(DS_cop_state) = 8;
    DSW(DS_cop_x) = (ROAD(p) & 0x80) ? 0x320 : 0x190;
}

void obj_place_roadside(u16 bx_code2)                   /* 06c9:499a */
{
    u16 bp = (u8)(DSB(DS_ring_counter) + 0x45) & 0x7F;
    u16 code = bx_code2 >> 1;
    u16 t = (u16)((code - 0x16) * 5);
    /* faithful-quirk: the original writes the rings through SS ([bp+368A], [bp+370A]); SS = DGROUP
     * (simulation.md Q8), so these are the DS arrays. */
    DSB(DS_roadside_type + bp) = (u8)t;
    DSB(DS_roadside_side + bp) = DSB(DS_ROADSIDE_SIDE_BASE + code);
}

void obj_toggle_37f7(void) { DSB(DS_toggle_37f7) ^= 1; }                        /* 06c9:49c9 */
void obj_toggle_median(void) { DSB(DS_median) ^= 1; }                          /* 06c9:49cf */
void obj_toggle_37f8(void) { DSB(DS_backdrop_off) ^= 1; }                      /* 06c9:49d5 */
void obj_density_up(void) { DSB(DS_scenery_density) += 0x10; }                 /* 06c9:49db */
void obj_density_down(void) { DSB(DS_scenery_density) -= 0x10; }               /* 06c9:49e1 */
void obj_none(void) { }                                                        /* 06c9:4a18 */

void obj_sign_posts(void)                               /* 06c9:49e7 */
{
    s16 ax = -400;
    if ((DSB(DS_region_flags) & 1) && DSB(DS_median) != 0) ax = -800;
    if (!overlap(ax, 5)) {
        ax = (DSB(DS_region_flags) & 1) ? 800 : 400;
        if (!overlap(ax, 5)) return;
    }
    hit_object();
}

void obj_lane_obstacle(u16 bx_code2)                    /* 06c9:4a19 */
{
    s16 ax = (bx_code2 & 2) ? -200 : 200;               /* odd codes: left lane */
    if (overlap(ax, 7)) hit_object();
}

/* 06c9:4a2e hit_object */
void hit_object(void)
{
    brake(0x640);
    sfx_play(ds_ptr(DS_SND_BUMP));                      /* 06c9:7977 */
    u8 al = rand8();
    if ((s8)al <= 0) return;                            /* 0 or >= 0x80: no damage */
    if (DSW(DS_demo_mode) != 0) return;
    DSB(DS_damage_hits)++;
    if (DSB(DS_damage_hits) > 4) {                      /* "Car took too much damage" */
        DSB(DS_run_state) = 8;
        return;
    }
    if (al <= 0x19) {                                   /* engine */
        u8 a = (u8)(DSB(DS_engine_damage) + 0x19);
        DSB(DS_engine_damage) += a;
        if ((u8)(a << 1) >= DSB(DS_car_engine_strength)) DSB(DS_run_state) = 5;
    } else if (al <= 0x32) {                            /* suspension */
        u16 a = (u16)(DSW(DS_suspension_damage) + 1000);
        DSW(DS_suspension_damage) += a;
        if ((u16)(a << 1) >= DSW(DS_car_grip)) DSB(DS_run_state) = 6;
    } else if (al <= 0x4B) {                            /* steering */
        u16 a = (u16)(DSW(DS_steering_health) - 0x4B);
        DSW(DS_steering_health) = a;
        if (a < 0x7D) DSB(DS_run_state) = 7;
    } else if (al <= 0x64) {                            /* alignment */
        s16 bx = DSS(DS_alignment);
        if (bx >= 0 && (al & 1)) {
            bx = (s16)(bx + 0x25);
            DSS(DS_alignment) = bx;
            if (!(bx < 0x3E)) DSB(DS_run_state) = 7;
        } else {
            bx = (s16)(bx - 0x25);
            DSS(DS_alignment) = bx;
            if (!(bx > -0x3E)) DSB(DS_run_state) = 7;
        }
    } else if (DSB(DS_auto_trans) == 0) {
        DSB(DS_gear_broken + DSB(DS_gear)) = 1;
    }
}

/* ---------------------------------------------------------------------------------------------- */
/* 06c9:4afa car_edges — simulation.md §4.11 */
void car_edges(void)
{
    s16 ax = (s16)(DSS(DS_player_x) + 0x28);
    DSS(DS_car_centre) = ax;
    s16 bx = (s16)(ax - 0x5A);
    DSS(DS_car_left) = bx;
    DSS(DS_car_right) = (s16)(bx + 0xB4);               /* x-50 .. x+130 */
    s8 cl = DSC(DS_station_zone);
    if (cl < 1) return;
    if (cl == 1) {
        if (DSB(DS_finished) != 0) {
            if (DSB(DS_speed_hi) <= 5) DSB(DS_run_state) = 1;
            return;
        }
        if (ax >= 0x258) DSS(DS_player_x) = 0x230;      /* ax (the centre) is not updated */
    }
    if (DSB(DS_speed_hi) > 5) return;
    if (cl == 2) {                                      /* stopped past the station */
        DSB(DS_run_state) = 4;
        return;
    }
    DSB(DS_run_state) = (ax < 0x64) ? 9 : 1;            /* too far left of the pump / stage done */
}

/* 06c9:4d26 overlap: ZF = 1 when centre +- half overlaps car_left..car_right */
bool overlap(s16 ax_centre, s16 bx_half)
{
    s16 lo = (s16)(ax_centre - bx_half);
    s16 hi = (s16)(ax_centre + bx_half);
    if (lo > DSS(DS_car_right)) return false;
    return hi >= DSS(DS_car_left);
}

/* 06c9:4b5d median_posts: posts every 16 units */
void median_posts(void)
{
    if (((u8)(DSB(DS_ring_counter) - 1) & 0x0F) != 0) return;
    s16 ax = -500;
    if ((DSB(DS_region_flags) & 1) && DSB(DS_median) != 0) ax = -900;
    if (!overlap(ax, 5)) {
        ax = (DSB(DS_region_flags) & 1) ? 900 : 500;
        if (!overlap(ax, 5)) return;
    }
    hit_object();
}

/* 06c9:4be1: start the fall animation at edge bx */
static void start_fall(s16 bx_edge)
{
    DSS(DS_fall_edge_x) = bx_edge;
    DSW(DS_fall_speed) = 0;
    DSW(DS_fall_depth) = 0;
    DSB(DS_grind_timer) = 0;
    DSB(DS_skidding) = 0;
}

/* 06c9:4b9a road_edges */
void road_edges(void)
{
    u8 dl = DSB(DS_region_flags);
    s16 bx;
    if (DSS(DS_car_centre) >= 0) {                      /* right side */
        bx = (dl & 0x80) ? 400 : (dl & 1) ? 900 : 500;
        if (!(DSS(DS_car_right) > bx)) return;
        if ((dl & 0x80) || (!(dl & 4) && (dl & 2))) {   /* wall / barrier */
            DSS(DS_player_x) = (s16)(bx - 0x82);
            DSB(DS_run_state) = 2;
            return;
        }
        if (dl & 4) {                                   /* drop-off */
            DSB(DS_fall_mode) = 2;
            start_fall((s16)(bx + 0x46));
            return;
        }
        /* 4c34: right-side zones (unit + 30) */
        u16 dx = (u16)(DSW(DS_player_pos) - 0x3B33);
        for (u16 si = 0; DSW(DS_right_zones + si) != 0; si = (u16)(si + 8)) {
            u16 end = DSW(DS_right_zones + 2 + si);
            if (dx > end) continue;
            u16 start = DSW(DS_right_zones + si);
            if (dx < start) break;
            dx = (u16)(dx - start);
            u16 len = (u16)(end - start);
            s16 a = DSS(DS_right_zones + 4 + si), b = DSS(DS_right_zones + 6 + si);
            s16 w = idiv32_16((s32)(s16)(b - a) * (s16)dx, (s16)len, NULL);
            w = (s16)(w + a);
            bx = (s16)(bx + (s16)((u16)w << 1));
            if (DSS(DS_car_left) < bx) break;
            DSB(DS_fall_mode) = 4;                      /* sinking */
            start_fall(bx);
            return;
        }
    } else {                                            /* left side */
        bx = (dl & 0x80) ? -400 : (dl & 1) ? -900 : -500;
        if (!(DSS(DS_car_left) < bx)) return;
        if ((dl & 0x80) || (!(dl & 0x20) && (dl & 0x10))) {
            DSS(DS_player_x) = (s16)(bx + 0x32);
            DSB(DS_run_state) = 2;
            return;
        }
        if (dl & 0x20) {
            DSB(DS_fall_mode) = 1;
            start_fall((s16)(bx - 0x96));
            return;
        }
        /* the left side does not look at the zones (4c1f jumps straight to 4c7e) */
    }
    brake(1000);                                        /* 4c7e shoulder */
}

/* 06c9:4c85 roadside_hit */
void roadside_hit(void)
{
    u16 bx = DSB(DS_ring_counter) & 0x7F;
    u8 t = DSB(DS_roadside_type + bx);
    if ((s8)t < 0) return;
    u16 cx;
    bool big;
    if (t < 0x50 && DSW(DS_scenery_sprites + ((u16)t << 2)) != 0) {
        cx = DSB(DS_ROADSIDE_HALFWIDTH + (t >> 2));
        big = t >= 0x14;                                /* BP = type (4c9c) */
    } else {
        if (t < 0x1E) return;
        u16 seg = DSW(DS_sgn_seg);
        if (seg == 0) return;
        u16 q = div16_8((u8)(t - 0x1E), 5);             /* AH = 0: remainder lands in the index */
        u16 off = rd16(seg, (u16)(q << 1));
        if (off == 0) return;
        cx = rd16(seg, off) >> 1;                       /* sign width / 2 */
        /* PORT: simulation.md §4.11. For t < 0x50 BP holds the type (>= 0x1E); for t >= 0x50 the
         * original compares a stale BP (rpm, zone length or road position), which is >= 20 in practice.
         * Billboards always crash the car. */
        big = true;
    }
    s16 x = (s16)((s8)DSB(DS_roadside_side + bx) * 50);
    x = (s16)(x >= 0 ? x + 500 : x - 500);
    if (DSB(DS_region_flags) & 1) {
        if (x >= 0) x = (s16)(x + 400);
        else if (DSB(DS_median) != 0) x = (s16)(x - 400);
    }
    if (!overlap(x, (s16)cx)) return;
    hit_object();
    if (big) DSB(DS_run_state) = 2;                     /* big objects crash the car */
}
