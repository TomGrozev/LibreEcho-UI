#include "led_music_render.h"
#include "music_visualizer_protocol.h"

#include <string.h>

/* Sub-pixel ring geometry: motion is kept in 1/65536 px, drawing in 1/256 px.
   motion_q16 wraps at two revolutions so half-speed drift stays continuous. */
#define LE_PX_Q16            65536U
#define LE_RING_Q16          ((unsigned int)LE_LED_PIXELS * LE_PX_Q16)
#define LE_MOTION_WRAP       (2U * LE_RING_Q16)
#define LE_RING8             ((unsigned int)LE_LED_PIXELS * 256U)
/* Beat phase is trusted for motion only above this confidence (as orbit). */
#define LE_BEAT_LOCK         110U
/* Onset envelope: instant attack, linear release from full in this time. */
#define LE_ONSET_RELEASE_MS  200U
#define LE_ONSET_MARGIN      12U
/* onset_low baseline time constant (frame-rate independent). */
#define LE_ONSET_FLOOR_MS    700U
/* Classic mirror peak release: 12 levels per 33 ms render tick. */
#define LE_PEAK_FALL_PER_TICK  12U
#define LE_PEAK_FALL_TICK_MS 33U
/* Low-confidence orbit: one pixel per 180 ms of render clock. */
#define LE_ORBIT_STEP_MS     180U
/* Longest clock step honoured at once (a stall must not jump the ring). */
#define LE_MAX_STEP_MS       500U

static unsigned int le_mul255(unsigned int value, unsigned int factor)
{
    return (value * factor + 127U) / 255U;
}

static struct led_rgb le_lerp(struct led_rgb a, struct led_rgb b,
                              unsigned int t)
{
    struct led_rgb out;

    out.r = (a.r * (255U - t) + b.r * t + 127U) / 255U;
    out.g = (a.g * (255U - t) + b.g * t + 127U) / 255U;
    out.b = (a.b * (255U - t) + b.b * t + 127U) / 255U;
    return out;
}

static void le_add(struct led_rgb *pixel, struct led_rgb colour,
                   unsigned int factor)
{
    unsigned int r = le_mul255(colour.r, factor);
    unsigned int g = le_mul255(colour.g, factor);
    unsigned int b = le_mul255(colour.b, factor);

    pixel->r = pixel->r + r > 255U ? 255U : pixel->r + r;
    pixel->g = pixel->g + g > 255U ? 255U : pixel->g + g;
    pixel->b = pixel->b + b > 255U ? 255U : pixel->b + b;
}

/* Replace a pixel with a scaled colour: used for calibrated white leads so
   they read as the named near-white point rather than base + white. */
static void le_set(struct led_rgb *pixel, struct led_rgb colour,
                   unsigned int factor)
{
    pixel->r = le_mul255(colour.r, factor);
    pixel->g = le_mul255(colour.g, factor);
    pixel->b = le_mul255(colour.b, factor);
}

static unsigned int le_distance(unsigned int a, unsigned int b)
{
    unsigned int d = a > b ? a - b : b - a;
    return d > LE_LED_PIXELS / 2U ? LE_LED_PIXELS - d : d;
}

/* Circular distance from pixel i to a sub-pixel position (both 1/256 px). */
static unsigned int le_distance8(unsigned int i, unsigned int pos8)
{
    unsigned int at = i * 256U;
    unsigned int d = at > pos8 ? at - pos8 : pos8 - at;
    return d > LE_RING8 / 2U ? LE_RING8 - d : d;
}

static unsigned int le_pos8(unsigned int q16, unsigned int offset_px)
{
    return ((q16 >> 8) + offset_px * 256U) % LE_RING8;
}

void le_music_render_state_reset(struct le_music_render_state *state,
                                 unsigned int session)
{
    if (state == NULL)
        return;
    memset(state, 0, sizeof(*state));
    state->free_phase = session % LE_LED_PIXELS;
    state->motion_q16 = (session % LE_LED_PIXELS) * LE_PX_Q16;
}

