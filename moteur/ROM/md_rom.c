// ============================================================================
//  Export d'une ROM Mega Drive — version Nintendo DS.
//
//  Principe : on ne porte PAS le tracker en 68000. On joue le morceau ici, on
//  note tout ce qui part vers les puces, image par image, et on met ce journal
//  dans la cartouche avec un petit lecteur qui le rejoue. Ce que la console
//  entend est donc, note pour note et registre pour registre, ce que la DS fait
//  entendre — il n'y a pas deux moteurs qui pourraient diverger.
//
//  DEUX DIFFÉRENCES avec la copie de l'iPad, imposées par la console :
//
//  1. La cartouche n'est JAMAIS tenue en mémoire. L'iPad fabrique 1 à 4 Mo dans
//     un tampon puis le rend ; la DS n'a pas cette place. Ici on écrit
//     directement dans le fichier, en avançant, et la somme de contrôle se
//     calcule au vol. L'en-tête — qui a besoin de la taille finale — est écrit
//     à la fin, en revenant au début du fichier.
//
//  2. Le convertisseur (DAC) est rééchantillonné.  (voir plus bas) Le flux capté sort à la
//     cadence de la puce émulée, et celle-ci n'est pas la même des deux côtés :
//     MD_YM_DIVISEUR vaut 144 sur l'iPad (53 267 Hz) et 288 sur la DS
//     (26 634 Hz), parce que la console ne tient pas le double. Le lecteur
//     68000, lui, débite toujours 444 valeurs par image. On ne peut donc pas
//     garder la décimation fixe « une sur deux » de l'iPad : elle donnerait ici
//     un PCM deux fois trop rapide et à moitié muet. Un accumulateur ramène le
//     flux à la cadence attendue quel que soit le diviseur.
// ============================================================================

#include "md_rom.h"
#include "../CustomReplayer/md_replayer.h"
#include "../MegaDrive/md_chip.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROM_PLAYER_ORG  0x000200u   // le lecteur, juste après l'en-tête
#define ROM_DATA_ORG    0x020000u   // le journal (adresse connue du lecteur)
#define ROM_MAX_BYTES   (4u * 1024u * 1024u)

// Le lecteur assemblé (voir mdplayer.s et l'outil qui le régénère).
#include "mdplayer_bin.h"

// Le convertisseur ne peut PAS passer par le journal d'écritures : il est
// alimenté à la cadence native du YM2612, ce qu'aucun journal de registres
// n'absorbe. On garde donc son flux à part, ramené à la cadence du lecteur, en
// un bloc de taille FIXE par image — la cartouche n'a plus qu'à le débiter à
// cadence régulière. Tout le travail (pas de lecture, volume, écrêtage,
// bouclage) a déjà été fait par le moteur : la ROM ne le refait pas, donc elle
// ne peut pas en diverger.
// La taille du bloc dépend de la cadence vidéo : 444 échantillons par image à
// 60 Hz, 532 à 50 Hz — dans les deux cas ~26 634 par seconde, la cadence du
// convertisseur. Elle est écrite DANS la ROM, le lecteur la lit.
//
// ⚠️ Elle doit rester PAIRE. Le lecteur saute le bloc d'un coup ; une taille
// impaire le laisse sur une adresse impaire, le mot d'en-tête de l'image
// suivante déclenche alors une ERREUR D'ADRESSE, le 68000 déroute et la
// cartouche redémarre en boucle. Vu à la mesure : 2 % de son, 98 % de silence.
#define MD_DAC_MAX       560          // de quoi tenir 50 Hz avec de la marge
#define MD_DAC_SILENCE   0x80
#define MD_DAC_DIVISEUR  288          // le diviseur qui donne 26 634 Hz
#define MD_DAC_HZ        (MD_YM2612_CLOCK / MD_DAC_DIVISEUR)
// Le pas d'attente du lecteur, CALIBRÉ (voir mdplayer.s) : 15 donne 26 604 Hz
// mesurés dans un vrai cœur Mega Drive, contre 26 634 visés.
#define MD_DAC_PAS       15

typedef struct {
  uint8_t   frame[3 * 1024];          // les écritures d'UNE image
  int       frame_n;
  int       count;
  int       deborde;                  // une image a-t-elle saturé le tampon ?
  uint8_t   dac[MD_DAC_MAX];
  int       dac_taille;               // échantillons attendus pour une image
  int       dac_n;                    // combien reçus dans cette image
  int       dac_acc;                  // accumulateur de rééchantillonnage
  int       dac_actif;                // la voie PCM a-t-elle joué ?
} md_capture_t;

