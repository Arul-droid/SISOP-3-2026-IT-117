/*
 * orion.c — Server Dunia Eterion
 *
 * Tanggung jawab:
 *  - Inisialisasi Shared Memory, Message Queue, Semaphore
 *  - Muat/simpan data player dari/ke file (persistent)
 *  - Layani request dari eternal via Message Queue
 *  - Jalankan battle loop realtime di thread terpisah
 *  - Cegah race condition dengan Semaphore
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/time.h>
#include "arena.h"

/* ── IPC handles ───────────────────────────────────────────────────────────── */
static int shm_id  = -1;
static int msgq_id = -1;
static int sem_id  = -1;
static SharedArena *arena = NULL;

/* ── Semaphore helper ──────────────────────────────────────────────────────── */
static void sem_wait_op(void) {
    struct sembuf op = { 0, -1, 0 };
    semop(sem_id, &op, 1);
}
static void sem_signal_op(void) {
    struct sembuf op = { 0, +1, 0 };
    semop(sem_id, &op, 1);
}

/* ── Waktu saat ini dalam ms ───────────────────────────────────────────────── */
static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* ── Persistent: simpan ke file ────────────────────────────────────────────── */
static void save_players(void) {
    FILE *f = fopen(DATA_FILE, "wb");
    if (!f) return;
    sem_wait_op();
    fwrite(&arena->player_count, sizeof(int), 1, f);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (arena->players[i].username[0] != '\0')
            fwrite(&arena->players[i], sizeof(Player), 1, f);
    }
    sem_signal_op();
    fclose(f);
}

/* ── Persistent: muat dari file ────────────────────────────────────────────── */
static void load_players(void) {
    FILE *f = fopen(DATA_FILE, "rb");
    if (!f) return;
    int count;
    fread(&count, sizeof(int), 1, f);
    Player tmp;
    int loaded = 0;
    while (fread(&tmp, sizeof(Player), 1, f) == 1 && loaded < MAX_PLAYERS) {
        /* Reset runtime fields */
        tmp.logged_in  = 0;
        tmp.in_battle  = 0;
        tmp.client_pid = 0;
        /* Cari slot kosong */
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (arena->players[i].username[0] == '\0') {
                arena->players[i] = tmp;
                arena->players[i].shm_slot = i;
                loaded++;
                break;
            }
        }
    }
    arena->player_count = loaded;
    fclose(f);
}

/* ── Cari player by username ────────────────────────────────────────────────── */
static int find_player(const char *username) {
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (arena->players[i].username[0] != '\0' &&
            strcmp(arena->players[i].username, username) == 0)
            return i;
    return -1;
}

/* ── Kirim response ke client ───────────────────────────────────────────────── */
static void send_resp(int msgq, pid_t pid, int status, const char *msg, int idata) {
    Response r;
    memset(&r, 0, sizeof(r));
    r.mtype  = 100L + (long)pid;
    r.status = status;
    r.idata  = idata;
    strncpy(r.msg, msg, sizeof(r.msg) - 1);
    msgsnd(msgq, &r, sizeof(r) - sizeof(long), 0);
}

/* ── Handler: CMD_REGISTER ───────────────────────────────────────────────────── */
static void handle_register(Message *m) {
    sem_wait_op();
    if (find_player(m->arg1) >= 0) {
        sem_signal_op();
        send_resp(msgq_id, m->client_pid, -1, "Username already taken!", -1);
        return;
    }
    /* Cari slot kosong */
    int slot = -1;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (arena->players[i].username[0] == '\0') { slot = i; break; }
    }
    if (slot < 0) {
        sem_signal_op();
        send_resp(msgq_id, m->client_pid, -1, "Server full!", -1);
        return;
    }
    Player *p = &arena->players[slot];
    memset(p, 0, sizeof(Player));
    strncpy(p->username, m->arg1, NAME_LEN - 1);
    strncpy(p->password, m->arg2, PASS_LEN - 1);
    p->xp         = BASE_XP;
    p->gold       = BASE_GOLD;
    p->lvl        = BASE_LVL;
    p->weapon_idx = -1;
    p->shm_slot   = slot;
    arena->player_count++;
    sem_signal_op();
    save_players();
    send_resp(msgq_id, m->client_pid, 0, "Account created!", slot);
}

