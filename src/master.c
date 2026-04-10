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

#include "game_state.h"
#include "game_sync.h"
#include "protocol.h"

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

/* ── init_master_defaults ───────────────────────────────────────────────── */

static void init_master_defaults(Master *m) {
    m->width        = DEFAULT_WIDTH;
    m->height       = DEFAULT_HEIGHT;
    m->delay_ms     = DEFAULT_DELAY;
    m->timeout_s    = DEFAULT_TIMEOUT;
    m->seed         = (unsigned int)time(NULL);
    m->view_path    = NULL;
    m->player_count = 0;
}

/* ── parse_players ──────────────────────────────────────────────────────── */

static void parse_players(int *i, int argc, char *argv[], Master *m) {
    while (*i + 1 < argc && argv[*i + 1][0] != '-') {
        if (m->player_count >= MAX_PLAYERS) {
            fprintf(stderr, "error: máximo %d jugadores\n", MAX_PLAYERS);
            exit(EXIT_FAILURE);
        }
        m->player_paths[m->player_count++] = argv[++(*i)];
    }
}

/* ── validate_args ──────────────────────────────────────────────────────── */

static void validate_args(const Master *m) {
    if (m->player_count == 0) {
        fprintf(stderr,
            "uso: master -p jugador1 [jugador2 ...] "
            "[-w W] [-h H] [-d delay_ms] [-t timeout_s] [-s seed] [-v vista]\n");
        exit(EXIT_FAILURE);
    }
}

/* ── parse_args ──────────────────────────────────────────────────────────── */

static void parse_args(int argc, char *argv[], Master *m) {
    if (m == NULL) {
        fprintf(stderr, "error: puntero nulo\n");
        exit(EXIT_FAILURE);
    }
    init_master_defaults(m);
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
            parse_players(&i, argc, argv, m);
        }
    }
    validate_args(m);
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

/* ── close_other_pipes ──────────────────────────────────────────────────── */

static void close_other_pipes(Master *m, int current) {
    for (int j = 0; j < m->player_count; j++) {
        close(m->player_pipes[j][0]);
        if (j != current)
            close(m->player_pipes[j][1]);
    }
}

/* ── spawn_player ───────────────────────────────────────────────────────── */

static void spawn_player(Master *m, int i, const char *w, const char *h) {
    pid_t pid = fork();
    if (pid < 0) die("fork jugador");
    if (pid == 0) {
        close_other_pipes(m, i);
        if (dup2(m->player_pipes[i][1], STDOUT_FILENO) == -1) die("dup2");
        close(m->player_pipes[i][1]);
        execlp(m->player_paths[i], m->player_paths[i], w, h, NULL);
        die("execlp jugador");
    }
    m->player_pids[i] = pid;
    close(m->player_pipes[i][1]);
    m->player_pipes[i][1] = -1;
}

/* ── spawn_view ─────────────────────────────────────────────────────────── */

static void spawn_view(Master *m, const char *w, const char *h) {
    if (m->view_path == NULL) { m->view_pid = -1; return; }
    pid_t pid = fork();
    if (pid < 0) die("fork vista");
    if (pid == 0) {
        execlp(m->view_path, m->view_path, w, h, NULL);
        die("execlp vista");
    }
    m->view_pid = pid;
}

/* ── initProcesses ───────────────────────────────────────────────────────── */

static void initProcesses(Master *m) {
    char w[16], h[16];
    snprintf(w, sizeof(w), "%d", m->width);
    snprintf(h, sizeof(h), "%d", m->height);
    for (int i = 0; i < m->player_count; i++)
        spawn_player(m, i, w, h);
    spawn_view(m, w, h);
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

/* ── lock_write / unlock_write ──────────────────────────────────────────── */

static void lock_write(GameSync *sync) {
    if (sem_wait(&sync->no_writer)   == -1) die("sem_wait no_writer (write)");
    if (sem_wait(&sync->state_mutex) == -1) die("sem_wait state_mutex (write)");
}

static void unlock_write(GameSync *sync) {
    if (sem_post(&sync->state_mutex) == -1) die("sem_post state_mutex (write)");
    if (sem_post(&sync->no_writer)   == -1) die("sem_post no_writer (write)");
}

/* ── notify_view ────────────────────────────────────────────────────────── */

static void notify_view(Master *m) {
    if (m->view_pid == -1) return;
    if (sem_post(&m->sync->view_notify) == -1) die("sem_post view_notify");
    if (sem_wait(&m->sync->view_done)   == -1) die("sem_wait view_done");
}

/* ── is_cell_free ───────────────────────────────────────────────────────── */

static bool is_cell_free(const GameState *gs, int x, int y) {
    if (x < 0 || x >= gs->width)  return false;
    if (y < 0 || y >= gs->height) return false;
    return gs->board[y * gs->width + x] > 0;
}

/* ── update_blocked ─────────────────────────────────────────────────────── */

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

/* ── apply_move ─────────────────────────────────────────────────────────── */

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

    char reward = gs->board[ny * gs->width + nx];
    gs->board[ny * gs->width + nx] = (char)(-idx);
    p->score += (unsigned int)reward;
    p->x = (unsigned short)nx;
    p->y = (unsigned short)ny;
    p->valid_moves++;

    for (int i = 0; i < gs->player_count; i++)
        update_blocked(gs, i);

    return true;
}

