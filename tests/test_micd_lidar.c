/*
 * The Lidar capture path is exercised through micd's own static helpers: the
 * profile is the Platform-rendered file, the capture layout is the 8-channel
 * S32 array, and the AEC reference is the 48 kHz -> 16 kHz FIR.
 */
#define main micd_daemon_main
#include "../src/adapter/micd.c"
#undef main

#include <math.h>
#include <sys/stat.h>

#define CHECK(x) do { \
    if (!(x)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
        return 1; \
    } \
} while (0)

#define TONE_AMPLITUDE 10000.0

#include "audio_profile_fixture.h"

static void store_s32(unsigned char *frame, unsigned int channel,
                      int32_t value)
{
    uint32_t raw = (uint32_t)value;

    frame[channel * 4] = (unsigned char)(raw & 0xff);
    frame[channel * 4 + 1] = (unsigned char)((raw >> 8) & 0xff);
    frame[channel * 4 + 2] = (unsigned char)((raw >> 16) & 0xff);
    frame[channel * 4 + 3] = (unsigned char)((raw >> 24) & 0xff);
}

static int32_t lane_s24(const unsigned char *frame, unsigned int lane)
{
    const unsigned char *p = frame + lane * 3;
    int32_t value = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 |
                              (uint32_t)p[2] << 24) >> 8;

    return value;
}

static void reference_push_tone(struct reference_source *reference,
                                double hertz, size_t index)
{
    unsigned char frame[4];
    int16_t value = (int16_t)lrint(TONE_AMPLITUDE *
        sin(2.0 * 3.14159265358979323846 * hertz *
            (double)index / 48000.0));

    frame[0] = (unsigned char)((uint16_t)value & 0xff);
    frame[1] = (unsigned char)((uint16_t)value >> 8);
    frame[2] = frame[0];
    frame[3] = frame[1];
    reference_push(reference, frame);
}

static double rms(const int16_t *samples, size_t count)
{
    double sum = 0.0;
    size_t i;

    for (i = 0; i < count; ++i)
        sum += (double)samples[i] * samples[i];
    return sqrt(sum / (double)count);
}

/* Best-fit delay of y against a 16 kHz tone; returns the residual. */
static double fit_delay(const int16_t *y, size_t count, double hertz,
                        unsigned int delay)
{
    double residual = 0.0;
    double numerator = 0.0, denominator = 0.0;
    double gain;
    size_t m;

    for (m = 200; m < count; ++m) {
        double x = sin(2.0 * 3.14159265358979323846 * hertz *
                      ((double)m - (double)delay) / 16000.0);

        numerator += (double)y[m] * x;
        denominator += x * x;
    }
    gain = numerator / denominator;
    for (m = 200; m < count; ++m) {
        double x = sin(2.0 * 3.14159265358979323846 * hertz *
                      ((double)m - (double)delay) / 16000.0);

        residual += (y[m] - gain * x) * (y[m] - gain * x);
    }
    return residual;
}