/* ── Handler: CMD_LOGIN ─────────────────────────────────────────────────────── */
static void handle_login(Message *m) {
    sem_wait_op();
    int slot = find_player(m->arg1);
    if (slot < 0) {
        sem_signal_op();
        send_resp(msgq_id, m->client_pid, -1, "Username not found!", -1);
        return;
    }
    Player *p = &arena->players[slot];
    if (strcmp(p->password, m->arg2) != 0) {
        sem_signal_op();
        send_resp(msgq_id, m->client_pid, -1, "Wrong password!", -1);
        return;
    }
    if (p->logged_in) {
        sem_signal_op();
        send_resp(msgq_id, m->client_pid, -1, "Account already logged in!", -1);
        return;
    }
    p->logged_in  = 1;
    p->client_pid = m->client_pid;
    sem_signal_op();
    send_resp(msgq_id, m->client_pid, 0, "Welcome!", slot);
}

/* ── Handler: CMD_LOGOUT ────────────────────────────────────────────────────── */
static void handle_logout(Message *m) {
    sem_wait_op();
    if (m->player_slot >= 0 && m->player_slot < MAX_PLAYERS) {
        Player *p = &arena->players[m->player_slot];
        p->logged_in  = 0;
        p->in_battle  = 0;
        p->client_pid = 0;
    }
    sem_signal_op();
    save_players();
    send_resp(msgq_id, m->client_pid, 0, "Logged out.", -1);
}

/* ── Handler: CMD_BUY_WEAPON ────────────────────────────────────────────────── */
static void handle_buy_weapon(Message *m) {
    sem_wait_op();
    Player *p = &arena->players[m->player_slot];
    int idx = m->iarg;
    if (idx < 0 || idx >= MAX_WEAPONS) {
        sem_signal_op();
        send_resp(msgq_id, m->client_pid, -1, "Invalid weapon!", -1);
        return;
    }
    if (p->gold < WEAPONS[idx].price) {
        sem_signal_op();
        send_resp(msgq_id, m->client_pid, -1, "Not enough gold!", -1);
        return;
    }
    /* Pakai senjata dengan damage terbesar */
    if (p->weapon_idx < 0 || WEAPONS[idx].bonus_dmg > WEAPONS[p->weapon_idx].bonus_dmg) {
        p->gold -= WEAPONS[idx].price;
        p->weapon_idx = idx;
        sem_signal_op();
        save_players();
        char msg[64];
        snprintf(msg, sizeof(msg), "Bought %s!", WEAPONS[idx].name);
        send_resp(msgq_id, m->client_pid, 0, msg, -1);
    } else {
        sem_signal_op();
        send_resp(msgq_id, m->client_pid, -1, "You already have a better weapon!", -1);
    }
}

/* ── Handler: CMD_GET_HISTORY ────────────────────────────────────────────────── */
static void handle_get_history(Message *m) {
    /* Kirim history count dulu, lalu client baca dari SHM langsung */
    sem_wait_op();
    int count = arena->players[m->player_slot].history_count;
    sem_signal_op();
    send_resp(msgq_id, m->client_pid, 0, "ok", count);
}

/* ── Handler: CMD_PING ───────────────────────────────────────────────────────── */
static void handle_ping(Message *m) {
    send_resp(msgq_id, m->client_pid, 0, "pong", -1);
}

