#define _POSIX_C_SOURCE 200809L

#include "wake_model.h"

#include "json.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Builtin operator codes from TensorFlow Lite's schema. The list is the 20
 * operators in LibreEcho-Platform's micro_wake_word_operators.config (the
 * resolver registration and the generated C name check come from the same
 * file). Keep the two in sync: the validator must never accept an operator
 * the device resolver cannot register. */
static const unsigned builtin_operators[] = {
    0,   /* ADD */
    1,   /* AVERAGE_POOL_2D */
    2,   /* CONCATENATION */
    3,   /* CONV_2D */
    4,   /* DEPTHWISE_CONV_2D */
    9,   /* FULLY_CONNECTED */
    14,  /* LOGISTIC */
    17,  /* MAX_POOL_2D */
    18,  /* MUL */
    22,  /* RESHAPE */
    34,  /* PAD */
    40,  /* MEAN */
    45,  /* STRIDED_SLICE */
    83,  /* PACK */
    102, /* SPLIT_V */
    114, /* QUANTIZE */
    129, /* CALL_ONCE */
    142, /* VAR_HANDLE */
    143, /* READ_VARIABLE */
    144, /* ASSIGN_VARIABLE */
};

#define TFLITE_TENSOR_INT8 9
#define TFLITE_TENSOR_UINT8 3

static int fail(char *reason, size_t reason_size, const char *text)
{
    if (reason && reason_size)
        snprintf(reason, reason_size, "%s", text);
    return -1;
}

int le_wake_model_name_valid(const char *name)
{
    size_t i, length;

    if (!name)
        return 0;
    length = strlen(name);
    if (length == 0 || length >= LE_WAKE_MODEL_NAME_MAX || name[0] == '.')
        return 0;
    for (i = 0; i < length; ++i) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

int le_wake_model_file_name(const char *name, int index, char *out, size_t size)
{
    const char *suffix;
    int n;

    if (!name || !out || !le_wake_model_name_valid(name))
        return -1;
    if (index == 0)
        suffix = ".json";
    else if (index == 1)
        suffix = ".tflite";
    else
        return -1;
    n = snprintf(out, size, "%s%s", name, suffix);
    return n > 0 && (size_t)n < size ? 0 : -1;
}

int le_wake_model_file_known(const char *name, const char *file)
{
    char expected[LE_WAKE_MODEL_NAME_MAX + 8];
    int i;

    for (i = 0; i < LE_WAKE_MODEL_FILE_COUNT; ++i) {
        if (le_wake_model_file_name(name, i, expected, sizeof(expected)) < 0)
            return 0;
        if (!strcmp(file, expected))
            return 1;
    }
    return 0;
}

static const char *path_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* Copies a directory basename into name after trimming trailing slashes. */
static int directory_name(const char *path, char *name, size_t size)
{
    char trimmed[1024];
    const char *base;
    size_t length;

    if (!path || !name || !size)
        return -1;
    length = strlen(path);
    if (length >= sizeof(trimmed))
        return -1;
    memcpy(trimmed, path, length + 1);
    while (length > 0 && trimmed[length - 1] == '/')
        trimmed[--length] = '\0';
    if (!length)
        return -1;
    base = path_basename(trimmed);
    if (strlen(base) >= size)
        return -1;
    memcpy(name, base, strlen(base) + 1);
    return length;
}

static int prettify(const char *name, char *phrase, size_t phrase_size)
{
    size_t i, used = 0;
    int word_start = 1;

    if (!name || !phrase || !phrase_size)
        return -1;
    for (i = 0; name[i] && used + 1 < phrase_size; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (c == '_' || c == '-') {
            phrase[used++] = ' ';
            word_start = 1;
        } else if (isalpha(c)) {
            phrase[used++] = (char)(word_start ? toupper(c) : tolower(c));
            word_start = 0;
        } else {
            phrase[used++] = (char)c;
            word_start = 0;
        }
    }
    if (name[i] || used >= phrase_size)
        return -1;
    while (used && phrase[used - 1] == ' ')
        --used;
    phrase[used] = '\0';
    return 0;
}

/* ---- file reading ---- */

static int read_regular_file(const char *path, const char *label, unsigned long minimum,
                             unsigned long limit, unsigned char **buffer, size_t *size,
                             char *reason, size_t reason_size)
{
    struct stat before, after;
    unsigned char *data;
    size_t total = 0;
    int fd;
    char message[160];

    if (!path || !buffer || !size)
        return fail(reason, reason_size, "model file is missing");
    if (lstat(path, &before) < 0) {
        snprintf(message, sizeof(message), "%s is missing", label);
        return fail(reason, reason_size, message);
    }
    if (!S_ISREG(before.st_mode)) {
        snprintf(message, sizeof(message), "%s is not a regular file", label);
        return fail(reason, reason_size, message);
    }
    if ((unsigned long)before.st_size < minimum ||
        (unsigned long)before.st_size > limit) {
        snprintf(message, sizeof(message), "%s size is outside the accepted range", label);
        return fail(reason, reason_size, message);
    }
    fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0 || fstat(fd, &after) < 0) {
        if (fd >= 0)
            close(fd);
        snprintf(message, sizeof(message), "%s could not be read", label);
        return fail(reason, reason_size, message);
    }
    if (!S_ISREG(after.st_mode) || after.st_dev != before.st_dev ||
        after.st_ino != before.st_ino || after.st_size != before.st_size) {
        close(fd);
        snprintf(message, sizeof(message), "%s changed while it was read", label);
        return fail(reason, reason_size, message);
    }
    data = malloc((size_t)after.st_size);
    if (!data) {
        close(fd);
        return fail(reason, reason_size, "model could not be checked: out of memory");
    }
    while (total < (size_t)after.st_size) {
        ssize_t n = read(fd, data + total, (size_t)after.st_size - total);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        total += (size_t)n;
    }
    close(fd);
    if (total != (size_t)after.st_size) {
        free(data);
        snprintf(message, sizeof(message), "%s could not be read", label);
        return fail(reason, reason_size, message);
    }
    *buffer = data;
    *size = total;
    return 0;
}

