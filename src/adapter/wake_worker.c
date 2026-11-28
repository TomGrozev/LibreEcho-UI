#define _POSIX_C_SOURCE 200809L

#include "wake_worker.h"

#include "json.h"
#include "wake_engine.h"
#include "wake_health.h"
#include "wake_model.h"
#include <math.h>
#include <time.h>

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * ESPHome's MIN_SLICES_BEFORE_DETECTION counts 10 ms feature slices, so the
 * refractory is one second of audio whose window mean stays below the cutoff.
 */
#define WAKE_IGNORE_SAMPLES (100U * 160U)
/* The vad model vetoes a word unless its window mean reaches this. */
#define WAKE_VAD_ACCEPT 0.5f
/* Sensitivity 50 is the manifest cutoff; each point moves it 0.004. */
#define WAKE_SENSITIVITY_MID 50
#define WAKE_SENSITIVITY_STEP 0.004f

#define WAKE_BLOCK_SAMPLES 1280U
#define WAKE_QUEUE_BLOCKS 8U
#define WAKE_MODEL_DIRECTORY_BYTES 1024U
#define WAKE_MAX_WORDS 2U
#define WAKE_RELOAD_SECONDS 5
#define WAKE_LOCKOUT_SAMPLES 28000ULL
#define WAKE_HISTORY_BLOCKS 16U

struct wake_block {
  int16_t samples[WAKE_BLOCK_SAMPLES];
  struct le_wake_observation observation;
};

struct wake_decoder {
  /* Recent PCM for near-miss clips, owned by the inference thread. */
  int16_t history[WAKE_HISTORY_BLOCKS][WAKE_BLOCK_SAMPLES];
  unsigned int history_next;
  unsigned int history_fill;
  uint64_t near_miss_lockout_until_sample;
  /* Refractory: below-cutoff samples still owed before the word re-arms. */
  uint64_t ignore_low_samples;
};

/* One wake word: its owner-visible name, and its own decoder state over the
 * scores of its classifier. */
struct wake_word {
  /* The stock model is alexa; a model folder reports its own name. */
  char name[LE_WAKE_MODEL_NAME_MAX];
  /* The manifest cutoff this model was trained with. */
  float manifest_cutoff;
  /* The cutoff in force after the current sensitivity is applied. */
  float cutoff;
  struct wake_decoder decoder;
};

struct wake_worker_impl {
  struct le_wake_engine *engine;
  char model_directory[WAKE_MODEL_DIRECTORY_BYTES];
  /* Optional second classifier file, run on the first word's features. */
  char second_classifier[WAKE_MODEL_DIRECTORY_BYTES];
  int has_second;
  struct wake_word words[WAKE_MAX_WORDS];
  unsigned int threads;
  pthread_t thread;
  pthread_mutex_t mutex;
  pthread_cond_t condition;
  struct wake_block queue[WAKE_QUEUE_BLOCKS];
  size_t read_position;
  size_t write_position;
  size_t queued;
  int stopping;
  int thread_started;
  int inference_failed;
  int reload_requested;
  int reload_cancelled;
  int reload_result;
  uint64_t last_inference_ns;

  int16_t accumulating[WAKE_BLOCK_SAMPLES];
  size_t accumulated;
  struct le_wake_observation accumulated_observation;

  struct le_wake_worker_metrics metrics;
  int sensitivity;
  le_wake_event_callback callback;
  void *callback_opaque;
  float near_miss_floor;
  le_wake_near_miss_callback near_miss_callback;
  void *near_miss_opaque;
  int16_t snapshot[LE_WAKE_HISTORY_SAMPLES];
};

/* The history ring and the snapshot must agree on capacity. */
typedef char
    wake_history_capacity_check[(WAKE_HISTORY_BLOCKS * WAKE_BLOCK_SAMPLES ==
                                 LE_WAKE_HISTORY_SAMPLES)
                                    ? 1
                                    : -1];

static void push_history(struct wake_decoder *decoder,
                         const struct wake_block *block) {
  memcpy(decoder->history[decoder->history_next], block->samples,
         sizeof(block->samples));
  decoder->history_next = (decoder->history_next + 1U) % WAKE_HISTORY_BLOCKS;
  if (decoder->history_fill < WAKE_HISTORY_BLOCKS)
    ++decoder->history_fill;
}