/* ── Handler: CMD_MATCHMAKE ──────────────────────────────────────────────────── */
static void handle_matchmake(Message *m) {
    sem_wait_op();
    Player *me = &arena->players[m->player_slot];

    /* Cari player lain yang juga waiting matchmake (tidak in_battle) */
    int opponent = -1;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (i == m->player_slot) continue;
        Player *p = &arena->players[i];
        if (p->username[0] == '\0') continue;
        if (!p->logged_in) continue;
        if (p->in_battle) continue;
        /* Cek apakah sedang waiting match (ada flag matchmaking) */
        /* Gunakan field client_pid sebagai proxy: jika matchmaking_start != 0 */
        /* Kita gunakan battle slot "waiting" */
        /* Cari battle slot di mana player ini sudah BATTLE_WAITING */
        int found_waiting = 0;
        for (int b = 0; b < MAX_PLAYERS / 2; b++) {
            BattleSession *bs = &arena->battles[b];
            if (bs->active && bs->done == 0 &&
                bs->result[0] == -1 && bs->result[1] == -1 &&
                bs->player_slot[1] == -1 &&
                bs->player_slot[0] == i) {
                /* i sedang menunggu, pasangkan */
                bs->player_slot[1] = m->player_slot;
                bs->hp[0] = player_health(&arena->players[i]);
                bs->hp[1] = player_health(me);
                me->in_battle = 1;
                arena->players[i].in_battle = 1;
                opponent = b;  /* pakai b sebagai battle slot idx */
                found_waiting = 1;
                break;
            }
        }
        if (found_waiting) { opponent = i; break; }
    }

    if (opponent >= 0) {
        /* Pasangkan: cari battle slot yang baru dibuat */
        int bslot = -1;
        for (int b = 0; b < MAX_PLAYERS / 2; b++) {
            if (arena->battles[b].active &&
                (arena->battles[b].player_slot[0] == opponent ||
                 arena->battles[b].player_slot[1] == m->player_slot)) {
                bslot = b;
                break;
            }
        }
        sem_signal_op();
        char msg[64];
        snprintf(msg, sizeof(msg), "%d", bslot);
        send_resp(msgq_id, m->client_pid, 1, msg, bslot); /* status=1: match found */
        /* Beritahu player lain juga */
        send_resp(msgq_id, arena->players[opponent].client_pid, 1, msg, bslot);
    } else {
        /* Buat battle slot baru (waiting) */
        int bslot = -1;
        for (int b = 0; b < MAX_PLAYERS / 2; b++) {
            if (!arena->battles[b].active) { bslot = b; break; }
        }
        if (bslot < 0) {
            sem_signal_op();
            send_resp(msgq_id, m->client_pid, -1, "No battle slot available!", -1);
            return;
        }
        BattleSession *bs = &arena->battles[bslot];
        memset(bs, 0, sizeof(BattleSession));
        bs->active          = 1;
        bs->done            = 0;
        bs->player_slot[0]  = m->player_slot;
        bs->player_slot[1]  = -1;  /* belum ada lawan */
        bs->hp[0]           = player_health(me);
        bs->hp[1]           = 0;
        bs->result[0]       = -1;
        bs->result[1]       = -1;
        bs->log_head        = 0;
        for (int l = 0; l < COMBAT_LOG_LINES; l++)
            strcpy(bs->log[l], ">");
        sem_signal_op();
        /* Kirim "waiting" ke client */
        send_resp(msgq_id, m->client_pid, 0, "waiting", bslot);
    }
}

/* ── Handler: CMD_CANCEL_MATCH ───────────────────────────────────────────────── */
static void handle_cancel_match(Message *m) {
    sem_wait_op();
    /* Hapus battle slot yang masih waiting milik player ini */
    for (int b = 0; b < MAX_PLAYERS / 2; b++) {
        BattleSession *bs = &arena->battles[b];
        if (bs->active && bs->player_slot[1] == -1 &&
            bs->player_slot[0] == m->player_slot) {
            bs->active = 0;
            break;
        }
    }
    if (m->player_slot >= 0)
        arena->players[m->player_slot].in_battle = 0;
    sem_signal_op();
    send_resp(msgq_id, m->client_pid, 0, "cancelled", -1);
}

