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
 *   no_writer    (C): bloquea nuevos lectores si el escritor espera
 *   state_mutex  (D): acceso exclusivo al estado (para el escritor)
 *   readers_mutex(E): mutex para la variable readers_count
 *   readers_count(F): cantidad de jugadores leyendo el estado ahora mismo
 *
 * Un semáforo por jugador:
 *   player_ack[i] (G[i]): master -> jugador[i]: "tu movimiento fue procesado,
 *                          podés enviar el siguiente"
 */
typedef struct {
    sem_t view_notify;             /* A  master -> vista */
    sem_t view_done;               /* B  vista -> master */
    sem_t no_writer;               /* C */
    sem_t state_mutex;             /* D */
    sem_t readers_mutex;           /* E */
    unsigned int readers_count;    /* F */
    sem_t player_ack[MAX_PLAYERS]; /* G */
} GameSync;
 
#endif /* GAME_SYNC_H */
 