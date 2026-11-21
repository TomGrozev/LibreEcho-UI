#ifndef LIBREECHO_AUDIO_PROFILE_H
#define LIBREECHO_AUDIO_PROFILE_H

/*
 * Per-image audio policy.  The image build renders /etc/libreecho/audio-profile
 * from the Platform target table; the engine, audiod, micd and the web daemon
 * all read the same file so a target describes its PCM layout once.
 *
 * Every key is optional here.  A file that carries only the original seven
 * keys, or no file at all, yields the MT8163 layout below byte for byte.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LE_AUDIO_PROFILE_PATH "/etc/libreecho/audio-profile"
#define LE_AUDIO_PROFILE_PATH_HOST "/proc/1/root/etc/libreecho/audio-profile"
#define LE_AUDIO_PROFILE_MAX_CHANNELS 8
/* The MT8163 default describes nine transport lanes (seven microphones and
 * two AEC lanes), one more than a profile file may declare, so the role
 * tables must hold the default as well. */
#define LE_AUDIO_PROFILE_ROLE_SLOTS 9
#define LE_AUDIO_PROFILE_TEXT 1024

enum le_audio_format {
    LE_AUDIO_FORMAT_S16_LE = 0,
    LE_AUDIO_FORMAT_S24_3LE,
    LE_AUDIO_FORMAT_S32_LE
};

enum le_audio_aec_source {
    LE_AUDIO_AEC_IN_STREAM = 0,
    LE_AUDIO_AEC_SEPARATE_PCM
};

/* The microphone roles are contiguous so a parsed slot maps to a logical
 * lane by subtract; AEC and line-in sit above them and must never collide
 * with a microphone index. */
enum le_audio_role {
    LE_AUDIO_ROLE_UNUSED = 0,
    LE_AUDIO_ROLE_MIC0,
    LE_AUDIO_ROLE_MIC1,
    LE_AUDIO_ROLE_MIC2,
    LE_AUDIO_ROLE_MIC3,
    LE_AUDIO_ROLE_MIC4,
    LE_AUDIO_ROLE_MIC5,
    LE_AUDIO_ROLE_MIC6,
    LE_AUDIO_ROLE_AEC,
    LE_AUDIO_ROLE_LINE_IN
};

typedef char le_audio_mic_roles_contiguous[
    LE_AUDIO_ROLE_MIC6 - LE_AUDIO_ROLE_MIC0 + 1 == 7 ? 1 : -1];

struct le_audio_profile {
    char target_id[32];
    char speaker_chain[24];
    char capture_chain[16];

    int playback_card;
    int playback_device;

    int capture_card;
    int capture_device;
    unsigned int capture_rate;
    unsigned int capture_channels;
    enum le_audio_format capture_format;
    int capture_roles[LE_AUDIO_PROFILE_ROLE_SLOTS];

    enum le_audio_aec_source aec_source;
    int aec_card;
    int aec_device;
    unsigned int aec_rate;
    unsigned int aec_channels;
    enum le_audio_format aec_format;
};

static inline int le_audio_profile_bits(enum le_audio_format format)
{
    switch (format) {
    case LE_AUDIO_FORMAT_S16_LE: return 16;
    case LE_AUDIO_FORMAT_S24_3LE: return 24;
    case LE_AUDIO_FORMAT_S32_LE: return 32;
    }
    return 16;
}

static inline size_t le_audio_profile_bytes(enum le_audio_format format)
{
    return (size_t)le_audio_profile_bits(format) / 8U;
}

static inline const char *le_audio_profile_format_name(
    enum le_audio_format format)
{
    switch (format) {
    case LE_AUDIO_FORMAT_S16_LE: return "s16_le";
    case LE_AUDIO_FORMAT_S24_3LE: return "s24_3le";
    case LE_AUDIO_FORMAT_S32_LE: return "s32_le";
    }
    return "s16_le";
}

static inline int le_audio_profile_format_parse(const char *text,
                                                enum le_audio_format *out)
{
    if (!strcmp(text, "s16_le")) *out = LE_AUDIO_FORMAT_S16_LE;
    else if (!strcmp(text, "s24_3le")) *out = LE_AUDIO_FORMAT_S24_3LE;
    else if (!strcmp(text, "s32_le")) *out = LE_AUDIO_FORMAT_S32_LE;
    else return -1;
    return 0;
}

static inline int le_audio_role_parse(const char *text)
{
    if (!strcmp(text, "unused")) return LE_AUDIO_ROLE_UNUSED;
    if (!strcmp(text, "aec")) return LE_AUDIO_ROLE_AEC;
    if (!strcmp(text, "line-in")) return LE_AUDIO_ROLE_LINE_IN;
    if (text[0] == 'm' && text[1] == 'i' && text[2] == 'c' &&
        text[3] >= '0' && text[3] <= '6' && text[4] == '\0')
        return LE_AUDIO_ROLE_MIC0 + (text[3] - '0');
    return -1;
}

