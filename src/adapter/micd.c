#include "adapter.h"
#include "audio_profile.h"
#include "log.h"
#include "mic_beamformer.h"
#include "voice_dsp.h"

#include <errno.h>
#include <math.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define MIC_CARD 0
#define MIC_DEVICE 24
#define MIC_RATE LE_VOICE_RATE
#define MIC_RAW_CHANNELS LE_VOICE_RAW_CHANNELS
#define MIC_CAPTURE_CHANNELS LE_VOICE_LOGICAL_CHANNELS
#define MIC_LOGICAL_CHANNELS LE_VOICE_LOGICAL_CHANNELS
#define MIC_BITS 24
#define MICCAL_UNITY LE_VOICE_MICCAL_UNITY

struct micd_config {
    const char *socket_path;
    const char *idme_dir;
    const char *dt_idme_dir;
    const char *pcm_path;
    const char *capture_bin;
    const char *mixer_bin;
    const char *profile_path;
    int pcm_path_set;
    struct le_audio_profile profile;
};

/*
 * The consumer contract is fixed: raw streams are 9 lanes of packed S24_3LE
 * at 16 kHz, lanes 7 and 8 reserved.  The profile decides what the hardware
 * actually captures; this module converts into that contract, so waked,
 * agentd and the web baby monitor never learn the target's layout.
 */
#define MIC_EXPORT_CHANNELS LE_VOICE_RAW_CHANNELS
#define MIC_EXPORT_BYTES LE_VOICE_BYTES_PER_SAMPLE

static volatile sig_atomic_t running = 1;

/*
 * Beam snapshot, shared between the stream worker that runs the beamformer
 * and the main process that answers "beams" requests.  The worker is a
 * forked child, so this lives in an anonymous shared mapping made before any
 * fork.  seq is odd while the worker is writing (a seqlock); readers retry.
 */
struct beam_view {
    volatile unsigned int seq;
    volatile int running;
    unsigned int beam_count;
    int azimuth_deg[LE_BEAM_MAX_BEAMS];
    float power[LE_BEAM_MAX_BEAMS];
    float floor[LE_BEAM_MAX_BEAMS];
    int selected_deg;
    int locked;
    unsigned long hops;
    unsigned long switches;
    long long updated_ms;
    /* enum le_mic_mix, set by "set_mix" and read by the stream worker per hop */
    volatile unsigned int mix_mode;
    /* Array lane used by the "single" mode; -1 is automatic (lowest selected lane). */
    volatile int mix_lane;
};

static struct beam_view *beam_view;

static unsigned int current_mix_mode(void)
{
    return beam_view ? beam_view->mix_mode : LE_MIC_MIX_BEAMFORMER;
}

static int current_mix_lane(void)
{
    return beam_view ? beam_view->mix_lane : -1;
}

/*
 * The mode named by "key" in a JSON object: "beamformer", "average" or
 * "single".  Returns -1 when the key is absent or the value is unknown.
 */
static int mix_mode_from_json(const char *text, const char *key)
{
    char name[24];
    const char *p;
    size_t length = 0;

    if (!text || !key)
        return -1;
    if (!(p = strstr(text, key)))
        return -1;
    p += strlen(key);
    while (*p == ' ' || *p == '\t')
        ++p;
    if (*p++ != ':')
        return -1;
    while (*p == ' ' || *p == '\t')
        ++p;
    if (*p++ != '"')
        return -1;
    while (p[length] && p[length] != '"' && length < sizeof(name) - 1) {
        name[length] = p[length];
        ++length;
    }
    if (p[length] != '"')
        return -1;
    name[length] = '\0';
    return le_mic_mix_parse(name);
}

/* The owner's saved mix settings from LE_MIC_CONFIG are read once at start-up. */
#define MIX_CONFIG_MAX_BYTES 16384

/*
 * The integer named by "key" in a JSON object, which must be within
 * [-1, MIC_LOGICAL_CHANNELS - 1]; -1 is the automatic lane.  Returns -1 when
 * the key is absent or the value is out of range, otherwise 0 with *lane set.
 */
static int lane_from_json(const char *text, const char *key, int *lane)
{
    const char *p;
    char *end;
    long value;

    if (!text || !key || !lane || !(p = strstr(text, key)))
        return -1;
    p += strlen(key);
    while (*p == ' ' || *p == '\t')
        ++p;
    if (*p++ != ':')
        return -1;
    while (*p == ' ' || *p == '\t')
        ++p;
    errno = 0;
    value = strtol(p, &end, 10);
    if (errno || end == p || value < -1 || value >= MIC_LOGICAL_CHANNELS)
        return -1;
    *lane = (int)value;
    return 0;
}

/* The owner's saved mix mode and lane from LE_MIC_CONFIG, read once at start-up. */
static void saved_mix_settings(int *mode, int *lane)
{
    const char *path = getenv("LE_MIC_CONFIG");
    static char text[MIX_CONFIG_MAX_BYTES];
    size_t used = 0;
    FILE *file;
    int value;

    *mode = LE_MIC_MIX_BEAMFORMER;
    *lane = -1;
    if (path && path[0] && (file = fopen(path, "r"))) {
        used = fread(text, 1, sizeof(text) - 1, file);
        (void)fclose(file);
    }
    text[used] = '\0';
    value = mix_mode_from_json(text, "\"microphone_mix\"");
    if (value >= 0)
        *mode = (int)value;
    if (lane_from_json(text, "\"microphone_lane\"", &value) == 0)
        *lane = value;
}

static long long monotonic_ms(void)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void beam_view_publish(const struct le_beamformer *bf)
{
    unsigned int b;

    if (!beam_view)
        return;
    beam_view->seq++;
    __sync_synchronize();
    beam_view->running = 1;
    beam_view->beam_count = bf->beam_count;
    for (b = 0; b < bf->beam_count && b < LE_BEAM_MAX_BEAMS; ++b) {
        beam_view->azimuth_deg[b] = bf->azimuth_deg[b];
        beam_view->power[b] = bf->power[b];
        beam_view->floor[b] = bf->floor[b];
    }
    beam_view->selected_deg = le_beamformer_selected_azimuth(bf);
    beam_view->locked = bf->locked >= 0;
    beam_view->hops++;
    beam_view->switches = bf->switches;
    beam_view->updated_ms = monotonic_ms();
    __sync_synchronize();
    beam_view->seq++;
}

static void beam_view_stop(void)
{
    if (!beam_view)
        return;
    beam_view->seq++;
    __sync_synchronize();
    beam_view->running = 0;
    __sync_synchronize();
    beam_view->seq++;
}

/* Hann-windowed full-scale tone, summed over the scoring band: 0 dB. */
#define BEAM_FULL_SCALE_DB 126.4

static double beam_db(float power)
{
    return 10.0 * log10((double)power + 1.0) - BEAM_FULL_SCALE_DB;
}

/*
 * Rough direction of arrival: the strongest beam, refined by fitting a
 * parabola through it and its two neighbours (beams wrap around).  The
 * contrast is how far the peak stands above the average beam, which says
 * how directional the sound field is; diffuse noise gives close to 0 dB.
 */
