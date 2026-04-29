/*
 * eternal.c — Client Dunia Eterion
 *
 * Fitur:
 *  - Cek orion aktif via CMD_PING
 *  - Register / Login (persistent, no double login)
 *  - Main menu: Battle, Armory, History, Logout
 *  - Matchmaking 35 detik → bot jika tidak ketemu
 *  - Battle realtime asinkron: 'a' attack, 'u' ultimate
 *  - Armory: beli senjata
 *  - Match history
 *
 * Compile: gcc -Wall -pthread eternal.c -o eternal -lrt
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <termios.h>
#include <time.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/time.h>
#include "arena.h"

/* ── IPC handles ───────────────────────────────────────────────────────────── */
static int          shm_id  = -1;
static int          msgq_id = -1;
static SharedArena *arena   = NULL;

/* ── Session state ─────────────────────────────────────────────────────────── */
static int   my_slot   = -1;   /* indeks di SHM */
static pid_t my_pid;

/* ── Warna terminal ────────────────────────────────────────────────────────── */
#define RESET   "\033[0m"
#define BOLD    "\033[1m"
#define RED     "\033[31m"
#define GREEN   "\033[32m"
#define YELLOW  "\033[33m"
#define CYAN    "\033[36m"
#define MAGENTA "\033[35m"
#define WHITE   "\033[37m"

/* ── ASCII art banner ──────────────────────────────────────────────────────── */
static void print_banner(void) {
    printf(YELLOW
    "   _/_/_/      _/_/    _/_/_/_/_/ _/_/_/_/_/  _/       _/_/_/_/    _/_/    _/_/_/_/\n"
    "  _/    _/  _/    _/     _/         _/      _/        _/        _/    _/ _/\n"
    " _/_/_/    _/_/_/_/     _/         _/      _/        _/_/_/    _/    _/ _/_/_/\n"
    "_/    _/  _/    _/     _/         _/      _/        _/        _/    _/ _/\n"
    "_/_/_/   _/    _/     _/         _/      _/_/_/_/ _/_/_/_/    _/_/   _/\n"
    RESET CYAN
    "    _/_/_/_/  _/_/_/_/_/  _/_/_/_/  _/_/_/    _/_/_/    _/_/    _/      _/\n"
    "   _/            _/      _/        _/    _/    _/      _/    _/  _/_/    _/\n"
    "  _/_/_/        _/      _/_/_/    _/_/_/      _/      _/    _/  _/  _/  _/\n"
    " _/            _/      _/        _/    _/    _/      _/    _/  _/    _/_/\n"
    "_/_/_/_/      _/      _/_/_/_/  _/    _/  _/_/_/    _/_/    _/      _/\n"
    RESET "\n");
}

/* ── Kirim message ke server ───────────────────────────────────────────────── */
static void send_msg(CmdType cmd, const char *arg1, const char *arg2, int iarg) {
    Message m;
    memset(&m, 0, sizeof(m));
    m.mtype       = MTYPE_TO_SERVER;
    m.cmd         = cmd;
    m.client_pid  = my_pid;
    m.player_slot = my_slot;
    if (arg1) strncpy(m.arg1, arg1, NAME_LEN - 1);
    if (arg2) strncpy(m.arg2, arg2, PASS_LEN - 1);
    m.iarg = iarg;
    msgsnd(msgq_id, &m, sizeof(m) - sizeof(long), 0);
}

/* ── Terima response dari server ───────────────────────────────────────────── */
static int recv_resp(Response *r) {
    long mtype = 100L + (long)my_pid;
    if (msgrcv(msgq_id, r, sizeof(*r) - sizeof(long), mtype, 0) < 0)
        return -1;
    return r->status;
}

/* ── Cek orion aktif ───────────────────────────────────────────────────────── */
static int ping_server(void) {
    send_msg(CMD_PING, NULL, NULL, 0);
    Response r;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 2;
    /* Gunakan blocking recv dengan timeout via alarm */
    alarm(2);
    int ret = recv_resp(&r);
    alarm(0);
    return (ret == 0);
}

