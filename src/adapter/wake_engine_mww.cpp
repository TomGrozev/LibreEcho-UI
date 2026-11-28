/*
 * microWakeWord engine for waked.
 *
 * One word directory holds <name>.json and the .tflite it names; the VAD model
 * lives beside it in <root>/vad/.  The audio frontend is the TFLite Micro
 * microfrontend with the settings ESPHome's preprocessor_settings.h uses.
 */

#include "wake_engine.h"

#include <dirent.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "microwakeword/operators.h"
#include "tensorflow/lite/experimental/microfrontend/lib/frontend.h"
#include "tensorflow/lite/experimental/microfrontend/lib/frontend_util.h"
#include "tensorflow/lite/micro/micro_allocator.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_resource_variable.h"
#include "tensorflow/lite/micro/schema/schema_generated.h"

namespace {

constexpr std::size_t kFeatureChannels = 40;
constexpr std::size_t kStepSamples = 160;   /* 10 ms at 16 kHz */
constexpr double kFeatureScale = 0.0390625; /* pymicro-features FLOAT32_SCALE */
constexpr std::size_t kResourceVariables = 8;

struct WordModel {
  std::string directory;
  std::string wake_word;
  float probability_cutoff = 0.0f;
  std::size_t sliding_window_size = 0;
  std::size_t feature_step_size = 0;
  std::size_t tensor_arena_size = 0;
  std::size_t stride = 0;

  std::vector<uint8_t> bytes;
  std::vector<uint8_t> arena;
  std::unique_ptr<tflite::MicroMutableOpResolver<
      libreecho::microwakeword::kSupportedOperatorCount>>
      resolver;
  tflite::MicroAllocator *allocator = nullptr;
  tflite::MicroResourceVariables *variables = nullptr;
  tflite::MicroInterpreter *interpreter = nullptr;
  TfLiteTensor *input = nullptr;
  const TfLiteTensor *output = nullptr;
  float input_scale = 0.0f;
  int input_zero_point = 0;
  float output_scale = 0.0f;
  int output_zero_point = 0;

