/*
 * navi.c — NAVI Client (The Wired)
 *
 * Konsep dari README:
 *  - Thread (pthread)    : Dua fungsi asinkron TANPA fork
 *      • thread_recv     : Terus mendengarkan transmisi dari server (listener)
 *      • main thread     : Membaca input user dan mengirimnya ke server
 *  - Socket Programming  : socket(), connect(), send(), recv()
 *  - Join Thread         : pthread_join() — main menunggu receiver selesai
 *  - Mutual Exclusion    : pthread_mutex untuk flag stop
 *
 * Compile: gcc -pthread -o navi navi.c
 * Run   : ./navi
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ── Konstanta ─────────────────────────────────────────────────────────────── */
#define BUF_SIZE  2048
#define NAME_SIZE 128

/* ── Globals ───────────────────────────────────────────────────────────────── */
static int            sock_fd  = -1;
static volatile int   stop     = 0;
static pthread_mutex_t stop_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Config dari file 'protocol' */
static char CFG_HOST[64] = "127.0.0.1";
static int  CFG_PORT     = 9999;

/* ── Baca protocol ─────────────────────────────────────────────────────────── */
static void load_protocol(void) {
    FILE *f = fopen("protocol", "r");
    if (!f) { perror("fopen protocol"); exit(1); }
    char admin[NAME_SIZE], pass[NAME_SIZE];
    fscanf(f, "%63s\n%d\n%127[^\n]\n%127[^\n]",
           CFG_HOST, &CFG_PORT, admin, pass);
    fclose(f);
}

/* ── Set/get flag stop dengan mutex ────────────────────────────────────────── */
static void set_stop(void) {
    pthread_mutex_lock(&stop_mutex);
    stop = 1;
    pthread_mutex_unlock(&stop_mutex);
}

static int get_stop(void) {
    pthread_mutex_lock(&stop_mutex);
    int v = stop;
    pthread_mutex_unlock(&stop_mutex);
    return v;
}

/*
 * ── Thread Receiver ─────────────────────────────────────────────────────────
 * Fungsi thread yang hanya mendengarkan data dari server dan mencetaknya.
 * Implementasi konsep Thread (pthread_create) dari README.
 */
static void *thread_recv(void *arg) {
    (void)arg;
    char buf[BUF_SIZE];
    char partial[BUF_SIZE * 2];
    int  partial_len = 0;

    while (!get_stop()) {
        int n = recv(sock_fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) {
            if (!get_stop())
                printf("\n[System] Connection to The Wired lost.\n");
            set_stop();
            break;
        }

        /* Kumpulkan sampai ada newline, lalu cetak */
        int space = (int)sizeof(partial) - partial_len - 1;
        if (n > space) n = space;
        memcpy(partial + partial_len, buf, n);
        partial_len += n;
        partial[partial_len] = '\0';

        char *start = partial;
        char *nl;
        while ((nl = strchr(start, '\n')) != NULL) {
            *nl = '\0';
            /* Hapus \r jika ada */
            int len = strlen(start);
            if (len > 0 && start[len-1] == '\r') start[len-1] = '\0';
            if (strlen(start) > 0)
                printf("%s\n", start);
            fflush(stdout);
            start = nl + 1;
        }
        /* Sisa belum lengkap */
        int remaining = partial_len - (int)(start - partial);
        memmove(partial, start, remaining);
        partial_len = remaining;
        partial[partial_len] = '\0';
    }

    return NULL;
}

/* ── Handle Ctrl+C di client ───────────────────────────────────────────────── */
static void handle_sigint(int sig) {
    (void)sig;
    set_stop();
    if (sock_fd != -1) {
        send(sock_fd, "/exit\n", 6, 0);
        close(sock_fd);
        sock_fd = -1;
    }
    printf("\n");
    exit(0);
}

/* ── Main ──────────────────────────────────────────────────────────────────── */
int main(void) {
    load_protocol();
    signal(SIGINT, handle_sigint);

    /* Buat socket dan connect ke server */
    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) { perror("socket"); exit(1); }

    struct sockaddr_in serv;
    memset(&serv, 0, sizeof(serv));
    serv.sin_family = AF_INET;
    serv.sin_port   = htons(CFG_PORT);
    if (inet_pton(AF_INET, CFG_HOST, &serv.sin_addr) <= 0) {
        fprintf(stderr, "[Error] Alamat tidak valid: %s\n", CFG_HOST);
        exit(1);
    }
    if (connect(sock_fd, (struct sockaddr *)&serv, sizeof(serv)) < 0) {
        fprintf(stderr, "[Error] Tidak bisa terhubung ke The Wired (%s:%d)\n",
                CFG_HOST, CFG_PORT);
        exit(1);
    }

    /*
     * Buat thread receiver — konsep pthread_create dari README.
     * Thread ini berjalan paralel dengan main thread (input loop),
     * tanpa menggunakan fork().
     */
    pthread_t tid;
    if (pthread_create(&tid, NULL, thread_recv, NULL) != 0) {
        perror("pthread_create");
        exit(1);
    }

    /*
     * Main thread: baca input user dan kirim ke server.
     * Dua fungsi berjalan ASINKRON:
     *   - thread_recv : menerima & cetak pesan dari server
     *   - main thread : kirim input user ke server
     */
    char line[BUF_SIZE];
    while (!get_stop()) {
        if (fgets(line, sizeof(line), stdin) == NULL) break;
        if (get_stop()) break;

        /* Kirim ke server (sudah ada \n dari fgets) */
        int n = send(sock_fd, line, strlen(line), 0);
        if (n < 0) {
            fprintf(stderr, "[System] Gagal mengirim pesan.\n");
            break;
        }

        /* Trim newline untuk cek /exit */
        line[strcspn(line, "\n")] = '\0';
        if (strcmp(line, "/exit") == 0) {
            set_stop();
            break;
        }
    }

    set_stop();

    /* pthread_join — tunggu thread receiver selesai (konsep Join Thread dari README) */
    pthread_join(tid, NULL);

    if (sock_fd != -1) {
        close(sock_fd);
        sock_fd = -1;
    }
    return 0;
}

void source(){
    "https://claude.ai/share/efc50853-4a48-4e05-afb1-dea012e25e58";
}