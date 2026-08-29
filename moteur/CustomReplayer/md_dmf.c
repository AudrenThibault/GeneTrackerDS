// ============================================================================
// Import des fichiers .dmf de DefleMask (Mega Drive)
// ============================================================================
//
// Écrit à partir de la seule description publique du format. Aucune ligne ne
// provient du code de DefleMask ni de Furnace : Furnace est sous GPL, donc
// incompatible avec une publication sur l'App Store. Un format de fichier
// n'est pas protégeable en soi — c'est ce qui rend cet import légal — mais le
// code qui le lit doit être le nôtre, et il l'est.
//
// La structure a été vérifiée octet par octet contre un vrai morceau : la
// table d'échantillons tombe exactement là où le dernier motif se termine.
//
// ── Ce que DefleMask range dans le fichier ──────────────────────────────────
//   « .DelekDefleMask. », version, système (02 = Mega Drive)
//   titre, auteur, surlignages
//   base de temps, deux durées de tick, PAL/NTSC, fréquence libre
//   lignes par motif (32 bits), lignes de la matrice
//   MATRICE : par canal, par ligne — 1 octet de numéro + 1 nom (souvent vide)
//   INSTRUMENTS : nom, mode (1 = FM), puis 4+48 octets en FM, ou 4 macros
//   WAVETABLES, puis MOTIFS : par canal, 1 octet de colonnes d'effet, puis
//   `lignes de matrice` motifs de `lignes par motif` lignes ; chaque ligne
//   fait 8 + 4×colonnes octets (note, octave, volume, effets, instrument)
//   ÉCHANTILLONS — que nous ignorons : le moteur n'a pas de lecteur de son.
//
// ── Comment ça entre dans notre modèle ──────────────────────────────────────
// DefleMask a des motifs de 64 lignes propres à chaque canal ; nous avons des
// phrases de 16 lignes puisées dans un pool commun. Un motif devient donc
// QUATRE phrases, et une ligne de matrice devient une chaîne de quatre
// phrases. Sans plus de précaution le compte explose — 2280 phrases pour 255
// places — mais les morceaux se répètent énormément : en ne gardant qu'un
// exemplaire de chaque phrase et de chaque chaîne distinctes, le morceau
// d'essai retombe à 59 phrases et 21 chaînes.
// ============================================================================

#include "md_replayer.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define DMF_HEADER ".DelekDefleMask."
#define DMF_SYSTEM_GENESIS 0x02
#define DMF_SYSTEM_GENESIS_EXT 0x42   // Mega Drive avec le canal 3 éclaté
#define DMF_PHRASE_ROWS 16

// Lecteur de flux qui refuse de sortir du tampon : un fichier tronqué ou
// malveillant doit échouer proprement, jamais lire à côté.
typedef struct {
  const uint8_t *d;
  uint32_t n, p;
  bool bad;
} dmf_reader_t;

static uint8_t rd8(dmf_reader_t *r) {
  if (r->p + 1 > r->n) { r->bad = true; return 0; }
  return r->d[r->p++];
}
static uint32_t rd32(dmf_reader_t *r) {
  if (r->p + 4 > r->n) { r->bad = true; r->p = r->n; return 0; }
  uint32_t v = (uint32_t)r->d[r->p] | ((uint32_t)r->d[r->p + 1] << 8) |
               ((uint32_t)r->d[r->p + 2] << 16) | ((uint32_t)r->d[r->p + 3] << 24);
  r->p += 4;
  return v;
}
static int16_t rd16s(dmf_reader_t *r) {
  if (r->p + 2 > r->n) { r->bad = true; r->p = r->n; return -1; }
  int16_t v = (int16_t)((uint16_t)r->d[r->p] | ((uint16_t)r->d[r->p + 1] << 8));
  r->p += 2;
  return v;
}
static void rdskip(dmf_reader_t *r, uint32_t k) {
  if (r->p + k > r->n) { r->bad = true; r->p = r->n; return; }
  r->p += k;
}
// Chaîne « 1 octet de longueur + octets ». `out` peut être NULL.
static void rdstr(dmf_reader_t *r, char *out, int cap) {
  uint8_t len = rd8(r);
  if (r->p + len > r->n) { r->bad = true; r->p = r->n; if (out) out[0] = 0; return; }
  if (out) {
    int k = (len < cap - 1) ? len : cap - 1;
    memcpy(out, r->d + r->p, (size_t)k);
    out[k] = 0;
  }
  r->p += len;
}

// ── Une ligne de phrase, en attente d'être posée dans le module ─────────────
typedef struct {
  uint8_t note, instr, vel, cmd, cmdval, mdcmd, mdval;
} dmf_row_t;

typedef struct {
  dmf_row_t rows[DMF_PHRASE_ROWS];
} dmf_phrase_t;

// Une note de DefleMask.
//
//   note 100                  = note-off
//   note 0 ET octave 0        = case VIDE
//   note 0 avec une octave    = un DO — c'est le piège du format : le zéro ne
//                               veut PAS dire « rien », il veut dire do. En le
//                               traitant comme vide, tous les do du morceau
//                               disparaissaient (32 dans le fichier d'essai).
//   note 1-11                 = do# … si
//   note 12                   = un do, mais de l'octave du DESSUS ; les deux
//                               écritures du do coexistent dans un même fichier.
//
// `note % 12` couvre les deux conventions d'un coup : 0 et 12 donnent tous
// deux le do, seule l'octave diffère.
static uint8_t dmf_note(int16_t note, int16_t octave) {
  if (note == 100) return MD_EMPTY;          // note-off
  if (note == 0 && octave == 0) return 0;    // case vide
  if (note < 0 || note > 12) return 0;
  int semi = note % 12;
  int oct = octave + (note == 12 ? 1 : 0);
  int v = oct * 12 + semi + 1;                // chez nous, do-0 vaut 1
  if (v < 1) v = 1;
  if (v > MD_MAX_NOTE) v = MD_MAX_NOTE;
  return (uint8_t)v;
}

