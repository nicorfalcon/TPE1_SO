#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <semaphore.h>
#include <stdbool.h>

#include "include/game_state.h"
#include "include/game_sync.h"

/* ── constantes ──────────────────────────────────────────────────────────── */

#define DEFAULT_WIDTH   10
#define DEFAULT_HEIGHT  10
#define DEFAULT_DELAY   200
#define DEFAULT_TIMEOUT 10
#define MIN_WIDTH       10
#define MIN_HEIGHT      10

/* ── estructura interna del master ──────────────────────────────────────── */

typedef struct {
    int width;
    int height;
    int delay_ms;
    int timeout_s;
    unsigned int seed;
    char *view_path;
    char *player_paths[MAX_PLAYERS];
    int   player_count;

    /* pipes: player_pipes[i][0]=lectura(master), player_pipes[i][1]=escritura(jugador) */
    int player_pipes[MAX_PLAYERS][2];

    /* PIDs de los hijos */
    pid_t player_pids[MAX_PLAYERS];
    pid_t view_pid;

    /* shared memory */
    GameState *gs;
    size_t     state_size;
    GameSync  *sync;
} Master;

/* ── helpers ─────────────────────────────────────────────────────────────── */

static void die(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

/* ── parse_args ──────────────────────────────────────────────────────────── */

static void parse_args(int argc, char *argv[], Master *m) {
    if (m == NULL) {
        fprintf(stderr, "error: puntero nulo\n");
        exit(EXIT_FAILURE);
    }
    m->width        = DEFAULT_WIDTH;
    m->height       = DEFAULT_HEIGHT;
    m->delay_ms     = DEFAULT_DELAY;
    m->timeout_s    = DEFAULT_TIMEOUT;
    m->seed         = (unsigned int)time(NULL);
    m->view_path    = NULL;
    m->player_count = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            m->width = atoi(argv[++i]);
            if (m->width < MIN_WIDTH) m->width = MIN_WIDTH;
        } else if (strcmp(argv[i], "-h") == 0 && i + 1 < argc) {
            m->height = atoi(argv[++i]);
            if (m->height < MIN_HEIGHT) m->height = MIN_HEIGHT;
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            m->delay_ms = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            m->timeout_s = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            m->seed = (unsigned int)atoi(argv[++i]);
        } else if (strcmp(argv[i], "-v") == 0 && i + 1 < argc) {
            m->view_path = argv[++i];
        } else if (strcmp(argv[i], "-p") == 0) {
            while (i + 1 < argc && argv[i + 1][0] != '-') {
                if (m->player_count >= MAX_PLAYERS) {
                    fprintf(stderr, "error: máximo %d jugadores\n", MAX_PLAYERS);
                    exit(EXIT_FAILURE);
                }
                m->player_paths[m->player_count++] = argv[++i];
            }
        }
    }

    if (m->player_count == 0) {
        fprintf(stderr,
            "uso: master -p jugador1 [jugador2 ...] "
            "[-w W] [-h H] [-d delay_ms] [-t timeout_s] [-s seed] [-v vista]\n");
        exit(EXIT_FAILURE);
    }
}

/* ── initGame: crea las dos shared memories ──────────────────────────────── */

static void initGame(Master *m) {
    /* ── /game_state ── */
    m->state_size = game_state_size((unsigned short)m->width,
                                    (unsigned short)m->height);

    int fdState = shm_open(SHM_STATE_NAME, O_CREAT | O_RDWR | O_TRUNC, 0666);
    if (fdState == -1) die("shm_open /game_state");
    if (ftruncate(fdState, (off_t)m->state_size) == -1) die("ftruncate /game_state");
    m->gs = mmap(NULL, m->state_size, PROT_READ | PROT_WRITE, MAP_SHARED, fdState, 0);
    if (m->gs == MAP_FAILED) die("mmap /game_state");
    close(fdState);

    /* inicializar campos */
    m->gs->width        = (unsigned short)m->width;
    m->gs->height       = (unsigned short)m->height;
    m->gs->player_count = (unsigned char)m->player_count;
    m->gs->game_over    = false;

    /* llenar tablero con recompensas aleatorias 1-9 */
    srand(m->seed);
    int total = m->width * m->height;
    for (int i = 0; i < total; i++)
        m->gs->board[i] = (char)(rand() % 9 + 1);

    /* ── /game_sync ── */
    int fdSync = shm_open(SHM_SYNC_NAME, O_CREAT | O_RDWR | O_TRUNC, 0666);
    if (fdSync == -1) die("shm_open /game_sync");
    if (ftruncate(fdSync, sizeof(GameSync)) == -1) die("ftruncate /game_sync");
    m->sync = mmap(NULL, sizeof(GameSync), PROT_READ | PROT_WRITE, MAP_SHARED, fdSync, 0);
    if (m->sync == MAP_FAILED) die("mmap /game_sync");
    close(fdSync);
}

