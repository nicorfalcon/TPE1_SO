#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>
#include <semaphore.h>
#include <stdbool.h>

#include "include/game_state.h"
#include "include/game_sync.h"
#include "include/protocol.h"

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
    m->state_size = game_state_size((unsigned short)m->width,
                                    (unsigned short)m->height);

    int fdState = shm_open(SHM_STATE_NAME, O_CREAT | O_RDWR | O_TRUNC, 0666);
    if (fdState == -1) die("shm_open /game_state");
    if (ftruncate(fdState, (off_t)m->state_size) == -1) die("ftruncate /game_state");
    m->gs = mmap(NULL, m->state_size, PROT_READ | PROT_WRITE, MAP_SHARED, fdState, 0);
    if (m->gs == MAP_FAILED) die("mmap /game_state");
    close(fdState);

    m->gs->width        = (unsigned short)m->width;
    m->gs->height       = (unsigned short)m->height;
    m->gs->player_count = (unsigned char)m->player_count;
    m->gs->game_over    = false;

    srand(m->seed);
    int total = m->width * m->height;
    for (int i = 0; i < total; i++)
        m->gs->board[i] = (char)(rand() % 9 + 1);

    int fdSync = shm_open(SHM_SYNC_NAME, O_CREAT | O_RDWR | O_TRUNC, 0666);
    if (fdSync == -1) die("shm_open /game_sync");
    if (ftruncate(fdSync, sizeof(GameSync)) == -1) die("ftruncate /game_sync");
    m->sync = mmap(NULL, sizeof(GameSync), PROT_READ | PROT_WRITE, MAP_SHARED, fdSync, 0);
    if (m->sync == MAP_FAILED) die("mmap /game_sync");
    close(fdSync);
}

/* ── initSem ─────────────────────────────────────────────────────────────── */

static void initSem(Master *m) {
    GameSync *s = m->sync;
    if (sem_init(&s->view_notify,   1, 0) == -1) die("sem_init view_notify");
    if (sem_init(&s->view_done,     1, 0) == -1) die("sem_init view_done");
    if (sem_init(&s->no_writer,     1, 1) == -1) die("sem_init no_writer");
    if (sem_init(&s->state_mutex,   1, 1) == -1) die("sem_init state_mutex");
    if (sem_init(&s->readers_mutex, 1, 1) == -1) die("sem_init readers_mutex");
    s->readers_count = 0;
    for (int i = 0; i < m->player_count; i++)
        if (sem_init(&s->player_ack[i], 1, 0) == -1) die("sem_init player_ack");
}

/* ── initCanales ─────────────────────────────────────────────────────────── */

static void initCanales(Master *m) {
    for (int i = 0; i < m->player_count; i++)
        if (pipe(m->player_pipes[i]) == -1) die("pipe");
}

/* ── initProcesses ───────────────────────────────────────────────────────── */

static void initProcesses(Master *m) {
    char w[16], h[16];
    snprintf(w, sizeof(w), "%d", m->width);
    snprintf(h, sizeof(h), "%d", m->height);

    for (int i = 0; i < m->player_count; i++) {
        pid_t pid = fork();
        if (pid < 0) die("fork jugador");

        if (pid == 0) {
            for (int j = 0; j < m->player_count; j++) {
                if (j == i) {
                    close(m->player_pipes[j][0]);
                } else {
                    close(m->player_pipes[j][0]);
                    close(m->player_pipes[j][1]);
                }
            }
            if (dup2(m->player_pipes[i][1], STDOUT_FILENO) == -1) die("dup2");
            close(m->player_pipes[i][1]);
            execlp(m->player_paths[i], m->player_paths[i], w, h, NULL);
            die("execlp jugador");
        }

        m->player_pids[i] = pid;
        close(m->player_pipes[i][1]);
        m->player_pipes[i][1] = -1;
    }

    m->view_pid = -1;
    if (m->view_path != NULL) {
        pid_t pid = fork();
        if (pid < 0) die("fork vista");
        if (pid == 0) {
            execlp(m->view_path, m->view_path, w, h, NULL);
            die("execlp vista");
        }
        m->view_pid = pid;
    }
}

/* ── initPlayers ─────────────────────────────────────────────────────────── */

