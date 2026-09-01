// ============================================================================
//  Export d'une ROM Mega Drive — version Nintendo DS.
//
//  Meme cartouche que sur l'iPad, fabriquee autrement : la DS n'a pas les
//  quelques megaoctets qu'il faudrait pour tenir la ROM entiere en memoire, et
//  elle ne peut pas rester figee plusieurs minutes pendant que le morceau se
//  deroule. L'export est donc DECOUPE : il ecrit directement sur la carte, une
//  image a la fois, et rend la main entre chaque paquet pour que l'ecran vive.
// ============================================================================
#ifndef MD_ROM_H
#define MD_ROM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  int      frames;        // images exportées (60 par seconde)
  double   seconds;       // durée du morceau exporté
  long     writes;        // écritures de registres au total
  uint32_t rom_bytes;     // taille de la cartouche produite
  bool     pcm_included;  // le convertisseur est-il du voyage ?
  int      dac_frames;    // images portant un bloc de convertisseur
  bool     truncated;     // le morceau a-t-il été coupé, faute de place ?
  int      hz;            // cadence du journal : 50 (PAL) ou 60 (NTSC)
  int      dac_block;     // échantillons de convertisseur par image
} md_rom_report_t;

typedef struct md_rom_export_s md_rom_export_t;

// Ouvre `chemin` et prépare l'export du morceau CHARGÉ. La lecture doit être
// lancée (md_replayer_play) : l'export la déroule lui-même.
// `sample_rate` est la cadence de mixage du moteur (celle passée à
// md_replayer_init), pas 44100 : une image, c'est une fraction DE CELLE-LÀ.
//
// `hz` est la cadence VIDÉO de la console visée : 60 pour une Mega Drive NTSC,
// 50 pour une PAL. Le lecteur avance d'une image de journal par retour vertical
// — il ne peut pas faire autrement, c'est sa seule horloge — donc c'est ici que
// la vitesse du morceau se décide. Un journal à 60 Hz joué sur une console PAL
// traîne de 20 %, et ça s'entend énormément.
md_rom_export_t *md_rom_export_begin(const char *chemin, const char *titre,
                                     int sample_rate, int hz);

// Déroule `images` images. Renvoie 1 s'il en reste, 0 si le morceau a bouclé,
// -1 en cas d'erreur d'écriture.
int md_rom_export_step(md_rom_export_t *e, int images);

// Combien d'images sont déjà passées, et combien d'octets de journal écrits.
int md_rom_export_frames(const md_rom_export_t *e);
int md_rom_export_max_frames(const md_rom_export_t *e);
uint32_t md_rom_export_bytes(const md_rom_export_t *e);

// Referme la cartouche (en-tête et somme de contrôle) et libère `e`.
bool md_rom_export_end(md_rom_export_t *e, md_rom_report_t *rapport);

// Abandonne : referme et efface le fichier, libère `e`.
void md_rom_export_abort(md_rom_export_t *e);

#ifdef __cplusplus
}
#endif

#endif
