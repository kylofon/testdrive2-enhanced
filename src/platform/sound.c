/* PC-speaker sound — port of TD2EGA 06c9:6269 sfx_tick, 06c9:758a..75ea music API, 06c9:75ea
 * music_tick and 06c9:7934..79c1 (port/spec/platform.md §4.15, §4.16, §5.5-5.7; checked against the
 * disassembly). All player state lives in DGROUP at its original address.
 *
 * Speaker hardware: PIT channel 2 (divisor latched through port 42h) and the gate (bit 0) / data (bit 1)
 * bits of port 61h. The port keeps a shadow of both and passes them to host_speaker(); the speaker
 * sounds while both bits are set. */
#include "sound.h"

#include "../host.h"
#include "../symbols.h"

/* Host-side hardware shadows (not game state). */
static u16 pit2_div;                        /* last divisor latched into PIT channel 2 */
static u8  port61;                          /* bits 0 and 1 of port 61h */

/* PORT: guard against the original's endless control-opcode loops (a stream or pattern made only of
 * control opcodes, a looping stream with segment 0, a vibrato whose limits both fail). The original
 * hangs inside the timer interrupt; the port stops after this many opcodes in one tick. */
#define LOOP_GUARD 100000

static void spk_update(void)
{
    host_speaker(pit2_div, (port61 & 3) == 3);
}

void spk_port61_and(u8 mask)
{
    port61 &= mask;
    spk_update();
}

void spk_port61_or(u8 bits)
{
    port61 = (u8)((port61 | bits) & 3);
    spk_update();
}

void spk_set_divisor(u16 div)
{
    pit2_div = div;
    spk_update();
}

/* ================================================================================ effect stream player */

/* shr ax,cl on an 8086 (count not masked). TODO(verify): 286+ CPUs mask cl to 5 bits; the shipped
 * streams only use shifts 0 and 3. */
static u16 shr16(u16 v, u8 cl)
{
    return cl >= 16 ? 0 : (u16)(v >> cl);
}

/* 06c9:6269 sfx_tick */
void sfx_tick(void)
{
    if (!(DSB(DS_snd_enable) & 1)) {
        DSB(DS_snd_busy) = 0;
        DSW(DS_sfx_seg) = 0;
        DSW(DS_sfx_note_left) = 0;
        spk_port61_and(0xFC);
        return;
    }
    if (DSW(DS_sfx_note_left) != 0) {
        u16 old = DSW(DS_sfx_note_left);
        DSW(DS_sfx_note_left)--;
        if (old == DSW(DS_sfx_note_cut)) spk_port61_and(0xFC);
        return;
    }
    for (long guard = 0; guard < LOOP_GUARD; guard++) {
        u16 es = DSW(DS_sfx_seg);
        if (es == 0) goto end;
        u16 si = DSW(DS_sfx_off);
        u8 bl = rd8(es, si);
        if ((s8)bl >= 0) {                                   /* note / rest event */
            u16 ax = rd16(es, (u16)(si + 1));
            DSW(DS_sfx_note_left) = ax;
            DSW(DS_sfx_off) = (u16)(DSW(DS_sfx_off) + 3);
            if (bl == 0) {
                DSW(DS_sfx_note_cut) = 0;
                spk_port61_and(0xFC);
                return;
            }
            u8 cl = DSB(DS_sfx_shift);
            DSW(DS_sfx_note_cut) = cl ? shr16(ax, cl) : 0;
            spk_set_divisor(DSW((u16)(DS_sfx_div + (u16)(bl << 1))));   /* no range check */
            spk_port61_or(3);
            return;
        }
        u16 k = (u8)-bl;                                     /* neg bl ; xor bh,bh */
        if (k > 0x0B) goto end;                              /* 80..F4 act like FF */
        switch (k) {                                         /* jump table CS:6334 */
        case 0:
        case 1:                                              /* FF */
            goto end;
        case 2:                                              /* FE s: articulation shift */
            DSB(DS_sfx_shift) = rd8(es, (u16)(si + 1));
            DSW(DS_sfx_off) = (u16)(DSW(DS_sfx_off) + 2);
            break;
        case 3: case 4: case 5: {                            /* FD/FC/FB n16: loop start */
            u16 j = (u16)((k - 3) << 1);
            DSW((u16)(DS_sfx_loop_cnt + j)) = rd16(es, (u16)(si + 1));
            DSW(DS_sfx_off) = (u16)(DSW(DS_sfx_off) + 3);
            DSW((u16)(DS_sfx_loop_start + j)) = DSW(DS_sfx_off);
            break;
        }
        case 6: case 7: case 8: {                            /* FA/F9/F8: loop end */
            u16 j = (u16)((k - 6) << 1);
            DSW((u16)(DS_sfx_loop_cnt + j))--;
            if ((s16)DSW((u16)(DS_sfx_loop_cnt + j)) < 0) {  /* js */
                DSW(DS_sfx_off)++;
            } else {
                DSW((u16)(DS_sfx_loop_end + j)) = si;
                DSW(DS_sfx_off) = DSW((u16)(DS_sfx_loop_start + j));
            }
            break;
        }
        default: {                                           /* F7/F6/F5: break out on the last pass */
            u16 j = (u16)((k - 9) << 1);
            if (DSW((u16)(DS_sfx_loop_cnt + j)) == 0)
                DSW(DS_sfx_off) = DSW((u16)(DS_sfx_loop_end + j));   /* possibly stale (original bug) */
            else
                DSW(DS_sfx_off)++;
            break;
        }
        }
        continue;

    end:                                                     /* 06c9:62fd */
        DSB(DS_snd_busy) = 0;
        if (DSB(DS_sfx_loop_set) != 0) {
            DSW(DS_sfx_off) = DSW(DS_sfx_loop_off);
            DSW(DS_sfx_seg) = DSW(DS_sfx_loop_seg);
            continue;
        }
        DSW(DS_sfx_note_left) = 0;                           /* pointer stays on the FF: re-hit every tick */
        spk_port61_and(0xFC);
        return;
    }
}

