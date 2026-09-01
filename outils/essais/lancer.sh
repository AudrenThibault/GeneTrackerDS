#!/bin/sh
# Verifie que CHAQUE commande agit, dans les DEUX contextes ou elle peut vivre.
#
#   CMD     : colonne d'une phrase, 1re et 2e colonne d'une table
#   MD CMD  : colonne d'une phrase, colonne d'une table
#
# Le moteur est compile pour le Mac et joue le meme morceau deux fois, avec et
# sans la commande, puis on compare les ondes ECHANTILLON PAR ECHANTILLON.
#
# ⚠️ Comparer l'energie par image ne suffit PAS : un desaccord fin (U) change
# beaucoup la forme d'onde et presque rien l'energie. Avec cette mesure-la, le
# test declarait plusieurs commandes « sans effet » a tort.
set -e
cd "$(dirname "$0")/../.."
T=$(mktemp -d)
for f in moteur/CustomReplayer/*.c moteur/emu76489/*.c; do
  clang -O1 -w -std=c11 -I moteur/CustomReplayer -I moteur/MegaDrive \
        -I moteur/emu76489 -c "$f" -o "$T/$(basename "$f" .c).o"
done
for f in moteur/MegaDrive/*.cpp moteur/ymfm/*.cpp; do
  clang++ -O1 -w -std=c++17 -I moteur/CustomReplayer -I moteur/MegaDrive \
          -I moteur/emu76489 -I moteur/ymfm -c "$f" -o "$T/$(basename "$f" .cpp).o"
done
clang++ -O1 -w -std=c++17 -I moteur/CustomReplayer -I moteur/MegaDrive \
        outils/essais/essai_commandes.cpp "$T"/*.o -o "$T/essai"
"$T/essai"
rm -rf "$T"