/*
 * Advance motion and the onset envelope to clock_ms.  Motion free-runs on the
 * render clock, faster with loudness/energy, unless the beat is locked: then
 * it advances exactly one pixel per beat (bpm, or the beat-phase delta when
 * no tempo is reported).  Everything is time-based, so the producer frame
 * rate (11.7 or 23.4 fps) does not change speeds.  The envelope kicks only on
 * a rising edge: onset_low rising above the previous frame and a slow
 * baseline, or a KICK flag that was clear in the previous frame (the producer
 * latches event flags across several frames).
 */
static void le_render_advance(struct le_music_render_state *s,
                              const struct le_music_features *f,
                              unsigned int clock_ms)
{
    unsigned int dt = 0U;
    unsigned int step = 0U;
    unsigned int fall;
    int locked = f->beat_confidence >= LE_BEAT_LOCK;
    int new_frame = !s->have_frame || f->seq != s->last_seq ||
                    f->timestamp_ms != s->last_frame_ms;

    if (!s->have_clock) {
        s->clock_ms = clock_ms;
        s->have_clock = 1;
    } else if (clock_ms - s->clock_ms < 0x80000000U) {
        dt = clock_ms - s->clock_ms;
        s->clock_ms = clock_ms;
    }
    if (dt > LE_MAX_STEP_MS)
        dt = LE_MAX_STEP_MS;
    s->step_ms = dt;

    fall = dt * 255U / LE_ONSET_RELEASE_MS;
    s->onset_env = s->onset_env > fall ? s->onset_env - fall : 0U;

    if (locked && f->bpm_x100 >= 4000U) {
        unsigned int bpm = f->bpm_x100 > 24000U ? 24000U : f->bpm_x100;
        step = dt * (bpm * LE_PX_Q16 / 6000000U);   /* 1 px per beat */
    } else if (locked) {
        if (new_frame && s->have_frame) {
            unsigned int delta = (f->beat_phase - s->last_phase) & 0xFFFFU;
            if (delta < 0x8000U)                     /* ignore re-syncs */
                step = delta;
        }
    } else {
        /* ~0.6 px/s when quiet up to ~3 px/s when loud. */
        step = dt * (40U + f->loudness_fast / 2U + f->energy / 8U);
    }
    s->motion_q16 = (s->motion_q16 + step) % LE_MOTION_WRAP;

    if (new_frame) {
        unsigned int kick = 0U;
        unsigned int frame_dt;
        if (!s->have_frame) {
            s->onset_floor = f->onset_low;
            s->last_onset = f->onset_low;
            s->last_events = 0U;
        }
        frame_dt = s->have_frame ? f->timestamp_ms - s->last_frame_ms : 0U;
        if (frame_dt > LE_ONSET_FLOOR_MS)
            frame_dt = LE_ONSET_FLOOR_MS;
        if (f->onset_low > s->last_onset + LE_ONSET_MARGIN &&
            f->onset_low > s->onset_floor + LE_ONSET_MARGIN) {
            kick = (f->onset_low - s->onset_floor) * 2U;
            if (kick > 255U)
                kick = 255U;
        }
        if ((f->events & LE_MUSIC_EVENT_KICK) &&
            !(s->last_events & LE_MUSIC_EVENT_KICK) && f->onset_low > kick)
            kick = f->onset_low;
        if (kick > s->onset_env)
            s->onset_env = kick;
        if (f->onset_low >= s->onset_floor)
            s->onset_floor += ((f->onset_low - s->onset_floor) * frame_dt +
                               LE_ONSET_FLOOR_MS / 2U) / LE_ONSET_FLOOR_MS;
        else
            s->onset_floor -= ((s->onset_floor - f->onset_low) * frame_dt +
                               LE_ONSET_FLOOR_MS / 2U) / LE_ONSET_FLOOR_MS;
        s->last_onset = f->onset_low;
        s->last_events = f->events;
        s->last_seq = f->seq;
        s->last_frame_ms = f->timestamp_ms;
        s->last_phase = f->beat_phase & 0xFFFFU;
        s->have_frame = 1;
    }
}

/* ------------------------------- grammars ------------------------------- */

