// ============================================================================
//  Export d'un fichier VGM. Voir md_vgm.h pour le pourquoi.
//
//  Un VGM se lit dans l'ordre : en-tête de 256 octets, puis une banque
//  d'échantillons, puis les commandes. Or on ne connaît la taille de la banque
//  qu'une fois le morceau déroulé. On écrit donc les deux flux dans deux
//  fichiers temporaires, et on les recopie dans l'ordre à la fin.
//
//  Le minutage du VGM est compté en 44 100e de seconde, quelle que soit la
//  cadence de qui l'a produit. Une image vaut donc 735 unités à 60 Hz et 882 à
//  50 Hz.
//
//  ── Le convertisseur passe par un FLUX DAC ────────────────────────────────
//  Il y a deux façons légales d'écrire du PCM dans un VGM :
//
//    • un octet à la fois, commande 0x8n, qui écrit dans le registre 2A et
//      attend n unités. C'est ce qu'on faisait, et c'est ce qu'écrit DefleMask.
//    • un FLUX (commandes 0x90 à 0x95) : on déclare une fois la puce visée et
//      la cadence, puis on dit « joue le bloc numéro tant ». La console débite
//      ensuite toute seule, à la cadence exacte qu'on a annoncée.
//
//  La banque est découpée en UN BLOC PAR RAFALE — une suite d'images qui
//  sonnent d'affilée. C'est le découpage naturel : une frappe de batterie, un
//  bloc. Les deux autres découpages ne tiennent pas :
//    • un seul bloc pour tout le morceau obligerait à écrire du silence pour
//      les images sans PCM — cinq mégaoctets pour trois minutes ;
//    • un bloc par image dépasse le plafond des lecteurs, qui comptent en
//      ÉCHANTILLONS : KokonoePlayer-Lite en accepte 256, et un morceau en
//      demanderait des milliers. Mesuré sur un vrai morceau : 453 images, 275
//      avec du PCM, mais seulement SEPT rafales.
//
//  C'est la seconde qui est employée ici. Trois raisons :
//    1. la cadence est ANNONCÉE, au hertz près, au lieu d'être approchée par
//       des attentes de 1 ou 2 unités entre chaque échantillon ;
//    2. le fichier est bien plus court — onze octets de commande par image au
//       lieu de 444 ;
//    3. KokonoePlayer-Lite, le seul lecteur Mega Drive matériel qu'on ait
//       trouvé qui joue juste, n'accepte QUE cette forme-là : avec l'autre il
//       répond « 0 songs » et refuse le fichier entier.
// ============================================================================

#include "md_vgm.h"
#include "../CustomReplayer/md_replayer.h"
#include "../MegaDrive/md_chip.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VGM_ENTETE     0x100
#define VGM_HZ         44100
#define MD_DAC_MAX     560
#define MD_DAC_SILENCE 0x80    // le zero d'un convertisseur non signe
#define MD_DAC_DIVISEUR 288
#define MD_DAC_HZ      (MD_YM2612_CLOCK / MD_DAC_DIVISEUR)

// Une écriture de registre, avec SA PLACE dans l'image.
//
// Elles étaient toutes posées au début de l'image, en bloc. Le convertisseur,
// lui, garde sa position exacte : une note qui tombe au milieu d'une image
// s'entendait donc jusqu'à une image AVANT la frappe de batterie de la même
// ligne. C'est ce décalage qu'on entend comme « le PCM est en retard ».
//
// Le VGM sait dater : entre deux écritures on peut insérer une attente. On note
// donc, pour chaque écriture, à quel échantillon de l'image elle est arrivée —
// le crochet du convertisseur bat une fois par échantillon, il sert d'horloge.
#define MD_VGM_ECR_MAX 1024
typedef struct {
  uint16_t pos;                       // échantillon dans l'image
  uint8_t  n;                         // 2 pour le PSG, 3 pour le YM2612
  uint8_t  b[3];
} md_vgm_ecr_t;

typedef struct {
  md_vgm_ecr_t ecr[MD_VGM_ECR_MAX];   // les écritures d'UNE image, datées
  int       ecr_n;
  int       pos;                      // où en est l'image, en échantillons
  int       count;
  int       deborde;
  uint8_t   dac[MD_DAC_MAX];
  int       dac_taille, dac_n, dac_acc, dac_actif;
} md_vgm_capture_t;

