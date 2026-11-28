/*
 * Owner-staged wake model contract: name rules, the labels waked reports, the
 * microWakeWord manifest and FlatBuffer checks, and the web store's stage,
 * commit, select and delete flow. The .tflite files are synthesised here with
 * a small FlatBuffer writer so every rejection path has a known-bad model to
 * reject.
 */
#define _POSIX_C_SOURCE 200809L

#include "wake_model.h"
#include "wake_model_store.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
static int checks;

#define CHECK(condition)                                                        \
    do {                                                                        \
        ++checks;                                                               \
        if (!(condition)) {                                                     \
            ++failures;                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        }                                                                       \
    } while (0)

/* ---- FlatBuffer writer ---- */

struct fb {
    unsigned char *p;
    size_t len;
    size_t cap;
    int bad;
};

static size_t fb_alloc(struct fb *b, size_t n)
{
    size_t pos;

    while (b->len % 4 && b->len < b->cap)
        b->p[b->len++] = 0;
    pos = b->len;
    if (n > b->cap - b->len) {
        b->bad = 1;
        b->len = b->cap;
        return pos;
    }
    memset(b->p + pos, 0, n);
    b->len += n;
    return pos;
}

static void fb_u16(struct fb *b, size_t at, unsigned value)
{
    if (at + 2 > b->cap)
        return;
    b->p[at] = (unsigned char)(value & 0xffu);
    b->p[at + 1] = (unsigned char)((value >> 8) & 0xffu);
}

static void fb_u32(struct fb *b, size_t at, uint32_t value)
{
    if (at + 4 > b->cap)
        return;
    b->p[at] = (unsigned char)(value & 0xffu);
    b->p[at + 1] = (unsigned char)((value >> 8) & 0xffu);
    b->p[at + 2] = (unsigned char)((value >> 16) & 0xffu);
    b->p[at + 3] = (unsigned char)((value >> 24) & 0xffu);
}

/* An inline field lives in a 4-byte slot at table + 4 + 4 * field. */
static void fb_field(struct fb *b, size_t table, unsigned field, uint32_t value)
{
    fb_u32(b, table + 4 + 4 * (size_t)field, value);
}

static void fb_field_offset(struct fb *b, size_t table, unsigned field, size_t child)
{
    size_t at = table + 4 + 4 * (size_t)field;
    fb_u32(b, at, (uint32_t)(child - at));
}

/* Vtables are written after the table and its children; FlatBuffers allows a
 * negative soffset, and the validator bounds-checks either direction. */
static void fb_table_end(struct fb *b, size_t table, unsigned fields, const int *present)
{
    size_t vtable = fb_alloc(b, 4 + 2 * (size_t)fields);
    unsigned i;

    fb_u16(b, vtable, 4 + 2 * fields);
    fb_u16(b, vtable + 2, 4 + 4 * fields);
    for (i = 0; i < fields; ++i)
        fb_u16(b, vtable + 4 + 2 * (size_t)i, present[i] ? 4 + 4 * i : 0);
    fb_u32(b, table, (uint32_t)(int32_t)((long long)table - (long long)vtable));
}

static size_t fb_int_vector(struct fb *b, const int32_t *values, size_t count)
{
    size_t vector = fb_alloc(b, 4 + 4 * count);
    size_t i;

    fb_u32(b, vector, (uint32_t)count);
    for (i = 0; i < count; ++i)
        fb_u32(b, vector + 4 + 4 * i, (uint32_t)values[i]);
    return vector;
}

/* A vector of offsets; child tables must be written after the call. */
static size_t fb_table_vector(struct fb *b, size_t count)
{
    size_t vector = fb_alloc(b, 4 + 4 * count);
    fb_u32(b, vector, (uint32_t)count);
    return vector;
}

static void fb_vector_entry(struct fb *b, size_t vector, size_t index, size_t child)
{
    size_t at = vector + 4 + 4 * index;
    fb_u32(b, at, (uint32_t)(child - at));
}

/* ---- TFLite model builder ---- */

