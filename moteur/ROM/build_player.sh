#!/bin/sh
# Régénère mdplayer_bin.h depuis mdplayer.s (copie DS, independante de l'iPad).
# L'assembleur (m68k-elf-as) ne part JAMAIS dans l'application : il tourne ici,
# et seuls les octets produits sont embarqués.
#   brew install m68k-elf-binutils    (une fois)
set -e
cd "$(dirname "$0")"
TMP=$(mktemp -d)
m68k-elf-as -m68000 -o "$TMP/p.o" mdplayer.s
m68k-elf-ld -Ttext=0x200 -o "$TMP/p.elf" "$TMP/p.o"
m68k-elf-objcopy -O binary "$TMP/p.elf" "$TMP/p.bin"
python3 - "$TMP/p.bin" mdplayer_bin.h <<'PY'
import sys
d = open(sys.argv[1],'rb').read()
l = ["// Généré depuis mdplayer.s par build_player.sh — NE PAS ÉDITER.",
     "#ifndef MD_PLAYER_BIN_H", "#define MD_PLAYER_BIN_H", "#include <stdint.h>",
     "static const uint8_t md_player_bin[] = {"]
for i in range(0, len(d), 12):
    l.append("    " + ", ".join("0x%02X" % b for b in d[i:i+12]) + ",")
l += ["};", "#endif"]
open(sys.argv[2],"w").write("\n".join(l)+"\n")
print("lecteur :", len(d), "octets")
PY
rm -rf "$TMP"