static int format_beams(char *buffer, size_t size)
{
    struct beam_view v;
    unsigned int seq, b, peak = 0, tries;
    double db[LE_BEAM_MAX_BEAMS], mean = 0.0, doa, offset = 0.0;
    size_t used;
    int n;

    if (!beam_view)
        return -1;
    for (tries = 0; tries < 100; ++tries) {
        seq = beam_view->seq;
        __sync_synchronize();
        memcpy(&v, (const void *)beam_view, sizeof(v));
        __sync_synchronize();
        if (!(seq & 1U) && seq == beam_view->seq)
            break;
    }
    if (tries == 100)
        return -1;
    if (!v.running || v.beam_count == 0 || v.beam_count > LE_BEAM_MAX_BEAMS) {
        n = snprintf(buffer, size,
                     "{\"running\":false,\"reason\":\"no beamformed stream is "
                     "running; the wake word service starts one\"}");
        return n < 0 || (size_t)n >= size ? -1 : n;
    }
    for (b = 0; b < v.beam_count; ++b) {
        db[b] = beam_db(v.power[b]);
        mean += db[b];
        if (db[b] > db[peak])
            peak = b;
    }
    mean /= v.beam_count;
    {
        double l = db[(peak + v.beam_count - 1) % v.beam_count];
        double c = db[peak], r = db[(peak + 1) % v.beam_count];
        double curve = l - 2.0 * c + r;
        if (curve < -1e-9)
            offset = 0.5 * (l - r) / curve;
        if (offset > 0.5) offset = 0.5;
        if (offset < -0.5) offset = -0.5;
    }
    doa = v.azimuth_deg[peak] + offset * (360.0 / v.beam_count);
    while (doa < 0.0) doa += 360.0;
    while (doa >= 360.0) doa -= 360.0;
    n = snprintf(buffer, size,
                 "{\"running\":true,\"age_ms\":%lld,\"hops\":%lu,"
                 "\"hop_ms\":8,\"switches\":%lu,"
                 "\"reference\":\"0 deg front (volume down), clockwise viewed "
                 "from above\",\"selected_deg\":%d,\"locked\":%s,"
                 "\"doa_deg\":%.1f,\"peak_db\":%.1f,\"contrast_db\":%.1f,"
                 "\"beams\":[",
                 monotonic_ms() - v.updated_ms, v.hops, v.switches,
                 v.selected_deg, v.locked ? "true" : "false",
                 doa, db[peak], db[peak] - mean);
    if (n < 0 || (size_t)n >= size)
        return -1;
    used = (size_t)n;
    for (b = 0; b < v.beam_count; ++b) {
        n = snprintf(buffer + used, size - used,
                     "%s{\"deg\":%d,\"power_db\":%.1f,\"snr_db\":%.1f}",
                     b ? "," : "", v.azimuth_deg[b], db[b],
                     10.0 * log10(((double)v.power[b] + 1.0) /
                                  ((double)v.floor[b] + 1.0)));
        if (n < 0 || (size_t)n >= size - used)
            return -1;
        used += (size_t)n;
    }
    n = snprintf(buffer + used, size - used, "]}");
    if (n < 0 || (size_t)n >= size - used)
        return -1;
    return (int)(used + (size_t)n);
}
static volatile sig_atomic_t child_changed = 0;

static void stop_handler(int signo)
{
    (void)signo;
    running = 0;
}

static void child_handler(int signo)
{
    (void)signo;
    child_changed = 1;
}

static int write_all(int fd, const void *buffer, size_t size)
{
    const char *p = buffer;

    while (size > 0) {
        ssize_t written = send(fd, p, size, MSG_NOSIGNAL);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return -1;
        p += written;
        size -= (size_t)written;
    }
    return 0;
}

static int read_request_line(int fd, char *buffer, size_t size)
{
    size_t used = 0;

    while (used + 1 < size) {
        struct pollfd descriptor;
        ssize_t count;

        descriptor.fd = fd;
        descriptor.events = POLLIN;
        descriptor.revents = 0;
        if (poll(&descriptor, 1, 3000) <= 0)
            return -1;
        count = read(fd, buffer + used, 1);
        if (count <= 0)
            return -1;
        if (buffer[used++] == '\n') {
            buffer[used - 1] = '\0';
            return 0;
        }
    }
    return -1;
}

static int read_miccal_file(const char *path, int *value)
{
    FILE *file;

    file = fopen(path, "r");
    if (!file)
        return 0;
    if (fscanf(file, "%d", value) != 1 || *value < 0 || *value > 65535) {
        fclose(file);
        return 0;
    }
    fclose(file);
    return 1;
}

static int read_miccal_list(const char *path, int values[7])
{
    FILE *file;
    int count = 0;
    int i;

    file = fopen(path, "r");
    if (!file)
        return 0;
    for (i = 0; i < MIC_LOGICAL_CHANNELS; ++i) {
        int value;

        if (fscanf(file, "%d", &value) != 1 ||
            value < 0 || value > 65535)
            break;
        values[i] = value;
        ++count;
    }
    fclose(file);
    return count;
}

static int read_miccal(const char *idme_dir, const char *dt_idme_dir,
                       int values[7])
{
    char path[256];
    int count = 0;
    int i;

    for (i = 0; i < MIC_LOGICAL_CHANNELS; ++i) {
        int value;

        values[i] = MICCAL_UNITY;
        if (snprintf(path, sizeof(path), "%s/miccal.%d",
                     idme_dir, i) >= (int)sizeof(path))
            continue;
        if (read_miccal_file(path, &value)) {
            values[i] = value;
            ++count;
        }
    }
    if (count != 0)
        return count;

    if (snprintf(path, sizeof(path), "%s/miccal",
                 idme_dir) >= (int)sizeof(path))
        return 0;
    {
        count = read_miccal_list(path, values);
    }
    if (count != 0)
        return count;

    for (i = 0; i < MIC_LOGICAL_CHANNELS; ++i) {
        int value;

        if (snprintf(path, sizeof(path), "%s/miccal.%d/value",
                     dt_idme_dir, i) >= (int)sizeof(path))
            continue;
        if (read_miccal_file(path, &value)) {
            values[i] = value;
            ++count;
        }
    }
    return count;
}

static const char *miccal_source(const char *idme_dir,
                                 const char *dt_idme_dir)
{
    static char source[256];
    char path[256];
    int i;

    for (i = 0; i < MIC_LOGICAL_CHANNELS; ++i) {
        if (snprintf(path, sizeof(path), "%s/miccal.%d",
                     idme_dir, i) < (int)sizeof(path) &&
            access(path, R_OK) == 0) {
            snprintf(source, sizeof(source), "%s/miccal.N", idme_dir);
            return source;
        }
    }
    if (snprintf(path, sizeof(path), "%s/miccal", idme_dir) <
            (int)sizeof(path) && access(path, R_OK) == 0) {
        snprintf(source, sizeof(source), "%s/miccal", idme_dir);
        return source;
    }
    for (i = 0; i < MIC_LOGICAL_CHANNELS; ++i) {
        if (snprintf(path, sizeof(path), "%s/miccal.%d/value",
                     dt_idme_dir, i) < (int)sizeof(path) &&
            access(path, R_OK) == 0) {
            snprintf(source, sizeof(source), "%s/miccal.N/value",
                     dt_idme_dir);
            return source;
        }
    }
    return "unavailable";
}

struct mixer_setting {
    const char *control;
    const char *value;
    const char *value2;
};

static int apply_mixer_setting(const char *mixer_bin,
                               const struct mixer_setting *setting)
{
    pid_t child;
    int status;

    child = fork();
    if (child < 0)
        return -1;
    if (child == 0) {
        char *const argv[] = {
            (char *)"tinymix", (char *)"set",
            (char *)setting->control, (char *)setting->value,
            (char *)setting->value2, NULL
        };
        int null_fd = open("/dev/null", O_WRONLY);

        if (null_fd >= 0) {
            (void)dup2(null_fd, STDOUT_FILENO);
            (void)dup2(null_fd, STDERR_FILENO);
            if (null_fd > STDERR_FILENO)
                close(null_fd);
        }
        execv(mixer_bin, argv);
        _exit(127);
    }
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

/*
 * Post-mixdown digital gain for the calibrated mono stream.
 *
 * The analogue PGA cannot reach the level the wake model needs.  Scoring
 * the device's own capture through the reference wake model, the
 * stream peaks at 0.41 against a 0.45 accept gate with one supporting
 * frame where two are required -- a near miss on every attempt.  The same
 * audio amplified scores 0.70 at three times and 0.80 at six, both of
 * which fire.  Raising the PGA further does not work: measured on
 * hardware, MICPGA 110 is about seven times quieter than 80, so the
 * control is not monotonic across its nominal range.
 *
 * Applying the gain here instead reproduces exactly what was measured,
 * and it lands on both consumers of this stream -- the wake detector and
 * speech recognition, which was transcribing single characters from the
 * same too-quiet audio.
 *
 * Digital gain does not improve signal to noise; it scales both.  That is
 * fine for a level-sensitive model, but it means any VAD floor configured
 * against the old level has to scale with it, or end-of-speech detection
 * breaks in the way it did before.
 */
#define MIC_DIGITAL_GAIN_UNITY 100U
#define MIC_DIGITAL_GAIN_DEFAULT 400U
#define MIC_DIGITAL_GAIN_MAX 1600U

static unsigned int digital_gain_from_environment(void)
{
    const char *text = getenv("LE_MIC_DIGITAL_GAIN");
    char *end;
    unsigned long parsed;

    if (!text || !text[0])
        return MIC_DIGITAL_GAIN_DEFAULT;
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno || *end || parsed < MIC_DIGITAL_GAIN_UNITY ||
        parsed > MIC_DIGITAL_GAIN_MAX)
        return MIC_DIGITAL_GAIN_DEFAULT;
    return (unsigned int)parsed;
}

