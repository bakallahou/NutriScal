// VOS REGLAGES : c'est le SEUL fichier a modifier. Il n'est jamais remplace par une mise a jour du .ino.
// Ne contient que des #define (pas de code).
#pragma once

// ============================ CONFIGURATION UTILISATEUR ============================
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