struct model_spec {
    unsigned builtin;         /* builtin_code of the one operator code */
    int opcode_index;         /* Operator.opcode_index */
    unsigned input_type;      /* TensorType (int8 = 9) */
    int32_t input_dims[3];
    size_t input_dim_count;
    unsigned output_type;     /* TensorType (uint8 = 3) */
    int32_t output_dims[2];
    size_t output_dim_count;
    int subgraph_count;
};

static const struct model_spec good_model = {
    14, /* LOGISTIC */
    0,
    9, {1, 3, 40}, 3,
    3, {1, 1}, 2,
    1,
};

static size_t tensor_table(struct fb *b, unsigned type, const int32_t *dims, size_t count)
{
    int present[2] = {1, 1};
    size_t table = fb_alloc(b, 4 + 4 * 2);
    size_t shape = fb_int_vector(b, dims, count);

    fb_field_offset(b, table, 0, shape);
    fb_field(b, table, 1, type);
    fb_table_end(b, table, 2, present);
    return table;
}

static size_t operator_table(struct fb *b, int opcode_index)
{
    int present[1] = {1};
    size_t table = fb_alloc(b, 4 + 4);
    fb_field(b, table, 0, (uint32_t)opcode_index);
    fb_table_end(b, table, 1, present);
    return table;
}

static size_t subgraph_table(struct fb *b, const struct model_spec *spec)
{
    int present[4] = {1, 1, 1, 1};
    size_t table = fb_alloc(b, 4 + 4 * 4);
    size_t tensors = fb_table_vector(b, 2);
    size_t inputs, outputs, operators;
    int32_t input_index = 0, output_index = 1;

    fb_vector_entry(b, tensors, 0,
                    tensor_table(b, spec->input_type, spec->input_dims,
                                 spec->input_dim_count));
    fb_vector_entry(b, tensors, 1,
                    tensor_table(b, spec->output_type, spec->output_dims,
                                 spec->output_dim_count));
    fb_field_offset(b, table, 0, tensors);
    inputs = fb_int_vector(b, &input_index, 1);
    fb_field_offset(b, table, 1, inputs);
    outputs = fb_int_vector(b, &output_index, 1);
    fb_field_offset(b, table, 2, outputs);
    operators = fb_table_vector(b, 1);
    fb_vector_entry(b, operators, 0, operator_table(b, spec->opcode_index));
    fb_field_offset(b, table, 3, operators);
    fb_table_end(b, table, 4, present);
    return table;
}

static size_t build_model(unsigned char *out, size_t cap, const struct model_spec *spec)
{
    struct fb b = {out, 0, cap, 0};
    size_t root, model, codes, code, subgraphs;
    int present_model[8] = {0}, present_code[4] = {0, 0, 0, 1};
    int i;

    root = fb_alloc(&b, 4);
    model = fb_alloc(&b, 4 + 4 * 8);
    codes = fb_table_vector(&b, 1);
    code = fb_alloc(&b, 4 + 4 * 4);
    fb_field(&b, code, 3, spec->builtin);
    fb_table_end(&b, code, 4, present_code);
    fb_vector_entry(&b, codes, 0, code);
    fb_field_offset(&b, model, 1, codes);
    subgraphs = fb_table_vector(&b, (size_t)(spec->subgraph_count > 0 ? spec->subgraph_count : 1));
    for (i = 0; i < (spec->subgraph_count > 0 ? spec->subgraph_count : 1); ++i)
        fb_vector_entry(&b, subgraphs, (size_t)i, subgraph_table(&b, spec));
    fb_field_offset(&b, model, 2, subgraphs);
    present_model[1] = present_model[2] = 1;
    fb_table_end(&b, model, 8, present_model);
    fb_u32(&b, root, (uint32_t)model);
    if (b.bad)
        return 0;
    while (b.len < 1200 && b.len < cap)
        out[b.len++] = 0;
    return b.len;
}

/* ---- filesystem helpers ---- */

static char root[512];

static void make_root(void)
{
    const char *tmp = getenv("TMPDIR");
    snprintf(root, sizeof(root), "%s/wake-model-test-XXXXXX", tmp ? tmp : "/tmp");
    if (!mkdtemp(root)) {
        perror("mkdtemp");
        exit(1);
    }
}

static void write_file(const char *path, const unsigned char *data, size_t length)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        exit(1);
    }
    fwrite(data, 1, length, f);
    fclose(f);
}

