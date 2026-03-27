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
 
/* ── tipos auxiliares ────────────────────────────────────────────────────── */
 
typedef struct { short x; short y; } Cell;
 
/* ── helpers ─────────────────────────────────────────────────────────────── */
 
static void die(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}
 
/* ── lock_read / unlock_read ─────────────────────────────────────────────
 *
 * Protocolo lectores-escritores sin inanición del escritor.
 *
 * lock_read:
 *   wait(C)            ← me bloqueo si el master está esperando para escribir
 *   wait(E)            ← lock del contador
 *     readers_count++
 *     si soy el primero: wait(D)  ← bloqueo al master (escritor)
 *   post(E)
 *   post(C)            ← libero C para otros lectores
 *
 * unlock_read:
 *   wait(E)
 *     readers_count--
 *     si soy el último: post(D)   ← libero al master para que escriba
 *   post(E)
 */
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
 
/* ── find_my_index ───────────────────────────────────────────────────────
 *
 * El master guarda el PID de cada jugador en gs->players[i].pid
 * después del fork(). Buscamos nuestro propio PID para saber
 * qué índice nos corresponde en la lista de jugadores.
 */
static int find_my_index(const GameState *gs) {
    pid_t my_pid = getpid();
    for (int i = 0; i < gs->player_count; i++)
        if (gs->players[i].pid == my_pid)
            return i;
    return -1;
}
 
/* ── flood_fill ──────────────────────────────────────────────────────────
 *
 * Cuenta cuántas celdas libres son alcanzables desde (sx, sy)
 * usando board_copy como referencia (no modifica el tablero real).
 * Usa un stack en heap para evitar recursión profunda.
 */
