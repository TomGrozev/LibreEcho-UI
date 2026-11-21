#include "adapter/audio_profile.h"
#include "audio_profile_fixture.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(x) do { \
    if (!(x)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
        return 1; \
    } \
} while (0)

static int write_text(const char *path, const char *text)
{
    FILE *file = fopen(path, "w");

    if (!file)
        return -1;
    if (fputs(text, file) < 0) {
        fclose(file);
        return -1;
    }
    return fclose(file);
}

static int expect_rejected(const char *directory, const char *name,
                           const char *text)
{
    char path[256];
    struct le_audio_profile profile;

    snprintf(path, sizeof(path), "%s/%s", directory, name);
    if (write_text(path, text) < 0)
        return -1;
    /* A rejected file must not leave half-parsed values behind either. */
    le_audio_profile_defaults(&profile);
    if (le_audio_profile_read(path, &profile) != -1)
        return -1;
    if (profile.capture_device != 24 || profile.capture_channels != 9)
        return -1;
    unlink(path);
    return 0;
}

int main(void)
{
    char directory[96];
    char path[128];
    struct le_audio_profile profile;
    unsigned int channel;

    snprintf(directory, sizeof(directory), "/tmp/libreecho-profile-test-%ld",
             (long)getpid());
    if (mkdir(directory, 0700) < 0) {
        perror("mkdir");
        return 1;
    }

    /* A missing file is the MT8163 layout, not an error. */
    snprintf(path, sizeof(path), "%s/absent", directory);
    CHECK(le_audio_profile_read(path, &profile) == 0);
    le_audio_profile_defaults(&profile);
    CHECK(profile.playback_card == 0 && profile.playback_device == 23);
    CHECK(profile.capture_card == 0 && profile.capture_device == 24);
    CHECK(profile.capture_channels == 9);
    CHECK(profile.capture_rate == 16000);
    CHECK(profile.capture_format == LE_AUDIO_FORMAT_S24_3LE);
    CHECK(profile.aec_source == LE_AUDIO_AEC_IN_STREAM);
    CHECK(!strcmp(profile.capture_chain, "radar"));
    CHECK(!strcmp(profile.speaker_chain, "radar_puffin"));
    for (channel = 0; channel < 7; ++channel)
        CHECK(profile.capture_roles[channel] ==
              LE_AUDIO_ROLE_MIC0 + (int)channel);
    CHECK(profile.capture_roles[7] == LE_AUDIO_ROLE_AEC);
    CHECK(profile.capture_roles[8] == LE_AUDIO_ROLE_AEC);

    /* The Platform-rendered Lidar profile, field for field. */
    snprintf(path, sizeof(path), "%s/lidar", directory);
    CHECK(write_text(path, lidar_profile) == 0);
    CHECK(le_audio_profile_read(path, &profile) == 1);
    CHECK(!strcmp(profile.target_id, "lidar"));
    CHECK(!strcmp(profile.speaker_chain, "lidar"));
    CHECK(!strcmp(profile.capture_chain, "lidar"));
    CHECK(profile.capture_card == 0 && profile.capture_device == 1);
    CHECK(profile.capture_rate == 16000 && profile.capture_channels == 8);
    CHECK(profile.capture_format == LE_AUDIO_FORMAT_S32_LE);
    for (channel = 0; channel < 7; ++channel)
        CHECK(profile.capture_roles[channel] ==
              LE_AUDIO_ROLE_MIC0 + (int)channel);
    CHECK(profile.capture_roles[7] == LE_AUDIO_ROLE_LINE_IN);
    CHECK(profile.aec_source == LE_AUDIO_AEC_SEPARATE_PCM);
    CHECK(profile.aec_card == 0 && profile.aec_device == 2);
    CHECK(profile.aec_rate == 48000 && profile.aec_channels == 2);
    CHECK(profile.aec_format == LE_AUDIO_FORMAT_S16_LE);
    unlink(path);

    /* Capture roles are matched to capture_channels by key, not line order. */
    CHECK(write_text(path,
        "schema=1\ntarget_id=lidar\n"
        "capture_roles=mic0,mic1,mic2,mic3,mic4,mic5,mic6,line-in\n"
        "capture_channels=8\n") == 0);
    CHECK(le_audio_profile_read(path, &profile) == 1);
    CHECK(profile.capture_roles[7] == LE_AUDIO_ROLE_LINE_IN);
    unlink(path);

    CHECK(expect_rejected(directory, "unknown-key",
        "schema=1\ntarget_id=lidar\nspeaker_bass=3\n") == 0);
    CHECK(expect_rejected(directory, "no-schema",
        "target_id=lidar\nspeaker_chain=lidar\n") == 0);
    CHECK(expect_rejected(directory, "no-target",
        "schema=1\nspeaker_chain=lidar\n") == 0);
    CHECK(expect_rejected(directory, "bad-schema",
        "schema=2\ntarget_id=lidar\n") == 0);
    CHECK(expect_rejected(directory, "duplicate-schema",
        "schema=1\nschema=1\ntarget_id=lidar\n") == 0);
    CHECK(expect_rejected(directory, "duplicate-capture-device",
        "schema=1\ntarget_id=lidar\n"
        "capture_device=1\ncapture_device=1\n") == 0);
    CHECK(expect_rejected(directory, "bad-chain",
        "schema=1\ntarget_id=lidar\nspeaker_chain=toaster\n") == 0);
    CHECK(expect_rejected(directory, "bad-codec",
        "schema=1\ntarget_id=lidar\ncodec_profile=Warm\n") == 0);
    CHECK(expect_rejected(directory, "bad-format",
        "schema=1\ntarget_id=lidar\ncapture_format=float_le\n") == 0);
    CHECK(expect_rejected(directory, "bad-rate",
        "schema=1\ntarget_id=lidar\ncapture_rate=12345\n") == 0);
    CHECK(expect_rejected(directory, "bad-channels",
        "schema=1\ntarget_id=lidar\ncapture_channels=9\n") == 0);
    CHECK(expect_rejected(directory, "bad-gain",
        "schema=1\ntarget_id=lidar\nhp_driver_gain=36\n") == 0);
    CHECK(expect_rejected(directory, "bad-pre-gain",
        "schema=1\ntarget_id=lidar\npre_gain_db=12.5\n") == 0);
    CHECK(expect_rejected(directory, "bad-role",
        "schema=1\ntarget_id=lidar\n"
        "capture_channels=8\ncapture_roles=mic0,mic1,mic2,mic3,mic4,mic5,"
        "mic6,mic7\n") == 0);
    CHECK(expect_rejected(directory, "short-roles",
        "schema=1\ntarget_id=lidar\n"
        "capture_channels=8\ncapture_roles=mic0,mic1\n") == 0);
    CHECK(expect_rejected(directory, "bad-aec-source",
        "schema=1\ntarget_id=lidar\naec_source=usb\n") == 0);
    CHECK(expect_rejected(directory, "no-equals",
        "schema=1\ntarget_id=lidar\ngarbage\n") == 0);
    CHECK(expect_rejected(directory, "empty", "") == 0);

    rmdir(directory);
    puts("audio profile: Lidar parses, MT8163 defaults, malformed rejected");
    return 0;
}