static void path_in(char *out, size_t size, const char *dir, const char *name)
{
    snprintf(out, size, "%s/%s", dir, name);
}

/* ---- manifest ---- */

enum {
    MF_BAD_TYPE = 1u << 0,
    MF_BAD_VERSION = 1u << 1,
    MF_NO_WORD = 1u << 2,
    MF_BAD_MODEL = 1u << 3,
    MF_NO_MICRO = 1u << 4,
    MF_BAD_CUTOFF = 1u << 5,
    MF_BAD_WINDOW = 1u << 6,
    MF_BAD_STEP = 1u << 7,
    MF_BAD_ARENA = 1u << 8,
    MF_HUGE_ARENA = 1u << 9,
};

static void manifest_text(char *out, size_t size, const char *name, const char *word,
                          unsigned flags)
{
    char model_name[96], model_field[128];
    char word_field[96] = "";
    char micro_field[192] = "";
    const char *type = (flags & MF_BAD_TYPE) ? "legacy" : "micro";
    int version = (flags & MF_BAD_VERSION) ? 1 : 2;

    snprintf(model_name, sizeof(model_name), "%s.tflite",
             (flags & MF_BAD_MODEL) ? "other" : name);
    snprintf(model_field, sizeof(model_field), "\"model\":\"%s\"", model_name);
    if (!(flags & MF_NO_WORD))
        snprintf(word_field, sizeof(word_field), "\"wake_word\":\"%s\",", word ? word : name);
    if (!(flags & MF_NO_MICRO)) {
        int window = (flags & MF_BAD_WINDOW) ? 0 : 5;
        int step = (flags & MF_BAD_STEP) ? 0 : 10;
        int arena = (flags & MF_BAD_ARENA) ? 0 : ((flags & MF_HUGE_ARENA) ? 100000 : 26080);
        double cutoff = (flags & MF_BAD_CUTOFF) ? 1.5 : 0.97;
        snprintf(micro_field, sizeof(micro_field),
                 ",\"micro\":{\"probability_cutoff\":%.2f,\"sliding_window_size\":%d,"
                 "\"feature_step_size\":%d,\"tensor_arena_size\":%d}",
                 cutoff, window, step, arena);
    }
    snprintf(out, size,
             "{\"type\":\"%s\",\"version\":%d,%s%s%s}",
             type, version, word_field, model_field, micro_field);
}

static void write_manifest(const char *dir, const char *name, const char *word,
                           unsigned flags)
{
    char path[700], text[512], file[96];

    manifest_text(text, sizeof(text), name, word, flags);
    snprintf(file, sizeof(file), "%s.json", name);
    path_in(path, sizeof(path), dir, file);
    write_file(path, (const unsigned char *)text, strlen(text));
}

/* ---- checks ---- */

static void check_names(void)
{
    char file[80];

    CHECK(le_wake_model_name_valid("my-word"));
    CHECK(le_wake_model_name_valid("Hey_Kitchen.2"));
    CHECK(!le_wake_model_name_valid(""));
    CHECK(!le_wake_model_name_valid(".hidden"));
    CHECK(!le_wake_model_name_valid("../escape"));
    CHECK(!le_wake_model_name_valid("with space"));
    CHECK(!le_wake_model_name_valid("slash/name"));
    CHECK(!le_wake_model_name_valid(NULL));

    CHECK(le_wake_model_file_name("my-word", 0, file, sizeof(file)) == 0);
    CHECK(!strcmp(file, "my-word.json"));
    CHECK(le_wake_model_file_name("my-word", 1, file, sizeof(file)) == 0);
    CHECK(!strcmp(file, "my-word.tflite"));
    CHECK(le_wake_model_file_name("my-word", 2, file, sizeof(file)) < 0);
    CHECK(le_wake_model_file_known("my-word", "my-word.json"));
    CHECK(le_wake_model_file_known("my-word", "my-word.tflite"));
    CHECK(!le_wake_model_file_known("my-word", "other.json"));
    CHECK(!le_wake_model_file_known("my-word", "my-word.bin"));
}