/* ---- minimal bounds-checked FlatBuffer reader ---- */

struct fb_view {
    const unsigned char *p;
    size_t n;
};

static int fb_range(const struct fb_view *v, size_t offset, size_t length)
{
    return offset <= v->n && length <= v->n - offset;
}

static int fb_u16(const struct fb_view *v, size_t offset, uint16_t *out)
{
    if (!fb_range(v, offset, 2))
        return -1;
    *out = (uint16_t)v->p[offset] | (uint16_t)((uint16_t)v->p[offset + 1] << 8);
    return 0;
}

static int fb_i32(const struct fb_view *v, size_t offset, int32_t *out)
{
    uint32_t value;

    if (!fb_range(v, offset, 4))
        return -1;
    value = (uint32_t)v->p[offset] | ((uint32_t)v->p[offset + 1] << 8) |
            ((uint32_t)v->p[offset + 2] << 16) | ((uint32_t)v->p[offset + 3] << 24);
    *out = (int32_t)value;
    return 0;
}

static int fb_u32(const struct fb_view *v, size_t offset, uint32_t *out)
{
    int32_t value;

    if (fb_i32(v, offset, &value) < 0)
        return -1;
    *out = (uint32_t)value;
    return 0;
}

/* Field offset relative to the table, or 0 when absent. */
static int fb_table_field(const struct fb_view *v, size_t table, unsigned field,
                          size_t field_size, size_t *field_offset)
{
    int32_t soffset;
    uint16_t vtable_size, table_size, entry;
    size_t vtable;
    int64_t position;

    *field_offset = 0;
    if (fb_i32(v, table, &soffset) < 0)
        return -1;
    position = (int64_t)table - (int64_t)soffset;
    if (position < 0 || (uint64_t)position >= v->n)
        return -1;
    vtable = (size_t)position;
    if (fb_u16(v, vtable, &vtable_size) < 0 || vtable_size < 4 ||
        !fb_range(v, vtable, vtable_size) ||
        fb_u16(v, vtable + 2, &table_size) < 0 || table_size < 4 ||
        !fb_range(v, table, table_size))
        return -1;
    if (4 + 2 * (size_t)field + 2 > (size_t)vtable_size)
        return 0;
    if (fb_u16(v, vtable + 4 + 2 * (size_t)field, &entry) < 0)
        return -1;
    if (!entry)
        return 0;
    if ((size_t)entry + field_size > (size_t)table_size ||
        !fb_range(v, table, (size_t)entry + field_size))
        return -1;
    *field_offset = table + entry;
    return 1;
}

