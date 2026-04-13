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

/* ── print_title ────────────────────────────────────────────────────────── */

static void print_title(bool game_over) {
    if (game_over) {
        printf("  " C_TITLE BOLD "✦ ChompChamps ✦" RESET
               "  " C_GAME_OVER BOLD "[ JUEGO TERMINADO ]" RESET
               ERASE_EOL "\n\n");
    } else {
        printf("  " C_TITLE BOLD "✦ ChompChamps ✦" RESET
               "  " DIM "[ en curso ]" RESET
               ERASE_EOL "\n\n");
    }
}

/* ── print_top_border / print_bottom_border ─────────────────────────────── */

static void print_top_border(int w) {
    printf("  " C_BORDER "┌");
    for (int x = 0; x < w; x++) printf("──");
    printf("┐" RESET ERASE_EOL "\n");
}

static void print_bottom_border(int w) {
    printf("  " C_BORDER "└");
    for (int x = 0; x < w; x++) printf("──");
    printf("┘" RESET ERASE_EOL "\n\n");
}

/* ── print_cell ─────────────────────────────────────────────────────────── */

static void print_cell(const GameState *gs, unsigned short x, unsigned short y) {
    signed char cell = (signed char)gs->board[y * gs->width + x];
    if (cell > 0) {
        printf(C_FOOD " %d" RESET, (int)cell);
    } else if (cell == 0) {
        printf("  ");
    } else {
        int pidx = (-(int)cell) - 1;
        if (pidx < 0 || pidx >= MAX_PLAYERS) pidx = 0;
        char letter = 'A' + pidx;
        bool is_head = (gs->players[pidx].x == x && gs->players[pidx].y == y);
        if (is_head) {
            printf("%s" UNDERLINE " %c" RESET, P_COLOR[pidx % 9], letter);
        } else {
            printf("%s ●" RESET, P_COLOR[pidx % 9]);
        }
    }
}

/* ── print_board_rows ───────────────────────────────────────────────────── */

static void print_board_rows(const GameState *gs) {
    for (unsigned short y = 0; y < gs->height; y++) {
        printf("  " C_BORDER "│" RESET);
        for (unsigned short x = 0; x < gs->width; x++)
            print_cell(gs, x, y);
        printf(C_BORDER "│" RESET ERASE_EOL "\n");
    }
}

/* ── print_player_row ───────────────────────────────────────────────────── */

static void print_player_row(const Player *p, int i) {
    const char *col = P_COLOR[i % 9];
    if (p->blocked) {
        printf("  %s%-16s" RESET "  %6u  %7u    %7u  "
               C_BLOCKED "[bloqueado]" RESET ERASE_EOL "\n",
               col, p->name, p->score, p->valid_moves, p->invalid_moves);
    } else {
        printf("  %s%-16s" RESET "  %6u  %7u    %7u  (%u,%u)"
               ERASE_EOL "\n",
               col, p->name, p->score, p->valid_moves, p->invalid_moves,
               p->x, p->y);
    }
}

/* ── print_scoreboard ───────────────────────────────────────────────────── */

static void print_scoreboard(const GameState *gs) {
    printf("  " C_HEADER BOLD
           "Jugador            Score  Válidos  Inválidos  Pos"
           RESET ERASE_EOL "\n");
    printf("  " C_BORDER
           "───────────────────────────────────────────────────"
           RESET ERASE_EOL "\n");
    for (unsigned char i = 0; i < gs->player_count; i++)
        print_player_row(&gs->players[i], (int)i);
    printf(ERASE_EOL "\n");
}

/* ── print_board ────────────────────────────────────────────────────────── */

static void print_board(const GameState *gs) {
    printf(CURSOR_HOME);
    print_title(gs->game_over);
    print_top_border(gs->width);
    print_board_rows(gs);
    print_bottom_border(gs->width);
    print_scoreboard(gs);
    fflush(stdout);
}

/* ── find_winner_idx ────────────────────────────────────────────────────── */

static int find_winner_idx(const GameState *gs, bool *empate_out) {
    int winner = 0;
    bool empate = false;
    int n = gs->player_count;
    for (int i = 1; i < n; i++) {
        const Player *w = &gs->players[winner];
        const Player *c = &gs->players[i];
        if (c->score > w->score) { winner = i; empate = false; }
        else if (c->score == w->score) {
            if (c->valid_moves < w->valid_moves) { winner = i; empate = false; }
            else if (c->valid_moves == w->valid_moves) empate = true;
        }
    }
    *empate_out = empate;
    return winner;
}

