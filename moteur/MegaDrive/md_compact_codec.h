// ============================================================================
//  LE CODEC DU FORMAT COMPACT MEGA DRIVE — copie conforme.
//
//  ⚠️ COPIE, PAS DEPENDANCE. Ce fichier vient de
//  « native megadrive Tracker/moteur/morceau/md_codec.c ». Les deux projets
//  restent independants ; en echange, une correction la-bas doit etre reportee
//  ici, sinon les fichiers echanges ne se relisent plus.
//
//  Seule difference avec l'original : l'en-tete des constantes s'appelle
//  md_compact.h ici, et md_song.h la-bas.
// ============================================================================
// ============================================================================
//  Comprimer un morceau, pour en loger plusieurs dans les 32 Ko de sauvegarde.
//
//  Deux étages, et chacun a sa raison :
//
//  1. UN CODAGE CREUX. La forme d'édition fait 28 224 octets dont 90 % de
//     vide : des phrases jamais écrites, des lignes de SONG jamais atteintes.
//     On ne range que ce qui est POSÉ, avec des masques de présence. Mesuré
//     sur LA DIFFE : 28 224 -> 4 302 octets.
//
//     ⚠️ Le SONG est DENSE, pas creux : on remplit des lignes consécutives.
//     Le coder en paires (ligne, valeur) gaspillerait un octet sur deux — on
//     range donc sa longueur utile puis les valeurs brutes. Deux fois moins.
//
//  2. LZSS par-dessus. 4 302 -> 2 727 octets, soit onze morceaux de cette
//     taille dans la sauvegarde, et plusieurs dizaines de morceaux courts.
//
//  Ce qu'on N'A PAS fait, et pourquoi : un codage d'entropie (Huffman) ferait
//  gagner encore un tiers — zlib atteint 2 121 — mais il faudrait aussi le
//  COMPRESSEUR sur le 68000. Onze morceaux valent mieux qu'un chantier.
//
//  Ce fichier se compile TEL QUEL sur le Mac : c'est ainsi qu'on éprouve
//  l'aller-retour sur les vrais morceaux avant d'aller sur la console.
// ============================================================================
#ifndef MD_COMPACT_CODEC_H
#define MD_COMPACT_CODEC_H

#include <stdint.h>

// Remet un tampon de morceau à l'état NEUF. Les valeurs neutres diffèrent
// selon les champs — 0 pour une note, MDC_VIDE pour une commande — d'où une
// fonction plutôt qu'un memset qui serait faux.
// L'instrument neuf, celui de la DS : algorithme 4, OP1 qui MODULE OP2.
// ⚠️ C'est le cœur du timbre. Un instrument neuf en algorithme 7 — les quatre
// opérateurs en parallèle, à niveau plein — ne module rien du tout : bouger un
// multiple ne fait alors que désaccorder une sinusoïde parmi quatre, et le
// tracker paraît incapable de faire un son sale.
void md_instr_defaut(uint8_t *b);

void md_codec_vide(uint8_t *morceau);

// Rendent le nombre d'octets produits, ou 0 en cas de dépassement.
uint32_t md_codec_comprime(const uint8_t *morceau, uint8_t *sortie, uint32_t place);
uint32_t md_codec_decomprime(const uint8_t *entree, uint32_t taille, uint8_t *morceau);

#endif
