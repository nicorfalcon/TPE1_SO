#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <semaphore.h>
#include <stdbool.h>
#include <signal.h>

#include "game_state.h"
#include "game_sync.h"

/* ── ANSI helpers ────────────────────────────────────────────────────────── */

#define RESET       "\033[0m"
#define BOLD        "\033[1m"
#define DIM         "\033[2m"
#define UNDERLINE   "\033[4m"
#define ERASE_EOL   "\033[K"

#define CURSOR_HOME  "\033[H"
#define CURSOR_HIDE  "\033[?25l"
#define CURSOR_SHOW  "\033[?25h"
#define CLEAR_SCREEN "\033[2J"

/* Un solo array de colores brillantes por jugador (tablero + scoreboard) */
static const char *P_COLOR[9] = {
    "\033[1;91m",  /* 0: rojo brillante    */
    "\033[1;92m",  /* 1: verde brillante   */
    "\033[1;93m",  /* 2: amarillo brillante*/
    "\033[1;94m",  /* 3: azul brillante    */
    "\033[1;95m",  /* 4: magenta brillante */
    "\033[1;96m",  /* 5: cian brillante    */
    "\033[1;97m",  /* 6: blanco brillante  */
    "\033[1;33m",  /* 7: amarillo oscuro   */
    "\033[1;35m",  /* 8: magenta oscuro    */
};

#define C_FOOD      "\033[0;37m"   /* comida 1-9: gris claro      */
#define C_BORDER    "\033[0;36m"   /* marcos: cian                */
#define C_TITLE     "\033[1;97m"   /* título: blanco brillante    */
#define C_GAME_OVER "\033[1;31m"   /* fin de juego: rojo           */
#define C_HEADER    "\033[1;37m"   /* encabezado tabla: bold grey  */
#define C_BLOCKED   "\033[0;31m"   /* bloqueado: rojo              */

/* ── helpers ─────────────────────────────────────────────────────────────── */

static void restore_terminal(void) {
    printf(CURSOR_SHOW RESET "\n");
    fflush(stdout);
}

static void sig_handler(int sig) {
    (void)sig;
    restore_terminal();
    _exit(0);
}

static void die(const char *msg) {
    restore_terminal();
    perror(msg);
    exit(EXIT_FAILURE);
}

/* ── print_board ────────────────────────────────────────────────────────── */

static void print_board(const GameState *gs) {
    unsigned short w = gs->width;
    unsigned short h = gs->height;

    /* Ir a la esquina superior — sobreescribir sin borrar (sin parpadeo) */
    printf(CURSOR_HOME);

    /* ── Título ── */
    if (gs->game_over) {
        printf("  " C_TITLE BOLD "✦ ChompChamps ✦" RESET
               "  " C_GAME_OVER BOLD "[ JUEGO TERMINADO ]" RESET
               ERASE_EOL "\n\n");
    } else {
        printf("  " C_TITLE BOLD "✦ ChompChamps ✦" RESET
               "  " DIM "[ en curso ]" RESET
               ERASE_EOL "\n\n");
    }

    /* ── Borde superior ── */
    printf("  " C_BORDER "┌");
    for (int x = 0; x < (int)w; x++) printf("──");
    printf("┐" RESET ERASE_EOL "\n");

    /* ── Filas del tablero ── */
    for (unsigned short y = 0; y < h; y++) {
        printf("  " C_BORDER "│" RESET);
        for (unsigned short x = 0; x < w; x++) {
            signed char cell = (signed char)gs->board[y * w + x];
            if (cell > 0) {
                /* Comida: gris claro uniforme */
                printf(C_FOOD " %d" RESET, (int)cell);
            } else if (cell == 0) {
                /* Celda vacía: espacio en blanco */
                printf("  ");
            } else {
                /* Territorio del jugador con índice = (-cell) - 1 */
                int pidx = (-(int)cell) - 1;
                if (pidx < 0 || pidx >= MAX_PLAYERS) pidx = 0;
                char letter = 'A' + pidx;
                bool is_head = (gs->players[pidx].x == x &&
                                gs->players[pidx].y == y);
                if (is_head) {
                    /* Cabeza del jugador: letra mayúscula con subrayado */
                    printf("%s" UNDERLINE " %c" RESET, P_COLOR[pidx % 9], letter);
                } else {
                    /* Territorio: puntito del color del jugador */
                    printf("%s ●" RESET, P_COLOR[pidx % 9]);
                }
            }
        }
        printf(C_BORDER "│" RESET ERASE_EOL "\n");
    }

    /* ── Borde inferior ── */
    printf("  " C_BORDER "└");
    for (int x = 0; x < (int)w; x++) printf("──");
    printf("┘" RESET ERASE_EOL "\n\n");

    /* ── Tabla de estadísticas ── */
    printf("  " C_HEADER BOLD
           "Jugador            Score  Válidos  Inválidos  Pos"
           RESET ERASE_EOL "\n");
    printf("  " C_BORDER
           "───────────────────────────────────────────────────"
           RESET ERASE_EOL "\n");

    for (unsigned char i = 0; i < gs->player_count; i++) {
        const Player *p = &gs->players[i];
        const char *col = P_COLOR[i % 9];
        if (p->blocked) {
            printf("  %s%-16s" RESET "  %6u  %7u    %7u  "
                   C_BLOCKED "[bloqueado]" RESET ERASE_EOL "\n",
                   col, p->name,
                   p->score, p->valid_moves, p->invalid_moves);
        } else {
            printf("  %s%-16s" RESET "  %6u  %7u    %7u  (%u,%u)"
                   ERASE_EOL "\n",
                   col, p->name,
                   p->score, p->valid_moves, p->invalid_moves,
                   p->x, p->y);
        }
    }
    printf(ERASE_EOL "\n");
    fflush(stdout);
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

/* ── view_loop ──────────────────────────────────────────────────────────── */

static void view_loop(const GameState *gs, GameSync *sync) {
    /* Limpiar pantalla una sola vez al inicio y ocultar cursor */
    printf(CLEAR_SCREEN CURSOR_HOME CURSOR_HIDE);
    fflush(stdout);

    while (1) {
        if (sem_wait(&sync->view_notify) == -1) die("sem_wait view_notify");
        bool over = gs->game_over;
        print_board(gs);
        if (sem_post(&sync->view_done) == -1) die("sem_post view_done");
        if (over) break;
    }
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "uso: vista <width> <height>\n");
        return EXIT_FAILURE;
    }

    atexit(restore_terminal);
    signal(SIGTERM, sig_handler);
    signal(SIGINT,  sig_handler);

    unsigned short width  = (unsigned short)atoi(argv[1]);
    unsigned short height = (unsigned short)atoi(argv[2]);

    GameState *gs   = map_game_state(width, height);
    GameSync  *sync = map_game_sync();

    view_loop(gs, sync);

    munmap(gs, game_state_size(width, height));
    munmap(sync, sizeof(GameSync));

    restore_terminal();
    return EXIT_SUCCESS;
}