static int flood_fill(const GameState *gs,
                      unsigned short sx, unsigned short sy,
                      const char *board_copy)
{
    int w     = gs->width;
    int h     = gs->height;
    int total = w * h;
 
    uint8_t *visited = calloc((size_t)total, 1);
    Cell    *stack   = malloc((size_t)total * sizeof(Cell));
    if (!visited || !stack) {
        free(visited);
        free(stack);
        return 0;
    }
 
    int count = 0;
    int top   = 0;
    stack[top++]         = (Cell){(short)sx, (short)sy};
    visited[sy * w + sx] = 1;
 
    while (top > 0) {
        Cell c = stack[--top];
        count++;
 
        for (int d = 0; d < DIRS; d++) {
            int nx = c.x + DX[d];
            int ny = c.y + DY[d];
            if (nx < 0 || nx >= w || ny < 0 || ny >= h) continue;
            int idx = ny * w + nx;
            if (visited[idx])         continue;
            if (board_copy[idx] <= 0) continue; /* ocupada o capturada */
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
 * IA basada en flood fill:
 *   Para cada dirección válida:
 *     1. Simular la captura de esa celda en una copia del tablero.
 *     2. Calcular cuántas celdas libres quedan accesibles (flood fill).
 *     3. Elegir la dirección que maximiza ese territorio.
 *     4. Desempate: mayor recompensa inmediata.
 *
 * Si no hay ningún movimiento válido devuelve 0. El master lo
 * contará como inválido; el EOF en el pipe lo marcará bloqueado.
 */
static unsigned char elegir_movimiento(const GameState *gs, int player_idx) {
    const Player *me    = &gs->players[player_idx];
    int           w     = gs->width;
    int           h     = gs->height;
    int           total = w * h;
 
    char *board_copy = malloc((size_t)total);
    if (!board_copy) return 0;
    memcpy(board_copy, gs->board, (size_t)total);
 
    int best_dir    = -1;
    int best_flood  = -1;
    int best_reward = -1;
 
    for (int d = 0; d < DIRS; d++) {
        int nx = (int)me->x + DX[d];
        int ny = (int)me->y + DY[d];
 
        if (nx < 0 || nx >= w || ny < 0 || ny >= h) continue;
 
        char cell = board_copy[ny * w + nx];
        if (cell <= 0) continue; /* celda ocupada */
 
        /* simular captura */
        board_copy[ny * w + nx] = (char)(-player_idx);
 
        int flood  = flood_fill(gs, (unsigned short)nx, (unsigned short)ny,
                                board_copy);
        int reward = (int)cell;
 
        /* restaurar */
        board_copy[ny * w + nx] = cell;
 
        /* prioridad 1: más territorio; prioridad 2: más recompensa inmediata */
        if (flood > best_flood || (flood == best_flood && reward > best_reward)) {
            best_flood  = flood;
            best_reward = reward;
            best_dir    = d;
        }
    }
 
    free(board_copy);
    return (unsigned char)(best_dir >= 0 ? best_dir : 0);
}
 
/* ── main ─────────────────────────────────────────────────────────────────── */
 
int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "uso: player <width> <height>\n");
        return EXIT_FAILURE;
    }
 
    unsigned short width  = (unsigned short)atoi(argv[1]);
    unsigned short height = (unsigned short)atoi(argv[2]);
 
    /* ── conectarse a /game_state (solo lectura) ── */
    int fd_state = shm_open(SHM_STATE_NAME, O_RDONLY, 0);
    if (fd_state == -1) die("shm_open game_state");
    size_t state_size = game_state_size(width, height);
    GameState *gs = mmap(NULL, state_size, PROT_READ, MAP_SHARED, fd_state, 0);
    if (gs == MAP_FAILED) die("mmap game_state");
    close(fd_state);
 
    /* ── conectarse a /game_sync (lectura+escritura para operar semáforos) ── */
    int fd_sync = shm_open(SHM_SYNC_NAME, O_RDWR, 0);
    if (fd_sync == -1) die("shm_open game_sync");
    GameSync *sync = mmap(NULL, sizeof(GameSync), PROT_READ | PROT_WRITE,
                          MAP_SHARED, fd_sync, 0);
    if (sync == MAP_FAILED) die("mmap game_sync");
    close(fd_sync);
 
    /* ── buscar nuestro índice en la lista de jugadores ──
     *
     * El master hace fork() y luego escribe el PID del hijo en
     * gs->players[i].pid dentro de initPlayers(). Hacemos polling
     * hasta que aparezca. Ocurre una sola vez al arrancar.
     */
    int my_idx = -1;
    while (my_idx == -1) {
        my_idx = find_my_index(gs);
        if (my_idx == -1) usleep(1000); /* 1ms entre intentos */
    }
 
    /* ── esperar la señal de largada del master ──
     *
     * El master hace sem_post(player_ack[i]) para cada jugador
     * al inicio de game_loop(), cuando todo está inicializado.
     */
    if (sem_wait(&sync->player_ack[my_idx]) == -1) die("sem_wait player_ack inicial");
 
    /* ── loop principal ── */
    while (1) {
        /*
         * 1. Adquirir acceso al estado como lector.
         *    Dentro del lock leemos game_over Y calculamos el movimiento
         *    para que ambas lecturas sean sobre el mismo estado consistente.
         */
        lock_read(sync);
 
        bool over          = gs->game_over;
        unsigned char move = 0;
 
        if (!over && !gs->players[my_idx].blocked)
            move = elegir_movimiento(gs, my_idx);
 
        unlock_read(sync);
 
        /* 2. Si el juego terminó, salir limpiamente */
        if (over) break;
 
        /*
         * 3. Enviar movimiento al master por stdout (fd 1).
         *    El master redirigió nuestro stdout al pipe con dup2().
         *    1 byte = dirección elegida (0-7).
         *    Si write falla (pipe roto) el master ya terminó → salir.
         */
        if (write(STDOUT_FILENO, &move, 1) != 1) break;
 
        /*
         * 4. Esperar el ack del master.
         *    El master hace sem_post(player_ack[my_idx]) cuando terminó
         *    de procesar nuestro movimiento (válido o inválido).
         *    Recién entonces podemos enviar el siguiente.
         */
        if (sem_wait(&sync->player_ack[my_idx]) == -1) die("sem_wait player_ack");
    }
 
    /* ── liberar mappings ── */
    munmap(gs,   state_size);
    munmap(sync, sizeof(GameSync));
    return EXIT_SUCCESS;
}
 