/* ── initSem: inicializa los semáforos ───────────────────────────────────── */

static void initSem(Master *m) {
    GameSync *s = m->sync;

    /*
     * sem_init(sem, pshared=1, valor)
     * pshared=1 → compartido entre procesos (vive en shared memory)
     *
     * A y B en 0: señalización, arrancan bloqueados
     * C, D, E en 1: mutex, arrancan libres
     * G[i] en 0: cada jugador espera el primer ack del master
     */
    if (sem_init(&s->view_notify,   1, 0) == -1) die("sem_init view_notify");
    if (sem_init(&s->view_done,     1, 0) == -1) die("sem_init view_done");
    if (sem_init(&s->no_writer,     1, 1) == -1) die("sem_init no_writer");
    if (sem_init(&s->state_mutex,   1, 1) == -1) die("sem_init state_mutex");
    if (sem_init(&s->readers_mutex, 1, 1) == -1) die("sem_init readers_mutex");
    s->readers_count = 0;

    for (int i = 0; i < m->player_count; i++) {
        if (sem_init(&s->player_ack[i], 1, 0) == -1) die("sem_init player_ack");
    }
}

/* ── initCanales: crea los pipes de los jugadores ───────────────────────── */

static void initCanales(Master *m) {
    for (int i = 0; i < m->player_count; i++) {
        if (pipe(m->player_pipes[i]) == -1) die("pipe");
    }
}

/* ── initProcesses: fork de vista y jugadores ────────────────────────────
 *
 * Para cada jugador:
 *   1. fork()
 *   2. hijo: cierra todos los pipes que no le pertenecen,
 *            redirige su extremo de escritura a stdout (fd 1),
 *            ejecuta el binario del jugador con execlp
 *   3. padre: guarda el pid, cierra el extremo de escritura
 *             (el master solo lee)
 *
 * Para la vista:
 *   1. fork()
 *   2. hijo: ejecuta el binario de la vista pasando width y height
 *   3. padre: guarda el pid
 *
 * La vista NO usa pipes — se comunica solo por shared memory + semáforos A/B.
 */
static void initProcesses(Master *m) {
    char w[16], h[16];
    snprintf(w, sizeof(w), "%d", m->width);
    snprintf(h, sizeof(h), "%d", m->height);

    /* ── fork jugadores ── */
    for (int i = 0; i < m->player_count; i++) {
        pid_t pid = fork();
        if (pid < 0) die("fork jugador");

        if (pid == 0) {
            /* hijo: cerrar todos los pipes que no son el mío */
            for (int j = 0; j < m->player_count; j++) {
                if (j == i) {
                    /* mi pipe: cierro el extremo de lectura (lo usa el master) */
                    close(m->player_pipes[j][0]);
                } else {
                    /* pipe ajeno: cierro ambos extremos */
                    close(m->player_pipes[j][0]);
                    close(m->player_pipes[j][1]);
                }
            }

            /* redirigir escritura a stdout para que el master pueda leer */
            if (dup2(m->player_pipes[i][1], STDOUT_FILENO) == -1) die("dup2");
            close(m->player_pipes[i][1]);

            /* ejecutar el jugador pasando width y height */
            execlp(m->player_paths[i], m->player_paths[i], w, h, NULL);
            die("execlp jugador");  /* solo llega acá si execlp falla */
        }

        /* padre: guardar pid y cerrar extremo de escritura */
        m->player_pids[i] = pid;
        close(m->player_pipes[i][1]);
        m->player_pipes[i][1] = -1;
    }

    /* ── fork vista ── */
    m->view_pid = -1;
    if (m->view_path != NULL) {
        pid_t pid = fork();
        if (pid < 0) die("fork vista");

        if (pid == 0) {
            /* la vista no usa pipes, solo conecta las shared memories */
            execlp(m->view_path, m->view_path, w, h, NULL);
            die("execlp vista");
        }

        m->view_pid = pid;
    }
}

/* ── cleanup ─────────────────────────────────────────────────────────────── */

