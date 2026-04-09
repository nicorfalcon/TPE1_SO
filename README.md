# TPE1 — ChompChamps
### Sistemas Operativos · Inter Process Communication

---

## Decisiones de diseño

### Arquitectura general
El proyecto está dividido en tres binarios independientes (`master`, `player`, `vista`) que se comunican exclusivamente a través de los mecanismos de IPC provistos por POSIX: memoria compartida (`shm_open`), semáforos anónimos (`sem_init`) y pipes anónimos (`pipe`).

### Sincronización
Para la sincronización entre el master (escritor) y los jugadores (lectores) se implementó el problema clásico de **lectores-escritores** en su variante sin inanición del escritor, usando los semáforos `no_writer` (C), `state_mutex` (D) y `readers_mutex` (E) junto con el contador `readers_count` (F). Esto garantiza que el master pueda escribir el estado sin esperar indefinidamente a que los jugadores terminen de leer.

La sincronización entre el master y la vista es más simple: señalización bidireccional con dos semáforos (`view_notify` y `view_done`), dado que la vista nunca modifica el estado.

### Round-robin
El master atiende las solicitudes de los jugadores siguiendo una política round-robin mediante la variable `last_served`, que recuerda el último jugador atendido y continúa desde el siguiente en la próxima iteración. Esto evita favorecer sistemáticamente a un mismo jugador.

### Distribución inicial de jugadores
Los jugadores son ubicados en el centro de sectores del tablero divididos en una grilla. Por ejemplo, con 4 jugadores el tablero se divide en 4 sectores de 2x2 y cada jugador arranca en el centro de su sector. Esto garantiza una distribución determinística con margen de movimiento similar para todos.

### Inteligencia artificial del jugador
El jugador implementa una IA basada en **flood fill**: para cada dirección válida, simula la captura de esa celda en una copia del tablero y calcula cuántas celdas libres quedarían accesibles desde esa nueva posición. Elige el movimiento que maximiza el territorio alcanzable. En caso de empate, prioriza la celda con mayor recompensa inmediata. Esto evita que el jugador se encierre en zonas sin salida.

---

## Instrucciones de compilación y ejecución

### Requisitos
El proyecto debe compilarse y ejecutarse dentro del contenedor de la cátedra:
```bash
docker pull agodio/itba-so-multiarch:3.1
```

### Compilación
Desde la raíz del proyecto, dentro del contenedor:
```bash
make
```
Los binarios se generan en la carpeta `bin/`.

Para limpiar los binarios:
```bash
make clean
```

### Ejecución
```bash
./bin/master -p ./bin/player [./bin/player ...] [-v ./bin/vista] [-w W] [-h H] [-d delay_ms] [-t timeout_s] [-s seed]
```

Ejemplo con 3 jugadores, vista y tablero de 20x20:
```bash
./bin/master -p ./bin/player ./bin/player ./bin/player -v ./bin/vista -w 20 -h 20
```

---

## Rutas relativas para el torneo

- **Vista:** `./bin/vista`
- **Jugador:** `./bin/player`

---

## Limitaciones

- El jugador no implementa ninguna estrategia cooperativa ni anticipación de movimientos de otros jugadores. La IA toma decisiones basadas únicamente en el estado actual del tablero sin lookahead multi-paso.
- La vista limpia la pantalla con secuencias ANSI (`\033[2J`), por lo que puede no funcionar correctamente en terminales que no soporten estos códigos.
- No se implementaron los chequeos de consistencia de la estructura `GameSync` que realiza el master provisto por la cátedra (la opción `-i`).

---

## Problemas encontrados y soluciones

- **Entendimiento de semáforos:** Al principio costó comprender el protocolo de lectores-escritores, en particular el rol del semáforo `no_writer` para evitar la inanición del escritor. Se aclaró con un ejemplo visto en clase y reforzando la teoría con diagramas de estados de cada semáforo.

- **`usleep` no disponible con `-std=c11`:** La función `usleep` no está disponible bajo el estándar C11 estricto. Se resolvió agregando `-D_DEFAULT_SOURCE` a los flags de compilación en el Makefile.

---

## Citas y uso de IA

Se utilizaron herramientas de inteligencia artificial (**Claude** de Anthropic y **ChatGPT**) como apoyo para:
- Entender los conceptos de IPC y sincronización (semáforos, shared memory, pipes).
- Clarificar el funcionamiento del protocolo lectores-escritores.
- Asistencia en la implementación y debugging del código.

Todo el código fue revisado, comprendido y adaptado por los integrantes del grupo. 