/* 06c9:7934 */
void sound_off(void)
{
    DSB(DS_snd_enable) &= 2;
    DSW(DS_sfx_seg) = 0;
}

/* 06c9:7940 */
void sound_on(void)
{
    DSB(DS_snd_enable) |= 1;
}

/* 06c9:7946 sfx_set_loop */
void sfx_set_loop(FarPtr s)
{
    DSW(DS_sfx_loop_seg) = s.seg;
    DSW(DS_sfx_loop_off) = s.off;
    DSB(DS_sfx_loop_set) = 1;
    if (DSB(DS_snd_busy) == 0) {
        DSW(DS_sfx_seg) = s.seg;
        DSW(DS_sfx_off) = s.off;
    }
}

/* 06c9:7971 */
void sfx_clear_loop(void)
{
    DSB(DS_sfx_loop_set) = 0;
}

/* 06c9:7977 sfx_play: note_left is not reset, so the sounding note finishes first */
void sfx_play(FarPtr s)
{
    DSW(DS_sfx_seg) = s.seg;
    DSW(DS_sfx_off) = s.off;
    DSB(DS_snd_busy) = 1;
}

/* 06c9:798f */
s16 sfx_is_playing(void)
{
    return DSB(DS_snd_busy);
}

/* 06c9:7995 */
void sfx_stop(void)
{
    DSW(DS_sfx_seg) = 0;
}

/* 06c9:799c */
void sfx_play_if_enabled(FarPtr s)
{
    if (DSB(DS_snd_enable) != 3) return;
    sfx_play(s);
}

/* 06c9:79bb */
void music_on(void)
{
    DSB(DS_snd_enable) |= 2;
}

/* 06c9:79c1 */
void music_off(void)
{
    DSB(DS_snd_enable) &= 1;
}

/* ================================================================================ music player */

/* Voice record at DS:6609 (platform.md §5.6) */
#define V_SHIFT      (DS_mus_voice + 0)     /* u8 */
#define V_ARP_INIT   (DS_mus_voice + 2)
#define V_ARP_RELOAD (DS_mus_voice + 4)
#define V_ARP_MASK   (DS_mus_voice + 6)
#define V_ARP_TABLE  (DS_mus_voice + 8)     /* u8[8] */
#define V_VIB_DELAY  (DS_mus_voice + 16)
#define V_VIB_PERIOD (DS_mus_voice + 18)
#define V_VIB_DIR    (DS_mus_voice + 20)    /* s8 */
#define V_VIB_STEP   (DS_mus_voice + 22)
#define V_VIB_MIN    (DS_mus_voice + 24)    /* s16 */
#define V_VIB_MAX    (DS_mus_voice + 26)    /* s16 */