static void le_render_ribbon(struct led_rgb out[LE_LED_PIXELS],
                             const struct le_music_features *f,
                             const struct le_music_render_state *state,
                             struct led_rgb primary, struct led_rgb secondary,
                             unsigned int warmth)
{
    unsigned int width = (2U + f->loudness_fast / 70U) * 256U;  /* 2..5 px */
    /* Free-running unless the beat is locked, then one pixel per beat. */
    unsigned int center = le_pos8(state->motion_q16, 0U);
    unsigned int level = 30U + f->energy / 2U + state->onset_env / 4U;
    struct led_rgb ribbon = le_lerp(primary, secondary, warmth);
    unsigned int i;

    if (level > 255U)
        level = 255U;
    for (i = 0U; i < LE_LED_PIXELS; i++) {
        unsigned int d = le_distance8(i, center);
        if (d < width) {
            unsigned int frac = (width - d) * 255U / width;
            le_add(&out[i], ribbon, le_mul255(frac, level));
        }
    }
}

static void le_render_twin_arcs(struct led_rgb out[LE_LED_PIXELS],
                                const struct le_music_features *f,
                                const struct le_music_render_state *state,
                                struct led_rgb primary,
                                struct led_rgb secondary)
{
    unsigned int env = state->onset_env;
    /* Arc half-length 1.1..2 px: at most four pixels per arc, so at most
       eight of twelve are ever lit and the ring never fills. */
    unsigned int half = 288U + f->density / 2U + env / 4U;
    unsigned int weight = 24U + f->loudness_fast / 2U + env * 104U / 255U;
    unsigned int a = le_pos8(state->motion_q16, 0U);
    unsigned int b = (a + 6U * 256U) % LE_RING8;
    unsigned int i;

    if (half > 512U)
        half = 512U;
    if (weight > 255U)
        weight = 255U;
    for (i = 0U; i < LE_LED_PIXELS; i++) {
        unsigned int d0 = le_distance8(i, a);
        unsigned int d1 = le_distance8(i, b);
        unsigned int d = d0 < d1 ? d0 : d1;
        if (d < half) {
            unsigned int frac = (half - d) * 255U / half;
            struct led_rgb colour = (d0 <= d1) ? primary : secondary;
            le_add(&out[i], colour, le_mul255(frac, weight));
        }
    }
    /* Transient accents land on the arc heads, never on dark pixels. */
    if (f->onset_high > 120U) {
        unsigned int head = ((a + 128U) / 256U) % LE_LED_PIXELS;
        unsigned int accent = f->onset_high;
        le_add(&out[head], primary, accent);
        le_add(&out[(head + 6U) % LE_LED_PIXELS], secondary, accent);
    }
}

static void le_render_orbit(struct led_rgb out[LE_LED_PIXELS],
                            const struct le_music_features *f,
                            struct le_music_render_state *state,
                            struct led_rgb primary, struct led_rgb secondary)
{
    unsigned int center;
    unsigned int size = 1U + f->onset_low / 90U;
    unsigned int level = 40U + f->energy / 2U;
    unsigned int i;

    if (f->beat_confidence >= 110U) {
        center = (f->beat_phase * LE_LED_PIXELS) / 65536U;
    } else {
        /* Free-running and deterministic: position follows the render clock
           (one pixel per 180 ms whatever the frame rate), offset by the
           session seed, never beat phase. */
        center = (state->clock_ms / LE_ORBIT_STEP_MS + state->free_phase) %
                 LE_LED_PIXELS;
    }
    if (f->events & LE_MUSIC_EVENT_SNARE)
        size++;
    for (i = 0U; i < LE_LED_PIXELS; i++) {
        unsigned int d0 = le_distance(i, center);
        unsigned int d1 = le_distance(i, (center + 6U) % LE_LED_PIXELS);
        unsigned int d = d0 < d1 ? d0 : d1;
        if (d < size) {
            unsigned int frac = (size - d) * 255U / size;
            struct led_rgb colour = (d0 <= d1) ? primary : secondary;
            le_add(&out[i], colour, le_mul255(frac, level));
        }
    }
}

static void le_render_split_pulse(struct led_rgb out[LE_LED_PIXELS],
                                  const struct le_music_features *f,
                                  struct led_rgb primary,
                                  struct led_rgb secondary)
{
    unsigned int count = (f->onset_high > f->onset_low) ? 4U : 2U;
    unsigned int step = LE_LED_PIXELS / count;
    unsigned int size = 1U + f->loudness_fast / 80U;
    unsigned int i;