/* ── Handler: CMD_ATTACK / CMD_ULTIMATE ──────────────────────────────────────── */
static void handle_attack(Message *m, int is_ult) {
    sem_wait_op();
    int bslot = m->iarg;
    if (bslot < 0 || bslot >= MAX_PLAYERS / 2 || !arena->battles[bslot].active) {
        sem_signal_op();
        return;
    }
    BattleSession *bs = &arena->battles[bslot];
    if (bs->done) { sem_signal_op(); return; }

    /* Cari siapa saya (0 atau 1) */
    int me = -1, opp = -1;
    for (int k = 0; k < 2; k++) {
        if (bs->player_slot[k] == m->player_slot) { me = k; opp = 1 - k; break; }
    }
    if (me < 0) { sem_signal_op(); return; }

    /* Cek apakah lawan masih ada (matchmaking belum selesai) */
    if (bs->player_slot[opp] < 0) { sem_signal_op(); return; }

    long now = now_ms();
    long *cd = is_ult ? &bs->ult_cooldown[me] : &bs->atk_cooldown[me];
    if (now - *cd < ATTACK_COOLDOWN_MS) { sem_signal_op(); return; }
    *cd = now;

    Player *attacker = &arena->players[m->player_slot];
    int dmg;
    if (is_ult) {
        if (attacker->weapon_idx < 0) { sem_signal_op(); return; }
        dmg = player_ultimate(attacker);
    } else {
        dmg = player_damage(attacker);
    }

    bs->hp[opp] -= dmg;
    if (bs->hp[opp] < 0) bs->hp[opp] = 0;

    /* Update combat log */
    char logline[128];
    snprintf(logline, sizeof(logline), "> %s hit for %d damage!",
             attacker->username, dmg);
    int head = bs->log_head % COMBAT_LOG_LINES;
    strncpy(bs->log[head], logline, sizeof(bs->log[0]) - 1);
    bs->log_head++;

    /* Cek apakah ada yang mati */
    if (bs->hp[opp] <= 0) {
        bs->result[me]  = 1; /* menang */
        bs->result[opp] = 0; /* kalah */
        bs->done = 1;
    }
    sem_signal_op();
}

/* ── Handler: CMD_GET_BATTLE_STATE ───────────────────────────────────────────── */
static void handle_get_battle_state(Message *m) {
    /* Client membaca SHM langsung; kita cukup ack */
    send_resp(msgq_id, m->client_pid, 0, "ok", 0);
}

/* ── Battle finalize (dipanggil setelah battle done) ────────────────────────── */
static void finalize_battle(int bslot) {
    BattleSession *bs = &arena->battles[bslot];
    if (!bs->done) return;

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char ts[16];
    strftime(ts, sizeof(ts), "%H:%M", t);

    for (int k = 0; k < 2; k++) {
        if (bs->player_slot[k] < 0) continue;
        Player *p  = &arena->players[bs->player_slot[k]];
        int    win = bs->result[k];
        int    xp_gain   = win ? 50 : 15;
        int    gold_gain = win ? 120 : 30;

        p->xp   += xp_gain;
        p->gold += gold_gain;
        /* Level up: setiap kelipatan 100 XP */
        p->lvl = 1 + (p->xp / 100);

        /* Simpan history */
        if (p->history_count < MAX_HISTORY) {
            HistoryEntry *h = &p->history[p->history_count++];
            strncpy(h->time_str, ts, sizeof(h->time_str) - 1);
            int opp_slot = bs->player_slot[1 - k];
            if (opp_slot >= 0)
                strncpy(h->opponent, arena->players[opp_slot].username, NAME_LEN - 1);
            else
                strncpy(h->opponent, "Wild Beast", NAME_LEN - 1);
            h->result    = win;
            h->xp_gained = xp_gain;
        }

        p->in_battle  = 0;
    }
    bs->active = 0;
    save_players();
}

/* ── Thread: battle monitor (finalize selesai + bot logic) ──────────────────── */
static void *battle_thread(void *arg) {
    (void)arg;
    while (1) {
        usleep(100000); /* 100ms */
        sem_wait_op();
        for (int b = 0; b < MAX_PLAYERS / 2; b++) {
            BattleSession *bs = &arena->battles[b];
            if (!bs->active) continue;

            /* Bot logic: jika player[1] == -1 (belum ada lawan) → tidak ada bot */
            /* Bot baru ditambahkan setelah timeout (handled di eternal) */

            /* Finalize jika done */
            if (bs->done && bs->result[0] != -2) {
                bs->result[0] = (bs->result[0] == -2) ? bs->result[0] : bs->result[0];
                /* Tandai sedang di-finalize */
                finalize_battle(b);
            }
        }
        sem_signal_op();
    }
    return NULL;
}

