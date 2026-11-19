#!/usr/bin/env python3
"""Regression : aucune definition de struct/class/enum/union/typedef dans le .ino.
Cause d'une erreur reelle ("'ScanResult' does not name a type") : l'IDE Arduino genere les prototypes
des fonctions du .ino AVANT les types definis dans le .ino. Les types doivent vivre dans des .h."""
import re, os, sys
p = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "firmware", "nutriscale_cloud", "nutriscale_cloud.ino")
src = re.sub(r"/\*.*?\*/", "", open(p, encoding="utf-8").read(), flags=re.S)
src = re.sub(r"//[^\n]*", "", src)
bad = [l.strip() for l in src.splitlines() if re.match(r"\s*(struct|class|enum|union|typedef)\b", l)]
if bad:
    print("ECHEC : types definis dans le .ino :", *bad, sep="\n  "); sys.exit(1)
print("OK : aucun type defini dans le .ino")
