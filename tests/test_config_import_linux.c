/*
 * Configuration restore is a backend operation, not a mock-only one. The
 * import path used to reject every non-mock backend before it validated the
 * document, so a real device could export its configuration and then fail to
 * restore it. Drive the shipped exporter and importer against a Linux-mode
 * backend whose device operations are the in-process mock, and check that the
 * restored file is written with the owner-only mode.
 *
 * The integrations field needs care on a real backend. Bit 0 (Home
 * Assistant) and bits 8/16/32/64 (Bluetooth, AirPlay, Spotify, Sendspin) only
 * mean the matching service is running: their handlers start it as well as
 * setting the bit. Import cannot run those transitions, so a restore keeps
 * the device's own bits for them while still importing every other bit.
 */
#define _POSIX_C_SOURCE 200809L
#include "../src/api.c"
#include "../src/backend_internal.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char csrf[] =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

static const unsigned applied_bits = 1u | 8u | 16u | 32u | 64u;

/* Rewrite the exported integrations value so the import differs from the
 * device's current state. */
static void set_integrations(char *doc, size_t size, unsigned value)
{
    static const char key[] = "\"integrations\": ";
    char *at = strstr(doc, key);
    char *end;
    char tail[8192];
    size_t prefix;

    assert(at != NULL);
    at += sizeof(key) - 1;
    end = at;
    while (*end >= '0' && *end <= '9') end++;
    assert(end > at);
    snprintf(tail, sizeof(tail), "%s", end);
    prefix = (size_t)(at - doc);
    assert(snprintf(at, size - prefix, "%u%s", value, tail) > 0);
}

int main(void)
{
    char dir[] = "/tmp/libreecho-import.XXXXXX";
    char path[384], exported[8192];
    struct le_backend *backend = NULL;
    struct api_context api;
    struct stat st;
    FILE *file;

    assert(mkdtemp(dir));
    snprintf(path, sizeof(path), "%s/config.json", dir);
    assert(le_backend_init(&backend, "mock", NULL, NULL, 7) == LE_OK);
    /* The mock's device operations run in Linux mode, as on the target. */
    strcpy(backend->mode, "linux");
    assert(api_init(&api, backend, 1, 0, NULL, NULL, csrf, path, NULL) == 0);

    /* Bluetooth is on in the device's current state; the file enables every
     * integration, including the ones the device cannot import. */
    api.integrations = 4u | 8u;
    assert(configuration_json(&api, exported, sizeof(exported)) == 0);
    {
        int schema = 0;

        assert(json_get_int(exported, "schema_version", &schema) == 1);
        assert(schema == 1);
    }
    set_integrations(exported, sizeof(exported), 127);

    /* A malformed document is rejected before anything is written. */
    assert(import_configuration(&api, "{}", 2) == LE_INVALID);

    assert(import_configuration(&api, exported, strlen(exported)) == LE_OK);
    /* Bluetooth stays on and MQTT comes from the file; the HA, AirPlay,
     * Spotify and Sendspin bits stay off because their services never ran. */
    assert((api.integrations & 8u) != 0);
    assert((api.integrations & 2u) != 0);
    assert((api.integrations & 4u) != 0);
    assert((api.integrations & (applied_bits & ~8u)) == 0);
    assert(stat(path, &st) == 0);
    assert(S_ISREG(st.st_mode));
    assert((st.st_mode & 0777) == 0600);

    /* Clearing integrations in the file still cannot switch off a service
     * that is running on the device. */
    set_integrations(exported, sizeof(exported), 0);
    assert(import_configuration(&api, exported, strlen(exported)) == LE_OK);
    assert((api.integrations & 8u) != 0);
    assert((api.integrations & 2u) == 0);

    /* The mock backend simulates the services, so it applies the file. */
    strcpy(backend->mode, "mock");
    set_integrations(exported, sizeof(exported), 127);
    assert(import_configuration(&api, exported, strlen(exported)) == LE_OK);
    assert(api.integrations == 127u);

    /* The imported document survives a re-export unchanged in the fields the
     * restore is responsible for. */
    {
        char again[8192];

        assert(configuration_json(&api, again, sizeof(again)) == 0);
        assert(strstr(again, "\"schema_version\": 1") != NULL);
        assert(strstr(again, "\"volume\":") != NULL);
    }

    file = fopen(path, "r");
    assert(file != NULL);
    fclose(file);
    remove(path);
    rmdir(dir);
    le_backend_destroy(backend);
    puts("config import: non-mock restore persists and keeps applied integrations: ok");
    return 0;
}
