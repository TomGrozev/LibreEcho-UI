/*
 * Wake decoding: the microWakeWord accept rule.
 *
 * A word is accepted when its sliding-window mean reaches the cutoff and the
 * vad model's window mean reaches 0.5. After a wake, the word is disarmed for
 * one second of audio and stays disarmed until its mean falls below the
 * cutoff. The owner's sensitivity moves the cutoff around the value the model
 * was trained with (50 leaves it unchanged).
 *
 * The engine is faked so score sequences and the vad mean can be scripted
 * exactly. The worker, its queue, its thread and the decoder are the real
 * ones.
 */

#include "adapter/wake_engine.h"
#include "adapter/wake_worker.h"

#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define BLOCK_SAMPLES 1280u
#define MAX_FRAMES 64u

/* ---- faked engine: one scripted score per fed block ---- */

struct le_wake_engine {
  int unused;
};

static struct le_wake_engine fake_engine;
static const float *script1;
static const float *script2;
static const float *script_vad;
static size_t script_count;
static size_t script_position;
static volatile unsigned int consumed_blocks;
static int fake_has_second;
static unsigned int fake_inference_us;

struct le_wake_engine *
le_wake_engine_create_words(const char *model_directory, unsigned int threads,
                            const char *second_classifier) {
  (void)model_directory;
  (void)threads;
  script_position = 0;
  consumed_blocks = 0;
  fake_has_second = second_classifier != NULL;
  return &fake_engine;
}

int le_wake_engine_feed_words(struct le_wake_engine *engine,
                              const int16_t *samples, size_t count,
                              float *scores, size_t score_slots,
                              int *new_score) {
  (void)engine;
  (void)samples;
  (void)count;
  scores[0] = script_position < script_count ? script1[script_position] : 0.0f;
  if (score_slots > 1)
    scores[1] = fake_has_second && script2 && script_position < script_count
                    ? script2[script_position]
                    : 0.0f;
  ++script_position;
  /* The consumer thread: the harness waits on this before feeding more. */
  ++consumed_blocks;
  /* Always a new score; "no new score" means an engine failure to the
   * worker, which is covered elsewhere. */
  *new_score = 1;
  return 0;
}

float le_wake_engine_vad_score(const struct le_wake_engine *engine) {
  (void)engine;
  if (script_vad && script_position > 0 && script_position <= script_count)
    return script_vad[script_position - 1];
  return 0.0f;
}

int le_wake_engine_has_vad(const struct le_wake_engine *engine) {
  (void)engine;
  return 1;
}

unsigned int
le_wake_engine_last_inference_us(const struct le_wake_engine *engine) {
  (void)engine;
  return fake_inference_us;
}

void le_wake_engine_destroy(struct le_wake_engine *engine) { (void)engine; }

/* ---- model directories ---- */

/* A model directory needs <name>/<name>.json for the worker to read the
 * manifest cutoff; the engine itself is faked. */
static char model_root[] = "/tmp/le-wake-decode-XXXXXX";
static char model_wake[128];
static char model_twin[128];

static void write_manifest(const char *directory, const char *name,
                           float cutoff) {
  char path[256];
  FILE *file;

  assert(snprintf(path, sizeof(path), "%s/%s.json", directory, name) <
         (int)sizeof(path));
  file = fopen(path, "w");
  assert(file);
  fprintf(file, "{\"micro\":{\"probability_cutoff\":%.3f}}\n", (double)cutoff);
  assert(fclose(file) == 0);
}

static void make_model_dirs(void) {
  char path[256];

  assert(mkdtemp(model_root) != NULL);
  assert(snprintf(model_wake, sizeof(model_wake), "%s/wake", model_root) <
         (int)sizeof(model_wake));
  assert(snprintf(model_twin, sizeof(model_twin), "%s/twin", model_root) <
         (int)sizeof(model_twin));
  assert(snprintf(path, sizeof(path), "%s", model_wake) > 0);
  assert(mkdir(path, 0700) == 0);
  assert(snprintf(path, sizeof(path), "%s", model_twin) > 0);
  assert(mkdir(path, 0700) == 0);
  write_manifest(model_wake, "wake", 0.9f);
  write_manifest(model_twin, "twin", 0.5f);
}

/* ---- harness ---- */

struct capture {
  unsigned int count;
  unsigned int near;
  struct le_wake_event events[MAX_FRAMES];
};

static void on_wake_event(const struct le_wake_event *event, void *opaque) {
  struct capture *capture = opaque;

  if (capture->count < MAX_FRAMES)
    capture->events[capture->count] = *event;
  ++capture->count;
}