static int fb_table_u32(const struct fb_view *v, size_t table, unsigned field,
                        uint32_t *value, int *present)
{
    size_t offset;
    int rc = fb_table_field(v, table, field, 4, &offset);

    *present = 0;
    if (rc <= 0)
        return rc;
    if (fb_u32(v, offset, value) < 0)
        return -1;
    *present = 1;
    return 1;
}

static int fb_table_byte(const struct fb_view *v, size_t table, unsigned field,
                         unsigned *value, int *present)
{
    size_t offset;
    int rc = fb_table_field(v, table, field, 1, &offset);

    *present = 0;
    if (rc <= 0)
        return rc;
    *value = v->p[offset];
    *present = 1;
    return 1;
}

/* Resolves an offset field to the absolute position it points at. */
static int fb_offset_target(const struct fb_view *v, size_t table, unsigned field,
                            size_t *target)
{
    size_t offset;
    uint32_t relative;
    int rc = fb_table_field(v, table, field, 4, &offset);

    *target = 0;
    if (rc <= 0)
        return rc;
    if (fb_u32(v, offset, &relative) < 0)
        return -1;
    if ((uint64_t)offset + relative >= v->n)
        return -1;
    *target = offset + relative;
    return 1;
}

static int fb_vector(const struct fb_view *v, size_t table, unsigned field,
                     size_t *data, uint32_t *count)
{
    size_t target;
    int rc = fb_offset_target(v, table, field, &target);

    *data = 0;
    *count = 0;
    if (rc <= 0)
        return rc;
    if (fb_u32(v, target, count) < 0)
        return -1;
    *data = target + 4;
    if (*count > (v->n - *data) / 4)
        return -1;
    return 1;
}

static int fb_vector_table(const struct fb_view *v, size_t data, uint32_t index,
                           size_t *table)
{
    uint32_t relative;
    size_t element = data + (size_t)index * 4;

    if (fb_u32(v, element, &relative) < 0 || (uint64_t)element + relative >= v->n)
        return -1;
    *table = element + relative;
    return 0;
}

static int operator_allowed(unsigned code)
{
    size_t i;

    for (i = 0; i < sizeof(builtin_operators) / sizeof(builtin_operators[0]); ++i)
        if (code == builtin_operators[i])
            return 1;
    return 0;
}

static int tensor_shape(const struct fb_view *v, size_t tensor, int32_t *dims,
                        size_t capacity, size_t *count)
{
    size_t data;
    uint32_t length, i;

    *count = 0;
    if (fb_vector(v, tensor, 0, &data, &length) <= 0 || length > capacity)
        return -1;
    for (i = 0; i < length; ++i) {
        if (fb_i32(v, data + (size_t)i * 4, &dims[i]) < 0)
            return -1;
    }
    *count = length;
    return 0;
}

static int tensor_named(const struct fb_view *v, size_t tensors_data, uint32_t index,
                        size_t *tensor)
{
    if (fb_vector_table(v, tensors_data, index, tensor) < 0)
        return -1;
    return 0;
}

static int tensor_type_is(const struct fb_view *v, size_t tensor, unsigned type)
{
    unsigned actual = 0;
    int present = 0;

    return fb_table_byte(v, tensor, 1, &actual, &present) == 1 && present &&
           actual == type;
}