/* ── Input tanpa echo (untuk password) ────────────────────────────────────── */
static void input_noecho(char *buf, int len) {
    struct termios old, noecho;
    tcgetattr(STDIN_FILENO, &old);
    noecho = old;
    noecho.c_lflag &= ~ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &noecho);
    if (fgets(buf, len, stdin)) {
        buf[strcspn(buf, "\n")] = '\0';
    }
    tcsetattr(STDIN_FILENO, TCSANOW, &old);
    printf("\n");
}

/* ── Input biasa ───────────────────────────────────────────────────────────── */
static void input_line(const char *prompt, char *buf, int len) {
    printf("%s", prompt);
    fflush(stdout);
    if (fgets(buf, len, stdin))
        buf[strcspn(buf, "\n")] = '\0';
}

/* ── REGISTER ──────────────────────────────────────────────────────────────── */
static void do_register(void) {
    printf("\n" CYAN "CREATE ACCOUNT\n" RESET);
    char user[NAME_LEN], pass[PASS_LEN];
    input_line("Username: ", user, sizeof(user));
    printf("Password: ");
    input_noecho(pass, sizeof(pass));

    send_msg(CMD_REGISTER, user, pass, 0);
    Response r;
    recv_resp(&r);
    if (r.status == 0)
        printf(GREEN " %s\n" RESET, r.msg);
    else
        printf(RED " Error: %s\n" RESET, r.msg);
}

/* ── LOGIN ─────────────────────────────────────────────────────────────────── */
static int do_login(void) {
    printf("\n" CYAN "LOGIN\n" RESET);
    char user[NAME_LEN], pass[PASS_LEN];
    input_line("Username: ", user, sizeof(user));
    printf("Password: ");
    input_noecho(pass, sizeof(pass));

    send_msg(CMD_LOGIN, user, pass, 0);
    Response r;
    recv_resp(&r);
    if (r.status == 0) {
        my_slot = r.idata;
        printf(GREEN " %s\n" RESET, r.msg);
        return 1;
    } else {
        printf(RED " Error: %s\n" RESET, r.msg);
        return 0;
    }
}

/* ── Tampilkan profil ──────────────────────────────────────────────────────── */
static void print_profile(void) {
    if (my_slot < 0) return;
    Player *p = &arena->players[my_slot];
    char wpn[32];
    if (p->weapon_idx >= 0)
        strncpy(wpn, WEAPONS[p->weapon_idx].name, sizeof(wpn)-1);
    else
        strncpy(wpn, "None", sizeof(wpn)-1);

    printf("\n");
    printf(CYAN "  ┌────────────── PROFILE ──────────────┐\n" RESET);
    printf(CYAN "  │" RESET " Name : " GREEN "%-15s" RESET "    Lvl : " YELLOW "%d" RESET "\n",
           p->username, p->lvl);
    printf(CYAN "  │" RESET " Gold : " YELLOW "%-15d" RESET "    XP  : " WHITE "%d" RESET "\n",
           p->gold, p->xp);
    printf(CYAN "  │" RESET " Weapon: " MAGENTA "%-14s" RESET "    DMG : " RED "%d" RESET "\n",
           wpn, player_damage(p));
    printf(CYAN "  └─────────────────────────────────────┘\n" RESET);
    printf("\n");
}

/* ── Main menu ─────────────────────────────────────────────────────────────── */
static void print_main_menu(void) {
    printf(WHITE "  ┌────────────────────┐\n" RESET);
    printf(WHITE "  │" RESET " 1. Battle           " WHITE "│\n" RESET);
    printf(WHITE "  │" RESET " 2. Armory           " WHITE "│\n" RESET);
    printf(WHITE "  │" RESET " 3. History          " WHITE "│\n" RESET);
    printf(WHITE "  │" RESET " 4. Logout           " WHITE "│\n" RESET);
    printf(WHITE "  └────────────────────┘\n" RESET);
    printf("\n> Choice: ");
    fflush(stdout);
}

