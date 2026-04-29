/*
 * server.c — The Wired Server
 *
 * Konsep dari README:
 *  - Socket Programming  : socket(), bind(), listen(), accept()
 *  - select()            : Async I/O, monitor banyak fd tanpa thread (Extras > Asynchronous Programming)
 *  - IPC/Message Passing : Broadcast pesan antar client via send()
 *  - RPC                 : Admin console dengan prosedur jarak jauh (GET_USERS, GET_UPTIME, SHUTDOWN)
 *
 * Compile: gcc -o server server.c
 * Run   : ./server
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ── Konstanta ─────────────────────────────────────────────────────────────── */
#define MAX_CLIENTS   64
#define BUF_SIZE      2048
#define NAME_SIZE     128
#define LOG_FILE      "history.log"

/* ── State per client ──────────────────────────────────────────────────────── */
typedef enum { STAGE_NAME, STAGE_PASSWORD, STAGE_CHAT, STAGE_ADMIN } Stage;

typedef struct {
    int  fd;
    char name[NAME_SIZE];
    int  is_admin;
    Stage stage;
    char buf[BUF_SIZE];   /* partial read buffer */
    int  buf_len;
} Client;

/* ── Globals ───────────────────────────────────────────────────────────────── */
static Client   clients[MAX_CLIENTS];
static int      client_count = 0;
static time_t   server_start;
static int      server_fd = -1;

/* Config dibaca dari file 'protocol' */
static char  CFG_HOST[64]  = "127.0.0.1";
static int   CFG_PORT       = 9999;
static char  CFG_ADMIN[NAME_SIZE] = "The Knights";
static char  CFG_PASS[NAME_SIZE]  = "protocol7";

/* ── Logging ───────────────────────────────────────────────────────────────── */
static void log_write(const char *level, const char *msg) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", t);

    char line[BUF_SIZE + 128];
    snprintf(line, sizeof(line), "[%s] [%s] [%s]", ts, level, msg);
    printf("%s\n", line);
    fflush(stdout);

    FILE *f = fopen(LOG_FILE, "a");
    if (f) {
        fprintf(f, "%s\n", line);
        fclose(f);
    }
}

/* ── Baca protocol ─────────────────────────────────────────────────────────── */
static void load_protocol(void) {
    FILE *f = fopen("protocol", "r");
    if (!f) { perror("fopen protocol"); exit(1); }
    fscanf(f, "%63s\n%d\n%127[^\n]\n%127[^\n]",
           CFG_HOST, &CFG_PORT, CFG_ADMIN, CFG_PASS);
    fclose(f);
}

/* ── Cari slot kosong ──────────────────────────────────────────────────────── */
static Client *find_slot(void) {
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd == -1) return &clients[i];
    return NULL;
}

/* ── Cek nama sudah dipakai ────────────────────────────────────────────────── */
static int name_taken(const char *name) {
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd != -1 &&
            clients[i].stage >= STAGE_CHAT &&
            strcmp(clients[i].name, name) == 0)
            return 1;
    return 0;
}

/* ── Hitung user aktif (non-admin) ────────────────────────────────────────── */
static int active_users(void) {
    int count = 0;
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd != -1 && !clients[i].is_admin &&
            clients[i].stage == STAGE_CHAT)
            count++;
    return count;
}

/* ── Kirim pesan ke satu fd ────────────────────────────────────────────────── */
static void send_to(int fd, const char *msg) {
    send(fd, msg, strlen(msg), 0);
}

/* ── Broadcast ke semua user chat (kecuali exclude_fd) ────────────────────── */
static void broadcast(const char *msg, int exclude_fd) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        Client *c = &clients[i];
        if (c->fd != -1 && !c->is_admin &&
            c->stage == STAGE_CHAT && c->fd != exclude_fd)
            send_to(c->fd, msg);
    }
}

/* ── Hapus / putus client ──────────────────────────────────────────────────── */
static void disconnect_client(Client *c) {
    if (c->fd == -1) return;

    char logmsg[NAME_SIZE + 32];
    snprintf(logmsg, sizeof(logmsg), "User '%s' disconnected", c->name);
    log_write("System", logmsg);

    if (!c->is_admin && c->stage == STAGE_CHAT) {
        char notice[NAME_SIZE + 64];
        snprintf(notice, sizeof(notice),
                 "[System] User '%s' has disconnected from The Wired.\n", c->name);
        broadcast(notice, c->fd);
    }

    close(c->fd);
    c->fd      = -1;
    c->buf_len = 0;
    c->stage   = STAGE_NAME;
    c->is_admin = 0;
    client_count--;
}