/*
 * Microphones fed to the fixed beams.  The default is the stock speech set
 * (lanes 0, 1, 3 and 4); LE_MIC_BEAM_MICS="all" or a list such as "0,1,3,4,6"
 * selects another subset for comparison.  An invalid value falls back to the
 * default rather than failing the stream.
 */
static unsigned int beam_mic_mask(void)
{
    unsigned int mask = le_beamformer_parse_mics(getenv("LE_MIC_BEAM_MICS"));

    return mask && (mask & (mask - 1U)) ? mask : LE_BEAM_DEFAULT_MIC_MASK;
}

/* Writes the selected lanes as "0,1,3,4"; needs 14 bytes for all seven. */
static void format_mic_list(unsigned int mask, char *out, size_t size)
{
    size_t used = 0;
    unsigned int i;

    for (i = 0; i < LE_VOICE_LOGICAL_CHANNELS && used + 2 < size; ++i) {
        if (!(mask & (1U << i)))
            continue;
        if (used)
            out[used++] = ',';
        out[used++] = (char)('0' + i);
    }
    out[used] = '\0';
}

static void apply_digital_gain(int16_t *samples, size_t count,
                               unsigned int gain_hundredths)
{
    size_t i;

    if (gain_hundredths == MIC_DIGITAL_GAIN_UNITY)
        return;
    for (i = 0; i < count; ++i) {
        int32_t scaled = (int32_t)((samples[i] * (int32_t)gain_hundredths) /
                                   (int32_t)MIC_DIGITAL_GAIN_UNITY);

        if (scaled > INT16_MAX)
            scaled = INT16_MAX;
        else if (scaled < INT16_MIN)
            scaled = INT16_MIN;
        samples[i] = (int16_t)scaled;
    }
}

/*
 * Map the owner's 0..100 microphone gain onto the codec's MICPGA control.
 *
 * The four MICPGA volumes were fixed at 40, and configure_capture_path()
 * runs on every capture start -- not just at boot -- so anything that set
 * the gain elsewhere was overwritten as soon as the wake word listener
 * reopened the stream.  Restoring the level in audiod could never hold for
 * that reason, whatever order the daemons started in.
 *
 * 50 maps to 40 so an unconfigured device keeps exactly the level it had,
 * and the scale runs to 80 at 100, which is the control's maximum:
 *
 *   ADC_A MICPGA Volume Ctrl   080, 80  (range 0->80)
 *
 * Do not raise this ceiling.  An earlier attempt at 110 was out of range,
 * and rather than being clamped it left the capture about seven times
 * quieter than 80 -- so an out-of-range write here is not a no-op, it is
 * actively harmful.  Level beyond this comes from the digital stage above.  The upper half only
 * became usable once the 24-bit capture scaling was fixed; before that the
 * stream saturated regardless of any gain.
 *
 * The top of the range matters.  Scoring real captured audio through the
 * reference wake model, the device's own stream peaked at 0.41 against
 * a 0.45 gate with a single supporting frame -- a near miss on every
 * attempt.  The same audio amplified three times scores 0.70 with two
 * supporting frames and would fire, and six times scores 0.80.  The word
 * was being recognised all along and simply arriving too quiet.
 */
#define MICPGA_DEFAULT_VOLUME 40
#define MICPGA_MAX_VOLUME 80

#define MIC_CONFIG_MAX_BYTES 16384

/*
 * The owner's saved gain, read from the persisted web configuration named by
 * LE_MIC_CONFIG, or -1.  This is read on every capture start rather than once
 * at boot: LE_MIC_CAPTURE_GAIN is fixed when micd starts, so a gain changed
 * afterwards was reverted to the boot-time value the next time the wake
 * listener reopened the stream.
 */
static long saved_capture_gain(void)
{
    const char *path = getenv("LE_MIC_CONFIG");
    char text[MIC_CONFIG_MAX_BYTES];
    const char *p;
    char *end;
    long percent;
    size_t used;
    FILE *file;

    if (!path || !path[0] || !(file = fopen(path, "r")))
        return -1;
    used = fread(text, 1, sizeof(text) - 1, file);
    (void)fclose(file);
    text[used] = '\0';
    if (!(p = strstr(text, "\"microphone_gain\"")))
        return -1;
    p += strlen("\"microphone_gain\"");
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        ++p;
    if (*p++ != ':')
        return -1;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        ++p;
    if (*p < '0' || *p > '9')
        return -1;
    errno = 0;
    percent = strtol(p, &end, 10);
    if (errno || percent > 100 || (*end != ',' && *end != '}' &&
        *end != ' ' && *end != '\t' && *end != '\r' && *end != '\n'))
        return -1;
    return percent;
}

/* Owner-facing 0..100 gain; the per-codec volume range is applied below. */
static int capture_gain_percent(void)
{
    const char *text = getenv("LE_MIC_CAPTURE_GAIN");
    char *end;
    long percent = saved_capture_gain();

    if (percent >= 0 && percent <= 100)
        return (int)percent;
    if (!text || !text[0])
        return MICPGA_DEFAULT_VOLUME * 100 / MICPGA_MAX_VOLUME;
    errno = 0;
    percent = strtol(text, &end, 10);
    if (errno || *end || percent < 0 || percent > 100)
        return MICPGA_DEFAULT_VOLUME * 100 / MICPGA_MAX_VOLUME;
    return (int)percent;
}

static int micpga_volume_from_environment(void)
{
    return capture_gain_percent() * MICPGA_MAX_VOLUME / 100;
}

static int configure_lidar_capture_path(const struct micd_config *config)
{
    static const char *const adc[4] = { "ADC1", "ADC2", "ADC3", "ADC4" };
    char volume[16];
    unsigned int i;

    /* The Lidar PGA range is 0..80 here, so a percentage matches audiod's
     * own writer. */
    (void)snprintf(volume, sizeof(volume), "%d%%", capture_gain_percent());
    for (i = 0; i < 4; ++i) {
        struct mixer_setting settings[2];
        char left[64], right[64];

        (void)snprintf(left, sizeof(left),
                       "%s Left Input DIF_2L_3L Capture Switch", adc[i]);
        (void)snprintf(right, sizeof(right),
                       "%s Right Input DIF_2R_3R Capture Switch", adc[i]);
        settings[0].control = left;
        settings[0].value = "1";
        settings[0].value2 = NULL;
        settings[1].control = right;
        settings[1].value = "1";
        settings[1].value2 = NULL;
        {
            char pga_volume[48];

            (void)snprintf(pga_volume, sizeof(pga_volume),
                           "%s PGA Capture Volume", adc[i]);
            if (apply_mixer_setting(config->mixer_bin, &settings[0]) < 0 ||
                apply_mixer_setting(config->mixer_bin, &settings[1]) < 0) {
                le_log_error("micd: Lidar ADC %s setup failed", adc[i]);
                return -1;
            }
            settings[0].control = pga_volume;
            settings[0].value = volume;
            settings[0].value2 = volume;
            if (apply_mixer_setting(config->mixer_bin, &settings[0]) < 0) {
                le_log_warn("micd: capture PGA %s refused for %s", volume,
                            pga_volume);
                return -1;
            }
        }
    }
    le_log_info("micd: Lidar OCM capture path configured (PGA %s)", volume);
    return 0;
}

