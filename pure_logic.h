// Logique SANS dependance Arduino (testee sur PC : tests/run_all.sh).
// Prerequis : les seuils PLACE_G, REMOVE_G, STABLE_BAND_G, STABLE_MS, REMOVE_DEBOUNCE_MS doivent etre
// definis AVANT d'inclure ce fichier (ils le sont dans le .ino).
#pragma once
#if !defined(PLACE_G) || !defined(REMOVE_G) || !defined(STABLE_BAND_G) || !defined(STABLE_MS) || !defined(REMOVE_DEBOUNCE_MS)
#error "Definir PLACE_G, REMOVE_G, STABLE_BAND_G, STABLE_MS et REMOVE_DEBOUNCE_MS avant d'inclure pure_logic.h"
#endif
// Logique sans dependance Arduino : testee sur PC (tests/run_all.sh).
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#define EVT_NONE    0
#define EVT_TRIGGER 1
#define EVT_REMOVED 2
#define LINE_W      32

// ---- Filtre de masse : mediane(5) sur le brut -> grammes -> moyenne(4) ----
class WeightFilter {
 public:
  WeightFilter() { reset(); }
  void reset() { nMed_ = iMed_ = nAvg_ = iAvg_ = 0; }
  void setCalibration(int32_t offset, float cpg) {
    offset_ = offset; cpg_ = cpg; ok_ = (cpg != 0.0f) && isfinite(cpg); nAvg_ = iAvg_ = 0;
  }
  bool calibrated() const { return ok_; }
  bool pushRaw(int32_t raw, float* gramsOut) {
    med_[iMed_] = raw; iMed_ = (iMed_ + 1) % 5; if (nMed_ < 5) nMed_++;
    if (nMed_ < 5 || !ok_) return false;
    int32_t t[5]; memcpy(t, med_, sizeof(t));
    for (int i = 1; i < 5; i++) { int32_t v = t[i]; int j = i - 1; while (j >= 0 && t[j] > v) { t[j + 1] = t[j]; j--; } t[j + 1] = v; }
    float g = (float)((int64_t)t[2] - (int64_t)offset_) / cpg_;
    avg_[iAvg_] = g; iAvg_ = (iAvg_ + 1) % 4; if (nAvg_ < 4) nAvg_++;
    float s = 0; for (int i = 0; i < nAvg_; i++) s += avg_[i];
    *gramsOut = s / nAvg_;
    return true;
  }
 private:
  int32_t med_[5]; float avg_[4]; int nMed_, iMed_, nAvg_, iAvg_;
  int32_t offset_ = 0; float cpg_ = 0; bool ok_ = false;
};

// Types utilises dans des signatures de fonctions : definis AVANT la 1re fonction du fichier
// (l'IDE Arduino genere des prototypes automatiques inseres avant la 1re fonction).
struct Macros { float prot, carb, fat, kcal; bool corrected; };

// Moyenne tronquee (tare / calibration robustes aux pics). Trie a[] sur place. Exige n > 2*trim.
static inline float trimmedMean(int32_t* a, int n, int trim) {
  for (int i = 1; i < n; i++) { int32_t v = a[i]; int j = i - 1; while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; j--; } a[j + 1] = v; }
  double s = 0; for (int i = trim; i < n - trim; i++) s += a[i];
  return (float)(s / (n - 2 * trim));
}

// ---- Detection aliment pose / stabilise / retire ----
enum ScalePhase : uint8_t { PH_EMPTY = 0, PH_SETTLING, PH_HOLD };
struct ScaleLogic {
  ScalePhase phase = PH_EMPTY;
  uint32_t winStart = 0, belowSince = 0, lastNow = 0;
  float wMin = 0, wMax = 0; bool below = false;
  // Retourne EVT_TRIGGER (stable 2 s, une seule fois par pose), EVT_REMOVED (retire) ou EVT_NONE.
  int update(uint32_t now, float g) {
    lastNow = now;
    if (phase == PH_EMPTY) {
      if (g >= PLACE_G) { phase = PH_SETTLING; winStart = now; wMin = wMax = g; below = false; }
      return EVT_NONE;
    }
    if (g < REMOVE_G) {                                   // retrait (avec anti-rebond)
      if (!below) { below = true; belowSince = now; }
      if (now - belowSince >= REMOVE_DEBOUNCE_MS) { phase = PH_EMPTY; below = false; return EVT_REMOVED; }
      return EVT_NONE;
    }
    below = false;
    if (phase == PH_SETTLING) {
      if (g < wMin) wMin = g;
      if (g > wMax) wMax = g;
      if ((wMax - wMin) >= STABLE_BAND_G) { winStart = now; wMin = wMax = g; }   // trop de variation : on repart
      else if (now - winStart >= STABLE_MS) { phase = PH_HOLD; return EVT_TRIGGER; }
    }
    return EVT_NONE;
  }
  float progress() const {
    if (phase != PH_SETTLING) return phase == PH_HOLD ? 1.0f : 0.0f;
    float p = (float)(lastNow - winStart) / (float)STABLE_MS; return p < 0 ? 0 : (p > 1 ? 1 : p);
  }
};

