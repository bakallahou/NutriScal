#!/bin/sh
# Verification de SYNTAXE du .ino avec des stubs (PC). Ne prouve pas la compilation reelle avec le paquet esp32.
cd "$(dirname "$0")/.."
cp firmware/nutriscale_cloud/nutriscale_cloud.ino /tmp/nutriscale_cloud_ino.cpp
if g++ -std=gnu++17 -fsyntax-only -Wall -Wextra -Wno-unused-parameter -Itests/stubs_cloud -Ifirmware/nutriscale_cloud /tmp/nutriscale_cloud_ino.cpp 2> /tmp/syn_cloud.err; then
  echo "nutriscale_cloud.ino ... OK"
else
  echo "nutriscale_cloud.ino ... ECHEC"; head -40 /tmp/syn_cloud.err; exit 1
fi
