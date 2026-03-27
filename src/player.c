#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdint.h>
 
#include "include/game_state.h"
#include "include/game_sync.h"
#include "include/protocol.h"

typedef struct { short x; short y; } Cell;


static void die(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

static int flood_fill(const GameState *gs,
                      unsigned short sx, unsigned short sy,
                      const char *board_copy)
{
    int w = gs->width;
    int h = gs->height;
    int total = w * h;

    uint8_t *visited = calloc((size_t)total, 1);
    Cell    *stack   = malloc((size_t)total * sizeof(Cell));
    if (!visited || !stack) {
        free(visited); free(stack);
        return 0;
    }
 
    int count = 0;
    int top = 0;
    stack[top++] = (Cell){(short)sx, (short)sy};
    visited[sy * w + sx] = 1;
 
    while (top > 0) {
        Cell c = stack[--top];
        count++;
 
        for (int d = 0; d < DIRS; d++) {
            int nx = c.x + DX[d];
            int ny = c.y + DY[d];
            if (nx < 0 || nx >= w || ny < 0 || ny >= h) continue;
            int idx = ny * w + nx;
            if (visited[idx]) continue;
            if (board_copy[idx] <= 0) continue;  /* ocupada o capturada */
            visited[idx] = 1;
            stack[top++] = (Cell){(short)nx, (short)ny};
        }
    }
 
    free(visited);
    free(stack);
    return count;
}

/* ── elegir_movimiento ───────────────────────────────────────────────────
 *
 * Para cada dirección válida:
 *   1. Simular el tablero con esa celda capturada.
 *   2. Calcular flood fill desde la nueva posición.
 *   3. Como desempate, preferir la celda con mayor recompensa inmediata.
 *
 * Si no hay movimientos válidos, devuelve 0 (el master lo contará como inválido
 * y el EOF en el pipe lo marcará bloqueado).
 */
 
static unsigned char elegir_movimiento(const GameState *gs, int player_idx) {
    const Player *me = &gs->players[player_idx];
    int w = gs->width;
    int h = gs->height;
    int total = w * h;
 
    /* copia del tablero para simular sin modificar el real */
    char *board_copy = malloc((size_t)total);
    if (!board_copy) return 0;
    memcpy(board_copy, gs->board, (size_t)total);
 
    int best_dir      = -1;
    int best_flood    = -1;
    int best_reward   = -1;
 
    for (int d = 0; d < DIRS; d++) {
        int nx = (int)me->x + DX[d];
        int ny = (int)me->y + DY[d];
 
        /* fuera del tablero */
        if (nx < 0 || nx >= w || ny < 0 || ny >= h) continue;
 
        /* celda ocupada */
        char cell = board_copy[ny * w + nx];
        if (cell <= 0) continue;
 
        /* simular captura */
        board_copy[ny * w + nx] = (char)(-player_idx);
 
        int flood   = flood_fill(gs, (unsigned short)nx, (unsigned short)ny,
                                 board_copy);
        int reward  = (int)cell;
 
        /* restaurar */
        board_copy[ny * w + nx] = cell;
 
        /* comparar: prioridad 1 → más territorio; prioridad 2 → más recompensa */
        if (flood > best_flood || (flood == best_flood && reward > best_reward)) {
            best_flood  = flood;
            best_reward = reward;
            best_dir    = d;
        }
    }
 
    free(board_copy);
    return (unsigned char)(best_dir >= 0 ? best_dir : 0);
}