static int check_subgraph(const struct fb_view *v, size_t model, size_t subgraph,
                          char *reason, size_t reason_size)
{
    size_t tensors = 0, inputs = 0, outputs = 0, operators = 0;
    uint32_t tensor_count = 0, input_count = 0, output_count = 0, operator_count = 0;
    uint32_t opcode_count = 0, index;
    int32_t input_index, output_index, dims[8];
    size_t count, tensor;

    if (fb_vector(v, subgraph, 1, &inputs, &input_count) <= 0 || input_count != 1 ||
        fb_vector(v, subgraph, 2, &outputs, &output_count) <= 0 || output_count != 1 ||
        fb_vector(v, subgraph, 0, &tensors, &tensor_count) <= 0 ||
        fb_vector(v, subgraph, 3, &operators, &operator_count) <= 0 ||
        operator_count == 0)
        return fail(reason, reason_size, "model has no single input and output");
    {
        size_t opcodes;
        if (fb_vector(v, model, 1, &opcodes, &opcode_count) < 0)
            opcode_count = 0;
    }
    if (fb_i32(v, inputs, &input_index) < 0 || input_index < 0 ||
        (uint32_t)input_index >= tensor_count ||
        fb_i32(v, outputs, &output_index) < 0 || output_index < 0 ||
        (uint32_t)output_index >= tensor_count)
        return fail(reason, reason_size, "model input or output index is invalid");

    if (tensor_named(v, tensors, (uint32_t)input_index, &tensor) < 0 ||
        !tensor_type_is(v, tensor, TFLITE_TENSOR_INT8) ||
        tensor_shape(v, tensor, dims, 8, &count) < 0 || count != 3 ||
        dims[0] != 1 || dims[1] <= 0 || dims[2] != 40)
        return fail(reason, reason_size, "model input is not int8 [1, N, 40]");
    if (tensor_named(v, tensors, (uint32_t)output_index, &tensor) < 0 ||
        !tensor_type_is(v, tensor, TFLITE_TENSOR_UINT8) ||
        tensor_shape(v, tensor, dims, 8, &count) < 0 || count != 2 ||
        dims[0] != 1 || dims[1] != 1)
        return fail(reason, reason_size, "model output is not uint8 [1, 1]");

    for (index = 0; index < operator_count; ++index) {
        size_t op;
        uint32_t opcode_index;
        int present;

        if (fb_vector_table(v, operators, index, &op) < 0)
            return fail(reason, reason_size, "model operator table is invalid");
        if (fb_table_u32(v, op, 0, &opcode_index, &present) < 0 || !present ||
            opcode_index >= opcode_count)
            return fail(reason, reason_size, "model operator has no opcode");
    }
    return 0;
}

/* Checks the Model table's operator_codes vector. */
static int check_operator_codes(const struct fb_view *v, size_t model,
                                char *reason, size_t reason_size)
{
    size_t opcodes;
    uint32_t count, i;

    if (fb_vector(v, model, 1, &opcodes, &count) <= 0 || count == 0)
        return fail(reason, reason_size, "model has no operator codes");
    for (i = 0; i < count; ++i) {
        size_t code_table, custom;
        uint32_t builtin = 0;
        unsigned deprecated = 0;
        int present_builtin = 0, present_deprecated = 0;
        int rc;

        if (fb_vector_table(v, opcodes, i, &code_table) < 0)
            return fail(reason, reason_size, "model operator code is invalid");
        rc = fb_table_u32(v, code_table, 3, &builtin, &present_builtin);
        if (rc < 0)
            return fail(reason, reason_size, "model operator code is invalid");
        if (!present_builtin) {
            rc = fb_table_byte(v, code_table, 0, &deprecated, &present_deprecated);
            if (rc < 0)
                return fail(reason, reason_size, "model operator code is invalid");
            builtin = present_deprecated ? deprecated : 0;
        }
        if (!operator_allowed(builtin))
            return fail(reason, reason_size, "model uses operators outside the contract");
        /* A custom operator is never in the resolver list. */
        if (fb_offset_target(v, code_table, 1, &custom) == 1) {
            uint32_t length;
            if (fb_u32(v, custom, &length) < 0)
                return fail(reason, reason_size, "model operator code is invalid");
            if (length > 0)
                return fail(reason, reason_size, "model uses operators outside the contract");
        }
    }
    return 0;
}

