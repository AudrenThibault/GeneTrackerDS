#ifndef MD_ROM_PROJET_H
#define MD_ROM_PROJET_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
//  IMPORTER UN PROJET DEPUIS UNE ROM GENETRACKER
//
//  ⚠️ POURQUOI UN PLAN, ET PAS DES DECALAGES ECRITS EN DUR.
//  Sur le Mac, verser un morceau regenere des tableaux C et rappelle le
//  compilateur 68000. La DS n'a pas de compilateur : elle doit ECRIRE DANS
//  L'IMAGE. Pour cela la ROM porte un descripteur — la marque
//  « GENETRK-PLAN01 » — qui dit ou sont les zones et ce qu'elles peuvent
//  contenir. On le retrouve en balayant le fichier.
//
//  Rien n'est donc fige de ce cote : la disposition peut changer dans le
//  projet Mega Drive sans casser l'outil d'ici.
//
//  ⚠️ LES ENTIERS DE LA ROM SONT EN GROS-BOUTIEN. Le 68000 range l'octet de
//  poids fort en premier, l'ARM de la DS l'inverse : toute lecture de 16 ou 32
//  bits doit passer par les fonctions ci-dessous, jamais par un cast.
// ============================================================================

#define MD_ROM_MARQUE "GENETRK-PLAN01"

typedef struct {
  uint32_t morceaux_n, morceaux_nom, morceaux_taille, morceaux_offset;
  uint32_t morceaux_data, morceaux_capacite, morceaux_max;
  uint32_t pcm_offset, pcm_longueur, pcm_note, pcm_boucle, pcm_nom;
  uint32_t pcm_banque, pcm_capacite, pcm_utilise, pcm_max;
} md_rom_plan_t;

// Trouve le descripteur. Rend 0 si l'image n'en porte pas : c'est alors une
// ROM d'avant le plan, ou pas une ROM GeneTracker du tout.
int md_rom_plan_lit(const uint8_t *rom, uint32_t taille, md_rom_plan_t *p);

// Combien de morceaux la ROM porte, et comment ils s'appellent.
int  md_rom_morceaux(const uint8_t *rom, const md_rom_plan_t *p);
void md_rom_nom(const uint8_t *rom, const md_rom_plan_t *p, int i, char nom[11]);

// Verse le morceau `i` dans le tracker : morceau, instruments, tables, et les
// echantillons qu'il emploie. Rend 0 en cas d'echec.
int md_rom_projet_importe(const uint8_t *rom, const md_rom_plan_t *p, int i);

#ifdef __cplusplus
}
#endif
#endif
