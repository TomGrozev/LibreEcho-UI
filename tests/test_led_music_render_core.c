/*
 * Focused behavioural tests for the twelve-pixel music renderer (#64, #65).
 *
 * Build:
 *   gcc -std=c99 -Wall -Wextra -Wpedantic -O2 -o build/test-led-music-render \
 *       tests/test_led_music_render_core.c src/adapter/led_music_render.c \
 *       src/adapter/led_music_director.c src/adapter/led_output.c \
 *       src/adapter/music_visualizer_protocol.c
 */
#include "../src/adapter/led_music_render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void require_condition(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "render: %s\n", message);
        exit(1);
    }
}

static struct le_music_features feat(void)
{
    struct le_music_features f;
    memset(&f, 0, sizeof(f));
    f.feature_version = 2U;
    f.session = 1U;
    f.timestamp_ms = 1000U;
    f.energy = 170U;
    f.density = 120U;
    f.warmth = 90U;
    f.transientness = 90U;
    f.loudness_fast = 140U;
    f.loudness_slow = 150U;
    f.onset_low = 180U;
    f.onset_mid = 120U;
    f.onset_high = 90U;
    f.beat_confidence = 200U;
    f.beat_phase = 20000U;
    return f;
}

static struct le_music_scene scene_for(enum le_music_grammar grammar,
                                       struct led_rgb accent)
{
    struct le_music_scene scene;
    memset(&scene, 0, sizeof(scene));
    scene.grammar = grammar;
    scene.next_grammar = grammar;
    scene.palette.primary = (struct led_rgb){40U, 120U, 220U};
    scene.palette.secondary = (struct led_rgb){20U, 200U, 190U};
    scene.palette.accent = accent;
    scene.next_palette = scene.palette;
    return scene;
}

static unsigned long signature(const struct led_rgb out[LE_LED_PIXELS])
{
    unsigned long h = 2166136261UL;
    int i;
    for (i = 0; i < LE_LED_PIXELS; i++) {
        h = (h ^ out[i].r) * 16777619UL;
        h = (h ^ out[i].g) * 16777619UL;
        h = (h ^ out[i].b) * 16777619UL;
    }
    return h;
}

static int lit_pixels(const struct led_rgb out[LE_LED_PIXELS])
{
    int count = 0, i;
    for (i = 0; i < LE_LED_PIXELS; i++)
        if (out[i].r || out[i].g || out[i].b)
            count++;
    return count;
}

static int exact_matches(const struct led_rgb out[LE_LED_PIXELS],
                         struct led_rgb colour)
{
    int count = 0, i;
    for (i = 0; i < LE_LED_PIXELS; i++)
        if (out[i].r == colour.r && out[i].g == colour.g &&
            out[i].b == colour.b)
            count++;
    return count;
}

static unsigned int total_light(const struct led_rgb out[LE_LED_PIXELS])
{
    unsigned int sum = 0U;
    int i;
    for (i = 0; i < LE_LED_PIXELS; i++)
        sum += out[i].r + out[i].g + out[i].b;
    return sum;
}

static unsigned int le_distance_px(int a, int b)
{
    int d = a > b ? a - b : b - a;
    return (unsigned int)(d > LE_LED_PIXELS / 2 ? LE_LED_PIXELS - d : d);
}

static int brightest(const struct led_rgb out[LE_LED_PIXELS])
{
    unsigned int best = 0U;
    int at = 0, i;
    for (i = 0; i < LE_LED_PIXELS; i++) {
        unsigned int v = out[i].r + out[i].g + out[i].b;
        if (v > best) {
            best = v;
            at = i;
        }
    }
    return at;
}

/*
 * Feed a constant-feature frame stream (85 ms apart, as the producer sends at
 * ~11.7 fps) for three seconds.  Returns the longest run, in ms, for which the
 * rendered frame stayed identical, and the number of distinct brightest
 * pixels; reports the most pixels lit in any frame.
 */