struct md_rom_export_s {
  FILE            *f;
  char             chemin[256];
  char             titre[64];
  md_capture_t     cap;
  int16_t         *pcm;
  int              hz;                // cadence vidéo visée : 50 ou 60
  int              hz100;             // ...la vraie, au centième près
  int              per_frame;         // échantillons d'une image de 1/hz s
  // La POSITION COMPLÈTE de chaque voie au départ : song, chain, phrase.
  int              start_song[MD_MAX_CHANNELS];
  int              start_chain[MD_MAX_CHANNELS];
  int              start_phrase[MD_MAX_CHANNELS];
  int              vivante[MD_MAX_CHANNELS];
  int              bouclee[MD_MAX_CHANNELS];   // a fini son tour de song
  int              partie[MD_MAX_CHANNELS];    // a quitte sa position de depart
  int              active;
  int              frames, max_frames;
  int              fini;
  uint32_t         data_len;
  unsigned         somme;             // somme de contrôle en cours
  int              demi;              // octet haut en attente
  unsigned         demi_val;
  md_rom_report_t  rep;
  int              erreur;
};

// ── Capture ────────────────────────────────────────────────────────────────
static void md_rom_hook(int chip, uint8_t a, uint8_t b, void *ctx) {
  md_capture_t *cap = (md_capture_t *)ctx;
  if (chip == 3) {
    if (b) cap->dac_actif = 1;
    // Rééchantillonnage : on veut MD_DAC_DIVISEUR valeurs sorties pour
    // MD_YM_DIVISEUR valeurs entrées. À 144 cela reprend la décimation « une
    // sur deux » de l'iPad ; à 288 cela garde tout ; au-delà cela double.
    cap->dac_acc += MD_YM_DIVISEUR;
    while (cap->dac_acc >= MD_DAC_DIVISEUR) {
      cap->dac_acc -= MD_DAC_DIVISEUR;
      if (cap->dac_n < cap->dac_taille) cap->dac[cap->dac_n++] = a;
    }
    return;
  }
  if (cap->frame_n + 3 > (int)sizeof(cap->frame)) { cap->deborde = 1; return; }
  cap->frame[cap->frame_n++] = (uint8_t)chip;
  cap->frame[cap->frame_n++] = a;
  cap->frame[cap->frame_n++] = b;
  cap->count++;
}

// ── Écriture, avec la somme de contrôle au vol ─────────────────────────────
// La somme de la Mega Drive porte sur les MOTS de $200 jusqu'à la fin. Tout ce
// qu'on écrit ici commence à $20000 et garde une longueur paire, donc le
// découpage en mots ne se décale jamais.
static void somme_ajoute(md_rom_export_t *e, const uint8_t *p, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (!e->demi) { e->demi_val = p[i]; e->demi = 1; }
    else { e->somme = (e->somme + ((e->demi_val << 8) | p[i])) & 0xFFFFu;
           e->demi = 0; }
  }
}

static void ecrit(md_rom_export_t *e, const uint8_t *p, size_t n) {
  if (e->erreur || n == 0) return;
  if (fwrite(p, 1, n, e->f) != n) { e->erreur = 1; return; }
  somme_ajoute(e, p, n);
  e->data_len += (uint32_t)n;
}

// Des zéros par blocs — ils ne changent pas la somme, seulement la taille.
static void ecrit_zeros(md_rom_export_t *e, uint32_t n) {
  static const uint8_t vide[512] = {0};
  while (n && !e->erreur) {
    uint32_t bloc = n > sizeof(vide) ? (uint32_t)sizeof(vide) : n;
    if (fwrite(vide, 1, bloc, e->f) != bloc) { e->erreur = 1; return; }
    n -= bloc;
  }
}

