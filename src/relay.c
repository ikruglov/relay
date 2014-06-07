#include "relay.h"
#include "worker.h"
#include "setproctitle.h"

static void cleanup();
static void sig_handler(int signum);
static sock_t *s_listen;
volatile uint32_t ABORT = 0;
#define DIE 1
#define RELOAD 2

struct config {
    char **argv;
    int argc;
    char *file;
    pthread_mutex_t lock;
} CONFIG;


void set_abort_bits(uint32_t v) {
    RELAY_ATOMIC_OR(ABORT, v);
}

void unset_abort_bits(uint32_t v) {
    v= ~v;
    RELAY_ATOMIC_AND(ABORT, v);
}

void set_aborted() {
    set_abort_bits(DIE);
}

int get_abort_val() {
    return RELAY_ATOMIC_READ(ABORT);
}

int not_aborted() {
    uint32_t v= RELAY_ATOMIC_READ(ABORT);
    return (v & DIE) == 0;
}

static void spawn(pthread_t *tid,void *(*func)(void *), void *arg, int type) {
    pthread_t unused;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, type);
    pthread_create(tid ? tid : &unused, &attr, func, arg);
    pthread_attr_destroy(&attr);
}

void trim(char * s) {
    char *p = s;
    int l = strlen(p);

    while(isspace(p[l - 1])) p[--l] = 0;
    while(*p && isspace(*p)) ++p, --l;

    memmove(s, p, l + 1);
}

void reload_config_file(void) {
    if (!CONFIG.file)
        return;
    FILE *f;
    char *line;
    ssize_t read;
    size_t len = 0;
    int i;

    f = fopen(CONFIG.file, "r");
    if (f == NULL)
        SAYPX("fopen");

    if (CONFIG.argc > 0) {
        for (i=0;i<CONFIG.argc;i++)
            free(CONFIG.argv[i]);
    }

    CONFIG.argc = 0;
    while ((read = getline(&line, &len, f)) != -1) {
        char *p;
        if ((p = strchr(line, '#')))
            *p = '\0';

        trim(line);
        if (strlen(line) != 0) {
            CONFIG.argv = realloc_or_die(CONFIG.argv, sizeof(line) * (CONFIG.argc + 1));
            CONFIG.argv[CONFIG.argc] = strdup(line);
            CONFIG.argc++;
        }
    }
    if (line)
        free(line);
    _D("loading config file %s", CONFIG.file);
}

void load_config(int argc, char **argv) {
    int i = 0;
    CONFIG.argv = NULL;
    CONFIG.argc = 0;
    CONFIG.file = NULL;
    if (argc == 2) {
        CONFIG.file = argv[1];
        reload_config_file();
    } else {
        CONFIG.argv = realloc_or_die(CONFIG.argv, sizeof(char *) * (argc));
        for (i=0; i < argc - 1; i++) {
            CONFIG.argv[i] = strdup(argv[i + 1]);
        }
        CONFIG.argc = i;
    }
}

void reload_workers(int reload) {
    worker_init_static(CONFIG.argc - 1, &CONFIG.argv[1], reload);
}

void *udp_server(void *arg) {
    sock_t *s = (sock_t *) arg;
    ssize_t received;
#ifdef PACKETS_PER_SECOND
    uint32_t packets = 0, prev_packets = 0;
    uint32_t epoch, prev_epoch = 0;
#endif
    char buf[MAX_CHUNK_SIZE]; // unused, but makes recv() happy
    while (not_aborted()) {
        received = recv(s->socket, buf, MAX_CHUNK_SIZE, MSG_PEEK);
#ifdef PACKETS_PER_SECOND
        if ((epoch = time(0)) != prev_epoch) {
            _D("packets: %d", packets - prev_packets);
            prev_epoch = epoch;
            prev_packets = packets;
        }
        packets++;
#endif
        if (received > 0) {
            blob_t *b = b_new(received);
            received = recv(s->socket, BLOB_BUF(b), received, 0);
            if (received < 0)
                break;
            enqueue_blob_for_transmission(b);
        } else {
            break;
        }
    }
    _ENO("recv failed");
    set_aborted();
    pthread_exit(NULL);
}