/* Copy the retained history oldest-first into the worker's snapshot buffer. */
static size_t history_snapshot(struct wake_worker_impl *worker) {
  /* The PCM history is shared by every word; it lives in the first one. */
  struct wake_decoder *decoder = &worker->words[0].decoder;
  unsigned int blocks = decoder->history_fill;
  unsigned int start = (decoder->history_next + WAKE_HISTORY_BLOCKS - blocks) %
                       WAKE_HISTORY_BLOCKS;
  unsigned int i;

  for (i = 0; i < blocks; ++i) {
    memcpy(worker->snapshot + (size_t)i * WAKE_BLOCK_SAMPLES,
           decoder->history[(start + i) % WAKE_HISTORY_BLOCKS],
           sizeof(int16_t) * WAKE_BLOCK_SAMPLES);
  }
  return (size_t)blocks * WAKE_BLOCK_SAMPLES;
}

static uint64_t wake_now_ns(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    return 0;
  return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

int le_wake_worker_health(struct le_wake_worker *worker,
                          int *inference_age_ms) {
  struct wake_worker_impl *impl;
  int loaded;
  *inference_age_ms = -1;
  if (!worker || !worker->implementation)
    return 0;
  impl = worker->implementation;
  pthread_mutex_lock(&impl->mutex);
  loaded = impl->thread_started && !impl->inference_failed && !impl->stopping;
  *inference_age_ms = le_wake_age_ms(wake_now_ns(), impl->last_inference_ns);
  pthread_mutex_unlock(&impl->mutex);
  return loaded;
}

static void merge_observation(struct le_wake_observation *destination,
                              const struct le_wake_observation *source) {
  destination->detection_sample = source->detection_sample;
  if (source->vad_score > destination->vad_score)
    destination->vad_score = source->vad_score;
  if (source->vad_active)
    destination->vad_active = 1;
  if (source->playback_active)
    destination->playback_active = 1;
}

static const struct json_member *
json_member_named(const struct json_member *members, size_t count,
                  const char *name) {
  size_t i, length = strlen(name);

  for (i = 0; i < count; ++i)
    if (members[i].name_len == length && !memcmp(members[i].name, name, length))
      return &members[i];
  return NULL;
}

/*
 * The engine validates each manifest but does not expose its cutoff, so the
 * decoder reads the same probability_cutoff here. A missing or invalid
 * manifest falls back to 0.5; the engine rejects such a model anyway.
 */
static float manifest_cutoff(const char *directory) {
  char path[WAKE_MODEL_DIRECTORY_BYTES + 64];
  char text[64];
  const char *base;
  unsigned char *data = NULL;
  struct json_member members[32], micro[16];
  const struct json_member *member;
  size_t count = 0, micro_count = 0, length, size = 0;
  FILE *file;
  float cutoff = 0.5f;

  if (!directory)
    return cutoff;
  base = strrchr(directory, '/');
  base = base ? base + 1 : directory;
  if (!base[0] || snprintf(path, sizeof(path), "%s/%.*s.json", directory,
                           (int)(sizeof(path) - strlen(directory) - 7),
                           base) >= (int)sizeof(path))
    return cutoff;
  file = fopen(path, "rb");
  if (!file)
    return cutoff;
  data = malloc(LE_WAKE_MODEL_MANIFEST_MAX_BYTES + 1U);
  if (data)
    size = fread(data, 1, LE_WAKE_MODEL_MANIFEST_MAX_BYTES + 1U, file);
  fclose(file);
  if (!data || size > LE_WAKE_MODEL_MANIFEST_MAX_BYTES) {
    free(data);
    return cutoff;
  }
  data[size] = '\0';
  if (json_object_members((const char *)data, size, members,
                          sizeof(members) / sizeof(members[0]), &count) < 0) {
    free(data);
    return cutoff;
  }
  member = json_member_named(members, count, "micro");
  if (!member ||
      json_object_members(member->value, member->value_len, micro,
                          sizeof(micro) / sizeof(micro[0]), &micro_count) < 0) {
    free(data);
    return cutoff;
  }
  member = json_member_named(micro, micro_count, "probability_cutoff");
  length = member ? member->value_len : 0;
  if (member && length > 0 && length < sizeof(text)) {
    char *end;
    double value;

    memcpy(text, member->value, length);
    text[length] = '\0';
    value = strtod(text, &end);
    if (end == text + length && isfinite(value) && value > 0.0 && value <= 1.0)
      cutoff = (float)value;
  }
  free(data);
  return cutoff;
}

/* Lower cutoff means more sensitive: sensitivity 100 subtracts 0.2. */
static float sensitivity_cutoff(float manifest, int sensitivity) {
  float cutoff = manifest + (float)(WAKE_SENSITIVITY_MID - sensitivity) *
                                WAKE_SENSITIVITY_STEP;

  if (cutoff < 0.01f)
    cutoff = 0.01f;
  if (cutoff > 0.99f)
    cutoff = 0.99f;
  return cutoff;
}

static void apply_sensitivity(struct wake_worker_impl *impl) {
  unsigned int i;

  for (i = 0; i < (impl->has_second ? WAKE_MAX_WORDS : 1U); ++i)
    impl->words[i].cutoff =
        sensitivity_cutoff(impl->words[i].manifest_cutoff, impl->sensitivity);
}

int le_wake_worker_status(struct le_wake_worker *worker,
                          uint64_t *dropped_blocks,
                          unsigned int *max_inference_us) {
  struct wake_worker_impl *impl;

  if (dropped_blocks)
    *dropped_blocks = 0;
  if (max_inference_us)
    *max_inference_us = 0;
  if (!worker || !worker->implementation)
    return -1;
  impl = worker->implementation;
  pthread_mutex_lock(&impl->mutex);
  if (dropped_blocks)
    *dropped_blocks = impl->metrics.dropped_blocks;
  if (max_inference_us)
    *max_inference_us = impl->metrics.max_inference_us;
  pthread_mutex_unlock(&impl->mutex);
  return 0;
}

static void decode_word(struct wake_worker_impl *worker, struct wake_word *word,
                        const struct wake_block *block, float score,
                        float vad_score, uint64_t samples) {
  struct wake_decoder *decoder = &word->decoder;
  float near_floor;
  float cutoff;
  le_wake_near_miss_callback near_callback;
  void *near_opaque;

  /* set_sensitivity rewrites cutoff under this mutex from the control thread.
   */
  pthread_mutex_lock(&worker->mutex);
  cutoff = word->cutoff;
  near_floor = worker->near_miss_floor;
  near_callback = worker->near_miss_callback;
  near_opaque = worker->near_miss_opaque;
  if (score > worker->metrics.max_score)
    worker->metrics.max_score = score;
  pthread_mutex_unlock(&worker->mutex);

  /*
   * Refractory after start or a wake. The audio is counted in samples, not
   * inference calls: microWakeWord infers on its own 30 ms stride, so the
   * worker must not assume an 80 ms block. The word also stays disarmed
   * until its window mean falls below the cutoff once. No near miss is
   * reported while disarmed, so the utterance that woke the device cannot
   * also arrive as a near miss on its falling edge.
   */
  if (decoder->ignore_low_samples > 0) {
    if (score < cutoff) {
      if (decoder->ignore_low_samples <= samples)
        decoder->ignore_low_samples = 0;
      else
        decoder->ignore_low_samples -= samples;
    }
    return;
  }

  if (score >= cutoff && vad_score >= WAKE_VAD_ACCEPT) {
    ++worker->metrics.events;
    decoder->ignore_low_samples = WAKE_IGNORE_SAMPLES;
    if (worker->callback) {
      struct le_wake_event event = {block->observation.detection_sample, score,
                                    vad_score,
                                    block->observation.playback_active, ""};

      /* The event names the word that fired, at most 23 characters. */
      snprintf(event.model_name, sizeof(event.model_name), "%.*s",
               (int)(sizeof(event.model_name) - 1), word->name);
      worker->callback(&event, worker->callback_opaque);
    }
    return;
  }

  /*
   * Near miss: the window mean reached the floor with speech present but
   * stayed under the cutoff. One utterance yields one clip.
   */
  if (near_callback && near_floor > 0.0f && near_floor < cutoff &&
      score >= near_floor && score < cutoff && vad_score >= WAKE_VAD_ACCEPT &&
      block->observation.detection_sample >=
          decoder->near_miss_lockout_until_sample) {
    struct le_wake_near_miss miss;
    size_t count;

    decoder->near_miss_lockout_until_sample =
        block->observation.detection_sample + WAKE_LOCKOUT_SAMPLES;
    worker->metrics.near_misses++;
    miss.detection_sample = block->observation.detection_sample;
    miss.score = score;
    miss.accept_threshold = cutoff;
    miss.vad_score = vad_score;
    miss.playback_active = block->observation.playback_active;
    count = history_snapshot(worker);
    near_callback(&miss, worker->snapshot, count, near_opaque);
  }
}

static void decode_score(struct wake_worker_impl *worker,
                         const struct wake_block *block, const float *scores,
                         uint64_t samples) {
  /* An engine without a vad model cannot express "speech present", so the
   * veto would reject every wake; only veto on engines that compute it. */
  float vad_score = le_wake_engine_has_vad(worker->engine)
                        ? le_wake_engine_vad_score(worker->engine)
                        : 1.0f;
  unsigned int i;

  ++worker->metrics.scores;
  for (i = 0; i < (worker->has_second ? WAKE_MAX_WORDS : 1U); ++i)
    decode_word(worker, &worker->words[i], block, scores[i], vad_score,
                samples);
}

static void *wake_thread(void *opaque) {
  struct wake_worker_impl *worker = opaque;
  unsigned int i;
  /* Audio fed since the last score. microWakeWord runs an inference per 30 ms
   * but reports only the newest score per fed block, so this is the only
   * measure of how much audio one decode call stands for. */
  uint64_t unscored_samples = 0;

  for (;;) {
    struct wake_block block;
    float scores[WAKE_MAX_WORDS] = {0.0f, 0.0f};
    int new_score = 0;
    unsigned int inference_us;

    pthread_mutex_lock(&worker->mutex);
    while ((worker->queued == 0 || worker->inference_failed) &&
           !worker->stopping && !worker->reload_requested)
      pthread_cond_wait(&worker->condition, &worker->mutex);
    if (worker->reload_requested) {
      struct le_wake_engine *replacement, *retired;

      /* No feed can compete with model initialization/warmup: this is
       * the same inference thread. Retain the old engine until load
       * succeeds, including on a cancelled/late initialization. */
      pthread_mutex_unlock(&worker->mutex);
      replacement = le_wake_engine_create_words(
          worker->model_directory, worker->threads,
          worker->has_second ? worker->second_classifier : NULL);
      pthread_mutex_lock(&worker->mutex);
      retired = replacement;
      worker->reload_result = -1;
      if (replacement && !worker->reload_cancelled && !worker->stopping) {
        retired = worker->engine;
        worker->engine = replacement;
        worker->read_position = worker->write_position = worker->queued = 0;
        worker->accumulated = 0;
        memset(&worker->accumulated_observation, 0,
               sizeof(worker->accumulated_observation));
        for (i = 0; i < WAKE_MAX_WORDS; ++i)
          memset(&worker->words[i].decoder, 0,
                 sizeof(worker->words[i].decoder));
        apply_sensitivity(worker);
        worker->inference_failed = 0;
        worker->last_inference_ns = 0;
        worker->reload_result = 0;
      }
      pthread_mutex_unlock(&worker->mutex);
      if (retired)
        le_wake_engine_destroy(retired);
      pthread_mutex_lock(&worker->mutex);
      worker->reload_requested = 0;
      pthread_cond_broadcast(&worker->condition);
      pthread_mutex_unlock(&worker->mutex);
      unscored_samples = 0;
      continue;
    }
    if ((worker->queued == 0 || worker->inference_failed) && worker->stopping) {
      pthread_mutex_unlock(&worker->mutex);
      break;
    }
    block = worker->queue[worker->read_position];
    worker->read_position = (worker->read_position + 1U) % WAKE_QUEUE_BLOCKS;
    --worker->queued;
    pthread_mutex_unlock(&worker->mutex);

    if (le_wake_engine_feed_words(worker->engine, block.samples,
                                  WAKE_BLOCK_SAMPLES, scores, WAKE_MAX_WORDS,
                                  &new_score) < 0 ||
        !new_score) {
      worker->metrics.failed = 1;
      pthread_mutex_lock(&worker->mutex);
      worker->inference_failed = 1;
      pthread_mutex_unlock(&worker->mutex);
      /* Keep the existing worker dormant so a later reload can recover
       * without adding a second inference thread. No more PCM is fed
       * to the failed engine unless reinitialization succeeds. */
      unscored_samples += WAKE_BLOCK_SAMPLES;
      continue;
    }
    inference_us = le_wake_engine_last_inference_us(worker->engine);
    pthread_mutex_lock(&worker->mutex);
    worker->last_inference_ns = wake_now_ns();
    if (inference_us > worker->metrics.max_inference_us)
      worker->metrics.max_inference_us = inference_us;
    pthread_mutex_unlock(&worker->mutex);
    push_history(&worker->words[0].decoder, &block);
    unscored_samples += WAKE_BLOCK_SAMPLES;
    decode_score(worker, &block, scores, unscored_samples);
    unscored_samples = 0;
  }
  return NULL;
}

int le_wake_worker_start(struct le_wake_worker *worker,
                         const char *model_directory, unsigned int threads,
                         int sensitivity, le_wake_event_callback callback,
                         void *callback_opaque) {
  return le_wake_worker_start_words(worker, model_directory, threads,
                                    sensitivity, NULL, NULL, callback,
                                    callback_opaque);
}

int le_wake_worker_start_words(struct le_wake_worker *worker,
                               const char *model_directory,
                               unsigned int threads, int sensitivity,
                               const char *second_classifier,
                               const char *second_name,
                               le_wake_event_callback callback,
                               void *callback_opaque) {
  struct wake_worker_impl *impl;
  pthread_condattr_t condition_attributes;

  if (!worker || worker->implementation || !model_directory ||
      strnlen(model_directory, WAKE_MODEL_DIRECTORY_BYTES) >=
          WAKE_MODEL_DIRECTORY_BYTES ||
      sensitivity < 0 || sensitivity > 100)
    return -1;
  if (second_classifier &&
      (!second_name || !le_wake_model_name_valid(second_name) ||
       strnlen(second_classifier, WAKE_MODEL_DIRECTORY_BYTES) >=
           WAKE_MODEL_DIRECTORY_BYTES))
    return -1;
  impl = calloc(1, sizeof(*impl));
  if (!impl)
    return -1;
  memcpy(impl->model_directory, model_directory, strlen(model_directory) + 1);
  {
    char phrase[LE_WAKE_MODEL_NAME_MAX];

    if (le_wake_model_label(model_directory, phrase, sizeof(phrase),
                            impl->words[0].name,
                            sizeof(impl->words[0].name)) < 0)
      snprintf(impl->words[0].name, sizeof(impl->words[0].name), "%s",
               LE_WAKE_MODEL_STOCK_NAME);
  }
  if (second_classifier) {
    memcpy(impl->second_classifier, second_classifier,
           strlen(second_classifier) + 1);
    snprintf(impl->words[1].name, sizeof(impl->words[1].name), "%s",
             second_name);
    impl->has_second = 1;
  }
  impl->threads = threads;
  impl->sensitivity = sensitivity;
  impl->words[0].manifest_cutoff = manifest_cutoff(model_directory);
  impl->words[0].decoder.ignore_low_samples = WAKE_IGNORE_SAMPLES;
  if (impl->has_second) {
    impl->words[1].manifest_cutoff = manifest_cutoff(second_classifier);
    impl->words[1].decoder.ignore_low_samples = WAKE_IGNORE_SAMPLES;
  }
  apply_sensitivity(impl);
  impl->callback = callback;
  impl->callback_opaque = callback_opaque;
  if (pthread_mutex_init(&impl->mutex, NULL) != 0)
    goto fail;
  if (pthread_condattr_init(&condition_attributes) != 0)
    goto fail_mutex;
  if (pthread_condattr_setclock(&condition_attributes, CLOCK_MONOTONIC) != 0) {
    pthread_condattr_destroy(&condition_attributes);
    goto fail_mutex;
  }
  if (pthread_cond_init(&impl->condition, &condition_attributes) != 0) {
    pthread_condattr_destroy(&condition_attributes);
    goto fail_mutex;
  }
  pthread_condattr_destroy(&condition_attributes);
  impl->engine = le_wake_engine_create_words(
      model_directory, threads,
      impl->has_second ? impl->second_classifier : NULL);
  if (!impl->engine)
    goto fail_condition;
  if (pthread_create(&impl->thread, NULL, wake_thread, impl) != 0)
    goto fail_engine;
  impl->thread_started = 1;
  worker->implementation = impl;
  return 0;

fail_engine:
  le_wake_engine_destroy(impl->engine);
fail_condition:
  pthread_cond_destroy(&impl->condition);
fail_mutex:
  pthread_mutex_destroy(&impl->mutex);
fail:
  free(impl);
  return -1;
}

int le_wake_worker_submit(struct le_wake_worker *worker, const int16_t *samples,
                          size_t count,
                          const struct le_wake_observation *observation) {
  struct wake_worker_impl *impl;
  size_t offset = 0;
  int dropped = 0;

  if (!worker || !worker->implementation || !samples || !observation)
    return -1;
  impl = worker->implementation;
  while (offset < count) {
    size_t copying = WAKE_BLOCK_SAMPLES - impl->accumulated;

    if (copying > count - offset)
      copying = count - offset;
    memcpy(impl->accumulating + impl->accumulated, samples + offset,
           copying * sizeof(samples[0]));
    merge_observation(&impl->accumulated_observation, observation);
    impl->accumulated += copying;
    offset += copying;
    if (impl->accumulated == WAKE_BLOCK_SAMPLES) {
      pthread_mutex_lock(&impl->mutex);
      if (impl->queued == WAKE_QUEUE_BLOCKS) {
        ++impl->metrics.dropped_blocks;
        dropped = 1;
      } else {
        struct wake_block *block = &impl->queue[impl->write_position];

        memcpy(block->samples, impl->accumulating, sizeof(block->samples));
        block->observation = impl->accumulated_observation;
        impl->write_position = (impl->write_position + 1U) % WAKE_QUEUE_BLOCKS;
        ++impl->queued;
        pthread_cond_signal(&impl->condition);
      }
      pthread_mutex_unlock(&impl->mutex);
      impl->accumulated = 0;
      memset(&impl->accumulated_observation, 0,
             sizeof(impl->accumulated_observation));
    }
  }
  return dropped;
}

int le_wake_worker_set_near_miss(struct le_wake_worker *worker, float floor,
                                 le_wake_near_miss_callback callback,
                                 void *callback_opaque) {
  struct wake_worker_impl *impl;

  if (!worker || !worker->implementation || !(floor >= 0.0f && floor < 1.0f) ||
      (floor > 0.0f && !callback))
    return -1;
  impl = worker->implementation;
  pthread_mutex_lock(&impl->mutex);
  impl->near_miss_floor = floor;
  impl->near_miss_callback = floor > 0.0f ? callback : NULL;
  impl->near_miss_opaque = callback_opaque;
  pthread_mutex_unlock(&impl->mutex);
  return 0;
}

int le_wake_worker_set_sensitivity(struct le_wake_worker *worker,
                                   int sensitivity) {
  struct wake_worker_impl *impl;

  if (!worker || !worker->implementation || sensitivity < 0 ||
      sensitivity > 100)
    return -1;
  impl = worker->implementation;
  pthread_mutex_lock(&impl->mutex);
  impl->sensitivity = sensitivity;
  apply_sensitivity(impl);
  pthread_mutex_unlock(&impl->mutex);
  return 0;
}

int le_wake_worker_reload(struct le_wake_worker *worker) {
  struct wake_worker_impl *impl;
  struct timespec deadline;
  int result;

  if (!worker || !worker->implementation ||
      clock_gettime(CLOCK_MONOTONIC, &deadline) != 0)
    return -1;
  deadline.tv_sec += WAKE_RELOAD_SECONDS;
  impl = worker->implementation;
  pthread_mutex_lock(&impl->mutex);
  if (!impl->thread_started || impl->stopping || impl->reload_requested) {
    pthread_mutex_unlock(&impl->mutex);
    return -1;
  }
  impl->reload_requested = 1;
  impl->reload_cancelled = 0;
  impl->reload_result = -1;
  pthread_cond_signal(&impl->condition);
  while (impl->reload_requested) {
    if (pthread_cond_timedwait(&impl->condition, &impl->mutex, &deadline) !=
            0 &&
        impl->reload_requested) {
      /* Publication is already irrevocable once reload_result is zero.
       * Slow retirement must not turn a committed success into a failure. */
      if (impl->reload_result == 0) {
        pthread_mutex_unlock(&impl->mutex);
        return 0;
      }
      /* Engine initialization itself cannot be interrupted. Cancel its
       * publication, retain the current model and refuse another load
       * until this one retires on the worker. */
      impl->reload_cancelled = 1;
      pthread_mutex_unlock(&impl->mutex);
      return -1;
    }
  }
  result = impl->reload_result;
  pthread_mutex_unlock(&impl->mutex);
  return result;
}

void le_wake_worker_stop(struct le_wake_worker *worker,
                         struct le_wake_worker_metrics *metrics) {
  struct wake_worker_impl *impl;

  if (metrics)
    memset(metrics, 0, sizeof(*metrics));
  if (!worker || !worker->implementation)
    return;
  impl = worker->implementation;
  pthread_mutex_lock(&impl->mutex);
  impl->stopping = 1;
  pthread_cond_signal(&impl->condition);
  pthread_mutex_unlock(&impl->mutex);
  if (impl->thread_started)
    pthread_join(impl->thread, NULL);
  if (metrics)
    *metrics = impl->metrics;
  le_wake_engine_destroy(impl->engine);
  pthread_cond_destroy(&impl->condition);
  pthread_mutex_destroy(&impl->mutex);
  free(impl);
  worker->implementation = NULL;
}