// ── Ouverture ──────────────────────────────────────────────────────────────
md_rom_export_t *md_rom_export_begin(const char *chemin, const char *titre,
                                     int sample_rate, int hz) {
  if (!chemin || sample_rate <= 0) return NULL;
  if (hz != 50 && hz != 60) hz = 60;

  md_rom_export_t *e = (md_rom_export_t *)calloc(1, sizeof(*e));
  if (!e) return NULL;

  strncpy(e->chemin, chemin, sizeof(e->chemin) - 1);
  if (titre) strncpy(e->titre, titre, sizeof(e->titre) - 1);

  // Une image : 1/hz s d'audio rendu à la cadence DU MOTEUR. C'est ce qui fixe
  // le tempo de la cartouche, puisque le lecteur 68000 avance d'une image par
  // trame vidéo — 60 par seconde sur une console NTSC, 50 sur une PAL.
  // Une Mega Drive ne tourne JAMAIS a 60 ni a 50 images : c'est 59,92 en NTSC
  // et 49,70 en PAL. Prendre les chiffres ronds coute 0,6 % sur le tempo — pas
  // grand-chose, mais c'est gratuit de viser juste.
  e->hz = hz;
  e->hz100 = (hz == 50) ? 4970 : 5992;
  e->per_frame = (int)(((long)sample_rate * 100) / e->hz100);
  // Le bloc du convertisseur, arrondi au PAIR superieur (voir plus haut).
  e->cap.dac_taille = (int)((((long)MD_DAC_HZ * 100) / e->hz100 + 1) & ~1L);
  e->rep.hz = hz;
  e->rep.dac_block = e->cap.dac_taille;
  e->pcm = (int16_t *)malloc((size_t)e->per_frame * 2 * sizeof(int16_t));
  if (!e->pcm) { free(e); return NULL; }

  e->f = fopen(chemin, "wb");
  if (!e->f) { free(e->pcm); free(e); return NULL; }

  // Le début de la cartouche — vecteurs, en-tête, lecteur — n'est écrit qu'à la
  // fin : il lui faut la taille finale. On réserve la place en zéros.
  ecrit_zeros(e, ROM_DATA_ORG);
  if (e->erreur) { fclose(e->f); remove(chemin); free(e->pcm); free(e); return NULL; }

  // Où chaque canal commence. L'export lit le song UNE FOIS : il s'arrête quand
  // chaque voie a parcouru sa colonne en entier et se retrouve à son point de
  // départ. C'est la cartouche qui reboucle ensuite, comme le tracker reboucle
  // quand on fait play — l'export, lui, ne tourne pas en rond.
  //
  // Deux pièges, tous les deux vus à l'usage :
  //
  //  • La ligne de song ne suffit pas à repérer ce point. Un morceau court tient
  //    sur une seule ligne de song : cette ligne ne change alors JAMAIS, le tour
  //    n'était jamais vu, et l'export tournait jusqu'au garde-fou de six minutes
  //    pour un morceau de trente secondes. On regarde donc la position entière —
  //    ligne de song, ligne de chain, ligne de phrase.
  //
  //  • Chaque voie compte SON tour, séparément. Exiger qu'elles reviennent
  //    toutes ensemble au départ, c'est attendre le plus petit multiple commun
  //    de leurs longueurs : deux voies de 4 et de 6 lignes feraient douze lignes
  //    d'export. Le morceau, lui, a été lu bien avant.
  for (int c = 0; c < MD_MAX_CHANNELS; c++) {
    e->start_song[c]   = md_replayer_play_song_row(c);
    e->start_chain[c]  = md_replayer_play_chain_row(c);
    e->start_phrase[c] = md_replayer_play_phrase_row(c);
    e->vivante[c] = (e->start_song[c] >= 0);   // une voie muette ne compte pas
    if (e->vivante[c]) e->active++;
  }
  e->max_frames = 60 * 60 * 6;              // six minutes, garde-fou

  md_chip_set_write_hook(md_rom_hook, &e->cap);
  return e;
}

int md_rom_export_frames(const md_rom_export_t *e) { return e ? e->frames : 0; }
int md_rom_export_max_frames(const md_rom_export_t *e) {
  return e ? e->max_frames : 0;
}
uint32_t md_rom_export_bytes(const md_rom_export_t *e) {
  return e ? ROM_DATA_ORG + e->data_len : 0;
}

