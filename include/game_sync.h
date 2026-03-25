#ifndef GAME_SYNC_H
#define GAME_SYNC_H
 
#include <semaphore.h>
#include "game_state.h"  /* MAX_PLAYERS */
 
#define SHM_SYNC_NAME "/game_sync"

/*
 * Estructura de sincronización almacenada en /game_sync.
 *
 * Semáforos entre master y vista (señalización bidireccional simple):
 *   view_notify (A): master -> vista: "hay cambios, imprimí"
 *   view_done   (B): vista -> master: "terminé de imprimir"
 *
 * Semáforos para el patrón lectores-escritores (sin inanición del escritor):
 *   no_writer   (C): bloquea nuevos lectores si el escritor espera
 *   state_mutex (D): acceso exclusivo al estado (para el escritor)
 *   readers_mutex(E): mutex para la variable readers_count
 *   readers_count(F): cantidad de jugadores leyendo el estado ahora mismo
 *
 * Un semáforo por jugador:
 *   player_ack[i] (G[i]): master -> jugador[i]: "tu movimiento fue procesado,
 *                          podés enviar el siguiente"
 */

typedef struct {
    sem_t view_notify;           /* A  master->vista*/
    sem_t view_done;             /* B  vista->master */
    sem_t no_writer;             /* C */
    sem_t state_mutex;           /* D */
    sem_t readers_mutex;         /* E */
    unsigned int readers_count;  /* F */
    sem_t player_ack[MAX_PLAYERS]; /* G */
} GameSync;

void initSem(Master *m){
    GameSync *s = m->sync;
 
    /*
     * sem_init(sem, pshared, valor)
     * pshared = 1 → el semáforo es compartido entre procesos (no solo threads)
     *               esto es obligatorio porque vive en shared memory
     */
     //inicializamos los semaforos
     //el semaforo A y B se inicializan en 0 porq lo que hace el A es mandar msj de master a visat cuando hay para imrpimir
     // y el semaforo B cuando ya se imprimio--> (borrar esto dsp) master quiere imprimir y hace up(A), la vista consume esto y hace down(A), termina y hace up(B) y master consume y hace down(B)
    if (sem_init(&s->view_notify,    1, 0) == -1) die("sem_init view_notify");
    if (sem_init(&s->view_done,      1, 0) == -1) die("sem_init view_done");
     // los semaforos C, D y E se inicializan en 1. D es como el candado de Game state, E es el q si ambos jugadores quieren acceder a E al mismo tiempo solo lo deja a uno
    if (sem_init(&s->no_writer,      1, 1) == -1) die("sem_init no_writer");
    if (sem_init(&s->state_mutex,    1, 1) == -1) die("sem_init state_mutex");
    if (sem_init(&s->readers_mutex,  1, 1) == -1) die("sem_init readers_mutex");
    //al inicio no hay nadie leyendo el estado
    s->readers_count = 0;
 
    for (int i = 0; i < m->player_count; i++) {
        if (sem_init(&s->player_ack[i], 1, 0) == -1) die("sem_init player_ack");
    }
}
 
#endif 