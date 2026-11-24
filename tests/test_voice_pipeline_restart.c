#define _POSIX_C_SOURCE 200809L
#define LE_INIT_AGENTD "build/libreecho-pr152-agentd.init"
#define LE_INIT_STTD "build/libreecho-pr152-sttd.init"
#define LE_INIT_TTSD "build/libreecho-pr152-ttsd.init"
#define LE_INIT_MDNSD "build/test-control-no-mdns.init"
#define LE_INIT_ESPHOMED "build/libreecho-pr152-esphomed.init"
#include "../src/api.c"

#include <assert.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char *const init_paths[] = {
    LE_INIT_AGENTD, LE_INIT_STTD, LE_INIT_TTSD, LE_INIT_ESPHOMED
};

static void write_init_script(const char *path, int status)
{
    FILE *file = fopen(path, "w");

    assert(file != NULL);
    assert(fputs("#!/bin/sh\nsleep 1\nexit ", file) >= 0);
    assert(fprintf(file, "%d\n", status) > 0);
    assert(fclose(file) == 0);
    assert(chmod(path, 0700) == 0);
}

static void remove_scripts(void)
{
    size_t i;

    for (i = 0; i < sizeof(init_paths) / sizeof(init_paths[0]); ++i)
        unlink(init_paths[i]);
}

static void wait_for_restart(void)
{
    struct timespec delay = {0, 50000000L};
    unsigned int attempts;

    for (attempts = 0; attempts < 240; ++attempts) {
        if (!voice_pipeline_restart_pending())
            return;
        nanosleep(&delay, NULL);
    }
    assert(!"voice pipeline restart did not finish");
}

int main(void)
{
    size_t i;

    remove_scripts();
    for (i = 0; i < sizeof(init_paths) / sizeof(init_paths[0]); ++i)
        write_init_script(init_paths[i], 0);

    /* One restart is accepted; a second cannot create another worker. */
    assert(apply_voice_pipeline_mode("local") == LE_BUSY);
    assert(apply_voice_pipeline_mode("local") == LE_BUSY);
    wait_for_restart();
    assert(!strcmp(voice_pipeline_restart_state(), "ready"));

    /* A child failure is observable and is returned on the next PUT attempt. */
    write_init_script(LE_INIT_AGENTD, 1);
    assert(apply_voice_pipeline_mode("local") == LE_BUSY);
    wait_for_restart();
    assert(!strcmp(voice_pipeline_restart_state(), "failed"));
    assert(apply_voice_pipeline_mode("local") == LE_IO);

    /*
     * A failed forward transition is reported, but only the tick starts its
     * rollback: a request-path advance must leave the rollback undone and
     * still expose the phase.
     */
    {
        struct api_context c;
        struct timespec delay = {0, 50000000L};
        unsigned int attempts;

        memset(&c, 0, sizeof(c));
        snprintf(c.config_path, sizeof(c.config_path),
                 "/tmp/libreecho-restart-phase-config.json");
        snprintf(c.voice_pipeline_mode, sizeof(c.voice_pipeline_mode),
                 "local");
        unlink(c.config_path);
        write_init_script(LE_INIT_AGENTD, 1);
        voice_transition_capture(&c);
        voice_pipeline_restart_result = LE_OK;
        assert(apply_voice_pipeline_mode("local") == LE_BUSY);
        for (attempts = 0; attempts < 240; ++attempts) {
            (void)voice_pipeline_restart_advance(0);
            if (voice_pipeline_restart_pid <= 0)
                break;
            nanosleep(&delay, NULL);
        }
        assert(voice_pipeline_restart_pid <= 0);
        assert(voice_pipeline_rollback_needed);
        assert(!strcmp(voice_pipeline_restart_state(), "pending"));
        assert(!strcmp(voice_pipeline_restart_phase(), "rolling-back"));
        (void)voice_pipeline_restart_advance(1);   /* the tick starts it */
        wait_for_restart();
        assert(!strcmp(voice_pipeline_restart_state(), "failed"));
        voice_pipeline_restart_result = LE_OK;
        unlink(c.config_path);
    }

    /* Home Assistant is rejected before any local daemon is stopped. */
    unlink(LE_INIT_ESPHOMED);
    assert(apply_voice_pipeline_mode("home-assistant") == LE_NOT_SUPPORTED);

    remove_scripts();
    puts("voice pipeline restart: bounded, observable, and capability-gated PASS");
    return 0;
}
