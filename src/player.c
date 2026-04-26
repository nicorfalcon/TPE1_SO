#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>

#include "game_state.h"
#include "game_sync.h"
#include "protocol.h"

/* ── tipos auxiliares ────────────────────────────────────────────────────── */

typedef struct { short x; short y; } Cell;

/* ── helpers ─────────────────────────────────────────────────────────────── */

static void die(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

/* ── lock_read / unlock_read ─────────────────────────────────────────────── */

static void lock_read(GameSync *sync) {
    if (sem_wait(&sync->no_writer)     == -1) die("sem_wait no_writer");
    if (sem_wait(&sync->readers_mutex) == -1) die("sem_wait readers_mutex");
    sync->readers_count++;
    if (sync->readers_count == 1)
        if (sem_wait(&sync->state_mutex) == -1) die("sem_wait state_mutex");
    if (sem_post(&sync->readers_mutex) == -1) die("sem_post readers_mutex");
    if (sem_post(&sync->no_writer)     == -1) die("sem_post no_writer");
}

static void unlock_read(GameSync *sync) {
    if (sem_wait(&sync->readers_mutex) == -1) die("sem_wait readers_mutex (unlock)");
    sync->readers_count--;
    if (sync->readers_count == 0)
        if (sem_post(&sync->state_mutex) == -1) die("sem_post state_mutex");
    if (sem_post(&sync->readers_mutex) == -1) die("sem_post readers_mutex (unlock)");
}

/* ── find_my_index ──────────────────────────────────────────────────────── */

static int find_my_index(const GameState *gs) {
    pid_t my_pid = getpid();
    for (int i = 0; i < gs->player_count; i++)
        if (gs->players[i].pid == my_pid)
            return i;
    return -1;
}

/* ── flood_push_neighbors ───────────────────────────────────────────────── */

static void flood_push_neighbors(const signed char *board_copy, int w, int h,
                                 Cell c, uint8_t *visited, Cell *stack, int *top) {
    for (int d = 0; d < DIRS; d++) {
        int nx = c.x + DX[d], ny = c.y + DY[d];
        if (nx < 0 || nx >= w || ny < 0 || ny >= h) continue;
        int idx = ny * w + nx;
        if (visited[idx] || board_copy[idx] <= 0) continue;
        visited[idx] = 1;
        stack[(*top)++] = (Cell){(short)nx, (short)ny};
    }
}

/* ── flood_fill ─────────────────────────────────────────────────────────── */

static int flood_fill(const GameState *gs, unsigned short sx, unsigned short sy,
                      const signed char *board_copy) {
    int w = gs->width, h = gs->height, total = w * h;
    uint8_t *visited = calloc((size_t)total, 1);
    Cell    *stack   = malloc((size_t)total * sizeof(Cell));
    if (!visited || !stack) { free(visited); free(stack); return 0; }
    int count = 0, top = 0;
    stack[top++] = (Cell){(short)sx, (short)sy};
    visited[sy * w + sx] = 1;
    while (top > 0) {
        Cell c = stack[--top];
        count++;
        flood_push_neighbors(board_copy, w, h, c, visited, stack, &top);
    }
    free(visited);
    free(stack);
    return count;
}

/* ── min_dist_to_opponents ──────────────────────────────────────────────── */

static int min_dist_to_opponents(const GameState *gs, int my_idx, int nx, int ny) {
    int min_dist = INT_MAX;
    for (int i = 0; i < gs->player_count; i++) {
        if (i == my_idx || gs->players[i].blocked) continue;
        int dx = (int)gs->players[i].x - nx; if (dx < 0) dx = -dx;
        int dy = (int)gs->players[i].y - ny; if (dy < 0) dy = -dy;
        int dist = dx > dy ? dx : dy;
        if (dist < min_dist) min_dist = dist;
    }
    return min_dist;
}

/* ── eval_direction ─────────────────────────────────────────────────────── */

static void eval_direction(const GameState *gs, int player_idx, signed char *board_copy,
                           int d, int *best_dir, int *best_flood, int *best_reward, int *best_dist) {
    const Player *me = &gs->players[player_idx];
    int w = gs->width, h = gs->height;
    int nx = (int)me->x + DX[d], ny = (int)me->y + DY[d];
    if (nx < 0 || nx >= w || ny < 0 || ny >= h) return;
    signed char cell = board_copy[ny * w + nx];
    if (cell <= 0) return;
    board_copy[ny * w + nx] = (signed char)(-(player_idx + 1));
    int flood  = flood_fill(gs, (unsigned short)nx, (unsigned short)ny, board_copy);
    int reward = (int)cell;
    board_copy[ny * w + nx] = cell;
    int dist = min_dist_to_opponents(gs, player_idx, nx, ny);
    if (flood > *best_flood ||
        (flood == *best_flood && dist < *best_dist) ||
        (flood == *best_flood && dist == *best_dist && reward > *best_reward)) {
        *best_flood  = flood;
        *best_reward = reward;
        *best_dist   = dist;
        *best_dir    = d;
    }
}

/* ── elegir_movimiento ──────────────────────────────────────────────────── */

static unsigned char elegir_movimiento(const GameState *gs, int player_idx) {
    int total = gs->width * gs->height;
    signed char *board_copy = malloc((size_t)total);
    if (!board_copy) return 0;
    memcpy(board_copy, gs->board, (size_t)total);
    int best_dir = -1, best_flood = -1, best_reward = -1, best_dist = INT_MAX;
    for (int d = 0; d < DIRS; d++)
        eval_direction(gs, player_idx, board_copy, d,
                       &best_dir, &best_flood, &best_reward, &best_dist);
    free(board_copy);
    return (unsigned char)(best_dir >= 0 ? best_dir : 0);
}

/* ── map_game_state ─────────────────────────────────────────────────────── */

static GameState *map_game_state(unsigned short width, unsigned short height) {
    int fd = shm_open(SHM_STATE_NAME, O_RDONLY, 0);
    if (fd == -1) die("shm_open game_state");
    size_t sz = game_state_size(width, height);
    GameState *gs = mmap(NULL, sz, PROT_READ, MAP_SHARED, fd, 0);
    if (gs == MAP_FAILED) die("mmap game_state");
    close(fd);
    return gs;
}

/* ── map_game_sync ──────────────────────────────────────────────────────── */

static GameSync *map_game_sync(void) {
    int fd = shm_open(SHM_SYNC_NAME, O_RDWR, 0);
    if (fd == -1) die("shm_open game_sync");
    GameSync *sync = mmap(NULL, sizeof(GameSync), PROT_READ | PROT_WRITE,
                          MAP_SHARED, fd, 0);
    if (sync == MAP_FAILED) die("mmap game_sync");
    close(fd);
    return sync;
}

/* ── wait_for_index ─────────────────────────────────────────────────────── */

static int wait_for_index(const GameState *gs) {
    int idx = -1;
    while (idx == -1) {
        idx = find_my_index(gs);
        if (idx == -1) {
            struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000L};
            nanosleep(&ts, NULL);
        }
    }
    return idx;
}

/* ── player_loop ────────────────────────────────────────────────────────── */

static void player_loop(const GameState *gs, GameSync *sync, int my_idx) {
    while (1) {
        lock_read(sync);
        bool over    = gs->game_over;
        bool blocked = gs->players[my_idx].blocked;
        unsigned char move = 0;
        if (!over && !blocked)
            move = elegir_movimiento(gs, my_idx);
        unlock_read(sync);
        if (over || blocked) break;
        if (write(STDOUT_FILENO, &move, 1) != 1) break;
        if (sem_wait(&sync->player_ack[my_idx]) == -1) die("sem_wait player_ack");
    }
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "uso: player <width> <height>\n");
        return EXIT_FAILURE;
    }
    unsigned short width  = (unsigned short)atoi(argv[1]);
    unsigned short height = (unsigned short)atoi(argv[2]);
    GameState *gs   = map_game_state(width, height);
    GameSync  *sync = map_game_sync();
    int my_idx = wait_for_index(gs);
    if (sem_wait(&sync->player_ack[my_idx]) == -1) die("sem_wait player_ack inicial");
    player_loop(gs, sync, my_idx);
    munmap(gs, game_state_size(width, height));
    munmap(sync, sizeof(GameSync));
    return EXIT_SUCCESS;
}