/* ── Menu admin ────────────────────────────────────────────────────────────── */
static const char *ADMIN_MENU =
    "\n=== THE KNIGHTS CONSOLE ===\n"
    "1. Check Active Entites (Users)\n"
    "2. Check Server Uptime\n"
    "3. Execute Emergency Shutdown\n"
    "4. Disconnect\n"
    "Command >> ";

static void send_admin_menu(Client *c) {
    send_to(c->fd, ADMIN_MENU);
}

/* ── Emergency shutdown ────────────────────────────────────────────────────── */
static void emergency_shutdown(void) {
    log_write("System", "EMERGENCY SHUTDOWN INITIATED");
    broadcast("[System] EMERGENCY SHUTDOWN: The Wired is going offline.\n", -1);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd != -1) {
            send_to(clients[i].fd, "[System] Server shutting down...\n");
            close(clients[i].fd);
            clients[i].fd = -1;
        }
    }
    if (server_fd != -1) close(server_fd);
    exit(0);
}

/* ── Handle SIGINT (Ctrl+C) di server ────────────────────────────────────── */
static void handle_sigint(int sig) {
    (void)sig;
    log_write("System", "SERVER SHUTDOWN (SIGINT)");
    broadcast("[System] The Wired server is shutting down.\n", -1);
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd != -1) close(clients[i].fd);
    if (server_fd != -1) close(server_fd);
    exit(0);
}

/* ── Proses satu baris dari client ────────────────────────────────────────── */
static void handle_line(Client *c, char *line) {
    /* Trim \r */
    int len = strlen(line);
    if (len > 0 && line[len-1] == '\r') line[--len] = '\0';

    /* ── Belum login: minta nama ──────────────────────────────────────────── */
    if (c->stage == STAGE_NAME) {
        if (len == 0) { send_to(c->fd, "Enter your name: "); return; }

        if (name_taken(line)) {
            char msg[NAME_SIZE + 80];
            snprintf(msg, sizeof(msg),
                     "[System] The identity '%s' is already synchronized in The Wired.\n"
                     "Enter your name: ", line);
            send_to(c->fd, msg);
            return;
        }

        strncpy(c->name, line, NAME_SIZE - 1);

        if (strcmp(c->name, CFG_ADMIN) == 0) {
            c->stage = STAGE_PASSWORD;
            send_to(c->fd, "Enter Password: ");
            return;
        }

        /* User biasa */
        c->stage = STAGE_CHAT;
        char logmsg[NAME_SIZE + 32];
        snprintf(logmsg, sizeof(logmsg), "User '%s' connected", c->name);
        log_write("System", logmsg);

        char welcome[NAME_SIZE + 64];
        snprintf(welcome, sizeof(welcome),
                 "--- Welcome to The Wired, %s ---\n", c->name);
        send_to(c->fd, welcome);

        char notice[NAME_SIZE + 64];
        snprintf(notice, sizeof(notice),
                 "[System] User '%s' has connected to The Wired.\n", c->name);
        broadcast(notice, c->fd);
        return;
    }

    /* ── Autentikasi admin ────────────────────────────────────────────────── */
    if (c->stage == STAGE_PASSWORD) {
        if (strcmp(line, CFG_PASS) == 0) {
            c->is_admin = 1;
            c->stage    = STAGE_ADMIN;

            char logmsg[NAME_SIZE + 32];
            snprintf(logmsg, sizeof(logmsg), "User '%s' connected", c->name);
            log_write("System", logmsg);
            log_write("System", "Authentication Successful for 'The Knights'");

            send_to(c->fd,
                "\n[System] Authentication Successful. Granted Admin privileges.");
            send_admin_menu(c);
        } else {
            send_to(c->fd, "[System] Authentication Failed. Access Denied.\n");
            disconnect_client(c);
        }
        return;
    }

    /* ── Admin RPC ────────────────────────────────────────────────────────── */
    if (c->stage == STAGE_ADMIN) {
        if (strcmp(line, "1") == 0) {
            log_write("Admin", "RPC_GET_USERS");
            char reply[64];
            snprintf(reply, sizeof(reply),
                     "[System] Active Entities: %d user(s) online.\n", active_users());
            send_to(c->fd, reply);
            send_admin_menu(c);

        } else if (strcmp(line, "2") == 0) {
            log_write("Admin", "RPC_GET_UPTIME");
            long elapsed = (long)(time(NULL) - server_start);
            long h = elapsed / 3600, m = (elapsed % 3600) / 60, s = elapsed % 60;
            char reply[64];
            snprintf(reply, sizeof(reply),
                     "[System] Server Uptime: %ldh %ldm %lds\n", h, m, s);
            send_to(c->fd, reply);
            send_admin_menu(c);

        } else if (strcmp(line, "3") == 0) {
            log_write("Admin", "RPC_SHUTDOWN");
            emergency_shutdown();

        } else if (strcmp(line, "4") == 0) {
            send_to(c->fd, "[System] Disconnecting from The Wired...\n");
            disconnect_client(c);

        } else {
            send_to(c->fd, "[System] Unknown command.\n");
            send_admin_menu(c);
        }
        return;
    }

    /* ── User chat ────────────────────────────────────────────────────────── */
    if (c->stage == STAGE_CHAT) {
        if (strcmp(line, "/exit") == 0) {
            send_to(c->fd, "[System] Disconnecting from The Wired...\n");
            disconnect_client(c);
            return;
        }
        if (len == 0) return;

        /* Broadcast + log */
        char msg[NAME_SIZE + BUF_SIZE + 8];
        snprintf(msg, sizeof(msg), "[%s]: %s\n", c->name, line);

        char logmsg[NAME_SIZE + BUF_SIZE + 8];
        snprintf(logmsg, sizeof(logmsg), "[%s]: %s", c->name, line);
        log_write("User", logmsg);

        broadcast(msg, c->fd);
    }
}