struct md_vgm_export_s {
  FILE            *f_cmd, *f_pcm;
  char             chemin[256], t_cmd[256], t_pcm[256];
  char             titre[64];
  md_vgm_capture_t cap;
  int16_t         *pcm;
  int              per_frame, hz, hz100;
  int              attente_img;       // unités VGM pour une image
  int              start_song[MD_MAX_CHANNELS];
  int              start_chain[MD_MAX_CHANNELS];
  int              start_phrase[MD_MAX_CHANNELS];
  int              vivante[MD_MAX_CHANNELS];
  int              bouclee[MD_MAX_CHANNELS];
  int              partie[MD_MAX_CHANNELS];
  int              active, frames, max_frames, fini;
  uint32_t         cmd_len, pcm_len;
  // Les rafales de PCM : une suite d'images qui sonnent d'affilee devient un
  // bloc de donnees, et c'est son NUMERO que la commande 0x95 designe.
  int              rafales;
  int              en_rafale;
  uint32_t         rafale_len[256];
  long             total_att;         // durée totale, en unités VGM
  md_vgm_report_t  rep;
  int              erreur;
};

// ── Capture ────────────────────────────────────────────────────────────────
// Les mêmes trois sources que la cartouche, traduites en commandes VGM :
//   0x50 dd     : SN76489
//   0x52 aa dd  : YM2612, port 0
//   0x53 aa dd  : YM2612, port 1
static void md_vgm_hook(int chip, uint8_t a, uint8_t b, void *ctx) {
  md_vgm_capture_t *cap = (md_vgm_capture_t *)ctx;
  if (chip == 3) {
    // Le convertisseur bat une fois par échantillon de la puce : c'est notre
    // horloge à l'intérieur de l'image.
    cap->pos++;
    if (b) cap->dac_actif = 1;
    cap->dac_acc += MD_YM_DIVISEUR;
    while (cap->dac_acc >= MD_DAC_DIVISEUR) {
      cap->dac_acc -= MD_DAC_DIVISEUR;
      if (cap->dac_n < cap->dac_taille) cap->dac[cap->dac_n++] = a;
    }
    return;
  }
  if (cap->ecr_n >= MD_VGM_ECR_MAX) { cap->deborde = 1; return; }
  md_vgm_ecr_t *e = &cap->ecr[cap->ecr_n++];
  e->pos = (uint16_t)cap->pos;
  if (chip == 2) { e->n = 2; e->b[0] = 0x50; e->b[1] = a; }
  else { e->n = 3; e->b[0] = chip ? 0x53 : 0x52; e->b[1] = a; e->b[2] = b; }
  cap->count++;
}

static void ecrit(FILE *f, uint32_t *len, int *err, const void *p, size_t n) {
  if (*err || !n) return;
  if (fwrite(p, 1, n, f) != n) { *err = 1; return; }
  *len += (uint32_t)n;
}

