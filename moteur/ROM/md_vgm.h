// ============================================================================
//  Export d'un fichier VGM.
//
//  Un VGM, c'est exactement ce que la cartouche contient déjà : le journal de
//  toutes les écritures qui partent vers le YM2612 et le SN76489, avec leur
//  minutage. La différence est le CONTENANT — un format ouvert, que lisent les
//  lecteurs VGM, les émulateurs, et les outils qui fabriquent des ROMs de
//  jukebox (XGMRomBuilder). Le morceau peut donc voyager à côté de VGM
//  récupérés ailleurs.
//
//  Découpé en tranches comme l'export ROM : la DS ne peut pas rester figée
//  pendant que le morceau se déroule.
// ============================================================================
#ifndef MD_VGM_H
#define MD_VGM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  int      frames;       // images exportées
  double   seconds;      // durée
  long     writes;       // écritures de registres
  uint32_t bytes;        // taille du fichier produit
  uint32_t pcm_bytes;    // taille de la banque d'échantillons
} md_vgm_report_t;

typedef struct md_vgm_export_s md_vgm_export_t;

// `chemin` est le .VGM final. Deux fichiers temporaires sont créés à côté puis
// effacés : le VGM veut sa banque d'échantillons AVANT les commandes, et on ne
// connaît sa taille qu'à la fin.
// `hz` fixe la durée d'une image, 60 ou 50.
// `titre` devient l'etiquette GD3 du fichier : c'est ce que les lecteurs VGM et
// le jukebox XGM affichent a cote du morceau. Sans elle, la liste ne montre que
// des numeros.
md_vgm_export_t *md_vgm_export_begin(const char *chemin, const char *titre,
                                     int sample_rate, int hz);
int      md_vgm_export_step(md_vgm_export_t *e, int images);
int      md_vgm_export_frames(const md_vgm_export_t *e);
uint32_t md_vgm_export_bytes(const md_vgm_export_t *e);
bool     md_vgm_export_end(md_vgm_export_t *e, md_vgm_report_t *rapport);
void     md_vgm_export_abort(md_vgm_export_t *e);

#ifdef __cplusplus
}
#endif

#endif
