/*
 * NutriScale Cloud - ESP32-S3 CAM + HX711 + OLED SSD1306 (Adafruit) + Ollama (vision) + Firebase RTDB
 * FICHIER UNIQUE : reglages, types, logique et programme sont dans ce seul .ino (plus de .h a copier).
 * Arduino IDE : carte "ESP32S3 Dev Module", Flash 16MB, PSRAM = "Disabled" (PSRAM non detectee sur cette carte ;
 * le code fonctionne aussi si une PSRAM est activee : tous les tampons passent par malloc()), USB CDC On Boot = Disabled.
 * Bibliotheques (Gestionnaire de bibliotheques) : "Adafruit SSD1306", "Adafruit GFX Library" (+ "Adafruit BusIO" si demandee)
 * et "ArduinoJson" 7.x. Le reste est fourni par le paquet esp32 3.x.
 *
 * IMPORTANT : les valeurs nutritionnelles viennent d'un modele de vision : ce sont des ESTIMATIONS approximatives.
 * Calibration : le facteur CALIBRATION_FACTOR est applique au 1er demarrage puis memorise ; la tare est faite
 * automatiquement (plateau VIDE) selon TARE_AT_BOOT. Commandes serie : tare, cal <masse_g>, calreset, raw, scan, status.
 */

// ---- Bibliotheques ----
#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_heap_caps.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "esp_camera.h"
#include "mbedtls/base64.h"
#if __has_include("esp_random.h")
#include "esp_random.h"
#endif

// ======================================================================================
// REGLAGES (anciennement user_config.h) : c'est ICI qu'on modifie Wi-Fi, Firebase, Ollama, seuils et broches
// ======================================================================================
#define WIFI_SSID        "."
#define WIFI_PASS        "houssam200"
#define FIREBASE_DB_URL  "https://balance-nutrition-ia-default-rtdb.europe-west1.firebasedatabase.app"  // sans '/' final
#define SCALE_ID         "Rw5pNdbTNdAdGMpurAFH1auBG7kDNbGl"   // >= 20 caracteres : c'est votre "mot de passe"
#define OLLAMA_HOST      "192.168.1.149"       // IP FIXE de votre PC (reservation DHCP conseillee)
#define OLLAMA_PORT      11434
#define OLLAMA_MODEL     "qwen2.5vl:7b"       // doit etre IDENTIQUE au nom affiche par : ollama list
#define OLLAMA_KEEP_ALIVE "30m"               // garde le modele en memoire entre deux scans

// Calibration HX711 : meme convention que la bibliotheque HX711 (bogde) de l'ancien programme :
//   poids_g = (brut - offset) / CALIBRATION_FACTOR   (brut = valeur 24 bits signee, voie A, gain 128 ; offset = tare)
// Facteur en counts par gramme (set_scale(680.336f) dans l'ancien programme). Applique au 1er demarrage, puis memorise
// dans Preferences ; re-applique seulement si CETTE valeur change. La commande serie "cal <masse_g>" le remplace de
// facon durable ; "calreset" revient a cette valeur.
#define CALIBRATION_FACTOR 680.336f

// Tare : comme l'ancien programme (balance.tare(30) a chaque demarrage). 1 = tare a CHAQUE demarrage (plateau VIDE) ;
// 0 = reutilise l'offset memorise et ne tare au demarrage que s'il n'y en a pas. L'offset est toujours memorise.
#define TARE_AT_BOOT 1
#define TARE_SAMPLES 30       // echantillons bruts par tare (12 a 30), moyenne tronquee

// Seuils fonctionnels
#define PLACE_G          35.0f                // aliment detecte a partir de cette valeur (en dessous : aucune detection)
#define REMOVE_G         30.0f                // aliment retire en dessous (< PLACE_G : hysteresis de 5 g, evite les va-et-vient)
#define STABLE_BAND_G    1.5f                 // variation maximale pendant la stabilisation
#define STABLE_MS        2000                 // duree de stabilisation
#define REMOVE_DEBOUNCE_MS 400

// Broches (HX711, OLED : celles de votre cahier des charges ; camera : brochage fabricant)
#define PIN_HX_DT   38     // HX711 DOUT (alimenter le module en 3.3V pour ne pas injecter 5V sur le GPIO)
#define PIN_HX_SCK  39     // HX711 SCK
#define PIN_OLED_SDA 21    // 21/14 recommandes : GPIO2 porte une LED embarquee qui perturbe l'I2C
#define PIN_OLED_SCL 14
#define OLED_ADDR   0x3C
#define CAM_PWDN -1
#define CAM_RESET -1
#define CAM_XCLK 15
#define CAM_SIOD 4
#define CAM_SIOC 5
#define CAM_Y9 16
#define CAM_Y8 17
#define CAM_Y7 18
#define CAM_Y6 12
#define CAM_Y5 10
#define CAM_Y4 8
#define CAM_Y3 9
#define CAM_Y2 11
#define CAM_VSYNC 6
#define CAM_HREF 7
#define CAM_PCLK 13

#define FW_VERSION "cloud-1.0.0"
#define OLLAMA_TOTAL_TIMEOUT_MS 240000UL
#define TARE_POLL_MS   2500
#define BEAT_MS        15000
#define LIVE_MIN_MS    2000
static_assert(REMOVE_G < PLACE_G, "REMOVE_G doit rester inferieur a PLACE_G (hysteresis)");

// ======================================================================================
// TYPES PARTAGES (anciennement ns_types.h) : definis AVANT la 1re fonction du fichier
// (l'IDE Arduino genere des prototypes automatiques inseres avant la 1re fonction)
// ======================================================================================
enum AiPhase : uint8_t { AI_IDLE = 0, AI_RUNNING, AI_OK, AI_ERR };

struct ScanResult {
  char id[20], name[48], nameL[48], advice[200], adviceL[200];
  float weight, prot, carb, fat, kcal; bool corrected;
};
struct Targets { float kcal, prot, carb, fat; };

// ======================================================================================
// LOGIQUE SANS DEPENDANCE ARDUINO (anciennement pure_logic.h)
// ======================================================================================
// Logique SANS dependance Arduino (testee sur PC : tests/run_all.sh).
// Prerequis : les seuils PLACE_G, REMOVE_G, STABLE_BAND_G, STABLE_MS, REMOVE_DEBOUNCE_MS doivent etre
// definis AVANT d'inclure ce fichier (ils le sont dans le .ino).
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