  ~WordModel() { delete interpreter; }
};

bool ReadFile(const std::string &path, std::vector<uint8_t> *bytes) {
  std::FILE *file = std::fopen(path.c_str(), "rb");

  if (!file)
    return false;
  if (std::fseek(file, 0, SEEK_END) != 0) {
    std::fclose(file);
    return false;
  }
  const long size = std::ftell(file);
  if (size < 0 || std::fseek(file, 0, SEEK_SET) != 0) {
    std::fclose(file);
    return false;
  }
  bytes->resize(static_cast<std::size_t>(size));
  const std::size_t read =
      bytes->empty() ? 0 : std::fread(bytes->data(), 1, bytes->size(), file);
  std::fclose(file);
  return read == bytes->size();
}

bool ReadTextFile(const std::string &path, std::string *text) {
  std::vector<uint8_t> bytes;

  if (!ReadFile(path, &bytes))
    return false;
  text->assign(bytes.begin(), bytes.end());
  return true;
}

/* The manifests are small and flat enough that the named fields can be read
 * without a JSON library.  Every field this engine needs is unique in the
 * document. */
bool JsonField(const std::string &document, const char *key, std::string *value,
               bool *is_string) {
  const std::string needle = std::string("\"") + key + "\"";
  std::size_t at = document.find(needle);

  if (at == std::string::npos)
    return false;
  at = document.find(':', at + needle.size());
  if (at == std::string::npos)
    return false;
  ++at;
  while (at < document.size() && (document[at] == ' ' || document[at] == '\t' ||
                                  document[at] == '\r' || document[at] == '\n'))
    ++at;
  if (at >= document.size())
    return false;
  *is_string = document[at] == '"';
  if (*is_string) {
    const std::size_t end = document.find('"', at + 1);

    if (end == std::string::npos)
      return false;
    *value = document.substr(at + 1, end - at - 1);
    return true;
  }
  const std::size_t end = document.find_first_of(",}\r\n", at);

  if (end == std::string::npos)
    return false;
  *value = document.substr(at, end - at);
  return true;
}

bool JsonString(const std::string &document, const char *key,
                std::string *value) {
  bool is_string = false;

  return JsonField(document, key, value, &is_string) && is_string;
}

bool JsonNumber(const std::string &document, const char *key, double *value) {
  std::string text;
  bool is_string = false;
  char *end = nullptr;

  if (!JsonField(document, key, &text, &is_string) || is_string)
    return false;
  *value = std::strtod(text.c_str(), &end);
  return end != text.c_str();
}

std::string DirectoryOf(const std::string &path) {
  std::string trimmed = path;

  while (trimmed.size() > 1 && trimmed.back() == '/')
    trimmed.pop_back();
  const std::size_t slash = trimmed.rfind('/');

  if (slash == std::string::npos)
    return ".";
  if (slash == 0)
    return "/";
  return trimmed.substr(0, slash);
}

std::string JoinPath(const std::string &directory, const std::string &name) {
  if (directory.empty() || directory == ".")
    return name;
  if (directory.back() == '/')
    return directory + name;
  return directory + "/" + name;
}

bool FindManifest(const std::string &directory, std::string *path) {
  DIR *dir = opendir(directory.c_str());
  std::string found;
  int count = 0;

  if (!dir)
    return false;
  while (struct dirent *entry = readdir(dir)) {
    const std::string name = entry->d_name;
    const std::size_t length = name.size();

    if (name.empty() || name[0] == '.' || length < 5 ||
        name.compare(length - 5, 5, ".json") != 0)
      continue;
    found = JoinPath(directory, name);
    ++count;
  }
  closedir(dir);
  if (count != 1)
    return false;
  *path = found;
  return true;
}

bool LoadManifest(const std::string &directory, WordModel *model) {
  std::string manifest;
  std::string model_file;
  std::string manifest_path;
  double number = 0.0;

  if (!FindManifest(directory, &manifest_path) ||
      !ReadTextFile(manifest_path, &manifest)) {
    std::fprintf(stderr, "wake: no single model manifest in %s\n",
                 directory.c_str());
    return false;
  }
  if (!JsonString(manifest, "wake_word", &model->wake_word) ||
      !JsonString(manifest, "model", &model_file) ||
      !JsonNumber(manifest, "probability_cutoff", &number)) {
    std::fprintf(stderr, "wake: manifest %s is missing a field\n",
                 manifest_path.c_str());
    return false;
  }
  model->probability_cutoff = static_cast<float>(number);
  if (!JsonNumber(manifest, "sliding_window_size", &number) || number < 1.0) {
    std::fprintf(stderr, "wake: manifest %s has no sliding window\n",
                 manifest_path.c_str());
    return false;
  }
  model->sliding_window_size = static_cast<std::size_t>(number);
  if (!JsonNumber(manifest, "feature_step_size", &number) || number < 1.0) {
    std::fprintf(stderr, "wake: manifest %s has no feature step\n",
                 manifest_path.c_str());
    return false;
  }
  model->feature_step_size = static_cast<std::size_t>(number);
  if (!JsonNumber(manifest, "tensor_arena_size", &number) || number < 1.0) {
    std::fprintf(stderr, "wake: manifest %s has no tensor arena\n",
                 manifest_path.c_str());
    return false;
  }
  model->tensor_arena_size = static_cast<std::size_t>(number);
  model->directory = directory;

  const std::string tflite_path = JoinPath(directory, model_file);

  if (!ReadFile(tflite_path, &model->bytes) || model->bytes.empty()) {
    std::fprintf(stderr, "wake: unable to read %s\n", tflite_path.c_str());
    return false;
  }
  const tflite::Model *tflite_model = tflite::GetModel(model->bytes.data());

  if (!tflite_model || tflite_model->version() != 3) {
    std::fprintf(stderr, "wake: %s is not a TFLite v3 model\n",
                 tflite_path.c_str());
    return false;
  }

  model->arena.assign(model->tensor_arena_size, 0);
  model->allocator =
      tflite::MicroAllocator::Create(model->arena.data(), model->arena.size());
  if (!model->allocator)
    return false;
  model->variables = tflite::MicroResourceVariables::Create(model->allocator,
                                                            kResourceVariables);
  if (!model->variables)
    return false;
  model->resolver.reset(new tflite::MicroMutableOpResolver<
                        libreecho::microwakeword::kSupportedOperatorCount>());
  if (libreecho::microwakeword::RegisterSupportedOps(*model->resolver) !=
      kTfLiteOk)
    return false;
  model->interpreter =
      new tflite::MicroInterpreter(tflite_model, *model->resolver,
                                   model->allocator, model->variables, nullptr);
  if (model->interpreter->AllocateTensors() != kTfLiteOk) {
    std::fprintf(stderr, "wake: %s does not fit its tensor arena of %zu\n",
                 tflite_path.c_str(), model->tensor_arena_size);
    return false;
  }
  model->input = model->interpreter->input(0);
  model->output = model->interpreter->output(0);
  if (!model->input || !model->output || model->input->type != kTfLiteInt8 ||
      model->output->type != kTfLiteUInt8 || model->input->dims->size != 3 ||
      model->input->dims->data[0] != 1 ||
      model->input->dims->data[2] != static_cast<int>(kFeatureChannels) ||
      model->output->dims->size != 2 || model->output->dims->data[0] != 1 ||
      model->output->dims->data[1] != 1) {
    std::fprintf(stderr, "wake: %s has an unsupported tensor shape\n",
                 tflite_path.c_str());
    return false;
  }
  model->stride = static_cast<std::size_t>(model->input->dims->data[1]);
  if (model->stride < 1)
    return false;
  model->input_scale = model->input->params.scale;
  model->input_zero_point = model->input->params.zero_point;
  model->output_scale = model->output->params.scale;
  model->output_zero_point = model->output->params.zero_point;
  if (model->input_scale <= 0.0f || model->output_scale <= 0.0f)
    return false;
  return true;
}

void FillFrontendConfig(struct FrontendConfig *config) {
  FrontendFillConfigWithDefaults(config);
  config->window.size_ms = 30;
  config->window.step_size_ms = 10;
  config->filterbank.num_channels = kFeatureChannels;
  config->filterbank.lower_band_limit = 125.0f;
  config->filterbank.upper_band_limit = 7500.0f;
  config->noise_reduction.smoothing_bits = 10;
  config->noise_reduction.even_smoothing = 0.025f;
  config->noise_reduction.odd_smoothing = 0.06f;
  config->noise_reduction.min_signal_remaining = 0.05f;
  config->pcan_gain_control.enable_pcan = 1;
  config->pcan_gain_control.strength = 0.95f;
  config->pcan_gain_control.offset = 80.0f;
  config->pcan_gain_control.gain_bits = 21;
  config->log_scale.enable_log = 1;
  config->log_scale.scale_shift = 6;
}

class WakeEngine {
public:
  WakeEngine(const char *directory, unsigned int threads,
             const char *second_classifier) {
    struct FrontendConfig config;

    (void)threads;
    if (!directory)
      throw std::runtime_error("no model directory");
    const std::string word_directory = directory;
    std::unique_ptr<WordModel> first(new WordModel);

    if (!LoadManifest(word_directory, first.get()))
      throw std::runtime_error("unable to load the wake word");
    stride_ = first->stride;
    words_[0] = std::move(first);
    word_count_ = 1;
    if (second_classifier) {
      std::unique_ptr<WordModel> second(new WordModel);
      const std::string second_directory = second_classifier;

      if (LoadManifest(second_directory, second.get()) &&
          second->stride == stride_) {
        words_[1] = std::move(second);
        word_count_ = 2;
      } else {
        std::fprintf(stderr, "wake: second word not loaded: %s\n",
                     second_classifier);
      }
    }
    const std::string vad_directory =
        JoinPath(DirectoryOf(word_directory), "vad");
    std::unique_ptr<WordModel> vad(new WordModel);

    if (LoadManifest(vad_directory, vad.get()) && vad->stride == stride_)
      vad_ = std::move(vad);
    else
      std::fprintf(stderr, "wake: VAD model not loaded from %s\n",
                   vad_directory.c_str());

    FillFrontendConfig(&config);
    if (!FrontendPopulateState(&config, &frontend_, 16000))
      throw std::runtime_error("unable to initialise the frontend");
    frontend_ready_ = true;
  }