    for (i = 0U; i < LE_LED_PIXELS; i++) {
        unsigned int region = i % step;
        unsigned int in_region = region < size;
        unsigned int band;
        if (!in_region)
            continue;
        if (i < 4U)
            band = f->onset_low;
        else if (i < 9U)
            band = f->onset_mid;
        else
            band = f->onset_high;
        le_add(&out[i], (i % 2U == 0U) ? primary : secondary, band);
    }
}

static void le_render_classic_mirror(struct led_rgb out[LE_LED_PIXELS],
                                     const struct le_music_features *f,
                                     struct le_music_render_state *state,
                                     struct led_rgb primary,
                                     struct led_rgb secondary)
{
    unsigned int rotate = (f->beat_phase * 2U) / 65536U;  /* 0..1 */
    unsigned int bands[6];
    unsigned int i;

    bands[0] = f->onset_low;
    bands[1] = f->onset_low / 2U + f->onset_mid / 2U;
    bands[2] = f->onset_mid;
    bands[3] = f->onset_mid / 2U + f->onset_high / 2U;
    bands[4] = f->onset_high;
    bands[5] = f->energy;

    for (i = 0U; i < 6U; i++) {
        unsigned int at = (i + rotate) % 6U;
        unsigned int mirrored = (LE_LED_PIXELS - 1U - at) % LE_LED_PIXELS;
        if (bands[i] > state->peak[i])
            state->peak[i] = bands[i];
        else {
            unsigned int fall = state->step_ms * LE_PEAK_FALL_PER_TICK /
                                LE_PEAK_FALL_TICK_MS;
            state->peak[i] = state->peak[i] > fall ? state->peak[i] - fall : 0U;
        }
        le_add(&out[at], primary, state->peak[i]);
        le_add(&out[mirrored], secondary, state->peak[i]);
    }
}

static void le_render_ember(struct led_rgb out[LE_LED_PIXELS],
                            const struct le_music_features *f,
                            struct le_music_render_state *state,
                            struct led_rgb primary, struct led_rgb secondary)
{
    /* The block drifts at half the shared motion rate. */
    unsigned int center = le_pos8(state->motion_q16 / 2U, 0U);
    unsigned int env = state->onset_env;
    unsigned int level = le_mul255(f->loudness_fast, 130U) + env * 90U / 255U;
    unsigned int i;

    if (level > 220U)
        level = 220U;
    /* Bass-weighted amber/red motion with substantial black space: a block
       of at most five pixels plus two opposite sparks on bass hits. */
    for (i = 0U; i < LE_LED_PIXELS; i++) {
        unsigned int d = le_distance8(i, center);
        if (d < 640U) {
            unsigned int cover = d <= 384U ? 255U : (640U - d) * 255U / 256U;
            le_add(&out[i], primary, le_mul255(cover, level));
        } else if (d >= 1152U && d < 1408U) {
            le_add(&out[i], secondary, env / 2U);
        }
    }
    /* Sparse cool transient sparks. */
    if (f->transientness > 120U) {
        unsigned int spark = (f->timestamp_ms / 240U) % LE_LED_PIXELS;
        le_add(&out[spark], (struct led_rgb){120U, 170U, 255U},
               f->transientness);
    }
}

/* ------------------------------ transitions ----------------------------- */

static void le_render_effect(struct led_rgb out[LE_LED_PIXELS],
                             const struct le_music_scene *scene,
                             struct led_rgb primary,
                             struct led_rgb accent)
{
    unsigned int age = scene->effect_age_ms;
    unsigned int center = scene->effect_center % LE_LED_PIXELS;
    unsigned int i;