static int tflite_check(const unsigned char *data, size_t size, char *reason,
                        size_t reason_size)
{
    struct fb_view v;
    uint32_t root, count;
    size_t subgraphs, subgraph, model;

    if (!data || size < 8)
        return fail(reason, reason_size, "model is not a TFLite flatbuffer");
    v.p = data;
    v.n = size;
    if (fb_u32(&v, 0, &root) < 0 || root == 0 || root >= size)
        return fail(reason, reason_size, "model is not a TFLite flatbuffer");
    model = root;
    if (fb_vector(&v, model, 2, &subgraphs, &count) <= 0 || count != 1)
        return fail(reason, reason_size, "model must have exactly one subgraph");
    if (fb_vector_table(&v, subgraphs, 0, &subgraph) < 0)
        return fail(reason, reason_size, "model subgraph is invalid");
    if (check_operator_codes(&v, model, reason, reason_size) < 0)
        return -1;
    return check_subgraph(&v, model, subgraph, reason, reason_size);
}

/* ---- manifest ---- */

static const struct json_member *member_named(const struct json_member *members,
                                              size_t count, const char *name)
{
    size_t i, length = strlen(name);

    for (i = 0; i < count; ++i)
        if (members[i].name_len == length && !memcmp(members[i].name, name, length))
            return &members[i];
    return NULL;
}

static int member_double(const struct json_member *member, double *out)
{
    char text[64], *end;
    size_t length;

    if (!member || !out || member->value_len == 0 || member->value_len >= sizeof(text))
        return -1;
    length = member->value_len;
    memcpy(text, member->value, length);
    text[length] = '\0';
    errno = 0;
    *out = strtod(text, &end);
    if (end != text + length || errno == ERANGE || !isfinite(*out))
        return -1;
    return 0;
}

static int manifest_check(const unsigned char *data, size_t size, const char *name,
                          char *phrase, size_t phrase_size, char *reason,
                          size_t reason_size)
{
    struct json_member members[32], micro_members[16];
    size_t count = 0, micro_count = 0;
    const struct json_member *member;
    char type[16], model[LE_WAKE_MODEL_NAME_MAX + 8], word[LE_WAKE_MODEL_PHRASE_MAX];
    char expected_model[LE_WAKE_MODEL_NAME_MAX + 8];
    int version = -1, sliding = 0, step = 0, arena = 0;
    double cutoff = 0.0;

    if (json_object_members((const char *)data, size, members,
                            sizeof(members) / sizeof(members[0]), &count) < 0)
        return fail(reason, reason_size, "manifest is not a JSON object");
    member = member_named(members, count, "type");
    if (!member || json_string_span(member->value, member->value_len,
                                    type, sizeof(type)) < 0 || strcmp(type, "micro"))
        return fail(reason, reason_size, "manifest type is not micro");
    member = member_named(members, count, "version");
    if (!member || json_int_span(member->value, member->value_len, &version) < 0 ||
        version != 2)
        return fail(reason, reason_size, "manifest version is not 2");
    member = member_named(members, count, "wake_word");
    if (!member || json_string_span(member->value, member->value_len,
                                    word, sizeof(word)) < 0 || !word[0])
        return fail(reason, reason_size, "manifest has no wake_word");
    member = member_named(members, count, "model");
    if (!member || json_string_span(member->value, member->value_len,
                                    model, sizeof(model)) < 0)
        return fail(reason, reason_size, "manifest has no model file");
    if (le_wake_model_file_name(name, 1, expected_model, sizeof(expected_model)) < 0 ||
        strcmp(model, expected_model))
        return fail(reason, reason_size, "manifest model must name <name>.tflite");
    member = member_named(members, count, "micro");
    if (!member ||
        json_object_members(member->value, member->value_len, micro_members,
                            sizeof(micro_members) / sizeof(micro_members[0]),
                            &micro_count) < 0)
        return fail(reason, reason_size, "manifest has no micro object");
    member = member_named(micro_members, micro_count, "probability_cutoff");
    if (!member || member_double(member, &cutoff) < 0 || cutoff <= 0.0 || cutoff > 1.0)
        return fail(reason, reason_size, "manifest probability_cutoff is invalid");
    member = member_named(micro_members, micro_count, "sliding_window_size");
    if (!member || json_int_span(member->value, member->value_len, &sliding) < 0 ||
        sliding <= 0)
        return fail(reason, reason_size, "manifest sliding_window_size is invalid");
    member = member_named(micro_members, micro_count, "feature_step_size");
    if (!member || json_int_span(member->value, member->value_len, &step) < 0 ||
        step <= 0)
        return fail(reason, reason_size, "manifest feature_step_size is invalid");
    member = member_named(micro_members, micro_count, "tensor_arena_size");
    if (!member || json_int_span(member->value, member->value_len, &arena) < 0 ||
        arena <= 0)
        return fail(reason, reason_size, "manifest tensor_arena_size is invalid");
    if ((unsigned long)arena > LE_WAKE_MODEL_ARENA_MAX) {
        char message[160];
        snprintf(message, sizeof(message),
                 "manifest tensor_arena_size exceeds the %lu byte cap",
                 (unsigned long)LE_WAKE_MODEL_ARENA_MAX);
        return fail(reason, reason_size, message);
    }
    if (phrase && phrase_size) {
        if (strlen(word) >= phrase_size)
            return fail(reason, reason_size, "manifest wake_word is too long");
        memcpy(phrase, word, strlen(word) + 1);
    }
    return 0;
}