/* ── all_blocked ────────────────────────────────────────────────────────── */

static bool all_blocked(const GameState *gs) {
    for (int i = 0; i < gs->player_count; i++)
        if (!gs->players[i].blocked) return false;
    return true;
}

/* ── print_exit_status ──────────────────────────────────────────────────── */

static void print_exit_status(int status) {
    if (WIFEXITED(status))
        fprintf(stderr, "exit(%d)\n", WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
        fprintf(stderr, "señal %d\n", WTERMSIG(status));
    else
        fprintf(stderr, "terminó de forma desconocida\n");
}

/* ── wait_children ──────────────────────────────────────────────────────── */

static void wait_children(Master *m) {
    for (int i = 0; i < m->player_count; i++) {
        int status;
        pid_t pid = waitpid(m->player_pids[i], &status, 0);
        if (pid == -1) { perror("waitpid jugador"); continue; }
        fprintf(stderr, "jugador %d (pid %d): score=%u  ",
                i, (int)pid, m->gs->players[i].score);
        print_exit_status(status);
    }
    if (m->view_pid != -1) {
        int status;
        pid_t pid = waitpid(m->view_pid, &status, 0);
        if (pid == -1) { perror("waitpid vista"); return; }
        fprintf(stderr, "vista (pid %d): ", (int)pid);
        print_exit_status(status);
    }
}

/* ── init_game_loop ─────────────────────────────────────────────────────── */

static void init_game_loop(Master *m, bool *pipe_open, int *max_fd) {
    int n = m->player_count;
    *max_fd = 0;
    for (int i = 0; i < n; i++) {
        pipe_open[i] = true;
        if (m->player_pipes[i][0] > *max_fd)
            *max_fd = m->player_pipes[i][0];
    }
    for (int i = 0; i < n; i++)
        if (sem_post(&m->sync->player_ack[i]) == -1) die("sem_post player_ack largada");
}

/* ── build_read_fds ─────────────────────────────────────────────────────── */

static int build_read_fds(Master *m, const bool *pipe_open, fd_set *fds) {
    int n = m->player_count;
    FD_ZERO(fds);
    int active = 0;
    for (int i = 0; i < n; i++) {
        if (pipe_open[i] && !m->gs->players[i].blocked) {
            FD_SET(m->player_pipes[i][0], fds);
            active++;
        }
    }
    return active;
}

/* ── calc_remaining ─────────────────────────────────────────────────────── */

static double calc_remaining(const struct timespec *last_valid, int timeout_s) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed = (double)(now.tv_sec  - last_valid->tv_sec) +
                     (double)(now.tv_nsec - last_valid->tv_nsec) / 1e9;
    return (double)timeout_s - elapsed;
}

/* ── handle_eof ─────────────────────────────────────────────────────────── */

static void handle_eof(Master *m, int idx, bool *pipe_open) {
    pipe_open[idx] = false;
    close(m->player_pipes[idx][0]);
    m->player_pipes[idx][0] = -1;
    lock_write(m->sync);
    m->gs->players[idx].blocked = true;
    unlock_write(m->sync);
}

/* ── process_move ───────────────────────────────────────────────────────── */