/* ── Thread: bot attacker ────────────────────────────────────────────────────── */
/* Bot disimulasikan: tiap ~1.5 detik bot serang player */
static void *bot_thread(void *arg) {
    (void)arg;
    while (1) {
        usleep(800000); /* 800ms */
        sem_wait_op();
        for (int b = 0; b < MAX_PLAYERS / 2; b++) {
            BattleSession *bs = &arena->battles[b];
            if (!bs->active || bs->done) continue;
            /* Bot battle: player_slot[1] == -2 menandakan bot */
            if (bs->player_slot[1] != -2) continue;

            long now = now_ms();
            if (now - bs->atk_cooldown[1] < 1200) continue; /* bot CD 1.2s */
            bs->atk_cooldown[1] = now;

            /* Bot damage: antara 8-14 */
            int dmg = 8 + (rand() % 7);
            bs->hp[0] -= dmg;
            if (bs->hp[0] < 0) bs->hp[0] = 0;

            char logline[128];
            snprintf(logline, sizeof(logline), "> Wild Beast hit for %d damage!", dmg);
            int head = bs->log_head % COMBAT_LOG_LINES;
            strncpy(bs->log[head], logline, sizeof(bs->log[0]) - 1);
            bs->log_head++;

            if (bs->hp[0] <= 0) {
                bs->result[0] = 0; /* player kalah */
                bs->result[1] = 1;
                bs->done = 1;
            }
        }
        sem_signal_op();
    }
    return NULL;
}

/* ── Cleanup IPC ────────────────────────────────────────────────────────────── */
static void cleanup(void) {
    save_players();
    if (arena)   shmdt(arena);
    if (shm_id  >= 0) shmctl(shm_id,  IPC_RMID, NULL);
    if (msgq_id >= 0) msgctl(msgq_id, IPC_RMID, NULL);
    if (sem_id  >= 0) semctl(sem_id,  0, IPC_RMID);
}

static void handle_sigint(int sig) {
    (void)sig;
    printf("\n[Orion] Shutting down...\n");
    cleanup();
    exit(0);
}

/* ── Main ─────────────────────────────────────────────────────────────────── */
int main(void) {
    signal(SIGINT, handle_sigint);
    srand(time(NULL));

    /* Shared Memory */
    shm_id = shmget(SHM_KEY, sizeof(SharedArena), IPC_CREAT | 0666);
    if (shm_id < 0) { perror("shmget"); exit(1); }
    arena = (SharedArena *)shmat(shm_id, NULL, 0);
    if (arena == (void *)-1) { perror("shmat"); exit(1); }
    memset(arena, 0, sizeof(SharedArena));
    /* Inisialisasi semua username kosong */
    for (int i = 0; i < MAX_PLAYERS; i++) arena->players[i].username[0] = '\0';

    /* Message Queue */
    msgq_id = msgget(MSGQ_KEY, IPC_CREAT | 0666);
    if (msgq_id < 0) { perror("msgget"); exit(1); }

    /* Semaphore */
    sem_id = semget(SEM_KEY, 1, IPC_CREAT | 0666);
    if (sem_id < 0) { perror("semget"); exit(1); }
    semctl(sem_id, 0, SETVAL, 1);

    /* Load persistent data */
    load_players();

    printf("Orion is ready (PID: %d)\n", getpid());
    fflush(stdout);

    /* Mulai thread battle monitor & bot */
    pthread_t t_battle, t_bot;
    pthread_create(&t_battle, NULL, battle_thread, NULL);
    pthread_create(&t_bot,    NULL, bot_thread,    NULL);
    pthread_detach(t_battle);
    pthread_detach(t_bot);

    /* ── Message loop ──────────────────────────────────────────────────────── */
    Message msg;
    while (1) {
        if (msgrcv(msgq_id, &msg, sizeof(msg) - sizeof(long), MTYPE_TO_SERVER, 0) < 0)
            continue;

        switch (msg.cmd) {
            case CMD_PING:             handle_ping(&msg);              break;
            case CMD_REGISTER:         handle_register(&msg);          break;
            case CMD_LOGIN:            handle_login(&msg);             break;
            case CMD_LOGOUT:           handle_logout(&msg);            break;
            case CMD_BUY_WEAPON:       handle_buy_weapon(&msg);        break;
            case CMD_GET_HISTORY:      handle_get_history(&msg);       break;
            case CMD_MATCHMAKE:        handle_matchmake(&msg);         break;
            case CMD_CANCEL_MATCH:     handle_cancel_match(&msg);      break;
            case CMD_ATTACK:           handle_attack(&msg, 0);         break;
            case CMD_ULTIMATE:         handle_attack(&msg, 1);         break;
            case CMD_GET_BATTLE_STATE: handle_get_battle_state(&msg);  break;
            default: break;
        }
    }
    return 0;
}