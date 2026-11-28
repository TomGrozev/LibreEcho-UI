#ifndef LIBREECHO_WAKE_WORKER_H
#define LIBREECHO_WAKE_WORKER_H

#include <stddef.h>
#include <stdint.h>

struct le_wake_worker {
  void *implementation;
};

struct le_wake_observation {
  uint64_t detection_sample;
  float vad_score;
  int vad_active;
  int playback_active;
};

struct le_wake_event {
  uint64_t detection_sample;
  float score;
  float vad_score;
  int playback_active;
  /*
   * Events cross a nonblocking in-process pipe. Keep the message
   * self-contained rather than copying a process-local pointer.
   */
  char model_name[24];
};

struct le_wake_worker_metrics {
  uint64_t scores;
  uint64_t events;
  uint64_t dropped_blocks;
  uint64_t near_misses;
  float max_score;
  unsigned int max_inference_us;
  int failed;
};

typedef void (*le_wake_event_callback)(const struct le_wake_event *event,
                                       void *opaque);

/*
 * A near miss is a window whose sliding-window mean score reached the near-miss
 * floor with speech present but stayed under the accept cutoff. Off unless a
 * floor is set with le_wake_worker_set_near_miss().
 */
struct le_wake_near_miss {
  uint64_t detection_sample;
  float score;
  float accept_threshold;
  float vad_score;
  int playback_active;
};

/* `samples` holds the most recent mono S16 PCM, oldest first, `count` samples
 * long (at most LE_WAKE_HISTORY_SAMPLES). It is valid only for the duration of
 * the call and is delivered on the inference thread. */
typedef void (*le_wake_near_miss_callback)(const struct le_wake_near_miss *miss,
                                           const int16_t *samples, size_t count,
                                           void *opaque);

/* Most recent PCM the worker keeps for a near-miss clip: 16 blocks of 80 ms,
 * 1.28 s at 16 kHz. */
#define LE_WAKE_HISTORY_SAMPLES 20480U

/* The owner-visible sensitivity (0..100). Each model's manifest probability
 * cutoff is the tuned operating point, so 50 runs that cutoff unchanged and
 * the ends move it by 0.2. */
#define LE_WAKE_DEFAULT_SENSITIVITY 50
int le_wake_worker_start(struct le_wake_worker *worker,
                         const char *model_directory, unsigned int threads,
                         int sensitivity, le_wake_event_callback callback,
                         void *callback_opaque);

/*
 * Submit continuous post-AEC mono PCM. This call only copies into a bounded
 * queue and never waits for engine inference.
 */
/*
 * As le_wake_worker_start, and also runs a second wake word directory that
 * shares the first word's mel and embedding features. Each word decodes on
 * its own, and a detection names the word that fired in its event. A NULL
 * second_classifier is the single-word worker. second_name must be a valid
 * model name (1..64 characters from [A-Za-z0-9._-]); only its first 23
 * characters reach the event.
 */
int le_wake_worker_start_words(struct le_wake_worker *worker,
                               const char *model_directory,
                               unsigned int threads, int sensitivity,
                               const char *second_classifier,
                               const char *second_name,
                               le_wake_event_callback callback,
                               void *callback_opaque);

int le_wake_worker_submit(struct le_wake_worker *worker, const int16_t *samples,
                          size_t count,
                          const struct le_wake_observation *observation);

/*
 * Enable near-miss reporting. A floor of 0 disables it. A non-zero floor must
 * lie in (0, 1); it is compared against the live accept threshold on each
 * window, so a floor at or above the accept threshold reports nothing.
 */
int le_wake_worker_set_near_miss(struct le_wake_worker *worker, float floor,
                                 le_wake_near_miss_callback callback,
                                 void *callback_opaque);

/* Apply a new owner sensitivity to the running models without a reload. */
int le_wake_worker_set_sensitivity(struct le_wake_worker *worker,
                                   int sensitivity);

/* Live dropped-block and worst-inference counters for the status line. */
int le_wake_worker_status(struct le_wake_worker *worker,
                          uint64_t *dropped_blocks,
                          unsigned int *max_inference_us);

/* Reload on the existing inference thread using owned startup settings and
 * the live sensitivity. Submit/sensitivity/reload/stop have one control
 * owner. Success discards old queued/partial PCM and decoder history;
 * failure keeps the current engine. A timed-out load cannot publish a late
 * replacement. */
int le_wake_worker_reload(struct le_wake_worker *worker);

/* Safe live observation; ages are monotonic and -1 until inference advances. */
int le_wake_worker_health(struct le_wake_worker *worker, int *inference_age_ms);

void le_wake_worker_stop(struct le_wake_worker *worker,
                         struct le_wake_worker_metrics *metrics);

#endif