int le_wake_model_phrase_for(const char *model_directory, char *phrase, size_t phrase_size)
{
    char name[LE_WAKE_MODEL_NAME_MAX], path[1200], file[LE_WAKE_MODEL_NAME_MAX + 8];
    unsigned char *data = NULL;
    size_t size = 0;
    int rc;

    if (!model_directory || !phrase || !phrase_size)
        return -1;
    if (directory_name(model_directory, name, sizeof(name)) < 0 ||
        le_wake_model_file_name(name, 0, file, sizeof(file)) < 0 ||
        snprintf(path, sizeof(path), "%s/%s", model_directory, file) >= (int)sizeof(path))
        return -1;
    if (read_regular_file(path, file, LE_WAKE_MODEL_MANIFEST_MIN_BYTES,
                          LE_WAKE_MODEL_MANIFEST_MAX_BYTES,
                          &data, &size, NULL, 0) == 0) {
        char reason[160];
        rc = manifest_check(data, size, name, phrase, phrase_size, reason, sizeof(reason));
        free(data);
        if (rc == 0)
            return 0;
    }
    return prettify(name, phrase, phrase_size);
}

int le_wake_model_label(const char *model_directory, char *phrase, size_t phrase_size,
                        char *model, size_t model_size)
{
    char name[LE_WAKE_MODEL_NAME_MAX];

    if (!model_directory || !phrase || !model || !phrase_size || !model_size)
        return -1;
    if (directory_name(model_directory, name, sizeof(name)) < 0)
        return -1;
    if (!strcmp(name, LE_WAKE_MODEL_STOCK_NAME)) {
        if (snprintf(model, model_size, "%s", LE_WAKE_MODEL_STOCK_NAME) >= (int)model_size)
            return -1;
    } else {
        if (!le_wake_model_name_valid(name))
            return -1;
        if (snprintf(model, model_size, "%s", name) >= (int)model_size)
            return -1;
    }
    if (le_wake_model_phrase_for(model_directory, phrase, phrase_size) < 0) {
        if (snprintf(phrase, phrase_size, "%s",
                     !strcmp(name, LE_WAKE_MODEL_STOCK_NAME)
                         ? LE_WAKE_MODEL_STOCK_PHRASE
                         : name) >= (int)phrase_size)
            return -1;
    }
    return 0;
}