/* ── ARMORY ────────────────────────────────────────────────────────────────── */
static void do_armory(void) {
    Player *p = &arena->players[my_slot];
    printf("\n" YELLOW "═══ ARMORY ═══\n" RESET);
    printf("Gold: " YELLOW "%d\n" RESET, p->gold);
    for (int i = 0; i < MAX_WEAPONS; i++) {
        printf("%d. %-12s | %4d G | +%3d Dmg\n",
               i+1, WEAPONS[i].name, WEAPONS[i].price, WEAPONS[i].bonus_dmg);
    }
    printf("0. Back | Choice: ");
    fflush(stdout);

    int choice;
    scanf("%d", &choice);
    getchar();
    if (choice == 0) return;
    if (choice < 1 || choice > MAX_WEAPONS) {
        printf(RED "Invalid choice.\n" RESET);
        return;
    }

    send_msg(CMD_BUY_WEAPON, NULL, NULL, choice - 1);
    Response r;
    recv_resp(&r);
    if (r.status == 0)
        printf(GREEN "%s\n" RESET, r.msg);
    else
        printf(RED "%s\n" RESET, r.msg);
}

/* ── HISTORY ───────────────────────────────────────────────────────────────── */
static void do_history(void) {
    send_msg(CMD_GET_HISTORY, NULL, NULL, 0);
    Response r;
    recv_resp(&r);

    Player *p = &arena->players[my_slot];
    printf("\n");
    printf(MAGENTA "  ┌──────────── MATCH HISTORY ────────────┐\n" RESET);
    printf("  │ %-8s │ %-12s │ %-4s │ %-5s │\n",
           "Time", "Opponent", "Res", "XP");
    printf("  ├──────────┼──────────────┼──────┼───────┤\n");

    int count = p->history_count;
    if (count == 0) {
        printf("  │ " WHITE "No battle history yet." RESET "              │\n");
    } else {
        /* Tampilkan dari terbaru */
        for (int i = count - 1; i >= 0 && i >= count - 10; i--) {
            HistoryEntry *h = &p->history[i];
            const char *res_color = h->result ? GREEN : RED;
            const char *res_str   = h->result ? "WIN" : "LOSS";
            printf("  │ %-8s │ %-12s │ %s%-4s%s │ +%-4d │\n",
                   h->time_str, h->opponent,
                   res_color, res_str, RESET,
                   h->xp_gained);
        }
    }
    printf(MAGENTA "  └──────────────────────────────────────┘\n" RESET);
    printf("\nPress any key...");
    fflush(stdout);
    getchar();
}

/* ══════════════════════════════════════════════════════════════════════════════
 * BATTLE SYSTEM
 * ══════════════════════════════════════════════════════════════════════════════ */

/* State battle lokal */
static volatile int  battle_bslot = -1;
static volatile int  battle_done  = 0;
static volatile int  battle_result = -1; /* 1=WIN, 0=LOSS */
static volatile int  atk_on_cd    = 0;
static volatile int  ult_on_cd    = 0;
static pthread_mutex_t battle_mtx = PTHREAD_MUTEX_INITIALIZER;

/* ── Nonblocking keyboard ──────────────────────────────────────────────────── */
static struct termios orig_termios;