    switch (scene->effect) {
    case LE_EFFECT_INHALE_CUT_BLOOM:
        /* 1. contract, 2. dark cut, 3. one near-white lead, 4. bloom. */
        if (age < 500U) {
            unsigned int factor = (500U - age) * 255U / 500U;
            for (i = 0U; i < LE_LED_PIXELS; i++) {
                out[i].r = le_mul255(out[i].r, factor);
                out[i].g = le_mul255(out[i].g, factor);
                out[i].b = le_mul255(out[i].b, factor);
            }
        } else if (age < 700U) {
            for (i = 0U; i < LE_LED_PIXELS; i++)
                out[i] = (struct led_rgb){0U, 0U, 0U};
        } else if (age < 950U) {
            for (i = 0U; i < LE_LED_PIXELS; i++)
                out[i] = (struct led_rgb){0U, 0U, 0U};
            le_set(&out[center], accent, 255U);
        } else {
            unsigned int factor = (age - 950U) * 255U / 550U;
            if (factor > 255U)
                factor = 255U;
            for (i = 0U; i < LE_LED_PIXELS; i++) {
                out[i].r = le_mul255(out[i].r, factor);
                out[i].g = le_mul255(out[i].g, factor);
                out[i].b = le_mul255(out[i].b, factor);
            }
        }
        break;
    case LE_EFFECT_WHITE_APERTURE: {
        unsigned int offset = age * 6U / 900U;
        unsigned int high = age < 300U ? 255U : 120U;
        le_set(&out[(center + offset) % LE_LED_PIXELS], accent, high);
        le_set(&out[(center + 6U + LE_LED_PIXELS - offset) % LE_LED_PIXELS],
               accent, high);
        break;
    }
    case LE_EFFECT_KNIFE_SWEEP: {
        unsigned int pos = age * LE_LED_PIXELS / 800U;
        unsigned int high = age < 200U ? 255U : 90U;
        le_set(&out[pos % LE_LED_PIXELS], accent, high);
        break;
    }
    case LE_EFFECT_SHOCK_RING:
        if (age < 150U) {
            le_set(&out[center], accent, 255U);
            le_set(&out[(center + 6U) % LE_LED_PIXELS], accent, 255U);
        } else {
            unsigned int spread = (age - 150U) * 4U / 550U + 1U;
            for (i = 0U; i < LE_LED_PIXELS; i++) {
                if (le_distance(i, center) <= spread)
                    le_add(&out[i], primary, 180U);
            }
        }
        break;
    default:
        break;
    }
}

/* -------------------------------- entry --------------------------------- */

void le_music_render(const struct le_music_scene *scene,
                     const struct le_music_features *features,
                     struct le_music_render_state *state,
                     struct led_rgb out[LE_LED_PIXELS])
{
    le_music_render_at(scene, features, state, features->timestamp_ms, out);
}

void le_music_render_at(const struct le_music_scene *scene,
                        const struct le_music_features *features,
                        struct le_music_render_state *state,
                        unsigned int clock_ms,
                        struct led_rgb out[LE_LED_PIXELS])
{
    struct led_rgb primary;
    struct led_rgb secondary;
    struct led_rgb accent;
    unsigned int blend = scene->crossfade;
    unsigned int i;

    for (i = 0U; i < LE_LED_PIXELS; i++)
        out[i] = (struct led_rgb){0U, 0U, 0U};

    primary = le_lerp(scene->palette.primary, scene->next_palette.primary,
                      blend);
    secondary = le_lerp(scene->palette.secondary,
                        scene->next_palette.secondary, blend);
    accent = le_lerp(scene->palette.accent, scene->next_palette.accent, blend);
    le_render_advance(state, features, clock_ms);

    switch (scene->grammar) {
    case LE_GRAMMAR_RIBBON:
        le_render_ribbon(out, features, state, primary, secondary,
                         features->warmth);
        break;
    case LE_GRAMMAR_TWIN_ARCS:
        le_render_twin_arcs(out, features, state, primary, secondary);
        break;
    case LE_GRAMMAR_ORBIT:
        le_render_orbit(out, features, state, primary, secondary);
        break;
    case LE_GRAMMAR_SPLIT_PULSE:
        le_render_split_pulse(out, features, primary, secondary);
        break;
    case LE_GRAMMAR_CLASSIC_MIRROR:
        le_render_classic_mirror(out, features, state, primary, secondary);
        break;
    case LE_GRAMMAR_EMBER:
        le_render_ember(out, features, state, primary, secondary);
        break;
    default:
        break;
    }

    if (scene->effect != LE_EFFECT_NONE)
        le_render_effect(out, scene, primary, accent);
}