/* ── Baca data dari client, pecah per baris ────────────────────────────────── */
static void client_recv(Client *c) {
    char tmp[BUF_SIZE];
    int n = recv(c->fd, tmp, sizeof(tmp) - 1, 0);
    if (n <= 0) { disconnect_client(c); return; }

    /* Tambahkan ke partial buffer */
    int space = BUF_SIZE - c->buf_len - 1;
    if (n > space) n = space;
    memcpy(c->buf + c->buf_len, tmp, n);
    c->buf_len += n;
    c->buf[c->buf_len] = '\0';

    /* Proses setiap baris lengkap */
    char *start = c->buf;
    char *nl;
    while ((nl = strchr(start, '\n')) != NULL) {
        *nl = '\0';
        handle_line(c, start);
        /* handle_line bisa disconnect c → cek */
        if (c->fd == -1) return;
        start = nl + 1;
    }

    /* Sisa belum lengkap → geser ke awal buffer */
    int remaining = c->buf_len - (int)(start - c->buf);
    memmove(c->buf, start, remaining);
    c->buf_len = remaining;
    c->buf[c->buf_len] = '\0';
}

/* ── Main ──────────────────────────────────────────────────────────────────── */
int main(void) {
    load_protocol();
    signal(SIGINT, handle_sigint);
    server_start = time(NULL);

    /* Inisialisasi slot client */
    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].fd      = -1;
        clients[i].buf_len = 0;
        clients[i].stage   = STAGE_NAME;
        clients[i].is_admin = 0;
    }

    /* Buat server socket */
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); exit(1); }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(CFG_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); exit(1);
    }
    if (listen(server_fd, 10) < 0) {
        perror("listen"); exit(1);
    }

    log_write("System", "SERVER ONLINE");

    /* ── Event loop dengan select() ──────────────────────────────────────── */
    while (1) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(server_fd, &read_fds);
        int max_fd = server_fd;

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd != -1) {
                FD_SET(clients[i].fd, &read_fds);
                if (clients[i].fd > max_fd) max_fd = clients[i].fd;
            }
        }

        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int activity = select(max_fd + 1, &read_fds, NULL, NULL, &tv);
        if (activity < 0) continue; /* interrupted (e.g. SIGINT) */

        /* ── Koneksi baru ─────────────────────────────────────────────────── */
        if (FD_ISSET(server_fd, &read_fds)) {
            struct sockaddr_in caddr;
            socklen_t clen = sizeof(caddr);
            int new_fd = accept(server_fd, (struct sockaddr *)&caddr, &clen);
            if (new_fd < 0) { perror("accept"); continue; }

            Client *slot = find_slot();
            if (!slot) {
                send(new_fd, "[System] Server penuh.\n", 23, 0);
                close(new_fd);
            } else {
                slot->fd      = new_fd;
                slot->buf_len = 0;
                slot->stage   = STAGE_NAME;
                slot->is_admin = 0;
                client_count++;
                send_to(new_fd, "Enter your name: \n");
            }
        }

        /* ── Data dari client yang ada ───────────────────────────────────── */
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd != -1 && FD_ISSET(clients[i].fd, &read_fds))
                client_recv(&clients[i]);
        }
    }

    return 0;
}

void source(){
    "https://claude.ai/share/efc50853-4a48-4e05-afb1-dea012e25e58";
}