static void check_labels(void)
{
    char dir[700], phrase[64], model[64];
    unsigned char bytes[16384];
    size_t length = build_model(bytes, sizeof(bytes), &good_model);

    CHECK(length > 0);
    CHECK(le_wake_model_label("/usr/share/libreecho/wakeword/alexa", phrase,
                              sizeof(phrase), model, sizeof(model)) == 0);
    CHECK(!strcmp(phrase, "Alexa"));
    CHECK(!strcmp(model, "alexa"));

    make_root();
    path_in(dir, sizeof(dir), root, "owner-word");
    mkdir(dir, 0700);
    {
        char model_path[720];
        path_in(model_path, sizeof(model_path), dir, "owner-word.tflite");
        write_file(model_path, bytes, length);
    }
    write_manifest(dir, "owner-word", "Hey Kitchen", 0);
    CHECK(le_wake_model_label(dir, phrase, sizeof(phrase), model, sizeof(model)) == 0);
    CHECK(!strcmp(phrase, "Hey Kitchen"));
    CHECK(!strcmp(model, "owner-word"));

    /* A manifest with a broken model field is ignored for the phrase. */
    path_in(dir, sizeof(dir), root, "two-words");
    mkdir(dir, 0700);
    write_manifest(dir, "two-words", "Two Words", MF_BAD_MODEL);
    CHECK(le_wake_model_label(dir, phrase, sizeof(phrase), model, sizeof(model)) == 0);
    CHECK(!strcmp(phrase, "Two Words"));
    CHECK(!strcmp(model, "two-words"));

    /* Without any manifest the phrase comes from the folder name. */
    path_in(dir, sizeof(dir), root, "hey_kitchen");
    mkdir(dir, 0700);
    CHECK(le_wake_model_phrase_for(dir, phrase, sizeof(phrase)) == 0);
    CHECK(!strcmp(phrase, "Hey Kitchen"));
    CHECK(le_wake_model_label("/data/libreecho/wakeword-models/bad name", phrase,
                              sizeof(phrase), model, sizeof(model)) < 0);
}