// ======================================================================================
// PROGRAMME PRINCIPAL
// ======================================================================================
// ================================ MATERIEL ET ETAT ================================
static Adafruit_SSD1306 display(128, 64, &Wire, -1);   // OLED SSD1306 128x64 I2C (Adafruit)
static Preferences prefs;
static WiFiClientSecure fbClient;
static portMUX_TYPE hxMux = portMUX_INITIALIZER_UNLOCKED;
static WeightFilter filt;
static ScaleLogic logic;
static SemaphoreHandle_t g_mx;
static TaskHandle_t hNet;

static int32_t g_offset = 0; static float g_cpg = 0; static bool g_hasOffset = false, g_hasCal = false;
static volatile float g_weight = 0;
static volatile bool g_hxOk = false, g_hxSat = false, g_camOk = false, g_cloudOk = false, g_cfgBad = false;
static uint32_t g_lastRawMs = 0;               // 0 = aucune mesure HX711 recue depuis le demarrage
static volatile int32_t g_lastRaw = 0;         // derniere lecture brute valide (diagnostic : commandes "status" et "raw")
static bool g_tarePending = false;             // true : tare a (re)faire des que le HX711 repond
static bool g_tareCheck = false; static uint32_t g_tareCheckAt = 0;   // controle de coherence ~2,5 s apres une tare
static volatile uint32_t g_gen = 0;            // +1 a chaque retrait : annule toute analyse en cours
static volatile float g_triggerWeight = 0;

static volatile AiPhase g_ai = AI_IDLE;
static ScanResult g_res;
static Targets g_targets = {0, 0, 0, 0};
static char g_goalCode[10] = "maintien";
static char g_err[40] = "";
static volatile bool g_tareReq = false, g_tareAck = false;
static volatile uint8_t g_tareResult = 0;      // 0 en attente, 1 ok, 2 echec
static volatile bool g_simScan = false;        // scan lance par la commande serie "scan <g>" (test sans balance)
static bool g_oledOk = false;                  // false : ecran absent -> affichage desactive

// ----- HX711 : lecture 24 bits (voie A, gain 128). SCK reste haut quelques us (>60 us = mise en veille) -----
static inline bool hxReady() { return digitalRead(PIN_HX_DT) == LOW; }
static bool hxRead(int32_t* out) {
  if (!hxReady()) return false;
  uint32_t v = 0;
  portENTER_CRITICAL(&hxMux);
  for (int i = 0; i < 24; i++) {
    digitalWrite(PIN_HX_SCK, HIGH); delayMicroseconds(1);
    v = (v << 1) | (uint32_t)digitalRead(PIN_HX_DT);
    digitalWrite(PIN_HX_SCK, LOW); delayMicroseconds(1);
  }
  digitalWrite(PIN_HX_SCK, HIGH); delayMicroseconds(1); digitalWrite(PIN_HX_SCK, LOW);   // 25e impulsion
  portEXIT_CRITICAL(&hxMux);
  if (v & 0x800000) v |= 0xFF000000;
  *out = (int32_t)v; return true;
}
static bool collectRaw(int n, int32_t* out, uint32_t timeoutMs) {
  uint32_t t0 = millis(); int k = 0;
  while (k < n && millis() - t0 < timeoutMs) {
    int32_t r;
    if (hxRead(&r) && r < 8388607 && r > -8388608) { out[k++] = r; g_lastRaw = r; uint32_t m = millis(); g_lastRawMs = m ? m : 1; }   // garde g_hxOk vrai pendant tare/cal
    delay(2);
  }
  return k == n;
}
// Tare : meme convention que la bibliotheque HX711 (tare() : offset = moyenne du brut a vide ; poids = (brut - offset) / facteur).
// Differences voulues : on laisse le HX711 se stabiliser, on jette les premieres lectures, on prend n echantillons
// (moyenne tronquee) et on retente jusqu'a 3 fenetres si le signal bouge ; la plus stable est retenue.
static bool doTareN(int n) {
  if (n > 30) n = 30;
  if (n < 12) n = 12;
  uint32_t up = millis(); if (up < 1500) delay(1500 - up);       // HX711 : ~400 ms de stabilisation apres mise sous tension (+ marge)
  int32_t a[30]; int trim = n / 6;                               // 12 -> 2, 30 -> 5
  uint32_t tmo = 1000UL + (uint32_t)n * 150UL; if (tmo < 4000UL) tmo = 4000UL;
  float cp = fabsf(g_cpg) > 0.0f ? fabsf(g_cpg) : 1.0f;
  float bestM = 0, bestSp = 1e30f; bool got = false;
  for (int t = 0; t < 3; t++) {
    int32_t purge[3]; collectRaw(3, purge, 1500);                // jette les lectures qui suivent une pause / mise en route
    if (!collectRaw(n, a, tmo)) { if (!got) { Serial.println("[tare] ECHEC : HX711 muet"); return false; } break; }
    float m = trimmedMean(a, n, trim);                           // trie a[] sur place : a[trim]..a[n-trim-1] = echantillons retenus
    float sp = (float)(a[n - trim - 1] - a[trim]);
    if (sp < bestSp) { bestSp = sp; bestM = m; got = true; }
    if (sp / cp <= 2.0f * STABLE_BAND_G) break;                  // fenetre assez stable
    Serial.printf("[tare] fenetre instable (dispersion %.1f g) : nouvel essai %d/3\n", sp / cp, t + 1);
  }
  if (bestSp / cp > 2.0f * STABLE_BAND_G)
    Serial.printf("[tare] ATTENTION : signal toujours instable (dispersion %.1f g) : plateau vide et immobile ? alimentation/cablage HX711 ?\n", bestSp / cp);
  g_offset = (int32_t)lroundf(bestM); g_hasOffset = true; prefs.putInt("off", g_offset);
  filt.reset(); if (g_hasCal) filt.setCalibration(g_offset, g_cpg);
  g_tarePending = false; g_tareCheck = true; g_tareCheckAt = millis() + 2500;
  Serial.printf("[tare] OK offset=%ld (%d echantillons, dispersion %.1f g)\n", (long)g_offset, n, bestSp / cp); return true;
}
static bool doTare() { return doTareN(TARE_SAMPLES); }
// Diagnostic : 20 lectures brutes -> moyenne / min / max et poids equivalent avec l'offset et le facteur en vigueur.
static void rawReport() {
  int32_t a[20]; if (!collectRaw(20, a, 6000)) { Serial.println("[raw] HX711 muet"); return; }
  float m = trimmedMean(a, 20, 0);                               // trie a[] : a[0] = min, a[19] = max
  Serial.printf("[raw] moy=%.0f min=%ld max=%ld (p-p %ld counts) | offset=%ld facteur=%.4f -> %.1f g\n",
                m, (long)a[0], (long)a[19], (long)(a[19] - a[0]), (long)g_offset, g_cpg, (m - (float)g_offset) / g_cpg);
}
static bool doCal(float massG) {
  if (!g_hasOffset) { Serial.println("[cal] ECHEC : faire 'tare' (plateau vide) d'abord"); return false; }
  if (!(massG > 0)) { Serial.println("[cal] masse invalide"); return false; }
  int32_t a[12]; if (!collectRaw(12, a, 4000)) { Serial.println("[cal] ECHEC : HX711 muet"); return false; }
  float delta = trimmedMean(a, 12, 2) - (float)g_offset;
  float spread = (float)(a[9] - a[2]);
  if (fabsf(delta) < 500) { Serial.printf("[cal] ECHEC : signal trop faible (%.0f counts) : masse posee ? cablage ?\n", delta); return false; }
  if (spread > 0.02f * fabsf(delta)) { Serial.printf("[cal] ECHEC : signal instable (dispersion %.0f counts pour %.0f) : masse immobile ? refaire 'cal'\n", spread, delta); return false; }
  g_cpg = delta / massG; g_hasCal = true; prefs.putFloat("cpg", g_cpg); filt.reset(); filt.setCalibration(g_offset, g_cpg);
  Serial.printf("[cal] OK %.4f counts/g (masse %.1f g)\n", g_cpg, massG); return true;
}
static void handleSerial() {
  static char line[40]; static int n = 0;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      line[n] = 0; n = 0;
      if (!strcmp(line, "tare")) { if (doTare()) g_weight = 0; }
      else if (!strncmp(line, "cal ", 4)) doCal(atof(line + 4));
      else if (!strcmp(line, "raw")) rawReport();
      else if (!strcmp(line, "calreset")) {                            // retour au facteur de user_config.h
        g_cpg = CALIBRATION_FACTOR; g_hasCal = true; prefs.putFloat("cpg", g_cpg); filt.reset();
        if (g_hasOffset) filt.setCalibration(g_offset, g_cpg);
        Serial.printf("[cal] facteur par defaut restaure : %.4f counts/g\n", g_cpg);
      }
      else if (!strncmp(line, "scan ", 5)) {                           // TEST sans balance : poids saisi a la main
        float g = atof(line + 5);
        if (g < PLACE_G || g > 5000) Serial.printf("[scan] usage : scan <poids en g, %.0f a 5000>\n", PLACE_G);
        else if (g_ai == AI_RUNNING) Serial.println("[scan] une analyse est deja en cours");
        else { g_triggerWeight = g; g_simScan = true; g_err[0] = 0; g_ai = AI_RUNNING; xTaskNotifyGive(hNet); Serial.printf("[scan] TEST : analyse avec un poids simule de %.0f g\n", g); }
      }
      else if (!strcmp(line, "status")) Serial.printf("[status] poids=%.1f cal=%d raw=%ld offset=%ld cpg=%.4f hx=%d oled=%d cam=%d wifi=%d cloud=%d heap=%u bloc_max=%u psram=%u\n", (float)g_weight, g_hasCal, (long)g_lastRaw, (long)g_offset, g_cpg, g_hxOk, g_oledOk, g_camOk, WiFi.status() == WL_CONNECTED, g_cloudOk, (unsigned)ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), (unsigned)ESP.getFreePsram());
      else if (line[0]) Serial.println("Commandes : tare | cal <masse_g> | calreset | raw | scan <masse_g> (test) | status");
    } else if (n < (int)sizeof(line) - 1) line[n++] = c;
  }
}

