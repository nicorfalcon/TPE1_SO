#define SHM_STATE  "/game_state"
   #define SHM_SYNC   "/game_sync"
   #define MAX_PLAYERS 9
   #define DIRS 8
   // Deltas para cada dirección [0-7] empezando arriba, sentido horario
   static const int DX[8] = { 0, 1, 1, 1, 0,-1,-1,-1};
   static const int DY[8] = {-1,-1, 0, 1, 1, 1, 0,-1};