static void enable_raw_mode(void) {
    tcgetattr(STDIN_FILENO, &orig_termios);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 1; /* 100ms timeout */
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

static void disable_raw_mode(void) {
    tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
}

/* ── Waktu ms ──────────────────────────────────────────────────────────────── */
static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* ── Render arena ──────────────────────────────────────────────────────────── */
static void render_arena(int bslot, int my_idx) {
    BattleSession *bs  = &arena->battles[bslot];
    int opp_idx = 1 - my_idx;

    int p0_slot = bs->player_slot[my_idx];
    int p1_slot = bs->player_slot[opp_idx];

    Player *me  = &arena->players[p0_slot];
    int     my_max_hp = player_health(me);

    char opp_name[NAME_LEN];
    int  opp_max_hp;

    if (p1_slot == -2) {
        /* Bot */
        strncpy(opp_name, "Wild Beast", NAME_LEN-1);
        opp_max_hp = 120;
    } else if (p1_slot >= 0) {
        Player *opp = &arena->players[p1_slot];
        strncpy(opp_name, opp->username, NAME_LEN-1);
        opp_max_hp = player_health(opp);
        (void)opp->weapon_idx;
    } else {
        return;
    }

    int my_hp  = bs->hp[my_idx];
    int opp_hp = bs->hp[opp_idx];

    /* Clear screen */
    printf("\033[H\033[J");

    /* Banner */
    print_banner();

    /* Box arena */
    printf(MAGENTA "  ┌──────────────────── ARENA ───────────────────────┐\n" RESET);

    /* Lawan */
    printf("  │  " CYAN "%-15s" RESET " Lvl %d\n", opp_name, 
           (p1_slot == -2) ? 1 : arena->players[p1_slot].lvl);

    /* HP bar lawan */
    int opp_bar = (opp_hp * 20) / (opp_max_hp > 0 ? opp_max_hp : 1);
    if (opp_bar < 0) opp_bar = 0;
    printf("  │  [" RED);
    for (int i = 0; i < 20; i++) printf(i < opp_bar ? "█" : " ");
    printf(RESET "] %d/%d\n", opp_hp < 0 ? 0 : opp_hp, opp_max_hp);

    printf("  │\n");
    printf("  │  " MAGENTA "           VS\n" RESET);
    printf("  │\n");

    /* Saya */
    char wpn_str[32];
    snprintf(wpn_str, sizeof(wpn_str), "%s",
             me->weapon_idx >= 0 ? WEAPONS[me->weapon_idx].name : "None");
    printf("  │  " GREEN "%-15s" RESET " Lvl %d | Weapon: " YELLOW "%s\n" RESET,
           me->username, me->lvl, wpn_str);

    int my_bar = (my_hp * 20) / (my_max_hp > 0 ? my_max_hp : 1);
    if (my_bar < 0) my_bar = 0;
    printf("  │  [" GREEN);
    for (int i = 0; i < 20; i++) printf(i < my_bar ? "█" : " ");
    printf(RESET "] %d/%d\n", my_hp < 0 ? 0 : my_hp, my_max_hp);

    printf("  │\n");

    /* Combat Log */
    printf("  │  " YELLOW "Combat Log:\n" RESET);
    int head = bs->log_head;
    for (int i = 0; i < COMBAT_LOG_LINES; i++) {
        int idx = (head - COMBAT_LOG_LINES + i + 1000) % COMBAT_LOG_LINES;
        printf("  │  %s\n", bs->log[idx]);
    }

    printf("  │\n");

    /* Cooldown indicator */
    long now = now_ms();
    double atk_cd = (double)(ATTACK_COOLDOWN_MS - (now - bs->atk_cooldown[my_idx])) / 1000.0;
    double ult_cd = (double)(ATTACK_COOLDOWN_MS - (now - bs->ult_cooldown[my_idx])) / 1000.0;
    if (atk_cd < 0) atk_cd = 0;
    if (ult_cd < 0) ult_cd = 0;
    printf("  │  " CYAN "CD: Atk(%.1fs) | Ult(%.1fs)\n" RESET, atk_cd, ult_cd);
    printf("  │\n");

    /* Controls */
    printf("  │  " WHITE "[a] Attack   [u] Ultimate\n" RESET);
    printf(MAGENTA "  └───────────────────────────────────────────────────┘\n" RESET);
    fflush(stdout);
}

/* ── Thread: render loop ───────────────────────────────────────────────────── */
typedef struct { int bslot; int my_idx; } RenderArgs;

static void *render_thread(void *arg) {
    RenderArgs *ra = (RenderArgs *)arg;
    while (!battle_done) {
        pthread_mutex_lock(&battle_mtx);
        render_arena(ra->bslot, ra->my_idx);
        pthread_mutex_unlock(&battle_mtx);
        usleep(100000); /* refresh 10x/detik */
    }
    return NULL;
}

/* ── Battle utama ──────────────────────────────────────────────────────────── */
static void run_battle(int bslot, int my_idx) {
    battle_done   = 0;
    battle_result = -1;

    enable_raw_mode();

    RenderArgs ra = { bslot, my_idx };
    pthread_t t_render;
    pthread_create(&t_render, NULL, render_thread, &ra);

    long last_atk = 0, last_ult = 0;

    while (!battle_done) {
        /* Cek kondisi battle dari SHM */
        BattleSession *bs = &arena->battles[bslot];
        if (bs->done) {
            battle_done   = 1;
            battle_result = bs->result[my_idx];
            break;
        }

        /* Baca keypress non-blocking */
        char c = 0;
        read(STDIN_FILENO, &c, 1);

        long now = now_ms();

        if (c == 'a' || c == 'A') {
            if (now - last_atk >= ATTACK_COOLDOWN_MS) {
                send_msg(CMD_ATTACK, NULL, NULL, bslot);
                last_atk = now;
            }
        } else if (c == 'u' || c == 'U') {
            if (arena->players[my_slot].weapon_idx >= 0 &&
                now - last_ult >= ATTACK_COOLDOWN_MS) {
                send_msg(CMD_ULTIMATE, NULL, NULL, bslot);
                last_ult = now;
            }
        }

        usleep(50000); /* 50ms polling */
    }

    pthread_join(t_render, NULL);
    disable_raw_mode();

    /* Render sekali lagi untuk tampilkan hasil akhir */
    render_arena(bslot, my_idx);

    /* Tampilkan hasil */
    printf("\n");
    if (battle_result == 1) {
        printf(GREEN "  ══ VICTORY ══\n" RESET);
    } else {
        printf(RED "  ══ DEFEAT ══\n" RESET);
    }
    printf("\n  Battle ended. Press [ENTER] to continue...");
    fflush(stdout);

    /* Tunggu ENTER */
    disable_raw_mode();
    getchar();
}

/* ── MATCHMAKING ───────────────────────────────────────────────────────────── */
static void do_battle(void) {
    printf("\033[H\033[J");
    print_banner();
    printf(MAGENTA "Searching for an opponent..." RESET);
    fflush(stdout);

    send_msg(CMD_MATCHMAKE, NULL, NULL, 0);

    /* Tunggu response: bisa "waiting" atau langsung match found */
    Response r;
    recv_resp(&r);

    int bslot  = r.idata;
    int my_idx = -1;

    if (r.status == 1) {
        /* Langsung match ditemukan */
        BattleSession *bs = &arena->battles[bslot];
        my_idx = (bs->player_slot[0] == my_slot) ? 0 : 1;
        printf("\n" GREEN "Opponent found! Starting battle...\n" RESET);
        sleep(1);
        run_battle(bslot, my_idx);
        return;
    }

    /* Status 0: waiting — tunggu hingga 35 detik */
    long start = now_ms();
    int  found = 0;

    while (now_ms() - start < MATCHMAKING_TIMEOUT * 1000L) {
        /* Cek SHM apakah battle slot sudah punya lawan */
        if (bslot >= 0) {
            BattleSession *bs = &arena->battles[bslot];
            if (bs->active && bs->player_slot[1] >= 0) {
                /* Lawan ditemukan */
                my_idx = (bs->player_slot[0] == my_slot) ? 0 : 1;
                found  = 1;
                break;
            }
        }

        long elapsed = (now_ms() - start) / 1000;
        printf("\r" MAGENTA "Searching for an opponent... [%ld s]" RESET "  ", elapsed);
        fflush(stdout);
        sleep(1);
    }

    if (!found) {
        /* Timeout: lawan bot Wild Beast */
        printf("\n" YELLOW "No opponent found. Challenging Wild Beast!\n" RESET);
        sleep(1);

        if (bslot >= 0) {
            BattleSession *bs = &arena->battles[bslot];
            bs->player_slot[0]  = my_slot;
            bs->player_slot[1]  = -2;
            bs->hp[0]           = player_health(&arena->players[my_slot]);
            bs->hp[1]           = 120;
            bs->result[0]       = -1;
            bs->result[1]       = -1;
            bs->atk_cooldown[0] = 0;
            bs->atk_cooldown[1] = 0;
            bs->ult_cooldown[0] = 0;
            bs->ult_cooldown[1] = 0;
            bs->log_head        = 0;
            bs->done            = 0;
            bs->active          = 1;
            for (int l = 0; l < COMBAT_LOG_LINES; l++)
                strcpy(bs->log[l], ">");
            arena->players[my_slot].in_battle = 1;
            my_idx = 0;
            run_battle(bslot, my_idx);
            return;
        }
    }
}

/* ── LOGOUT ────────────────────────────────────────────────────────────────── */
static void do_logout(void) {
    send_msg(CMD_LOGOUT, NULL, NULL, 0);
    Response r;
    recv_resp(&r);
    printf(YELLOW "Logging out...\n" RESET);
    my_slot = -1;
}

/* ── Cleanup ───────────────────────────────────────────────────────────────── */
static void cleanup(void) {
    if (my_slot >= 0) {
        send_msg(CMD_LOGOUT, NULL, NULL, 0);
        /* Non-blocking logout */
        struct timespec ts = {0, 500000000L};
        nanosleep(&ts, NULL);
    }
    if (arena  != (void *)-1 && arena  != NULL) shmdt(arena);
    disable_raw_mode();
}

static void handle_sigint(int sig) {
    (void)sig;
    cleanup();
    printf("\n");
    exit(0);
}

/* ── Main ─────────────────────────────────────────────────────────────────── */
int main(void) {
    my_pid = getpid();
    signal(SIGINT, handle_sigint);
    signal(SIGALRM, SIG_IGN);

    /* Attach ke IPC */
    shm_id = shmget(SHM_KEY, sizeof(SharedArena), 0666);
    if (shm_id < 0) {
        printf(RED "Orion are you there?\n" RESET);
        exit(1);
    }
    arena = (SharedArena *)shmat(shm_id, NULL, 0);
    if (arena == (void *)-1) {
        printf(RED "Orion are you there?\n" RESET);
        exit(1);
    }

    msgq_id = msgget(MSGQ_KEY, 0666);
    if (msgq_id < 0) {
        printf(RED "Orion are you there?\n" RESET);
        exit(1);
    }

    /* Ping server */
    if (!ping_server()) {
        printf(RED "Orion are you there?\n" RESET);
        exit(1);
    }

    /* ── Auth loop ──────────────────────────────────────────────────────── */
    while (my_slot < 0) {
        printf("\033[H\033[J");
        print_banner();
        printf("1. Register\n");
        printf("2. Login\n");
        printf("3. Exit\n");
        printf("Choice: ");
        fflush(stdout);

        int c;
        scanf("%d", &c);
        getchar();

        if (c == 1) {
            do_register();
        } else if (c == 2) {
            if (do_login()) {
                /* Login sukses */
            }
        } else if (c == 3) {
            cleanup();
            exit(0);
        }
    }

    /* ── Main menu loop ─────────────────────────────────────────────────── */
    while (my_slot >= 0) {
        printf("\033[H\033[J");
        print_banner();
        print_profile();
        print_main_menu();

        int c;
        scanf("%d", &c);
        getchar();

        switch (c) {
            case 1: do_battle();  break;
            case 2: do_armory();  break;
            case 3: do_history(); break;
            case 4: do_logout();  break;
            default:
                printf(RED "Invalid choice.\n" RESET);
                sleep(1);
        }
    }

    cleanup();
    return 0;
}