static void cleanup(Master *m) {
    if (m->sync != NULL) {
        GameSync *s = m->sync;
        sem_destroy(&s->view_notify);
        sem_destroy(&s->view_done);
        sem_destroy(&s->no_writer);
        sem_destroy(&s->state_mutex);
        sem_destroy(&s->readers_mutex);
        for (int i = 0; i < m->player_count; i++)
            sem_destroy(&s->player_ack[i]);
        munmap(m->sync, sizeof(GameSync));
        shm_unlink(SHM_SYNC_NAME);
    }
    if (m->gs != NULL) {
        munmap(m->gs, m->state_size);
        shm_unlink(SHM_STATE_NAME);
    }
    /* cerrar pipes que quedaron abiertos */
    for (int i = 0; i < m->player_count; i++) {
        if (m->player_pipes[i][0] != -1) close(m->player_pipes[i][0]);
    }
}
static void initPlayers(Master *m) {
    GameState *gs = m->gs;
    int n = m->player_count;

    /* cuántas columnas y filas de sectores necesitamos
     * por ejemplo 4 jugadores → cols=2, rows=2
     *             3 jugadores → cols=3, rows=1
     *             9 jugadores → cols=3, rows=3
     */
    int cols = 1;
    int rows = 1;
    while (cols * rows < n) {
        if (cols <= rows) cols++;
        else              rows++;
    }

    /* ancho y alto de cada sector */
    int sector_w = m->width  / cols;
    int sector_h = m->height / rows;

    for (int i = 0; i < n; i++) {
        int col = i % cols;
        int row = i / cols;

        /* centro del sector */
        int x = col * sector_w + sector_w / 2;
        int y = row * sector_h + sector_h / 2;

        gs->players[i].x = (unsigned short)x;
        gs->players[i].y = (unsigned short)y;
        gs->players[i].score         = 0;
        gs->players[i].valid_moves   = 0;
        gs->players[i].invalid_moves = 0;
        gs->players[i].blocked       = false;
        gs->players[i].pid           = m->player_pids[i];

        /* nombre del jugador: "player_0", "player_1", etc */
        snprintf(gs->players[i].name, sizeof(gs->players[i].name),
                 "player_%d", i);

        /* la celda inicial no da recompensa, la marcamos como capturada */
        *board_cell(gs, (unsigned short)x, (unsigned short)y) = (char)(-i);
    }
}
static void initPlayers(Master *m) {
    GameState *gs = m->gs;
    int n = m->player_count;

    /* cuántas columnas y filas de sectores necesitamos
     * por ejemplo 4 jugadores → cols=2, rows=2
     *             3 jugadores → cols=3, rows=1
     *             9 jugadores → cols=3, rows=3
     */
    int cols = 1;
    int rows = 1;
    while (cols * rows < n) {
        if (cols <= rows) cols++;
        else              rows++;
    }

    /* ancho y alto de cada sector */
    int sector_w = m->width  / cols;
    int sector_h = m->height / rows;

    for (int i = 0; i < n; i++) {
        int col = i % cols;
        int row = i / cols;

        /* centro del sector */
        int x = col * sector_w + sector_w / 2;
        int y = row * sector_h + sector_h / 2;

        gs->players[i].x = (unsigned short)x;
        gs->players[i].y = (unsigned short)y;
        gs->players[i].score         = 0;
        gs->players[i].valid_moves   = 0;
        gs->players[i].invalid_moves = 0;
        gs->players[i].blocked       = false;
        gs->players[i].pid           = m->player_pids[i];

        /* nombre del jugador: "player_0", "player_1", etc */
        snprintf(gs->players[i].name, sizeof(gs->players[i].name),
                 "player_%d", i);

        /* la celda inicial no da recompensa, la marcamos como capturada */
        *board_cell(gs, (unsigned short)x, (unsigned short)y) = (char)(-i);
    }
}



/* ── main ─────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    Master m;
    memset(&m, 0, sizeof(m));

    /* inicializar pipe fds a -1 para detectar los no abiertos en cleanup */
    for (int i = 0; i < MAX_PLAYERS; i++)
        m.player_pipes[i][0] = m.player_pipes[i][1] = -1;

    parse_args(argc, argv, &m);
    initGame(&m);       /* 1. crear shared memories y llenar tablero */
    initSem(&m);        /* 2. inicializar semáforos */
    initCanales(&m);    /* 3. crear pipes de jugadores */
    initProcesses(&m);  /* 4. fork vista y jugadores */
    initPlayers(&m);    /* 5. inicializar posiciones de jugadores */
    /* TODO: game_loop(&m) — próximo paso */
    game_loop(&m);

    cleanup(&m);
    return EXIT_SUCCESS;
}