static void process_move(Master *m, int idx, unsigned char dir,
                         struct timespec *last_valid) {
    lock_write(m->sync);
    bool valid = apply_move(m->gs, idx, dir);
    unlock_write(m->sync);
    if (sem_post(&m->sync->player_ack[idx]) == -1) die("sem_post player_ack");
    if (valid) clock_gettime(CLOCK_MONOTONIC, last_valid);
    notify_view(m);
    if (m->delay_ms > 0) {
        struct timespec delay_ts = {
            .tv_sec  = m->delay_ms / 1000,
            .tv_nsec = (m->delay_ms % 1000) * 1000000L
        };
        nanosleep(&delay_ts, NULL);
    }
}

/* ── serve_round_robin ──────────────────────────────────────────────────── */

static int serve_round_robin(Master *m, bool *pipe_open, fd_set *read_fds,
                             int last_served, struct timespec *last_valid) {
    int n = m->player_count;
    for (int i = 0; i < n; i++) {
        int idx = (last_served + 1 + i) % n;
        if (!pipe_open[idx] || m->gs->players[idx].blocked) continue;
        if (!FD_ISSET(m->player_pipes[idx][0], read_fds)) continue;
        unsigned char dir;
        ssize_t r = read(m->player_pipes[idx][0], &dir, 1);
        if (r == 0)       handle_eof(m, idx, pipe_open);
        else if (r < 0)   die("read pipe jugador");
        else              process_move(m, idx, dir, last_valid);
        return idx;
    }
    return last_served;
}

/* ── print_winner ───────────────────────────────────────────────────────── */

static void print_winner(const GameState *gs) {
    int winner = 0;
    bool empate = false;

    for (int i = 1; i < (int)gs->player_count; i++) {
        const Player *w = &gs->players[winner];
        const Player *c = &gs->players[i];

        if (c->score > w->score) {
            winner = i; empate = false;
        } else if (c->score == w->score) {
            if (c->valid_moves < w->valid_moves) {
                winner = i; empate = false;
            } else if (c->valid_moves == w->valid_moves) {
                if (c->invalid_moves < w->invalid_moves) {
                    winner = i; empate = false;
                } else if (c->invalid_moves == w->invalid_moves) {
                    empate = true;
                }
            }
        }
    }

    if (empate) {
        fprintf(stderr, "empate\n");
    } else {
        const Player *w = &gs->players[winner];
        fprintf(stderr, "ganador: %s  score=%u  valid=%u  invalid=%u\n",
                w->name, w->score, w->valid_moves, w->invalid_moves);
    }
}

/* ── finalize_game ──────────────────────────────────────────────────────── */

static void finalize_game(Master *m) {
    lock_write(m->sync);
    m->gs->game_over = true;
    unlock_write(m->sync);
    notify_view(m);
    for (int i = 0; i < m->player_count; i++)
        if (sem_post(&m->sync->player_ack[i]) == -1) die("sem_post player_ack fin");
    wait_children(m);
    print_winner(m->gs);
}

/* ── game_loop ──────────────────────────────────────────────────────────── */

static void game_loop(Master *m) {
    GameState *gs = m->gs;
    int n = m->player_count;
    bool pipe_open[MAX_PLAYERS];
    int max_fd;
    init_game_loop(m, pipe_open, &max_fd);
    struct timespec last_valid;
    clock_gettime(CLOCK_MONOTONIC, &last_valid);
    int last_served = n - 1;
    while (1) {
        if (all_blocked(gs)) break;
        fd_set read_fds;
        if (build_read_fds(m, pipe_open, &read_fds) == 0) break;
        double remaining = calc_remaining(&last_valid, m->timeout_s);
        if (remaining <= 0.0) break;
        struct timeval tv = {
            .tv_sec  = (time_t)remaining,
            .tv_usec = (suseconds_t)((remaining - (double)(time_t)remaining) * 1e6)
        };
        int ret = select(max_fd + 1, &read_fds, NULL, NULL, &tv);
        if (ret < 0) die("select");
        if (ret == 0) break;
        last_served = serve_round_robin(m, pipe_open, &read_fds,
                                        last_served, &last_valid);
    }
    finalize_game(m);
}

/* ── cleanup ────────────────────────────────────────────────────────────── */

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

/* ── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    Master m;
    memset(&m, 0, sizeof(m));

    for (int i = 0; i < MAX_PLAYERS; i++)
        m.player_pipes[i][0] = m.player_pipes[i][1] = -1;

    parse_args(argc, argv, &m);
    initGame(&m);
    initSem(&m);
    initCanales(&m);
    initProcesses(&m);
    initPlayers(&m);
    game_loop(&m);
    cleanup(&m);
    return EXIT_SUCCESS;
}
