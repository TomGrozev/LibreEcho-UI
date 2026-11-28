#ifndef LIBREECHO_WAKE_MODEL_H
#define LIBREECHO_WAKE_MODEL_H

#include <stddef.h>

/*
 * Owner-staged wake models: the host-side contract shared by the web control
 * centre (validation before a model is accepted or selected) and waked (the
 * label it reports for the model it is running).
 *
 * A model is a folder named after the wake word holding exactly two files:
 * <name>.tflite, a microWakeWord TensorFlow Lite Micro model, and <name>.json,
 * its manifest (type "micro", version 2). The manifest's "model" field names
 * <name>.tflite, its "wake_word" field is the phrase sent to Home Assistant,
 * and its "micro" object carries the cutoff, sliding window, feature step and
 * tensor arena size. The bundled vad model uses the same shape under the
 * shared root's vad folder.
 *
 * The .tflite is checked structurally: every FlatBuffer offset is bounds
 * checked, only the operators the device resolver registers are accepted,
 * the input is int8 [1, N, 40] and the output uint8 [1, 1]. The arena cap
 * keeps a corrupt or hostile manifest from asking the device for more memory
 * than the engine can spare.
 */

#define LE_WAKE_MODEL_STOCK_NAME "alexa"
#define LE_WAKE_MODEL_STOCK_PHRASE "Alexa"
#define LE_WAKE_MODEL_NAME_MAX 64
#define LE_WAKE_MODEL_PHRASE_MAX 64
#define LE_WAKE_MODEL_MIN_BYTES 1024UL
#define LE_WAKE_MODEL_MAX_BYTES (8UL * 1024UL * 1024UL)
#define LE_WAKE_MODEL_MANIFEST_MIN_BYTES 64UL
#define LE_WAKE_MODEL_MANIFEST_MAX_BYTES 65536UL
/* Largest bundled model asks for 26,080 bytes; two words can be loaded at
 * once, and the device has 512 MB. 64 KiB leaves room for a rebuilt model
 * without letting a bad manifest exhaust the heap. */
#define LE_WAKE_MODEL_ARENA_MAX 65536UL
#define LE_WAKE_MODEL_FILE_COUNT 2

/* 1..63 characters from [A-Za-z0-9._-], not starting with '.'. */
int le_wake_model_name_valid(const char *name);

/* Writes the file name at index 0 (<name>.json) or 1 (<name>.tflite).
 * Returns 0 or -1 when the name is invalid or does not fit. */
int le_wake_model_file_name(const char *name, int index, char *out, size_t size);

/* 1 when file is exactly <name>.json or <name>.tflite. */
int le_wake_model_file_known(const char *name, const char *file);

/* Writes the phrase and the model name waked reports for a model directory.
 * The stock word directory reports the stock labels; any other directory
 * reports its folder name, and the phrase is the manifest's wake_word.
 * Returns 0 or -1. */
int le_wake_model_label(const char *model_directory, char *phrase, size_t phrase_size,
                        char *model, size_t model_size);

/* Writes the manifest wake_word for a model directory, whose basename is the
 * model name. Falls back to the prettified folder name when the manifest
 * cannot be read, so a status line never fails for a missing phrase. */
int le_wake_model_phrase_for(const char *model_directory, char *phrase, size_t phrase_size);

/* Validates one file of a model folder: a regular file (not a symlink) whose
 * name is <model>.json or <model>.tflite. The manifest and the TFLite
 * flatbuffer are checked against the contract above. Returns 0 when accepted. */
int le_wake_model_validate_file(const char *path, const char *file_name,
                                char *reason, size_t reason_size);

/* Validates a whole model folder: a real directory holding exactly the two
 * files named after it, each passing le_wake_model_validate_file, and a
 * manifest whose model field names <name>.tflite. Returns 0 when accepted. */
int le_wake_model_validate_directory(const char *directory,
                                    char *reason, size_t reason_size);

#endif