static int configure_capture_path(const struct micd_config *config)
{
    char micpga[16];
    char default_micpga[8];
    struct mixer_setting micpga_settings[4];
    static const char *const micpga_controls[4] = {
        "ADC_A MICPGA Volume Ctrl", "ADC_B MICPGA Volume Ctrl",
        "ADC_C MICPGA Volume Ctrl", "ADC_D MICPGA Volume Ctrl"
    };
    static const struct mixer_setting settings[] = {
        {"Mic PGA Switch", "1", "1"},
        {"ADCFGA Left Mute Switch", "0", NULL},
        {"ADCFGA Right Mute Switch", "0", NULL},
        {"ADC_A Left Ip Select ADC_A DIF1_L switch", "1", NULL},
        {"ADC_A Right Ip Select ADC_A DIF1_R switch", "1", NULL},
        {"ADC_B Left Ip Select ADC_B DIF1_L switch", "1", NULL},
        {"ADC_B Right Ip Select ADC_B DIF1_R switch", "1", NULL},
        {"ADC_C Left Ip Select ADC_C DIF1_L switch", "1", NULL},
        {"ADC_C Right Ip Select ADC_C DIF1_R switch", "1", NULL},
        {"ADC_D Left Ip Select ADC_D DIF1_L switch", "1", NULL},
        {"ADC_D Right Ip Select ADC_D DIF1_R switch", "1", NULL},
        {"ADC_A DIF1_L Input Gain", "1", NULL},
        {"ADC_A DIF1_R Input Gain", "1", NULL},
        {"ADC_B DIF1_L Input Gain", "1", NULL},
        {"ADC_B DIF1_R Input Gain", "1", NULL},
        {"ADC_C DIF1_L Input Gain", "1", NULL},
        {"ADC_C DIF1_R Input Gain", "1", NULL},
        {"ADC_D DIF1_L Input Gain", "1", NULL},
        {"ADC_D DIF1_R Input Gain", "1", NULL},
        {"SpiTimeStamps", "0", NULL}
    };
    size_t i;

    if (!strcmp(config->profile.capture_chain, "lidar"))
        return configure_lidar_capture_path(config);
    for (i = 0; i < sizeof(settings) / sizeof(settings[0]); ++i) {
        if (apply_mixer_setting(config->mixer_bin, &settings[i]) < 0) {
            le_log_error("unable to apply microphone mixer control: %s",
                         settings[i].control);
            return -1;
        }
    }
    (void)snprintf(micpga, sizeof(micpga), "%d",
                   micpga_volume_from_environment());
    (void)snprintf(default_micpga, sizeof(default_micpga), "%d",
                   MICPGA_DEFAULT_VOLUME);
    for (i = 0; i < 4; ++i) {
        micpga_settings[i].control = micpga_controls[i];
        micpga_settings[i].value = micpga;
        micpga_settings[i].value2 = micpga;
        if (apply_mixer_setting(config->mixer_bin, &micpga_settings[i]) < 0) {
            /*
             * The control's usable maximum is a property of the codec, not
             * of this scale.  If a value is refused, fall back to the level
             * the capture path used before rather than failing the whole
             * profile -- an unusable microphone is far worse than a quiet
             * one, and configure_capture_path() failing leaves the device
             * with no capture at all.
             */
            struct mixer_setting fallback;

            le_log_warn("micd: capture PGA %s refused for %s; using %d",
                        micpga, micpga_controls[i], MICPGA_DEFAULT_VOLUME);
            fallback.control = micpga_controls[i];
            fallback.value = default_micpga;
            fallback.value2 = default_micpga;
            if (apply_mixer_setting(config->mixer_bin, &fallback) < 0) {
                le_log_error("unable to apply microphone mixer control: %s",
                             micpga_controls[i]);
                return -1;
            }
        }
    }
    le_log_info("micd: capture PGA set to %s", micpga);
    return 0;
}

static int format_status(char *buffer, size_t size,
                         const struct micd_config *config,
                         int capture_active, int capture_configured)
{
    int values[MIC_LOGICAL_CHANNELS];
    int count = read_miccal(config->idme_dir, config->dt_idme_dir, values);
    int available = access(config->pcm_path, R_OK) == 0;
    char mics[16];
    int n;

    format_mic_list(beam_mic_mask(), mics, sizeof(mics));
    n = snprintf(
        buffer, size,
        "{\"available\":%s,\"simulated\":false,"
        "\"sources\":[{\"id\":\"%d:%d\",\"card\":%d,\"device\":%d,"
        "\"name\":\"%s microphone array\","
        "\"rate\":16000,\"channels\":9,\"bits\":24,\"valid_bits\":16,"
        "\"encoding\":\"pcm_s24_3le\","
        "\"microphones\":["
        "{\"channel\":0,\"name\":\"Array microphone 1\"},"
        "{\"channel\":1,\"name\":\"Array microphone 2\"},"
        "{\"channel\":2,\"name\":\"Array microphone 3\"},"
        "{\"channel\":3,\"name\":\"Array microphone 4\"},"
        "{\"channel\":4,\"name\":\"Array microphone 5\"},"
        "{\"channel\":5,\"name\":\"Array microphone 6\"},"
        "{\"channel\":6,\"name\":\"Array microphone 7\"}]}],"
        "\"raw_channels\":9,\"active_microphone_channels\":7,"
        "\"inactive_transport_channels\":[7,8],\"logical_channels\":7,"
        "\"capture\":{\"active\":%s,\"owner\":%s},"
        "\"hardware_profile\":{\"name\":\"radar-audio-device-v4.02\","
        "\"configured\":%s,\"adc_codecs\":4,\"analog_pga_db\":20},"
        "\"calibration\":{\"q14\":[%d,%d,%d,%d,%d,%d,%d],"
        "\"fallback\":16384,\"source\":\"%s\","
        "\"values_found\":%d,\"complete\":%s,"
        "\"selected_logical_mics\":[%s],"
        "\"polarity\":\"all-positive-measured\","
        "\"applied_to_raw_stream\":false,"
        "\"applied_to_calibrated_stream\":true,"
        "\"mapping_available\":true,"
        "\"mapping\":\"measured; lane n is physical microphone n\","
        "\"geometry\":{\"reference\":\"0 deg front (volume down), clockwise viewed from above\",\"ring_lanes\":[0,1,2,3,4,5],\"ring_azimuth_deg\":[150,210,270,330,30,90],\"centre_lane\":6,\"ring_radius_mm\":36,\"lane_skew\":\"none measured\"},"
        "\"relative_delay_samples\":{}},"
        "\"pipeline\":{\"stage\":\"fixed-beam-select\","
        "\"microphone_mix\":\"%s\",\"microphone_lane\":%d,"
        "\"beams\":%d,\"fft\":%d,\"hop\":%d,"
        "\"beam_design\":\"superdirective, white-noise gain >= -3 dB\","
        "\"highpass_hz\":80,\"vad\":\"native-energy-baseline\","
        "\"aec\":false,\"beamforming\":true,\"wake_word\":false}}",
        available ? "true" : "false",
        config->profile.capture_card, config->profile.capture_device,
        config->profile.capture_card, config->profile.capture_device,
        !strcmp(config->profile.capture_chain, "lidar")
            ? "Lidar" : "Amazon MT8163",
        capture_active ? "true" : "false",
        capture_active ? "\"raw-stream\"" : "null",
        capture_configured ? "true" : "false",
        values[0], values[1], values[2], values[3],
        values[4], values[5], values[6],
        miccal_source(config->idme_dir, config->dt_idme_dir),
        count, count == MIC_LOGICAL_CHANNELS ? "true" : "false",
        mics, le_mic_mix_name(current_mix_mode()), current_mix_lane(),
        LE_BEAM_DEFAULT_BEAMS, LE_BEAM_FFT, LE_BEAM_HOP);
    return n < 0 || (size_t)n >= size ? -1 : n;
}

static int parse_channel(const char *args, int *channel)
{
    const char *key;
    const char *colon;
    char *end;
    long value;

    if (!args || !channel)
        return -1;
    key = strstr(args, "\"channel\"");
    if (!key)
        return -1;
    colon = strchr(key + 9, ':');
    if (!colon)
        return -1;
    errno = 0;
    value = strtol(colon + 1, &end, 10);
    if (errno || end == colon + 1 || value < 0 ||
        value >= MIC_CAPTURE_CHANNELS)
        return -1;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')
        ++end;
    if (*end != ',' && *end != '}')
        return -1;
    *channel = (int)value;
    return 0;
}

struct capture_layout {
    unsigned int channels;
    unsigned int rate;
    enum le_audio_format format;
    size_t frame_bytes;
    unsigned int logical_channel[LE_AUDIO_PROFILE_ROLE_SLOTS];
    unsigned int aec_slot[2];
    unsigned int aec_slots;
    int passthrough;
};

static void capture_layout_init(const struct le_audio_profile *profile,
                                struct capture_layout *layout)
{
    unsigned int channel;