static void initPlayers(Master *m) {
    GameState *gs = m->gs;
    int n = m->player_count;

    int cols = 1, rows = 1;
    while (cols * rows < n) {
        if (cols <= rows) cols++;
        else              rows++;
    }

    int sector_w = m->width  / cols;
    int sector_h = m->height / rows;

    for (int i = 0; i < n; i++) {
        int col = i % cols;
        int row = i / cols;
        int x   = col * sector_w + sector_w / 2;
        int y   = row * sector_h + sector_h / 2;

        gs->players[i].x             = (unsigned short)x;
        gs->players[i].y             = (unsigned short)y;
        gs->players[i].score         = 0;
        gs->players[i].valid_moves   = 0;
        gs->players[i].invalid_moves = 0;
        gs->players[i].blocked       = false;
        gs->players[i].pid           = m->player_pids[i];
        snprintf(gs->players[i].name, sizeof(gs->players[i].name), "player_%d", i);
        *board_cell(gs, (unsigned short)x, (unsigned short)y) = (char)(-i);
    }
}

/* ── lock_write / unlock_write ───────────────────────────────────────────
 *
 * El master es el único escritor. Para escribir en el estado:
 *
 *   lock_write:
 *     sem_wait(C/no_writer)   ← anuncia intención: nuevos lectores se bloquean
 *     sem_wait(D/state_mutex) ← espera a que los lectores actuales terminen
 *
 *   unlock_write:
 *     sem_post(D/state_mutex) ← libera a cualquier lector esperando en D
 *     sem_post(C/no_writer)   ← permite que lleguen nuevos lectores
 */
static void lock_write(GameSync *sync) {
    if (sem_wait(&sync->no_writer)   == -1) die("sem_wait no_writer (write)");
    if (sem_wait(&sync->state_mutex) == -1) die("sem_wait state_mutex (write)");
}

static void unlock_write(GameSync *sync) {
    if (sem_post(&sync->state_mutex) == -1) die("sem_post state_mutex (write)");
    if (sem_post(&sync->no_writer)   == -1) die("sem_post no_writer (write)");
}

/* ── notify_view ─────────────────────────────────────────────────────────
 *
 * Avisa a la vista que hay cambios y espera a que termine de imprimir.
 * Solo se llama si hay vista activa (view_pid != -1).
 */
static void notify_view(Master *m) {
    if (m->view_pid == -1) return;
    if (sem_post(&m->sync->view_notify) == -1) die("sem_post view_notify");
    if (sem_wait(&m->sync->view_done)   == -1) die("sem_wait view_done");
}

/* ── is_cell_free ────────────────────────────────────────────────────────── */

static bool is_cell_free(const GameState *gs, int x, int y) {
    if (x < 0 || x >= gs->width)  return false;
    if (y < 0 || y >= gs->height) return false;
    return gs->board[y * gs->width + x] > 0;
}

/* ── update_blocked ──────────────────────────────────────────────────────
 *
 * Recorre las 8 direcciones del jugador i y actualiza su campo blocked.
 * Un jugador está bloqueado si ninguna celda adyacente está libre.
 */
static void update_blocked(GameState *gs, int idx) {
    Player *p = &gs->players[idx];
    for (int d = 0; d < DIRS; d++) {
        int nx = (int)p->x + DX[d];
        int ny = (int)p->y + DY[d];
        if (is_cell_free(gs, nx, ny)) {
            p->blocked = false;
            return;
        }
    }
    p->blocked = true;
}

/* ── apply_move ──────────────────────────────────────────────────────────
 *
 * Valida y aplica el movimiento del jugador idx.
 * Devuelve true si el movimiento fue válido, false si no.
 *
 * Movimiento válido:
 *   1. dirección en rango [0, DIRS)
 *   2. celda destino dentro del tablero
 *   3. celda destino libre (valor > 0)
 *
 * Si es válido:
 *   - captura la celda (board = -idx)
 *   - suma la recompensa al puntaje
 *   - actualiza posición
 *   - incrementa valid_moves
 *   - actualiza blocked de todos los jugadores
 *
 * Si es inválido:
 *   - incrementa invalid_moves
 */