// ── Instruments ─────────────────────────────────────────────────────────────
// Chaque opérateur tient sur douze octets, dans cet ordre :
//   AM, AR, D1R, MULT, RR, D1L, TL, DT2, RS, DT, D2R, SSG-EG
//
// Mais ATTENTION à deux pièges, tous deux vérifiés contre l'écran instrument
// de DefleMask sur l'instrument « Beep » :
//
//  1. Les opérateurs sont rangés dans l'ORDRE DES REGISTRES du YM2612 — 1, 3,
//     2, 4 — et non dans celui de l'écran. Nos `op[]` sont, eux, dans l'ordre
//     de l'écran (c'est md_op_reg_offset qui fait l'entrelacement matériel).
//     Sans cette permutation, OP2 et OP3 échangeaient tous leurs réglages et
//     l'instrument ne sonnait pas du tout comme dans DefleMask.
static const int dmf_op_order[4] = {0, 2, 1, 3};

//  2. Le detune est rangé de 0 à 6, CENTRÉ SUR 3 : 3 = pas de detune, 5 = +2,
//     0 = -3. Le YM2612 attend autre chose : 0 = zéro, 1-3 = +1 à +3, et le
//     bit 2 porte le signe, donc 5-7 = -1 à -3. Recopier l'octet tel quel
//     transformait un +2 en -1.
static uint8_t dmf_detune(uint8_t stored) {
  int d = (int)stored - 3;
  if (d > 3) d = 3;
  if (d < -3) d = -3;
  return (uint8_t)(d >= 0 ? d : (4 + (-d)));
}

// DefleMask IGNORE un instrument dont le type ne convient pas à la voie : la
// voie garde alors le dernier valable, et n'a rien à jouer si elle n'en a
// jamais eu. Une seule règle, vérifiée sur les deux cas du fichier d'essai :
//
//  - FM4 : le tout PREMIER instrument posé est « Ins 7 », de type PSG. Aucun
//    repli possible, et de fait la voie reste muette dans DefleMask.
//  - NOI : « Ins 0 », de type FM, arrive après « Ins 6 » qui est PSG. DefleMask
//    l'ignore, garde Ins 6, et le si-3 sonne. Je recopiais bêtement Ins 0, ce
//    qui posait un instrument FM sur la voie de bruit — donc rien.
static bool dmf_instr_fits(int ins1, int ch) {
  int k = md_replayer_get_instr_kind(ins1);
  if (k == MD_INSTR_KIND_ANY) return true;
  if (ch < MD_NUM_FM_CHANNELS)
    return k == MD_INSTR_KIND_FM || k == MD_INSTR_KIND_PCM;
  return k == MD_INSTR_KIND_PSG;
}

static void dmf_read_fm_instrument(dmf_reader_t *r, int ins) {
  int alg = rd8(r), fb = rd8(r), fms = rd8(r), ams = rd8(r);
  md_replayer_set_instr_gen_val(ins, MD_GEN_PROP_ALGORITHM, alg & 7);
  md_replayer_set_instr_gen_val(ins, MD_GEN_PROP_FEEDBACK, fb & 7);
  md_replayer_set_instr_gen_val(ins, MD_GEN_PROP_PMS, fms & 7);
  md_replayer_set_instr_gen_val(ins, MD_GEN_PROP_AMS, ams & 3);
  // Le LFO n'a de sens que si une sensibilité l'écoute — sinon on le laisse
  // éteint, comme sur la puce.
  md_replayer_set_instr_gen_val(ins, MD_GEN_PROP_LFO_ENABLE,
                                (fms || ams) ? 1 : 0);
  for (int slot = 0; slot < 4; slot++) {
    uint8_t am = rd8(r), ar = rd8(r), d1r = rd8(r), mul = rd8(r);
    uint8_t rr = rd8(r), d1l = rd8(r), tl = rd8(r);
    rd8(r);                                   // DT2 : le YM2612 ne l'a pas
    uint8_t rs = rd8(r), dt = rd8(r), d2r = rd8(r), ssg = rd8(r);
    const int op = dmf_op_order[slot];        // 1, 3, 2, 4 -> 1, 2, 3, 4
    md_replayer_set_instr_op_val(ins, op, MD_OP_PROP_AM, am ? 1 : 0);
    md_replayer_set_instr_op_val(ins, op, MD_OP_PROP_ATTACK, ar & 0x1F);
    md_replayer_set_instr_op_val(ins, op, MD_OP_PROP_DECAY, d1r & 0x1F);
    md_replayer_set_instr_op_val(ins, op, MD_OP_PROP_MULTIPLE, mul & 0x0F);
    md_replayer_set_instr_op_val(ins, op, MD_OP_PROP_RELEASE, rr & 0x0F);
    md_replayer_set_instr_op_val(ins, op, MD_OP_PROP_SUSTAIN_LEVEL, d1l & 0x0F);
    md_replayer_set_instr_op_val(ins, op, MD_OP_PROP_TOTAL_LEVEL, tl & 0x7F);
    md_replayer_set_instr_op_val(ins, op, MD_OP_PROP_KEY_SCALE, rs & 3);
    md_replayer_set_instr_op_val(ins, op, MD_OP_PROP_DETUNE, dmf_detune(dt));
    md_replayer_set_instr_op_val(ins, op, MD_OP_PROP_SUSTAIN_RATE, d2r & 0x1F);
    md_replayer_set_instr_op_val(ins, op, MD_OP_PROP_SSG_EG, ssg & 0x0F);
  }
}