// ── Une tranche ────────────────────────────────────────────────────────────
int md_rom_export_step(md_rom_export_t *e, int images) {
  if (!e) return -1;
  if (e->erreur) return -1;
  if (e->fini) return 0;

  for (int n = 0; n < images && e->frames < e->max_frames; n++, e->frames++) {
    md_capture_t *cap = &e->cap;
    cap->frame_n = 0; cap->count = 0;
    cap->dac_n = 0; cap->dac_actif = 0;
    md_replayer_update((unsigned char *)e->pcm, (unsigned)(e->per_frame * 4));

    // Le compteur porte un drapeau : bit 15 levé = un bloc DAC suit les
    // écritures. Le compte lui-même ne dépasse jamais quelques centaines, et
    // 0xFFFF reste la marque de fin — les deux cohabitent sans ambiguïté.
    unsigned tete = (unsigned)cap->count | (cap->dac_actif ? 0x8000u : 0u);
    uint8_t entete[2] = { (uint8_t)(tete >> 8), (uint8_t)(tete & 0xFF) };
    ecrit(e, entete, 2);
    ecrit(e, cap->frame, (size_t)cap->frame_n);
    // Un triplet fait 3 octets : une image au nombre impair d'écritures laisse
    // le journal sur une adresse impaire, et le compteur de l'image suivante
    // serait lu en travers. Sur un 68000, lire un mot à une adresse impaire est
    // une ERREUR D'ADRESSE : le processeur déroute et le morceau part en vrille.
    // On complète donc chaque image à une longueur paire ; le lecteur réaligne
    // de son côté, ce qui revient à sauter cet octet.
    if (cap->frame_n & 1) { uint8_t z = 0; ecrit(e, &z, 1); }
    if (cap->dac_actif) {
      // Bloc de taille fixe : ce qui manque est du silence (0x80), pas un
      // trou. Une image où le PCM démarre en cours de route en reçoit moins.
      for (int i = cap->dac_n; i < cap->dac_taille; i++)
        cap->dac[i] = MD_DAC_SILENCE;
      ecrit(e, cap->dac, (size_t)cap->dac_taille);
      e->rep.dac_frames++;
    }
    e->rep.writes += cap->count;
    // Une image saturée voudrait dire des écritures perdues, donc une cartouche
    // qui ne joue plus la même chose. On arrête net plutôt que de livrer ça.
    if (cap->deborde) e->erreur = 1;
    if (e->erreur) return -1;

    // Le song est-il lu ? Chaque voie a quitté son point de départ, puis y est
    // revenue : sa colonne est faite. Quand toutes l'ont fait, on arrête.
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
    // Aucune voie ne joue : la cartouche n'aurait rien à dire. Une seconde de
    // silence suffit à en faire une ROM valide.
    if (e->active == 0 && e->frames >= 60) { e->frames++; e->fini = 1; break; }
    if (done && e->active > 0) { e->frames++; e->fini = 1; break; }

    // La plus grosse cartouche fait quatre mégaoctets. Plutôt que d'aller au
    // bout pour tout jeter à l'arrivée, on s'arrête ici et on le dit : le
    // morceau exporté est écourté, mais il existe et il joue.
    if (ROM_DATA_ORG + e->data_len + 8192u > ROM_MAX_BYTES) {
      e->rep.truncated = true; e->frames++; e->fini = 1; break;
    }
  }
  if (e->frames >= e->max_frames) e->fini = 1;
  return e->fini ? 0 : 1;
}

