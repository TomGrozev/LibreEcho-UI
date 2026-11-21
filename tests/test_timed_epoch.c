/*
 * timed keeps the last synchronized wall clock in userdata for boards with no
 * RTC (the Lidar Echo Plus has none), and must reject a torn or stale store.
 */
#define main timed_main
#include "adapter/timed.c"
#undef main

#include <stdio.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        return 1; \
    } \
} while (0)

static int write_text(const char *path, const char *text)
{
    FILE *file = fopen(path, "w");
    if (!file)
        return -1;
    if (fputs(text, file) == EOF) {
        fclose(file);
        return -1;
    }
    return fclose(file);
}

int main(void)
{
    char directory[] = "/tmp/libreecho-epoch-test.XXXXXX";
    char path[256];
    time_t epoch = 0;

    CHECK(mkdtemp(directory) != NULL);
    snprintf(path, sizeof(path), "%s/epoch", directory);

    CHECK(!read_epoch_file(path, &epoch));
    CHECK(persist_epoch(path, 1700000000) == 0);
    CHECK(read_epoch_file(path, &epoch) && epoch == 1700000000);
    CHECK(persist_epoch(path, 1800000000) == 0);
    CHECK(read_epoch_file(path, &epoch) && epoch == 1800000000);

    CHECK(write_text(path, "123\n") == 0);
    CHECK(!read_epoch_file(path, &epoch));
    CHECK(write_text(path, "not-a-number\n") == 0);
    CHECK(!read_epoch_file(path, &epoch));
    CHECK(write_text(path, "1700000000junk\n") == 0);
    CHECK(!read_epoch_file(path, &epoch));

    CHECK(unlink(path) == 0);
    CHECK(persist_epoch(path, 1000000000) == 0);
    CHECK(access(path, F_OK) != 0);

    printf("timed epoch persistence: ok\n");
    return 0;
}