void *tcp_worker(void *arg) {
    int fd = (int )arg;
    _D("new tcp worker for fd: %d", fd);
    while (not_aborted()) {
        uint32_t expected;
        int rc = recv(fd, &expected, sizeof(expected), MSG_WAITALL);
        if (rc != sizeof(expected)) {
            _ENO("failed to receive header for next packet, expected: %zu got: %d", sizeof(expected), rc);
            break;
        }

        if (expected > MAX_CHUNK_SIZE) {
            _ENO("requested packet(%d) > MAX_CHUNK_SIZE(%d)", expected, MAX_CHUNK_SIZE);
            break;
        }

        blob_t *b = b_new(expected);
        rc = recv(fd, BLOB_BUF_addr(b), expected, MSG_WAITALL);
        if (rc != BLOB_BUF_SIZE(b)) {
            _ENO("failed to receve packet payload, expected: %d got: %d", BLOB_BUF_SIZE(b), rc);
            b_destroy(b);
            break;
        }
        enqueue_blob_for_transmission(b);
    }
    close(fd);
    pthread_exit(NULL);
}

void *tcp_server(void *arg) {
    sock_t *s = (sock_t *) arg;

    for (;;) {
        int fd = accept(s->socket, NULL, NULL);
        if (fd < 0) {
            set_aborted();
            _ENO("accept");
            break;
        }
        spawn(NULL, tcp_worker, (void *)fd, PTHREAD_CREATE_DETACHED);
    }

    pthread_exit(NULL);
}

int main(int argc, char **argv) {
    pthread_t server_tid;
    signal(SIGTERM, sig_handler);
    signal(SIGINT, sig_handler);
    signal(SIGPIPE, sig_handler);
    signal(SIGHUP, sig_handler);

    load_config(argc, argv);

    initproctitle(argc, argv);
    setproctitle("relay","testing");

    if (CONFIG.argc < 2)
        SAYX(EXIT_FAILURE, "%s local-host:local-port tcp@remote-host:remote-port ...\n"     \
                          "or file with socket description per day like:\n"                 \
                          "\tlocal-host:local-port\n"                                       \
                          "\ttcp@remote-host:remote-port ...\n", argv[0]);
    s_listen = malloc_or_die(sizeof(*s_listen));
    socketize(CONFIG.argv[0], s_listen);
    reload_workers(0);
    open_socket(s_listen, DO_BIND);

    if (s_listen->proto == IPPROTO_UDP)
        spawn(&server_tid, udp_server, s_listen, PTHREAD_CREATE_JOINABLE);
    else
        spawn(&server_tid, tcp_server, s_listen, PTHREAD_CREATE_JOINABLE);

    for (;;) {
        int abort= get_abort_val();
        if (abort & DIE) {
            break;
        }
        else
        if (abort & RELOAD) {
            reload_config_file();
            reload_workers(1);
            unset_abort_bits(RELOAD);
        }
        sleep(1);
    }
    cleanup(server_tid);
    _D("bye");
    return(0);
}

static void sig_handler(int signum) {
    switch(signum) {
        case SIGHUP:
            set_abort_bits(RELOAD);
            break;
        case SIGTERM:
        case SIGINT:
            set_aborted();
            break;
        default:
            _E("IGNORE: unexpected signal %d", signum);
    }
}

static void cleanup(pthread_t server_tid) {
    shutdown(s_listen->socket, SHUT_RDWR);
    close(s_listen->socket);
    pthread_join(server_tid, NULL);
    worker_destroy_static();
    int i;
    for (i = 0; i < CONFIG.argc; i++)
        free(CONFIG.argv[i]);

    free(s_listen);
    sleep(1); // give a chance to the detachable tcp worker threads to pthread_exit()
}