static inline int le_audio_profile_roles(const char *text, int *roles,
                                         unsigned int channels)
{
    char copy[128], *token, *save = NULL;
    unsigned int channel = 0;

    if (strlen(text) >= sizeof(copy))
        return -1;
    strcpy(copy, text);
    for (token = strtok_r(copy, ",", &save); token;
         token = strtok_r(NULL, ",", &save)) {
        int role = le_audio_role_parse(token);

        if (role < 0 || channel >= channels ||
            channel >= LE_AUDIO_PROFILE_ROLE_SLOTS)
            return -1;
        roles[channel++] = role;
    }
    return channel == channels ? 0 : -1;
}

/* Missing keys mean the MT8163 layout: card 0, device 23 playback and
 * device 24 capture, 9-channel packed 24-bit microphones with the two
 * electrical AEC lanes last. */
static inline void le_audio_profile_defaults(struct le_audio_profile *profile)
{
    unsigned int i;

    memset(profile, 0, sizeof(*profile));
    strcpy(profile->speaker_chain, "radar_puffin");
    strcpy(profile->capture_chain, "radar");
    profile->playback_card = 0;
    profile->playback_device = 23;
    profile->capture_card = 0;
    profile->capture_device = 24;
    profile->capture_rate = 16000;
    profile->capture_channels = 9;
    profile->capture_format = LE_AUDIO_FORMAT_S24_3LE;
    for (i = 0; i < 9; ++i)
        profile->capture_roles[i] = i < 7 ? LE_AUDIO_ROLE_MIC0 + (int)i
                                          : LE_AUDIO_ROLE_AEC;
    profile->aec_source = LE_AUDIO_AEC_IN_STREAM;
    profile->aec_card = 0;
    profile->aec_device = 24;
    profile->aec_rate = 16000;
    profile->aec_channels = 2;
    profile->aec_format = LE_AUDIO_FORMAT_S24_3LE;
}

/* Returns 1 parsed, 0 missing, -1 present but rejected, -2 unreadable. */
static inline int le_audio_profile_read(const char *path,
                                        struct le_audio_profile *profile)
{
    char text[LE_AUDIO_PROFILE_TEXT];
    char role_text[LE_AUDIO_PROFILE_TEXT / 4];
    const char *keys[32];
    char *line, *save = NULL;
    struct le_audio_profile parsed;
    unsigned int key_count = 0;
    int has_schema = 0;
    int fd;
    ssize_t n;

    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return errno == ENOENT || errno == ENOTDIR ? 0 : -2;
    n = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (n <= 0 || (size_t)n >= sizeof(text) - 1)
        return -1;
    text[n] = '\0';
    role_text[0] = '\0';
    le_audio_profile_defaults(&parsed);

