#!/bin/bash
# ============================================================================
#  Compilation de GeneTrackerDS.
#
#  Pourquoi un script et pas un Makefile : le chemin du projet contient des
#  espaces (« Megadrive ipad tracker »), et les regles de devkitARM ne les
#  supportent pas — make coupe le chemin au premier espace. Un script cite
#  correctement ses chemins.
#
#  Ce projet est AUTONOME. Le moteur vit dans moteur/, ici meme. Il vient a
#  l'origine du tracker iPad, mais c'est une COPIE : les deux projets n'ont plus
#  aucun lien. Une modification ici n'atteint pas l'iPad, et l'inverse est vrai.
#  Le seul contrat entre eux est le FORMAT DES FICHIERS : un projet fait sur
#  l'iPad doit s'ouvrir ici, et reciproquement.
# ============================================================================
set -e
export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
export DEVKITARM=$DEVKITPRO/devkitARM
PATH=$DEVKITARM/bin:$PATH

# ── Pourquoi libnds 1.8.3 et non le libnds 2.0 installe sur la machine ──────
# nds-bootstrap, le chargeur qu'utilise TWiLight Menu++ sur la vraie console,
# ne sait pas demarrer les cartouches produites par libnds 2.0 / calico : il
# affiche « an error has occurred » et s'arrete. Le defaut est connu et
# toujours ouvert (DS-Homebrew/nds-bootstrap, issue #1777, fevrier 2025), et
# il frappe aussi l'exemple « hello world » officiel de devkitPro, verifie ici
# sur la console : lui non plus ne demarre pas.
#
# On compile donc contre libnds 1.8.3, la derniere version d'avant la
# cassure, rangee dans outils/. Elle n'est PAS installee sur le systeme :
# le devkitPro de la machine reste intact, et les autres projets avec lui.
LIBNDS="$(cd "$(dirname "$0")" && pwd)/outils/libnds1/opt/devkitpro/libnds"
if [ ! -d "$LIBNDS/include" ]; then
  echo "libnds 1.8.3 absent de outils/ — voir outils/LISEZMOI.txt" >&2
  exit 1
fi

ICI="$(cd "$(dirname "$0")" && pwd)"
MOTEUR="$ICI/moteur"
OBJ="$ICI/build"
CIBLE="$ICI/GeneTrackerDS"

ARCH="-march=armv5te -mtune=arm946e-s"
# En tableau, pas en chaine : les chemins contiennent des espaces et une
# simple variable serait redecoupee dessus.
INC=(-iquote "$ICI/include" -iquote "$MOTEUR" -I"$LIBNDS/include")
COMMUN="-g -O3 -funroll-loops -fomit-frame-pointer -Wall -ffunction-sections -fdata-sections $ARCH -D__NDS__ -DARM9 -DMD_TARGET_NDS=1"
CF="$COMMUN"
CXXF="$COMMUN -fno-rtti -fno-exceptions -std=gnu++17"

mkdir -p "$OBJ"
OBJETS=()

compile () {   # $1 = fichier source
  local f="$1" b
  b="$OBJ/$(basename "${f%.*}").o"
  case "$f" in
    *.cpp) arm-none-eabi-g++ $CXXF "${INC[@]}" -c "$f" -o "$b" ;;
    *.c)   arm-none-eabi-gcc $CF   "${INC[@]}" -c "$f" -o "$b" ;;
  esac
  OBJETS+=("$b")
}

echo "  compilation du moteur (partage avec l'iPad)..."
# ROM/ : l'exportateur de cartouche Mega Drive, copie du projet iPad. Le
# fichier .s qui l'accompagne est le lecteur 68000 ; il n'est PAS compile ici —
# il l'a ete une fois, et son binaire vit dans mdplayer_bin.h.
for f in "$MOTEUR"/CustomReplayer/*.c "$MOTEUR"/emu76489/*.c "$MOTEUR"/ROM/*.c \
         "$MOTEUR"/ymfm/*.cpp "$MOTEUR"/MegaDrive/*.cpp \
         "$MOTEUR"/MegaDrive/*.c; do
  [ -e "$f" ] || continue
  echo "    $(basename "$f")"
  compile "$f"
done

echo "  compilation de la partie DS..."
for f in "$ICI"/source/*.c "$ICI"/source/*.cpp; do
  [ -e "$f" ] || continue
  echo "    $(basename "$f")"
  compile "$f"
done

echo "  compilation du noyau ARM7..."
# L'ARM7 tient le son, les touches et l'alimentation. libnds 2.0 le fournissait
# tout fait ; en 1.8.3 chaque projet compile le sien.
arm-none-eabi-gcc -g -O2 -Wall -mcpu=arm7tdmi -mtune=arm7tdmi \
  -ffunction-sections -fdata-sections -D__NDS__ -DARM7 \
  -I"$LIBNDS/include" -c "$ICI/arm7/main.c" -o "$OBJ/arm7.o"
arm-none-eabi-gcc -specs=ds_arm7_iwram.specs -g -mcpu=arm7tdmi -mtune=arm7tdmi \
  -Wl,--gc-sections "$OBJ/arm7.o" -L"$LIBNDS/lib" -lnds7 -o "$CIBLE.arm7.elf"

echo "  edition de liens (ARM9)..."
arm-none-eabi-g++ -specs=ds_arm9.specs -g $ARCH \
  -Wl,--gc-sections -Wl,-Map,"$OBJ/GeneTrackerDS.map" \
  "${OBJETS[@]}" -L"$LIBNDS/lib" -lfat -lnds9 -o "$CIBLE.elf"

# En-tete de 0x4000 : la cartouche se declare titre DSi, ce qui lui vaut le
# processeur a 134 MHz au lieu de 67.
#
# Ce format echouait au demarrage avant — mais DEUX choses changeaient alors
# en meme temps : l'en-tete et calico. Le coupable etait calico, prouve par
# l'exemple officiel de devkitPro qui echouait pareil. Si la console refusait
# a nouveau, repasser a 0x200 : la cartouche redevient un titre DS pur, elle
# demarre a coup sur, mais tourne a 67 MHz.
echo "  fabrication de la cartouche..."
ndstool -c "$CIBLE.nds" -h 0x4000 -9 "$CIBLE.elf" -7 "$CIBLE.arm7.elf" \
  -b "$LIBNDS/icon.bmp" "GeneTrackerDS;Mega Drive tracker;YM2612 + SN76489" \
  >/dev/null

# ndstool 2.3.1 n'a aucune option pour le code unite : il le tenait de calico.
# On le pose donc a la main. 0x02 = « DS et DSi » : la console lance la
# cartouche en mode DSi, ce qui donne le processeur a 134 MHz au lieu de 67 —
# et permet aussi de la tester dans melonDS a la vraie vitesse.
python3 - "$CIBLE.nds" <<'FIN'
import sys
c = open(sys.argv[1], 'r+b')
c.seek(0x12); c.write(b'\x02'); c.close()
FIN
ndstool -f "$CIBLE.nds" >/dev/null   # recalcule la somme de controle d'en-tete

ls -l "$CIBLE.nds" | awk '{printf "  -> %s  (%d octets)\n", $NF, $5}'
