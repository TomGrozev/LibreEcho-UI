#ifndef LE_IMAGE_TARGET_H
#define LE_IMAGE_TARGET_H
/* The image builder writes target_id=<id>, one per line, to this file on every
 * target. A missing file means the image carries no identity line. */
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#define LE_IMAGE_TARGET_FILE "/etc/libreecho/target"

static inline int le_image_target_is(const char *id)
{
    char text[256];
    char *line, *save = NULL;
    size_t id_length;
    ssize_t n;
    int fd, found = 0;

    if (!id || !*id)
        return 0;
    id_length = strlen(id);
    fd = open(LE_IMAGE_TARGET_FILE, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    n = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    text[n] = '\0';
    for (line = strtok_r(text, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *value = line + 10;
        size_t value_length;

        if (strncmp(line, "target_id=", 10))
            continue;
        value_length = strlen(value);
        if (value_length && value[value_length - 1] == '\r')
            --value_length;
        if (value_length == id_length && !memcmp(value, id, id_length)) {
            found = 1;
            break;
        }
    }
    return found;
}
#endif
