#ifndef GAME_STATE_H
#define GAME_STATE_H
 
#include <stdbool.h>
#include <sys/types.h>

#define MAX_PLAYERS 9
#define SHM_STATE_NAME "/game_state"

typedef struct {
    char name[16];           // Nombre del jugador
    unsigned int score;      // Puntaje acumulado
    unsigned int invalid_moves; // Solicitudes inválidas
    unsigned int valid_moves;   // Solicitudes válidas
    unsigned short x;        // Columna (origen: esquina superior izquierda)
    unsigned short y;        // Fila
    pid_t pid;               // PID del proceso jugador
    bool blocked;            // true si no puede moverse
} Player;

typedef struct {
    unsigned short width;        // Ancho del tablero
    unsigned short height;       // Alto del tablero
    unsigned char player_count;  // Cantidad de jugadores activos
    Player players[MAX_PLAYERS]; // Lista de jugadores
    bool game_over;              // true cuando el juego terminó
    signed char board[];         // Tablero: width * height chars (flexible array)
} GameState;

static inline size_t game_state_size(unsigned short w, unsigned short h) {
    return sizeof(GameState) + (size_t)w * h * sizeof(signed char);
}
 
/* Acceso a una celda del tablero */
static inline signed char *board_cell(GameState *gs, unsigned short x, unsigned short y) {
    return &gs->board[y * gs->width + x];
}
 
#endif /* GAME_STATE_H */