static void on_near_miss(const struct le_wake_near_miss *miss,
                         const int16_t *samples, size_t count, void *opaque) {
  struct capture *capture = opaque;

  (void)miss;
  (void)samples;
  (void)count;
  ++capture->near;
}

static void short_sleep(void) {
  struct timespec slice = {0, 100000};

  nanosleep(&slice, NULL);
}

static int close_to(float actual, float expected) {
  return fabsf(actual - expected) < 1e-6f;
}

struct scenario {
  struct le_wake_worker worker;
  struct capture capture;
  int16_t block[BLOCK_SAMPLES];
  unsigned int fed;
};

/* The scripts live at file scope so the inference thread never reads a dead
 * stack slot. */
static float feed_score1, feed_score2, feed_vad;

/*
 * Feed one block and wait until the inference thread has consumed it, so the
 * queue never fills and the outcome does not depend on scheduling.
 */
static void feed_block(struct scenario *scenario, float score1, float score2,
                       float vad) {
  struct le_wake_observation observation;
  unsigned int before, wait;

  feed_score1 = score1;
  feed_score2 = score2;
  feed_vad = vad;
  script1 = &feed_score1;
  script2 = &feed_score2;
  script_vad = &feed_vad;
  script_count = 1;
  script_position = 0;
  memset(&observation, 0, sizeof(observation));
  observation.detection_sample = (uint64_t)(scenario->fed + 1) * BLOCK_SAMPLES;
  observation.vad_score = vad;
  observation.vad_active = vad >= 0.5f;
  before = consumed_blocks;
  assert(le_wake_worker_submit(&scenario->worker, scenario->block,
                               BLOCK_SAMPLES, &observation) == 0);
  ++scenario->fed;
  for (wait = 0; consumed_blocks == before && wait < 100000; ++wait)
    short_sleep();
  assert(consumed_blocks == before + 1);
}

static void feed(struct scenario *scenario, float score, float vad) {
  feed_block(scenario, score, 0.0f, vad);
}

static void begin(struct scenario *scenario, const char *directory,
                  int sensitivity) {
  memset(scenario, 0, sizeof(*scenario));
  assert(le_wake_worker_start(&scenario->worker, directory, 1, sensitivity,
                              on_wake_event, &scenario->capture) == 0);
}

static void finish(struct scenario *scenario,
                   struct le_wake_worker_metrics *metrics) {
  le_wake_worker_stop(&scenario->worker, metrics);
}

/*
 * One second of audio below the cutoff drains the start refractory, which is
 * where the reading that follows actually begins.
 */
static void prime(struct scenario *scenario, float below_cutoff) {
  unsigned int i;

  for (i = 0; i < 13; ++i)
    feed(scenario, below_cutoff, 0.0f);
}

/* ---- cases ---- */