static unsigned int longest_static_ms(enum le_music_grammar grammar,
                                      struct led_rgb accent,
                                      struct le_music_features base,
                                      int *distinct_peaks, int *max_lit)
{
    struct le_music_scene scene = scene_for(grammar, accent);
    struct le_music_render_state state;
    struct led_rgb out[LE_LED_PIXELS];
    struct led_rgb prev[LE_LED_PIXELS];
    int seen[LE_LED_PIXELS];
    unsigned int run = 0U, longest = 0U, t;
    int i;

    memset(seen, 0, sizeof(seen));
    *max_lit = 0;
    le_music_render_state_reset(&state, 1U);
    for (t = 0U; t <= 3000U; t += 85U) {
        struct le_music_features f = base;
        f.seq = t / 85U + 1U;
        f.timestamp_ms = 1000U + t;
        le_music_render(&scene, &f, &state, out);
        if (lit_pixels(out) > *max_lit)
            *max_lit = lit_pixels(out);
        seen[brightest(out)] = 1;
        if (t > 0U && memcmp(out, prev, sizeof(out)) == 0) {
            run += 85U;
            if (run > longest)
                longest = run;
        } else {
            run = 0U;
        }
        memcpy(prev, out, sizeof(out));
    }
    *distinct_peaks = 0;
    for (i = 0; i < LE_LED_PIXELS; i++)
        *distinct_peaks += seen[i];
    return longest;
}