static void pose32(uint8_t *p, uint32_t v) {   // le VGM est petit-boutiste
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

// ── L'etiquette GD3 ────────────────────────────────────────────────────────
// C'est elle qui porte le nom du morceau. Sans elle, un lecteur VGM et le
// jukebox XGM n'affichent qu'un numero — on ne sait plus lequel on ecoute.
// Onze chaines, dans un ordre fixe, chacune en UTF-16 et terminee par un zero.
static uint32_t gd3_chaine(uint8_t *p, const char *s) {
  uint32_t n = 0;
  if (s) for (; *s; s++) { p[n++] = (uint8_t)*s; p[n++] = 0; }
  p[n++] = 0; p[n++] = 0;
  return n;
}

static uint32_t gd3_construit(uint8_t *p, uint32_t cap, const char *titre) {
  if (cap < 256) return 0;
  uint32_t n = 12;                               // on remplit l'en-tete apres
  n += gd3_chaine(p + n, titre);                 // titre, en anglais
  n += gd3_chaine(p + n, "");                    // titre, en japonais
  n += gd3_chaine(p + n, "");                    // jeu
  n += gd3_chaine(p + n, "");
  n += gd3_chaine(p + n, "Sega Mega Drive");     // systeme
  n += gd3_chaine(p + n, "");
  n += gd3_chaine(p + n, "");                    // auteur
  n += gd3_chaine(p + n, "");
  n += gd3_chaine(p + n, "");                    // date de sortie
  n += gd3_chaine(p + n, "GeneTrackerDS");       // qui a converti
  n += gd3_chaine(p + n, "");                    // remarques
  memcpy(p, "Gd3 ", 4);
  pose32(p + 4, 0x00000100);                     // version
  pose32(p + 8, n - 12);                         // longueur des chaines
  return n;
}

md_vgm_export_t *md_vgm_export_begin(const char *chemin, const char *titre,
                                     int sample_rate, int hz) {
  if (!chemin || sample_rate <= 0) return NULL;
  if (hz != 50 && hz != 60) hz = 60;

  md_vgm_export_t *e = (md_vgm_export_t *)calloc(1, sizeof(*e));
  if (!e) return NULL;
  strncpy(e->chemin, chemin, sizeof(e->chemin) - 1);
  if (titre) strncpy(e->titre, titre, sizeof(e->titre) - 1);
  snprintf(e->t_cmd, sizeof(e->t_cmd), "%s.TMC", chemin);
  snprintf(e->t_pcm, sizeof(e->t_pcm), "%s.TMP", chemin);

  e->hz = hz;
  e->hz100 = (hz == 50) ? 4970 : 5992;
  e->per_frame = (int)(((long)sample_rate * 100) / e->hz100);
  e->cap.dac_taille = (int)(((long)MD_DAC_HZ * 100) / e->hz100);
  e->attente_img = (hz == 50) ? 882 : 735;

  e->pcm = (int16_t *)malloc((size_t)e->per_frame * 2 * sizeof(int16_t));
  if (!e->pcm) { free(e); return NULL; }
  e->f_cmd = fopen(e->t_cmd, "wb");
  e->f_pcm = fopen(e->t_pcm, "wb");
  if (!e->f_cmd || !e->f_pcm) {
    if (e->f_cmd) fclose(e->f_cmd);
    if (e->f_pcm) fclose(e->f_pcm);
    remove(e->t_cmd); remove(e->t_pcm);
    free(e->pcm); free(e); return NULL;
  }

  // ── Le flux du convertisseur, déclaré une fois pour toutes ─────────────
  // 0x90 : le flux 0 écrit dans le YM2612 (puce 02), port 0, registre 2A.
  // 0x91 : il puise dans la banque 0, un octet par pas.
  // 0x92 : sa cadence, en hertz — celle-là même que le moteur a produite.
  { uint8_t d[5] = { 0x90, 0x00, 0x02, 0x00, 0x2A };
    ecrit(e->f_cmd, &e->cmd_len, &e->erreur, d, 5); }
  { uint8_t d[5] = { 0x91, 0x00, 0x00, 0x01, 0x00 };
    ecrit(e->f_cmd, &e->cmd_len, &e->erreur, d, 5); }
  { const uint32_t hz_dac = (uint32_t)(((long)e->cap.dac_taille * e->hz100) / 100);
    uint8_t d[6] = { 0x92, 0x00, 0, 0, 0, 0 };
    pose32(d + 2, hz_dac);
    ecrit(e->f_cmd, &e->cmd_len, &e->erreur, d, 6); }

  for (int c = 0; c < MD_MAX_CHANNELS; c++) {
    e->start_song[c]   = md_replayer_play_song_row(c);
    e->start_chain[c]  = md_replayer_play_chain_row(c);
    e->start_phrase[c] = md_replayer_play_phrase_row(c);
    e->vivante[c] = (e->start_song[c] >= 0);
    if (e->vivante[c]) e->active++;
  }
  e->max_frames = hz * 60 * 6;
  md_chip_set_write_hook(md_vgm_hook, &e->cap);
  return e;
}

int md_vgm_export_frames(const md_vgm_export_t *e) { return e ? e->frames : 0; }
uint32_t md_vgm_export_bytes(const md_vgm_export_t *e) {
  return e ? VGM_ENTETE + e->pcm_len + e->cmd_len : 0;
}

int md_vgm_export_step(md_vgm_export_t *e, int images) {
  if (!e) return -1;
  if (e->erreur) return -1;
  if (e->fini) return 0;

  for (int n = 0; n < images && e->frames < e->max_frames; n++, e->frames++) {
    md_vgm_capture_t *cap = &e->cap;
    cap->ecr_n = 0; cap->count = 0; cap->dac_n = 0; cap->dac_actif = 0;
    cap->pos = 0;
    md_replayer_update((unsigned char *)e->pcm, (unsigned)(e->per_frame * 4));

    if (cap->dac_actif && cap->dac_n > 0) {
      // Chaque image apporte exactement une image d'echantillons ; ce qui
      // manque est du silence, jamais un trou.
      for (int i = cap->dac_n; i < cap->dac_taille; i++)
        cap->dac[i] = MD_DAC_SILENCE;
      if (!e->en_rafale) {
        // Une rafale commence : on ouvre un bloc et on dit de le jouer.
        if (e->rafales < (int)(sizeof(e->rafale_len)/sizeof(e->rafale_len[0]))) {
          const int id = e->rafales++;
          e->rafale_len[id] = 0;
          uint8_t d[5] = { 0x95, 0x00, (uint8_t)id, (uint8_t)(id >> 8), 0x00 };
          ecrit(e->f_cmd, &e->cmd_len, &e->erreur, d, 5);
          e->en_rafale = 1;
        }
      }
      if (e->en_rafale) {
        ecrit(e->f_pcm, &e->pcm_len, &e->erreur, cap->dac,
              (size_t)cap->dac_taille);
        e->rafale_len[e->rafales - 1] += (uint32_t)cap->dac_taille;
      }
    } else {
      e->en_rafale = 0;                  // la rafale se referme
    }
    // ── Les écritures, CHACUNE A SA PLACE ────────────────────────────────
    // On avance dans l'image en insérant des attentes : une écriture arrivée au
    // tiers de l'image se retrouve au tiers de l'image, et non à son début.
    // Le total des attentes vaut exactement une image, sans quoi le morceau
    // dériverait un peu plus à chaque seconde.
    {
      const int total = (cap->pos > 0) ? cap->pos : e->cap.dac_taille;
      long ecoule = 0;
      for (int k = 0; k < cap->ecr_n; k++) {
        long cible = ((long)cap->ecr[k].pos * e->attente_img) / total;
        if (cible > e->attente_img) cible = e->attente_img;
        if (cible > ecoule) {
          const long dt = cible - ecoule;
          uint8_t d[3] = { 0x61, (uint8_t)dt, (uint8_t)(dt >> 8) };
          ecrit(e->f_cmd, &e->cmd_len, &e->erreur, d, 3);
          ecoule = cible;
        }
        ecrit(e->f_cmd, &e->cmd_len, &e->erreur, cap->ecr[k].b, cap->ecr[k].n);
      }
      const long reste = e->attente_img - ecoule;
      if (reste > 0) {
        uint8_t d[3] = { 0x61, (uint8_t)reste, (uint8_t)(reste >> 8) };
        ecrit(e->f_cmd, &e->cmd_len, &e->erreur, d, 3);
      }
    }
    e->total_att += e->attente_img;
    e->rep.writes += cap->count;
    if (cap->deborde) e->erreur = 1;
    if (e->erreur) return -1;

    int done = 1;
    for (int c = 0; c < MD_MAX_CHANNELS; c++) {
      if (!e->vivante[c] || e->bouclee[c]) continue;
      const int rs = md_replayer_play_song_row(c);
      const int rc = md_replayer_play_chain_row(c);
      const int rp = md_replayer_play_phrase_row(c);
      const int auDepart = (rs == e->start_song[c] && rc == e->start_chain[c] &&
                            rp == e->start_phrase[c]);
      if (!auDepart) e->partie[c] = 1;
      else if (e->partie[c]) e->bouclee[c] = 1;
      if (!e->bouclee[c]) done = 0;
    }
    if (e->active == 0 && e->frames >= e->hz) { e->frames++; e->fini = 1; break; }
    if (done && e->active > 0) { e->frames++; e->fini = 1; break; }
  }
  if (e->frames >= e->max_frames) e->fini = 1;
  return e->fini ? 0 : 1;
}

static void ferme_et_efface(md_vgm_export_t *e) {
  if (e->f_cmd) fclose(e->f_cmd);
  if (e->f_pcm) fclose(e->f_pcm);
  remove(e->t_cmd); remove(e->t_pcm);
  free(e->pcm); free(e);
}

bool md_vgm_export_end(md_vgm_export_t *e, md_vgm_report_t *rapport) {
  if (!e) return false;
  md_chip_set_write_hook(NULL, NULL);

  { uint8_t fin = 0x66;                       // fin du morceau
    ecrit(e->f_cmd, &e->cmd_len, &e->erreur, &fin, 1); }
  fclose(e->f_cmd); e->f_cmd = NULL;
  fclose(e->f_pcm); e->f_pcm = NULL;
  if (e->erreur) { ferme_et_efface(e); return false; }

  // Le bloc de données qui porte la banque : 0x67 0x66 0x00 puis sa taille.
  // La banque est écrite en BLOCS, un par rafale, chacun avec son en-tête
  // « 0x67 0x66 0x00 taille » : c'est leur ordre qui leur donne leur numéro.
  const uint32_t nb_blocs = (uint32_t)e->rafales;
  uint32_t bloc = 0;
  for (uint32_t i = 0; i < nb_blocs; i++) bloc += 7 + e->rafale_len[i];
  const uint32_t debut = VGM_ENTETE;                 // début des commandes
  const uint32_t fin_cmd = VGM_ENTETE + bloc + e->cmd_len;

  static uint8_t gd3[512];
  const uint32_t gd3_len = gd3_construit(gd3, sizeof(gd3), e->titre);
  const uint32_t taille = fin_cmd + gd3_len;

  uint8_t h[VGM_ENTETE];
  memset(h, 0, sizeof(h));
  memcpy(h, "Vgm ", 4);
  pose32(h + 0x04, taille - 4);                      // fin du fichier
  pose32(h + 0x08, 0x00000161);                      // version 1.61
  pose32(h + 0x0C, MD_PSG_CLOCK);
  if (gd3_len) pose32(h + 0x14, fin_cmd - 0x14);     // où trouver l'étiquette
  pose32(h + 0x18, (uint32_t)e->total_att);          // durée totale
  // Le morceau boucle sur lui-même : c'est ce que fait le tracker, et les
  // lecteurs comme les convertisseurs XGM s'en servent.
  pose32(h + 0x1C, (debut + bloc) - 0x1C);
  pose32(h + 0x20, (uint32_t)e->total_att);
  pose32(h + 0x24, (uint32_t)e->hz);
  h[0x28] = 0x09; h[0x29] = 0x00;                    // SN76489 : polynôme
  h[0x2A] = 16;                                      // ...largeur du registre
  pose32(h + 0x2C, MD_YM2612_CLOCK);
  pose32(h + 0x34, VGM_ENTETE - 0x34);               // début des données

  FILE *o = fopen(e->chemin, "wb");
  if (!o) { ferme_et_efface(e); return false; }
  int ok = fwrite(h, 1, VGM_ENTETE, o) == VGM_ENTETE;

  static uint8_t tampon[1024];
  if (ok && nb_blocs) {
    FILE *s = fopen(e->t_pcm, "rb");
    if (!s) ok = 0;
    for (uint32_t b = 0; ok && b < nb_blocs; b++) {
      uint8_t d[7] = { 0x67, 0x66, 0x00, 0, 0, 0, 0 };
      pose32(d + 3, e->rafale_len[b]);
      if (fwrite(d, 1, 7, o) != 7) { ok = 0; break; }
      long reste = (long)e->rafale_len[b];
      while (ok && reste > 0) {
        const int veut = reste > (long)sizeof(tampon) ? (int)sizeof(tampon)
                                                      : (int)reste;
        const size_t n = fread(tampon, 1, (size_t)veut, s);
        if ((int)n != veut) { ok = 0; break; }
        if (fwrite(tampon, 1, n, o) != n) ok = 0;
        reste -= veut;
      }
    }
    if (s) fclose(s);
  }
  if (ok) {
    FILE *s = fopen(e->t_cmd, "rb");
    if (!s) ok = 0;
    while (ok && s) {
      size_t n = fread(tampon, 1, sizeof(tampon), s);
      if (!n) break;
      if (fwrite(tampon, 1, n, o) != n) ok = 0;
    }
    if (s) fclose(s);
  }
  if (ok && gd3_len) ok = fwrite(gd3, 1, gd3_len, o) == gd3_len;
  if (fclose(o) != 0) ok = 0;
  if (!ok) { remove(e->chemin); ferme_et_efface(e); return false; }

  e->rep.frames = e->frames;
  e->rep.seconds = e->frames * 100.0 / e->hz100;
  e->rep.bytes = taille;
  e->rep.pcm_bytes = e->pcm_len;
  if (rapport) *rapport = e->rep;
  ferme_et_efface(e);
  return true;
}

void md_vgm_export_abort(md_vgm_export_t *e) {
  if (!e) return;
  md_chip_set_write_hook(NULL, NULL);
  ferme_et_efface(e);
}