// ----- Camera : JPEG QVGA 320x240, frame buffer en DRAM interne (carte sans PSRAM) -----
static bool cameraInit() {
  Serial.printf("[camera] PSRAM %s -> frame buffer en DRAM interne (JPEG QVGA 320x240) ; heap=%u bloc_max=%u\n",
                psramFound() ? "detectee (non utilisee)" : "absente", (unsigned)ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  camera_config_t c = {};

  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer = LEDC_TIMER_0;

  c.pin_d0 = CAM_Y2;
  c.pin_d1 = CAM_Y3;
  c.pin_d2 = CAM_Y4;
  c.pin_d3 = CAM_Y5;
  c.pin_d4 = CAM_Y6;
  c.pin_d5 = CAM_Y7;
  c.pin_d6 = CAM_Y8;
  c.pin_d7 = CAM_Y9;

  c.pin_xclk = CAM_XCLK;
  c.pin_pclk = CAM_PCLK;
  c.pin_vsync = CAM_VSYNC;
  c.pin_href = CAM_HREF;
  c.pin_sccb_sda = CAM_SIOD;
  c.pin_sccb_scl = CAM_SIOC;
  c.pin_pwdn = CAM_PWDN;
  c.pin_reset = CAM_RESET;

  c.pixel_format = PIXFORMAT_JPEG;
  c.frame_size = FRAMESIZE_QVGA;
  c.fb_count = 1;
  c.fb_location = CAMERA_FB_IN_DRAM;
  c.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

  // 1er essai : 20 MHz / qualite 15 ; en cas d'echec : 10 MHz / qualite 20 (image plus petite, horloge plus tolerante).
  const uint32_t xclk[2] = {20000000, 10000000};
  const int quality[2] = {15, 20};
  for (int i = 0; i < 2; i++) {
    c.xclk_freq_hz = xclk[i];
    c.jpeg_quality = quality[i];
    esp_err_t err = esp_camera_init(&c);
    if (err == ESP_OK) {
      Serial.printf("[camera] Initialisee en QVGA (XCLK %u MHz, qualite JPEG %d)\n", (unsigned)(xclk[i] / 1000000), quality[i]);
      return true;
    }
    Serial.printf("[camera] Erreur 0x%x (essai %d/2)\n", (unsigned)err, i + 1);
    esp_camera_deinit();
  }
  return false;
}

// ================================ FIREBASE (REST) ================================
// Donnees sous /scales/<SCALE_ID>/ : goal, targets, status, lastScan, command/tare, journal/<jour>/<id>
static bool fbRequest(const char* method, const char* sub, const char* body, String* resp, int* code, bool silent) {
  if (WiFi.status() != WL_CONNECTED) { if (code) *code = -100; return false; }
  String url = String(FIREBASE_DB_URL) + "/scales/" + SCALE_ID + sub + ".json" + (silent ? "?print=silent" : "");
  HTTPClient http; http.setReuse(true); http.setTimeout(8000);
  if (!http.begin(fbClient, url)) { if (code) *code = -101; return false; }
  http.addHeader("Content-Type", "application/json");
  int c = http.sendRequest(method, (uint8_t*)body, body ? strlen(body) : 0);
  if (code) *code = c;
  if (c > 0 && resp) *resp = http.getString();
  http.end();
  g_cloudOk = (c >= 200 && c < 300);
  return g_cloudOk;
}
static const char* goalPhrase() {
  if (!strcmp(g_goalCode, "masse")) return "Prise de masse musculaire";
  if (!strcmp(g_goalCode, "seche")) return "Seche / perte de gras";
  return "Maintien du poids";
}
static void fetchGoalTargets() {                       // l'objectif est lu AVANT chaque analyse
  String r; int code;
  if (fbRequest("GET", "/goal", nullptr, &r, &code, false)) {
    JsonDocument d;
    if (!deserializeJson(d, r) && d.is<const char*>()) {
      const char* g = d.as<const char*>();
      if (!strcmp(g, "masse") || !strcmp(g, "seche") || !strcmp(g, "maintien")) strlcpy(g_goalCode, g, sizeof(g_goalCode));  // liste blanche
    }
  } else Serial.printf("[firebase] lecture objectif impossible (HTTP %d) - objectif par defaut\n", code);
  if (fbRequest("GET", "/targets", nullptr, &r, &code, false)) {
    JsonDocument d;
    if (!deserializeJson(d, r) && d.is<JsonObject>()) {
      g_targets.kcal = d["kcal"] | 0.0f; g_targets.prot = d["proteines"] | 0.0f;
      g_targets.carb = d["glucides"] | 0.0f; g_targets.fat = d["lipides"] | 0.0f;
    }
  }
}
static const char* phaseName() {
  if (!g_hasCal || !g_hasOffset) return "uncalibrated";
  if (logic.phase == PH_EMPTY) return "empty";
  if (logic.phase == PH_SETTLING) return "settling";
  return g_ai == AI_OK ? "result" : (g_ai == AI_ERR ? "error" : "analyzing");
}
static void publishStatus() {
  JsonDocument d;
  d["ts"][".sv"] = "timestamp";
  if (g_hasCal && g_hasOffset && g_hxOk) d["weight_g"] = roundf((float)g_weight * 10.0f) / 10.0f; else d["weight_g"] = nullptr;
  d["phase"] = phaseName(); d["calibrated"] = (bool)(g_hasCal && g_hasOffset); d["hx711_ok"] = (bool)g_hxOk; d["cam_ok"] = (bool)g_camOk;
  d["rssi"] = WiFi.RSSI(); d["fw"] = FW_VERSION; d["model"] = OLLAMA_MODEL; d["heap"] = (uint32_t)ESP.getFreeHeap();
  String s; serializeJson(d, s); int code;
  fbRequest("PATCH", "/status", s.c_str(), nullptr, &code, true);
}
static void ackTare(bool ok) {
  JsonDocument d; d["lastTare"]["ts"][".sv"] = "timestamp"; d["lastTare"]["ok"] = ok;
  String s; serializeJson(d, s); int code;
  fbRequest("PATCH", "/status", s.c_str(), nullptr, &code, true);
  fbRequest("DELETE", "/command/tare", nullptr, nullptr, &code, true);
}
static double g_lastTareStamp = -1; static bool g_tareInit = false;
static void pollRemoteTare() {
  String r; int code;
  if (!fbRequest("GET", "/command/tare", nullptr, &r, &code, false)) return;
  JsonDocument d; if (deserializeJson(d, r)) return;
  double v = d.isNull() ? 0.0 : d.as<double>();
  if (!g_tareInit) { g_lastTareStamp = v; g_tareInit = true; return; }      // commande deja presente au demarrage : ignoree
  if (v > 0 && v != g_lastTareStamp) { g_lastTareStamp = v; g_tareResult = 0; g_tareAck = true; g_tareReq = true; }
}

// ================================ OLLAMA ================================
static float readNum(JsonVariant v) {
  if (v.is<float>()) return v.as<float>();
  if (v.is<const char*>()) { float f; if (numberFromText(v.as<const char*>(), &f)) return f; }
  return 0;
}
// Prompt : objectif + poids exact. Reponse attendue : JSON strict.
static void buildPrompt(char* out, size_t cap, float weightG) {
  snprintf(out, cap,
    "Tu es un nutritionniste sportif. L'image montre un aliment ou un plat pose sur une balance de cuisine. "
    "Poids mesure exact : %.0f g. Objectif de l'utilisateur : %s. "
    "Identifie l'aliment principal (nom court en francais, precise cru ou cuit si visible) et estime, pour CE poids exact "
    "(et non pour 100 g), les proteines, glucides, lipides (en grammes) et les calories (kcal). "
    "Reponds UNIQUEMENT par un objet JSON strict, sans texte autour, avec exactement ces cles : "
    "{\"aliment\": texte, \"proteines\": nombre, \"glucides\": nombre, \"lipides\": nombre, \"calories\": nombre, \"conseil_coach\": texte}. "
    "Les nombres sont sans unite. \"conseil_coach\" : une ou deux phrases courtes en francais (140 caracteres maximum) adaptees a l'objectif. "
    "Si l'image ne montre aucun aliment : aliment = \"inconnu\" et tous les nombres a 0.",
    weightG, goalPhrase());
}
// Capture une photo JPEG QVGA (frame buffer en DRAM) et construit le corps JSON (image en Base64) via malloc()
// (memoire interne si pas de PSRAM ; PSRAM automatiquement si elle est activee).
static char* buildOllamaBody(const char* prompt, size_t* outLen) {
  JsonDocument d;
  d["model"] = OLLAMA_MODEL; d["prompt"] = prompt; d["stream"] = false; d["format"] = "json"; d["keep_alive"] = OLLAMA_KEEP_ALIVE;
  JsonObject o = d["options"].to<JsonObject>(); o["temperature"] = 0.2; o["num_predict"] = 400; o["num_ctx"] = 4096;
  String head; serializeJson(d, head); head.remove(head.length() - 1);          // retire le '}' final
  for (int i = 0; i < 2; i++) { camera_fb_t* w = esp_camera_fb_get(); if (w) esp_camera_fb_return(w); }   // auto-exposition
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) { strlcpy(g_err, "Camera: pas d'image", sizeof(g_err)); return nullptr; }
  if (fb->format != PIXFORMAT_JPEG || fb->len < 4 || fb->buf[0] != 0xFF || fb->buf[1] != 0xD8) {
    esp_camera_fb_return(fb); strlcpy(g_err, "Camera: JPEG invalide", sizeof(g_err)); return nullptr;
  }
  const char* mid = ",\"images\":[\""; const char* tail = "\"]}";
  size_t b64cap = 4 * ((fb->len + 2) / 3) + 1;
  size_t total = head.length() + strlen(mid) + b64cap + strlen(tail) + 1;
  char* buf = (char*)malloc(total);
  if (!buf) {
    Serial.printf("[scan] malloc(%u) impossible : heap=%u bloc_max=%u\n", (unsigned)total, (unsigned)ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    esp_camera_fb_return(fb); strlcpy(g_err, "Memoire insuffisante", sizeof(g_err)); return nullptr;
  }
  size_t p = 0; memcpy(buf, head.c_str(), head.length()); p += head.length();
  memcpy(buf + p, mid, strlen(mid)); p += strlen(mid);
  size_t olen = 0;
  mbedtls_base64_encode((unsigned char*)buf + p, b64cap, &olen, fb->buf, fb->len); p += olen;
  esp_camera_fb_return(fb);
  memcpy(buf + p, tail, strlen(tail) + 1); p += strlen(tail);
  *outLen = p; return buf;
}
// POST HTTP/1.0 : le serveur repond sans "chunked" et ferme la connexion ; permet d'attendre longtemps (CPU lent).
// Retourne le code HTTP (>0) ou : -1 connexion, -2 envoi, -3 annule (aliment retire), -4 reponse invalide, -5 delai depasse, -6 memoire.
static int ollamaPost(const char* body, size_t blen, uint32_t myGen, char** respOut) {
  WiFiClient c;
  if (!c.connect(OLLAMA_HOST, OLLAMA_PORT, 5000)) return -1;
  c.printf("POST /api/generate HTTP/1.0\r\nHost: %s:%d\r\nContent-Type: application/json\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",
           OLLAMA_HOST, OLLAMA_PORT, (unsigned)blen);
  uint32_t t0 = millis(); size_t off = 0;
  while (off < blen) {
    if (g_gen != myGen) { c.stop(); return -3; }
    size_t n = blen - off; if (n > 1460) n = 1460;
    size_t w = c.write((const uint8_t*)body + off, n); off += w;
    if (w == 0) { if (!c.connected() || millis() - t0 > 20000) { c.stop(); return -2; } delay(5); }
  }
  // Tampon reponse en memoire interne (pas de PSRAM). 16 Ko suffisent : "response" precede le gros tableau "context"
  // d'Ollama ; si la reponse depasse, elle est tronquee apres ce champ et parseOllama() la recupere quand meme.
  const size_t CAP = 16384; char* buf = (char*)malloc(CAP); if (!buf) { c.stop(); return -6; }
  size_t len = 0; bool done = false;
  while (millis() - t0 < OLLAMA_TOTAL_TIMEOUT_MS) {
    if (g_gen != myGen) { c.stop(); free(buf); return -3; }
    int av = c.available();
    if (av > 0) { size_t room = CAP - 1 - len; if (room == 0) { done = true; break; } int r = c.read((uint8_t*)buf + len, (size_t)av < room ? (size_t)av : room); if (r > 0) len += r; }
    else if (!c.connected()) { done = true; break; }
    else delay(25);
  }
  buf[len] = 0; c.stop();
  if (!done) { free(buf); return -5; }
  const char* b; bool ch; int code = parseHttpResponse(buf, &b, &ch);
  if (code <= 0) { free(buf); return -4; }
  if (ch) dechunk((char*)b, strlen(b));
  memmove(buf, b, strlen(b) + 1); *respOut = buf; return code;
}
static bool parseOllama(const char* resp, float weightG, ScanResult* out) {
  // Filtre : on ne garde que "response" et "error" (le tableau "context" d'Ollama est ignore).
  JsonDocument filter; filter["response"] = true; filter["error"] = true;
  JsonDocument outer;
  DeserializationError de = deserializeJson(outer, resp, DeserializationOption::Filter(filter));
  const char* txt = de ? "" : (outer["response"] | "");
  JsonDocument alt;                                       // repli si la reponse a ete tronquee apres "response"
  if (!*txt) {
    char* raw = (char*)malloc(3072);
    if (raw) {
      if (extractRawJsonString(resp, "response", raw, 3072) && !deserializeJson(alt, (const char*)raw)) txt = alt.as<const char*>() ? alt.as<const char*>() : "";
      free(raw);
    }
  }
  if (!*txt) {
    if (de) { strlcpy(g_err, "Reponse Ollama illisible", sizeof(g_err)); return false; }
    const char* e = outer["error"] | ""; Serial.printf("[ollama] erreur : %s\n", e); strlcpy(g_err, *e ? "Ollama: voir serie" : "Reponse vide", sizeof(g_err)); return false;
  }
  char* js = (char*)malloc(4096); if (!js) { strlcpy(g_err, "Memoire", sizeof(g_err)); return false; }
  if (!extractJsonObject(txt, js, 4096)) { free(js); Serial.printf("[ollama] pas de JSON dans : %s\n", txt); strlcpy(g_err, "Pas de JSON valide", sizeof(g_err)); return false; }
  JsonDocument d; DeserializationError e = deserializeJson(d, js); free(js);
  if (e) { strlcpy(g_err, "JSON modele invalide", sizeof(g_err)); return false; }
  const char* ali = d["aliment"] | "inconnu"; if (!*ali) ali = "inconnu";
  strlcpy(out->name, ali, sizeof(out->name));
  const char* adv = d["conseil_coach"] | ""; strlcpy(out->advice, adv, sizeof(out->advice));
  Macros m; m.prot = readNum(d["proteines"].as<JsonVariant>()); m.carb = readNum(d["glucides"].as<JsonVariant>());
  m.fat = readNum(d["lipides"].as<JsonVariant>()); m.kcal = readNum(d["calories"].as<JsonVariant>());
  sanitizeMacros(&m, weightG);
  out->weight = weightG; out->prot = m.prot; out->carb = m.carb; out->fat = m.fat; out->kcal = m.kcal; out->corrected = m.corrected;
  utf8ToLatin1(out->name, out->nameL, sizeof(out->nameL)); utf8ToLatin1(out->advice, out->adviceL, sizeof(out->adviceL));
  return true;
}
static bool publishScan(const ScanResult& r, bool simulated) {
  JsonDocument d;
  d["id"] = r.id; d["ts"][".sv"] = "timestamp"; d["aliment"] = r.name; d["poids_g"] = roundf(r.weight * 10) / 10.0f;
  d["proteines"] = roundf(r.prot * 10) / 10.0f; d["glucides"] = roundf(r.carb * 10) / 10.0f; d["lipides"] = roundf(r.fat * 10) / 10.0f;
  d["calories"] = roundf(r.kcal); d["conseil_coach"] = r.advice; d["objectif"] = g_goalCode; d["modele"] = OLLAMA_MODEL; d["corrige"] = r.corrected; d["simule"] = simulated;
  String s; serializeJson(d, s); int code;
  bool ok = fbRequest("PUT", "/lastScan", s.c_str(), nullptr, &code, true);
  if (!ok) Serial.printf("[firebase] ecriture lastScan impossible (HTTP %d) : regles/URL/SCALE_ID ?\n", code);
  return ok;
}
static void finishAi(uint32_t myGen, bool ok, const ScanResult* r) {
  if (g_gen != myGen) return;                     // l'aliment a ete retire entre-temps : resultat abandonne
  if (ok && r) { xSemaphoreTake(g_mx, portMAX_DELAY); g_res = *r; xSemaphoreGive(g_mx); }
  g_ai = ok ? AI_OK : AI_ERR;
}
static void runAnalysis() {
  uint32_t myGen = g_gen; float w = g_triggerWeight; g_err[0] = 0;
  bool sim = g_simScan; g_simScan = false;
  Serial.printf("[scan] aliment stable a %.1f g : analyse...\n", w);
  fetchGoalTargets();
  if (g_gen != myGen) return;
  if (!g_camOk) { strlcpy(g_err, "Camera indisponible", sizeof(g_err)); finishAi(myGen, false, nullptr); return; }
  fbClient.stop();                                // libere le contexte TLS Firebase (sans PSRAM, la DRAM est comptee) ; reconnexion automatique ensuite
  char prompt[1400]; buildPrompt(prompt, sizeof(prompt), w);
  size_t blen = 0; char* body = buildOllamaBody(prompt, &blen);
  if (!body) { finishAi(myGen, false, nullptr); return; }
  Serial.printf("[scan] image + requete : %u octets -> http://%s:%d (%s) ; heap=%u\n", (unsigned)blen, OLLAMA_HOST, OLLAMA_PORT, OLLAMA_MODEL, (unsigned)ESP.getFreeHeap());
  uint32_t t0 = millis(); char* resp = nullptr;
  int code = ollamaPost(body, blen, myGen, &resp); free(body);
  if (code == -3) { Serial.println("[scan] annule (aliment retire)"); return; }
  if (code < 0) {
    const char* m = code == -1 ? "Ollama injoignable (IP/pare-feu)" : code == -5 ? "Ollama: delai depasse" : code == -6 ? "Memoire insuffisante" : "Ollama: erreur reseau";
    strlcpy(g_err, m, sizeof(g_err)); Serial.printf("[scan] %s (code %d)\n", m, code); finishAi(myGen, false, nullptr); return;
  }
  Serial.printf("[scan] reponse HTTP %d en %lu ms\n", code, (unsigned long)(millis() - t0));
  if (code != 200) {
    if (code == 404) strlcpy(g_err, "Modele absent (ollama pull)", sizeof(g_err)); else snprintf(g_err, sizeof(g_err), "Ollama HTTP %d", code);
    Serial.printf("[scan] corps : %.200s\n", resp); free(resp); finishAi(myGen, false, nullptr); return;
  }
  ScanResult r; memset(&r, 0, sizeof(r));
  bool ok = parseOllama(resp, w, &r); free(resp);
  if (!ok) { Serial.printf("[scan] echec : %s\n", g_err); finishAi(myGen, false, nullptr); return; }
  snprintf(r.id, sizeof(r.id), "%08lx%04x", (unsigned long)esp_random(), (unsigned)(millis() & 0xFFFF));
  Serial.printf("[scan] %s : P%.0f G%.0f L%.0f = %.0f kcal%s\n", r.name, r.prot, r.carb, r.fat, r.kcal, r.corrected ? " (corrige)" : "");
  if (g_gen != myGen) return;
  publishScan(r, sim);                             // echec Firebase : le resultat reste affiche sur l'OLED
  finishAi(myGen, true, &r);
}

// ================================ TACHE RESEAU (coeur 0) ================================
static void netTask(void*) {
  fbClient.setInsecure();      // pas de verification du certificat (voir docs/GUIDE.md pour le durcir)
  uint32_t lastPoll = 0, lastBeat = 0, lastLive = 0, lastWifi = 0; float lastW = -999; const char* lastPhase = nullptr;
  for (;;) {
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100))) runAnalysis();
    uint32_t now = millis();
    if (WiFi.status() != WL_CONNECTED) {
      g_cloudOk = false;
      if (now - lastWifi > 10000) { lastWifi = now; WiFi.reconnect(); }
      continue;
    }
    if (g_tareAck && g_tareResult) { ackTare(g_tareResult == 1); g_tareAck = false; }
    if (now - lastPoll >= TARE_POLL_MS) { lastPoll = now; pollRemoteTare(); }
    const char* ph = phaseName();                 // litteraux : comparaison de pointeurs suffisante
    bool changed = fabsf((float)g_weight - lastW) >= 1.0f || ph != lastPhase;
    if ((changed && now - lastLive >= LIVE_MIN_MS) || now - lastBeat >= BEAT_MS) {
      lastLive = lastBeat = now; lastW = g_weight; lastPhase = ph; publishStatus();
    }
  }
}