int main(void)
{
    struct led_rgb accent;
    struct le_music_render_state state;
    struct le_music_features f;
    struct led_rgb out[LE_LED_PIXELS];
    unsigned long sig[LE_GRAMMAR_COUNT];
    enum le_music_grammar g;

    require_condition(led_output_accent(LE_ACCENT_WARM_WHITE, &accent) == 1,
                      "calibrated accent must exist");

    /* 1. Every grammar has a distinct geometry signature. */
    for (g = LE_GRAMMAR_RIBBON; g < LE_GRAMMAR_COUNT; g++) {
        struct le_music_scene scene = scene_for(g, accent);
        int j;
        f = feat();
        le_music_render_state_reset(&state, 1U);
        le_music_render(&scene, &f, &state, out);
        sig[g] = signature(out);
        for (j = 0; j < (int)g; j++)
            require_condition(sig[j] != sig[g],
                              "grammars must render distinct geometry");
    }

    /* 2. Determinism: identical inputs render identical frames. */
    {
        struct le_music_scene scene = scene_for(LE_GRAMMAR_ORBIT, accent);
        struct led_rgb out2[LE_LED_PIXELS];
        struct le_music_render_state state2;
        f = feat();
        le_music_render_state_reset(&state, 3U);
        le_music_render_state_reset(&state2, 3U);
        le_music_render(&scene, &f, &state, out);
        le_music_render(&scene, &f, &state2, out2);
        require_condition(memcmp(out, out2, sizeof(out)) == 0,
                          "the same scene must render identically");
    }

    /* 3. Sparse material keeps visible negative space for ribbon/ember. */
    {
        struct le_music_scene scene = scene_for(LE_GRAMMAR_RIBBON, accent);
        f = feat();
        f.density = 10U;
        f.loudness_fast = 20U;
        f.energy = 30U;
        le_music_render_state_reset(&state, 1U);
        le_music_render(&scene, &f, &state, out);
        require_condition(lit_pixels(out) < LE_LED_PIXELS,
                          "sparse ribbon must leave dark pixels");
    }

    /* 4. Effects expose at most two near-white pixels, from the calibrated
       accent rather than raw white. */
    require_condition(!(accent.r == 255U && accent.g == 255U &&
                        accent.b == 255U),
                      "accent must not be raw 255,255,255");
    {
        struct le_music_scene scene = scene_for(LE_GRAMMAR_RIBBON, accent);
        f = feat();

        scene.effect = LE_EFFECT_INHALE_CUT_BLOOM;
        scene.effect_center = 4U;
        scene.effect_age_ms = 800U;      /* 100..300 ms white window */
        le_music_render_state_reset(&state, 1U);
        le_music_render(&scene, &f, &state, out);
        require_condition(exact_matches(out, accent) == 1,
                          "inhale must show exactly one near-white lead pixel");

        scene.effect_age_ms = 600U;      /* dark cut */
        le_music_render(&scene, &f, &state, out);
        require_condition(lit_pixels(out) == 0, "inhale cut must be dark");

        scene.effect = LE_EFFECT_WHITE_APERTURE;
        scene.effect_age_ms = 100U;
        le_music_render(&scene, &f, &state, out);
        require_condition(exact_matches(out, accent) == 2,
                          "aperture must show exactly two near-white pixels");

        scene.effect = LE_EFFECT_KNIFE_SWEEP;
        scene.effect_age_ms = 120U;
        le_music_render(&scene, &f, &state, out);
        require_condition(exact_matches(out, accent) <= 2,
                          "knife sweep must stay within the accent budget");
        require_condition(exact_matches(out, accent) >= 1,
                          "knife sweep must show a moving point");

        scene.effect = LE_EFFECT_SHOCK_RING;
        scene.effect_age_ms = 60U;
        le_music_render(&scene, &f, &state, out);
        require_condition(exact_matches(out, accent) <= 2,
                          "shock ring must keep at most two white pixels");

        /* After the high-contrast window the accent must be gone. */
        scene.effect = LE_EFFECT_INHALE_CUT_BLOOM;
        scene.effect_age_ms = 1200U;     /* bloom phase */
        le_music_render(&scene, &f, &state, out);
        require_condition(exact_matches(out, accent) == 0,
                          "the near-white lead must not persist past its window");
    }

    /* 5. A crossfade blends two palettes without going dark. */
    {
        struct le_music_scene scene = scene_for(LE_GRAMMAR_RIBBON, accent);
        struct le_music_scene faded = scene;
        f = feat();
        faded.next_palette.primary = (struct led_rgb){255U, 120U, 30U};
        faded.next_palette.secondary = (struct led_rgb){255U, 70U, 90U};
        faded.next_palette.accent = accent;
        faded.crossfade = 128U;
        le_music_render_state_reset(&state, 1U);
        le_music_render(&faded, &f, &state, out);
        require_condition(lit_pixels(out) > 0,
                          "mid-crossfade must still light the active scene");
    }

    /* 6. Constant music with low beat confidence must keep moving: no
       grammar may hold an identical frame for a second (hardware showed
       twin_arcs frozen for 9.9 s) and twin_arcs must never fill the ring. */
    {
        static const enum le_music_grammar moving[3] = {
            LE_GRAMMAR_TWIN_ARCS, LE_GRAMMAR_EMBER, LE_GRAMMAR_RIBBON
        };
        int k;
        for (k = 0; k < 3; k++) {
            int peaks, max_lit;
            unsigned int still;
            f = feat();
            f.beat_confidence = 40U;      /* typical: beat not locked */
            f.beat_phase = 12345U;        /* phase does not advance */
            f.density = 255U;             /* worst case for arc length */
            still = longest_static_ms(moving[k], accent, f, &peaks,
                                      &max_lit);
            if (still >= 1000U || peaks < 2)
                fprintf(stderr, "render: %s static %u ms, %d peak pixels\n",
                        le_music_grammar_name(moving[k]), still, peaks);
            require_condition(still < 1000U,
                              "constant music must not freeze the ring");
            require_condition(peaks >= 2,
                              "the bright point must travel around the ring");
            if (moving[k] == LE_GRAMMAR_TWIN_ARCS)
                require_condition(max_lit <= 8,
                                  "twin arcs must leave the ring partly dark");
            if (moving[k] == LE_GRAMMAR_EMBER)
                require_condition(max_lit <= 8,
                                  "ember must keep substantial black space");
        }
    }

    /* 7. A locked beat moves the ribbon one pixel per beat, not a whole
       revolution per beat (which aliases at the producer frame rate). */
    {
        struct le_music_scene scene = scene_for(LE_GRAMMAR_RIBBON, accent);
        int seen[LE_LED_PIXELS];
        int distinct = 0, i, last = -1, max_step = 0;
        unsigned int t;
        memset(seen, 0, sizeof(seen));
        le_music_render_state_reset(&state, 1U);
        for (t = 0U; t <= 2000U; t += 85U) {   /* four beats at 120 bpm */
            f = feat();
            f.beat_confidence = 220U;
            f.bpm_x100 = 12000U;
            f.beat_phase = (t % 500U) * 65536U / 500U;
            f.seq = t / 85U + 1U;
            f.timestamp_ms = 1000U + t;
            le_music_render(&scene, &f, &state, out);
            if (last >= 0) {
                int step = (int)le_distance_px(last, brightest(out));
                if (step > max_step)
                    max_step = step;
            }
            last = brightest(out);
            seen[last] = 1;
        }
        for (i = 0; i < LE_LED_PIXELS; i++)
            distinct += seen[i];
        require_condition(max_step <= 1,
                          "locked ribbon must not jump pixels between frames");
        require_condition(distinct >= 3 && distinct <= 6,
                          "locked ribbon must step about one pixel per beat");
    }

    /* 8. A bass onset flashes and then decays within a few hundred ms. */
    {
        struct le_music_scene scene = scene_for(LE_GRAMMAR_TWIN_ARCS, accent);
        unsigned int baseline = 0U, spike, later, settled = 0U, t;
        unsigned int seq = 1U;
        le_music_render_state_reset(&state, 1U);
        for (t = 0U; t < 1000U; t += 33U) {
            f = feat();
            f.beat_confidence = 40U;
            f.onset_low = 40U;
            f.seq = seq++;
            f.timestamp_ms = 1000U + t;
            le_music_render(&scene, &f, &state, out);
            baseline = total_light(out);
        }
        f.onset_low = 255U;
        f.seq = seq++;
        f.timestamp_ms = 1000U + t;
        le_music_render(&scene, &f, &state, out);
        spike = total_light(out);
        require_condition(spike > baseline + baseline / 4U,
                          "a bass onset must flash the ring");
        f.onset_low = 40U;
        t += 99U;
        f.seq = seq++;
        f.timestamp_ms = 1000U + t;
        le_music_render(&scene, &f, &state, out);
        later = total_light(out);
        require_condition(later < spike,
                          "the onset flash must start to decay");
        for (t += 33U; t < 1000U + 500U; t += 33U) {
            f.seq = seq++;
            f.timestamp_ms = 1000U + t;
            le_music_render(&scene, &f, &state, out);
            settled = total_light(out);
        }
        require_condition(settled < baseline + baseline / 8U,
                          "the onset flash must release back to the base level");
    }

    /* 9. ledd re-renders the last frame every 33 ms: the extended render
       clock keeps motion and the onset release running between frames, and
       a clock behind the last one holds rather than jumping. */
    {
        struct le_music_scene scene = scene_for(LE_GRAMMAR_TWIN_ARCS, accent);
        struct led_rgb first[LE_LED_PIXELS];
        unsigned int flash, decayed;
        le_music_render_state_reset(&state, 1U);
        f = feat();
        f.beat_confidence = 40U;
        f.onset_low = 40U;
        f.seq = 1U;
        f.timestamp_ms = 1000U;
        le_music_render_at(&scene, &f, &state, 1000U, out);
        f.onset_low = 255U;
        f.seq = 2U;
        f.timestamp_ms = 1085U;
        le_music_render_at(&scene, &f, &state, 1085U, first);
        flash = total_light(first);
        le_music_render_at(&scene, &f, &state, 1085U + 66U, out);
        decayed = total_light(out);
        require_condition(memcmp(first, out, sizeof(out)) != 0,
                          "re-rendering one frame must keep the ring moving");
        require_condition(decayed < flash,
                          "the onset must decay between producer frames");
        memcpy(first, out, sizeof(out));
        le_music_render_at(&scene, &f, &state, 1085U, out);
        require_condition(memcmp(first, out, sizeof(out)) == 0,
                          "a clock step backwards must hold the ring");
    }

    puts("music renderer: ok");
    return 0;
}
