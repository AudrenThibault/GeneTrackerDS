#ifndef MDC_COMPACT_H
#define MDC_COMPACT_H
#include <stdint.h>

// ============================================================================
//  LE FORMAT COMPACT DE LA MEGA DRIVE — les constantes, et rien d'autre.
//
//  ⚠️ COPIE, PAS DEPENDANCE. Ces valeurs viennent de
//  « native megadrive Tracker/moteur/morceau/md_song.h ». Les deux projets
//  restent strictement independants : on copie le code, on ne s'y refere
//  jamais. En contrepartie, TOUTE MODIFICATION LA-BAS DOIT ETRE REPORTEE ICI —
//  c'est le format d'un fichier qu'on echange, il ne peut pas diverger.
//
//  ⚠️ PREFIXE MDC_, ET CE N'EST PAS COSMETIQUE. Le replayer de la DS definit
//  deja MD_MAX_CHAINS, MD_MAX_PHRASES, MD_MAX_TABLES — avec D'AUTRES VALEURS.
//  La DS tient 128 chaines, 255 phrases, 255 instruments et 32 tables ; la
//  Mega Drive 96, 160, 32 et 16. Laisser les deux jeux se recouvrir donnerait
//  un import qui deborde en silence. Le prefixe force a savoir de quel format
//  on parle a chaque ligne.
//
//  Consequence a retenir : importer d'une ROM ne peut pas deborder, tout y
//  tient. EXPORTER, si — un projet DS peut depasser ce que la cartouche sait
//  porter, et il faudra le DIRE plutot que de tronquer.
//
//  Ce que la Mega Drive appelle un « morceau » tient en 32 576 octets a plat.
//  C'est ce bloc-la que la ROM porte, comprime, et que la DS doit savoir lire
//  et ecrire pour importer ou exporter un projet.
// ============================================================================

#define MDC_CANAUX          10
#define MDC_SONG_LIGNES     256
#define MDC_MAX_CHAINS      96
#define MDC_LIGNES_CHAIN    16
#define MDC_MAX_PHRASES     160
#define MDC_LIGNES_PHRASE   16
#define MDC_MAX_INSTR       32
#define MDC_MAX_TABLES      16
#define MDC_LIGNES_TABLE    16

#define MDC_VIDE            0xFF   // case vide, pour tous les champs 8 bits

// ── Découpage de la SRAM, en décalages LOGIQUES ───────────────────────────
#define MDC_OFF_ENTETE      0
#define MDC_TAILLE_ENTETE   64

#define MDC_OFF_SONG        (MDC_OFF_ENTETE + MDC_TAILLE_ENTETE)
#define MDC_TAILLE_SONG     (MDC_CANAUX * MDC_SONG_LIGNES)

#define MDC_OFF_CHAINS      (MDC_OFF_SONG + MDC_TAILLE_SONG)
#define MDC_TAILLE_CHAINS   (MDC_MAX_CHAINS * MDC_LIGNES_CHAIN * 2)

#define MDC_OFF_PHRASES     (MDC_OFF_CHAINS + MDC_TAILLE_CHAINS)
#define MDC_PHRASE_OCTETS   7        // note, instr, vel, cmd, val, mdcmd, mdval
#define MDC_TAILLE_PHRASES  (MDC_MAX_PHRASES * MDC_LIGNES_PHRASE * MDC_PHRASE_OCTETS)

#define MDC_OFF_INSTR       (MDC_OFF_PHRASES + MDC_TAILLE_PHRASES)
// ⚠️ 216 ET NON 80 : il fallait la place des MACROS PSG.
//
// Une voix PSG ne tire pas son caractère de son enveloppe à trois points mais
// de ses deux macros — une suite de niveaux et une suite de transpositions,
// déroulées un pas par tick. Sans elles la page PSG n'avait que quatre
// réglages là où la DS en a douze, et un morceau importé perdait tout son
// grain.
//
// Soixante-quatre pas : mesuré sur les morceaux existants, la plus longue
// macro de volume en fait 57 et le plus long arpège 30. Seize — ce que montre
// l'éditeur de la DS — aurait tronqué.
//
//   63      longueur de la macro de volume, 0 = aucune
//   64      son point de bouclage, MDC_VIDE = pas de boucle
//   65-128  ses soixante-quatre pas, niveaux 0-15
//   129     longueur de la macro d'arpège
//   130     son point de bouclage
//   131     arpège FIXE : la valeur est une note, pas un écart
//   132-195 ses soixante-quatre pas, écarts signés
//   196-215 libre
#define MDC_INSTR_OCTETS    216

// ── LES TROIS MACROS PSG ──────────────────────────────────────────────────
// Volume, arpège, et mode de bruit — celle-ci ne sert qu'à la voie NOISE, où
// elle change le grain pendant la note. Le tracker DS en garde 128 pas pour
// les deux premières et 32 pour la troisième ; ici les trente-deux
// instruments doivent tenir dans les 32 Ko de la cartouche, alors elles sont
// coupées à la mesure de ce que les morceaux emploient réellement :
// la plus longue macro de volume de tous les morceaux fait 57 pas, la plus
// longue macro d'arpège 30. D'où 64 et 32.
//
// ⚠️ Ces trois blocs remplissent EXACTEMENT les 216 octets déjà réservés, nom
// compris. Les agrandir demanderait de reprendre MDC_TAILLE_TOTALE, qui n'a
// plus que 192 octets de marge avant les 32 Ko de la sauvegarde.
#define MDC_MACRO_VOL_PAS   64
#define MDC_MACRO_ARP_PAS   32
#define MDC_MACRO_NZ_PAS    32

#define MDC_OFF_VOL_LEN     63
#define MDC_OFF_VOL_BOUCLE  64
#define MDC_OFF_VOL_MAC     65    /* 65..128 */
#define MDC_OFF_ARP_LEN     129
#define MDC_OFF_ARP_BOUCLE  130
#define MDC_OFF_ARP_FIXE    131
#define MDC_OFF_ARP_MAC     132   /* 132..163 */
#define MDC_OFF_NZ_LEN      164
#define MDC_OFF_NZ_BOUCLE   165
#define MDC_OFF_NZ_MAC      166   /* 166..197 */
// ⚠️ Le nom vit ici, PAS en 64 : 64 est le point de bouclage de la macro de
// volume. L'y laisser écrivait des lettres au milieu de la macro.
#define MDC_OFF_NOM         198   /* 198..213, deux octets de rab derrière */
#define MDC_NOM_OCTETS      16
#define MDC_TAILLE_INSTR    (MDC_MAX_INSTR * MDC_INSTR_OCTETS)

#define MDC_OFF_TABLES      (MDC_OFF_INSTR + MDC_TAILLE_INSTR)
#define MDC_TABLE_OCTETS    8
#define MDC_TAILLE_TABLES   (MDC_MAX_TABLES * MDC_LIGNES_TABLE * MDC_TABLE_OCTETS)

#define MDC_TAILLE_TOTALE   (MDC_OFF_TABLES + MDC_TAILLE_TABLES)

#endif
