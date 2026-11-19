# Guide NutriScale Cloud

## Ce qui est vérifié, et ce qui ne l'est pas
| Élément | État |
|---|---|
| Logique pure du firmware (détection > 5 g, stabilité 2 s / variation < 1,5 g, filtre, extraction du JSON d'Ollama, découpage du texte OLED, garde-fous) | **Testée sur PC** (61 vérifications) |
| Syntaxe du `.ino` | Vérifiée contre des *stubs* ; **jamais compilé avec le vrai paquet esp32 ni les vraies bibliothèques** |
| Calculs et câblage de l'interface web | **Testés sous Node** (39 vérifications, faux DOM + faux Firebase) |
| Rendu réel dans un navigateur, vrai projet Firebase, vrai Ollama, caméra, HX711, OLED, Wi-Fi | **Non testés** : à valider chez vous |

Si la compilation échoue, copiez le message d'erreur complet : c'est probable et rapide à corriger.

---
## 1. Ollama : modèle et lancement

**Modèle retenu pour ce projet : `qwen2.5vl:7b`** (environ 6 Go, choix de l'utilisateur ; c'est la valeur par défaut de `OLLAMA_MODEL`). Alternative plus légère et plus rapide sur PC sans carte graphique : `qwen3-vl:4b-instruct` (environ 3,3 Go).

Pour référence, **`qwen3-vl:4b-instruct`** (environ 3,3 Go). Qwen3-VL a remplacé Qwen2.5-VL comme référence sur Ollama (informations de la bibliothèque Ollama relevées en 2026 ; vérifiez sur ollama.com/library).
- PC avec ≥ 12 Go de VRAM, ou beaucoup de RAM : `qwen3-vl:8b-instruct` (environ 6,1 Go), plus précis.
- Choisissez les tags **`-instruct`** : les versions « raisonnement » (tag par défaut) réfléchissent avant de répondre, ce qui ralentit et peut casser le JSON strict.
- Vos pistes `qwen2.5vl:7b` et `llama3.2-vision:11b` restent utilisables mais sont plus anciennes ; `llama3.2-vision` est plus lourd et son mode image est annoncé en anglais seulement.

**Étapes**
1. Installer Ollama (ollama.com) puis télécharger le modèle :
   ```
   ollama pull qwen3-vl:4b-instruct
   ```
2. Ouvrir Ollama au réseau local (l'ESP32 est un autre appareil) :
   - **Windows (PowerShell)** — permanent : `setx OLLAMA_HOST "0.0.0.0:11434"`, puis quitter Ollama (icône de la barre des tâches) et le relancer.
     Pour un essai dans la session : `$env:OLLAMA_HOST="0.0.0.0:11434"; ollama serve`
   - **Linux (systemd)** : `sudo systemctl edit ollama.service` → ajouter `[Service]` et `Environment="OLLAMA_HOST=0.0.0.0:11434"`, puis `sudo systemctl daemon-reload && sudo systemctl restart ollama`
   - **macOS** : `launchctl setenv OLLAMA_HOST "0.0.0.0:11434"` puis relancer l'application.
3. Autoriser le port dans le pare-feu (Windows, PowerShell administrateur) :
   `New-NetFirewallRule -DisplayName "Ollama" -Direction Inbound -Protocol TCP -LocalPort 11434 -Action Allow -Profile Private`
4. Noter l'**IP du PC** (`ipconfig` / `ip a`) et la **fixer** (réservation DHCP dans la box) : elle est écrite dans le firmware.
5. Tester depuis un autre appareil du réseau : `curl http://IP_DU_PC:11434/api/tags` doit lister le modèle.

**Sécurité** : Ollama n'a aucune authentification. N'ouvrez jamais ce port vers Internet (pas de redirection de port) : réseau local de confiance uniquement.
La première requête charge le modèle en mémoire (plusieurs dizaines de secondes possibles) ; le firmware attend jusqu'à 4 minutes.

## 2. Firebase : base gratuite, URL et clé
Les intitulés de la console peuvent évoluer légèrement.
1. https://console.firebase.google.com → **Ajouter un projet** (Google Analytics non nécessaire).
2. **Build → Realtime Database → Créer une base de données** : choisir la région proche (ex. `europe-west1`), mode **verrouillé**. Le plan gratuit (Spark) suffit pour ce prototype ; vérifiez les quotas sur la page de tarification.
3. Onglet **Règles** : collez le contenu de `database.rules.json` puis **Publier**.
4. **URL de la base** : en haut de l'onglet Données, `https://NOM-default-rtdb.REGION.firebasedatabase.app` → à copier dans `FIREBASE_DB_URL` (firmware) et `databaseURL` (index.html).
5. **Clé API** : roue dentée → **Paramètres du projet → Général → Vos applications → icône Web `</>`** → enregistrer l'application → copier l'objet `firebaseConfig` dans `web/index.html` (en haut du script).
6. **Générer votre identifiant de balance** (≥ 20 caractères, lettres/chiffres/`-`/`_`) :
   `python3 -c "import secrets;print(secrets.token_urlsafe(24))"` → à mettre dans `SCALE_ID` (firmware) et à saisir sur le site à la première connexion.

**Modèle de sécurité (à comprendre)** : l'identifiant long joue le rôle de mot de passe. Quiconque le connaît peut lire et écrire vos données (profil, poids, repas) ; les règles empêchent seulement de lister les balances. La clé API du site est publique par conception. Pour un vrai service multi-utilisateurs, passez à Firebase Authentication avec des règles par utilisateur.
Le firmware accède à Firebase en HTTPS **sans vérifier le certificat** (`setInsecure`) : acceptable sur un réseau domestique, à durcir (épinglage de l'autorité racine) pour un usage sérieux.

**Structure des données** : `scales/<ID>/` → `goal` (`masse`|`seche`|`maintien`), `targets`, `profile`, `status` (balance), `lastScan`, `command/tare`, `journal/<AAAA-MM-JJ>/<id du scan>`.

### Test du modèle AVANT la balance (PowerShell)
Vérifie que le modèle répond en JSON strict et mesure le temps d'un scan. Remplacez le chemin de la photo (un repas réel) :
```
$img = [Convert]::ToBase64String([IO.File]::ReadAllBytes("C:\chemin\photo.jpg"))
$prompt = 'Tu es un nutritionniste sportif. Poids mesure exact : 150 g. Objectif : Prise de masse. Identifie l aliment et estime pour CE poids les proteines, glucides, lipides (g) et calories. Reponds UNIQUEMENT par un JSON strict : {"aliment": texte, "proteines": nombre, "glucides": nombre, "lipides": nombre, "calories": nombre, "conseil_coach": texte}'
$body = @{ model="qwen2.5vl:7b"; prompt=$prompt; images=@($img); stream=$false; format="json" } | ConvertTo-Json -Depth 5
$sw = [Diagnostics.Stopwatch]::StartNew()
$r = Invoke-RestMethod -Uri http://localhost:11434/api/generate -Method Post -Body ([Text.Encoding]::UTF8.GetBytes($body)) -ContentType "application/json; charset=utf-8" -TimeoutSec 300
"Duree : $($sw.Elapsed.TotalSeconds) s"; $r.response
```
La **première** exécution charge le modèle (plus long) ; relancez-la : le second temps est celui d'un scan réel. Si la durée dépasse ~3 minutes, le firmware abandonnera (`OLLAMA_TOTAL_TIMEOUT_MS`) : prenez le modèle plus léger.

## 3. Firmware
0. Ouvrez **le dossier** `firmware/nutriscale_cloud` (3 fichiers à garder ensemble : `nutriscale_cloud.ino`, `ns_types.h`, `pure_logic.h`). Ne copiez pas le `.ino` seul.
1. Arduino IDE : paquet **esp32 (Espressif) 3.x**, bibliothèques **ArduinoJson 7.x** et **U8g2**.
2. Réglages : *ESP32S3 Dev Module*, Flash 16MB, **PSRAM = OPI PSRAM**, USB CDC On Boot = Disabled, port **USB-UART**.
3. Éditer le bloc **CONFIGURATION UTILISATEUR** du `.ino` : Wi-Fi 2,4 GHz, `FIREBASE_DB_URL`, `SCALE_ID`, `OLLAMA_HOST`.
4. Téléverser. Moniteur série à 115200 bauds.
5. **Calibrer** (obligatoire, aucun facteur n'est supposé) : plateau vide → `tare` ; poser une masse de référence connue → `cal 500` (valeur en grammes). Stocké en mémoire non volatile.
6. Câblage : HX711 DOUT = GPIO41, SCK = GPIO42, alimentation **3,3 V** ; OLED SDA = GPIO1, SCL = GPIO2.
   **Attention GPIO2** : une LED est câblée dessus sur la carte (fiche du fabricant). Elle peut empêcher l'I2C de fonctionner. Le firmware le détecte au démarrage et l'écrit sur la console ; dans ce cas déplacez l'OLED sur **SDA = GPIO21, SCL = GPIO14** (modifier `PIN_OLED_SDA/SCL`). Ne pas utiliser GPIO 35–37 (PSRAM) ni 19/20 (USB).

**Fonctionnement** : aliment > 5 g stable 2 s (variation < 1,5 g) → lecture de l'objectif dans Firebase → photo VGA → Base64 → Ollama → JSON → écran OLED + envoi à Firebase. Retrait (< 5 g) → remise à zéro. Si l'aliment est retiré pendant l'analyse, elle est abandonnée.
La balance fonctionne (pesée, écran) sans Wi-Fi ; l'analyse exige Wi-Fi + PC + Ollama allumés.
La **tare à distance** est lue par interrogation de Firebase toutes les ~2,5 s : délai de quelques secondes (pas de notification instantanée).

## 4. Site web
`web/index.html` est un fichier unique (Firebase Web SDK 10.12.2 chargé depuis les serveurs de Google).
- **Firebase Hosting** : `npm i -g firebase-tools` → `firebase login` → dans le dossier du projet `firebase init hosting` (dossier public : `web`, application monopage : non) → `firebase deploy`.
- **Netlify** : glisser-déposer le dossier `web` sur app.netlify.com/drop. **Vercel** : `vercel` depuis le dossier `web`.
Au premier chargement, saisissez votre `SCALE_ID` : il est mémorisé dans ce navigateur seulement.

## 5. Limites à connaître
- **Les macros sont des estimations d'un modèle de vision** : le poids est exact, mais l'identification de l'aliment et la composition ne le sont pas (plats mixtes, sauces, cru/cuit, huile cachée). Le firmware recale les valeurs incohérentes (calories vs macros, macros > masse) et le marque « corrigé », sans pouvoir garantir la justesse. Comparez avec des tables de composition avant de vous y fier.
- Les besoins calculés (Mifflin-St Jeor × activité, ajustés à l'objectif) sont des **estimations générales, pas un avis médical**. Pour les moins de 18 ans, aucun déficit calorique n'est proposé.
- Un seul aliment à la fois ; PC et Ollama doivent être allumés ; HTTP non chiffré vers Ollama (réseau local).

## 6. Dépannage
| Symptôme | Piste |
|---|---|
| OLED noir, message « AUCUNE réponse » | GPIO2 + LED : passer l'OLED sur 21/14 ; vérifier l'ordre des broches du module (GND, VCC, SCL, SDA) |
| « BALANCE NON CALIBRÉE » | `tare` puis `cal <g>` sur la console |
| « Ollama injoignable » | IP, `OLLAMA_HOST=0.0.0.0`, pare-feu, PC allumé ; test `curl http://IP:11434/api/tags` |
| « Modèle absent » (HTTP 404) | `ollama pull qwen2.5vl:7b` ; nom identique à `OLLAMA_MODEL` |
| « Délai dépassé » | modèle trop lourd pour le PC : prendre un modèle plus petit / un GPU |
| « Pas de JSON valide » | utiliser un tag `-instruct` ; la console affiche la réponse brute |
| Rien dans Firebase / site vide | URL, `SCALE_ID` identique des deux côtés (≥ 20 car.), règles publiées ; console : `[firebase] ... HTTP 401/403` |
| Caméra KO | PSRAM = OPI PSRAM, nappe bien verrouillée |
