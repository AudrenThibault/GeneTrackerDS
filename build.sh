#!/bin/bash
# ============================================================================
#  Compilation de MD Tracker DS.
#
#  Pourquoi un script et pas un Makefile : le chemin du projet contient des
#  espaces (« Megadrive ipad tracker »), et les regles de devkitARM ne les
#  supportent pas — make coupe le chemin au premier espace. Un script cite
#  correctement ses chemins.
#
#  Le MOTEUR n'est pas recopie ici : il est pris chez le voisin iPad. Un seul
#  exemplaire du code, donc aucune divergence possible entre les deux trackers.
# ============================================================================
set -e
export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
export DEVKITARM=$DEVKITPRO/devkitARM
export CALICO=$DEVKITPRO/calico
PATH=$DEVKITARM/bin:$PATH

ICI="$(cd "$(dirname "$0")" && pwd)"
MOTEUR="$ICI/../MDTracker/Engine"
OBJ="$ICI/build"
CIBLE="$ICI/MDTrackerDS"

ARCH="-march=armv5te -mtune=arm946e-s"
# En tableau, pas en chaine : les chemins contiennent des espaces et une
# simple variable serait redecoupee dessus.
INC=(-iquote "$ICI/include" -iquote "$MOTEUR" -I"$DEVKITPRO/libnds/include" -I"$CALICO/include")
COMMUN="-g -O2 -Wall -ffunction-sections -fdata-sections $ARCH -D__NDS__ -DARM9 -DMD_TARGET_NDS=1"
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
for f in "$MOTEUR"/CustomReplayer/*.c "$MOTEUR"/emu76489/*.c \
         "$MOTEUR"/ymfm/*.cpp "$MOTEUR"/MegaDrive/*.cpp; do
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

echo "  edition de liens..."
arm-none-eabi-g++ -specs="$CALICO/share/ds9.specs" -g $ARCH \
  -Wl,--gc-sections -Wl,-Map,"$OBJ/MDTrackerDS.map" \
  "${OBJETS[@]}" -L"$DEVKITPRO/libnds/lib" -L"$CALICO/lib" -lfat -lnds9 -lcalico_ds9 -o "$CIBLE.elf"

echo "  fabrication de la cartouche..."
ndstool -c "$CIBLE.nds" -9 "$CIBLE.elf" -7 "$CALICO/bin/ds7_maine.elf" \
  -b "$CALICO/share/nds-icon.bmp" "MD Tracker;Mega Drive tracker;YM2612 + SN76489" \
  >/dev/null

ls -l "$CIBLE.nds" | awk '{printf "  -> %s  (%d octets)\n", $NF, $5}'