static bool apply_move(GameState *gs, int idx, unsigned char dir) {
    Player *p = &gs->players[idx];

    if (dir >= DIRS) {
        p->invalid_moves++;
        return false;
    }

    int nx = (int)p->x + DX[dir];
    int ny = (int)p->y + DY[dir];

    if (!is_cell_free(gs, nx, ny)) {
        p->invalid_moves++;
        return false;
    }

    /* movimiento válido */
    char reward = gs->board[ny * gs->width + nx];
    gs->board[ny * gs->width + nx] = (char)(-idx);
    p->score += (unsigned int)reward;
    p->x = (unsigned short)nx;
    p->y = (unsigned short)ny;
    p->valid_moves++;

    /* actualizar blocked de todos (un movimiento puede desbloquear a otro) */
    for (int i = 0; i < gs->player_count; i++)
        update_blocked(gs, i);

    return true;
}

/* ── all_blocked ─────────────────────────────────────────────────────────── */

static bool all_blocked(const GameState *gs) {
    for (int i = 0; i < gs->player_count; i++)
        if (!gs->players[i].blocked) return false;
    return true;
}

/* ── wait_children ───────────────────────────────────────────────────────
 *
 * Espera a que terminen todos los hijos e imprime su estado de salida.
 * Para jugadores también imprime el puntaje final.
 */
static void wait_children(Master *m) {
    /* esperar jugadores */
    for (int i = 0; i < m->player_count; i++) {
        int status;
        pid_t pid = waitpid(m->player_pids[i], &status, 0);
        if (pid == -1) { perror("waitpid jugador"); continue; }

        fprintf(stderr, "jugador %d (pid %d): score=%u  ",
                i, (int)pid, m->gs->players[i].score);

        if (WIFEXITED(status))
            fprintf(stderr, "exit(%d)\n", WEXITSTATUS(status));
        else if (WIFSIGNALED(status))
            fprintf(stderr, "señal %d\n", WTERMSIG(status));
        else
            fprintf(stderr, "terminó de forma desconocida\n");
    }

    /* esperar vista */
    if (m->view_pid != -1) {
        int status;
        pid_t pid = waitpid(m->view_pid, &status, 0);
        if (pid == -1) { perror("waitpid vista"); return; }

        fprintf(stderr, "vista (pid %d): ", (int)pid);
        if (WIFEXITED(status))
            fprintf(stderr, "exit(%d)\n", WEXITSTATUS(status));
        else if (WIFSIGNALED(status))
            fprintf(stderr, "señal %d\n", WTERMSIG(status));
        else
            fprintf(stderr, "terminó de forma desconocida\n");
    }
}

/* ── game_loop ───────────────────────────────────────────────────────────
 *
 * Flujo general por iteración:
 *
 *   1. Armar fd_set con los pipes de jugadores no bloqueados y no EOF.
 *   2. select() con timeout calculado en base al tiempo restante.
 *   3. Si timeout → terminar juego.
 *   4. Si hay datos: recorrer en round-robin y atender UNO por vuelta.
 *      a. read() del pipe → 1 byte (dirección)
 *      b. Si EOF → marcar bloqueado, cerrar pipe.
 *      c. Si hay byte → lock_write, apply_move, unlock_write.
 *      d. sem_post(player_ack[i]) → el jugador puede enviar el siguiente.
 *      e. Si el movimiento fue válido → resetear timer.
 *   5. Notificar vista y esperar delay.
 *   6. Chequear fin: all_blocked o timeout.
 *
 * Round-robin: last_served guarda el último índice atendido.
 * La próxima búsqueda empieza desde (last_served+1) % n.
 */
