#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <semaphore.h>
#include <stdbool.h>

#include "game_state.h"
#include "game_sync.h"

/* ── helpers ─────────────────────────────────────────────────────────────── */

static void die(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

/* ── print_board ─────────────────────────────────────────────────────────── */

static void print_board(const GameState *gs) {
    printf("\033[2J\033[H");

    printf("══ ChompChamps ══  %s\n\n",
           gs->game_over ? "[ JUEGO TERMINADO ]" : "[ en curso ]");

    for (unsigned short y = 0; y < gs->height; y++) {
        for (unsigned short x = 0; x < gs->width; x++) {
            char cell = gs->board[y * gs->width + x];
            if (cell > 0)
                printf(" %d", cell);
            else if (cell == 0)
                printf(" .");
            else
                printf(" %c", 'A' + (int)(-cell));
        }
        printf("\n");
    }

    printf("\n%-16s %6s %8s %8s  pos\n",
           "jugador", "score", "válidos", "inválidos");
    printf("──────────────────────────────────────────────\n");
    for (unsigned char i = 0; i < gs->player_count; i++) {
        const Player *p = &gs->players[i];
        printf("%-16s %6u %8u %8u  (%u,%u)%s\n",
               p->name, p->score, p->valid_moves, p->invalid_moves,
               p->x, p->y, p->blocked ? "  [bloqueado]" : "");
    }
    printf("\n");
    fflush(stdout);
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "uso: vista <width> <height>\n");
        return EXIT_FAILURE;
    }

    unsigned short width  = (unsigned short)atoi(argv[1]);
    unsigned short height = (unsigned short)atoi(argv[2]);

    /* conectarse a /game_state (solo lectura) */
    int fd_state = shm_open(SHM_STATE_NAME, O_RDONLY, 0);
    if (fd_state == -1) die("shm_open game_state");
    size_t state_size = game_state_size(width, height);
    GameState *gs = mmap(NULL, state_size, PROT_READ, MAP_SHARED, fd_state, 0);
    if (gs == MAP_FAILED) die("mmap game_state");
    close(fd_state);

    /* conectarse a /game_sync (necesita escritura para los semáforos) */
    int fd_sync = shm_open(SHM_SYNC_NAME, O_RDWR, 0);
    if (fd_sync == -1) die("shm_open game_sync");
    GameSync *sync = mmap(NULL, sizeof(GameSync), PROT_READ | PROT_WRITE,
                          MAP_SHARED, fd_sync, 0);
    if (sync == MAP_FAILED) die("mmap game_sync");
    close(fd_sync);

    /*
     * Loop principal.
     * La sincronización vista<->master usa solo los semáforos A y B:
     *
     *   sem_wait(A)  ← espera que el master avise "hay cambios"
     *   print_board  ← lee directo, el master ya terminó de escribir
     *   sem_post(B)  ← avisa al master "terminé de imprimir"
     *
     * No se usan C/D/E/F porque esos son exclusivos del patrón
     * lectores-escritores entre jugadores y master. La vista ya está
     * coordinada por A y B — cuando A se libera, el master no escribe.
     */
    while (1) {
        if (sem_wait(&sync->view_notify) == -1) die("sem_wait view_notify");

        bool over = gs->game_over;
        print_board(gs);

        if (sem_post(&sync->view_done) == -1) die("sem_post view_done");

        if (over) break;
    }

    munmap(gs, state_size);
    munmap(sync, sizeof(GameSync));
    return EXIT_SUCCESS;
}