// Une macro : longueur, valeurs sur 32 bits signés, puis la boucle si non vide.
// `vals` peut être NULL quand on ne veut que sauter la macro.
// `loop` reçoit la position de bouclage (-1 = aucune) et `mode` le mode
// d'arpège (1 = « Fixed Arpeggio » : des notes absolues, pas des écarts).
static int dmf_read_macro(dmf_reader_t *r, int32_t *vals, int cap,
                          bool arpeggio, int *loop, int *mode) {
  uint8_t len = rd8(r);
  for (int i = 0; i < len; i++) {
    int32_t v = (int32_t)rd32(r);
    if (vals && i < cap) vals[i] = v;
  }
  int lp = -1;
  if (len > 0) lp = (int8_t)rd8(r);   // position de bouclage, -1 = aucune
  int md = 0;
  if (arpeggio) md = rd8(r);          // mode d'arpège : toujours présent
  if (loop) *loop = lp;
  if (mode) *mode = md;
  return len;
}

// Instrument PSG. Les macros de DefleMask sont désormais reprises TELLES
// QUELLES : un pas par tick, jusqu'à 128 pas, avec leur point de bouclage.
// Plus d'approximation sur trois points — l'instrument sonne comme l'original.
//
// La macro de mode de bruit garde son emplacement historique (32 pas).
static void dmf_read_std_instrument(dmf_reader_t *r, int ins, int *approximated) {
  int32_t vol[MD_PSG_MACRO_MAX], arp[MD_PSG_MACRO_MAX], nz[MD_PSG_MACRO_MAX];
  int vloop = -1, aloop = -1, amode = 0, nloop = -1;
  int nvol = dmf_read_macro(r, vol, MD_PSG_MACRO_MAX, false, &vloop, NULL);
  int narp = dmf_read_macro(r, arp, MD_PSG_MACRO_MAX, true, &aloop, &amode);
  int nnz  = dmf_read_macro(r, nz,  MD_PSG_MACRO_MAX, false, &nloop, NULL);
  dmf_read_macro(r, NULL, 0, false, NULL, NULL);   // wavetable : sans objet ici

  if (nvol > MD_PSG_MACRO_MAX) { nvol = MD_PSG_MACRO_MAX; (*approximated)++; }
  if (narp > MD_PSG_MACRO_MAX) { narp = MD_PSG_MACRO_MAX; (*approximated)++; }

  if (nvol > 0) {
    uint8_t v[MD_PSG_MACRO_MAX];
    for (int i = 0; i < nvol; i++) {
      int32_t x = vol[i];
      v[i] = (uint8_t)(x < 0 ? 0 : (x > 15 ? 15 : x));
    }
    md_replayer_set_psg_vol_macro(ins, v, nvol, vloop);
  }
  if (narp > 0) {
    // La macro d'arpège est rangée AVEC UN BIAIS DE 12 : le fichier écrit 0
    // là où DefleMask affiche -12, et 19 pour +7. Aucune valeur brute n'est
    // jamais négative, ce qui confirme le décalage ; retirer 12 redonne des
    // arpèges musicalement sensés partout (0/12/24 pour une octave, 0/0/0
    // pour un arpège neutre).
    //
    // Non vérifié en mode « Fixed Arpeggio » : aucun instrument du fichier
    // d'essai ne s'en sert. Si un morceau importé sonne transposé d'une
    // octave sur un instrument en mode fixe, c'est ici qu'il faut regarder.
    int8_t a[MD_PSG_MACRO_MAX];
    for (int i = 0; i < narp; i++) {
      int32_t x = arp[i] - 12;
      a[i] = (int8_t)(x < -128 ? -128 : (x > 127 ? 127 : x));
    }
    md_replayer_set_psg_arp_macro(ins, a, narp, aloop, amode != 0);
  }
  if (nnz > 0) {
    int n = nnz > MD_PSG_ENV_MAX ? MD_PSG_ENV_MAX : nnz;
    if (nnz > MD_PSG_ENV_MAX) (*approximated)++;
    for (int i = 0; i < n; i++) {
      int32_t x = nz[i];
      md_replayer_set_psg_noise_step(ins, i, (int)(x < 0 ? 0 : (x > 7 ? 7 : x)));
    }
    md_replayer_set_psg_noise_len(ins, n);
    md_replayer_set_psg_noise_loop(ins, (nloop >= 0 && nloop < n) ? nloop : MD_EMPTY);
  }
}