static void game_loop(Master *m) {
    GameState *gs   = m->gs;
    GameSync  *sync = m->sync;
    int        n    = m->player_count;

    /* pipe_open[i]: true mientras el pipe del jugador i está activo */
    bool pipe_open[MAX_PLAYERS];
    for (int i = 0; i < n; i++) pipe_open[i] = true;

    /* máximo fd para select() */
    int max_fd = 0;
    for (int i = 0; i < n; i++)
        if (m->player_pipes[i][0] > max_fd)
            max_fd = m->player_pipes[i][0];

    /* dar largada: sem_post(player_ack[i]) para cada jugador */
    for (int i = 0; i < n; i++)
        if (sem_post(&sync->player_ack[i]) == -1) die("sem_post player_ack largada");

    /* timer: cuándo fue el último movimiento válido */
    struct timespec last_valid;
    clock_gettime(CLOCK_MONOTONIC, &last_valid);

    int last_served = n - 1; /* empezamos desde 0 en la primera vuelta */

    while (1) {
        /* ── chequeo de fin por all_blocked ── */
        if (all_blocked(gs)) break;

        /* ── armar fd_set ── */
        fd_set read_fds;
        FD_ZERO(&read_fds);
        int active = 0;
        for (int i = 0; i < n; i++) {
            if (pipe_open[i] && !gs->players[i].blocked) {
                FD_SET(m->player_pipes[i][0], &read_fds);
                active++;
            }
        }

        /* si no hay ningún pipe activo (todos bloqueados o cerrados), salir */
        if (active == 0) break;

        /* ── calcular tiempo restante para el timeout ── */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (double)(now.tv_sec  - last_valid.tv_sec) +
                         (double)(now.tv_nsec - last_valid.tv_nsec) / 1e9;
        double remaining = (double)m->timeout_s - elapsed;
        if (remaining <= 0.0) break; /* timeout ya superado */

        struct timeval tv;
        tv.tv_sec  = (time_t)remaining;
        tv.tv_usec = (suseconds_t)((remaining - (double)tv.tv_sec) * 1e6);

        /* ── select ── */
        int ret = select(max_fd + 1, &read_fds, NULL, NULL, &tv);
        if (ret < 0)  die("select");
        if (ret == 0) break; /* timeout */

        /* ── round-robin: atender uno por vuelta ── */
        for (int i = 0; i < n; i++) {
            int idx = (last_served + 1 + i) % n;

            if (!pipe_open[idx])           continue;
            if (gs->players[idx].blocked)  continue;
            if (!FD_ISSET(m->player_pipes[idx][0], &read_fds)) continue;

            /* leer 1 byte del pipe */
            unsigned char dir;
            ssize_t r = read(m->player_pipes[idx][0], &dir, 1);

            if (r == 0) {
                /* EOF: el jugador cerró su extremo del pipe → bloqueado */
                pipe_open[idx] = false;
                close(m->player_pipes[idx][0]);
                m->player_pipes[idx][0] = -1;

                lock_write(sync);
                gs->players[idx].blocked = true;
                unlock_write(sync);

                last_served = idx;
                break;
            }

            if (r < 0) die("read pipe jugador");

            /* aplicar movimiento dentro del lock de escritura */
            lock_write(sync);
            bool valid = apply_move(gs, idx, dir);
            unlock_write(sync);

            /* ack al jugador: ya puede enviar el siguiente movimiento */
            if (sem_post(&sync->player_ack[idx]) == -1) die("sem_post player_ack");

            /* si fue válido, resetear el timer */
            if (valid)
                clock_gettime(CLOCK_MONOTONIC, &last_valid);

            /* notificar vista y esperar que imprima */
            notify_view(m);

            /* delay entre impresiones */
            if (m->delay_ms > 0)
                usleep((useconds_t)m->delay_ms * 1000);

            last_served = idx;
            break; /* una sola solicitud por vuelta */
        }
    }

    /* ── fin del juego ── */
    lock_write(sync);
    gs->game_over = true;
    unlock_write(sync);

    /* notificar a la vista el estado final */
    notify_view(m);

    /*
     * Desbloquear a todos los jugadores que puedan estar esperando en
     * player_ack[i] para que puedan leer game_over y salir limpiamente.
     */
    for (int i = 0; i < n; i++)
        if (sem_post(&sync->player_ack[i]) == -1) die("sem_post player_ack fin");

    /* esperar a que todos los hijos terminen e imprimir su estado */
    wait_children(m);
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
    for (int i = 0; i < m->player_count; i++)
        if (m->player_pipes[i][0] != -1) close(m->player_pipes[i][0]);
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    Master m;
    memset(&m, 0, sizeof(m));

    for (int i = 0; i < MAX_PLAYERS; i++)
        m.player_pipes[i][0] = m.player_pipes[i][1] = -1;

    parse_args(argc, argv, &m);
    initGame(&m);       /* 1. crear shared memories y llenar tablero */
    initSem(&m);        /* 2. inicializar semáforos                  */
    initCanales(&m);    /* 3. crear pipes de jugadores               */
    initProcesses(&m);  /* 4. fork vista y jugadores                 */
    initPlayers(&m);    /* 5. posiciones iniciales + PIDs            */
    game_loop(&m);      /* 6. el juego                               */
    cleanup(&m);        /* 7. liberar recursos                       */
    return EXIT_SUCCESS;
}