static void check_contract(void)
{
    char dir[600], path[700], reason[200];
    unsigned char bytes[16384];
    size_t length;

    make_root();
    path_in(dir, sizeof(dir), root, "contract");
    mkdir(dir, 0700);
    path_in(path, sizeof(path), dir, "my-word.tflite");

    length = build_model(bytes, sizeof(bytes), &good_model);
    CHECK(length >= LE_WAKE_MODEL_MIN_BYTES);
    write_file(path, bytes, length);
    CHECK(le_wake_model_validate_file(path, "my-word.tflite", reason, sizeof(reason)) == 0);

    {
        struct model_spec s = good_model;
        s.builtin = 5; /* not in the resolver's operator set */
        length = build_model(bytes, sizeof(bytes), &s);
        write_file(path, bytes, length);
        CHECK(le_wake_model_validate_file(path, "my-word.tflite", reason, sizeof(reason)) < 0);
        CHECK(strstr(reason, "operators") != NULL);
    }
    {
        struct model_spec s = good_model;
        s.input_type = 3; /* uint8 input */
        length = build_model(bytes, sizeof(bytes), &s);
        write_file(path, bytes, length);
        CHECK(le_wake_model_validate_file(path, "my-word.tflite", reason, sizeof(reason)) < 0);
        CHECK(strstr(reason, "int8") != NULL);
    }
    {
        struct model_spec s = good_model;
        s.input_dims[1] = 0;
        length = build_model(bytes, sizeof(bytes), &s);
        write_file(path, bytes, length);
        CHECK(le_wake_model_validate_file(path, "my-word.tflite", reason, sizeof(reason)) < 0);
    }
    {
        struct model_spec s = good_model;
        s.input_dims[2] = 41;
        length = build_model(bytes, sizeof(bytes), &s);
        write_file(path, bytes, length);
        CHECK(le_wake_model_validate_file(path, "my-word.tflite", reason, sizeof(reason)) < 0);
    }
    {
        struct model_spec s = good_model;
        s.output_type = 9; /* int8 output */
        length = build_model(bytes, sizeof(bytes), &s);
        write_file(path, bytes, length);
        CHECK(le_wake_model_validate_file(path, "my-word.tflite", reason, sizeof(reason)) < 0);
        CHECK(strstr(reason, "uint8") != NULL);
    }
    {
        struct model_spec s = good_model;
        s.output_dims[1] = 2;
        length = build_model(bytes, sizeof(bytes), &s);
        write_file(path, bytes, length);
        CHECK(le_wake_model_validate_file(path, "my-word.tflite", reason, sizeof(reason)) < 0);
    }
    {
        struct model_spec s = good_model;
        s.subgraph_count = 2;
        length = build_model(bytes, sizeof(bytes), &s);
        write_file(path, bytes, length);
        CHECK(le_wake_model_validate_file(path, "my-word.tflite", reason, sizeof(reason)) < 0);
        CHECK(strstr(reason, "one subgraph") != NULL);
    }
    {
        struct model_spec s = good_model;
        s.opcode_index = 1;
        length = build_model(bytes, sizeof(bytes), &s);
        write_file(path, bytes, length);
        CHECK(le_wake_model_validate_file(path, "my-word.tflite", reason, sizeof(reason)) < 0);
        CHECK(strstr(reason, "opcode") != NULL);
    }
    {
        /* An all-zero file has no usable root table. */
        memset(bytes, 0, 1200);
        write_file(path, bytes, 1200);
        CHECK(le_wake_model_validate_file(path, "my-word.tflite", reason, sizeof(reason)) < 0);
        CHECK(strstr(reason, "flatbuffer") != NULL);
    }
    {
        /* A symlink to a good model is refused, not followed. */
        char link_path[700];
        length = build_model(bytes, sizeof(bytes), &good_model);
        write_file(path, bytes, length);
        path_in(link_path, sizeof(link_path), dir, "link.tflite");
        symlink(path, link_path);
        CHECK(le_wake_model_validate_file(link_path, "link.tflite", reason,
                                          sizeof(reason)) < 0);
        unlink(link_path);
    }
    length = build_model(bytes, sizeof(bytes), &good_model);
    write_file(path, bytes, length);
    CHECK(le_wake_model_validate_file(path, "unknown.bin", reason, sizeof(reason)) < 0);
    CHECK(le_wake_model_validate_file(path, "my-word.tflite", NULL, 0) == 0);

    /* Manifest contract. */
    path_in(path, sizeof(path), dir, "my-word.json");
    {
        char text[512];
        manifest_text(text, sizeof(text), "my-word", "My Word", 0);
        write_file(path, (const unsigned char *)text, strlen(text));
    }
    CHECK(le_wake_model_validate_file(path, "my-word.json", reason, sizeof(reason)) == 0);

    {
        const unsigned flags[] = {MF_BAD_TYPE, MF_BAD_VERSION, MF_NO_WORD, MF_BAD_MODEL,
                                  MF_NO_MICRO, MF_BAD_CUTOFF, MF_BAD_WINDOW, MF_BAD_STEP,
                                  MF_BAD_ARENA, MF_HUGE_ARENA};
        size_t i;
        for (i = 0; i < sizeof(flags) / sizeof(flags[0]); ++i) {
            char text[512];
            manifest_text(text, sizeof(text), "my-word", "My Word", flags[i]);
            write_file(path, (const unsigned char *)text, strlen(text));
            CHECK(le_wake_model_validate_file(path, "my-word.json", reason,
                                              sizeof(reason)) < 0);
        }
    }
}

static void write_set(const char *dir, const char *name, unsigned manifest_flags)
{
    unsigned char bytes[16384];
    char path[700], file[96];
    size_t length = build_model(bytes, sizeof(bytes), &good_model);

    snprintf(file, sizeof(file), "%s.tflite", name);
    path_in(path, sizeof(path), dir, file);
    write_file(path, bytes, length);
    write_manifest(dir, name, name, manifest_flags);
}