  ~WakeEngine() {
    if (frontend_ready_)
      FrontendFreeStateContents(&frontend_);
  }

  std::size_t WordCount() const { return word_count_; }

  float VadScore() const {
    if (!vad_ || vad_window_.size() < vad_->sliding_window_size)
      return 0.0f;
    return static_cast<float>(WindowMean(vad_window_));
  }

  /* A missing vad model is reported, not faked with a score of 0. */
  int HasVad() const { return vad_ ? 1 : 0; }

  int Feed(const int16_t *samples, std::size_t count, float *scores,
           std::size_t score_slots, int *new_score) {
    std::size_t offset = 0;
    bool inferred = false;

    if (!samples || !scores || !new_score || score_slots == 0)
      return -1;
    for (std::size_t i = 0; i < score_slots; ++i)
      scores[i] = 0.0f;
    *new_score = 0;
    while (offset < count) {
      const std::size_t copying =
          std::min(kStepSamples - step_fill_, count - offset);

      std::memcpy(step_.data() + step_fill_, samples + offset,
                  copying * sizeof(samples[0]));
      step_fill_ += copying;
      offset += copying;
      if (step_fill_ == kStepSamples) {
        if (ProcessStep())
          inferred = true;
        step_fill_ = 0;
      }
    }
    if (inferred) {
      for (std::size_t i = 0; i < score_slots; ++i) {
        scores[i] = i < word_count_ ? WordScore(i, words_[i].get()) : 0.0f;
      }
      *new_score = 1;
    }
    return 0;
  }