    for (line = strtok_r(text, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *value = strchr(line, '=');
        unsigned long number;
        char *end;
        unsigned int key;

        if (!value)
            return -1;
        *value++ = '\0';
        /* Reject a repeated key: taking the last occurrence would make the
         * policy depend on line order. */
        for (key = 0; key < key_count; ++key)
            if (!strcmp(keys[key], line))
                return -1;
        if (key_count >= sizeof(keys) / sizeof(keys[0]))
            return -1;
        keys[key_count++] = line;
        if (!strcmp(line, "schema")) {
            if (strcmp(value, "1"))
                return -1;
            has_schema = 1;
        } else if (!strcmp(line, "target_id")) {
            if (!*value || strlen(value) >= sizeof(parsed.target_id))
                return -1;
            strcpy(parsed.target_id, value);
        } else if (!strcmp(line, "speaker_chain")) {
            if (!strcmp(value, "radar_puffin") || !strcmp(value, "biscuit") ||
                !strcmp(value, "lidar")) {
                if (strlen(value) >= sizeof(parsed.speaker_chain))
                    return -1;
                strcpy(parsed.speaker_chain, value);
            } else return -1;
        } else if (!strcmp(line, "capture_chain")) {
            if (!strcmp(value, "radar") || !strcmp(value, "lidar")) {
                strcpy(parsed.capture_chain, value);
            } else return -1;
        } else if (!strcmp(line, "codec_profile")) {
            /* Validated for the image build; only the engine applies it. */
            if (strcmp(value, "Radar") && strcmp(value, "Flat"))
                return -1;
        } else if (!strcmp(line, "hp_driver_gain") ||
                   !strcmp(line, "lo_driver_gain")) {
            errno = 0;
            number = strtoul(value, &end, 10);
            if (errno || end == value || *end || number > 35)
                return -1;
        } else if (!strcmp(line, "pre_gain_db") ||
                   !strcmp(line, "bass_makeup_db")) {
            double db;

            errno = 0;
            db = strtod(value, &end);
            if (errno || end == value || *end ||
                !(db >= 0.0 && db <= (line[0] == 'p' ? 12.0 : 6.0)))
                return -1;
        } else if (!strcmp(line, "playback_card") ||
                   !strcmp(line, "playback_device") ||
                   !strcmp(line, "capture_card") ||
                   !strcmp(line, "capture_device") ||
                   !strcmp(line, "aec_card") ||
                   !strcmp(line, "aec_device")) {
            errno = 0;
            number = strtoul(value, &end, 10);
            if (errno || end == value || *end || number > 31)
                return -1;
            if (!strcmp(line, "playback_card")) parsed.playback_card = (int)number;
            else if (!strcmp(line, "playback_device")) parsed.playback_device = (int)number;
            else if (!strcmp(line, "capture_card")) parsed.capture_card = (int)number;
            else if (!strcmp(line, "capture_device")) parsed.capture_device = (int)number;
            else if (!strcmp(line, "aec_card")) parsed.aec_card = (int)number;
            else parsed.aec_device = (int)number;
        } else if (!strcmp(line, "playback_rate") ||
                   !strcmp(line, "capture_rate") ||
                   !strcmp(line, "aec_rate")) {
            errno = 0;
            number = strtoul(value, &end, 10);
            if (errno || end == value || *end ||
                (number != 8000 && number != 16000 && number != 44100 &&
                 number != 48000 && number != 96000 && number != 192000))
                return -1;
            if (!strcmp(line, "capture_rate")) parsed.capture_rate = (unsigned int)number;
            else if (!strcmp(line, "aec_rate")) parsed.aec_rate = (unsigned int)number;
        } else if (!strcmp(line, "playback_channels") ||
                   !strcmp(line, "capture_channels") ||
                   !strcmp(line, "aec_channels")) {
            errno = 0;
            number = strtoul(value, &end, 10);
            if (errno || end == value || *end || number < 1 ||
                number > LE_AUDIO_PROFILE_MAX_CHANNELS)
                return -1;
            if (!strcmp(line, "capture_channels")) parsed.capture_channels = (unsigned int)number;
            else if (!strcmp(line, "aec_channels")) parsed.aec_channels = (unsigned int)number;
        } else if (!strcmp(line, "playback_format") ||
                   !strcmp(line, "capture_format") ||
                   !strcmp(line, "aec_format")) {
            enum le_audio_format format;

            if (le_audio_profile_format_parse(value, &format))
                return -1;
            if (!strcmp(line, "capture_format")) parsed.capture_format = format;
            else if (!strcmp(line, "aec_format")) parsed.aec_format = format;
        } else if (!strcmp(line, "capture_roles")) {
            if (strlen(value) >= sizeof(role_text))
                return -1;
            strcpy(role_text, value);
        } else if (!strcmp(line, "aec_source")) {
            if (!strcmp(value, "in-stream")) parsed.aec_source = LE_AUDIO_AEC_IN_STREAM;
            else if (!strcmp(value, "separate-pcm")) parsed.aec_source = LE_AUDIO_AEC_SEPARATE_PCM;
            else return -1;
        } else {
            return -1;
        }
    }
    /* Roles are validated after the whole file is read, so key order does
     * not decide whether capture_roles precedes its capture_channels. */
    if (role_text[0] &&
        le_audio_profile_roles(role_text, parsed.capture_roles,
                               parsed.capture_channels))
        return -1;
    if (!has_schema || !parsed.target_id[0])
        return -1;
    *profile = parsed;
    return 1;
}

/* The audio daemons run before or outside the feature chroot, so try the
 * runtime path first and then PID 1's root, as the engine does.  A rejected
 * runtime file is final rather than silently replaced by the host copy. */
static inline int le_audio_profile_load(struct le_audio_profile *profile)
{
    int rc;

    le_audio_profile_defaults(profile);
    rc = le_audio_profile_read(LE_AUDIO_PROFILE_PATH, profile);
    if (rc < 0)
        return rc;
    if (rc == 0) {
        rc = le_audio_profile_read(LE_AUDIO_PROFILE_PATH_HOST, profile);
        /* PID 1's root is unreadable outside its namespace, which says
         * nothing about the image; the defaults stand in that case. */
        if (rc == -2)
            rc = 0;
    }
    return rc;
}

#endif