// ---- Texte UTF-8 -> Latin-1 (police des ecrans), caracteres francais conserves ----
static inline size_t utf8ToLatin1(const char* in, char* out, size_t cap) {
  size_t o = 0; const unsigned char* p = (const unsigned char*)in;
  while (*p && o + 4 < cap) {
    unsigned c = *p;
    if (c < 0x20) { out[o++] = ' '; p++; }
    else if (c < 0x80) { out[o++] = (char)c; p++; }
    else if ((c & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
      unsigned cp = ((c & 0x1F) << 6) | (p[1] & 0x3F); p += 2;
      if (cp <= 0xFF) out[o++] = (char)cp;
      else if (cp == 0x153) { out[o++] = 'o'; out[o++] = 'e'; }
      else if (cp == 0x152) { out[o++] = 'O'; out[o++] = 'E'; }
    } else if ((c & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
      unsigned cp = ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3;
      switch (cp) {
        case 0x2018: case 0x2019: out[o++] = '\''; break;
        case 0x201C: case 0x201D: out[o++] = '"'; break;
        case 0x2013: case 0x2014: out[o++] = '-'; break;
        case 0x2022: out[o++] = '*'; break;
        case 0x2026: out[o++] = '.'; out[o++] = '.'; out[o++] = '.'; break;
        case 0x20AC: out[o++] = 'E'; out[o++] = 'U'; out[o++] = 'R'; break;
        default: break;                                   // autres symboles : ignores
      }
    } else if ((c & 0xF8) == 0xF0) { p += 4; }             // emoji : ignores
    else p++;
  }
  out[o] = 0; return o;
}

// ---- Latin-1 -> ASCII pur (police du LCD 1602 : pas d'accents) : e/e/e -> e, a -> a, c -> c ... Modifie s sur place ----
static inline void latin1ToAscii(char* s) {
  static const char map[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPsaaaaaaaceeeeiiiidnooooo/ouuuuypy";   // 0xC0..0xFF
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (c >= 0xC0) *s = map[c - 0xC0];
    else if (c >= 0x80) *s = ' ';
  }
}

// ---- Retour a la ligne (mots) pour l'ecran. Retourne le nombre de lignes ----
static inline int wrapText(const char* s, int maxChars, char (*lines)[LINE_W], int maxLines) {
  if (maxChars > LINE_W - 1) maxChars = LINE_W - 1;
  int n = 0, len = 0; char cur[LINE_W]; cur[0] = 0; bool more = false;
  const char* p = s;
  auto commit = [&]() { cur[len] = 0; if (n < maxLines) { memcpy(lines[n], cur, len + 1); n++; } else more = true; len = 0; cur[0] = 0; };
  while (*p) {
    while (*p == ' ') p++;
    if (!*p) break;
    int wl = 0; while (p[wl] && p[wl] != ' ') wl++;
    const char* w = p; p += wl;
    while (wl > maxChars) {                               // mot plus long qu'une ligne : coupe
      if (len > 0) commit();
      if (n >= maxLines) { more = true; break; }
      memcpy(cur, w, maxChars); len = maxChars; commit(); w += maxChars; wl -= maxChars;
    }
    if (n >= maxLines) { more = true; break; }
    if (wl == 0) continue;
    if (len == 0) { memcpy(cur, w, wl); len = wl; }
    else if (len + 1 + wl <= maxChars) { cur[len++] = ' '; memcpy(cur + len, w, wl); len += wl; }
    else { commit(); if (n >= maxLines) { more = true; break; } memcpy(cur, w, wl); len = wl; }
  }
  if (len > 0) commit();
  if (more && n > 0) {                                    // texte tronque : "..."
    int l = (int)strlen(lines[n - 1]);
    if (l > maxChars - 3) l = maxChars - 3;
    if (l < 0) l = 0;
    memcpy(lines[n - 1] + l, "...", 4);
  }
  return n;
}

// ---- Extraction du 1er objet JSON equilibre dans un texte (le modele peut ajouter du texte/```) ----
static inline size_t extractJsonObject(const char* in, char* out, size_t cap) {
  const char* s = strchr(in, '{'); if (!s) return 0;
  int depth = 0; bool inStr = false, esc = false;
  for (const char* p = s; *p; p++) {
    char c = *p;
    if (inStr) { if (esc) esc = false; else if (c == '\\') esc = true; else if (c == '"') inStr = false; continue; }
    if (c == '"') { inStr = true; continue; }
    if (c == '{') depth++;
    else if (c == '}') { if (--depth == 0) { size_t n = (size_t)(p - s) + 1; if (n + 1 > cap) return 0; memcpy(out, s, n); out[n] = 0; return n; } }
  }
  return 0;
}

// ---- Copie brute (guillemets inclus) de la valeur chaine d'une cle JSON : "key":"...." ----
// Sert quand la reponse HTTP est tronquee APRES le champ voulu (ex. "response" avant le gros tableau "context" d'Ollama).
// Le resultat est ensuite decode (echappements) par un deserializeJson() sur ce seul fragment.
static inline size_t extractRawJsonString(const char* in, const char* key, char* out, size_t cap) {
  char pat[24]; int pl = snprintf(pat, sizeof(pat), "\"%s\"", key);
  if (pl <= 0 || pl >= (int)sizeof(pat)) return 0;
  const char* p = strstr(in, pat); if (!p) return 0;
  p += pl; while (*p == ' ' || *p == ':') p++;
  if (*p != '"') return 0;
  const char* s = p; bool esc = false;
  for (p++; *p; p++) {
    if (esc) esc = false;
    else if (*p == '\\') esc = true;
    else if (*p == '"') { size_t n = (size_t)(p - s) + 1; if (n + 1 > cap) return 0; memcpy(out, s, n); out[n] = 0; return n; }
  }
  return 0;                                               // chaine non terminee (tronquee)
}

// ---- Reponse HTTP brute : code + pointeur de corps (0 = en-tetes incomplets, -1 = invalide) ----
static inline int parseHttpResponse(const char* buf, const char** body, bool* chunked) {
  const char* eoh = strstr(buf, "\r\n\r\n"); if (!eoh) return 0;
  if (strncmp(buf, "HTTP/", 5) != 0) return -1;
  const char* sp = strchr(buf, ' '); if (!sp || sp > eoh) return -1;
  *body = eoh + 4;
  *chunked = false;
  for (const char* p = buf; p < eoh; p++)
    if ((*p == '\n') && strncasecmp(p + 1, "Transfer-Encoding:", 18) == 0) {
      const char* e = strstr(p, "\r\n"); const char* c = strstr(p, "chunked"); if (c && (!e || c < e)) *chunked = true;
    }
  return atoi(sp + 1);
}

// Decode un corps "chunked" sur place ; retourne la nouvelle longueur.
static inline size_t dechunk(char* b, size_t len) {
  size_t r = 0, w = 0;
  while (r < len) {
    char* e; unsigned long sz = strtoul(b + r, &e, 16);
    while (e < b + len && *e != '\n') e++;
    if (e >= b + len) break;
    r = (size_t)(e - b) + 1;
    if (sz == 0) break;
    if (r + sz > len) sz = len - r;
    memmove(b + w, b + r, sz); w += sz; r += sz;
    if (r < len && b[r] == '\r') r++;
    if (r < len && b[r] == '\n') r++;
  }
  b[w] = 0; return w;
}

// ---- "25 g", "12,5", "~30" -> nombre ----
static inline bool numberFromText(const char* s, float* out) {
  while (*s && !((*s >= '0' && *s <= '9') || *s == '-' || *s == '.' || *s == ',')) s++;
  char t[24]; int n = 0;
  while (*s && n < 23 && ((*s >= '0' && *s <= '9') || *s == '.' || *s == ',' || (*s == '-' && n == 0))) { t[n++] = (*s == ',') ? '.' : *s; s++; }
  t[n] = 0; if (n == 0 || (n == 1 && (t[0] == '-' || t[0] == '.'))) return false;
  *out = (float)atof(t); return true;
}

// ---- Garde-fous sur les valeurs du modele ----
static inline void sanitizeMacros(Macros* m, float weightG) {
  m->corrected = false;
  if (!isfinite(m->prot) || m->prot < 0) m->prot = 0;
  if (!isfinite(m->carb) || m->carb < 0) m->carb = 0;
  if (!isfinite(m->fat)  || m->fat  < 0) m->fat  = 0;
  if (!isfinite(m->kcal) || m->kcal < 0) m->kcal = 0;
  float sum = m->prot + m->carb + m->fat;
  if (weightG > 0 && sum > weightG * 1.02f) {             // plus de macros que de masse : impossible -> mise a l'echelle
    float k = weightG / sum; m->prot *= k; m->carb *= k; m->fat *= k; m->kcal = 0; m->corrected = true;
  }
  float at = 4 * m->prot + 4 * m->carb + 9 * m->fat;
  if (at > 1.0f && (m->kcal <= 0 || fabsf(m->kcal - at) / at > 0.35f)) { m->kcal = at; m->corrected = true; }
}
