/*
 * The Lidar radio setting rides in the exported configuration and in config
 * import: a document may omit it (the EFR32 stays at "none") or carry one of
 * the three the owner can choose, but an unknown value is rejected before
 * anything is written. The value is only persisted on the Lidar target, so
 * Radar and Biscuit exports stay byte-identical.
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

static void add_radio(const char *src, const char *value, char *out, size_t size)
{
    const char *key = strstr(src, "\"radio\": \"");
    if (key != NULL) {
        const char *start = key + strlen("\"radio\": \"");
        const char *end = strchr(start, '"');
        assert(end != NULL);
        assert(snprintf(out, size, "%.*s%s%s", (int)(start - src), src,
                        value, end) < (int)size);
        return;
    }
    {
        const char *brace = strrchr(src, '}');
        assert(brace != NULL);
        assert(snprintf(out, size, "%.*s,\n  \"radio\": \"%s\"%s",
                        (int)(brace - src), src, value, brace) < (int)size);
    }
}

int main(void)
{
    char dir[] = "/tmp/libreecho-import.XXXXXX";
    char path[384], exported[8192], document[8192];
    struct le_backend *backend = NULL;
    struct api_context api;
    struct api_request request;
    struct api_response response;
    struct stat st;
    FILE *file;
    size_t length;

    assert(mkdtemp(dir));
    snprintf(path, sizeof(path), "%s/config.json", dir);
    assert(le_backend_init(&backend, "mock", NULL, NULL, 7) == LE_OK);
    assert(api_init(&api, backend, 1, 0, NULL, NULL, csrf, path, NULL) == 0);
    api.target_lidar = 1;  /* radio is persisted only on the Lidar image */

    assert(configuration_json(&api, exported, sizeof(exported)) == 0);
    {
        int schema = 0;

        assert(json_get_int(exported, "schema_version", &schema) == 1);
        assert(schema == 1);
    }

    /* A malformed document is rejected before anything is written. */
    assert(import_configuration(&api, "{}", 2) == LE_INVALID);
    assert(!strcmp(api.radio_setting, "none"));

    /* radio is optional and defaults to none, but a present value must be one
     * of the three the EFR32 owner can choose. */
    add_radio(exported, "bogus", document, sizeof(document));
    assert(import_configuration(&api, document, strlen(document)) == LE_INVALID);
    assert(!strcmp(api.radio_setting, "none"));

    add_radio(exported, "thread", document, sizeof(document));
    assert(import_configuration(&api, document, strlen(document)) == LE_OK);
    assert(!strcmp(api.radio_setting, "thread"));
    length = (size_t)config_read(path, document, sizeof(document));
    assert(length > 0 && strstr(document, "\"radio\": \"thread\"") != NULL);
    assert(stat(path, &st) == 0);
    assert(S_ISREG(st.st_mode));
    assert((st.st_mode & 0777) == 0600);

    /* The imported document survives a re-export unchanged in the fields the
     * restore is responsible for. */
    {
        char again[8192];

        assert(configuration_json(&api, again, sizeof(again)) == 0);
        assert(strstr(again, "\"schema_version\": 1") != NULL);
        assert(strstr(again, "\"volume\":") != NULL);
        assert(strstr(again, "\"radio\": \"thread\"") != NULL);
    }

    /* The endpoint exists only on Lidar and rejects a value outside the enum
     * before it is stored. */
    memset(&request, 0, sizeof(request));
    snprintf(request.path, sizeof(request.path), "/api/v1/radio");
    snprintf(request.method, sizeof(request.method), "GET");
    api.target_lidar = 0;
    memset(&response, 0, sizeof(response));
    api_handle(&api, &request, &response);
    assert(response.status == 404);

    api.target_lidar = 1;
    snprintf(request.method, sizeof(request.method), "PUT");
    snprintf(request.csrf, sizeof(request.csrf), "%s", csrf);
    request.body = "{\"radio\":\"bogus\"}";
    request.body_len = strlen(request.body);
    memset(&response, 0, sizeof(response));
    api_handle(&api, &request, &response);
    assert(response.status == 400);
    assert(!strcmp(api.radio_setting, "thread"));

    file = fopen(path, "r");
    assert(file != NULL);
    fclose(file);
    remove(path);
    rmdir(dir);
    le_backend_destroy(backend);
    puts("config import: Lidar radio round-trips through import and export: ok");
    return 0;
}