    memset(layout, 0, sizeof(*layout));
    layout->channels = profile->capture_channels;
    layout->rate = profile->capture_rate;
    layout->format = profile->capture_format;
    layout->frame_bytes = layout->channels *
                          le_audio_profile_bytes(layout->format);
    for (channel = 0; channel < layout->channels &&
                      channel < LE_AUDIO_PROFILE_ROLE_SLOTS; ++channel)
        layout->logical_channel[channel] = channel < LE_VOICE_LOGICAL_CHANNELS
            ? channel : 0;
    for (channel = 0; channel < layout->channels &&
                      channel < LE_AUDIO_PROFILE_ROLE_SLOTS; ++channel) {
        int role = profile->capture_roles[channel];

        if (role >= LE_AUDIO_ROLE_MIC0 && role <= LE_AUDIO_ROLE_MIC6)
            layout->logical_channel[role - LE_AUDIO_ROLE_MIC0] = channel;
        else if (role == LE_AUDIO_ROLE_AEC &&
                 layout->aec_slots < 2)
            layout->aec_slot[layout->aec_slots++] = channel;
    }
    /* The MT8163 layout already is the wire contract: nine packed 24-bit
     * lanes in order with the AEC lanes last, so its capture can be forwarded
     * byte for byte instead of unpacked and repacked. */
    layout->passthrough = layout->format == LE_AUDIO_FORMAT_S24_3LE &&
                          layout->channels == LE_VOICE_RAW_CHANNELS &&
                          layout->aec_slots == 2 &&
                          layout->aec_slot[0] == 7 &&
                          layout->aec_slot[1] == 8;
    for (channel = 0; channel < LE_VOICE_LOGICAL_CHANNELS; ++channel)
        if (layout->logical_channel[channel] != channel)
            layout->passthrough = 0;
}

static int16_t capture_sample(const unsigned char *frame, size_t offset,
                              enum le_audio_format format)
{
    const unsigned char *p = frame + offset;