// ================================ AFFICHAGE OLED 128x64 (Adafruit SSD1306 + Adafruit GFX) ================================
// Police integree d'Adafruit GFX : 6x8 px par caractere x taille ; pas d'accents -> texte converti en ASCII (latin1ToAscii).
static void oText(const char* t, int x, int y, uint8_t size, uint16_t col) {
  display.setTextSize(size); display.setTextColor(col); display.setCursor(x, y); display.print(t);
}
static void centerText(const char* t, int y, uint8_t size) { int w = (int)strlen(t) * 6 * size; oText(t, (128 - w) / 2, y, size, SSD1306_WHITE); }
static void header(const char* title) {
  display.fillRect(0, 0, 128, 11, SSD1306_WHITE);
  oText(title, 3, 2, 1, SSD1306_BLACK);
  if (WiFi.status() == WL_CONNECTED) display.fillCircle(112, 5, 3, SSD1306_BLACK); else display.drawCircle(112, 5, 3, SSD1306_BLACK);   // WiFi
  if (g_cloudOk) display.fillCircle(122, 5, 3, SSD1306_BLACK); else display.drawCircle(122, 5, 3, SSD1306_BLACK);                          // Firebase
}
static void weightBox(float g, int y, int h) {
  if (g < 0) g = 0;
  display.drawRoundRect(0, y, 128, h, 4, SSD1306_WHITE);
  char b[12]; snprintf(b, sizeof(b), "%.1f", g);
  uint8_t sz = strlen(b) > 6 ? 2 : 3;
  int w = (int)strlen(b) * 6 * sz; int x = (128 - w - 14) / 2; if (x < 2) x = 2;
  oText(b, x, y + (h - 8 * sz) / 2, sz, SSD1306_WHITE);
  oText("g", x + w + 3, y + h - 20, 2, SSD1306_WHITE);
}
static void bar(int x, int y, int w, int h, float frac) {
  if (frac < 0) frac = 0;
  if (frac > 1) frac = 1;
  display.drawRect(x, y, w, h, SSD1306_WHITE); int fw = (int)((w - 4) * frac); if (fw > 0) display.fillRect(x + 2, y + 2, fw, h - 4, SSD1306_WHITE);
}
static void macroRow(int y, char label, float val, float target, float fallbackMax) {
  char b[16]; snprintf(b, sizeof(b), "%c %.0fg", label, val);
  oText(b, 0, y - 7, 1, SSD1306_WHITE);
  float ref = target > 0 ? target : fallbackMax; bar(64, y - 8, 62, 9, ref > 0 ? val / ref : 0);
}
static void drawResult(const ScanResult& r) {
  bool page2 = ((millis() / 4500) % 2) == 1 && r.adviceL[0];
  if (!page2) {
    char ttl[24]; snprintf(ttl, sizeof(ttl), "IA~ %.0f g", r.weight); header(ttl);
    char full[48]; strlcpy(full, r.nameL, sizeof(full)); latin1ToAscii(full);
    char nm[15]; strlcpy(nm, full, sizeof(nm));      // 14 car. max : laisse la place aux kcal
    oText(nm, 0, 15, 1, SSD1306_WHITE);
    char b[24]; snprintf(b, sizeof(b), "%.0fkcal", r.kcal);
    oText(b, 128 - (int)strlen(b) * 6, 15, 1, SSD1306_WHITE);
    float mx = r.prot > r.carb ? r.prot : r.carb; if (r.fat > mx) mx = r.fat;
    macroRow(36, 'P', r.prot, g_targets.prot, mx); macroRow(48, 'G', r.carb, g_targets.carb, mx); macroRow(60, 'L', r.fat, g_targets.fat, mx);
  } else {
    header("CONSEIL COACH");
    char adv[200]; strlcpy(adv, r.adviceL, sizeof(adv)); latin1ToAscii(adv);
    char L[5][LINE_W]; int n = wrapText(adv, 21, L, 5);
    for (int i = 0; i < n; i++) oText(L[i], 1, 14 + i * 9, 1, SSD1306_WHITE);
  }
}
static void drawScreen(uint32_t now) {
  display.clearDisplay();
  float g = g_weight;
  if (!g_hxOk) {                                   // diagnostic cablage AVANT l'invite de tare
    header("NUTRISCALE");
    centerText("HX711 SANS REPONSE", 24, 1);
    char b[32]; snprintf(b, sizeof(b), "cablage GPIO%d/%d", PIN_HX_DT, PIN_HX_SCK);
    centerText(b, 38, 1);
  } else if (!g_hasOffset || !g_hasCal) {
    header("NUTRISCALE");
    centerText("TARE EN ATTENTE", 20, 1);
    centerText("Plateau VIDE puis", 34, 1);
    centerText("serie : tare", 46, 1);
  } else if (logic.phase == PH_EMPTY) {
    char b[32]; snprintf(b, sizeof(b), "AU REPOS  (>= %.0f g)", PLACE_G);
    header("NUTRISCALE"); weightBox(g, 14, 32);
    centerText(g_cfgBad ? "CONFIG WIFI/FIREBASE" : b, 54, 1);
  } else if (logic.phase == PH_SETTLING) {
    header("STABILISATION"); weightBox(g, 14, 32);
    centerText("Ne touche plus", 48, 1); bar(0, 55, 128, 9, logic.progress());
  } else if (g_ai == AI_OK) {
    ScanResult r; xSemaphoreTake(g_mx, portMAX_DELAY); r = g_res; xSemaphoreGive(g_mx);
    drawResult(r);
  } else if (g_ai == AI_ERR) {
    header("ERREUR IA");
    char L[3][LINE_W]; int n = wrapText(g_err, 21, L, 3);
    for (int i = 0; i < n; i++) oText(L[i], 2, 20 + i * 10, 1, SSD1306_WHITE);
    centerText("Retirez l'aliment", 54, 1);
  } else {
    header("ANALYSE IA");
    centerText("Analyse IA en cours", 20, 1);
    int span = 128 - 30; int x = (int)((now / 12) % (2 * span)); if (x > span) x = 2 * span - x;
    display.drawRect(0, 32, 128, 9, SSD1306_WHITE); display.fillRect(2 + x * 96 / span, 34, 28, 5, SSD1306_WHITE);
    char b[20]; snprintf(b, sizeof(b), "%.1f g", g); centerText(b, 50, 1);
  }
  display.display();
}
static void splash(const char* msg, float frac) {
  if (!g_oledOk) return;
  display.clearDisplay(); centerText("NUTRISCALE", 14, 2); centerText(msg, 36, 1);
  if (frac >= 0) bar(14, 50, 100, 9, frac); display.display();
}