// ============================================================================
bool md_replayer_import_dmf(const uint8_t *data, uint32_t size,
                            md_dmf_report_t *report) {
  md_dmf_report_t rep;
  memset(&rep, 0, sizeof(rep));
  if (report) memset(report, 0, sizeof(*report));

  if (!data || size < 32 || memcmp(data, DMF_HEADER, 16) != 0)
    return false;

  dmf_reader_t R = {data, size, 16, false};
  rep.version = rd8(&R);
  uint8_t system = rd8(&R);
  rep.system = system;
  if (system != DMF_SYSTEM_GENESIS && system != DMF_SYSTEM_GENESIS_EXT)
    return false;                       // ce n'est pas un morceau Mega Drive
  rep.ext_ch3 = (system == DMF_SYSTEM_GENESIS_EXT);

  rdstr(&R, rep.song, sizeof(rep.song));
  rdstr(&R, rep.author, sizeof(rep.author));
  uint8_t highlight_a = rd8(&R);        // surlignages : deux grilles visuelles
  uint8_t highlight_b = rd8(&R);
  uint8_t timebase = rd8(&R);
  uint8_t tick1 = rd8(&R), tick2 = rd8(&R);
  uint8_t frames = rd8(&R);             // 0 = PAL, 1 = NTSC
  uint8_t custom = rd8(&R);
  uint8_t hz1 = rd8(&R), hz2 = rd8(&R);
  rd8(&R);
  uint32_t rows_per_pattern = rd32(&R);
  uint8_t orders = rd8(&R);
  if (R.bad || rows_per_pattern == 0 || rows_per_pattern > 256 || orders == 0)
    return false;
  rep.orders = orders;
  rep.rows_per_pattern = (int)rows_per_pattern;

  const int CH = MD_MAX_CHANNELS;       // le Mega Drive en a exactement dix

  // ── Résolution des lignes ─────────────────────────────────────────────
  // DefleMask compte ici huit lignes par temps (« Highlight A ») ; un tracker
  // façon LSDJ en compte quatre, une phrase de seize lignes valant une mesure.
  // On fond donc deux lignes DefleMask en une, et on double le nombre de ticks
  // par ligne pour que le TEMPO ne bouge pas.
  //
  // Sans ça, un motif de 64 lignes occupait quatre phrases et la musique était
  // étalée sur le double de la durée attendue à l'écran.
  int divisor = (highlight_a >= 8) ? (int)highlight_a / 4 : 1;
  if (divisor < 1) divisor = 1;
  if (divisor > 4) divisor = 4;
  rep.row_divisor = divisor;
  const uint32_t rows_out = (rows_per_pattern + (uint32_t)divisor - 1) /
                            (uint32_t)divisor;
  const int quarters = (int)((rows_out + DMF_PHRASE_ROWS - 1) /
                             DMF_PHRASE_ROWS);

  // ── Matrice ───────────────────────────────────────────────────────────
  uint8_t *matrix = (uint8_t *)calloc((size_t)CH * orders, 1);
  if (!matrix) return false;
  for (int c = 0; c < CH; c++)
    for (int o = 0; o < orders; o++) {
      matrix[c * orders + o] = rd8(&R);
      rdstr(&R, NULL, 0);               // nom du motif, presque toujours vide
    }
  if (R.bad) { free(matrix); return false; }

  // ── Instruments ───────────────────────────────────────────────────────
  md_replayer_new_empty();
  int ninst = rd8(&R);
  for (int i = 0; i < ninst; i++) {
    char name[43];
    rdstr(&R, name, sizeof(name));
    uint8_t mode = rd8(&R);
    int ins = i + 1;                    // nos instruments comptent depuis 1
    bool keep = (ins >= 1 && ins <= MD_MAX_INSTRUMENTS);
    if (keep && name[0]) md_replayer_set_instr_name(ins, name);
    // On retient la puce visée. Un instrument PSG posé sur une colonne FM ne
    // doit rien jouer, comme dans DefleMask : sans ça, notre moteur chargeait
    // la moitié FM restée aux valeurs d'usine et inventait un son.
    if (keep)
      md_replayer_set_instr_kind(ins, (mode == 1) ? MD_INSTR_KIND_FM
                                                  : MD_INSTR_KIND_PSG);
    if (mode == 1)
      dmf_read_fm_instrument(&R, keep ? ins : 1);
    else
      dmf_read_std_instrument(&R, keep ? ins : 1,
                              &rep.instruments_approximated);
    if (keep) rep.instruments++;
    if (R.bad) { free(matrix); return false; }
  }

  // ── Wavetables : le Mega Drive n'en a pas, on saute ───────────────────
  int nwave = rd8(&R);
  for (int w = 0; w < nwave; w++) {
    uint32_t sz = rd32(&R);
    rdskip(&R, sz * 4);
  }
  if (R.bad) { free(matrix); return false; }

  // ── Motifs ────────────────────────────────────────────────────────────
  // ATTENTION, c'est le piège du format : les blocs ne sont PAS rangés par
  // numéro de motif. Chaque canal en range un PAR LIGNE DE LA MATRICE, dans
  // l'ordre du morceau. Le bloc n° 3 est donc ce que joue la ligne de song
  // n° 3 — et si deux lignes pointent le même motif, leur contenu est écrit
  // deux fois, à l'identique.
  //
  // Vérifié sur le fichier d'essai : 134 paires de lignes partageant un même
  // numéro de motif ont des blocs rigoureusement identiques, sans exception.
  //
  // En lisant les blocs comme s'ils étaient indexés par numéro de motif, tous
  // les canaux sauf le premier jouaient les mauvaises mesures.
  size_t nslots = (size_t)CH * orders * quarters;
  dmf_phrase_t *slots = (dmf_phrase_t *)calloc(nslots, sizeof(dmf_phrase_t));
  if (!slots) { free(matrix); return false; }
  // ── Voie DAC ────────────────────────────────────────────────────────────
  // Sur FM6, l'effet 17xx débranche la synthèse au profit du convertisseur.
  // DefleMask ne pose alors AUCUN instrument, ni aucun volume : la note est
  // seule à décider. Vérifié sur le fichier d'essai, où la voie ne porte que
  // des notes et un « 17 01 ».
  //
  // Deux choses en découlent, et elles sont indépendantes :
  //
  //  1. QUEL échantillon : c'est le demi-ton. Mesuré contre l'enregistrement
  //     de référence — do# joue bien le hihat (ressemblance 0,89, loin devant
  //     les quatre autres), mi joue bien snare2. La hauteur, elle, est
  //     ÉCARTÉE : en cherchant pour chaque note la cadence de lecture qui
  //     colle le mieux, on la voit DESCENDRE quand la note monte (61 kHz pour
  //     do-4, 34 kHz pour mi-5), l'inverse exact d'une transposition.
  //
  //  2. QUELLE note s'affiche : celle du fichier, telle quelle. J'écrasais
  //     tout par la note de référence, si bien que la voie n'affichait que des
  //     do-4 là où DefleMask montre do-4, do-5, do#-5 et mi-5.
  //
  // On garde donc la note ici ; chaque note distincte recevra plus bas sa
  // propre entrée de banque, calée pour jouer à la cadence naturelle.
  //
  // Réserve honnête : do-4 et do-5 partagent le demi-ton 0 et désignent donc
  // le même échantillon. Impossible de le confirmer à l'écoute — sur ces deux
  // notes l'enregistrement ne laisse rien émerger du fond (cohérence interne
  // 0,14 et 0,30, contre 0,89 pour le do#). Si l'un des deux sonne faux, c'est
  // l'échantillon d'UN instrument PCM à changer, rien de plus.
  uint8_t *dac_want = (uint8_t *)calloc(nslots * DMF_PHRASE_ROWS, 1);  // 0 = rien
  if (!dac_want) { free(slots); free(matrix); return false; }
  for (size_t i = 0; i < nslots; i++)
    for (int k = 0; k < DMF_PHRASE_ROWS; k++) {
      slots[i].rows[k].vel = MD_EMPTY;
      slots[i].rows[k].cmd = MD_EMPTY;
      slots[i].rows[k].mdcmd = MD_EMPTY;
    }

  for (int c = 0; c < CH; c++) {
    int fx = rd8(&R);
    if (fx < 0 || fx > 8) { free(dac_want); free(slots); free(matrix); return false; }
    bool dac_on = false;
    int last_ok_ins = -1;               // dernier instrument valable sur CETTE voie
    for (int pat = 0; pat < orders; pat++) {
      for (uint32_t row = 0; row < rows_per_pattern; row++) {
        int16_t note = rd16s(&R), octave = rd16s(&R), vol = rd16s(&R);
        int16_t ecode[8], eval[8];
        for (int e = 0; e < fx; e++) { ecode[e] = rd16s(&R); eval[e] = rd16s(&R); }
        int16_t ins = rd16s(&R);
        if (R.bad) { free(dac_want); free(slots); free(matrix); return false; }

        uint32_t orow = row / (uint32_t)divisor;
        int q = (int)(orow / DMF_PHRASE_ROWS);
        int k = (int)(orow % DMF_PHRASE_ROWS);
        if (q >= quarters) continue;
        dmf_row_t *dst =
            &slots[((size_t)c * orders + pat) * quarters + q].rows[k];

        // Deux lignes DefleMask arrivent sur la même des nôtres.
        //
        // Cas très courant : une note suivie IMMÉDIATEMENT d'un note-off —
        // c'est un staccato. Jeter le note-off laisserait la note sonner
        // jusqu'au prochain évènement, ce qui s'entend beaucoup. On le repousse
        // donc d'une ligne quand la suivante est libre : la note dure deux
        // lignes DefleMask au lieu d'une, mais elle S'ARRÊTE.
        //
        // Une note qui en écrase une autre, en revanche, n'est pas déplaçable :
        // décaler une note change le rythme. Celle-là est bien perdue.
        // L'effet 17xx vaut pour SA PROPRE ligne : on le lit donc avant la
        // note. Sans ça, la première note posée en même temps que le « 17 01 »
        // — celle de la ligne 00, systématiquement — restait sans échantillon.
        for (int e = 0; e < fx; e++)
          if (ecode[e] == 0x17) dac_on = (eval[e] > 0);

        // Le suivi de l'instrument courant vaut pour TOUTE ligne qui en pose
        // un, note ou pas : c'est l'état de la voie, pas celui de la note.
        if (ins >= 0 && ins < MD_MAX_INSTRUMENTS && dmf_instr_fits(ins + 1, c))
          last_ok_ins = ins;

        uint8_t nn = dmf_note(note, octave);
        // Voie DAC : la note ne dit pas une hauteur mais QUEL échantillon.
        if (dac_on && c == MD_PCM_CHANNEL && nn && nn != MD_EMPTY &&
            note >= 0 && note <= 12) {
          dac_want[((size_t)((size_t)c * orders + pat) * quarters + q)
                   * DMF_PHRASE_ROWS + k] = nn;   // la note du fichier
        }
        if (nn == MD_EMPTY && dst->note && dst->note != MD_EMPTY) {
          dmf_row_t *nxt = NULL;
          if (k + 1 < DMF_PHRASE_ROWS)
            nxt = &slots[((size_t)c * orders + pat) * quarters + q].rows[k + 1];
          else if (q + 1 < quarters)
            nxt = &slots[((size_t)c * orders + pat) * quarters + q + 1].rows[0];
          if (nxt && !nxt->note) {
            nxt->note = MD_EMPTY;
            continue;                 // rien d'autre à poser pour un note-off
          }
          rep.rows_merged++;
          continue;
        }
        if (nn) {
          if (dst->note) {
            rep.rows_merged++;
          } else {
            dst->note = nn;
            // DefleMask pose volontiers un numéro d'instrument sur une ligne
            // sans note ; chez nous ça n'a pas de sens et ça encombre l'écran.
            if (nn != MD_EMPTY && ins >= 0 && ins < MD_MAX_INSTRUMENTS) {
              // Mauvais type pour la voie : on pose celui qui joue vraiment.
              int use = dmf_instr_fits(ins + 1, c) ? ins : last_ok_ins;
              if (use >= 0) dst->instr = (uint8_t)(use + 1);
            }
          }
        }
        if (vol >= 0 && dst->vel == MD_EMPTY)
          dst->vel = (uint8_t)(vol > 127 ? 127 : vol);

        // Les effets vont tous dans MD CMD : elle porte le jeu DefleMask au
        // complet, et la colonne CMD reste libre pour l'écriture à la main.
        for (int e = 0; e < fx; e++) {
          if (ecode[e] < 0) continue;
          // 17xx : déjà traité au-dessus. On l'absorbe — il devient le TYPE
          // de l'instrument chez nous — plutôt que de le compter comme perdu.
          if (ecode[e] == 0x17) continue;
          int cmd; uint8_t v; bool is_md;
          if (md_dmf_effect_map((uint8_t)ecode[e],
                                (uint8_t)(eval[e] < 0 ? 0 : eval[e]),
                                &cmd, &v, &is_md)) {
            if (dst->mdcmd == MD_EMPTY) { dst->mdcmd = (uint8_t)cmd; dst->mdval = v; }
            else rep.effects_dropped++;   // une seule colonne MD par ligne
          } else {
            rep.effects_unsupported++;
          }
        }
      }
    }
  }
  // ── Échantillons ──────────────────────────────────────────────────────
  // Disposition trouvée en sondant un vrai fichier : 4 octets de longueur (en
  // PAS, pas en octets), le nom, puis DOUZE octets de paramètres — dont la
  // fréquence, la hauteur, l'amplitude et le nombre de bits — et enfin les
  // données en 16 bits signés.
  //
  // On les rééchantillonne vers la cadence du pilote et on les convertit en
  // 8 bits, ce que recevra le DAC : la lecture n'a alors plus rien à calculer.
  rep.samples_ignored = 0;
  // Quelles notes la voie DAC emploie-t-elle ? Les motifs sont déjà lus, donc
  // on le sait avant même de charger la banque.
  bool dac_note_used[MD_MAX_NOTE + 1];
  int  bank_of_note[MD_MAX_NOTE + 1];
  for (int i = 0; i <= MD_MAX_NOTE; i++) { dac_note_used[i] = false; bank_of_note[i] = -1; }
  for (size_t i = 0; i < nslots * DMF_PHRASE_ROWS; i++) {
    uint8_t nt = dac_want[i];
    if (nt >= 1 && nt <= MD_MAX_NOTE) dac_note_used[nt] = true;
  }
  if (!R.bad && R.p < R.n) {
    // La table est lue à partir de UN, pas de zéro. Le fichier d'essai stocke
    // 4 pour un échantillon et 5 pour les autres ; en lisant à partir de zéro
    // j'obtenais 32 000 et 44 100, et tout sortait environ 1,4 fois trop vite
    // — un demi-ton près de six trop haut, ce qui s'entend tout de suite.
    //
    // Vérifié deux fois, indépendamment :
    //  - à l'oreille, contre DefleMask : le snareclap demande 22 594 Hz et le
    //    snare2 31 954 Hz, soit 22 050 et 32 000 à moins de 2,5 % ;
    //  - sur l'enregistrement, en cherchant la cadence qui fait le mieux
    //    coïncider l'échantillon d'origine avec le son entendu, fenêtre de
    //    150 ms pour que la décroissance compte : mi-5 (snare2) donne 32 500 Hz
    //    et rejette 38 000 (−0,07 contre +0,34).
    static const int dmf_rates[] = {8000, 11025, 16000, 22050, 32000, 44100, 48000};
    int nsamp = rd8(&R);
    for (int i = 0; i < nsamp && !R.bad; i++) {
      uint32_t steps = rd32(&R);
      char name[24];
      rdstr(&R, name, sizeof(name));
      uint8_t rate_idx = rd8(&R);
      rd8(&R);                                  // hauteur d'origine
      rd8(&R);                                  // amplitude
      uint8_t bits = rd8(&R);
      rdskip(&R, 8);                            // le reste des paramètres
      if (R.bad || steps == 0 || steps > (1u << 22)) break;

      const uint8_t *pcm16 = R.d + R.p;
      if (R.p + steps * 2 > R.n) break;
      rdskip(&R, steps * 2);

      int ri = (int)rate_idx - 1;                // la table part de un
      int src_rate = (ri >= 0 && ri < (int)(sizeof(dmf_rates) / sizeof(dmf_rates[0])))
                         ? dmf_rates[ri] : 16000;
      // ── Conversion vers la cadence du pilote, hauteur CONSERVÉE ────────
      //
      // Chaque échantillon de DefleMask porte sa propre cadence ; notre banque
      // n'en a qu'une, celle du pilote — comme une vraie cartouche, qui débite
      // tout à un seul rythme. On rééchantillonne donc ici, ce qui garde la
      // hauteur d'origine et permet à la phrase d'afficher la note du fichier.
      //
      // En descendant, on MOYENNE la fenêtre source au lieu de piocher un
      // point : sans ça les aigus se replient et le son grésille. En montant,
      // on interpole entre deux points.
      double ratio = (double)src_rate / (double)MD_PCM_RATE;   // pas source par pas sortie
      if (ratio <= 0.0) ratio = 1.0;
      uint32_t out_len = (uint32_t)((double)steps / ratio);
      if (out_len == 0) out_len = 1;
      if (out_len > MD_PCM_BANK_BYTES) out_len = MD_PCM_BANK_BYTES;
      uint8_t *out = (uint8_t *)malloc(out_len);
      if (!out) break;
      for (uint32_t k = 0; k < out_len; k++) {
        double t = (double)k * ratio;
        double acc;
        if (ratio > 1.0) {
          uint32_t a = (uint32_t)t;
          uint32_t b = (uint32_t)(t + ratio);
          if (b > steps) b = steps;
          if (b <= a) b = a + 1 <= steps ? a + 1 : steps;
          double sum = 0.0; uint32_t cnt = 0;
          for (uint32_t j = a; j < b; j++) {
            sum += (double)(int16_t)(pcm16[j * 2] | (pcm16[j * 2 + 1] << 8));
            cnt++;
          }
          acc = cnt ? sum / (double)cnt : 0.0;
        } else {
          uint32_t a = (uint32_t)t;
          uint32_t b = a + 1 < steps ? a + 1 : a;
          double fr = t - (double)a;
          double va = (double)(int16_t)(pcm16[a * 2] | (pcm16[a * 2 + 1] << 8));
          double vb = (double)(int16_t)(pcm16[b * 2] | (pcm16[b * 2 + 1] << 8));
          acc = va + (vb - va) * fr;
        }
        // 16 bits signés -> 8 bits non signés, centrés sur 0x80. On ARRONDIT :
        // tronquer ajoutait un souffle constant.
        int q = (int)floor(acc / 256.0 + 0.5) + 128;
        out[k] = (uint8_t)(q < 0 ? 0 : (q > 255 ? 255 : q));
      }
      (void)bits;                               // toujours 16 dans la pratique

      // Une entrée de banque PAR NOTE qui emploie cet échantillon, calée sur
      // cette note : la lecture tombe alors exactement sur la cadence
      // naturelle, et la phrase peut afficher la note du fichier au lieu d'une
      // note de référence unique. Deux notes sur le même échantillon en
      // dupliquent les octets — quelques kilo-octets sur une banque de 512 Ko.
      int added = 0;
      for (int nt = 1; nt <= MD_MAX_NOTE; nt++) {
        if (!dac_note_used[nt] || (nt - 1) % 12 != i) continue;
        int b = md_replayer_add_sample(name, out, out_len, -1, nt);
        if (b >= 0) { bank_of_note[nt] = b; added++; }
        else rep.samples_ignored++;             // banque pleine
      }
      if (added == 0) {
        // Aucune note ne le réclame : on le charge quand même, il reste à
        // disposition dans la banque.
        if (md_replayer_add_sample(name, out, out_len, -1, 49) >= 0) added = 1;
        else rep.samples_ignored++;
      }
      if (added > 0) rep.samples_imported++;    // on compte les SOURCES
      free(out);
    }
  }

  // ── Instruments de la voie DAC ────────────────────────────────────────
  // Les échantillons sont maintenant dans la banque, un par note employée. On
  // crée un instrument PCM par entrée, et on laisse à la ligne LA NOTE DU
  // FICHIER : l'écran montre alors ce que montre DefleMask, et comme l'entrée
  // est calée sur cette note, l'échantillon sort à sa cadence naturelle.
  {
    int pcm_instr[MD_MAX_SAMPLES];
    for (int i = 0; i < MD_MAX_SAMPLES; i++) pcm_instr[i] = 0;
    for (size_t i = 0; i < nslots; i++) {
      for (int k = 0; k < DMF_PHRASE_ROWS; k++) {
        uint8_t want = dac_want[i * DMF_PHRASE_ROWS + k];   // la note
        if (!want || want > MD_MAX_NOTE) continue;
        int smp = bank_of_note[want];
        if (smp < 0) continue;                  // aucun échantillon pour ce demi-ton
        uint32_t len = 0;
        if (!md_replayer_get_sample(smp, NULL, 0, &len, NULL, NULL) || len == 0)
          continue;                       // échantillon absent du fichier
        if (pcm_instr[smp] == 0) {
          // Un emplacement libre après les instruments du fichier.
          int slot = ninst + 1 + smp;
          if (slot < 1 || slot > MD_MAX_INSTRUMENTS) continue;
          char nm[24];
          md_replayer_get_sample(smp, nm, sizeof(nm), NULL, NULL, NULL);
          md_replayer_init_default_instrument(slot - 1);
          md_replayer_set_instr_name(slot, nm);
          md_replayer_set_instr_kind(slot, MD_INSTR_KIND_PCM);
          md_replayer_set_instr_sample(slot, smp);
          pcm_instr[smp] = slot;
          rep.instruments++;
        }
        slots[i].rows[k].instr = (uint8_t)pcm_instr[smp];
        slots[i].rows[k].note = want;           // la note telle qu'écrite
      }
    }
  }

  // ── Dédoublonnage : une phrase distincte, une seule fois ──────────────
  dmf_phrase_t *pool = (dmf_phrase_t *)calloc(MD_MAX_PHRASES, sizeof(dmf_phrase_t));
  int *slot_phrase = (int *)malloc(nslots * sizeof(int));
  if (!pool || !slot_phrase) { free(dac_want); free(pool); free(slot_phrase); free(slots); free(matrix); return false; }
  int npool = 0;

  for (size_t i = 0; i < nslots; i++) {
    bool empty = true;
    for (int k = 0; k < DMF_PHRASE_ROWS && empty; k++) {
      const dmf_row_t *r0 = &slots[i].rows[k];
      if (r0->note || r0->instr || r0->vel != MD_EMPTY || r0->mdcmd != MD_EMPTY)
        empty = false;
    }
    if (empty) { slot_phrase[i] = -1; continue; }
    int found = -1;
    for (int p = 0; p < npool; p++)
      if (memcmp(&pool[p], &slots[i], sizeof(dmf_phrase_t)) == 0) { found = p; break; }
    if (found < 0) {
      if (npool >= MD_MAX_PHRASES) { rep.truncated = true; slot_phrase[i] = -1; continue; }
      pool[npool] = slots[i];
      found = npool++;
    }
    slot_phrase[i] = found;
  }
  rep.phrases_used = npool;

  // Les phrases portent des numéros à partir de 1 : 0 reste la phrase vide.
  for (int p = 0; p < npool && p + 1 < MD_MAX_PHRASES; p++)
    for (int k = 0; k < DMF_PHRASE_ROWS; k++) {
      const dmf_row_t *r0 = &pool[p].rows[k];
      md_replayer_set_phrase(p + 1, k, r0->note, r0->instr, r0->vel,
                             r0->cmd, r0->cmdval, r0->mdcmd, r0->mdval);
    }

  // ── Chaînes : une ligne de matrice = un motif = `quarters` phrases ─────
  // Une phrase de SILENCE, pour boucher les trous.
  //
  // Dans le tracker d'exemple, une case SONG vide TERMINE le bloc du canal :
  // il reboucle alors sur son bout de morceau pendant que les autres
  // continuent, et tout se disloque. DefleMask, lui, a des motifs vides en
  // pagaille. On ne laisse donc AUCUN trou : un motif silencieux devient une
  // chaîne de phrases vides, qui occupe le même temps que les autres.
  int silence = npool + 1;
  if (silence >= MD_MAX_PHRASES) silence = -1;   // plus de place : tant pis

  int (*chain_key)[MD_ROWS_PER_CHAIN] =
      calloc(MD_MAX_CHAINS, sizeof(*chain_key));
  if (!chain_key) { free(dac_want); free(pool); free(slot_phrase); free(slots); free(matrix); return false; }
  int nchain = 0;

  for (int c = 0; c < CH; c++) {
    for (int o = 0; o < orders && o < MD_SONG_ROWS; o++) {
      // Le bloc de cette ligne, c'est celui d'INDICE `o` — la ligne elle-même.
      // Prendre `matrix[...]` comme indice revenait à lire le bloc d'une autre
      // ligne : les canaux jouaient les mesures de leurs voisines.
      int key[MD_ROWS_PER_CHAIN];
      for (int k = 0; k < MD_ROWS_PER_CHAIN; k++) key[k] = -1;
      for (int q = 0; q < quarters && q < MD_ROWS_PER_CHAIN; q++)
        key[q] = slot_phrase[((size_t)c * orders + o) * quarters + q];
      // Chaque ligne de SONG occupe le MÊME nombre de phrases sur tous les
      // canaux : les quarts silencieux prennent la phrase de silence. Sans ça
      // un canal muet raccourcissait son bloc et rebouclait tout seul.
      if (silence >= 0)
        for (int q = 0; q < quarters && q < MD_ROWS_PER_CHAIN; q++)
          if (key[q] < 0) key[q] = silence - 1;   // -1 : on ajoute +1 plus bas

      int found = -1;
      for (int ci = 0; ci < nchain; ci++)
        if (memcmp(chain_key[ci], key, sizeof(key)) == 0) { found = ci; break; }
      if (found < 0) {
        if (nchain >= MD_MAX_CHAINS) { rep.truncated = true; md_replayer_set_song(c, o, MD_EMPTY); continue; }
        memcpy(chain_key[nchain], key, sizeof(key));
        found = nchain++;
        for (int k = 0; k < MD_ROWS_PER_CHAIN; k++)
          md_replayer_set_chain(found, k,
                                key[k] < 0 ? MD_EMPTY : (uint8_t)(key[k] + 1), 0);
      }
      md_replayer_set_song(c, o, (uint8_t)found);
    }
  }
  rep.chains_used = nchain;

  // ── Vitesse ───────────────────────────────────────────────────────────
  // La « Base Time » AFFICHÉE par DefleMask vaut ce qui est stocké PLUS UN :
  // le fichier d'essai range 2 et l'écran annonce 3. Sans ce +1 le morceau
  // allait une fois et demie trop vite.
  //
  // Une ligne dure BaseTime × Speed ticks, et DefleMask alterne Speed A et
  // Speed B d'une ligne à l'autre. Notre moteur n'a qu'une vitesse : on prend
  // la moyenne des deux, ce qui est exact quand elles sont égales (le cas le
  // plus courant) et approché sinon.
  int speed = ((int)timebase + 1) * ((int)tick1 + (int)tick2);
  speed = (speed + 1) / 2;                    // moyenne, arrondie
  speed *= divisor;                           // deux lignes fondues = deux fois
                                              // plus de ticks par ligne
  if (speed < 1) speed = 1;
  if (speed > 31) speed = 31;
  md_replayer_set_speed(speed);
  int hz = custom ? ((int)hz1 + (int)hz2 * 100) : (frames ? 60 : 50);
  if (hz < 20 || hz > 200) hz = frames ? 60 : 50;
  md_replayer_set_tempo(hz);
  // Lignes par temps : c'est le « Highlight A » de DefleMask. Il ne change RIEN
  // à la vitesse de lecture — celle-ci vient du rapport tempo/vitesse, déjà
  // réglé — mais il décide du BPM affiché.
  //
  // Avec le fichier d'essai : 60 Hz ÷ (3 ticks × 8 lignes) × 60 = 150 BPM,
  // exactement ce qu'annonce DefleMask.
  md_replayer_set_rows_per_beat(highlight_a > 0 ? (int)highlight_a / divisor : 4);
  (void)highlight_b;

  free(chain_key); free(pool); free(slot_phrase); free(dac_want); free(slots); free(matrix);
  if (report) *report = rep;
  return true;
}
