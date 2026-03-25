#include "include/game_state.h"
#include "include/game_sync.h"
#include "include/protocol.h"

#define DEFAULT_WIDTH   10
#define DEFAULT_HEIGHT  10
#define DEFAULT_DELAY   200
#define DEFAULT_TIMEOUT 10
#define MIN_WIDTH       10
#define MIN_HEIGHT      10

typedef struct {
    int width;
    int height;
    int delay_ms;
    int timeout_s;
    unsigned int seed;
    char *view_path;
    char *player_paths[MAX_PLAYERS];
    int player_pipes[MAX_PLAYERS];
    int player_count;
    GameState *gs;
    size_t     state_size;
    GameSync  *sync;
} master;

static void parse_args(int argc, char *argv[], Master *m) {
    // super imporatnte entender, aca el usuario pone en la consola y se pushean los argumentos cuando ponemos a correr el programa(tipo arqui)
    if(m==NULL){
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

int initGame(master *m){

    //INICIALIZO MEMORIA COMPARTIDA GAME SYNC
    int fdSync = shm_open("/game_sync", O_CREAT | O_RDWR, 0666); 
    //“quiero una shared memory llamada /game_state”
    //O_CREAT = si no existe, crearla
    //O_RDWR = la voy a leer y escribir
    int sync_size = sizeof(m->sync) ////verrrr
    ftruncate(fdSync, sync_size); // asignarle size q va a tener memoria compartida 
    m->sync = mmap(NULL, sync_size, PROT_READ | PROT_WRITE, MAP_SHARED, fdSync, 0); // da permisos para leer y escribir en la memoria compartida y esta diciendo q otros procesos puedan acceder a ella y 0 es el offset


    //INICIALIZO MEMORIA COMPARTIDA GAME STATE
    int fdState = shm_open("/game_state", O_CREAT | O_RDWR, 0666); 
    m->state_size = game_state_size(m->width, m->height);
    ftruncate(fdState, m->state_size);
    m->gs = mmap(NULL, m->state_size, PROT_READ | PROT_WRITE, MAP_SHARED, fdState, 0);
       
}
int main()
{
    Master m;
    memset(&m, 0, sizeof(m));
 
    parse_args(argc, argv, &m); //relleno mi master struct con los argumentos que me pasan por linea de comando
 
    initGame(&m); //inicializamos las memorias compartidas
    initSem(&m); //inicializamos los semaforos
    initCanales(&m): //inicializamos los canales de comunicacion con los jugadores


    return EXIT_SUCCESS;
}

void initCanales(Master *m){
    //visto en clase
    //int fd[2]
    //pipe(fd) crea un par de descriptores de archivo conectados entre sí: fd[0] para lectura y fd[1] para escritura
    int cantPlayers = m->player_count;
    for (i = 0; i < cantPlayers; i++) {
        pid = fork();

        if (pid == 0) {
            // hijo jugador i

            // cerrar todo salvo player_pipes[i][1]
            for (j = 0; j < cantPlayers; j++) {
                if (j == i) {
                    close(player_pipes[j][0]);   // no leo mi pipe
                } else {
                    close(player_pipes[j][0]);   // no uso pipe ajeno
                    close(player_pipes[j][1]);   // no uso pipe ajeno
                }
            }

            // me queda solo player_pipes[i][1]
            // después vendrá dup2(...) y exec(...)
        }
    }

    // master: cerrar todos los [1], quedarse con todos los [0]
    for (i = 0; i < cantPlayers; i++) {
        close(player_pipes[i][1]);
    }
        for(int i=0; i<m->player_count; i++){
            if (pipe(m->player_pipes[i]) == -1) {
                perror("pipe");
                exit(EXIT_FAILURE);
            }

        }
    }

// /*/* ── helpers ─────────────────────────────────────────────────────────────── */
 
// static void die(const char *msg) {
//     perror(msg);
//     exit(EXIT_FAILURE);
// }

// /* ── init_game_state ─────────────────────────────────────────────────────
//  *
//  * Crea la shared memory /game_state y la inicializa:
//  *   - dimensiones del tablero
//  *   - cantidad de jugadores
//  *   - game_over = false
//  *   - tablero lleno con recompensas aleatorias 1..9 usando la seed
//  */
// static void init_game_state(Master *m) {
//     /* tamaño total = struct fijo + width*height bytes del tablero */
//     m->state_size = game_state_size((unsigned short)m->width,
//                                     (unsigned short)m->height);
 
//     /*
//      * O_CREAT | O_RDWR  → crear si no existe, abrir para lectura/escritura
//      * O_TRUNC           → si ya existía (run anterior), resetearla
//      * 0666              → permisos del objeto shm
//      */
//     int fd = shm_open(SHM_STATE_NAME, O_CREAT | O_RDWR | O_TRUNC, 0666);
//     if (fd == -1) die("shm_open /game_state");
 
//     /* darle el tamaño correcto (por defecto tiene tamaño 0) */
//     if (ftruncate(fd, (off_t)m->state_size) == -1) die("ftruncate /game_state");
 
//     /* mapear en nuestro espacio de memoria */
//     m->gs = mmap(NULL, m->state_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
//     if (m->gs == MAP_FAILED) die("mmap /game_state");
 
//     /* el fd ya no es necesario una vez mapeado */
//     close(fd);
 
//     /* inicializar campos del struct */
//     m->gs->width        = (unsigned short)m->width;
//     m->gs->height       = (unsigned short)m->height;
//     m->gs->player_count = (unsigned char)m->player_count;
//     m->gs->game_over    = false;
 
//     /*
//      * Llenar el tablero con recompensas aleatorias entre 1 y 9.
//      * Usamos la seed para que sea determinístico: misma seed = mismo tablero.
//      */
//     srand(m->seed);
//     int total = m->width * m->height;
//     for (int i = 0; i < total; i++)
//         m->gs->board[i] = (char)(rand() % 9 + 1);
 
//     printf("shared memory /game_state creada (%zu bytes)\n", m->state_size);
// }
 
// /* ── cleanup parcial (por ahora solo game_state) ─────────────────────────── */
 
// static void cleanup(Master *m) {
//     if (m->gs != NULL) {
//         munmap(m->gs, m->state_size);
//         shm_unlink(SHM_STATE_NAME);
//     }
// }
 

// /* ── init_game_sync ──────────────────────────────────────────────────────
//  *
//  * Crea la shared memory /game_sync e inicializa todos los semáforos.
//  *
//  * Valores iniciales:
//  *   view_notify  (A) = 0  bloqueado: la vista espera hasta que el master avise
//  *   view_done    (B) = 0  bloqueado: el master espera hasta que la vista avise
//  *   no_writer    (C) = 1  libre: ningún escritor esperando todavía
//  *   state_mutex  (D) = 1  libre: el estado está disponible
//  *   readers_mutex(E) = 1  libre: el contador de lectores está disponible
//  *   readers_count(F) = 0  nadie leyendo todavía
//  *   player_ack[i](G) = 0  bloqueado: cada jugador espera el primer ack
//  */
// static void init_game_sync(Master *m) {
//     int fd = shm_open(SHM_SYNC_NAME, O_CREAT | O_RDWR | O_TRUNC, 0666);
//     if (fd == -1) die("shm_open /game_sync");
 
//     if (ftruncate(fd, sizeof(GameSync)) == -1) die("ftruncate /game_sync");
 
//     m->sync = mmap(NULL, sizeof(GameSync), PROT_READ | PROT_WRITE,
//                    MAP_SHARED, fd, 0);
//     if (m->sync == MAP_FAILED) die("mmap /game_sync");
//     close(fd);
 
//     GameSync *s = m->sync;
 
//     /*
//      * sem_init(sem, pshared, valor)
//      * pshared = 1 → el semáforo es compartido entre procesos (no solo threads)
//      *               esto es obligatorio porque vive en shared memory
//      */
//     if (sem_init(&s->view_notify,    1, 0) == -1) die("sem_init view_notify");
//     if (sem_init(&s->view_done,      1, 0) == -1) die("sem_init view_done");
//     if (sem_init(&s->no_writer,      1, 1) == -1) die("sem_init no_writer");
//     if (sem_init(&s->state_mutex,    1, 1) == -1) die("sem_init state_mutex");
//     if (sem_init(&s->readers_mutex,  1, 1) == -1) die("sem_init readers_mutex");
//     s->readers_count = 0;
 
//     for (int i = 0; i < m->player_count; i++) {
//         if (sem_init(&s->player_ack[i], 1, 0) == -1) die("sem_init player_ack");
//     }
 
//     printf("shared memory /game_sync creada (%zu bytes, %d semáforos)\n",
//            sizeof(GameSync), 5 + m->player_count);
// }
 
// /* ── cleanup ─────────────────────────────────────────────────────────────── */
 
// static void cleanup(Master *m) {
//     /* destruir semáforos antes de desmapear */
//     if (m->sync != NULL) {
//         GameSync *s = m->sync;
//         sem_destroy(&s->view_notify);
//         sem_destroy(&s->view_done);
//         sem_destroy(&s->no_writer);
//         sem_destroy(&s->state_mutex);
//         sem_destroy(&s->readers_mutex);
//         for (int i = 0; i < m->player_count; i++)
//             sem_destroy(&s->player_ack[i]);
//         munmap(m->sync, sizeof(GameSync));
//         shm_unlink(SHM_SYNC_NAME);
//     }
//     if (m->gs != NULL) {
//         munmap(m->gs, m->state_size);
//         shm_unlink(SHM_STATE_NAME);
//     }
// }
// /* ── main ─────────────────────────────────────────────────────────────────── */
// int main(int argc, char *argv[]) {
//     Master m;
//     memset(&m, 0, sizeof(m));
 
//     parse_args(argc, argv, &m);
 
//     init_game_state(&m);
//     init_game_sync(&m);
 
//     /* verificación: leer valor actual de cada semáforo */
//     printf("\nestado inicial de semáforos:\n");
//     int val;
//     sem_getvalue(&m.sync->view_notify,   &val); printf("  A view_notify:   %d\n", val);
//     sem_getvalue(&m.sync->view_done,     &val); printf("  B view_done:     %d\n", val);
//     sem_getvalue(&m.sync->no_writer,     &val); printf("  C no_writer:     %d\n", val);
//     sem_getvalue(&m.sync->state_mutex,   &val); printf("  D state_mutex:   %d\n", val);
//     sem_getvalue(&m.sync->readers_mutex, &val); printf("  E readers_mutex: %d\n", val);
//     printf("  F readers_count: %u\n", m.sync->readers_count);
//     for (int i = 0; i < m.player_count; i++) {
//         sem_getvalue(&m.sync->player_ack[i], &val);
//         printf("  G player_ack[%d]: %d\n", i, val);
//     }
 
//     cleanup(&m);
 
//     printf("\n(próximo paso: place_players)\n");
//     return EXIT_SUCCESS;
// }