static void check_directory(void)
{
    char dir[600], extra[700], reason[200];

    make_root();
    path_in(dir, sizeof(dir), root, "hey_kitchen");
    mkdir(dir, 0700);
    write_set(dir, "hey_kitchen", 0);
    CHECK(le_wake_model_validate_directory(dir, reason, sizeof(reason)) == 0);

    path_in(extra, sizeof(extra), dir, "notes.txt");
    write_file(extra, (const unsigned char *)"x", 1);
    CHECK(le_wake_model_validate_directory(dir, reason, sizeof(reason)) < 0);
    unlink(extra);

    path_in(extra, sizeof(extra), dir, "hey_kitchen.tflite");
    unlink(extra);
    CHECK(le_wake_model_validate_directory(dir, reason, sizeof(reason)) < 0);
    CHECK(strstr(reason, "exactly") != NULL);

    /* A folder holding an unsupported model file is rejected. */
    path_in(dir, sizeof(dir), root, "old_word");
    mkdir(dir, 0700);
    path_in(extra, sizeof(extra), dir, "old_word.bin");
    write_file(extra, (const unsigned char *)"x", 1);
    CHECK(le_wake_model_validate_directory(dir, reason, sizeof(reason)) < 0);
    CHECK(strstr(reason, "unexpected file") != NULL);
}

/* ---- store ---- */

struct tally {
    int count;
    int valid;
    int selected;
    unsigned long bytes;
};

static void tally_visit(const struct le_wake_model_entry *entry, void *opaque)
{
    struct tally *t = opaque;
    ++t->count;
    if (entry->valid)
        ++t->valid;
    if (entry->selected)
        ++t->selected;
    t->bytes += entry->bytes;
}

static void stage_file(const struct le_wake_model_paths *paths, const char *name,
                       const char *file, const char *source_dir)
{
    char path[700];
    unsigned char bytes[16384];
    FILE *f;
    size_t n, offset = 0;
    unsigned long staged = 0;

    path_in(path, sizeof(path), source_dir, file);
    f = fopen(path, "rb");
    CHECK(f != NULL);
    if (!f)
        return;
    n = fread(bytes, 1, sizeof(bytes), f);
    fclose(f);
    /* Two chunks, so the offset rule is exercised. */
    while (offset < n) {
        size_t piece = n - offset > 700 ? 700 : n - offset;
        int rc = le_wake_model_stage_chunk(paths, name, file, (unsigned long)offset,
                                           bytes + offset, piece, &staged);
        if (rc != LE_WAKE_MODEL_STORE_OK)
            fprintf(stderr, "stage %s/%s rc=%d offset=%lu\n", name, file, rc,
                    (unsigned long)offset);
        CHECK(rc == LE_WAKE_MODEL_STORE_OK);
        offset += piece;
    }
    CHECK(staged == n);
}

