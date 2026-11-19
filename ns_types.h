// Types partages. Dans un en-tete (et non dans le .ino) : l'IDE Arduino genere des prototypes de fonctions
// AVANT les types definis dans le .ino, ce qui provoque "'ScanResult' does not name a type".
#pragma once
#include <stdint.h>

enum AiPhase : uint8_t { AI_IDLE = 0, AI_RUNNING, AI_OK, AI_ERR };

struct ScanResult {
  char id[20], name[48], nameL[48], advice[200], adviceL[200];
  float weight, prot, carb, fat, kcal; bool corrected;
};
struct Targets { float kcal, prot, carb, fat; };
