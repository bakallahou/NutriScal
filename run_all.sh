#!/bin/sh
# Tous les tests executables sur PC (aucun materiel, aucun compte Firebase, aucun Ollama necessaires).
cd "$(dirname "$0")/.."
set -e
echo "== 1/3 logique pure du firmware (stabilite 2 s / 1,5 g, filtre, texte OLED, JSON/HTTP, garde-fous)"; tests/host/run.sh | tail -1
echo "== 2/3 syntaxe du .ino (stubs) + regression types/prototypes"; python3 tests/check_no_types_in_ino.py; tests/syntax_check.sh
echo "== 3/3 plateforme web (calculs, ids HTML/JS, syntaxe, execution de l'interface simulee)"; node tests/test_web.js