static void check_store(void)
{
    char config_dir[600], config_file[700], source[600], reason[200], name[64];
    struct le_wake_model_paths paths;
    struct tally t;
    unsigned long staged = 0;
    unsigned char chunk[8];
    int rc;

    make_root();
    path_in(config_dir, sizeof(config_dir), root, "config");
    mkdir(config_dir, 0700);
    path_in(config_file, sizeof(config_file), config_dir, "web-config.json");
    CHECK(le_wake_model_paths_from_config(config_file, &paths) == 0);
    CHECK(strstr(paths.models, "/wakeword-models") != NULL);
    CHECK(strstr(paths.selection, "/config/wake-model") != NULL);

    /* Rejected names and files never touch the store. */
    CHECK(le_wake_model_stage_chunk(&paths, "../evil", "evil.json", 0, chunk, 1,
                                    &staged) == LE_WAKE_MODEL_STORE_INVALID);
    CHECK(le_wake_model_stage_chunk(&paths, "my-word", "other.bin", 0, chunk, 1,
                                    &staged) == LE_WAKE_MODEL_STORE_INVALID);
    CHECK(le_wake_model_stage_chunk(&paths, LE_WAKE_MODEL_STOCK_NAME, "alexa.json", 0,
                                    chunk, 1, &staged) == LE_WAKE_MODEL_STORE_INVALID);
    CHECK(le_wake_model_stage_chunk(&paths, "vad", "vad.json", 0, chunk, 1,
                                    &staged) == LE_WAKE_MODEL_STORE_INVALID);
    CHECK(le_wake_model_stage_chunk(&paths, "my-word", "my-word.json", 0, chunk,
                                    LE_WAKE_MODEL_STAGE_CHUNK_MAX + 1,
                                    &staged) == LE_WAKE_MODEL_STORE_INVALID);

    /* Offsets must follow on from what is staged. */
    path_in(source, sizeof(source), root, "source");
    mkdir(source, 0700);
    write_set(source, "my-word", 0);
    stage_file(&paths, "my-word", "my-word.json", source);
    stage_file(&paths, "my-word", "my-word.tflite", source);
    CHECK(le_wake_model_stage_chunk(&paths, "my-word", "my-word.tflite", 3, chunk, 2,
                                    &staged) == LE_WAKE_MODEL_STORE_INVALID);

    /* Committing a folder that does not pass is refused and cleaned up. */
    {
        write_set(source, "bad-word", MF_BAD_CUTOFF);
        stage_file(&paths, "bad-word", "bad-word.json", source);
        stage_file(&paths, "bad-word", "bad-word.tflite", source);
        rc = le_wake_model_commit(&paths, "bad-word", reason, sizeof(reason));
        CHECK(rc == LE_WAKE_MODEL_STORE_INVALID);
        CHECK(strstr(reason, "probability_cutoff") != NULL);
        CHECK(le_wake_model_commit(&paths, "bad-word", reason, sizeof(reason)) ==
              LE_WAKE_MODEL_STORE_NOT_FOUND);
    }

    rc = le_wake_model_commit(&paths, "my-word", reason, sizeof(reason));
    CHECK(rc == LE_WAKE_MODEL_STORE_OK);
    CHECK(le_wake_model_commit(&paths, "my-word", reason, sizeof(reason)) ==
          LE_WAKE_MODEL_STORE_NOT_FOUND);

    memset(&t, 0, sizeof(t));
    CHECK(le_wake_model_scan(&paths, tally_visit, &t) == 1);
    CHECK(t.count == 1 && t.valid == 1 && t.selected == 0 && t.bytes > 1200);

    /* Selection: stock by default, the folder once selected, stock again after. */
    CHECK(le_wake_model_selection_read(&paths, name, sizeof(name)) == 0);
    CHECK(le_wake_model_select(&paths, "missing") == LE_WAKE_MODEL_STORE_NOT_FOUND);
    CHECK(le_wake_model_select(&paths, "../my-word") == LE_WAKE_MODEL_STORE_INVALID);
    CHECK(le_wake_model_select(&paths, "my-word") == LE_WAKE_MODEL_STORE_OK);
    CHECK(le_wake_model_selection_read(&paths, name, sizeof(name)) == 1);
    CHECK(!strcmp(name, "my-word"));
    memset(&t, 0, sizeof(t));
    le_wake_model_scan(&paths, tally_visit, &t);
    CHECK(t.selected == 1);
    CHECK(le_wake_model_delete(&paths, "my-word") == LE_WAKE_MODEL_STORE_CONFLICT);
    CHECK(le_wake_model_select(&paths, LE_WAKE_MODEL_STOCK_NAME) == LE_WAKE_MODEL_STORE_OK);
    CHECK(le_wake_model_selection_read(&paths, name, sizeof(name)) == 0);

    CHECK(le_wake_model_delete(&paths, "my-word") == LE_WAKE_MODEL_STORE_OK);
    CHECK(le_wake_model_delete(&paths, "my-word") == LE_WAKE_MODEL_STORE_NOT_FOUND);
    memset(&t, 0, sizeof(t));
    CHECK(le_wake_model_scan(&paths, tally_visit, &t) == 0);
}

static void check_base64(void)
{
    unsigned char out[16];

    CHECK(le_wake_model_base64_decode("aGVsbG8=", 8, out, sizeof(out)) == 5);
    CHECK(!memcmp(out, "hello", 5));
    CHECK(le_wake_model_base64_decode("aGk=", 4, out, sizeof(out)) == 2);
    CHECK(!memcmp(out, "hi", 2));
    CHECK(le_wake_model_base64_decode("a!==", 4, out, sizeof(out)) < 0);
    CHECK(le_wake_model_base64_decode("abc", 3, out, sizeof(out)) < 0);
    CHECK(le_wake_model_base64_decode("aGVsbG8=", 8, out, 2) < 0);
}

int main(void)
{
    check_names();
    check_labels();
    check_contract();
    check_directory();
    check_store();
    check_base64();
    if (failures) {
        fprintf(stderr, "wake model: %d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("wake model: %d checks passed\n", checks);
    return 0;
}