// ================================ SETUP / LOOP (coeur 1) ================================
static void handleEvent(int ev, float g) {
  if (ev == EVT_TRIGGER) { g_triggerWeight = g; g_ai = AI_RUNNING; xTaskNotifyGive(hNet); }
  else if (ev == EVT_REMOVED) { g_gen++; g_ai = AI_IDLE; Serial.println("[scan] aliment retire : remise a zero"); }
}

void setup() {
  Serial.begin(115200); delay(300);
  Serial.printf("\n=== NutriScale Cloud %s ===\n", FW_VERSION);
  g_mx = xSemaphoreCreateMutex();
  pinMode(PIN_HX_DT, INPUT_PULLUP);          // pull-up : sans module, DT reste haut (= pas pret)
  pinMode(PIN_HX_SCK, OUTPUT);
  digitalWrite(PIN_HX_SCK, LOW);
  Serial.printf("[hx711] DT=GPIO%d SCK=GPIO%d\n", PIN_HX_DT, PIN_HX_SCK);
  prefs.begin("scale", false);
  g_hasOffset = prefs.isKey("off");
  g_offset = prefs.getInt("off", 0); g_cpg = prefs.getFloat("cpg", 0.0f);
  const char* cpgSrc = "memorise";
  // Facteur : valeur de user_config.h au 1er demarrage (ou si CALIBRATION_FACTOR a change, ou valeur memorisee invalide) ;
  // sinon valeur memorisee (y compris celle d'un "cal <masse_g>" ulterieur).
  if (prefs.getFloat("cpgdef", 0.0f) != (float)CALIBRATION_FACTOR || !isfinite(g_cpg) || g_cpg == 0.0f) {
    g_cpg = CALIBRATION_FACTOR; prefs.putFloat("cpg", g_cpg); prefs.putFloat("cpgdef", (float)CALIBRATION_FACTOR);
    cpgSrc = "defaut (user_config.h)";
  }
  g_hasCal = true;
  if (g_hasOffset) filt.setCalibration(g_offset, g_cpg);
  Serial.printf("[balance] tare=%s offset=%ld facteur=%.4f counts/g (%s)\n", g_hasOffset ? "memorisee" : "ABSENTE", (long)g_offset, g_cpg, cpgSrc);
#if TARE_AT_BOOT
  const bool bootTare = true;                  // comme l'ancien programme : tare(30) a chaque demarrage
#else
  const bool bootTare = !g_hasOffset;          // sinon : seulement si aucune tare valide n'est memorisee
#endif
  if (bootTare) {
    Serial.println(g_hasOffset ? "[balance] tare au demarrage : plateau VIDE requis..." : "[balance] aucune tare valide : tare automatique, plateau VIDE requis...");
    delay(500);
    if (!doTareN(TARE_SAMPLES)) { g_tarePending = true; Serial.println("[balance] tare impossible pour l'instant : nouvel essai en arriere-plan"); }
  }

  delay(100);                                  // laisse l'OLED s'alimenter avant de la sonder
  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);      // reste actif : Adafruit_SSD1306 est initialise avec periphBegin = false
  uint8_t err = 4;
  for (int i = 0; i < 5 && err; i++) {         // jusqu'a 5 essais (l'ecran peut demarrer lentement)
    Wire.beginTransmission(OLED_ADDR); err = Wire.endTransmission();
    if (err) delay(60);
  }
  if (err) Serial.printf("[oled] AUCUNE reponse a 0x%02X (SDA=GPIO%d, SCL=GPIO%d) : verifier cablage, alimentation 3.3V et adresse (0x3C ou 0x3D).\n", OLED_ADDR, PIN_OLED_SDA, PIN_OLED_SCL);
  else Serial.printf("[oled] detecte a 0x%02X (SDA=GPIO%d, SCL=GPIO%d)\n", OLED_ADDR, PIN_OLED_SDA, PIN_OLED_SCL);
  g_oledOk = (err == 0) && display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR, true, false);
  if (g_oledOk) { Wire.setClock(400000); display.setTextWrap(false); splash("Demarrage...", -1); }
  else Serial.println("[oled] ecran absent ou init impossible : affichage desactive (le reste fonctionne ; test possible sans ecran)");

  g_cfgBad = strlen(SCALE_ID) < 20 || strstr(SCALE_ID, "REMPLACEZ") || strstr(FIREBASE_DB_URL, "VOTRE-PROJET") || !strcmp(WIFI_SSID, "VotreBoxWiFi");
  if (g_cfgBad) Serial.println("[config] WIFI_SSID / FIREBASE_DB_URL / SCALE_ID non renseignes (SCALE_ID >= 20 caracteres) : le cloud ne fonctionnera pas.");
  g_camOk = cameraInit();                      // AVANT le Wi-Fi : les tampons DMA de la camera ont besoin de DRAM contigue

  WiFi.mode(WIFI_STA); WiFi.setHostname("nutriscale"); WiFi.setAutoReconnect(true); WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) { splash("Connexion WiFi...", (millis() - t0) / 15000.0f); delay(200); }
  if (WiFi.status() == WL_CONNECTED) Serial.printf("[wifi] connecte, IP %s\n", WiFi.localIP().toString().c_str());
  else Serial.println("[wifi] pas encore connecte : nouvelle tentative en arriere-plan (la pesee locale fonctionne)");

  xTaskCreatePinnedToCore(netTask, "net", 20480, nullptr, 1, &hNet, 0);
  Serial.println("Commandes serie : tare | cal <masse_g> | calreset | raw | scan <masse_g> (test) | status");
}

