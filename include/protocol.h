#ifndef PROTOCOL_H
#define PROTOCOL_H

#define DIRS 8

// Deltas para cada dirección [0-7] empezando arriba, sentido horario
static const int DX[8] = { 0, 1, 1, 1, 0,-1,-1,-1};
static const int DY[8] = {-1,-1, 0, 1, 1, 1, 0,-1};

#endif /* PROTOCOL_H */