    switch (format) {
    case LE_AUDIO_FORMAT_S16_LE:
        return (int16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
    case LE_AUDIO_FORMAT_S24_3LE: {
        int32_t value = (int32_t)((uint32_t)p[0] << 8 |
                                  (uint32_t)p[1] << 16 |
                                  (uint32_t)p[2] << 24) >> 8;

        return (int16_t)(value >> 8);
    }
    case LE_AUDIO_FORMAT_S32_LE:
        return (int16_t)(((int32_t)((uint32_t)p[0] |
                                    (uint32_t)p[1] << 8 |
                                    (uint32_t)p[2] << 16 |
                                    (uint32_t)p[3] << 24)) >> 16);
    }
    return 0;
}

static pid_t start_capture_pcm(const struct micd_config *config, int output_fd,
                               int card, int device, unsigned int channels,
                               unsigned int rate, enum le_audio_format format)
{
    char card_text[16], device_text[16], channel_text[16], rate_text[16];
    char bits_text[16], period_text[16];
    unsigned int period = rate / 25; /* 40 ms, as the Amazon SPI ring expects */
    pid_t capture;

    (void)snprintf(card_text, sizeof(card_text), "%d", card);
    (void)snprintf(device_text, sizeof(device_text), "%d", device);
    (void)snprintf(channel_text, sizeof(channel_text), "%u", channels);
    (void)snprintf(rate_text, sizeof(rate_text), "%u", rate);
    (void)snprintf(bits_text, sizeof(bits_text), "%d",
                   le_audio_profile_bits(format));
    (void)snprintf(period_text, sizeof(period_text), "%u", period);

    capture = fork();
    if (capture != 0)
        return capture;
    {
        char *const argv[] = {
            (char *)"tinycap", (char *)"--",
            (char *)"-D", card_text,
            (char *)"-d", device_text,
            (char *)"-c", channel_text,
            (char *)"-r", rate_text,
            (char *)"-b", bits_text,
            (char *)"-p", period_text,
            (char *)"-n", (char *)"4",
            NULL
        };

        if (dup2(output_fd, STDOUT_FILENO) < 0)
            _exit(127);
        if (output_fd != STDOUT_FILENO)
            close(output_fd);
        execv(config->capture_bin, argv);
        _exit(127);
    }
}

static pid_t start_capture(const struct micd_config *config, int output_fd)
{
    return start_capture_pcm(config, output_fd,
                             config->profile.capture_card,
                             config->profile.capture_device,
                             config->profile.capture_channels,
                             config->profile.capture_rate,
                             config->profile.capture_format);
}

/*
 * One mono AEC reference lane, resampled 48 kHz -> 16 kHz by a 193-tap
 * Kaiser-windowed low-pass (7 kHz passband edge, 60 dB down at 8 kHz) and
 * decimation by three.  hw:0,2 is a bit-exact loopback of the playback bus
 * and cannot rate-convert, so the conversion belongs here.
 *
 * Group delay is (193 - 1) / 2 = 96 input samples = 32 output samples, so
 * the emitted reference is a fixed 32 samples (2 ms) behind the microphone
 * frame.  Keep this delay constant: the voice AEC aligns the two streams by
 * correlation and a moving delay would make that alignment wander.
 */
#define REFERENCE_FIR_TAPS 193
static const int16_t reference_fir[REFERENCE_FIR_TAPS] = {
    0, -2, -3, -1, 3, 5, 2, -3,
    -7, -4, 3, 9, 7, -2, -11, -11,
    0, 13, 16, 4, -14, -21, -9, 14,
    27, 16, -12, -32, -25, 7, 37, 35,
    0, -40, -47, -11, 41, 60, 25, -38,
    -72, -42, 31, 83, 63, -18, -92, -87,
    0, 96, 112, 25, -95, -139, -57, 87,
    164, 96, -69, -186, -141, 41, 204, 193,
    0, -213, -249, -55, 211, 308, 127, -194,
    -369, -217, 158, 430, 330, -97, -489, -471,
    0, 543, 652, 149, -590, -899, -388, 630,
    1281, 817, -659, -2032, -1836, 677, 4813, 8671,
    10241, 8671, 4813, 677, -1836, -2032, -659, 817,
    1281, 630, -388, -899, -590, 149, 652, 543,
    0, -471, -489, -97, 330, 430, 158, -217,
    -369, -194, 127, 308, 211, -55, -249, -213,
    0, 193, 204, 41, -141, -186, -69, 96,
    164, 87, -57, -139, -95, 25, 112, 96,
    0, -87, -92, -18, 63, 83, 31, -42,
    -72, -38, 25, 60, 41, -11, -47, -40,
    0, 35, 37, 7, -25, -32, -12, 16,
    27, 14, -9, -21, -14, 4, 16, 13,
    0, -11, -11, -2, 7, 9, 3, -4,
    -7, -3, 2, 5, 3, -1, -3, -2,
    0
};

struct reference_source {
    int fd;
    enum le_audio_format format;
    unsigned int channels;
    unsigned int factor;
    int16_t samples[8192];
    size_t head;
    size_t tail;
    int16_t last;
    int have_last;
    int16_t history[REFERENCE_FIR_TAPS];
    size_t history_pos;
    unsigned char residue[16];
    size_t residue_bytes;
};

static void reference_push(struct reference_source *reference,
                           const unsigned char *frame)
{
    size_t bytes = le_audio_profile_bytes(reference->format);
    int32_t sum = 0;
    unsigned int channel;
    size_t slot;

    for (channel = 0; channel < reference->channels; ++channel)
        sum += capture_sample(frame, channel * bytes, reference->format);
    slot = (reference->head + 1) % (sizeof(reference->samples) /
                                    sizeof(reference->samples[0]));
    if (slot == reference->tail)
        reference->tail = (reference->tail + 1) %
                          (sizeof(reference->samples) /
                           sizeof(reference->samples[0]));
    reference->samples[reference->head] =
        (int16_t)(sum / (int32_t)reference->channels);
    reference->head = slot;
}

static void reference_fill(struct reference_source *reference)
{
    unsigned char buffer[4096];
    size_t bytes = le_audio_profile_bytes(reference->format);
    size_t frame_bytes = bytes * reference->channels;
    size_t pending = reference->residue_bytes;
    ssize_t count;

    if (reference->fd < 0 || !frame_bytes || frame_bytes > sizeof(buffer))
        return;
    memcpy(buffer, reference->residue, pending);
    while ((count = read(reference->fd, buffer + pending,
                         sizeof(buffer) - pending)) > 0) {
        size_t available = pending + (size_t)count;
        size_t frames = available / frame_bytes;
        size_t frame;

        for (frame = 0; frame < frames; ++frame)
            reference_push(reference, buffer + frame * frame_bytes);
        pending = available - frames * frame_bytes;
        if (pending)
            memmove(buffer, buffer + frames * frame_bytes, pending);
    }
    reference->residue_bytes = pending;
    if (pending)
        memcpy(reference->residue, buffer, pending);
}

static int16_t reference_sample(struct reference_source *reference)
{
    int64_t sum = 0;
    int32_t value;
    unsigned int i;
    size_t index;

    if (!reference->factor)
        return 0;
    for (i = 0; i < reference->factor; ++i) {
        if (reference->tail != reference->head) {
            reference->last = reference->samples[reference->tail];
            reference->tail = (reference->tail + 1) %
                (sizeof(reference->samples) / sizeof(reference->samples[0]));
            reference->have_last = 1;
        }
        reference->history[reference->history_pos] = reference->have_last
            ? reference->last : 0;
        reference->history_pos = (reference->history_pos + 1) %
                                 REFERENCE_FIR_TAPS;
    }
    index = reference->history_pos;
    for (i = 0; i < REFERENCE_FIR_TAPS; ++i) {
        index = (index + REFERENCE_FIR_TAPS - 1) % REFERENCE_FIR_TAPS;
        sum += (int64_t)reference_fir[i] * reference->history[index];
    }
    value = (int32_t)(sum >> 15);
    if (value > INT16_MAX)
        value = INT16_MAX;
    else if (value < INT16_MIN)
        value = INT16_MIN;
    return (int16_t)value;
}

static void export_raw_frame(unsigned char out[MIC_EXPORT_CHANNELS *
                                               MIC_EXPORT_BYTES],
                             const struct capture_layout *layout,
                             const unsigned char *frame,
                             const struct reference_source *reference,
                             int16_t reference_value)
{
    size_t bytes = le_audio_profile_bytes(layout->format);
    int16_t values[MIC_EXPORT_CHANNELS] = {0};
    unsigned int channel;

    for (channel = 0; channel < LE_VOICE_LOGICAL_CHANNELS; ++channel)
        values[channel] = capture_sample(
            frame, layout->logical_channel[channel] * bytes,
            layout->format);
    if (layout->aec_slots > 0)
        values[7] = capture_sample(frame, layout->aec_slot[0] * bytes,
                                   layout->format);
    if (layout->aec_slots > 1)
        values[8] = capture_sample(frame, layout->aec_slot[1] * bytes,
                                   layout->format);
    if (reference->fd >= 0) {
        values[7] = reference_value;
        values[8] = reference_value;
    }
    for (channel = 0; channel < MIC_EXPORT_CHANNELS; ++channel) {
        int32_t value = (int32_t)values[channel] << 8;

        out[channel * 3] = (unsigned char)(value & 0xff);
        out[channel * 3 + 1] = (unsigned char)((value >> 8) & 0xff);
        out[channel * 3 + 2] = (unsigned char)((value >> 16) & 0xff);
    }
}

static int relay_capture(int client_fd, const struct micd_config *config)
{
    struct capture_layout layout;
    struct reference_source reference;
    unsigned char input[65536];
    unsigned char output[MIC_EXPORT_CHANNELS * MIC_EXPORT_BYTES * 2048];
    size_t pending = 0;
    int pipe_fds[2];
    int reference_fds[2];
    pid_t capture;
    pid_t reference_capture = -1;
    ssize_t count;

    capture_layout_init(&config->profile, &layout);
    if (!layout.frame_bytes)
        return -1;
    memset(&reference, 0, sizeof(reference));
    reference.fd = -1;
    if (config->profile.aec_source == LE_AUDIO_AEC_SEPARATE_PCM) {
        if (config->profile.aec_rate >= config->profile.capture_rate &&
            config->profile.aec_rate % config->profile.capture_rate == 0) {
            if (pipe(reference_fds) == 0) {
                reference_capture = start_capture_pcm(
                    config, reference_fds[1], config->profile.aec_card,
                    config->profile.aec_device, config->profile.aec_channels,
                    config->profile.aec_rate, config->profile.aec_format);
                close(reference_fds[1]);
                if (reference_capture < 0) {
                    close(reference_fds[0]);
                } else {
                    reference.fd = reference_fds[0];
                    reference.format = config->profile.aec_format;
                    reference.channels = config->profile.aec_channels;
                    reference.factor = config->profile.aec_rate /
                                       config->profile.capture_rate;
                    {
                        int flags = fcntl(reference.fd, F_GETFL, 0);
                        if (flags >= 0)
                            (void)fcntl(reference.fd, F_SETFL, flags | O_NONBLOCK);
                    }
                }
            }
        }
        if (reference.fd < 0)
            le_log_warn("micd: AEC reference unavailable; raw lanes 7/8 zero");
    }
    if (pipe(pipe_fds) < 0) {
        if (reference.fd >= 0) close(reference.fd);
        return -1;
    }
    capture = start_capture(config, pipe_fds[1]);
    if (capture < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        if (reference.fd >= 0) close(reference.fd);
        return -1;
    }
    close(pipe_fds[1]);
    if (layout.passthrough && reference.fd < 0) {
        /* MT8163: the capture already is the 9-lane S24 contract, so keep
         * the historical byte-for-byte relay (partial reads included). */
        while ((count = read(pipe_fds[0], input, sizeof(input))) > 0)
            if (write_all(client_fd, input, (size_t)count) < 0)
                break;
    } else {
        size_t read_cap = sizeof(output) / (MIC_EXPORT_CHANNELS *
                                            MIC_EXPORT_BYTES) *
                          layout.frame_bytes;
        if (read_cap > sizeof(input))
            read_cap = sizeof(input);
        while ((count = read(pipe_fds[0], input + pending,
                             read_cap - pending)) > 0) {
            size_t available = pending + (size_t)count;
            size_t frames = available / layout.frame_bytes;
            size_t consumed = frames * layout.frame_bytes;
            size_t frame;

            reference_fill(&reference);
            for (frame = 0; frame < frames; ++frame) {
                int16_t reference_value = reference.fd >= 0
                    ? reference_sample(&reference) : 0;

                export_raw_frame(output + frame * MIC_EXPORT_CHANNELS *
                                 MIC_EXPORT_BYTES, &layout,
                                 input + frame * layout.frame_bytes,
                                 &reference, reference_value);
            }
            if (frames &&
                write_all(client_fd, output,
                          frames * MIC_EXPORT_CHANNELS * MIC_EXPORT_BYTES) < 0)
                break;
            pending = available - consumed;
            if (pending)
                memmove(input, input + consumed, pending);
        }
    }
    close(pipe_fds[0]);
    kill(capture, SIGTERM);
    while (waitpid(capture, NULL, 0) < 0 && errno == EINTR)
        ;
    if (reference.fd >= 0) {
        close(reference.fd);
        if (reference_capture > 0) {
            kill(reference_capture, SIGTERM);
            while (waitpid(reference_capture, NULL, 0) < 0 && errno == EINTR)
                ;
        }
    }
    return count < 0 ? -1 : 0;
}

/* Re-pack the target layout into the 9-lane S24 the voice stack parses. */
static void normalize_capture_frame(const struct capture_layout *layout,
                                    const unsigned char *frame,
                                    unsigned char out[LE_VOICE_RAW_FRAME_BYTES])
{
    size_t bytes = le_audio_profile_bytes(layout->format);
    unsigned int channel;

    if (layout->passthrough) {
        memcpy(out, frame, LE_VOICE_RAW_FRAME_BYTES);
        return;
    }
    memset(out, 0, LE_VOICE_RAW_FRAME_BYTES);
    for (channel = 0; channel < LE_VOICE_LOGICAL_CHANNELS; ++channel) {
        int32_t value = (int32_t)capture_sample(
            frame, layout->logical_channel[channel] * bytes,
            layout->format) << 8;

        out[channel * 3] = (unsigned char)(value & 0xff);
        out[channel * 3 + 1] = (unsigned char)((value >> 8) & 0xff);
        out[channel * 3 + 2] = (unsigned char)((value >> 16) & 0xff);
    }
}

/*
 * Streams calibrated mono audio. The OK header is the only success signal a
 * client gets, so it is written once tinycap has delivered its first period.
 * If tinycap exits first (the PCM is busy or missing), the client gets an
 * error and not an OK header followed by an empty stream.
 */
static int relay_capture_mono(int client_fd, unsigned long id,
                              const char *header, size_t header_size,
                              const struct micd_config *config)
{
    static struct le_beamformer beamformer;
    struct capture_layout layout;
    struct le_voice_calibration calibration;
    struct le_voice_highpass highpass;
    unsigned char input[65536];
    int16_t output[256];
    uint8_t logical_map[LE_VOICE_LOGICAL_CHANNELS];
    size_t frame_bytes;
    unsigned int lane;
    int miccal[LE_VOICE_LOGICAL_CHANNELS];
    size_t pending = 0;
    int pipe_fds[2];
    pid_t capture;
    ssize_t count = 0;
    unsigned int digital_gain = digital_gain_from_environment();
    unsigned int mix_mask = LE_BEAM_DEFAULT_MIC_MASK;
    int result = -1;

    capture_layout_init(&config->profile, &layout);
    frame_bytes = layout.frame_bytes;
    if (!frame_bytes)
        return -1;
    for (lane = 0; lane < LE_VOICE_LOGICAL_CHANNELS; ++lane)
        logical_map[lane] = (uint8_t)lane;
    if (pipe(pipe_fds) < 0)
        return -1;
    capture = start_capture(config, pipe_fds[1]);
    if (capture < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return -1;
    }
    close(pipe_fds[1]);

    /*
     * This pinned tinycap treats "--" as a raw-stdout transport, so the
     * first byte read here is microphone PCM rather than a WAV header.
     */
    (void)read_miccal(config->idme_dir, config->dt_idme_dir, miccal);
    le_voice_calibration_init(
        &calibration, miccal, logical_map,
        LE_VOICE_CALIBRATION_DIRECT_Q14);
    if (le_beamformer_init(&beamformer, beam_mic_mask(),
                           LE_BEAM_DEFAULT_BEAMS) < 0) {
        close(pipe_fds[0]);
        kill(capture, SIGTERM);
        while (waitpid(capture, NULL, 0) < 0 && errno == EINTR)
            ;
        return -1;
    }
    le_voice_highpass_init(&highpass);
    mix_mask = beam_mic_mask();

    count = read(pipe_fds[0], input,
                 sizeof(output) / sizeof(output[0]) * frame_bytes);
    if (count <= 0) {
        /* tinycap exited before its first period: no capture, say so. */
        char error[LE_ADAPTER_MSG_MAX];
        int n = le_adapter_respond_err(error, sizeof(error), id,
                                       "microphone capture unavailable");

        if (n > 0)
            (void)write_all(client_fd, error, (size_t)n);
    } else if (write_all(client_fd, header, header_size) == 0) {
        while (count > 0) {
            size_t available = pending + (size_t)count;
            size_t frames = available / frame_bytes;
            size_t consumed = frames * frame_bytes;

            if (frames > 0) {
                size_t frame;

                for (frame = 0; frame < frames; ++frame) {
                    int16_t logical[LE_VOICE_LOGICAL_CHANNELS];
                    unsigned char normalized[LE_VOICE_RAW_FRAME_BYTES];

                    normalize_capture_frame(&layout,
                                            input + frame * frame_bytes,
                                            normalized);
                    le_voice_decode_calibrated(
                        normalized, &calibration, logical);
                    /* The beamformer runs in every mode, so its selection
                       and beam view stay warm; the mode only picks the
                       output. */
                    unsigned int mode = current_mix_mode();
                    int16_t mixed = le_beamformer_sample(&beamformer, logical);

                    if (mode == LE_MIC_MIX_AVERAGE)
                        mixed = le_mic_mix_average(logical, mix_mask);
                    else if (mode == LE_MIC_MIX_SINGLE)
                        mixed = le_mic_mix_single(logical, mix_mask,
                                                  current_mix_lane());
                    output[frame] = le_voice_highpass_sample(&highpass, mixed);
                    if (beamformer.fill == 0)
                        beam_view_publish(&beamformer);
                }
                apply_digital_gain(output, frames, digital_gain);
                if (write_all(client_fd, output,
                              frames * sizeof(output[0])) < 0)
                    break;
            }
            pending = available - consumed;
            if (pending > 0)
                memmove(input, input + consumed, pending);
            count = read(pipe_fds[0], input + pending,
                         sizeof(output) / sizeof(output[0]) * frame_bytes -
                         pending);
        }
        result = count < 0 ? -1 : 0;
    }
    beam_view_stop();

    close(pipe_fds[0]);
    kill(capture, SIGTERM);
    while (waitpid(capture, NULL, 0) < 0 && errno == EINTR)
        ;
    return result;
}

static void run_stream_worker(int client_fd, unsigned long id, int channel,
                              int calibrated_mono,
                              const struct micd_config *config)
{
    char data[512];
    char response[768];
    int n;

    if (calibrated_mono) {
        char mics[16];

        format_mic_list(beam_mic_mask(), mics, sizeof(mics));
        n = snprintf(
            data, sizeof(data),
            "{\"format\":\"pcm_s16_le\",\"rate\":16000,"
            "\"channels\":1,\"bits\":16,\"valid_bits\":16,"
            "\"selected_logical_mics\":[%s],"
            "\"calibration_applied\":true,"
            "\"calibration_mode\":\"direct_q14\","
            "\"logical_map\":\"measured\","
            "\"beamforming\":\"fixed_beam_select\","
            "\"beams\":%d,\"latency_samples\":%d,"
            "\"highpass_hz\":80}", mics, LE_BEAM_DEFAULT_BEAMS, LE_BEAM_FFT);
    } else {
        n = snprintf(data, sizeof(data),
                     "{\"format\":\"pcm_s24_3le\",\"rate\":16000,"
                     "\"channels\":9,\"bits\":24,\"valid_bits\":16,"
                     "\"selected_raw_channel\":%d,"
                     "\"calibration_applied\":false}", channel);
    }
    if (n > 0 && (size_t)n < sizeof(data))
        n = le_adapter_respond_ok(response, sizeof(response), id, data);
    else
        n = -1;
    if (n > 0) {
        if (calibrated_mono) {
            /* relay_capture_mono sends this header once capture has started. */
            (void)relay_capture_mono(client_fd, id, response, (size_t)n,
                                     config);
        } else if (write_all(client_fd, response, (size_t)n) == 0) {
            (void)relay_capture(client_fd, config);
        }
    }
    close(client_fd);
}

static int respond(int client_fd, unsigned long id, int ok,
                   const char *payload)
{
    char response[LE_ADAPTER_MSG_MAX];
    int n = ok
        ? le_adapter_respond_ok(response, sizeof(response), id, payload)
        : le_adapter_respond_err(response, sizeof(response), id, payload);

    return n < 0 ? -1 : write_all(client_fd, response, (size_t)n);
}

int main(int argc, char **argv)
{
    struct micd_config config = {
        .socket_path = LE_ADAPTER_MIC_SOCK,
        .idme_dir = "/proc/idme",
        .dt_idme_dir = "/sys/firmware/devicetree/base/idme",
        .pcm_path = "/dev/snd/pcmC0D24c",
        .capture_bin = "/sbin/tinycap",
        .mixer_bin = "/sbin/tinymix"
    };
    pid_t stream_worker = -1;
    int capture_configured = 0;
    int listen_fd;
    int i;

    le_log_init("micd", argc, argv);
    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--foreground")) {
            continue;
        } else if (!strcmp(argv[i], "--socket") && i + 1 < argc) {
            config.socket_path = argv[++i];
        } else if (!strcmp(argv[i], "--idme-dir") && i + 1 < argc) {
            config.idme_dir = argv[++i];
        } else if (!strcmp(argv[i], "--dt-idme-dir") && i + 1 < argc) {
            config.dt_idme_dir = argv[++i];
        } else if (!strcmp(argv[i], "--pcm-path") && i + 1 < argc) {
            config.pcm_path = argv[++i];
            config.pcm_path_set = 1;
        } else if (!strcmp(argv[i], "--profile") && i + 1 < argc) {
            config.profile_path = argv[++i];
        } else if (!strcmp(argv[i], "--capture-bin") && i + 1 < argc) {
            config.capture_bin = argv[++i];
        } else if (!strcmp(argv[i], "--mixer-bin") && i + 1 < argc) {
            config.mixer_bin = argv[++i];
        } else if (!strcmp(argv[i], "--verbose") ||
                   !strcmp(argv[i], "--debug") ||
                   !strcmp(argv[i], "--quiet") ||
                   !strcmp(argv[i], "--syslog")) {
            continue;
        } else {
            fprintf(stderr, "Usage: %s [--foreground] [--socket PATH] "
                    "[--idme-dir DIR] [--pcm-path PATH] "
                    "[--dt-idme-dir DIR] "
                    "[--capture-bin PATH] [--mixer-bin PATH] "
                    "[--profile PATH]\n", argv[0]);
            return 2;
        }
    }

    signal(SIGINT, stop_handler);
    signal(SIGTERM, stop_handler);
    signal(SIGCHLD, child_handler);
    signal(SIGPIPE, SIG_IGN);
    if (config.profile_path) {
        int parsed = le_audio_profile_read(config.profile_path,
                                           &config.profile);
        if (parsed < 0) {
            le_log_error("micd: audio profile %s is invalid",
                         config.profile_path);
            return 2;
        }
        if (parsed == 0)
            le_audio_profile_defaults(&config.profile);
    } else if (le_audio_profile_load(&config.profile) < 0) {
        le_log_warn("micd: audio profile invalid; using MT8163 layout");
    }
    if (!config.pcm_path_set) {
        static char pcm_path[64];

        (void)snprintf(pcm_path, sizeof(pcm_path), "/dev/snd/pcmC%dD%dc",
                       config.profile.capture_card,
                       config.profile.capture_device);
        config.pcm_path = pcm_path;
    }
    if (config.profile.capture_rate != 16000) {
        le_log_error("micd: capture rate %u is not supported (need 16000)",
                     config.profile.capture_rate);
        return 2;
    }
    if (access(config.pcm_path, R_OK) == 0) {
        capture_configured = configure_capture_path(&config) == 0;
        if (capture_configured)
            le_log_info("Radar microphone mixer profile configured");
        else
            le_log_warn("microphone endpoint exists but mixer setup failed");
    }
    beam_view = mmap(NULL, sizeof(*beam_view), PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (beam_view == MAP_FAILED)
        beam_view = NULL;
    else {
        int mode, lane;

        memset(beam_view, 0, sizeof(*beam_view));
        saved_mix_settings(&mode, &lane);
        beam_view->mix_mode = (unsigned int)mode;
        beam_view->mix_lane = lane;
    }
    listen_fd = le_adapter_listen(config.socket_path);
    if (listen_fd < 0) {
        le_log_perr("unable to listen on %s", config.socket_path);
        return 1;
    }
    le_log_info("microphone daemon listening on %s (capture idle)",
                config.socket_path);

    while (running) {
        struct pollfd descriptor;
        int client_fd;
        char message[LE_ADAPTER_MSG_MAX];
        char command[64];
        char status[LE_ADAPTER_MSG_MAX];
        char *args = NULL;
        unsigned long id = 0;

        if (child_changed || stream_worker > 0) {
            int child_status;
            pid_t result = 0;

            child_changed = 0;
            /*
             * -1 means "any child" to waitpid and "no worker" here, so the
             * old unconditional call waited on whatever else had exited:
             * configuring the capture path forks and reaps tinymix, and a
             * SIGCHLD from one of those made waitpid return -1 (ECHILD),
             * which compared equal to the sentinel and logged a false
             * "raw microphone stream stopped" at every boot.  Ask only
             * about a worker that actually exists.
             */
            if (stream_worker > 0)
                result = waitpid(stream_worker, &child_status, WNOHANG);
            if (stream_worker > 0 && result == stream_worker) {
                /* A worker that died mid-write must not leave stale beams. */
                if (beam_view) {
                    beam_view->running = 0;
                    beam_view->seq = (beam_view->seq + 1U) & ~1U;
                }
                le_log_info("raw microphone stream stopped");
                stream_worker = -1;
            }
        }
        descriptor.fd = listen_fd;
        descriptor.events = POLLIN;
        descriptor.revents = 0;
        if (poll(&descriptor, 1, 500) <= 0)
            continue;
        client_fd = le_adapter_accept(listen_fd);
        if (client_fd < 0)
            continue;
        if (read_request_line(client_fd, message, sizeof(message)) < 0 ||
            le_adapter_parse_request(message, command, sizeof(command),
                                     &args, &id) < 0) {
            (void)respond(client_fd, id, 0, "malformed request");
            close(client_fd);
            continue;
        }

        if (!strcmp(command, "status")) {
            if (format_status(status, sizeof(status), &config,
                              stream_worker > 0,
                              capture_configured) < 0)
                (void)respond(client_fd, id, 0, "status unavailable");
            else
                (void)respond(client_fd, id, 1, status);
            close(client_fd);
        } else if (!strcmp(command, "beams")) {
            if (format_beams(status, sizeof(status)) < 0)
                (void)respond(client_fd, id, 0, "beam state unavailable");
            else
                (void)respond(client_fd, id, 1, status);
            close(client_fd);
        } else if (!strcmp(command, "set_mix")) {
            int mode = mix_mode_from_json(args, "\"mode\"");

            if (mode < 0) {
                (void)respond(client_fd, id, 0,
                              "invalid microphone mix mode");
            } else if (!beam_view) {
                (void)respond(client_fd, id, 0,
                              "microphone mix is unavailable");
            } else {
                beam_view->mix_mode = (unsigned int)mode;
                le_log_info("microphone mix set to %s", le_mic_mix_name(mode));
                (void)snprintf(status, sizeof(status),
                               "{\"microphone_mix\":\"%s\"}",
                               le_mic_mix_name(mode));
                (void)respond(client_fd, id, 1, status);
            }
            close(client_fd);
        } else if (!strcmp(command, "set_lane")) {
            int lane;

            if (lane_from_json(args, "\"lane\"", &lane) < 0) {
                (void)respond(client_fd, id, 0, "invalid microphone lane");
            } else if (!beam_view) {
                (void)respond(client_fd, id, 0,
                              "microphone lane is unavailable");
            } else {
                beam_view->mix_lane = lane;
                le_log_info("microphone lane set to %d", lane);
                (void)snprintf(status, sizeof(status),
                               "{\"microphone_lane\":%d}", lane);
                (void)respond(client_fd, id, 1, status);
            }
            close(client_fd);
        } else if (!strcmp(command, "stream_raw")) {
            int channel;
            pid_t worker;
            if (parse_channel(args, &channel) < 0) {
                (void)respond(client_fd, id, 0,
                              "invalid raw microphone channel");
                close(client_fd);
                continue;
            }
            if (access(config.pcm_path, R_OK) != 0) {
                (void)respond(client_fd, id, 0,
                              "microphone capture endpoint unavailable");
                close(client_fd);
                continue;
            }
            if (stream_worker > 0) {
                (void)respond(client_fd, id, 0,
                              "microphone capture is already in use");
                close(client_fd);
                continue;
            }
            capture_configured = configure_capture_path(&config) == 0;
            if (!capture_configured) {
                (void)respond(client_fd, id, 0,
                              "microphone mixer configuration failed");
                close(client_fd);
                continue;
            }
            worker = fork();
            if (worker < 0) {
                (void)respond(client_fd, id, 0,
                              "unable to start microphone stream");
                close(client_fd);
                continue;
            }
            if (worker == 0) {
                close(listen_fd);
                run_stream_worker(client_fd, id, channel, 0, &config);
                _exit(0);
            }
            stream_worker = worker;
            close(client_fd);
            le_log_info("raw microphone stream started (lane=%d)", channel);
        } else if (!strcmp(command, "stream_mono")) {
            pid_t worker;
            if (access(config.pcm_path, R_OK) != 0) {
                (void)respond(client_fd, id, 0,
                              "microphone capture endpoint unavailable");
                close(client_fd);
                continue;
            }
            if (stream_worker > 0) {
                (void)respond(client_fd, id, 0,
                              "microphone capture is already in use");
                close(client_fd);
                continue;
            }
            capture_configured = configure_capture_path(&config) == 0;
            if (!capture_configured) {
                (void)respond(client_fd, id, 0,
                              "microphone mixer configuration failed");
                close(client_fd);
                continue;
            }
            worker = fork();
            if (worker < 0) {
                (void)respond(client_fd, id, 0,
                              "unable to start calibrated microphone stream");
                close(client_fd);
                continue;
            }
            if (worker == 0) {
                close(listen_fd);
                run_stream_worker(client_fd, id, 0, 1, &config);
                _exit(0);
            }
            stream_worker = worker;
            close(client_fd);
            le_log_info("calibrated mono microphone stream started");
        } else if (!strcmp(command, "stream_logical")) {
            (void)respond(client_fd, id, 0,
                          "logical microphone mapping is not available");
            close(client_fd);
        } else {
            (void)respond(client_fd, id, 0, "unknown command");
            close(client_fd);
        }
    }

    if (stream_worker > 0) {
        kill(stream_worker, SIGTERM);
        while (waitpid(stream_worker, NULL, 0) < 0 && errno == EINTR)
            ;
    }
    close(listen_fd);
    unlink(config.socket_path);
    le_log_info("microphone daemon stopped");
    return 0;
}