int main(void)
{
    char directory[96];
    char profile_path[128];
    char capture_path[128];
    char args_path[128];
    struct le_audio_profile profile;
    struct capture_layout layout;
    struct reference_source reference;
    struct micd_config config;
    int16_t output[1200];
    unsigned char frame[8 * 4];
    unsigned char exported[MIC_EXPORT_CHANNELS * MIC_EXPORT_BYTES];
    unsigned int channel, delay;
    double best = -1.0, best_delay_samples = 0.0;

    snprintf(directory, sizeof(directory), "/tmp/libreecho-micd-lidar-%ld",
             (long)getpid());
    CHECK(mkdir(directory, 0700) == 0);
    snprintf(profile_path, sizeof(profile_path), "%s/audio-profile",
             directory);
    {
        FILE *file = fopen(profile_path, "w");

        CHECK(file != NULL);
        CHECK(fputs(lidar_profile, file) >= 0);
        CHECK(fclose(file) == 0);
    }
    CHECK(le_audio_profile_read(profile_path, &profile) == 1);

    /* 8 channels of S32, one role per lane; line-in has no export lane. */
    capture_layout_init(&profile, &layout);
    CHECK(layout.channels == 8 && layout.frame_bytes == 32);
    CHECK(layout.format == LE_AUDIO_FORMAT_S32_LE);
    CHECK(!layout.passthrough);
    for (channel = 0; channel < 8; ++channel)
        /* S32 to S16 keeps the top 16 bits; put the level there. */
        store_s32(frame, channel, (int32_t)(channel + 1) * 1000 * 0x10000);
    {
        struct reference_source none;

        memset(&none, 0, sizeof(none));
        none.fd = -1;
        export_raw_frame(exported, &layout, frame, &none, 0);
    }
    /* capture_roles maps lane n to logical microphone n (S16 << 8). */
    for (channel = 0; channel < 7; ++channel) {
        int32_t expected = (int32_t)(channel + 1) * 1000 * 256;

        if (lane_s24(exported, channel) != expected) {
            fprintf(stderr, "lane %u is %d, expected %d\n", channel,
                    (int)lane_s24(exported, channel), (int)expected);
            return 1;
        }
    }
    CHECK(lane_s24(exported, 7) == 0);
    CHECK(lane_s24(exported, 8) == 0);
    puts("micd lidar: 8-channel S32 capture maps to lanes 0..6, line-in "
         "dropped");

    /* The default MT8163 profile stays byte-for-byte passthrough. */
    le_audio_profile_defaults(&profile);
    capture_layout_init(&profile, &layout);
    CHECK(layout.passthrough);
    CHECK(layout.frame_bytes == LE_VOICE_RAW_FRAME_BYTES);
    puts("micd lidar: MT8163 layout keeps the passthrough relay");

    /* The capture invocation follows the profile, not the old constants. */
    snprintf(capture_path, sizeof(capture_path), "%s/fake-tinycap",
             directory);
    snprintf(args_path, sizeof(args_path), "%s/capture.args", directory);
    {
        FILE *file = fopen(capture_path, "w");

        CHECK(file != NULL);
        CHECK(fprintf(file,
              "#!/bin/sh\nprintf '%%s\\n' \"$@\" > '%s'\n", args_path) > 0);
        CHECK(fclose(file) == 0);
        CHECK(chmod(capture_path, 0700) == 0);
    }
    CHECK(le_audio_profile_read(profile_path, &profile) == 1);
    memset(&config, 0, sizeof(config));
    config.capture_bin = capture_path;
    config.profile = profile;
    {
        int fd = open(args_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        pid_t child;

        CHECK(fd >= 0);
        child = start_capture(&config, fd);
        CHECK(child > 0);
        close(fd);
        while (waitpid(child, NULL, 0) < 0 && errno == EINTR)
            ;
    }
    {
        char args[512];
        FILE *file = fopen(args_path, "r");
        size_t used;

        CHECK(file != NULL);
        used = fread(args, 1, sizeof(args) - 1, file);
        CHECK(fclose(file) == 0);
        args[used] = '\0';
        CHECK(!strcmp(args,
              "--\n-D\n0\n-d\n1\n-c\n8\n-r\n16000\n-b\n32\n"
              "-p\n640\n-n\n4\n"));
    }
    puts("micd lidar: capture uses card 0 device 1, 8 channels, S32, 16 kHz");

    /* Separate 48 kHz stereo reference: passband tone, fixed delay. */
    memset(&reference, 0, sizeof(reference));
    reference.fd = -1;
    reference.format = LE_AUDIO_FORMAT_S16_LE;
    reference.channels = 2;
    reference.factor = profile.aec_rate / profile.capture_rate;
    CHECK(reference.factor == 3);
    for (channel = 0; channel < 1000; ++channel) {
        unsigned int step;

        for (step = 0; step < 3; ++step)
            reference_push_tone(&reference, 200.0,
                                (size_t)channel * 3 + step);
        output[channel] = reference_sample(&reference);
    }
    /* The FIR group delay is 32 output samples; search its neighbourhood so
     * the half-period fit (same residual, inverted gain) cannot win. */
    for (delay = 24; delay <= 40; ++delay) {
        double residual = fit_delay(output, 1000, 200.0, delay);

        if (best < 0.0 || residual < best) {
            best = residual;
            best_delay_samples = delay;
        }
    }
    /* 96 input samples of group delay is 32 output samples; the decimation
     * phase makes that 31.33, so one output sample of slack is expected. */
    if (fabs(best_delay_samples - 32.0) > 1.0)
        fprintf(stderr, "reference delay is %.0f samples, expected 32\n",
                best_delay_samples);
    CHECK(fabs(best_delay_samples - 32.0) <= 1.0);
    CHECK(rms(output + 200, 800) > TONE_AMPLITUDE / 2.0);
    printf("micd lidar: 48 kHz reference arrives at 16 kHz %.0f samples "
           "late (32 nominal)\n", best_delay_samples);

    /* A 9 kHz tone aliases after decimation and must be > 60 dB down. */
    memset(&reference, 0, sizeof(reference));
    reference.fd = -1;
    reference.format = LE_AUDIO_FORMAT_S16_LE;
    reference.channels = 2;
    reference.factor = 3;
    for (channel = 0; channel < 1000; ++channel) {
        unsigned int step;

        for (step = 0; step < 3; ++step)
            reference_push_tone(&reference, 9000.0,
                                (size_t)channel * 3 + step);
        output[channel] = reference_sample(&reference);
    }
    {
        double attenuation = rms(output + 200, 800) / (TONE_AMPLITUDE / sqrt(2.0));

        if (-20.0 * log10(attenuation + 1e-12) < 60.0)
            fprintf(stderr, "9 kHz suppression is only %.1f dB\n",
                    -20.0 * log10(attenuation + 1e-12));
        CHECK(attenuation < 1e-3);
        puts("micd lidar: 9 kHz reference is attenuated by more than 60 dB");
    }

    /* Underrun: the last consumed reference sample is held, not zeroed. */
    memset(&reference, 0, sizeof(reference));
    reference.fd = -1;
    reference.format = LE_AUDIO_FORMAT_S16_LE;
    reference.channels = 2;
    reference.factor = 3;
    for (channel = 0; channel < 100; ++channel) {
        unsigned int step;

        for (step = 0; step < 3; ++step)
            reference_push_tone(&reference, 200.0,
                                (size_t)channel * 3 + step);
        output[channel] = reference_sample(&reference);
    }
    CHECK(reference.have_last);
    for (channel = 100; channel < 400; ++channel)
        output[channel] = reference_sample(&reference);
    for (channel = 300; channel < 400; ++channel)
        CHECK(abs((int)output[channel] - (int)reference.last) <= 3);
    puts("micd lidar: an underrunning reference holds its last sample");

    unlink(args_path);
    unlink(capture_path);
    unlink(profile_path);
    rmdir(directory);
    return 0;
}
