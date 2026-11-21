/*
 * The kernel rejects MGMT SET_DISCOVERABLE unless the controller is already
 * connectable, for the disable request too (net/bluetooth/mgmt.c).  The
 * standalone btd command used to send it alone, so the web API turned every
 * discoverable request into a 501.  A socketpair stands in for the management
 * channel and records the opcodes btd sends.
 */
#define main btd_main
#include "adapter/btd.c"
#undef main

#include <signal.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/wait.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        return 1; \
    } \
} while (0)

int le_profile_open(struct le_profiles *p, const char *name)
{
    (void)p; (void)name; return -1;
}
void le_profile_close(struct le_profiles *p) { (void)p; }
int le_profile_poll_setup(struct le_profiles *p, struct pollfd *pollfds,
                          int max_fds, int *fd_map)
{
    (void)p; (void)pollfds; (void)max_fds; (void)fd_map; return 0;
}
void le_profile_poll_events(struct le_profiles *p, const struct pollfd *pollfds,
                            const int *fd_map, int count)
{
    (void)p; (void)pollfds; (void)fd_map; (void)count;
}
int le_profile_registered_sdp(const struct le_profiles *p) { (void)p; return 0; }
int le_profile_registered_a2dp_sink(const struct le_profiles *p) { (void)p; return 0; }
int le_profile_registered_avrcp(const struct le_profiles *p) { (void)p; return 0; }
int le_profile_stream_active(const struct le_profiles *p) { (void)p; return 0; }
void le_profile_set_volume_handler(struct le_profiles *p,
                                   int (*set)(void *context, int percent),
                                   void *context)
{ (void)p; (void)set; (void)context; }
void le_profile_set_local_volume(struct le_profiles *p, int percent)
{ (void)p; (void)percent; }

static int read_full(int fd, void *buffer, size_t size)
{
    char *at = buffer;
    while (size) {
        ssize_t count = read(fd, at, size);
        if (count <= 0)
            return -1;
        at += count;
        size -= (size_t)count;
    }
    return 0;
}

static int write_full(int fd, const void *buffer, size_t size)
{
    const char *at = buffer;
    while (size) {
        ssize_t count = write(fd, at, size);
        if (count <= 0)
            return -1;
        at += count;
        size -= (size_t)count;
    }
    return 0;
}

/* Answers each management command with a successful command-complete and
 * records "opcode value" in order. */
static void serve_mgmt(int fd, const char *log_path, int commands)
{
    uint8_t header[sizeof(struct mgmt_hdr_wire)];
    uint8_t payload[MGMT_PACKET_MAX];
    uint8_t response[sizeof(struct mgmt_hdr_wire) + 3];
    FILE *log = fopen(log_path, "w");
    int i;

    if (!log)
        _exit(1);
    for (i = 0; i < commands; ++i) {
        uint16_t opcode, length;

        if (read_full(fd, header, sizeof(header)) != 0)
            _exit(2);
        memcpy(&opcode, header, sizeof(opcode));
        memcpy(&length, header + 4, sizeof(length));
        if (length > sizeof(payload) ||
            read_full(fd, payload, length) != 0)
            _exit(3);
        fprintf(log, "%04x %u\n", opcode, length ? payload[0] : 0);
        fflush(log);
        memset(response, 0, sizeof(response));
        memcpy(response, "\x01\x00", 2);
        memcpy(response + 4, "\x03\x00", 2);
        memcpy(response + 6, header, 2);
        if (write_full(fd, response, sizeof(response)) != 0)
            _exit(4);
    }
    fclose(log);
    _exit(0);
}

static pid_t spawn_responder(int fd, const char *log_path, int commands)
{
    pid_t pid = fork();
    if (pid == 0)
        serve_mgmt(fd, log_path, commands);
    return pid;
}

static int request(struct bt_context *context, const char *command,
                   int enabled, char *response, size_t response_size)
{
    char message[LE_ADAPTER_MSG_MAX];

    snprintf(message, sizeof(message),
             "{\"v\":1,\"id\":1,\"cmd\":\"%s\",\"args\":{\"enabled\":%s}}",
             command, enabled ? "true" : "false");
    return handle_request(context, message, response, response_size);
}

