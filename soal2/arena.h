/*
 * arena.h — Dunia Eterion: Definisi bersama
 *
 * IPC yang digunakan:
 *   - Shared Memory  (key 0x00001234) : data semua player + state arena
 *   - Message Queue  (key 0x00005678) : komunikasi eternal <-> orion
 *   - Semaphore      (key 0x00009012) : mutual exclusion (cegah race condition)
 */

#ifndef ARENA_H
#define ARENA_H

#include <sys/types.h>
#include <time.h>

/* ── IPC Keys (sesuai Makefile clear_ipc) ─────────────────────────────────── */
#define SHM_KEY        0x00001234
#define MSGQ_KEY       0x00005678
#define SEM_KEY        0x00009012

/* ── Konstanta game ────────────────────────────────────────────────────────── */
#define MAX_PLAYERS     32
#define MAX_HISTORY     50
#define MAX_WEAPONS     5
#define NAME_LEN        64
#define PASS_LEN        64
#define DATA_FILE       "players.dat"

#define BASE_DAMAGE     10
#define BASE_HEALTH     100
#define BASE_GOLD       150
#define BASE_XP         0
#define BASE_LVL        1

#define MATCHMAKING_TIMEOUT  35   /* detik */
#define ATTACK_COOLDOWN_MS   1000 /* ms */
#define COMBAT_LOG_LINES     5

/* ── Weapon ────────────────────────────────────────────────────────────────── */
typedef struct {
    char name[32];
    int  price;
    int  bonus_dmg;
} Weapon;

static const Weapon WEAPONS[MAX_WEAPONS] = {
    { "Wood Sword",  100,   5 },
    { "Iron Sword",  300,  15 },
    { "Steel Axe",   600,  30 },
    { "Demon Blade", 1500, 60 },
    { "God Slayer",  5000, 150},
};

/* ── Match History entry ───────────────────────────────────────────────────── */
typedef struct {
    char   time_str[16];   /* "HH:MM" */
    char   opponent[NAME_LEN];
    int    result;         /* 1=WIN, 0=LOSS */
    int    xp_gained;
} HistoryEntry;

/* ── Player data (persistent + shared) ────────────────────────────────────── */
typedef struct {
    char   username[NAME_LEN];
    char   password[PASS_LEN];
    int    xp;
    int    gold;
    int    lvl;
    int    weapon_idx;      /* -1 = none, 0-4 = WEAPONS index */

    /* History */
    HistoryEntry history[MAX_HISTORY];
    int          history_count;

    /* Runtime state (in shared memory) */
    int    logged_in;       /* 1 jika sedang aktif di sesi manapun */
    int    in_battle;       /* 1 jika sedang bertarung */
    int    shm_slot;        /* indeks di SHM */
    pid_t  client_pid;      /* PID eternal yang memakai slot ini */
} Player;

/* ── Arena session (satu slot battle) ─────────────────────────────────────── */
#define BATTLE_NONE     0
#define BATTLE_WAITING  1   /* sedang matchmaking */
#define BATTLE_ACTIVE   2   /* sedang bertarung */
#define BATTLE_DONE     3   /* selesai */

typedef struct {
    int    active;          /* slot ini dipakai? */
    int    player_slot[2];  /* indeks player di SHM */
    int    hp[2];           /* HP real-time */
    int    result[2];       /* 1=WIN, 0=LOSS, -1=belum */
    long   atk_cooldown[2]; /* timestamp ms terakhir serang */
    long   ult_cooldown[2];
    char   log[COMBAT_LOG_LINES][128];
    int    log_head;
    int    done;
} BattleSession;

/* ── Shared Memory Layout ──────────────────────────────────────────────────── */
typedef struct {
    Player        players[MAX_PLAYERS];
    int           player_count;
    BattleSession battles[MAX_PLAYERS / 2];
    int           battle_count;
} SharedArena;

/* ── Message Queue ─────────────────────────────────────────────────────────── */
/* mtype: dipakai sebagai routing */
#define MTYPE_TO_SERVER    1L   /* eternal → orion */
#define MTYPE_BROADCAST    2L   /* orion → semua (tidak dipakai langsung) */
/* mtype >= 100: reply ke eternal dengan PID tertentu */

typedef enum {
    CMD_PING = 0,
    CMD_REGISTER,
    CMD_LOGIN,
    CMD_LOGOUT,
    CMD_MATCHMAKE,
    CMD_CANCEL_MATCH,
    CMD_ATTACK,
    CMD_ULTIMATE,
    CMD_BUY_WEAPON,
    CMD_GET_HISTORY,
    CMD_GET_BATTLE_STATE,
} CmdType;

typedef struct {
    long    mtype;
    CmdType cmd;
    pid_t   client_pid;
    int     player_slot;   /* slot player yang sudah login */
    char    arg1[NAME_LEN]; /* username / misc */
    char    arg2[PASS_LEN]; /* password / misc */
    int     iarg;           /* integer arg (weapon idx, dll) */
} Message;

/* ── Response dari server ──────────────────────────────────────────────────── */
typedef struct {
    long  mtype;           /* = 100 + client_pid untuk routing */
    int   status;          /* 0=OK, -1=ERR */
    char  msg[256];
    int   idata;           /* data integer (slot, dll) */
} Response;

/* ── Helper: hitung stats dari player ─────────────────────────────────────── */
static inline int player_damage(const Player *p) {
    int bonus = (p->weapon_idx >= 0) ? WEAPONS[p->weapon_idx].bonus_dmg : 0;
    return BASE_DAMAGE + (p->xp / 50) + bonus;
}

static inline int player_health(const Player *p) {
    return BASE_HEALTH + (p->xp / 10);
}

static inline int player_ultimate(const Player *p) {
    return player_damage(p) * 3;
}

#endif /* ARENA_H */