/* ── sort_by_score ──────────────────────────────────────────────────────── */

static void sort_by_score(const GameState *gs, int *order) {
    int n = gs->player_count;
    for (int i = 0; i < n; i++) order[i] = i;
    for (int i = 0; i < n - 1; i++)
        for (int j = i + 1; j < n; j++)
            if (gs->players[order[j]].score > gs->players[order[i]].score) {
                int tmp = order[i]; order[i] = order[j]; order[j] = tmp;
            }
}

/* ── print_centered_box_line ────────────────────────────────────────────── */

static void print_centered_box_line(const char *txt, int vlen, int box_w) {
    printf("  " C_BORDER "║" RESET);
    int pad_l = (box_w - vlen) / 2;
    int pad_r = box_w - vlen - pad_l;
    for (int i = 0; i < pad_l; i++) putchar(' ');
    printf("%s", txt);
    for (int i = 0; i < pad_r; i++) putchar(' ');
    printf(C_BORDER "║" RESET ERASE_EOL "\n");
}

/* ── print_game_over_header ─────────────────────────────────────────────── */

static void print_game_over_header(const GameState *gs, int winner,
                                   bool empate, int box_w) {
    printf("\n");
    printf("  " C_BORDER "╔════════════════════════════════════════════════════╗" RESET ERASE_EOL "\n");
    print_centered_box_line(C_TITLE BOLD "★  JUEGO TERMINADO  ★" RESET, 21, box_w);
    if (empate) {
        print_centered_box_line(C_GAME_OVER BOLD "¡EMPATE!" RESET, 8, box_w);
    } else {
        const Player *wp = &gs->players[winner];
        const char   *wc = P_COLOR[winner % 9];
        char line[80];
        snprintf(line, sizeof(line), BOLD "%sGanador: %s" RESET, wc, wp->name);
        int vlen = 9 + (int)strlen(wp->name);
        print_centered_box_line(line, vlen, box_w);
    }
}

/* ── print_game_over_results ────────────────────────────────────────────── */

static void print_game_over_results(const GameState *gs, const int *order,
                                    int winner, bool empate) {
    int n = gs->player_count;
    printf("  " C_BORDER "╠════════════════════════════════════════════════════╣" RESET ERASE_EOL "\n");
    printf("  " C_BORDER "║" RESET C_HEADER BOLD
           "  #  Jugador            Score  Validos  Invalidos   "
           RESET C_BORDER "║" RESET ERASE_EOL "\n");
    printf("  " C_BORDER "╠════════════════════════════════════════════════════╣" RESET ERASE_EOL "\n");
    for (int r = 0; r < n; r++) {
        int idx = order[r];
        const Player *p   = &gs->players[idx];
        const char   *col = P_COLOR[idx % 9];
        const char   *medal = (idx == winner && !empate) ? "\033[1;33m★ " RESET : "  ";
        printf("  " C_BORDER "║" RESET
               " %s%2d  %s%-16s" RESET "  %6u   %6u    %6u  "
               C_BORDER "║" RESET ERASE_EOL "\n",
               medal, r + 1, col, p->name,
               p->score, p->valid_moves, p->invalid_moves);
    }
}

/* ── print_game_over ────────────────────────────────────────────────────── */

static void print_game_over(const GameState *gs) {
    int box_w = 52;
    bool empate;
    int winner = find_winner_idx(gs, &empate);
    int order[MAX_PLAYERS];
    sort_by_score(gs, order);
    print_game_over_header(gs, winner, empate, box_w);
    print_game_over_results(gs, order, winner, empate);
    printf("  " C_BORDER "╠════════════════════════════════════════════════════╣" RESET ERASE_EOL "\n");
    print_centered_box_line(DIM "Presioná Ctrl+C para salir" RESET, 26, box_w);
    printf("  " C_BORDER "╚════════════════════════════════════════════════════╝" RESET ERASE_EOL "\n\n");
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

    /* Pantalla final con cuadro estilizado — no redibujar el tablero */
    print_game_over(gs);

    /* Esperar Ctrl+C */
    while (1) pause();
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