  unsigned int LastInferenceUs() const { return last_inference_us_; }

private:
  bool ProcessStep() {
    std::size_t samples_read = 0;
    const struct FrontendOutput output = FrontendProcessSamples(
        &frontend_, step_.data(), step_.size(), &samples_read);

    if (output.size == 0)
      return false;
    if (output.size != kFeatureChannels) {
      std::fprintf(stderr, "wake: frontend produced %zu values\n", output.size);
      return false;
    }
    pending_.push_back(std::array<uint16_t, kFeatureChannels>());
    std::memcpy(pending_.back().data(), output.values,
                kFeatureChannels * sizeof(output.values[0]));
    if (pending_.size() < stride_)
      return false;

    bool invoked = false;
    unsigned int stride_us = 0;

    for (std::size_t i = 0; i < word_count_; ++i) {
      if (Invoke(words_[i].get(), word_window_[i])) {
        invoked = true;
        stride_us = std::max(stride_us, invoke_us_);
      }
    }
    if (vad_ && Invoke(vad_.get(), vad_window_)) {
      invoked = true;
      stride_us = std::max(stride_us, invoke_us_);
    }
    pending_.clear();
    if (invoked)
      last_inference_us_ = stride_us;
    return invoked;
  }

  bool Invoke(WordModel *model, std::deque<float> &window) {
    int8_t *input = model->input->data.int8;

    for (std::size_t row = 0; row < model->stride; ++row) {
      for (std::size_t channel = 0; channel < kFeatureChannels; ++channel) {
        const double feature =
            static_cast<double>(pending_[row][channel]) * kFeatureScale;
        const double quantized =
            std::nearbyint(feature / static_cast<double>(model->input_scale) +
                           model->input_zero_point);

        input[row * kFeatureChannels + channel] =
            static_cast<int8_t>(static_cast<int>(quantized));
      }
    }
    const auto started = std::chrono::steady_clock::now();

    if (model->interpreter->Invoke() != kTfLiteOk) {
      std::fprintf(stderr, "wake: inference failed for %s\n",
                   model->wake_word.c_str());
      return false;
    }
    invoke_us_ = static_cast<unsigned int>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started)
            .count());
    const double probability =
        (static_cast<double>(model->output->data.uint8[0]) -
         model->output_zero_point) *
        model->output_scale;