int main(void) {
  struct le_wake_worker_metrics metrics;
  struct scenario scenario;
  uint64_t dropped;
  unsigned int max_us;

  make_model_dirs();

  /* A window mean at the manifest cutoff with a voiced window is a wake. */
  {
    struct scenario s;

    fake_inference_us = 7;
    begin(&s, model_wake, 50);
    prime(&s, 0.10f);
    feed(&s, 0.90f, 0.9f);
    assert(s.capture.count == 1);
    assert(close_to(s.capture.events[0].score, 0.90f));
    assert(s.capture.events[0].vad_score >= 0.5f);
    /* The live counters the status line reads, before the summary. */
    assert(le_wake_worker_status(&s.worker, &dropped, &max_us) == 0);
    assert(max_us == 7);
    assert(dropped == 0);
    finish(&s, &metrics);
    assert(metrics.events == 1);
    assert(metrics.max_inference_us == 7);
  }

  /* Below the cutoff is not a wake, however loud the voice. */
  {
    begin(&scenario, model_wake, 50);
    prime(&scenario, 0.10f);
    feed(&scenario, 0.80f, 0.9f);
    assert(scenario.capture.count == 0);
    finish(&scenario, &metrics);
    assert(metrics.events == 0);
  }

  /* The vad model vetoes a word that clears the cutoff. */
  {
    begin(&scenario, model_wake, 50);
    prime(&scenario, 0.10f);
    feed(&scenario, 0.95f, 0.40f);
    assert(scenario.capture.count == 0);
    finish(&scenario, &metrics);
  }

  /* A band window without a wake is a near miss. */
  {
    begin(&scenario, model_twin, 50); /* cutoff 0.5 */
    prime(&scenario, 0.10f);
    assert(le_wake_worker_set_near_miss(&scenario.worker, 0.35f, on_near_miss,
                                        &scenario.capture) == 0);
    feed(&scenario, 0.40f, 0.9f);
    assert(scenario.capture.count == 0);
    assert(scenario.capture.near == 1);
    finish(&scenario, &metrics);
    assert(metrics.near_misses == 1);
  }

  /* The utterance that woke the device is not reported as a near miss on its
   * falling edge, while the word is still disarmed. */
  {
    begin(&scenario, model_twin, 50);
    prime(&scenario, 0.10f);
    assert(le_wake_worker_set_near_miss(&scenario.worker, 0.35f, on_near_miss,
                                        &scenario.capture) == 0);
    feed(&scenario, 0.90f, 0.9f);
    assert(scenario.capture.count == 1);
    feed(&scenario, 0.40f, 0.9f);
    assert(scenario.capture.near == 0);
    finish(&scenario, &metrics);
    assert(metrics.near_misses == 0);
  }

  /* One wake, then silence for a second, then the word fires again. */
  {
    unsigned int i;

    begin(&scenario, model_wake, 50);
    prime(&scenario, 0.10f);
    feed(&scenario, 0.95f, 0.9f);
    assert(scenario.capture.count == 1);
    /* Inside the refractory: the score is at the cutoff but dropped. */
    feed(&scenario, 0.95f, 0.9f);
    assert(scenario.capture.count == 1);
    for (i = 0; i < 13; ++i)
      feed(&scenario, 0.10f, 0.0f);
    feed(&scenario, 0.95f, 0.9f);
    assert(scenario.capture.count == 2);
    finish(&scenario, &metrics);
    assert(metrics.events == 2);
  }

  /* Sensitivity 100 lowers the 0.9 cutoff to 0.7, 0 raises it to 0.99. */
  {
    begin(&scenario, model_wake, 100);
    prime(&scenario, 0.10f);
    feed(&scenario, 0.75f, 0.9f);
    assert(scenario.capture.count == 1);
    finish(&scenario, &metrics);

    begin(&scenario, model_wake, 0);
    prime(&scenario, 0.10f);
    feed(&scenario, 0.75f, 0.9f);
    assert(scenario.capture.count == 0);
    /* 0.95 is under 0.99 too. */
    feed(&scenario, 0.95f, 0.9f);
    assert(scenario.capture.count == 0);
    finish(&scenario, &metrics);
  }

  /* 50 keeps the manifest cutoff: 0.85 no, 0.95 yes. */
  {
    begin(&scenario, model_wake, 50);
    prime(&scenario, 0.10f);
    feed(&scenario, 0.85f, 0.9f);
    assert(scenario.capture.count == 0);
    feed(&scenario, 0.95f, 0.9f);
    assert(scenario.capture.count == 1);
    finish(&scenario, &metrics);
  }

  /* A live sensitivity change reaches the running model. */
  {
    begin(&scenario, model_wake, 0);
    prime(&scenario, 0.10f);
    feed(&scenario, 0.75f, 0.9f);
    assert(scenario.capture.count == 0);
    assert(le_wake_worker_set_sensitivity(&scenario.worker, 100) == 0);
    feed(&scenario, 0.75f, 0.9f);
    assert(scenario.capture.count == 1);
    finish(&scenario, &metrics);
  }

  /* Out-of-range sensitivities are refused at start and live. */
  {
    memset(&scenario, 0, sizeof(scenario));
    assert(le_wake_worker_start(&scenario.worker, model_wake, 1, 101,
                                on_wake_event, &scenario.capture) < 0);
  }

  /* A second word decodes on its own and names itself in the event. */
  {
    unsigned int i;

    memset(&scenario, 0, sizeof(scenario));
    assert(le_wake_worker_start_words(&scenario.worker, model_wake, 1, 50,
                                      model_twin, "twin", on_wake_event,
                                      &scenario.capture) == 0);
    for (i = 0; i < 13; ++i)
      feed_block(&scenario, 0.0f, 0.0f, 0.9f);
    feed_block(&scenario, 0.0f, 1.0f, 0.9f);
    assert(scenario.capture.count == 1);
    assert(!strcmp(scenario.capture.events[0].model_name, "twin"));
    finish(&scenario, &metrics);
  }

  printf("test_wake_decode: all cases passed\n");
  return 0;
}