// ── Fermeture ──────────────────────────────────────────────────────────────
bool md_rom_export_end(md_rom_export_t *e, md_rom_report_t *rapport) {
  if (!e) return false;
  md_chip_set_write_hook(NULL, NULL);

  uint8_t marque[2] = { 0xFF, 0xFF };
  ecrit(e, marque, 2);                       // marque de rebouclage

  uint32_t size = ROM_DATA_ORG + e->data_len;
  uint32_t rounded = 0x80000;                // 512 Ko au minimum
  while (rounded < size && rounded < ROM_MAX_BYTES) rounded <<= 1;
  if (e->erreur || size > ROM_MAX_BYTES) {
    fclose(e->f); remove(e->chemin); free(e->pcm); free(e); return false;
  }
  ecrit_zeros(e, rounded - size);            // le remplissage ne compte pas

  // ── Le début de la cartouche ────────────────────────────────────────────
  // Vecteurs, en-tête et lecteur : à peine plus d'un kilo-octet, tout le reste
  // jusqu'à $20000 est déjà en zéros dans le fichier.
  const uint32_t tete_len = ROM_PLAYER_ORG + (uint32_t)sizeof(md_player_bin);
  uint8_t *tete = (uint8_t *)calloc(tete_len + 1, 1);
  if (!tete) { fclose(e->f); remove(e->chemin); free(e->pcm); free(e); return false; }

  // Table des vecteurs : pile initiale, puis le point d'entrée pour TOUS les
  // vecteurs — une exception inattendue redémarre le lecteur au lieu de planter.
  uint32_t sp = 0x00FFFFF0, pc = ROM_PLAYER_ORG;
  for (int i = 0; i < 64; i++) {
    uint32_t v = (i == 0) ? sp : pc;
    tete[i*4+0] = (uint8_t)(v >> 24); tete[i*4+1] = (uint8_t)(v >> 16);
    tete[i*4+2] = (uint8_t)(v >> 8);  tete[i*4+3] = (uint8_t)v;
  }

  // En-tête. Les champs sont à position fixe et remplis d'espaces.
  uint8_t *h = tete + 0x100;
  memset(h, ' ', 0x100);
  memcpy(h + 0x000, "SEGA MEGA DRIVE ", 16);
  // ⚠️ SEIZE CARACTERES, EXACTEMENT. Le champ de copyright de l'en-tete SEGA
  // est de longueur fixe : plus court, il deborde sur le champ suivant ; plus
  // long, il ecrase la date. On complete donc a la main.
  memcpy(h + 0x010, "(C)GENETRACKER  ", 16);
  char nm[49];
  memset(nm, ' ', sizeof(nm));
  { size_t n = strlen(e->titre); if (n > 48) n = 48; memcpy(nm, e->titre, n); }
  memcpy(h + 0x020, nm, 48);          // titre national
  memcpy(h + 0x050, nm, 48);          // titre international
  memcpy(h + 0x080, "GM MDTRACK-00", 13);
  h[0x0A0] = 0; h[0x0A1] = 0; h[0x0A2] = 0; h[0x0A3] = 0;          // ROM début
  uint32_t last = rounded - 1;
  h[0x0A4] = (uint8_t)(last >> 24); h[0x0A5] = (uint8_t)(last >> 16);
  h[0x0A6] = (uint8_t)(last >> 8);  h[0x0A7] = (uint8_t)last;      // ROM fin
  memcpy(h + 0x0A8, "\xFF\xFF\x00\x00\xFF\xFF\xFF\xFF", 8);        // RAM
  memcpy(h + 0x0F0, "JUE             ", 16);                       // régions

  memcpy(tete + ROM_PLAYER_ORG, md_player_bin, sizeof(md_player_bin));
  // Les DEUX DERNIERS MOTS du lecteur sont les seules valeurs qu'on lui écrit :
  // la taille du bloc de convertisseur, puis le pas d'attente. Ne rien ajouter
  // après eux dans mdplayer.s (voir le commentaire là-bas).
  {
    uint8_t *v = tete + tete_len - 4;
    v[0] = (uint8_t)(e->cap.dac_taille >> 8); v[1] = (uint8_t)e->cap.dac_taille;
    v[2] = (uint8_t)(MD_DAC_PAS >> 8);        v[3] = (uint8_t)MD_DAC_PAS;
  }

  // La somme de contrôle : ce qui a déjà été écrit, plus le lecteur. Ce qui est
  // avant $200 n'y entre pas, et les zéros n'y changent rien.
  const uint32_t joueur_len = (tete_len - ROM_PLAYER_ORG + 1) & ~1u;
  e->demi = 0;
  somme_ajoute(e, tete + ROM_PLAYER_ORG, joueur_len);
  tete[0x18E] = (uint8_t)(e->somme >> 8); tete[0x18F] = (uint8_t)e->somme;

  int ok = (fseek(e->f, 0, SEEK_SET) == 0) &&
           (fwrite(tete, 1, tete_len, e->f) == tete_len);
  free(tete);
  ok = (fclose(e->f) == 0) && ok;
  if (!ok) { remove(e->chemin); free(e->pcm); free(e); return false; }

  e->rep.frames = e->frames;
  e->rep.rom_bytes = rounded;
  e->rep.seconds = e->frames * 100.0 / e->hz100;
  e->rep.pcm_included = true;
  if (rapport) *rapport = e->rep;
  free(e->pcm); free(e);
  return true;
}

void md_rom_export_abort(md_rom_export_t *e) {
  if (!e) return;
  md_chip_set_write_hook(NULL, NULL);
  fclose(e->f);
  remove(e->chemin);
  free(e->pcm);
  free(e);
}