void loop() {
  uint32_t now = millis();
  if (hxReady()) {
    int32_t raw;
    if (hxRead(&raw)) {
      if (raw >= 8388607 || raw <= -8388608) g_hxSat = true;               // sature : capteur debranche ou surcharge
      else {
        g_hxSat = false; g_lastRawMs = now ? now : 1; float g;
        if (filt.pushRaw(raw, &g)) { g_weight = g; handleEvent(logic.update(now, g), g); }
      }
    }
  }
  g_hxOk = g_lastRawMs != 0 && (now - g_lastRawMs) < 1500 && !g_hxSat;
  handleSerial();
  static uint32_t lastAutoTare = 0;
  if (g_tarePending && g_hxOk && !g_tareReq && now - lastAutoTare >= 5000) { lastAutoTare = now; g_tareReq = true; }   // nouvel essai de tare tant qu'elle n'a pas abouti
  if (g_tareReq) {                                                          // tare locale ou a distance
    g_tareReq = false; bool ok = doTare();
    logic = ScaleLogic(); g_gen++; g_ai = AI_IDLE; g_tareResult = ok ? 1 : 2;
    if (ok) g_weight = 0;                                                   // affichage a 0 sans attendre le refill du filtre
  }
  if (g_tareCheck && (int32_t)(now - g_tareCheckAt) >= 0) {                 // coherence : poids ~0 apres une tare a vide
    g_tareCheck = false;
    if (g_hxOk && fabsf((float)g_weight) >= 5.0f)
      Serial.printf("[tare] ATTENTION : %.1f g affiches 2,5 s apres la tare (attendu ~0) : objet sur le plateau, ou signal brut derive depuis la tare (alimentation / cablage HX711). Voir 'raw' et 'status'.\n", (float)g_weight);
    else Serial.println("[tare] controle OK : poids ~0 apres la tare");
  }
  static uint32_t lastDraw = 0;
  if (g_oledOk && now - lastDraw >= 100) { lastDraw = now; drawScreen(now); }
  delay(2);
}