/* 06c9:758a music_play */
void music_play(FarPtr songs, u16 n)
{
    if (DSB(DS_snd_enable) != 3) return;
    u16 bx = (u16)(songs.off + (u16)(n << 2));
    DSW(DS_mus_seg) = songs.seg;
    u16 ax = rd16(songs.seg, bx);
    DSW(DS_mus_list) = ax;
    DSW(DS_mus_list_loop) = ax;
    ax = rd16(songs.seg, (u16)(bx + 2));
    DSW(DS_mus_list2) = ax;
    DSW(DS_mus_list2_loop) = ax;
    DSB(DS_mus_start) = 1;
    DSB(DS_snd_busy) = 1;
}

/* 06c9:75c8 music_set_voices: segment word first */
void music_set_voices(FarPtr voices)
{
    DSW(DS_mus_voices) = voices.seg;
    DSW(DS_mus_voices + 2) = voices.off;
}

/* 06c9:75d9 (no callers) */
void music_set_6625(u16 w6627, u16 w6625)
{
    DSW(0x6625) = w6625;                   /* never read */
    DSW(0x6627) = w6627;
}

/* 06c9:75ea music_tick */
void music_tick(void)
{
    long guard = 0;
    u16 es, si, bx;
    if (DSB(DS_snd_enable) != 3) goto stop;
    if (DSB(DS_mus_start) != 0) {
        DSB(DS_mus_start) = 0;
        spk_port61_or(2);                                    /* speaker data on; notes switch the gate */
        goto next_pattern;
    }
    if (DSB(DS_snd_busy) == 0) goto stop;
    goto step;

next_pattern:                                                /* 06c9:761e */
    if (++guard > LOOP_GUARD) goto stop;
    es = DSW(DS_mus_seg);
    si = DSW(DS_mus_list);
    if (si == 0) goto stop;
    DSW(DS_mus_pat) = rd16(es, si);
    DSW(DS_mus_transpose) = (u16)(rd8(es, (u16)(si + 2)) + DSW(DS_mus_transpose_base));
    DSW(DS_mus_list) = (u16)(DSW(DS_mus_list) + 3);

next_event:                                                  /* 06c9:7643 */
    for (;;) {
        if (++guard > LOOP_GUARD) goto stop;
        si = DSW(DS_mus_pat);
        if (si == 0) goto stop;
        DSW(DS_mus_pat) = (u16)(DSW(DS_mus_pat) + 2);
        es = DSW(DS_mus_seg);
        bx = rd8(es, si);
        if ((s8)bx >= 0) break;                              /* note or rest */
        if ((s8)bx < (s8)0xFB) goto stop;                    /* 80..FA */
        switch ((u8)-(s8)bx) {                               /* jump table CS:766C */
        case 1:                                              /* FF: restart the song's pattern list */
            DSW(DS_mus_list) = DSW(DS_mus_list_loop);
            goto next_pattern;
        case 2:                                              /* FE t: tempo */
            DSW(DS_mus_tempo) = rd8(es, (u16)(si + 1));
            continue;
        case 3: {                                            /* FD v: voice v (28 bytes of VOICES + v*32) */
            u16 vsi = (u16)((u16)(rd8(es, (u16)(si + 1)) << 5) + DSW(DS_mus_voices + 2));
            u16 vds = DSW(DS_mus_voices);
            for (u16 i = 0; i < 28; i++) DSB((u16)(DS_mus_voice + i)) = rd8(vds, (u16)(vsi + i));
            continue;
        }
        case 4:                                              /* FC: end of pattern */
            goto next_pattern;
        default:                                             /* FB: stop song, speaker left as it is */
            DSB(DS_snd_busy) = 0;
            DSB(DS_mus_start) = 0;
            DSW(DS_mus_list) = 0;
            DSW(DS_mus_list2) = 0;
            return;
        }
    }

    /* note / rest: es:si = event */
    if (bx == 0) {                                           /* rest */
        u16 ax = (u16)((u8)DSW(DS_mus_tempo) * rd8(es, (u16)(si + 1)));   /* mul byte: AL * arg */
        DSW(DS_mus_on_left) = bx;
        DSW(DS_mus_off_left) = ax;
        goto gate_off;
    } else {
        u8 arg = rd8(es, (u16)(si + 1));
        u16 ax = (u16)((u8)DSW(DS_mus_tempo) * (u8)(arg & 0x7F));
        u16 dx;
        u8 cl = DSB(V_SHIFT);
        if ((arg & 0x80) || cl == 0) dx = 0;
        else dx = shr16(ax, cl);                             /* shr dx,cl */
        DSW(DS_mus_off_left) = dx;
        ax = (u16)(ax - dx);
        DSW(DS_mus_on_left) = ax;
        if (ax == 0) goto gate_off;
        bx = (u16)(bx + DSW(DS_mus_transpose));
        DSW(DS_mus_note) = bx;
        DSW(DS_mus_div) = DSW((u16)(DS_mus_div_table + (u16)(bx << 1)));   /* no range check */
        spk_set_divisor(DSW(DS_mus_div));
        spk_port61_or(1);                                    /* gate on */
        DSW(DS_mus_arp_cnt) = DSW(V_ARP_INIT);
        DSW(DS_mus_arp_idx) = 0;
        DSW(DS_mus_vib_ofs) = 0;
        DSW(DS_mus_vib_cnt) = DSW(V_VIB_DELAY);
        u8 al = DSB(V_VIB_DIR);
        if (al == 0) al++;
        DSB(DS_mus_vib_dir) = al;
        /* fall into step */
    }

step:                                                        /* 06c9:7758 */
    if (DSW(DS_mus_on_left) != 0) {
        if (--DSW(DS_mus_on_left) != 0) goto effects;
        spk_port61_and(0xFE);                                /* gate off */
        DSB(DS_mus_start) = 0;
        return;
    }
off_part:                                                    /* 06c9:7771 */
    if (DSW(DS_mus_off_left) != 0) {
        DSW(DS_mus_off_left)--;
        return;
    }
    goto next_event;

gate_off:                                                    /* 06c9:76e3 */
    spk_port61_and(0xFE);
    goto off_part;

effects:                                                     /* 06c9:7780 */
    if (DSW(DS_mus_arp_cnt) != 0 && --DSW(DS_mus_arp_cnt) == 0) {
        DSW(DS_mus_arp_cnt) = DSW(V_ARP_RELOAD);
        DSW(DS_mus_arp_idx)++;
        u16 i = (u16)(DSW(DS_mus_arp_idx) & DSW(V_ARP_MASK));     /* may index past the 8-byte table */
        u16 b = (u16)(DSB((u16)(V_ARP_TABLE + i)) + DSW(DS_mus_note));
        DSW(DS_mus_div) = DSW((u16)(DS_mus_div_table + (u16)(b << 1)));
    }
    if (DSW(DS_mus_vib_cnt) != 0) {
        DSW(DS_mus_vib_cnt)--;
        goto out;
    }
    for (;;) {                                               /* 06c9:77c2 */
        u16 ax;
        if (++guard > LOOP_GUARD) break;
        if ((s8)DSB(DS_mus_vib_dir) >= 0) {
            ax = (u16)(DSW(V_VIB_STEP) + DSW(DS_mus_vib_ofs));
            if ((s16)ax > (s16)DSW(V_VIB_MAX)) goto bounce;
        } else {
            ax = (u16)(DSW(DS_mus_vib_ofs) - DSW(V_VIB_STEP));
            if ((s16)ax < (s16)DSW(V_VIB_MIN)) goto bounce;
        }
        DSW(DS_mus_vib_ofs) = ax;                            /* 06c9:77d6 */
        DSW(DS_mus_vib_cnt) = DSW(V_VIB_PERIOD);
        break;
    bounce:                                                  /* 06c9:77fc */
        if (DSB(V_VIB_DIR) == 0) {                           /* triangle: turn round */
            DSB(DS_mus_vib_dir) = (u8)-(s8)DSB(DS_mus_vib_dir);
            continue;
        }
        DSW(DS_mus_vib_ofs) = 0;                             /* sawtooth: restart */
        DSW(DS_mus_vib_cnt) = DSW(V_VIB_PERIOD);
        break;
    }
out:                                                         /* 06c9:77df */
    spk_set_divisor((u16)(DSW(DS_mus_vib_ofs) + DSW(DS_mus_div)));
    return;

stop:                                                        /* 06c9:7602 */
    DSB(DS_snd_busy) = 0;
    DSB(DS_mus_start) = 0;
    spk_port61_and(0xFC);
}