int le_wake_model_validate_file(const char *path, const char *file_name,
                                char *reason, size_t reason_size)
{
    char name[LE_WAKE_MODEL_NAME_MAX];
    unsigned char *data = NULL;
    size_t size = 0;
    int rc;

    if (!path || !file_name)
        return fail(reason, reason_size, "model file is missing");
    {
        const char *base = path_basename(file_name);
        size_t length = strlen(base), suffix;

        if (length > 5 && !strcmp(base + length - 5, ".json"))
            suffix = 5;
        else if (length > 7 && !strcmp(base + length - 7, ".tflite"))
            suffix = 7;
        else
            return fail(reason, reason_size, "unexpected model file");
        if (length - suffix == 0 || length - suffix >= sizeof(name))
            return fail(reason, reason_size, "unexpected model file");
        memcpy(name, base, length - suffix);
        name[length - suffix] = '\0';
        if (!le_wake_model_name_valid(name) || !le_wake_model_file_known(name, base))
            return fail(reason, reason_size, "unexpected model file");
    }
    {
        const char *base = path_basename(file_name);
        size_t length = strlen(base);
        if (length > 5 && !strcmp(base + length - 5, ".json")) {
            if (read_regular_file(path, base, LE_WAKE_MODEL_MANIFEST_MIN_BYTES,
                                  LE_WAKE_MODEL_MANIFEST_MAX_BYTES,
                                  &data, &size, reason, reason_size) < 0)
                return -1;
            rc = manifest_check(data, size, name, NULL, 0, reason, reason_size);
            free(data);
            return rc;
        }
    }
    if (read_regular_file(path, path_basename(file_name), LE_WAKE_MODEL_MIN_BYTES,
                          LE_WAKE_MODEL_MAX_BYTES,
                          &data, &size, reason, reason_size) < 0)
        return -1;
    rc = tflite_check(data, size, reason, reason_size);
    free(data);
    return rc;
}

int le_wake_model_validate_directory(const char *directory, char *reason, size_t reason_size)
{
    char name[LE_WAKE_MODEL_NAME_MAX], path[1200];
    char manifest_name[LE_WAKE_MODEL_NAME_MAX + 8];
    char tflite_name[LE_WAKE_MODEL_NAME_MAX + 8];
    struct stat st;
    DIR *dir;
    struct dirent *entry;
    int found = 0, unexpected = 0;

    if (!directory)
        return fail(reason, reason_size, "model directory is missing");
    if (lstat(directory, &st) < 0 || !S_ISDIR(st.st_mode))
        return fail(reason, reason_size, "model directory is missing");
    if (directory_name(directory, name, sizeof(name)) < 0 ||
        !le_wake_model_name_valid(name))
        return fail(reason, reason_size, "model folder name is invalid");
    if (le_wake_model_file_name(name, 0, manifest_name, sizeof(manifest_name)) < 0 ||
        le_wake_model_file_name(name, 1, tflite_name, sizeof(tflite_name)) < 0)
        return fail(reason, reason_size, "model folder name is invalid");
    dir = opendir(directory);
    if (!dir)
        return fail(reason, reason_size, "model directory could not be read");
    while ((entry = readdir(dir)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        if (strcmp(entry->d_name, manifest_name) && strcmp(entry->d_name, tflite_name))
            unexpected = 1;
        else
            ++found;
    }
    closedir(dir);
    if (unexpected)
        return fail(reason, reason_size, "model folder holds an unexpected file");
    if (found != LE_WAKE_MODEL_FILE_COUNT)
        return fail(reason, reason_size,
                    "model folder must hold exactly <name>.tflite and <name>.json");
    if (snprintf(path, sizeof(path), "%s/%s", directory, manifest_name) >=
            (int)sizeof(path) ||
        le_wake_model_validate_file(path, manifest_name, reason, reason_size) < 0)
        return -1;
    if (snprintf(path, sizeof(path), "%s/%s", directory, tflite_name) >=
            (int)sizeof(path) ||
        le_wake_model_validate_file(path, tflite_name, reason, reason_size) < 0)
        return -1;
    return 0;
}
