/*
 * Twelve-pixel music renderer.
 *
 * Turns a director scene plus the current feature frame into a logical
 * 12-pixel RGB frame.  Every grammar shares one interface so the director can
 * crossfade between them.  Rhythm controls motion, timbre controls colour,
 * dynamics control occupied area; the palette stays restrained and dark space
 * is preserved by starting from black each frame.
 *
 * The output is a logical frame (brightness is applied later by led_output).
 * Near-white accents use the calibrated named warm/cool points from
 * led_output, never raw 255,255,255.
 */
#ifndef LE_MUSIC_RENDER_H
#define LE_MUSIC_RENDER_H

#include "led_music_director.h"
#include "led_output.h"

struct le_music_render_state {
    unsigned int peak[LE_LED_PIXELS];  /* classic mirror peak hold */
    unsigned int free_phase;           /* free-running orbit position */
    int have_phase;
    /* Motion and onset envelope, advanced by the render clock so the ring
       keeps moving and beats decay between producer frames. */
    unsigned int clock_ms;             /* last render clock */
    int have_clock;
    unsigned int last_seq;             /* last feature frame seen */
    unsigned int last_frame_ms;
    unsigned int last_phase;           /* last frame beat_phase */
    int have_frame;
    unsigned int motion_q16;           /* ring position, 1 px == 65536 */
    unsigned int onset_env;            /* bass hit envelope 0..255 */
    unsigned int onset_floor;          /* slow onset_low baseline */
};

void le_music_render_state_reset(struct le_music_render_state *state,
                                 unsigned int session);

/* Render at the frame's own timestamp (one render per frame). */
void le_music_render(const struct le_music_scene *scene,
                     const struct le_music_features *features,
                     struct le_music_render_state *state,
                     struct led_rgb out[LE_LED_PIXELS]);

/*
 * Render at clock_ms, the producer timeline extended by the time since the
 * last accepted frame.  ledd re-renders the last frame every 33 ms; passing
 * the extended clock lets motion and the onset release run between frames.
 * The clock is treated as monotonic: a value behind the last one holds.
 */
void le_music_render_at(const struct le_music_scene *scene,
                        const struct le_music_features *features,
                        struct le_music_render_state *state,
                        unsigned int clock_ms,
                        struct led_rgb out[LE_LED_PIXELS]);

#endif /* LE_MUSIC_RENDER_H */