static int check_log(const char *path, const char *expected)
{
    FILE *file = fopen(path, "r");
    char actual[4096];
    size_t used;

    if (!file)
        return -1;
    used = fread(actual, 1, sizeof(actual) - 1, file);
    fclose(file);
    actual[used] = '\0';
    return strcmp(actual, expected) == 0 ? 0 : -1;
}

static int test_discoverable_enables_connectable_first(void)
{
    struct bt_context context;
    char response[LE_ADAPTER_MSG_MAX];
    char log_path[] = "/tmp/libreecho-btd-order.XXXXXX";
    int pair[2], fd;
    pid_t pid;

    memset(&context, 0, sizeof(context));
    context.enabled = 1;
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    fd = mkstemp(log_path);
    CHECK(fd >= 0);
    close(fd);
    pid = spawn_responder(pair[1], log_path, 4);
    CHECK(pid > 0);
    close(pair[1]);
    context.mgmt_fd = pair[0];

    CHECK(request(&context, "set_discoverable", 1, response,
                  sizeof(response)) > 0);
    CHECK(strstr(response, "\"ok\":true") != NULL);
    CHECK(context.current_settings & MGMT_SETTING_CONNECTABLE);
    CHECK(context.current_settings & MGMT_SETTING_DISCOVERABLE);

    CHECK(request(&context, "set_discoverable", 0, response,
                  sizeof(response)) > 0);
    CHECK(strstr(response, "\"ok\":true") != NULL);
    CHECK(!(context.current_settings & MGMT_SETTING_CONNECTABLE));
    CHECK(!(context.current_settings & MGMT_SETTING_DISCOVERABLE));
    CHECK(context.discoverable_auto_connectable == 0);

    close(pair[0]);
    CHECK(waitpid(pid, NULL, 0) == pid);
    CHECK(check_log(log_path,
                    "0007 1\n0006 1\n0006 0\n0007 0\n") == 0);
    unlink(log_path);
    return 0;
}

static int test_discoverable_keeps_existing_connectable(void)
{
    struct bt_context context;
    char response[LE_ADAPTER_MSG_MAX];
    char log_path[] = "/tmp/libreecho-btd-order.XXXXXX";
    int pair[2], fd;
    pid_t pid;

    memset(&context, 0, sizeof(context));
    context.enabled = 1;
    context.current_settings = MGMT_SETTING_CONNECTABLE;
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    fd = mkstemp(log_path);
    CHECK(fd >= 0);
    close(fd);
    pid = spawn_responder(pair[1], log_path, 2);
    CHECK(pid > 0);
    close(pair[1]);
    context.mgmt_fd = pair[0];

    CHECK(request(&context, "set_discoverable", 1, response,
                  sizeof(response)) > 0);
    CHECK(request(&context, "set_discoverable", 0, response,
                  sizeof(response)) > 0);

    close(pair[0]);
    CHECK(waitpid(pid, NULL, 0) == pid);
    CHECK(check_log(log_path, "0006 1\n0006 0\n") == 0);
    CHECK(context.current_settings & MGMT_SETTING_CONNECTABLE);
    CHECK(!(context.current_settings & MGMT_SETTING_DISCOVERABLE));
    unlink(log_path);
    return 0;
}

static int test_disable_when_not_connectable_sends_nothing(void)
{
    struct bt_context context;
    char response[LE_ADAPTER_MSG_MAX];

    memset(&context, 0, sizeof(context));
    context.enabled = 1;
    context.mgmt_fd = -1;

    CHECK(request(&context, "set_discoverable", 0, response,
                  sizeof(response)) > 0);
    CHECK(strstr(response, "\"ok\":true") != NULL);
    CHECK(strstr(response, "\"changed\":false") != NULL);
    CHECK(context.current_settings == 0);
    CHECK(context.discoverable_auto_connectable == 0);
    return 0;
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    if (test_discoverable_enables_connectable_first() ||
        test_discoverable_keeps_existing_connectable() ||
        test_disable_when_not_connectable_sends_nothing())
        return 1;
    printf("bluetooth discoverable order: ok\n");
    return 0;
}