    window.push_back(static_cast<float>(probability));
    if (window.size() > model->sliding_window_size)
      window.pop_front();
    return true;
  }

  float WordScore(std::size_t index, const WordModel *model) const {
    const std::deque<float> &window = word_window_[index];

    if (window.size() < model->sliding_window_size)
      return 0.0f;
    return static_cast<float>(WindowMean(window));
  }

  static double WindowMean(const std::deque<float> &window) {
    double sum = 0.0;

    for (float value : window)
      sum += value;
    return sum / static_cast<double>(window.size());
  }

  std::array<int16_t, kStepSamples> step_{};
  std::size_t step_fill_ = 0;
  std::vector<std::array<uint16_t, kFeatureChannels>> pending_;
  struct FrontendState frontend_{};
  bool frontend_ready_ = false;
  std::size_t stride_ = 0;
  std::size_t word_count_ = 0;
  std::unique_ptr<WordModel> words_[2];
  std::unique_ptr<WordModel> vad_;
  std::deque<float> word_window_[2];
  std::deque<float> vad_window_;
  unsigned int invoke_us_ = 0;
  unsigned int last_inference_us_ = 0;
};

} // namespace

struct le_wake_engine {
  std::unique_ptr<WakeEngine> implementation;
};

extern "C" struct le_wake_engine *
le_wake_engine_create_words(const char *model_directory, unsigned int threads,
                            const char *second_classifier) {
  try {
    std::unique_ptr<le_wake_engine> engine(new le_wake_engine);

    engine->implementation.reset(
        new WakeEngine(model_directory, threads, second_classifier));
    return engine.release();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "wake engine initialization failed: %s\n",
                 error.what());
    return nullptr;
  }
}

extern "C" struct le_wake_engine *
le_wake_engine_create(const char *model_directory, unsigned int threads) {
  return le_wake_engine_create_words(model_directory, threads, nullptr);
}

extern "C" size_t
le_wake_engine_word_count(const struct le_wake_engine *engine) {
  return engine && engine->implementation ? engine->implementation->WordCount()
                                          : 0;
}

extern "C" int le_wake_engine_feed_words(struct le_wake_engine *engine,
                                         const int16_t *samples, size_t count,
                                         float *scores, size_t score_slots,
                                         int *new_score) {
  if (!engine || !engine->implementation)
    return -1;
  try {
    return engine->implementation->Feed(samples, count, scores, score_slots,
                                        new_score);
  } catch (const std::exception &error) {
    std::fprintf(stderr, "wake engine inference failed: %s\n", error.what());
    return -1;
  }
}

extern "C" int le_wake_engine_feed(struct le_wake_engine *engine,
                                   const int16_t *samples, size_t count,
                                   float *score, int *new_score) {
  return le_wake_engine_feed_words(engine, samples, count, score, 1, new_score);
}

extern "C" unsigned int
le_wake_engine_last_inference_us(const struct le_wake_engine *engine) {
  return engine && engine->implementation
             ? engine->implementation->LastInferenceUs()
             : 0;
}

extern "C" float le_wake_engine_vad_score(const struct le_wake_engine *engine) {
  return engine && engine->implementation ? engine->implementation->VadScore()
                                          : 0.0f;
}

extern "C" int le_wake_engine_has_vad(const struct le_wake_engine *engine) {
  return engine && engine->implementation && engine->implementation->HasVad();
}

extern "C" void le_wake_engine_destroy(struct le_wake_engine *engine) {
  delete engine;
}
