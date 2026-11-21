#ifndef LIBREECHO_AUDIO_PROFILE_FIXTURE_H
#define LIBREECHO_AUDIO_PROFILE_FIXTURE_H

/* The bytes libreecho_platform_targets.audio_profile_bytes('lidar') renders,
 * so the tests parse what the image build actually writes. */
static const char lidar_profile[] =
    "schema=1\n"
    "target_id=lidar\n"
    "speaker_chain=lidar\n"
    "codec_profile=Flat\n"
    "hp_driver_gain=6\n"
    "lo_driver_gain=6\n"
    "pre_gain_db=0.00\n"
    "bass_makeup_db=0.00\n"
    "playback_card=0\n"
    "playback_device=0\n"
    "playback_rate=48000\n"
    "playback_channels=2\n"
    "playback_format=s16_le\n"
    "capture_card=0\n"
    "capture_device=1\n"
    "capture_rate=16000\n"
    "capture_channels=8\n"
    "capture_format=s32_le\n"
    "capture_roles=mic0,mic1,mic2,mic3,mic4,mic5,mic6,line-in\n"
    "capture_chain=lidar\n"
    "aec_source=separate-pcm\n"
    "aec_card=0\n"
    "aec_device=2\n"
    "aec_rate=48000\n"
    "aec_channels=2\n"
    "aec_format=s16_le\n";

#endif
