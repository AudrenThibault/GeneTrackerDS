#include "md_replayer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../MegaDrive/md_chip.h"
#include "md_lock.h"

md_lock_t replayer_lock = MD_LOCK_INIT;

static md_module_t *current_module = NULL;
static md_play_status_t play_status = MD_STOPPED;
static int replay_sample_rate = 44100;

// ============================================================================
// Représentation interne de la hauteur
//
// On CONSERVE la représentation « block(3 bits) | F-Num(10 bits) » héritée du
// moteur d'origine : tous les effets (slides, portamento, vibrato, arpège) ont
// été écrits autour d'elle et continuent donc de fonctionner à l'identique.
// Elle est traduite en Hz puis en registres YM2612 / SN76489 au moment de
// l'écriture (md_apply_pitch).
// ============================================================================

// Table de F-Num pour les 12 demi-tons (référence de l'octave).
static const uint16_t md_note_fnums[12] = {
    0x157 /*C*/,  0x16B /*C#*/, 0x181 /*D*/,  0x198 /*D#*/,
    0x1B0 /*E*/,  0x1CA /*F*/,  0x1E5 /*F#*/, 0x202 /*G*/,
    0x220 /*G#*/, 0x241 /*A*/,  0x263 /*A#*/, 0x287 /*B*/
};

// Horloge de référence utilisée pour convertir block/F-Num en Hz.
// L'octave 0 du tracker correspond au bloc 0 du YM2612 : C-0 = 16.35 Hz,
// la note la plus grave que la puce sache produire.
#define MD_PITCH_REF_CLOCK 3579545.0

// Transposition propre aux voies PSG.
//
// Le compteur de période du SN76489 fait 10 bits : à 1023 il est au bout, soit
// 3579545/(32*1023) = 109.35 Hz. La puce ne descend pas plus bas, point.
// Sans correction, tout ce qui est écrit sous cette limite sature sur la même
// note et C-0 = C-1 = C-2 = un bourdon plat.
//
// Les canaux PSG ont donc leur propre transposition : 2 octaves vers l'aigu.
// La transposition est appliquée au tout dernier moment, à l'écriture du
// registre : la grille de notes, les slides, le portamento et le vibrato
// continuent de travailler sur la même représentation que le FM.
//
// Pourquoi 2 et pas 3 : avec 3, une note écrite sur une voie PSG sonnait une
// octave au-dessus de la même note dans DefleMask — il fallait écrire C-3 pour
// obtenir son C-4. Vérifié à l'oreille sur un morceau importé.
//
// ⚠️ À numéro de note égal, une voie PSG sonne donc 2 octaves au-dessus d'une
// voie FM, et les neuf demi-tons sous A-0 tapent le plancher de la puce et
// sonnent tous pareil. C'est le prix à payer pour que les noms de notes du PSG
// veuillent dire la même chose ici et dans DefleMask.
// Vérifié à l'oreille contre DefleMask : avec 3, une note écrite sur une voie
// PSG sonnait UNE OCTAVE TROP HAUT — il fallait écrire C-3 pour obtenir le C-4
// du fichier d'origine. 2 remet les noms de notes du PSG en accord avec ce que
// joue DefleMask.
//
// Contrepartie assumée : le SN76489 ne descend pas sous 109 Hz (période 10 bits
// à 3,58 MHz). Avec ce décalage, les notes en dessous de A-0 tapent ce plancher
// et sonnent toutes pareil. Ça ne concerne plus que les neuf demi-tons les plus
// graves, contre presque trois octaves si on retirait une octave de plus.
#define MD_PSG_OCTAVE_SHIFT 2

// ── État par canal logique ──────────────────────────────────────────────────
// bit 5 (0x20) = key-on, bits 0-4 = poids forts de block/F-Num. Format conservé
// tel quel : de nombreux effets le lisent/écrivent directement.
static uint8_t ch_b0_reg[MD_TOTAL_VOICES] = {0};
static uint16_t ch_cur_fnum[MD_TOTAL_VOICES] = {0}; // hauteur réellement émise
static int ch_current_instr[MD_TOTAL_VOICES] = {0};
static uint8_t ch_current_note[MD_TOTAL_VOICES] = {0};

// ── Tables d'instrument en cours de lecture ─────────────────────────────────
// Une ligne par tick, rebouclée tant que la note dure. Le canal mémorise aussi
// la transposition courante, pour ne réécrire la hauteur que lorsqu'elle change
// — sinon on écraserait à chaque tick le travail des slides et du vibrato.
static int ch_table[MD_TOTAL_VOICES];       // table en cours, -1 = aucune
static bool ch_table_running[MD_TOTAL_VOICES];
static int8_t ch_table_tsp[MD_TOTAL_VOICES];
// Position de chacun des trois flux (voir MD_TABLE_STREAMS dans l'en-tête).
static uint8_t ch_table_pos[MD_TOTAL_VOICES][MD_TABLE_STREAMS];
// Compteur de répétitions restantes pour chaque H rencontré. Il est indexé par
// la LIGNE qui porte le saut, ce qui permet d'imbriquer les boucles : chaque H
// compte ses propres tours sans écraser ceux d'un H englobant.
static uint8_t ch_hop_left[MD_TOTAL_VOICES][MD_TABLE_STREAMS][MD_TABLE_ROWS];
// La table de ce canal a-t-elle été imposée par une commande A ? Dans ce cas
// elle l'emporte sur celle de l'instrument, et le reste jusqu'au prochain A —
// une note suivante ne doit pas la reprendre.
static bool ch_table_forced[MD_TOTAL_VOICES];
static uint8_t ch_panning[MD_TOTAL_VOICES];

// Voie matérielle utilisée par chaque canal logique.
//   0-5 → FM 1-6 (YM2612)   6-9 → PSG (3 tons + bruit)
// Le canal d'audition (MD_TEST_CHANNEL) vole dynamiquement une voie FM libre.
static uint8_t ch_hw_voice[MD_TOTAL_VOICES];

// Volumes abstraits (atténuation 0 = fort … 63 = silence), exactement comme
// dans le moteur d'origine : les effets de volume n'ont pas bougé d'une ligne.
typedef struct {
  uint8_t volC;         // atténuation appliquée aux opérateurs PORTEURS
  uint8_t volM;         // atténuation appliquée aux opérateurs MODULATEURS
  uint8_t feedbackConn; // bit 0 = algorithme additif (ALG >= 4), bits 1-3 = FB
} md_voice_par_t;
static md_voice_par_t ch_fmpar[MD_TOTAL_VOICES];

// Copie de travail de l'instrument par canal : les effets « live » (EXTENDED3,
// set algorithm…) la modifient sans toucher à l'instrument du module.
static md_instr_t ch_instr_shadow[MD_TOTAL_VOICES];

// ── Réglages MD CMD encore actifs sur le canal ──────────────────────────────
// Une commande MD CMD règle un REGISTRE de la puce, pas une note : elle doit
// donc tenir tant qu'on ne la change pas, y compris sur les notes suivantes.
//
// Or chaque note recharge l'instrument dans la copie de travail et effaçait la
// retouche : un « 20 10 » posé une ligne plus haut était bien appliqué, puis
// annulé par la note d'après, qui sonnait donc sans lui.
//
// On mémorise donc ce qui a été posé, et on le rejoue après chaque
// rechargement d'instrument.
#define MD_MDCMD_SLOTS 64
// Vrai quand la colonne porte un instrument destiné à l'autre puce : elle
// reste muette jusqu'à ce qu'un instrument compatible y soit posé.
static bool ch_instr_blocked[MD_TOTAL_VOICES];
static uint8_t ch_mdcmd_val[MD_TOTAL_VOICES][MD_MDCMD_SLOTS];
static bool ch_mdcmd_on[MD_TOTAL_VOICES][MD_MDCMD_SLOTS];
// Ce réglage vient-il d'une TABLE (et non d'une phrase) ? Voir md_refresh_table.
static bool ch_mdcmd_from_table[MD_TOTAL_VOICES][MD_MDCMD_SLOTS];

// Rejoue sur le canal tous les réglages MD CMD encore actifs.
static void md_apply_mdcmd(int c, uint8_t cmd, uint8_t val,
                           uint8_t row_cmd, int slot);
// Le mode de bruit est-il tenu par une commande sur ce canal ?
//
// Une commande MD CMD règle un registre et tient jusqu'à ce qu'on la change ;
// l'instrument, lui, se recharge à chaque note, et sa macro de bruit se rejoue
// à chaque tick. Sans ce garde-fou, l'instrument reprenait la main sur ce que
// la commande avait posé — c'est bien la COMMANDE qui doit primer.
static bool md_noise_forced(int c);   // défini avec la table des commandes

static void md_reapply_mdcmds(int c) {
  if (c < 0 || c >= MD_TOTAL_VOICES) return;
  for (int i = 0; i < MD_MDCMD_SLOTS && i < md_mdcmd_count(); i++)
    if (ch_mdcmd_on[c][i])
      md_apply_mdcmd(c, (uint8_t)i, ch_mdcmd_val[c][i], MD_EMPTY, -1);
}

// Runtime Effect Tracking
static uint8_t ch_volslide_val[MD_TOTAL_VOICES] = {0};
static uint16_t ch_porta_target[MD_TOTAL_VOICES] = {0};
static uint8_t ch_porta_speed[MD_TOTAL_VOICES] = {0};
static uint8_t ch_fslide_speed[MD_TOTAL_VOICES] = {0};
static uint8_t ch_vibrato_speed[MD_TOTAL_VOICES] = {0};
static uint8_t ch_vibrato_depth[MD_TOTAL_VOICES] = {0};
static uint8_t ch_vibrato_pos[MD_TOTAL_VOICES] = {0};
static uint8_t ch_tremolo_speed[MD_TOTAL_VOICES] = {0};
static uint8_t ch_tremolo_depth[MD_TOTAL_VOICES] = {0};
static uint8_t ch_tremolo_pos[MD_TOTAL_VOICES] = {0};
static uint8_t ch_tremor_val[MD_TOTAL_VOICES] = {0};
static uint8_t ch_tremor_count[MD_TOTAL_VOICES] = {0};
static uint8_t ch_arpeggio_val[MD_TOTAL_VOICES] = {0};
static uint8_t ch_arpeggio_state[MD_TOTAL_VOICES] = {0};
static uint16_t ch_base_fnum[MD_TOTAL_VOICES] = {0}; // hauteur de référence

// Arpège de prévisualisation (audition piano). -1 = inactif.
static int test_arp_channel = -1;

// Canal de la chanson sur lequel se trouve le curseur d'édition. L'audition
// (piano, saisie d'une note) emprunte une voie du MÊME type que ce canal :
// sans ça, écrire une note sur PSG1 ou sur NOIS faisait entendre du FM, ce qui
// n'a aucun sens — sur Mega Drive ces colonnes ne sortent que du SN76489.
static int audition_channel = 0;

// Globals
static uint8_t replay_global_vol = 63;
// Emplacements d'effet actifs par canal :
//   0-1 → les commandes de la ligne de PHRASE (le moteur d'origine)
//   2-3 → les deux colonnes CMD de la TABLE de l'instrument
// Les séparer évite qu'une table écrase la commande écrite dans la phrase.
#define MD_EFF_SLOTS 6
#define MD_EFF_SLOT_TABLE 2
// La colonne MD CMD peut porter un effet DefleMask : il lui faut son propre
// emplacement, sinon elle écraserait la commande CMD de la même ligne.
#define MD_EFF_SLOT_MDCMD 4        // MD CMD d'une ligne de phrase
#define MD_EFF_SLOT_TABLE_MD 5     // MD CMD d'une ligne de table
static uint8_t ch_active_eff[MD_TOTAL_VOICES][MD_EFF_SLOTS] = {
    [0 ... MD_TOTAL_VOICES - 1] = {0xFF, 0xFF, 0xFF, 0xFF}};
static uint8_t ch_active_eff_val[MD_TOTAL_VOICES][MD_EFF_SLOTS] = {0};
static uint8_t ch_note_delay[MD_TOTAL_VOICES] = {0};
static uint8_t ch_note_cut[MD_TOTAL_VOICES] = {0};
static uint8_t ch_retrig_speed[MD_TOTAL_VOICES] = {0};
static uint8_t ch_retrig_count[MD_TOTAL_VOICES] = {0};

// Global effect states
static int8_t global_volslide_val = 0;
static int16_t global_fslide_speed = 0;

static bool md_channel_muted[32] = {false};

static const uint8_t vibtrem_table[32] = {
    0,   24,  49,  74,  97,  120, 141, 161, 180, 197, 212,
    224, 235, 244, 250, 253, 255, 253, 250, 244, 235, 224,
    212, 197, 180, 161, 141, 120, 97,  74,  49,  24};

// ============================================================================
// Couche YM2612 / SN76489
// ============================================================================

// Ordre des opérateurs dans les registres du YM2612 : OP1, OP3, OP2, OP4.
// Index logique 0..3 = OP1..OP4 → décalage de registre.
static const uint8_t md_op_reg_offset[4] = {0x00, 0x08, 0x04, 0x0C};

// Quels opérateurs sortent du son, selon l'algorithme (bit 0 = OP1 … bit 3 = OP4).
static const uint8_t md_carrier_mask[8] = {0x08, 0x08, 0x08, 0x08,
                                           0x0A, 0x0E, 0x0E, 0x0F};

static inline bool md_hw_is_psg(uint8_t hw) { return hw >= MD_NUM_FM_CHANNELS; }
static inline uint8_t md_hw_psg_index(uint8_t hw) {
  return (uint8_t)(hw - MD_NUM_FM_CHANNELS);
}
static inline uint8_t md_hw_fm_part(uint8_t hw) { return (uint8_t)(hw / 3); }
static inline uint8_t md_hw_fm_index(uint8_t hw) { return (uint8_t)(hw % 3); }
static inline uint8_t md_hw_fm_keycode(uint8_t hw) {
  return (uint8_t)((hw < 3) ? hw : (hw + 1)); // 0,1,2, puis 4,5,6
}

// ── Enveloppe des voies PSG ─────────────────────────────────────────────────
// Le SN76489 n'a pas d'enveloppe matérielle : son unique contrôle de niveau est
// un registre d'atténuation sur 4 bits. Sur une vraie Megadrive, c'est le CPU
// qui le réécrit image par image en déroulant une table de volumes — c'est ce
// que font SMPS, GEMS ou Echo. On fait exactement pareil : un pas de table par
// tick du replayer, valeurs 0-15, avec un point de boucle et un point de
// release. Rien ici n'emprunte quoi que ce soit aux paramètres FM.
typedef struct {
  uint8_t active;    // 0 = voie au repos
  uint8_t point;     // point de rupture courant de l'enveloppe (0 … 2)
  int32_t level256;  // amplitude courante en 1/256e d'unité (0 … 15*256)
  uint8_t instr;     // instrument dont on lit les réglages (0 = aucun)
  uint8_t noise_mode;
  uint8_t nz_pos;    // index courant dans la macro de mode de bruit
  uint8_t vol_pos;   // index courant dans la macro de VOLUME (v9)
  uint8_t arp_pos;   // index courant dans la macro d'ARPÈGE (v9)
} md_psg_env_t;
static md_psg_env_t psg_env[MD_NUM_PSG_CHANNELS];

// Index suivant d'une macro de longueur `len` : on reboucle si un point de
// bouclage est défini, sinon on tient la dernière valeur.
static int md_macro_next(int pos, int len, uint8_t loop) {
  int next = pos + 1;
  if (next < len)
    return next;
  if (loop != MD_EMPTY && loop < len)
    return loop;
  return len - 1;
}

// Amplitude audible courante de la voie, de 0 (silence) à 15 (maximum).
static uint8_t md_env_level(const md_psg_env_t *e) {
  int v = e->level256 >> 8;
  return (uint8_t)(v < 0 ? 0 : (v > 15 ? 15 : v));
}

// Vitesse de rampe, en 1/256e d'unité d'amplitude par tick.
//
// Le manuel de LSDJ dit « Speed 1 is fastest, F is slowest, 0 means hold ».
// On traduit ça par un temps de parcours de toute l'échelle qui croît comme le
// carré de la vitesse : 1 → un tick (instantané), 8 → un peu plus d'une
// seconde, F → environ quatre secondes et demie. Plus le chiffre monte, plus le
// segment s'étire — c'est bien le sens de l'écran.
static int32_t md_env_rate(uint8_t speed) {
  if (speed == 0)
    return 0;                       // 0 = tenir : on ne bouge pas
  int32_t r = (15 * 256) / ((int32_t)speed * speed);
  return r < 1 ? 1 : r;
}

// Amplitude de départ de l'enveloppe (point 0). Un instrument sans enveloppe
// définie sonne au maximum, comme une voie dont on écrirait l'atténuation une
// seule fois.
static int32_t md_env_start_level(const md_instr_t *ins) {
  if (!ins || ins->env_amp[0] == MD_EMPTY)
    return 15 * 256;
  uint8_t a = ins->env_amp[0];
  return (int32_t)(a > 15 ? 15 : a) * 256;
}

// Instrument dont la voie PSG `p` lit ses réglages (NULL si aucun).
static const md_instr_t *md_psg_env_instr(const md_psg_env_t *e) {
  if (!current_module || e->instr == 0 || e->instr > MD_MAX_INSTRUMENTS)
    return NULL;
  return &current_module->instruments[e->instr - 1];
}

// Convertit la représentation interne block/F-Num en fréquence réelle.
static double md_fnum_to_hz(uint16_t fnum_block) {
  uint32_t fnum = fnum_block & 0x03FF;
  uint32_t block = (fnum_block >> 10) & 0x0F;
  double denom = 72.0 * (double)(1u << (20 - block));
  return (double)fnum * MD_PITCH_REF_CLOCK / denom;
}

// Écrit la hauteur sur la voie matérielle du canal.
static void md_apply_pitch(int ch) {
  if (ch < 0 || ch >= MD_TOTAL_VOICES)
    return;
  uint8_t hw = ch_hw_voice[ch];
  double hz = md_fnum_to_hz(ch_cur_fnum[ch]);
  if (hz < 1.0)
    hz = 1.0;

  if (md_hw_is_psg(hw)) {
    uint8_t p = md_hw_psg_index(hw);
    hz *= (double)(1u << MD_PSG_OCTAVE_SHIFT);
    if (p == 3) {
      // Canal de bruit. Le SN76489 n'offre que trois périodes fixes plus un
      // mode « verrouillé sur le ton 3 » : seul ce dernier permet un bruit dont
      // la hauteur suit la note. On écrit alors la période dans le registre de
      // la voie PSG3, que le générateur de bruit relit en continu.
      // ⚠️ Conséquence matérielle : dans ce mode, la colonne PSG3 partage son
      // registre de période avec le bruit.
      if ((psg_env[p].noise_mode & 0x03) == 0x03) {
        // Une octave DE PLUS vers le grave que les voies de ton.
        //
        // Le registre de période pilote ici le décalage du registre à
        // décalage du générateur de bruit, pas directement une fréquence
        // audible : à période égale, le bruit s'entend une octave au-dessus
        // du ton. Vérifié à l'oreille contre DefleMask, où la même note sur
        // le canal de bruit sonnait une octave trop haut chez nous.
        hz *= 0.5;
        int32_t period = (int32_t)((double)MD_PSG_CLOCK / (32.0 * hz) + 0.5);
        if (period < 1)
          period = 1;
        if (period > 1023)
          period = 1023;
        md_chip_psg_set_period(2, (uint16_t)period);
      }
      return;
    }
    int32_t period = (int32_t)((double)MD_PSG_CLOCK / (32.0 * hz) + 0.5);
    if (period < 1)
      period = 1;
    if (period > 1023)
      period = 1023;
    md_chip_psg_set_period(p, (uint16_t)period);
    return;
  }

  // ── YM2612 : on choisit le bloc pour garder le F-Num dans une octave ──
  // MD_YM_DIVISEUR et non 144 : la puce tourne a cadence reduite sur DS, et le
  // F-Num doit suivre, sinon tout le morceau descendrait d'une octave.
  double k = hz * (double)MD_YM_DIVISEUR * 2097152.0 / (double)MD_YM2612_CLOCK;
  int block = 0;
  while (k >= 1234.0 && block < 7) {
    k *= 0.5;
    block++;
  }
  int fnum = (int)(k + 0.5);
  if (fnum < 0)
    fnum = 0;
  if (fnum > 2047)
    fnum = 2047; // limite matérielle réelle du YM2612 (~6.6 kHz)

  uint8_t part = md_hw_fm_part(hw);
  uint8_t idx = md_hw_fm_index(hw);
  // L'ordre compte : le registre 0xA4 est latché, puis 0xA0 valide l'écriture.
  md_chip_ym_write(part, (uint8_t)(0xA4 + idx),
                   (uint8_t)((block << 3) | ((fnum >> 8) & 0x07)));
  md_chip_ym_write(part, (uint8_t)(0xA0 + idx), (uint8_t)(fnum & 0xFF));
}

// Met à jour la hauteur courante (et le cache ch_b0_reg) puis l'envoie à la puce.
static void md_write_freq(int ch, uint16_t fnum_block) {
  if (ch < 0 || ch >= MD_TOTAL_VOICES)
    return;
  ch_cur_fnum[ch] = fnum_block;
  ch_b0_reg[ch] = (uint8_t)((ch_b0_reg[ch] & 0xE0) | ((fnum_block >> 8) & 0x1F));
  md_apply_pitch(ch);
}

// Atténuation finale d'un canal : volume abstrait + volume global + mute.
static uint8_t md_effective_atten(int ch, uint8_t atten) {
  if (ch >= 0 && ch < 32 && md_channel_muted[ch])
    return 63;
  if (replay_global_vol >= 63)
    return atten;
  int loud = 63 - (int)atten;
  loud = (loud * ((int)replay_global_vol + 1)) >> 6;
  int res = 63 - loud;
  if (res > 63)
    res = 63;
  if (res < 0)
    res = 0;
  return (uint8_t)res;
}

// Applique une atténuation « porteuse » (0-63) sur le canal. `atten` peut être
// temporaire (trémolo, tremor) sans être mémorisée dans ch_fmpar.
static void md_write_carrier_volume(int ch, uint8_t atten) {
  if (ch < 0 || ch >= MD_TOTAL_VOICES || !current_module)
    return;
  uint8_t hw = ch_hw_voice[ch];
  uint8_t eff = md_effective_atten(ch, (uint8_t)(atten & 0x3F));

  if (md_hw_is_psg(hw)) {
    uint8_t p = md_hw_psg_index(hw);
    // Le registre du SN76489 est gradué en atténuation, ~2 dB par pas :
    // additionner les atténuations revient à multiplier les gains, donc la
    // table de volumes et le volume du canal se cumulent correctement ici.
    int env_at = psg_env[p].active ? (15 - (int)md_env_level(&psg_env[p])) : 15;
    int total = env_at + (eff >> 2);
    if (total > 15)
      total = 15;
    if (total < 0)
      total = 0;
    md_chip_psg_set_volume(p, (uint8_t)total);
    return;
  }

  const md_instr_t *ins = &ch_instr_shadow[ch];
  uint8_t mask = md_carrier_mask[ins->algorithm & 7];
  uint8_t part = md_hw_fm_part(hw);
  uint8_t idx = md_hw_fm_index(hw);

  // Atténuation supplémentaire en unités TL (0-127) : 63 → totalement muet.
  int extra = ((int)eff * 127) / 63;
  int extra_mod = ((int)md_effective_atten(ch, ch_fmpar[ch].volM) * 127) / 63;

  // OP1 porteuse ET bouclée sur elle-même : cas particulier. Sur le YM2612 le
  // Total Level d'OP1 règle à la fois son niveau ET l'intensité du feedback, si
  // bien que l'atténuer éteint le grain. Mesuré sur l'instrument « Ins 0 »
  // (algorithme 7, feedback 7) : dès 6 dB, la ressemblance du spectre tombe à
  // 0,60 et le centre spectral perd 59 % — et ça ne bouge plus ensuite. C'est
  // une falaise, pas une pente.
  //
  // On laisse donc OP1 tranquille dans ce cas et les autres porteuses encaissent
  // l'atténuation. C'est le contournement des musiciens sur la vraie machine, et
  // il s'exporte comme le reste puisque ce ne sont que des Total Level. Prix à
  // payer : une voix de ce genre descend moins que les autres.
  const bool op1_boucle =
      MD_FM_KEEP_FEEDBACK_GRIT && (mask & 1) && ins->feedback != 0;

  // Ce que les VOISINES d'une OP1 épargnée doivent encaisser en plus, calculé
  // pour cet instrument-là.
  //
  // But : que la voix descende d'exactement autant que les autres, sinon elle
  // reste debout pendant que l'orchestre baisse et l'arrangement se déforme.
  // Un pas de Total Level vaut 0,75 dB, donc l'amplitude d'une porteuse est
  // 10^(-0,0375 · TL) ; les porteuses s'additionnent. On cherche donc le
  // facteur à appliquer aux autres pour que la somme retombe sur la cible.
  //
  // Une valeur fixe ne pouvait pas marcher : elle dépend de la répartition des
  // TL, propre à chaque instrument. Réglée à la main sur un patch, elle laissait
  // 3 dB d'écart sur un autre.
  int comp = 0;
  if (op1_boucle) {
    double a1 = 0.0, autres = 0.0;
    for (int op = 0; op < 4; op++) {
      if (!(mask & (1 << op))) continue;
      double a = pow(10.0, -0.0375 * (double)ins->op[op].total_level);
      if (op == 0) a1 = a; else autres += a;
    }
    double cible = (a1 + autres) * pow(10.0, -0.0375 * (double)MD_FM_CARRIER_ATTEN);
    if (autres > 0.0 && cible > a1) {
      double besoin = (cible - a1) / autres;
      double pas = -log10(besoin) / 0.0375;
      comp = (int)(pas - (double)MD_FM_CARRIER_ATTEN + 0.5);
      if (comp < 0) comp = 0;
    } else {
      // OP1 seule dépasse déjà la cible : impossible d'y arriver sans la
      // toucher. On éteint les autres et on s'arrête là — c'est le plancher
      // que la puce impose.
      comp = 127;
    }
  }

  for (int op = 0; op < 4; op++) {
    int tl = (int)ins->op[op].total_level;
    // Les PORTEUSES portent en plus l'atténuation générale : c'est elle qui
    // laisse le bruit et le PCM ressortir sans sortir de ce que la machine
    // sait faire. Les modulateurs n'y touchent pas.
    if (mask & (1 << op))
      tl += extra + ((op == 0 && op1_boucle)
                         ? MD_FM_FEEDBACK_OP1_ATTEN
                         : MD_FM_CARRIER_ATTEN + comp);
    else
      tl += extra_mod;
    if (tl > 127)
      tl = 127;
    md_chip_ym_write(part, (uint8_t)(0x40 + md_op_reg_offset[op] + idx),
                     (uint8_t)tl);
  }
}

// Réécrit le volume courant du canal (porteuses + modulateurs).
static void md_write_volume(int ch) {
  md_write_carrier_volume(ch, ch_fmpar[ch].volC);
}

// Arme la table de l'instrument du canal (si l'instrument en a une). Appelée à
// chaque note : la table repart toujours de sa première ligne.
static void md_table_start(int ch) {
  ch_table_tsp[ch] = 0;
  // Les effets que la table avait armés repartent de zéro. Le moteur ne réarme
  // un effet que s'il CHANGE — sans cet oubli volontaire, un vibrato continu
  // serait relancé à chaque tick — mais du coup, une table modifiée pendant un
  // silence gardait l'ancien couple (effet, valeur) et la note suivante ne
  // rejouait rien de neuf.
  for (int sl = 0; sl < MD_TABLE_STREAMS - 1; sl++)
    ch_active_eff[ch][MD_EFF_SLOT_TABLE + sl] = 0xFF;
  for (int s = 0; s < MD_TABLE_STREAMS; s++) {
    ch_table_pos[ch][s] = 0;
    for (int r = 0; r < MD_TABLE_ROWS; r++)
      ch_hop_left[ch][s][r] = 0;
  }
  // Une commande A a imposé la table : elle prime sur celle de l'instrument.
  // On repart simplement de sa première ligne — mais il faut la RÉARMER, car
  // le note-off précédent a éteint la lecture. En sortant d'ici sans le faire,
  // la table se taisait dès la deuxième note et ne revenait qu'au prochain
  // départ de lecture : en composant, on modifiait la table sans plus rien
  // entendre bouger.
  if (ch_table_forced[ch]) {
    ch_table_running[ch] = (ch_table[ch] >= 0 && ch_table[ch] < MD_MAX_TABLES);
    return;
  }

  ch_table[ch] = -1;
  ch_table_running[ch] = false;
  if (!current_module)
    return;
  int ins_idx = ch_current_instr[ch];
  if (ins_idx < 1 || ins_idx > MD_MAX_INSTRUMENTS)
    return;
  uint8_t t = current_module->instruments[ins_idx - 1].table;
  if (t == MD_EMPTY || t >= MD_MAX_TABLES)
    return;
  ch_table[ch] = t;
  ch_table_running[ch] = true;
}

// Commande A : démarre la table donnée sur ce canal, ou l'arrête (valeur >= 20
// d'après le manuel de LSDJ). C'est la seule commande qui touche à la table
// elle-même plutôt qu'au son.
static void md_cmd_table_select(int ch, uint8_t val) {
  if (ch < 0 || ch >= MD_TOTAL_VOICES)
    return;
  // À partir d'ici c'est A qui décide, plus l'instrument.
  ch_table_forced[ch] = true;
  if (val >= 0x20 || val >= MD_MAX_TABLES) {
    ch_table_running[ch] = false;
    return;
  }
  ch_table[ch] = val;
  ch_table_running[ch] = true;
  ch_table_tsp[ch] = 0;
  for (int s = 0; s < MD_TABLE_STREAMS; s++) {
    ch_table_pos[ch][s] = 0;
    for (int r = 0; r < MD_TABLE_ROWS; r++)
      ch_hop_left[ch][s][r] = 0;
  }
}

static void md_key_on(int ch) {
  if (ch < 0 || ch >= MD_TOTAL_VOICES)
    return;
  md_table_start(ch);
  ch_b0_reg[ch] |= 0x20;
  uint8_t hw = ch_hw_voice[ch];
  if (md_hw_is_psg(hw)) {
    uint8_t p = md_hw_psg_index(hw);
    md_psg_env_t *e = &psg_env[p];
    e->instr = (uint8_t)(ch_current_instr[ch] & 0xFF);
    e->active = 1;
    e->point = 0;
    e->vol_pos = 0;
    e->arp_pos = 0;
    e->nz_pos = 0;
    const md_instr_t *ki = md_psg_env_instr(e);
    e->level256 = md_env_start_level(ki);
    // Une commande de mode de bruit encore active passe devant l'instrument.
    if (ki && ki->psg_noise_len > 0 && p == 3 && !md_noise_forced(ch)) {
      e->noise_mode = (uint8_t)(ki->psg_noise_mac[0] & 0x07);
      md_chip_psg_set_noise(e->noise_mode);
    }
    md_write_volume(ch);
    return;
  }
  md_chip_ym_key(hw, true);
}

// ── Voies DESACTIVEES ──────────────────────────────────────────────────────
// A ne pas confondre avec le silence (md_replayer_mute_channel), qui se
// contente de mettre le volume a zero : la voie continue alors d'etre jouee et
// calculee, donc elle coute autant de processeur qu'avant.
//
// Une voie desactivee, elle, ne recoit plus aucune note. Elle n'est donc plus
// active dans le YM2612, et ymfm cesse completement de la calculer — c'est la
// que se trouve l'economie : une voie FM, ce sont quatre operateurs sur les
// vingt-quatre, soit un sixieme du gros du travail.
//
// Les donnees du morceau ne sont JAMAIS touchees : on refuse de les jouer, on
// ne les efface pas. Le meme fichier rouvert sur iPad sonne complet.
bool md_channel_disabled[MD_TOTAL_VOICES] = {false};

static void md_key_off(int ch) {
  if (ch < 0 || ch >= MD_TOTAL_VOICES)
    return;
  ch_table_running[ch] = false;
  ch_b0_reg[ch] &= ~0x20;
  uint8_t hw = ch_hw_voice[ch];
  if (md_hw_is_psg(hw)) {
    uint8_t p = md_hw_psg_index(hw);
    md_psg_env_t *e = &psg_env[p];
    if (!e->active)
      return;
    // L'enveloppe de LSDJ n'a pas de segment de relâchement : le note-off coupe
    // net, comme le ferait un driver qui écrit simplement l'atténuation
    // maximale. C'est la table de l'instrument qui sert à faire mieux.
    e->active = 0;
    e->level256 = 0;
    md_write_volume(ch);
    return;
  }
  md_chip_ym_key(hw, false);
}

// Fait avancer les enveloppes logicielles des voies PSG (appelé à chaque tick).
// Définies plus bas.
static void md_write_note_offset(int c, int semitones);
static void md_apply_mdcmd(int c, uint8_t cmd, uint8_t val,
                           uint8_t row_cmd, int slot);

// Canal du morceau qui pilote cette voie PSG (-1 si aucune).
static int md_psg_channel(int p) {
  for (int c = 0; c < MD_TOTAL_VOICES; c++)
    if (md_hw_is_psg(ch_hw_voice[c]) && md_hw_psg_index(ch_hw_voice[c]) == p)
      return c;
  return -1;
}

static void md_psg_env_tick(void) {
  for (int p = 0; p < MD_NUM_PSG_CHANNELS; p++) {
    md_psg_env_t *e = &psg_env[p];
    if (!e->active)
      continue;
    const md_instr_t *ins = md_psg_env_instr(e);
    if (!ins) {
      e->level256 = 15 * 256;
      continue;
    }

    // ── Macro d'arpège (DefleMask) ────────────────────────────────────────
    // Un demi-ton par tick. La colonne TSP d'une table s'AJOUTE par-dessus :
    // la macro donne le dessin de l'instrument, la table le retouche.
    if (ins->psg_arp_len > 0) {
      int c = md_psg_channel(p);
      if (c >= 0) {
        int v = ins->psg_arp_mac[e->arp_pos];
        // « Fixed Arpeggio » : la valeur est une note ABSOLUE, pas un écart.
        int off = ins->psg_arp_fixed ? (v + 1 - (int)ch_current_note[c]) : v;
        md_write_note_offset(c, off);   // le TSP est ajouté par la fonction
      }
      e->arp_pos = (uint8_t)md_macro_next(e->arp_pos, ins->psg_arp_len,
                                          ins->psg_arp_loop);
    }

    // ── Macro de volume (DefleMask) ───────────────────────────────────────
    // Un niveau 0-15 par tick, écrit tel quel. Quand elle existe, elle
    // REMPLACE l'enveloppe à trois points : les deux décriraient le même
    // niveau et se contrediraient.
    if (ins->psg_vol_len > 0) {
      uint8_t lvl = ins->psg_vol_mac[e->vol_pos];
      if (lvl > 15) lvl = 15;
      e->level256 = (int32_t)lvl * 256;
      e->vol_pos = (uint8_t)md_macro_next(e->vol_pos, ins->psg_vol_len,
                                          ins->psg_vol_loop);
      // Un niveau nul en fin de macro éteint la voie, comme pour l'enveloppe.
      if (e->level256 == 0 && ins->psg_vol_loop == MD_EMPTY &&
          e->vol_pos == ins->psg_vol_len - 1)
        e->active = 0;
      continue;
    }

    // ── Enveloppe : trois points de rupture reliés par deux rampes ─────────
    // La vitesse d'un point dit à quelle allure rejoindre l'amplitude du point
    // SUIVANT. Vitesse 0, ou absence de point suivant : on tient l'amplitude
    // courante.
    int cur = e->point;
    if (cur >= MD_ENV_POINTS)
      cur = MD_ENV_POINTS - 1;
    int32_t rate = md_env_rate(ins->env_speed[cur]);
    int next = cur + 1;
    bool has_next = (next < MD_ENV_POINTS) && (ins->env_amp[next] != MD_EMPTY);

    // Cible de la rampe : l'amplitude du point suivant, ou le SILENCE quand il
    // n'y a pas de point suivant. C'est ce qui donne son sens à la vitesse du
    // dernier point : 0 tient l'amplitude indéfiniment (le « infinite sustain »
    // du manuel), toute autre valeur fait retomber la note vers zéro, d'autant
    // plus lentement que le chiffre est grand.
    int32_t target = 0;
    if (has_next) {
      uint8_t a = ins->env_amp[next];
      target = (int32_t)(a > 15 ? 15 : a) * 256;
    }

    if (rate > 0) {
      if (e->level256 < target) {
        e->level256 += rate;
        if (e->level256 > target) e->level256 = target;
      } else if (e->level256 > target) {
        e->level256 -= rate;
        if (e->level256 < target) e->level256 = target;
      }
      if (e->level256 == target && has_next)
        e->point = (uint8_t)next;      // point atteint : on passe au suivant
    }

    // Amplitude nulle sans espoir de remonter = la note s'est éteinte d'elle-
    // même. C'est ce qui dispense d'écrire des note-off partout.
    if (e->level256 <= 0 && !(rate > 0 && target > 0)) {
      e->level256 = 0;
      e->active = 0;
    }

    // ── Macro de mode de bruit ────────────────────────────────────────────
    if (p == 3 && ins->psg_noise_len > 0) {
      int nlen = ins->psg_noise_len > MD_PSG_ENV_MAX ? MD_PSG_ENV_MAX
                                                     : ins->psg_noise_len;
      e->nz_pos = (uint8_t)md_macro_next(e->nz_pos, nlen, ins->psg_noise_loop);
      uint8_t m = (uint8_t)(ins->psg_noise_mac[e->nz_pos] & 0x07);
      // La macro continue d'avancer, mais ne pose rien tant qu'une commande
      // tient le mode : sinon elle le reprendrait à chaque tick.
      if (m != e->noise_mode && !md_noise_forced(md_psg_channel(p))) {
        e->noise_mode = m;
        md_chip_psg_set_noise(m);
      }
    }
  }
  for (int ch = MD_PSG_FIRST_CHANNEL; ch < MD_MAX_CHANNELS; ch++)
    md_write_volume(ch);
  if (md_hw_is_psg(ch_hw_voice[MD_TEST_CHANNEL]))
    md_write_volume(MD_TEST_CHANNEL);
}

// ── Glissandos ──────────────────────────────────────────────────────────────
// Inchangé par rapport au moteur d'origine : on glisse dans l'espace
// block/F-Num, en gérant les changements d'octave.
static uint16_t md_slide_frequency(uint16_t current_fnum_block,
                                   int16_t slide_amount) {
  uint16_t fnum = current_fnum_block & 0x03FF;
  uint16_t block = (current_fnum_block & 0x3C00) >> 10;

  int32_t new_fnum = (int32_t)fnum + slide_amount;

  while (new_fnum >= 0x2AE && block < MD_MAX_BLOCK) {
    block++;
    new_fnum -= (0x2AE - 0x156);
  }
  while (new_fnum <= 0x156 && block > 0) {
    block--;
    new_fnum += (0x2AE - 0x156);
  }
  if (new_fnum > 0x2AE)
    new_fnum = 0x2AE;
  if (new_fnum < 0x156)
    new_fnum = 0x156;

  return (uint16_t)((block << 10) | (new_fnum & 0x03FF));
}

// Choisit la voie utilisée par l'audition, du même type que le canal édité.
static void md_assign_test_voice(void) {
  // Curseur sur une voie PSG : il n'en existe qu'une de chaque (3 tons + 1
  // bruit). L'audition doit donc utiliser EXACTEMENT celle-là, quitte à la
  // voler au séquenceur — prendre une autre voie donnerait un timbre qui n'a
  // rien à voir avec ce que la colonne jouera.
  if (MD_IS_PSG_CHANNEL(audition_channel)) {
    ch_hw_voice[MD_TEST_CHANNEL] = (uint8_t)audition_channel;
    return;
  }
  // Curseur sur une voie FM : on prend la première voie FM libre.
  for (int hw = 0; hw < MD_NUM_FM_CHANNELS; hw++) {
    bool busy = false;
    for (int c = 0; c < MD_MAX_CHANNELS; c++) {
      if (ch_hw_voice[c] == hw && (ch_b0_reg[c] & 0x20)) {
        busy = true;
        break;
      }
    }
    if (!busy) {
      ch_hw_voice[MD_TEST_CHANNEL] = (uint8_t)hw;
      return;
    }
  }
  // Toutes occupées : on vole la dernière voie FM.
  ch_hw_voice[MD_TEST_CHANNEL] = MD_NUM_FM_CHANNELS - 1;
}

// Applique le panning courant du canal (registre L/R du YM2612 ou panning
// logiciel du PSG).
static void md_apply_panning(int ch) {
  if (ch < 0 || ch >= MD_TOTAL_VOICES)
    return;
  uint8_t hw = ch_hw_voice[ch];
  const md_instr_t *ins = &ch_instr_shadow[ch];
  if (md_hw_is_psg(hw)) {
    md_chip_psg_set_pan(md_hw_psg_index(hw), ch_panning[ch]);
    return;
  }
  uint8_t lr = 0xC0;
  if (ch_panning[ch] == 1)
    lr = 0x80;
  else if (ch_panning[ch] == 2)
    lr = 0x40;
  md_chip_ym_write(md_hw_fm_part(hw), (uint8_t)(0xB4 + md_hw_fm_index(hw)),
                   (uint8_t)(lr | ((ins->ams & 3) << 4) | (ins->pms & 7)));
}

// Écrit sur la puce l'instrument actuellement en shadow sur ce canal.
static void md_apply_instrument(uint8_t ch) {
  if (ch >= MD_TOTAL_VOICES)
    return;
  const md_instr_t *ins = &ch_instr_shadow[ch];
  ch_fmpar[ch].feedbackConn =
      (uint8_t)(((ins->algorithm >= 4) ? 1 : 0) | ((ins->feedback & 7) << 1));

  uint8_t hw = ch_hw_voice[ch];

  if (md_hw_is_psg(hw)) {
    uint8_t p = md_hw_psg_index(hw);
    md_psg_env_t *e = &psg_env[p];
    // La voie PSG ne lit AUCUN paramètre FM : son enveloppe est la table de
    // volumes de l'instrument, déroulée par md_psg_env_tick().
    e->instr = (uint8_t)(ch_current_instr[ch] & 0xFF);
    if (!md_noise_forced(ch)) {
      e->noise_mode = (uint8_t)(ins->psg_noise & 0x07);
      if (p == 3)
        md_chip_psg_set_noise(e->noise_mode);
    }
    md_apply_panning(ch);
    md_write_volume(ch);
    return;
  }

  uint8_t part = md_hw_fm_part(hw);
  uint8_t idx = md_hw_fm_index(hw);

  for (int op = 0; op < 4; op++) {
    const md_op_params_t *o = &ins->op[op];
    uint8_t ro = (uint8_t)(md_op_reg_offset[op] + idx);
    md_chip_ym_write(part, (uint8_t)(0x30 + ro),
                     (uint8_t)(((o->detune & 7) << 4) | (o->multiple & 0x0F)));
    md_chip_ym_write(part, (uint8_t)(0x50 + ro),
                     (uint8_t)(((o->key_scale & 3) << 6) | (o->attack & 0x1F)));
    md_chip_ym_write(part, (uint8_t)(0x60 + ro),
                     (uint8_t)(((o->am_enable & 1) << 7) | (o->decay & 0x1F)));
    md_chip_ym_write(part, (uint8_t)(0x70 + ro),
                     (uint8_t)(o->sustain_rate & 0x1F));
    md_chip_ym_write(part, (uint8_t)(0x80 + ro),
                     (uint8_t)(((o->sustain_level & 0x0F) << 4) |
                               (o->release & 0x0F)));
    md_chip_ym_write(part, (uint8_t)(0x90 + ro), (uint8_t)(o->ssg_eg & 0x0F));
  }

  md_chip_ym_write(part, (uint8_t)(0xB0 + idx),
                   (uint8_t)(((ins->feedback & 7) << 3) | (ins->algorithm & 7)));

  // Panning + sensibilité au LFO
  md_apply_panning(ch);

  // Le LFO du YM2612 est GLOBAL (une seule vitesse pour la puce entière). Un
  // instrument peut donc l'allumer, mais on ne le laisse pas l'éteindre : sinon
  // la note suivante jouée par un instrument sans LFO couperait le vibrato de
  // tous les autres canaux. Les instruments qui ne s'en servent pas ont de
  // toute façon AMS = PMS = 0, donc le LFO ne les affecte pas.
  // Le LFO repart éteint à chaque md_chip_reset (nouveau morceau, play, stop).
  if (ins->lfo_enable)
    md_chip_ym_write(0, 0x22, (uint8_t)(0x08 | (ins->lfo_freq & 0x07)));

  md_write_volume(ch);
}

// Charge un instrument sur un canal (copie dans le shadow puis écriture puce).
static void md_set_instrument(uint8_t ch, const md_instr_t *ins) {
  if (ch >= MD_TOTAL_VOICES || !ins)
    return;
  ch_instr_shadow[ch] = *ins;
  ch_panning[ch] = ins->panning;
  md_apply_instrument(ch);
}

// L'instrument chargé sur ce canal joue-t-il un échantillon ?
//
// On regarde l'INSTRUMENT, pas le numéro de canal. Le DAC est une ressource
// unique de la puce, et la voie d'audition — celle qui fait sonner une note
// qu'on vient d'écrire — n'est pas la voie du morceau. En la laissant hors du
// test, une note posée sur la colonne PCM s'auditionnait en synthèse FM, avec
// le son du patch d'usine : absurde, puisque l'instrument est un échantillon.
//
// Le placement d'un instrument PCM sur la bonne colonne reste vérifié à part,
// par md_instr_fits_channel().
static bool md_channel_is_pcm(int c) {
  if (!current_module || c < 0 || c >= MD_TOTAL_VOICES) return false;
  int ins = ch_current_instr[c];
  if (ins < 1 || ins > MD_MAX_INSTRUMENTS) return false;
  int k = current_module->instruments[ins - 1].kind;
  if (k == MD_INSTR_KIND_PCM) return true;
  // La 6ᵉ voie ne fait QUE de l'échantillon. Un instrument qui ne dit pas sa
  // nature — c'est le cas de tout instrument neuf, et de ceux des anciens
  // fichiers — y est donc du PCM et non un patch FM. Sans ça, poser une note
  // sur cette colonne pouvait encore déclencher le son d'usine du YM2612.
  if (c == MD_PCM_CHANNEL && k == MD_INSTR_KIND_ANY) return true;
  return false;
}

static void md_play_note(uint8_t ch, uint8_t note) {
  if (ch >= MD_TOTAL_VOICES || note == 0 || note > MD_MAX_NOTE)
    return;
  if (md_channel_disabled[ch])
    return;   // desactivee : aucune note, donc aucun calcul

  ch_current_note[ch] = note;

  // ── Voie PCM ──────────────────────────────────────────────────────────
  // La hauteur d'un échantillon, c'est sa VITESSE DE LECTURE : il n'y a pas
  // d'oscillateur à accorder. On part de la cadence du pilote et on l'étire
  // selon l'écart avec la note de référence de l'échantillon.
  if (md_channel_is_pcm(ch)) {
    const md_instr_t *ins = &current_module->instruments[ch_current_instr[ch] - 1];
    int si = ins->pcm_sample;
    if (si < 0 || si >= MD_MAX_SAMPLES) {
      md_chip_pcm_stop();               // aucun échantillon : rien à jouer
    } else {
      const md_sample_t *sm = &current_module->samples[si];
      // Une voie coupée doit se taire, y compris ici : le DAC ne passe pas par
      // le chemin de volume des canaux, donc couper la voie PCM la laissait
      // jouer. Ça fausse aussi toute mesure faite voie par voie.
      if (md_channel_muted[ch]) {
        md_chip_pcm_stop();
      } else if (sm->length == 0) {
        // Instrument PCM sans échantillon : la voie se TAIT. On coupe le DAC
        // pour ne pas laisser FM6 tenir la dernière valeur écrite.
        md_chip_pcm_stop();
      } else {
        double ratio = pow(2.0, ((double)note - (double)sm->base_note) / 12.0);
        // Deux niveaux se multiplient : la colonne VEL, ligne par ligne, et
        // le volume de l'instrument, une fois pour toutes.
        int loud = 63 - (int)(ch_fmpar[ch].volC & 0x3F);
        if (loud < 0) loud = 0;
        // 7F = unité, au-delà on POUSSE l'échantillon (écrêtage dans la voie).
        int iv = ins->pcm_volume;
        int vol = (loud * 255 / 63) * iv / 127;
        md_chip_pcm_enable(true);
        md_chip_pcm_play(current_module->pcm + sm->offset, sm->length, sm->loop,
                         (double)MD_PCM_RATE * ratio, vol);
      }
    }
    return;                         // aucun key-on FM sur cette voie
  }

  // Coupe la note précédente pour relancer proprement l'enveloppe.
  md_key_off(ch);

  // La transposition de la table entre dans la hauteur de RÉFÉRENCE, pas
  // seulement dans l'écriture du moment : c'est elle que le moteur restaure au
  // début de chaque ligne, et sans ça le TSP disparaissait dès qu'une commande
  // était posée sur la même ligne.
  {
    int tn = (int)note + (int)ch_table_tsp[ch];
    if (tn < 1) tn = 1;
    if (tn > MD_MAX_NOTE) tn = MD_MAX_NOTE;
    note = (uint8_t)tn;
  }

  uint8_t octave = (uint8_t)((note - 1) / 12);
  uint8_t pitch = (uint8_t)((note - 1) % 12);
  uint16_t fnum = md_note_fnums[pitch];

  if (current_module && ch_current_instr[ch] > 0 &&
      ch_current_instr[ch] <= MD_MAX_INSTRUMENTS) {
    int8_t ftune =
        current_module->instruments[ch_current_instr[ch] - 1].fine_tune;
    fnum = (uint16_t)((int16_t)fnum + ftune);
  }

  uint8_t block = (octave <= MD_MAX_BLOCK) ? octave : MD_MAX_BLOCK;
  ch_base_fnum[ch] = (uint16_t)((block << 10) | (fnum & 0x03FF));

  md_write_freq(ch, ch_base_fnum[ch]);
  md_key_on(ch);
}

// ── Position de lecture, INDÉPENDANTE PAR CANAL (modèle LSDJ) ─────────────
// Chaque canal descend sa propre colonne du SONG, entre dans son CHAIN, puis
// dans sa PHRASE. Deux canaux ne sont donc pas forcément sur la même ligne de
// song : une basse peut boucler sur un seul chain pendant qu'une lead en
// enchaîne dix.
static int ch_song_row[MD_MAX_CHANNELS];    // ligne du SONG
static int ch_chain[MD_MAX_CHANNELS];       // chain en cours, -1 = canal muet
static int ch_chain_row[MD_MAX_CHANNELS];   // ligne dans le chain
static int ch_phrase[MD_MAX_CHANNELS];      // phrase en cours, -1 = aucune
static int ch_phrase_row[MD_MAX_CHANNELS];  // ligne dans la phrase
static int8_t ch_row_transpose[MD_MAX_CHANNELS];
static bool ch_running[MD_MAX_CHANNELS];    // ce canal a-t-il encore à jouer ?

// Ligne du SONG où démarre la lecture (F5 = 0, F8 = ligne du curseur).
static int song_start_row = 0;
// Ligne de départ dans l'écran CHAIN : « START on the CHAIN screen begins at
// the row the cursor is on (not row 0) » (SamplePanel.swift, toggleTransport).
static int scope_start_row = 0;
// Haut du bloc contigu sur lequel chaque canal boucle.
static int ch_block_top[MD_MAX_CHANNELS] = {0};

// Portée de la lecture : chanson entière, un chain, ou une seule phrase.
// Voir md_replayer_set_play_scope() dans l'en-tête.
static md_play_scope_t play_scope = MD_SCOPE_SONG;
static int scope_channel = 0;
static int scope_id = 0;

// ── Mode LIVE ───────────────────────────────────────────────────────────────
// En mode SONG, la lecture lance les 10 colonnes ensemble. En mode LIVE, elles
// restent muettes jusqu'à ce qu'on les arme une par une ; l'armement prend effet
// à la ligne suivante, ce qui garde tout le monde en phase.
static bool live_mode = false;
static int ch_pending_row[MD_MAX_CHANNELS];   // -1 = rien en attente

uint8_t md_current_row = 0;   // conservé : l'UI hérité l'utilise encore
bool md_loop_pattern = false;

static inline uint16_t read16le(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t read32le(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static inline void write16le(uint8_t *p, uint16_t v) {
  p[0] = v & 0xFF;
  p[1] = (v >> 8) & 0xFF;
}

static inline void write32le(uint8_t *p, uint32_t v) {
  p[0] = v & 0xFF;
  p[1] = (v >> 8) & 0xFF;
  p[2] = (v >> 16) & 0xFF;
  p[3] = (v >> 24) & 0xFF;
}

// ============================================================================
// Format de fichier .mdm (GeneTracker Module) — version 2
//
// Format maison, non compressé. La version 2 abandonne les patterns au profit
// de la structure LSDJ : SONG (256 lignes × 10 canaux de numéros de chain),
// CHAIN (16 lignes : phrase + transposition), PHRASE (16 lignes : note,
// instrument, vélocité, commande, valeur).
// Les champs sont écrits un par un (pas de memcpy de struct) pour rester
// indépendant de l'alignement du compilateur.
//
//   0   char   magic[8]      "MDMTRACK"
//   8   u16    version       (2)
//  10   u8     num_channels
//  11   u8     initial_tempo
//  12   u8     initial_speed
//  13   u8     common_flag
//  14   u16    macro_speedup
//  16   i16    bpm_tempo_finetune
//  18   u8     bpm_rows_per_beat
//  19   u8     réservé
//  20   char   songname[43]
//  63   char   composer[43]
// 106   song      : 10 canaux × 256 lignes × 1 octet   (numéro de chain)
// 2666  chains    : 128 × 16 lignes × 2 octets         (phrase, transpose)
// 6762  phrases   : 255 × 16 lignes × 5 octets         (note, ins, vel, cmd, val)
// 27162 instruments : 255 × 64 octets
// 43482 noms d'instruments : 255 × 43 octets
// ============================================================================

// v7 : seconde colonne de commande (MD CMD) dans les phrases et les tables.
// v6 : l'enveloppe PSG devient un ADSR à six paramètres, comme l'écran ENV de
//      LSDJ, à la place de la table de volumes.
// v5 : tables d'instrument façon LSDJ (32 tables × 16 lignes) ; l'arpégiateur
//      cède sa place au numéro de table dans l'instrument.
// v4 : macros d'arpège et de mode de bruit (256 octets/instrument).
// v3 : table de volumes PSG (128 octets). v2 : enveloppe dérivée d'OP4 (64).
// v12 : volume propre à l'instrument PCM. Les fichiers antérieurs le prennent
//       au maximum, donc rien ne change pour eux.
// v11 : la vélocité va de 0 à 7F, l'échelle de DefleMask, au lieu de 0 à 3F.
//       Le moteur écrêtait à 3F, si bien qu'un volume DefleMask supérieur à la
//       moitié sonnait au maximum et que tout le reste sonnait deux fois trop
//       bas. Les fichiers antérieurs sont remis à l'échelle en les chargeant.
// v10 : banque d'échantillons pour la voie PCM (le DAC du YM2612). Le fichier
//       n'est plus de taille fixe : une section variable est ajoutée à la fin.
// v9 : macros PSG façon DefleMask (volume et arpège, un pas par tick) ;
//      l'enregistrement d'instrument passe de 256 à 512 octets.
// v8 : la colonne MD CMD porte tout le jeu d'effets DefleMask (44 commandes)
//      et le fichier range désormais le CODE de l'effet, pas son rang dans la
//      table — réordonner la table ne change plus le sens d'un morceau.
#define MD_FILE_VERSION 12
// v9 : l'enregistrement passe à 512 octets pour loger les macros PSG.
#define MD_INSTR_RECORD_SIZE 512
#define MD_TABLE_ROW_BYTES 8
#define MD_TABLE_ROW_BYTES_V6 6
#define MD_TABLES_BYTES (MD_MAX_TABLES * MD_TABLE_ROWS * MD_TABLE_ROW_BYTES)

// Taille d'enregistrement d'une version donnée, 0 si la version est inconnue.
// ── La colonne MD CMD dans le fichier ───────────────────────────────────────
// On y range le CODE d'effet DefleMask plutôt que le rang dans md_mdcmds :
// ajouter ou déplacer une commande ne doit pas changer ce que joue un morceau
// déjà enregistré.
static uint8_t md_mdcmd_to_file(uint8_t idx) {
  return (idx == MD_EMPTY) ? MD_EMPTY : md_mdcmd_code((int)idx);
}

static uint8_t md_mdcmd_from_file(uint8_t stored, uint16_t version) {
  if (stored == MD_EMPTY)
    return MD_EMPTY;
  if (version < 8) {
    // La v7 rangeait le rang dans une table de vingt-cinq lettres, dans cet
    // ordre : L F T U V W M A B C D E N G O P Q H K X Y Z R I S.
    static const uint8_t v7[] = {
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x19, 0x1A, 0x1B, 0x1C,
        0x1D, 0x20, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
        0x39, 0x3A, 0x3B};
    if (stored >= (uint8_t)(sizeof(v7) / sizeof(v7[0])))
      return MD_EMPTY;
    stored = v7[stored];
  }
  int i = md_mdcmd_index_of_code(stored);
  return (i < 0) ? MD_EMPTY : (uint8_t)i;
}

static uint32_t md_instr_record_size(uint16_t version) {
  // Depuis la v9 l'enregistrement fait 512 octets et n'a plus changé de
  // taille : les ajouts se logent dans la place restante. On répond donc pour
  // TOUTE version récente — sans ça, chaque changement de version rendait les
  // fichiers illisibles, ce qui est exactement ce qui venait d'arriver.
  if (version >= 9) return 512;
  switch (version) {
  case 2: return 64;
  case 3: return 128;
  case 4: return 256;
  case 5: return 256;
  case 6: return 256;
  case 7: return 256;
  case 8: return 256;
  case 9: return 512;
  case 10: return 512;
  default: return 0;
  }
}
#define MD_SONG_OFFSET 106
#define MD_SONG_BYTES (MD_MAX_CHANNELS * MD_SONG_ROWS)
#define MD_CHAIN_OFFSET (MD_SONG_OFFSET + MD_SONG_BYTES)
#define MD_CHAIN_BYTES (MD_MAX_CHAINS * MD_ROWS_PER_CHAIN * 2)
#define MD_PHRASE_OFFSET (MD_CHAIN_OFFSET + MD_CHAIN_BYTES)
#define MD_PHRASE_ROW_BYTES 7
#define MD_PHRASE_ROW_BYTES_V6 5
#define MD_PHRASE_BYTES (MD_MAX_PHRASES * MD_ROWS_PER_PHRASE * MD_PHRASE_ROW_BYTES)
#define MD_INSTR_TABLE_OFFSET (MD_PHRASE_OFFSET + MD_PHRASE_BYTES)
#define MD_INSTR_TABLE_SIZE (MD_MAX_INSTRUMENTS * MD_INSTR_RECORD_SIZE)
#define MD_NAMES_TABLE_OFFSET (MD_INSTR_TABLE_OFFSET + MD_INSTR_TABLE_SIZE)
#define MD_NAMES_TABLE_SIZE (MD_MAX_INSTRUMENTS * 43)
#define MD_TABLES_OFFSET (MD_NAMES_TABLE_OFFSET + MD_NAMES_TABLE_SIZE)
#define MD_FILE_SIZE (MD_TABLES_OFFSET + MD_TABLES_BYTES)
// ── Section des échantillons (v10) ──────────────────────────────────────────
// Ajoutée APRÈS le bloc de taille fixe : un en-tête par emplacement, puis les
// données brutes bout à bout. C'est ce qui rend le fichier de taille variable.
#define MD_SAMPLE_HDR_BYTES 40
#define MD_SAMPLES_HDR_TOTAL (MD_MAX_SAMPLES * MD_SAMPLE_HDR_BYTES)

static void md_write_instrument(uint8_t *p, const md_instr_t *ins) {
  memset(p, 0, MD_INSTR_RECORD_SIZE);
  int o = 0;
  for (int i = 0; i < 4; i++) {
    const md_op_params_t *op = &ins->op[i];
    p[o++] = op->detune;
    p[o++] = op->multiple;
    p[o++] = op->total_level;
    p[o++] = op->key_scale;
    p[o++] = op->attack;
    p[o++] = op->decay;
    p[o++] = op->sustain_rate;
    p[o++] = op->sustain_level;
    p[o++] = op->release;
    p[o++] = op->am_enable;
    p[o++] = op->ssg_eg;
  }
  p[o++] = ins->algorithm;
  p[o++] = ins->feedback;
  p[o++] = ins->ams;
  p[o++] = ins->pms;
  p[o++] = ins->lfo_enable;
  p[o++] = ins->lfo_freq;
  p[o++] = ins->panning;
  p[o++] = (uint8_t)ins->fine_tune;
  p[o++] = ins->psg_noise;
  for (int i = 0; i < MD_ENV_POINTS; i++) {
    p[o++] = ins->env_amp[i];
    p[o++] = ins->env_speed[i];
  }
  o += 29; // ancien emplacement de la table de volumes
  p[o++] = ins->table;
  o += 33; // ancien emplacement de la macro d'arpège, laissé à zéro
  p[o++] = ins->psg_noise_len;
  p[o++] = ins->psg_noise_loop;
  for (int i = 0; i < MD_PSG_ENV_MAX; i++)
    p[o++] = ins->psg_noise_mac[i];
  // ── Macros PSG (v9) ────────────────────────────────────────────────────
  p[o++] = ins->psg_vol_len;
  p[o++] = ins->psg_vol_loop;
  for (int i = 0; i < MD_PSG_MACRO_MAX; i++)
    p[o++] = ins->psg_vol_mac[i];
  p[o++] = ins->psg_arp_len;
  p[o++] = ins->psg_arp_loop;
  p[o++] = ins->psg_arp_fixed;
  for (int i = 0; i < MD_PSG_MACRO_MAX; i++)
    p[o++] = (uint8_t)ins->psg_arp_mac[i];
  p[o++] = ins->kind;
  p[o++] = ins->pcm_sample;
  p[o++] = ins->pcm_volume;
}

static void md_read_instrument(const uint8_t *p, md_instr_t *ins, uint16_t version) {
  memset(ins, 0, sizeof(*ins));
  int o = 0;
  for (int i = 0; i < 4; i++) {
    md_op_params_t *op = &ins->op[i];
    op->detune = p[o++];
    op->multiple = p[o++];
    op->total_level = p[o++];
    op->key_scale = p[o++];
    op->attack = p[o++];
    op->decay = p[o++];
    op->sustain_rate = p[o++];
    op->sustain_level = p[o++];
    op->release = p[o++];
    op->am_enable = p[o++];
    op->ssg_eg = p[o++];
  }
  ins->algorithm = p[o++];
  ins->feedback = p[o++];
  ins->ams = p[o++];
  ins->pms = p[o++];
  ins->lfo_enable = p[o++];
  ins->lfo_freq = p[o++];
  ins->panning = p[o++];
  ins->fine_tune = (int8_t)p[o++];
  ins->psg_noise = p[o++];

  // Ce qui n'existe pas dans une ancienne version reste neutre.
  ins->table = MD_EMPTY;
  ins->psg_noise_loop = MD_EMPTY;

  // v2 : l'enveloppe PSG était dérivée d'OP4. On ne tente pas de la
  // reconstituer — on repart d'une table neutre (volume tenu).
  if (version < 3)
    return;

  // v3-v5 rangeaient ici une table de volumes ; v6 y range l'ADSR. On ne relit
  // les six paramètres que si le fichier est bien en v6 — sinon on laisse
  // l'enveloppe par défaut plutôt que d'interpréter d'anciens octets à tort.
  if (version >= 6) {
    for (int i = 0; i < MD_ENV_POINTS; i++) {
      ins->env_amp[i] = p[o + i * 2];
      ins->env_speed[i] = p[o + i * 2 + 1];
    }
  }
  o += 35;

  if (version < 4)
    return;

  // v4 rangeait ici la macro d'arpège ; v5 y range le numéro de table. Une v4
  // relue donne donc une longueur d'arpège comme numéro de table — on ne la
  // prend que si le fichier est bien en v5.
  if (version >= 5)
    ins->table = p[o];
  o += 34;
  ins->psg_noise_len = p[o++];
  ins->psg_noise_loop = p[o++];
  for (int i = 0; i < MD_PSG_ENV_MAX; i++)
    ins->psg_noise_mac[i] = p[o++];

  // Les macros PSG n'existent qu'à partir de la v9 ; avant, l'enregistrement
  // s'arrêtait là et les octets suivants n'existent pas.
  if (version < 9)
    return;
  ins->psg_vol_len = p[o++];
  ins->psg_vol_loop = p[o++];
  for (int i = 0; i < MD_PSG_MACRO_MAX; i++)
    ins->psg_vol_mac[i] = p[o++];
  ins->psg_arp_len = p[o++];
  ins->psg_arp_loop = p[o++];
  ins->psg_arp_fixed = p[o++];
  for (int i = 0; i < MD_PSG_MACRO_MAX; i++)
    ins->psg_arp_mac[i] = (int8_t)p[o++];
  // Écrit à zéro par les toutes premières v9 : MD_INSTR_KIND_ANY, l'ancien
  // comportement.
  ins->kind = p[o++];
  // L'échantillon choisi n'existe qu'à partir de la v10 ; avant, l'octet est à
  // zéro, ce qui désigne le premier emplacement — sans conséquence puisqu'il
  // n'y avait pas d'instrument PCM.
  ins->pcm_sample = (version >= 10) ? p[o++] : 0;
  // Avant la v12 il n'y avait pas de volume PCM : plein, comme avant.
  ins->pcm_volume = (version >= 12) ? p[o++] : 127;
}

// Sérialise le module courant au format .mdm v2.
// L'appelant doit free() le tampon renvoyé.
uint8_t *md_replayer_save_mem(uint32_t *out_size) {
  if (!current_module || !out_size)
    return NULL;

  md_module_t *mod = current_module;
  // Taille totale = bloc fixe + en-têtes d'échantillons + données PCM.
  const uint32_t pcm_bytes = mod->pcm_used;
  const uint32_t total = MD_FILE_SIZE + 4 + MD_SAMPLES_HDR_TOTAL + pcm_bytes;
  uint8_t *buf = (uint8_t *)calloc(1, total);
  if (!buf)
    return NULL;

  memcpy(buf, "MDMTRACK", 8);
  write16le(buf + 8, MD_FILE_VERSION);
  buf[10] = mod->num_channels;
  buf[11] = mod->initial_tempo;
  buf[12] = mod->initial_speed;
  buf[13] = mod->common_flag;
  write16le(buf + 14, mod->macro_speedup);
  write16le(buf + 16, (uint16_t)mod->bpm_tempo_finetune);
  buf[18] = mod->bpm_rows_per_beat;
  memcpy(buf + 20, mod->songname, 43);
  memcpy(buf + 63, mod->composer, 43);

  for (int c = 0; c < MD_MAX_CHANNELS; c++)
    memcpy(buf + MD_SONG_OFFSET + c * MD_SONG_ROWS, mod->song[c], MD_SONG_ROWS);

  for (int i = 0; i < MD_MAX_CHAINS; i++) {
    for (int r = 0; r < MD_ROWS_PER_CHAIN; r++) {
      uint8_t *d = buf + MD_CHAIN_OFFSET + (i * MD_ROWS_PER_CHAIN + r) * 2;
      d[0] = mod->chains[i].rows[r].phrase;
      d[1] = (uint8_t)mod->chains[i].rows[r].transpose;
    }
  }

  for (int i = 0; i < MD_MAX_PHRASES; i++) {
    for (int r = 0; r < MD_ROWS_PER_PHRASE; r++) {
      uint8_t *d = buf + MD_PHRASE_OFFSET +
                   (i * MD_ROWS_PER_PHRASE + r) * MD_PHRASE_ROW_BYTES;
      const md_phrase_row_t *pr = &mod->phrases[i].rows[r];
      d[0] = pr->note;
      d[1] = pr->instr;
      d[2] = pr->vel;
      d[3] = pr->cmd;
      d[4] = pr->cmdval;
      d[5] = md_mdcmd_to_file(pr->mdcmd);
      d[6] = pr->mdval;
    }
  }

  for (int i = 0; i < MD_MAX_INSTRUMENTS; i++) {
    md_write_instrument(buf + MD_INSTR_TABLE_OFFSET + i * MD_INSTR_RECORD_SIZE,
                        &mod->instruments[i]);
    memcpy(buf + MD_NAMES_TABLE_OFFSET + i * 43, mod->instr_names[i], 43);
  }

  // Les tables. Elles manquaient purement et simplement du fichier depuis
  // leur introduction : tout ce qu'on y écrivait était perdu à l'ouverture
  // suivante.
  for (int i = 0; i < MD_MAX_TABLES; i++) {
    for (int r = 0; r < MD_TABLE_ROWS; r++) {
      uint8_t *d = buf + MD_TABLES_OFFSET +
                   (i * MD_TABLE_ROWS + r) * MD_TABLE_ROW_BYTES;
      const md_table_row_t *tr = &mod->tables[i].rows[r];
      d[0] = tr->vol;
      d[1] = (uint8_t)tr->transpose;
      d[2] = tr->cmd1;  d[3] = tr->val1;
      d[4] = tr->cmd2;  d[5] = tr->val2;
      d[6] = md_mdcmd_to_file(tr->mdcmd); d[7] = tr->mdval;
    }
  }

  // ── Banque d'échantillons ───────────────────────────────────────────────
  {
    uint8_t *p = buf + MD_FILE_SIZE;
    write32le(p, pcm_bytes); p += 4;
    for (int i = 0; i < MD_MAX_SAMPLES; i++) {
      const md_sample_t *sm = &mod->samples[i];
      memcpy(p, sm->name, 24);
      write32le(p + 24, sm->offset);
      write32le(p + 28, sm->length);
      write32le(p + 32, (uint32_t)sm->loop);
      p[36] = sm->base_note;
      p += MD_SAMPLE_HDR_BYTES;
    }
    if (pcm_bytes) memcpy(p, mod->pcm, pcm_bytes);
  }

  *out_size = total;
  return buf;
}

// Internal playback state
static int replay_ticks_left = 0;
static int replay_speed = 6;
static int replay_tempo = 50;
static int samples_per_tick = 0;
static int samples_until_next_tick = 0;

// ── Timing AT2 : fréquence de base de l'IRQ + finetune ──────────────────────
// AT2 ne joue pas à `tempo` Hz : il choisit une fréquence d'IRQ de base (à
// partir de 250, multiple de tempo*macro_speedup, plafonnée à 1000), puis
// AJOUTE `bpm_tempo_finetune` à cette fréquence. Le tempo réel est donc mis à
// l'échelle par (base+finetune)/base. On ignorait ce finetune → lecture trop
// lente et BPM faux. (cf. calc_bpm_speed / set_timer dans AT2.)
static int at2_base_irq_freq(int tempo, int macro) {
  if (tempo <= 0) tempo = 50;
  if (macro <= 0) macro = 1;
  int div = tempo * macro;
  if (div <= 0) div = tempo > 0 ? tempo : 50;
  int irq = 250;
  while (irq % div != 0) irq++;
  if (irq > 1000) irq = 1000;
  return irq;
}

// Échelle de tempo due au finetune : (base+finetune)/base. 1.0 si pas de module.
static double md_finetune_scale(void) {
  if (!current_module) return 1.0;
  int tempo = current_module->initial_tempo > 0 ? current_module->initial_tempo : 50;
  int macro = current_module->macro_speedup > 0 ? current_module->macro_speedup : 1;
  int base = at2_base_irq_freq(tempo, macro);
  if (base <= 0) return 1.0;
  double s = ((double)base + (double)current_module->bpm_tempo_finetune) / (double)base;
  if (s < 0.05) s = 0.05;   // garde-fou
  return s;
}

void md_replayer_init(int sample_rate) {
  replay_sample_rate = sample_rate;
  md_chip_reset(sample_rate);

  // Chaque canal logique est câblé sur sa voie matérielle :
  //   0-5 → FM 1-6, 6-8 → PSG ton 1-3, 9 → PSG bruit.
  for (int c = 0; c < MD_MAX_CHANNELS; c++)
    ch_hw_voice[c] = (uint8_t)c;
  ch_hw_voice[MD_TEST_CHANNEL] = MD_NUM_FM_CHANNELS - 1;
  memset(psg_env, 0, sizeof(psg_env));
  memset(ch_table_forced, 0, sizeof(ch_table_forced));

  // Le volume global est modifiable par la commande M. Sans cette remise à
  // zéro il survivait à un changement de morceau : une seule commande M
  // suffisait à laisser TOUTES les lectures suivantes assourdies, y compris
  // après chargement d'un autre fichier.
  replay_global_vol = 63;
  memset(ch_fmpar, 0, sizeof(ch_fmpar));

  // Set a default tick rate so macros work before pattern playback starts
  // (e.g. for the instrument editor piano). Default AT2 interrupt freq = 50 Hz.
  samples_per_tick = sample_rate / 50;
}

void md_replayer_shut() {
  md_replayer_stop();
  if (current_module) {
    free(current_module);
    current_module = NULL;
  }
}

bool md_replayer_load_mem(const uint8_t *data, uint32_t size) {
  if (!data || memcmp(data, "MDMTRACK", 8) != 0)
    return false;

  // Toutes les versions restent lisibles : seule la table des instruments a
  // changé de taille d'enregistrement au fil des ajouts. Tout ce qui précède
  // est identique, donc seuls les deux derniers blocs se calculent autrement.
  uint16_t version = read16le(data + 8);
  const uint32_t rec = md_instr_record_size(version);
  if (rec == 0)
    return false;
  // Les lignes de phrase et de table se sont allongées en v7.
  const uint32_t prow = (version >= 7) ? MD_PHRASE_ROW_BYTES
                                       : MD_PHRASE_ROW_BYTES_V6;

  const uint32_t instr_off = MD_PHRASE_OFFSET +
                             MD_MAX_PHRASES * MD_ROWS_PER_PHRASE * prow;
  const uint32_t names_off = instr_off + MD_MAX_INSTRUMENTS * rec;
  if (size < names_off + MD_NAMES_TABLE_SIZE)
    return false;

  md_lock(&replayer_lock);

  md_replayer_shut();

  md_module_t *mod = (md_module_t *)calloc(1, sizeof(md_module_t));
  if (!mod) {
    md_unlock(&replayer_lock);
    return false;
  }

  mod->num_channels = data[10];
  if (mod->num_channels == 0 || mod->num_channels > MD_MAX_CHANNELS)
    mod->num_channels = MD_MAX_CHANNELS;
  mod->initial_tempo = data[11];
  mod->initial_speed = data[12];
  mod->common_flag = data[13];
  mod->macro_speedup = read16le(data + 14);
  if (mod->macro_speedup == 0)
    mod->macro_speedup = 1;
  mod->bpm_tempo_finetune = (int16_t)read16le(data + 16);
  mod->bpm_rows_per_beat = data[18];
  if (mod->bpm_rows_per_beat == 0)
    mod->bpm_rows_per_beat = 4;

  memcpy(mod->songname, data + 20, 43);
  mod->songname[42] = '\0';
  memcpy(mod->composer, data + 63, 43);
  mod->composer[42] = '\0';

  for (int c = 0; c < MD_MAX_CHANNELS; c++)
    memcpy(mod->song[c], data + MD_SONG_OFFSET + c * MD_SONG_ROWS, MD_SONG_ROWS);

  for (int i = 0; i < MD_MAX_CHAINS; i++) {
    for (int r = 0; r < MD_ROWS_PER_CHAIN; r++) {
      const uint8_t *d = data + MD_CHAIN_OFFSET + (i * MD_ROWS_PER_CHAIN + r) * 2;
      mod->chains[i].rows[r].phrase = d[0];
      mod->chains[i].rows[r].transpose = (int8_t)d[1];
    }
  }

  for (int i = 0; i < MD_MAX_PHRASES; i++) {
    for (int r = 0; r < MD_ROWS_PER_PHRASE; r++) {
      const uint8_t *d = data + MD_PHRASE_OFFSET +
                         (i * MD_ROWS_PER_PHRASE + r) * prow;
      md_phrase_row_t *pr = &mod->phrases[i].rows[r];
      pr->note = d[0];
      pr->instr = d[1];
      // Avant la v11 la vélocité tenait sur 0-3F : on la remet sur 0-7F pour
      // qu'un morceau ancien garde exactement le même volume.
      pr->vel = (d[2] == MD_EMPTY || version >= 11)
                    ? d[2]
                    : (uint8_t)((d[2] > 63 ? 63 : d[2]) * 127 / 63);
      pr->cmd = d[3];
      pr->cmdval = d[4];
      // La colonne MD n'existe qu'à partir de la v7.
      pr->mdcmd = (prow >= 7) ? md_mdcmd_from_file(d[5], version) : MD_EMPTY;
      pr->mdval = (prow >= 7) ? d[6] : 0;
    }
  }

  // Les tables ne sont pas encore dans le fichier : on les crée vides plutôt
  // qu'à zéro, « VOL 0 » voulant dire silence et non « pas de consigne ».
  for (int i = 0; i < MD_MAX_TABLES; i++)
    for (int r = 0; r < MD_TABLE_ROWS; r++) {
      md_table_row_t *tr = &mod->tables[i].rows[r];
      tr->vol = 0;
      tr->cmd1 = MD_EMPTY;
      tr->cmd2 = MD_EMPTY;
      tr->mdcmd = MD_EMPTY;
    }

  // Les tables ne sont dans le fichier qu'à partir de la v7 ; avant, elles
  // restent celles qu'on vient d'initialiser à vide.
  const uint32_t tables_off = names_off + MD_NAMES_TABLE_SIZE;
  if (version >= 7 && size >= tables_off + MD_TABLES_BYTES) {
    for (int i = 0; i < MD_MAX_TABLES; i++) {
      for (int r = 0; r < MD_TABLE_ROWS; r++) {
        const uint8_t *d = data + tables_off +
                           (i * MD_TABLE_ROWS + r) * MD_TABLE_ROW_BYTES;
        md_table_row_t *tr = &mod->tables[i].rows[r];
        tr->vol = d[0];
        tr->transpose = (int8_t)d[1];
        tr->cmd1 = d[2];  tr->val1 = d[3];
        tr->cmd2 = d[4];  tr->val2 = d[5];
        tr->mdcmd = md_mdcmd_from_file(d[6], version); tr->mdval = d[7];
      }
    }
  }

  for (int i = 0; i < MD_MAX_INSTRUMENTS; i++) {
    md_read_instrument(data + instr_off + i * rec,
                       &mod->instruments[i], version);
    memcpy(mod->instr_names[i], data + names_off + i * 43, 43);
    mod->instr_names[i][42] = '\0';
  }

  // ── Banque d'échantillons (v10) ─────────────────────────────────────────
  // Absente des versions antérieures : le morceau se charge alors sans
  // échantillon, ce qui est le bon comportement.
  if (version >= 10) {
    const uint32_t base = tables_off + MD_TABLES_BYTES;
    if (size >= base + 4 + MD_SAMPLES_HDR_TOTAL) {
      const uint8_t *p = data + base;
      uint32_t pcm_bytes = read32le(p); p += 4;
      if (pcm_bytes > MD_PCM_BANK_BYTES) pcm_bytes = MD_PCM_BANK_BYTES;
      for (int i = 0; i < MD_MAX_SAMPLES; i++) {
        md_sample_t *sm = &mod->samples[i];
        memcpy(sm->name, p, 24);
        sm->name[23] = 0;
        sm->offset = read32le(p + 24);
        sm->length = read32le(p + 28);
        sm->loop = (int32_t)read32le(p + 32);
        sm->base_note = p[36];
        // Un en-tête qui sortirait de la banque est ignoré plutôt que de
        // faire lire n'importe quoi au moteur.
        if (sm->offset > pcm_bytes || sm->length > pcm_bytes - sm->offset) {
          memset(sm, 0, sizeof(*sm));
        }
        p += MD_SAMPLE_HDR_BYTES;
      }
      if (size >= base + 4 + MD_SAMPLES_HDR_TOTAL + pcm_bytes) {
        memcpy(mod->pcm, p, pcm_bytes);
        mod->pcm_used = pcm_bytes;
      }
    }
  }

  current_module = mod;

  md_unlock(&replayer_lock);
  return true;
}


// Cherche, à partir de `from`, la prochaine ligne du SONG qui porte un chain
// sur ce canal. Renvoie -1 si la colonne est vide jusqu'en bas.
static int md_next_song_row(int c, int from) {
  for (int r = from; r < MD_SONG_ROWS; r++)
    if (current_module->song[c][r] != MD_EMPTY)
      return r;
  return -1;
}

// Place le canal sur la première phrase utilisable à partir de sa position
// courante, en sautant les lignes de chain vides et en passant au chain suivant
// du song quand le chain est épuisé. Boucle en fin de colonne (comme LSDJ).
// ── Blocs de la grille SONG ─────────────────────────────────────────────────
// Modèle repris d'un tracker de samples à deux canaux pris pour référence
// (soloSongLocs, blockTopRow, addChainLocs, buildSongLocations).
//
// Chaque canal joue le BLOC CONTIGU de lignes remplies qui contient le curseur,
// et boucle sur CE bloc. Trois règles, dans les mots du fichier :
//   « Play CONTIGUOUS non-empty song rows; stop at the first empty (loops there). »
//   « A song loop must return to the TOP of its block, not to wherever playback
//     entered it. »
//   « enter on the cursor row within the block »
// Et pour les chaînes : « STOPS at the first empty row (it never skips an empty
// to reach a later value) ».
//
// Les canaux sont INDÉPENDANTS : chacun boucle sur son propre bloc, qui n'a pas
// forcément la même longueur que celui du voisin.

// Haut du bloc contigu qui contient `from` : on remonte tant que c'est rempli.
static int md_block_top_row(int c, int from) {
  if (!current_module || c < 0 || c >= MD_MAX_CHANNELS)
    return 0;
  if (from < 0 || from >= MD_SONG_ROWS)
    return 0;
  if (current_module->song[c][from] == MD_EMPTY)
    return from;
  int t = from;
  while (t > 0 && current_module->song[c][t - 1] != MD_EMPTY)
    t--;
  return t;
}

static void md_channel_seek(int c) {
  for (int guard = 0; guard < 4096; guard++) {
    if (ch_chain[c] < 0) { // il faut (re)prendre un chain dans le song
      int r = ch_song_row[c];
      // On joue les lignes CONTIGUËS. À la première case vide — ou au bas de la
      // grille — on reboucle en HAUT DU BLOC, pas en ligne 0. Sans ça, un
      // morceau qui démarre en ligne 02 remontait toujours en 00.
      if (r < 0 || r >= MD_SONG_ROWS ||
          current_module->song[c][r] == MD_EMPTY) {
        r = ch_block_top[c];
        if (r < 0 || r >= MD_SONG_ROWS ||
            current_module->song[c][r] == MD_EMPTY) {
          ch_running[c] = false; ch_phrase[c] = -1; return;
        }
      }
      ch_song_row[c] = r;
      ch_chain[c] = current_module->song[c][r];
      ch_chain_row[c] = 0;
    }
    if (ch_chain_row[c] >= MD_ROWS_PER_CHAIN) { // chain fini → ligne suivante
      ch_song_row[c]++;
      ch_chain[c] = -1;
      continue;
    }
    const md_chain_t *ch = &current_module->chains[ch_chain[c] & (MD_MAX_CHAINS - 1)];
    const md_chain_row_t *cr = &ch->rows[ch_chain_row[c]];
    if (cr->phrase == MD_EMPTY) {   // la chaîne s'arrête ici, on ne saute pas
      ch_song_row[c]++;
      ch_chain[c] = -1;
      continue;
    }
    ch_phrase[c] = cr->phrase;
    ch_row_transpose[c] = cr->transpose;
    return;
  }
  ch_running[c] = false;
}

// Cherche la prochaine ligne non vide À L'INTÉRIEUR du chain courant, en
// rebouclant sur lui : utilisé quand la lecture est limitée à un chain, où l'on
// ne doit jamais retourner chercher une ligne dans le song.
static void md_chain_seek_in_scope(int c) {
  const md_chain_t *ch =
      &current_module->chains[ch_chain[c] & (MD_MAX_CHAINS - 1)];
  // Écran CHAIN : la lecture part de la ligne du CURSEUR, avance sur les slots
  // contigus, s'arrête au premier vide — et reboucle sur LA LIGNE DU CURSEUR,
  // pas sur le slot 0.
  //
  // C'est ce que fait `addChainLocs(cid, fromRow: chainRow)` du tracker
  // d'exemple : la séquence commence à `fromRow`, « STOPS at the first empty
  // row », et boucle sur elle-même — donc elle revient à `fromRow`. Revenir au
  // slot 0 rejouait des lignes situées AVANT le curseur, séparées de lui par
  // un trou.
  int base = (scope_start_row >= 0 && scope_start_row < MD_ROWS_PER_CHAIN)
                 ? scope_start_row : 0;
  if (ch_chain_row[c] >= MD_ROWS_PER_CHAIN ||
      ch->rows[ch_chain_row[c]].phrase == MD_EMPTY)
    ch_chain_row[c] = base;
  const md_chain_row_t *cr = &ch->rows[ch_chain_row[c]];
  if (cr->phrase == MD_EMPTY) {
    ch_running[c] = false;   // chaîne entièrement vide
    ch_phrase[c] = -1;
    return;
  }
  ch_phrase[c] = cr->phrase;
  ch_row_transpose[c] = cr->transpose;
}

// ── H : le SAUT, aussi dans une PHRASE ─────────────────────────────────────
// Il n'existait que dans les tables. Dans une phrase, poser H ne faisait
// strictement rien — la commande se résolvait en « aucun effet », et le journal
// des essais le confirmait : H était l'une des deux seules lettres sans action.
//
// Mêmes conventions que dans une table, pour qu'il n'y ait qu'un H à retenir :
//   chiffre de gauche  combien de fois sauter avant de passer outre, 0 = sans fin
//   chiffre de droite  la ligne visée dans la phrase
// H00 fait donc boucler la phrase sur elle-même, indéfiniment.
//
// Le compteur est indexé par la LIGNE qui porte le H : deux boucles imbriquées
// comptent chacune leurs tours, comme dans une table.
//
// ⚠️ DIVERGENCE assumée avec le projet iPad, qui n'a pas encore ce saut-là.
static uint8_t ch_phrase_hop[MD_TOTAL_VOICES][MD_ROWS_PER_PHRASE];

void md_replayer_reset_phrase_hops(int c) {
  if (c < 0 || c >= MD_TOTAL_VOICES) return;
  for (int r = 0; r < MD_ROWS_PER_PHRASE; r++) ch_phrase_hop[c][r] = 0;
}

// La ligne courante porte-t-elle un H ?
static bool md_phrase_row_is_hop(int c) {
  if (!current_module || ch_phrase[c] < 0) return false;
  const int row = ch_phrase_row[c];
  if (row < 0 || row >= MD_ROWS_PER_PHRASE) return false;
  const md_phrase_row_t *pr =
      &current_module->phrases[ch_phrase[c] % MD_MAX_PHRASES].rows[row];
  return pr->cmd != MD_EMPTY && md_table_cmd_kind(pr->cmd) == MD_CMD_HOP;
}

// Effectue le saut de la ligne courante. Renvoie vrai s'il a eu lieu ; faux
// quand la boucle est terminée et qu'il faut simplement passer outre.
static bool md_phrase_hop_prend(int c) {
  const int row = ch_phrase_row[c];
  const md_phrase_row_t *pr =
      &current_module->phrases[ch_phrase[c] % MD_MAX_PHRASES].rows[row];
  const int fois = (pr->cmdval >> 4) & 0x0F;
  const int cible = pr->cmdval & 0x0F;
  if (fois == 0) { ch_phrase_row[c] = cible; return true; }   // sans fin
  if (ch_phrase_hop[c][row] < fois) {
    ch_phrase_hop[c][row]++;
    ch_phrase_row[c] = cible;
    return true;
  }
  ch_phrase_hop[c][row] = 0;                  // boucle terminée
  return false;
}

// Avance d'UNE ligne, en cascadant sur le chain puis sur le song quand on
// arrive au bout — sauf si la lecture est limitée à un chain ou une phrase.
static void md_channel_pas(int c) {
  ch_phrase_row[c]++;
  if (ch_phrase_row[c] < MD_ROWS_PER_PHRASE)
    return;
  ch_phrase_row[c] = 0;

  // Écran PHRASE : la phrase boucle sur elle-même, on ne remonte pas au chain.
  if (play_scope == MD_SCOPE_PHRASE)
    return;

  ch_chain_row[c]++;
  ch_phrase[c] = -1;
  // Écran CHAIN : on reste dans ce chain, qui boucle sur lui-même.
  if (play_scope == MD_SCOPE_CHAIN) {
    md_chain_seek_in_scope(c);
    return;
  }
  md_channel_seek(c);
}

// Fait avancer le canal, puis FRANCHIT tout de suite un éventuel H.
//
// Le repère de lecture ne doit JAMAIS s'arrêter sur la ligne qui porte le
// saut : la boucle se referme AVANT elle. Avec H00 en ligne 3, on parcourt
// 0 1 2 0 1 2… et la ligne 3 n'est ni affichée ni jouée. C'est ce que fait
// déjà la table quelques dizaines de lignes plus bas — la phrase, elle,
// s'arrêtait dessus une image, ce qui s'entendait et se voyait.
static void md_channel_advance(int c) {
  if (!ch_running[c]) return;
  md_channel_pas(c);
  // La borne évite de tourner sans fin si toutes les lignes portent un H.
  for (int garde = 0; garde <= MD_ROWS_PER_PHRASE; garde++) {
    if (!md_phrase_row_is_hop(c)) return;
    if (!md_phrase_hop_prend(c)) md_channel_pas(c);   // boucle finie : on passe
  }
}

// Construit l'événement de la ligne courante du canal, dans le format attendu
// par le moteur d'effets hérité (qui n'a pas changé d'une ligne).
static md_event_t md_channel_event(int c) {
  md_event_t ev;
  ev.note = 0; ev.instr = 0;
  ev.effects[0].type = 0xFF; ev.effects[0].val = 0;
  ev.effects[1].type = 0xFF; ev.effects[1].val = 0;
  if (!ch_running[c] || ch_phrase[c] < 0) return ev;

  const md_phrase_t *ph = &current_module->phrases[ch_phrase[c] % MD_MAX_PHRASES];
  const md_phrase_row_t *pr = &ph->rows[ch_phrase_row[c]];

  ev.note = pr->note;
  // Transposition du chain : elle décale la note, pas le note-off.
  if (ev.note > 0 && ev.note <= MD_MAX_NOTE && ch_row_transpose[c] != 0) {
    int n = (int)ev.note + ch_row_transpose[c];
    ev.note = (uint8_t)(n < 1 ? 1 : (n > MD_MAX_NOTE ? MD_MAX_NOTE : n));
  }
  ev.instr = pr->instr;
  if (pr->cmd != MD_EMPTY) {
    // La colonne CMD stocke l'INDICE de la commande (la lettre affichée) ; le
    // moteur d'effets attend un numéro d'effet. Même traduction que dans les
    // tables, par la liste partagée — il n'y a qu'un vocabulaire de commandes
    // dans tout le tracker.
    int pe = -1;
    uint8_t pv = 0;
    if (pr->cmd != MD_EMPTY)
      md_table_cmd_resolve(pr->cmd, pr->cmdval, &pe, &pv);
    ev.effects[0].type = (pe < 0) ? 0xFF : (uint8_t)pe;
    ev.effects[0].val = (pe < 0) ? 0 : pv;
  }
  return ev;
}

// Un effet qui module le son en continu (hauteur ou volume), par opposition à
// une action instantanée comme couper la note ou changer le tempo. Seuls les
// premiers survivent au changement de ligne.
// L'instrument convient-il à la colonne ?
//
// Un instrument PSG n'a aucun paramètre FM et réciproquement. DefleMask laisse
// la voie MUETTE quand les deux ne s'accordent pas ; sans ce garde-fou, notre
// moteur chargeait la moitié FM restée aux valeurs d'usine et jouait un son
// que le morceau d'origine ne produit jamais.
//
// Les instruments écrits à la main (MD_INSTR_KIND_ANY) passent partout.
static bool md_instr_fits_channel(int c, int instr) {
  if (!current_module || instr < 1 || instr > MD_MAX_INSTRUMENTS)
    return true;
  uint8_t kind = current_module->instruments[instr - 1].kind;
  if (kind == MD_INSTR_KIND_ANY)
    return true;
  bool psg = md_hw_is_psg(ch_hw_voice[c]);
  if (kind == MD_INSTR_KIND_PCM)
    return !psg && c == MD_PCM_CHANNEL;   // le DAC n'existe que sur FM6
  return psg ? (kind == MD_INSTR_KIND_PSG) : (kind == MD_INSTR_KIND_FM);
}

static bool md_effect_is_continuous(uint8_t eff) {
  switch (eff) {
  case MD_EFF_ARPEGGIO:
  case MD_EFF_FSLIDE_UP:
  case MD_EFF_FSLIDE_DOWN:
  case MD_EFF_FSLIDE_UP_FINE:
  case MD_EFF_FSLIDE_DOWN_FINE:
  case MD_EFF_TONE_PORTAMENTO:
  case MD_EFF_VIBRATO:
  case MD_EFF_TREMOLO:
    return true;
  default:
    return false;
  }
}

static void md_process_row() {

  if (!current_module)
    return;

  int next_order_index = -1;
  int next_row_index = -1; // conservés : les effets B/C les écrivent encore

  // Process all channels
  for (int c = 0; c < current_module->num_channels; c++) {
    if (md_channel_disabled[c]) continue;
    md_event_t event = md_channel_event(c);
    md_event_t *ev = &event;

    // Vélocité : elle fixe le volume du canal au déclenchement de la ligne.
    if (ch_running[c] && ch_phrase[c] >= 0) {
      uint8_t v = current_module->phrases[ch_phrase[c] % MD_MAX_PHRASES]
                      .rows[ch_phrase_row[c]].vel;
      if (v != MD_EMPTY) {
        // La colonne va de 0 à 7F comme DefleMask ; l'atténuation interne, elle,
        // tient sur six bits (les glissandos de volume la manipulent partout en
        // 0x3F). On ramène donc l'échelle ici, à l'unique endroit qui la lit.
        int vv = v > 127 ? 127 : v;
        ch_fmpar[c].volC = (uint8_t)(63 - (vv * 63 + 63) / 127);
        md_write_volume(c);
      }
    }

    if (ev->instr) {
      // Instrument destiné à l'autre puce : la colonne se tait, comme dans
      // DefleMask. On ne charge rien et on retiendra la note plus bas.
      if (!md_instr_fits_channel(c, ev->instr)) {
        ch_instr_blocked[c] = true;
      } else {
        ch_instr_blocked[c] = false;
        ch_current_instr[c] = ev->instr;
        // Poser un instrument FM sur la voie PCM la rebranche à la synthèse —
        // c'est le mode « FMP » : la colonne redevient une sixième voie FM.
        if (c == MD_PCM_CHANNEL &&
            current_module->instruments[ev->instr - 1].kind != MD_INSTR_KIND_PCM) {
          md_chip_pcm_stop();
          md_chip_pcm_enable(false);
        }
        md_set_instrument(c, &current_module->instruments[ev->instr - 1]);
        // La note vient d'écraser la copie de travail : on remet par-dessus
        // les réglages MD CMD encore actifs sur ce canal.
        md_reapply_mdcmds(c);
      }
    }

    // In trackers, most tick-based effects (Arpeggio, F-Slide, Vibrato) ONLY
    // run on the row they are defined. If the next row does not re-define them
    // (e.g. it is empty), the effect stops.
    // Some states (like base frequency, current volume) persist, but the
    // *action* of the effect does not. We must clear active tick effects per
    // row.
    for (int e = 0; e < 2; e++) {
      uint8_t eff_type = ev->effects[e].type;
      uint8_t eff_val = ev->effects[e].val;

      // Un effet CONTINU survit au changement de ligne.
      //
      // Dans un tracker classique, un effet ne dure que la ligne où il est
      // écrit. Ici on suit la convention de LSDJ : un pitch bend, un vibrato
      // ou un arpège continue de tourner jusqu'à ce qu'on l'arrête — sinon le
      // glissando s'interrompt sèchement au bout d'une ligne au lieu de courir
      // sur toute la phrase. On l'arrête en réécrivant la commande avec la
      // valeur 00.
      if (!md_effect_is_continuous(ch_active_eff[c][e])) {
        ch_active_eff[c][e] = 0xFF;
        ch_active_eff_val[c][e] = 0;
      } else if (eff_type == ch_active_eff[c][e] && eff_val == 0) {
        ch_active_eff[c][e] = 0xFF;   // même commande à 00 = on coupe
        ch_active_eff_val[c][e] = 0;
      }

      // ⚠️ UNE VALEUR 00 ARRÊTE UN EFFET CONTINU, elle ne le relance pas.
      // On coupait bien juste au-dessus, puis on le RÉARMAIT plus bas avec la
      // valeur nulle. Et comme les paramètres sont mémorisés
      // (md_table_latch_effect ne retient que les valeurs NON nulles), le
      // glissando repartait à la vitesse de la fois d'avant : « 02 00 » après
      // « 02 56 » continuait de glisser au lieu de s'arrêter.
      //
      // ⚠️ SEULEMENT LES CONTINUS. Le zéro est une valeur légitime ailleurs :
      // « J00 » veut dire « saute à la ligne 0 », pas « n'y va pas ».
      if (eff_val == 0 && md_effect_is_continuous(eff_type)) {
        ch_active_eff[c][e] = 0xFF;
        ch_active_eff_val[c][e] = 0;
        continue;
      }

      // In AT2, 0 is Arpeggio. But '000' is an empty cell.
      // We must explicitly skip '000' so it doesn't trigger the PERSISTENCE
      // of the last Arpeggio value.
      if (eff_type == 0 && eff_val == 0) {
        continue;
      }

      if (eff_type == 0xFF) {
        continue;
      }

      // (log [ARP] retiré — même raison que [NOTE].)

      if (eff_type == MD_EFF_ARPEGGIO || eff_type == MD_EFF_VIBRATO ||
          eff_type == MD_EFF_VOL_SLIDE || eff_type == MD_EFF_FSLIDE_UP ||
          eff_type == MD_EFF_FSLIDE_DOWN ||
          eff_type == MD_EFF_FSLIDE_UP_FINE ||
          eff_type == MD_EFF_FSLIDE_DOWN_FINE ||
          eff_type == MD_EFF_VOL_SLIDE_FINE ||
          eff_type == MD_EFF_TONE_PORTAMENTO ||
          eff_type == MD_EFF_TPORTA_VSLIDE ||
          eff_type == MD_EFF_TPORTA_VSLIDE_FINE ||
          eff_type == MD_EFF_VIBRATO_VSLIDE ||
          eff_type == MD_EFF_VIBRATO_VSLIDE_FINE ||
          eff_type == MD_EFF_ARPEGGIO_VSLIDE ||
          eff_type == MD_EFF_ARPEGGIO_VSLIDE_FINE ||
          eff_type == MD_EFF_FSLIDE_UP_VSLIDE ||
          eff_type == MD_EFF_FSLIDE_DOWN_VSLIDE ||
          eff_type == MD_EFF_FSLIDE_UP_FINE_VSLIDE ||
          eff_type == MD_EFF_FSLIDE_DOWN_FINE_VSLIDE ||
          eff_type == MD_EFF_FSLIDE_UP_VSLF ||
          eff_type == MD_EFF_FSLIDE_DOWN_VSLF ||
          eff_type == MD_EFF_FSLIDE_UP_FINE_VSLF ||
          eff_type == MD_EFF_FSLIDE_DOWN_FINE_VSLF ||
          eff_type == MD_EFF_TREMOLO ||
          eff_type == MD_EFF_TREMOR || eff_type == MD_EFF_RETRIG_NOTE ||
          eff_type == MD_EFF_MULTI_RETRIG_NOTE ||
          eff_type == MD_EFF_GLOBAL_FSLIDE_UP ||
          eff_type == MD_EFF_GLOBAL_FSLIDE_DOWN ||
          (eff_type == MD_EFF_EXTENDED2 &&
           ((eff_val >> 4) == MD_EX2_NOTE_DELAY ||
            (eff_val >> 4) == MD_EX2_NOTE_CUT ||
            (eff_val >> 4) == MD_EX2_GL_VOL_SLIDE_UP ||
            (eff_val >> 4) == MD_EX2_GL_VOL_SLIDE_DN ||
            (eff_val >> 4) == MD_EX2_GL_VOL_SLIDE_UP_F ||
            (eff_val >> 4) == MD_EX2_GL_VOL_SLIDE_DN_F))) {
        ch_active_eff[c][e] = eff_type;
        ch_active_eff_val[c][e] = eff_val;
      }

      switch (eff_type) {
      case MD_EFF_POSITION_JUMP: // Bxx
        next_order_index = eff_val;
        break;
      case MD_EFF_PATTERN_BREAK: // Cxx
        // In AT2, parameter is the row number (0-255). It is usually in hex in
        // UI but stored as int.
        next_row_index = eff_val;
        break;
      // ⚠️ Ces deux commandes ne touchent QUE l'état de lecture, jamais les
      // réglages du MORCEAU. Elles y écrivaient (initial_speed / initial_tempo),
      // or c'est exactement de là que le départ de lecture relit ses valeurs :
      // une commande survolée en éditant changeait donc le tempo du morceau
      // POUR DE BON. À une vitesse de 0xFF, la musique avançait d'une ligne
      // toutes les cinq secondes — on croit tout entendre s'arrêter — et le
      // stop/play n'y changeait rien puisqu'il rechargeait la valeur écrasée.
      case MD_EFF_SET_SPEED: // Axx with x < 0x20 usually (not tempo)
        if (eff_val > 0) {
          replay_speed = eff_val;
        }
        break;
      case MD_EFF_SET_TEMPO: // Nxx or Axx >= 0x20
        if (eff_val >= 18) {  // Minimum tempo
          replay_tempo = eff_val;
          // Update irq timings
          int irq_freq = replay_tempo * (current_module->macro_speedup > 0
                                             ? current_module->macro_speedup
                                             : 1);
          if (irq_freq == 0)
            irq_freq = 50;
          samples_per_tick = (int)((double)replay_sample_rate / ((double)irq_freq * md_finetune_scale()));
        if (samples_per_tick < 1) samples_per_tick = 1;
        }
        break;
      case MD_EFF_VOL_SLIDE:
      case MD_EFF_VOL_SLIDE_FINE:        // 20 : idem, mais appliqué au tick 0
        if (eff_val != 0)
          ch_volslide_val[c] = eff_val;
        break;
      case MD_EFF_FSLIDE_UP:
      case MD_EFF_FSLIDE_DOWN:
      case MD_EFF_FSLIDE_UP_FINE:        // 7 : fine = appliqué au tick 0
      case MD_EFF_FSLIDE_DOWN_FINE:      // 8
        if (eff_val != 0)
          ch_fslide_speed[c] = eff_val;
        break;

      // ── Effets COMBINÉS « X + volume slide » ───────────────────────────────
      // Pour TOUS ces combos, le paramètre alimente le VOLUME slide ; la partie
      // X (freq slide / arpège / vibrato) réutilise sa vitesse mémorisée.
      case MD_EFF_ARPEGGIO_VSLIDE:        // 24
      case MD_EFF_ARPEGGIO_VSLIDE_FINE:   // 25
      case MD_EFF_VIBRATO_VSLIDE_FINE:    // 17
      case MD_EFF_FSLIDE_UP_VSLIDE:       // 27
      case MD_EFF_FSLIDE_DOWN_VSLIDE:     // 28
      case MD_EFF_FSLIDE_UP_FINE_VSLIDE:  // 29
      case MD_EFF_FSLIDE_DOWN_FINE_VSLIDE:// 30
      case MD_EFF_FSLIDE_UP_VSLF:         // 31
      case MD_EFF_FSLIDE_DOWN_VSLF:       // 32
      case MD_EFF_FSLIDE_UP_FINE_VSLF:    // 33
      case MD_EFF_FSLIDE_DOWN_FINE_VSLF:  // 34
        if (eff_val != 0)
          ch_volslide_val[c] = eff_val;
        ch_arpeggio_state[c] = 0; // (utile pour 24/25 ; inoffensif sinon)
        break;

      case MD_EFF_TPORTA_VSLIDE_FINE:     // 16 : porta (mémorisé) + fine vol slide
        if (eff_val != 0)
          ch_volslide_val[c] = eff_val;
        if (ev->note > 0 && ev->note <= MD_MAX_NOTE) {
          uint8_t octave = (ev->note - 1) / 12;
          uint8_t pitch = (ev->note - 1) % 12;
          uint16_t tgt_fnum = md_note_fnums[pitch];
          if (current_module && ch_current_instr[c] > 0 &&
              ch_current_instr[c] <= 255) {
            int8_t ftune =
                current_module->instruments[ch_current_instr[c] - 1].fine_tune;
            tgt_fnum = (uint16_t)((int16_t)tgt_fnum + ftune);
          }
          uint8_t block = (octave <= MD_MAX_BLOCK) ? octave : MD_MAX_BLOCK;
          ch_porta_target[c] = (block << 10) | (tgt_fnum & 0x03FF);
        }
        break;
      case MD_EFF_TONE_PORTAMENTO:
        if (eff_val != 0)
          ch_porta_speed[c] = eff_val;
        if (ev->note > 0 && ev->note <= MD_MAX_NOTE) {
          uint8_t octave = (ev->note - 1) / 12;
          uint8_t pitch = (ev->note - 1) % 12;
          uint16_t tgt_fnum = md_note_fnums[pitch];
          if (current_module && ch_current_instr[c] > 0 &&
              ch_current_instr[c] <= 255) {
            int8_t ftune =
                current_module->instruments[ch_current_instr[c] - 1].fine_tune;
            tgt_fnum = (uint16_t)((int16_t)tgt_fnum + ftune);
          }
          uint8_t block = (octave <= MD_MAX_BLOCK) ? octave : MD_MAX_BLOCK;
          // Very simple tone portamento tracking, ideally tracking absolute
          // frequencies including block shifts. For now we track target fnum in
          // the same block.
          ch_porta_target[c] = (block << 10) | (tgt_fnum & 0x03FF);
        }
        break;
      case MD_EFF_TPORTA_VSLIDE:
        if (eff_val != 0)
          ch_volslide_val[c] = eff_val;
        break;
      case MD_EFF_VIBRATO:
      case MD_EFF_VIBRATO_VSLIDE:
        if (eff_val != 0) {
          if (eff_val >> 4)
            ch_vibrato_speed[c] = eff_val >> 4;
          if (eff_val & 0x0F)
            ch_vibrato_depth[c] = eff_val & 0x0F;
        }
        if (eff_type == MD_EFF_VIBRATO_VSLIDE && eff_val != 0)
          ch_volslide_val[c] = eff_val;
        break;
      case MD_EFF_TREMOLO:
        if (eff_val != 0) {
          if (eff_val >> 4)
            ch_tremolo_speed[c] = eff_val >> 4;
          if (eff_val & 0x0F)
            ch_tremolo_depth[c] = eff_val & 0x0F;
        }
        break;
      case MD_EFF_TREMOR:
        if (eff_val != 0)
          ch_tremor_val[c] = eff_val;
        break;
      case MD_EFF_ARPEGGIO:
        if (eff_val != 0)
          ch_arpeggio_val[c] = eff_val;
        ch_arpeggio_state[c] = 0; // reset
        break;
      case MD_EFF_SET_GLOBAL_VOLUME:
        replay_global_vol = eff_val;
        if (replay_global_vol > 63)
          replay_global_vol = 63;
        break;
      case MD_EFF_RETRIG_NOTE:
        if (eff_val != 0)
          ch_retrig_speed[c] = eff_val;
        ch_retrig_count[c] = 0;
        break;
      case MD_EFF_MULTI_RETRIG_NOTE:
        // Multi-retrig : intervalle = nibble HAUT, opération volume = nibble BAS
        // (cf. AdlibTracker2 ef_MultiRetrigNote). On (re)démarre le compteur.
        ch_retrig_count[c] = 0;
        break;

      // ── Effets « set » instantanés (appliqués au déclenchement de la row) ──
      // Sémantique conservée du moteur d'origine : le paramètre est la LOUDNESS
      // (0..63) ; l'atténuation interne vaut 63 - paramètre.
      case MD_EFF_SET_CAR_VOL: { // 18 : volume des opérateurs porteurs
        ch_fmpar[c].volC = (uint8_t)((63 - (eff_val & 0x3F)) & 0x3F);
        md_write_volume(c);
        break;
      }
      case MD_EFF_SET_MOD_VOL: { // 9 : volume des opérateurs modulateurs
        ch_fmpar[c].volM = (uint8_t)((63 - (eff_val & 0x3F)) & 0x3F);
        md_write_volume(c);
        break;
      }
      case MD_EFF_SET_INS_VOLUME: { // 12 : volume instrument
        uint8_t atten = (uint8_t)((63 - (eff_val & 0x3F)) & 0x3F);
        ch_fmpar[c].volC = atten;
        if (ch_fmpar[c].feedbackConn & 1) // algorithme additif → aussi les mod
          ch_fmpar[c].volM = atten;
        md_write_volume(c);
        break;
      }
      case MD_EFF_SET_ALGORITHM: { // 19 : algorithme (nibble haut) / feedback (bas)
        uint8_t alg = eff_val >> 4, fb = eff_val & 0x0F;
        md_instr_t *sh = &ch_instr_shadow[c];
        if (alg <= 7)
          sh->algorithm = alg;
        if (fb <= 7)
          sh->feedback = fb;
        md_apply_instrument((uint8_t)c);
        break;
      }
      case MD_EFF_FORCE_INS_VOLUME: { // 40 : force le volume
        uint8_t atten = (uint8_t)((63 - (eff_val & 0x3F)) & 0x3F);
        ch_fmpar[c].volC = atten;
        if (ch_fmpar[c].feedbackConn & 1)
          ch_fmpar[c].volM = atten;
        md_write_volume(c);
        break;
      }
      case MD_EFF_EXTENDED3: { // 41 : réglages YM2612 en direct
        // nibble haut = sous-commande, nibble bas = valeur (0-15).
        // Ces réglages n'affectent que la VOIE en cours, pas l'instrument.
        uint8_t sub = eff_val >> 4;
        uint8_t v = eff_val & 0x0F;
        md_instr_t *sh = &ch_instr_shadow[c];
        switch (sub) {
        case 0: // algorithme (0-7)
          sh->algorithm = (v <= 7) ? v : 7;
          break;
        case 1: // feedback (0-7)
          sh->feedback = (v <= 7) ? v : 7;
          break;
        case 2: // multiple OP1
        case 3: // multiple OP2
        case 4: // multiple OP3
        case 5: // multiple OP4
          sh->op[sub - 2].multiple = v;
          break;
        case 6: // detune OP1
        case 7: // detune OP2
        case 8: // detune OP3
        case 9: // detune OP4
          sh->op[sub - 6].detune = (uint8_t)(v & 0x07);
          break;
        case 10: // attaque (tous les opérateurs)
          for (int o = 0; o < 4; o++)
            sh->op[o].attack = (uint8_t)(v * 2 + 1);
          break;
        case 11: // decay (tous les opérateurs)
          for (int o = 0; o < 4; o++)
            sh->op[o].decay = (uint8_t)(v * 2);
          break;
        case 12: // niveau de maintien (tous les opérateurs)
          for (int o = 0; o < 4; o++)
            sh->op[o].sustain_level = v;
          break;
        case 13: // release (tous les opérateurs)
          for (int o = 0; o < 4; o++)
            sh->op[o].release = v;
          break;
        case 14: // AMS (0-3)
          sh->ams = (uint8_t)(v & 0x03);
          break;
        case 15: // PMS (0-7)
          sh->pms = (uint8_t)(v & 0x07);
          break;
        default:
          break;
        }
        md_apply_instrument((uint8_t)c);
        break;
      }
      case MD_EFF_EXTENDED: {
        uint8_t ext_type = eff_val >> 4;
        uint8_t ext_val = eff_val & 0x0F;
        if (ext_type == MD_EX_SET_PANNING_POS) {
          ch_panning[c] = (ext_val > 2) ? 0 : ext_val;
          md_apply_panning(c);
        }
        break;
      }
      case MD_EFF_EXTENDED2: {
        uint8_t ext_type = eff_val >> 4;
        uint8_t ext_val = eff_val & 0x0F;
        if (ext_type == MD_EX2_NOTE_DELAY) {
          ch_note_delay[c] = ext_val;
        } else if (ext_type == MD_EX2_NOTE_CUT) {
          ch_note_cut[c] = ext_val;
        } else if (ext_type == MD_EX2_FINE_TUNE_UP) {
          ch_base_fnum[c] = md_slide_frequency(ch_base_fnum[c], -ext_val);
        } else if (ext_type == MD_EX2_FINE_TUNE_DOWN) {
          ch_base_fnum[c] = md_slide_frequency(ch_base_fnum[c], ext_val);
        }
        break;
      }
      case MD_EFF_GLOBAL_FSLIDE_UP:
      case MD_EFF_GLOBAL_FSLIDE_DOWN:
        // These are parsed into ch_active_eff for tick processing
        break;
      }
    }

    // Check if Tone Portamento prevented the actual note-on. If so, don't
    // retrigger.
    bool has_tone_porta = false;
    bool has_note_delay = false;
    for (int e = 0; e < 2; e++) {
      if (ev->effects[e].type == MD_EFF_TONE_PORTAMENTO ||
          ev->effects[e].type == MD_EFF_TPORTA_VSLIDE ||
          ev->effects[e].type == MD_EFF_TPORTA_VSLIDE_FINE) {
        has_tone_porta = true;
      }
      if (ev->effects[e].type == MD_EFF_EXTENDED2 &&
          (ev->effects[e].val >> 4) == MD_EX2_NOTE_DELAY) {
        has_note_delay = true;
      }
    }

    // La colonne MD CMD peut porter les mêmes effets. Retard et glissando
    // vers la note doivent être vus AVANT de déclencher la note, alors que
    // la commande MD, elle, s'applique après : sans cette lecture anticipée,
    // un ED (retard) écrit en MD CMD ne retenait rien du tout.
    if (ch_running[c] && ch_phrase[c] >= 0) {
      const md_phrase_row_t *mr =
          &current_module->phrases[ch_phrase[c] % MD_MAX_PHRASES]
               .rows[ch_phrase_row[c]];
      if (mr->mdcmd != MD_EMPTY && md_mdcmd_is_effect(mr->mdcmd)) {
        int me; uint8_t mv;
        md_mdcmd_resolve(mr->mdcmd, mr->mdval, &me, &mv);
        // La commande CMD reste prioritaire : si elle décrit déjà le même
        // effet, la colonne MD se tait, ici comme à l'application.
        char meq = md_mdcmd_equivalent(mr->mdcmd);
        bool masquee = meq && mr->cmd != MD_EMPTY &&
                       md_table_cmd_letter(mr->cmd) == meq;
        if (!masquee) {
          if (me == MD_EFF_TONE_PORTAMENTO || me == MD_EFF_TPORTA_VSLIDE ||
              me == MD_EFF_TPORTA_VSLIDE_FINE)
            has_tone_porta = true;
          if (me == MD_EFF_EXTENDED2 && (mv >> 4) == MD_EX2_NOTE_DELAY)
            has_note_delay = true;
        }
      }
    }

    if (ev->note > 0 && ev->note <= MD_MAX_NOTE && !has_tone_porta &&
        !has_note_delay && !ch_instr_blocked[c]) {
      md_play_note(c, ev->note);
      // New notes usually reset Arpeggio and tone portamento state in trackers
      // For AT2 specifically, a new note resets continuous effect trackers
      // if they aren't explicitly re-triggered.
      ch_arpeggio_state[c] = 0;
    } else if (ev->note == 0xFF || ev->note == 0x80) {
      if (md_channel_is_pcm(c)) md_chip_pcm_stop();
      md_key_off(c);
      for (int e = 0; e < MD_EFF_SLOTS; e++) {
        ch_active_eff[c][e] = 0xFF;
        ch_active_eff_val[c][e] = 0;
      }
    }

    // ── Commande A : choisir la table jouée par ce canal ──────────────────
    // Elle n'est pas un effet — elle pilote la lecture de table. Il faut donc
    // la lire directement dans la ligne de phrase, et surtout l'appliquer
    // APRÈS le déclenchement de la note : md_key_on() réarme la table de
    // l'instrument et écraserait notre choix.
    if (ch_running[c] && ch_phrase[c] >= 0) {
      const md_phrase_row_t *pr =
          &current_module->phrases[ch_phrase[c] % MD_MAX_PHRASES]
               .rows[ch_phrase_row[c]];
      if (pr->cmd != MD_EMPTY && md_table_cmd_kind(pr->cmd) == MD_CMD_TABLE)
        md_cmd_table_select(c, pr->cmdval);
      // Colonne MD : réglages de la machine, appliqués APRÈS la note, qui
      // vient de recharger l'instrument et écraserait sinon la retouche.
      if (pr->mdcmd != MD_EMPTY) {
        md_apply_mdcmd(c, pr->mdcmd, pr->mdval, pr->cmd, MD_EFF_SLOT_MDCMD);
        // Le saut de position ne passe pas par le moteur d'effets : il change
        // la ligne de SONG, décidée quelques lignes plus bas. On le relève
        // donc ici, sinon un 0Bxx venu d'un .dmf ne sauterait nulle part.
        int je; uint8_t jv;
        md_mdcmd_resolve(pr->mdcmd, pr->mdval, &je, &jv);
        char jeq = md_mdcmd_equivalent(pr->mdcmd);
        bool jmasq = jeq && pr->cmd != MD_EMPTY &&
                     md_table_cmd_letter(pr->cmd) == jeq;
        if (!jmasq && je == MD_EFF_POSITION_JUMP && jv < MD_SONG_ROWS)
          next_order_index = jv;
      }
    }
  }

  // ── Avance : chaque canal progresse dans SA colonne ──
  // Position Jump (B) et Pattern Break (C) : dans le modèle LSDJ il n'y a plus
  // de pattern global. B relance TOUS les canaux à la ligne de song demandée,
  // C saute à la ligne de phrase demandée sur le canal qui l'a posée — le plus
  // proche équivalent utile.
  if (next_order_index >= 0 && next_order_index < MD_SONG_ROWS) {
    for (int c = 0; c < current_module->num_channels; c++) {
      ch_song_row[c] = next_order_index;
      ch_block_top[c] = md_block_top_row(c, next_order_index);
      ch_chain[c] = -1; ch_chain_row[c] = 0;
      ch_phrase[c] = -1; ch_phrase_row[c] = 0;
      ch_running[c] = true;
      md_channel_seek(c);
    }
  } else {
    for (int c = 0; c < current_module->num_channels; c++)
      md_channel_advance(c);
  }
  (void)next_row_index;

  // Mode LIVE : les canaux armés démarrent maintenant, tous sur la même ligne.
  if (live_mode) {
    for (int c = 0; c < current_module->num_channels; c++) {
      if (ch_pending_row[c] < 0) continue;
      ch_song_row[c] = ch_pending_row[c];
      ch_pending_row[c] = -1;
      ch_chain[c] = -1; ch_chain_row[c] = 0;
      ch_phrase[c] = -1; ch_phrase_row[c] = 0;
      ch_running[c] = true;
      md_channel_seek(c);
    }
  }

  // Ligne affichée par l'UI hérité : celle du premier canal encore en lecture.
  for (int c = 0; c < current_module->num_channels; c++) {
    if (ch_running[c]) { md_current_row = (uint8_t)ch_phrase_row[c]; break; }
  }
}

// Advance the Arpeggio (0xy) effect by one tick on channel `c`: cycles the
// pitch base -> base+x -> base+y while keeping the note keyed-on (no envelope
// retrigger). Used both by the sequencer tick and by the piano preview so the
// preview sounds IDENTICAL to the 0xy command.
// Réécrit la hauteur du canal à partir de sa note courante décalée de
// `semitones`. Partagé par l'effet d'arpège et par la macro d'arpège PSG.
// L'état key-on/off n'est pas touché : la hauteur continue donc de bouger
// pendant la phase de release d'une note relâchée.
// Écrit la hauteur à `semitones` de la note en cours. La transposition de la
// TABLE (colonne TSP) est TOUJOURS comprise dedans : c'est un décalage de la
// voie, pas un événement ponctuel. Sans ça, la moindre commande qui recalculait
// la hauteur — un arpège, un slide, un vibrato — effaçait le TSP posé sur la
// même ligne, et la transposition semblait ne pas marcher.
// Refait la hauteur de référence du canal à partir de sa note et de la
// transposition de table en cours, et l'envoie à la puce. Sans key-on : on
// transpose une note qui sonne, on ne la redéclenche pas.
static void md_rebuild_base_pitch(int c) {
  if (c < 0 || c >= MD_TOTAL_VOICES || ch_current_note[c] == 0)
    return;
  int note = (int)ch_current_note[c] + (int)ch_table_tsp[c];
  if (note < 1) note = 1;
  if (note > MD_MAX_NOTE) note = MD_MAX_NOTE;
  uint8_t octave = (uint8_t)((note - 1) / 12);
  uint16_t fnum = md_note_fnums[(note - 1) % 12];
  if (current_module && ch_current_instr[c] > 0 &&
      ch_current_instr[c] <= MD_MAX_INSTRUMENTS)
    fnum = (uint16_t)((int16_t)fnum +
                      current_module->instruments[ch_current_instr[c] - 1].fine_tune);
  uint8_t block = (octave <= MD_MAX_BLOCK) ? octave : MD_MAX_BLOCK;
  ch_base_fnum[c] = (uint16_t)((block << 10) | (fnum & 0x03FF));
  md_write_freq(c, ch_base_fnum[c]);
}

static void md_write_note_offset(int c, int semitones) {
  if (c < 0 || c >= MD_TOTAL_VOICES)
    return;
  int note = (int)ch_current_note[c] + semitones + (int)ch_table_tsp[c];
  if (note < 1)
    note = 1;
  if (note > MD_MAX_NOTE)
    note = MD_MAX_NOTE;

  uint8_t octave = (uint8_t)((note - 1) / 12);
  uint8_t pitch = (uint8_t)((note - 1) % 12);
  uint16_t fnum = md_note_fnums[pitch];

  if (current_module && ch_current_instr[c] > 0 && ch_current_instr[c] <= 255) {
    int8_t ftune =
        current_module->instruments[ch_current_instr[c] - 1].fine_tune;
    fnum = (uint16_t)((int16_t)fnum + ftune);
  }

  uint8_t block = (octave <= MD_MAX_BLOCK) ? octave : MD_MAX_BLOCK;
  md_write_freq(c, (uint16_t)((block << 10) | (fnum & 0x03FF)));
}

static void apply_arpeggio_tick(int c) {
  if (c < 0 || c >= MD_TOTAL_VOICES)
    return;
  uint8_t val = ch_arpeggio_val[c];
  if (val == 0)
    return;

  ch_arpeggio_state[c] = (ch_arpeggio_state[c] + 1) % 3;
  int semitones = 0;
  if (ch_arpeggio_state[c] == 1)
    semitones = val >> 4;
  else if (ch_arpeggio_state[c] == 2)
    semitones = val & 0x0F;

  uint8_t note = ch_current_note[c];
  if (note == 0 || note > MD_MAX_NOTE)
    return;

  md_write_note_offset(c, semitones);
}

// Déroule d'une ligne la table de l'instrument du canal, si elle en a une.
// Une ligne par tick, rebouclée tant que la note dure — c'est le mécanisme des
// tables de LSDJ, et il vaut pour les voies FM comme pour les voies PSG.
// Fait franchir au flux `s` tous les H qu'il rencontre, SANS rien jouer.
//
// Un saut ne consomme pas de tick : la ligne qui porte le H n'est jamais
// jouée, et le repère de lecture ne s'y arrête pas non plus. La boucle se
// déroule donc entièrement AU-DESSUS du H.
// Le garde-fou évite de tourner en rond si un H se pointe lui-même.
// Mémorise les paramètres d'un effet posé par une TABLE.
//
// Le moteur d'effets fonctionne en deux temps : la LIGNE mémorise les
// paramètres (vitesse de vibrato, valeur d'arpège…), puis chaque TICK les
// applique. Une table ne passe jamais par le traitement de ligne : sans cette
// fonction, elle armait l'effet sans lui donner ses paramètres, et il ne se
// passait rien. On reproduit donc ici la mémorisation, pour les seules
// commandes qu'une table peut poser.
static void md_table_latch_effect(int c, uint8_t eff, uint8_t val) {
  switch (eff) {
  case MD_EFF_ARPEGGIO:
    if (val) ch_arpeggio_val[c] = val;
    ch_arpeggio_state[c] = 0;
    break;
  case MD_EFF_VIBRATO:
    if (val) {
      if (val >> 4) ch_vibrato_speed[c] = val >> 4;
      if (val & 0x0F) ch_vibrato_depth[c] = val & 0x0F;
    }
    break;
  case MD_EFF_TREMOLO:
    if (val) {
      if (val >> 4) ch_tremolo_speed[c] = val >> 4;
      if (val & 0x0F) ch_tremolo_depth[c] = val & 0x0F;
    }
    break;
  case MD_EFF_TONE_PORTAMENTO:
    if (val) ch_porta_speed[c] = val;
    break;
  case MD_EFF_FSLIDE_UP:
  case MD_EFF_FSLIDE_DOWN:
  case MD_EFF_FSLIDE_UP_FINE:
  case MD_EFF_FSLIDE_DOWN_FINE:
    // Le glissando lit sa vitesse dans ch_fslide_speed : sans cette ligne,
    // l'effet était armé mais glissait de zéro.
    if (val) ch_fslide_speed[c] = val;
    break;
  case MD_EFF_RETRIG_NOTE:
    if (val) ch_retrig_speed[c] = val;
    ch_retrig_count[c] = 0;
    break;
  case MD_EFF_SET_GLOBAL_VOLUME:
    replay_global_vol = (val > 63) ? 63 : val;
    break;
  case MD_EFF_VOL_SLIDE:
  case MD_EFF_TPORTA_VSLIDE:
  case MD_EFF_VIBRATO_VSLIDE:
    if (val) ch_volslide_val[c] = val;
    break;
  case MD_EFF_SET_SPEED:
    if (val > 0) replay_speed = val;
    break;
  case MD_EFF_SET_TEMPO:
    if (val >= 18 && current_module) {
      replay_tempo = val;
      int mul = current_module->macro_speedup > 0 ? current_module->macro_speedup : 1;
      int irq = replay_tempo * mul;
      if (irq == 0) irq = 50;
      samples_per_tick =
          (int)((double)replay_sample_rate / ((double)irq * md_finetune_scale()));
      if (samples_per_tick < 1) samples_per_tick = 1;
    }
    break;
  case MD_EFF_EXTENDED: {
    uint8_t t = val >> 4, v = val & 0x0F;
    if (t == MD_EX_SET_PANNING_POS) {
      ch_panning[c] = (v > 2) ? 0 : v;
      md_apply_panning(c);
    }
    break;
  }
  case MD_EFF_EXTENDED2: {
    uint8_t t = val >> 4, v = val & 0x0F;
    if (t == MD_EX2_NOTE_CUT) ch_note_cut[c] = v;
    else if (t == MD_EX2_NOTE_DELAY) ch_note_delay[c] = v;
    break;
  }
  default:
    break;
  }
}

static void md_table_resolve_hops(int c, int s, const md_table_t *tbl) {
  for (int guard = 0; guard < MD_TABLE_ROWS + 1; guard++) {
    int row = ch_table_pos[c][s] & (MD_TABLE_ROWS - 1);
    const md_table_row_t *r = &tbl->rows[row];
    uint8_t cmd = (s == 1) ? r->cmd1 : r->cmd2;
    uint8_t val = (s == 1) ? r->val1 : r->val2;
    if (s == 0 || cmd == MD_EMPTY || md_table_cmd_kind(cmd) != MD_CMD_HOP)
      return;

    // Premier chiffre : combien de fois sauter avant de passer outre
    // (0 = à l'infini). Second chiffre : la ligne visée. Le compteur est
    // indexé par la ligne qui porte le H, ce qui permet d'imbriquer les
    // boucles — chacune compte ses propres tours.
    int times = (val >> 4) & 0x0F;
    int dest = val & 0x0F;
    if (times == 0) {
      ch_table_pos[c][s] = (uint8_t)dest;
    } else if (ch_hop_left[c][s][row] < times) {
      ch_hop_left[c][s][row]++;
      ch_table_pos[c][s] = (uint8_t)dest;
    } else {
      ch_hop_left[c][s][row] = 0;   // boucle terminée
      ch_table_pos[c][s] = (uint8_t)((row + 1) & (MD_TABLE_ROWS - 1));
    }
  }
}

static void md_table_tick(int c) {
  if (c < 0 || c >= MD_TOTAL_VOICES || !current_module)
    return;
  if (!ch_table_running[c])
    return;
  int t = ch_table[c];
  if (t < 0 || t >= MD_MAX_TABLES) {
    ch_table_running[c] = false;
    return;
  }
  const md_table_t *tbl = &current_module->tables[t];

  // Les trois flux avancent chacun pour leur compte : c'est ce qui permet à un
  // H de faire boucler la colonne TSP pendant que la seconde CMD continue.
  for (int s = 0; s < MD_TABLE_STREAMS; s++) {
    md_table_resolve_hops(c, s, tbl);
    int row = ch_table_pos[c][s] & (MD_TABLE_ROWS - 1);
    const md_table_row_t *r = &tbl->rows[row];

    if (s == 0) {
      // ── Flux 0 : VOL ────────────────────────────────────────────────────
      // 0 = on n'y touche pas.
      if (r->vol != 0) {
        static const uint8_t vol_to_atten[256] = {
        63, 32, 28, 26, 24, 23, 22, 21, 20, 19, 19, 18, 18, 17, 17, 16,
        16, 16, 15, 15, 15, 14, 14, 14, 14, 13, 13, 13, 13, 12, 12, 12,
        12, 12, 12, 11, 11, 11, 11, 11, 11, 10, 10, 10, 10, 10, 10, 10,
        10,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  8,  8,  8,  8,  8,
         8,  8,  8,  8,  8,  8,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,
         7,  7,  7,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
         6,  6,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
         5,  5,  5,  5,  5,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,
         4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  3,  3,  3,  3,  3,
         3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,
         3,  3,  3,  3,  3,  3,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,
         2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,
         2,  2,  2,  2,  2,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
         1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
         1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  0,  0,  0,  0,  0,  0,
         0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0
        };
        ch_fmpar[c].volC = vol_to_atten[r->vol];
        md_write_volume(c);
      }
    } else {
      // ── Flux 1 : TSP + première CMD · Flux 2 : seconde CMD ──────────────
      if (s == 1 && r->transpose != ch_table_tsp[c]) {
        // On ne réécrit la hauteur que lorsque la transposition CHANGE, sinon
        // on écraserait à chaque tick le travail des slides et du vibrato.
        //
        // Elle est reportée dans la hauteur de RÉFÉRENCE : c'est celle que le
        // moteur restaure au début de chaque ligne. En se contentant d'écrire
        // la hauteur du moment, le TSP était effacé à la ligne suivante dès
        // qu'une commande l'accompagnait.
        ch_table_tsp[c] = r->transpose;
        md_rebuild_base_pitch(c);
      }

      // La colonne MD voyage avec le second flux : c'est un réglage ponctuel,
      // il n'a pas besoin d'un flux à lui.
      if (s == 2 && r->mdcmd != MD_EMPTY)
        md_apply_mdcmd(c, r->mdcmd, r->mdval, r->cmd2, MD_EFF_SLOT_TABLE_MD);

      uint8_t cmd = (s == 1) ? r->cmd1 : r->cmd2;
      uint8_t val = (s == 1) ? r->val1 : r->val2;
      int eff = -1;
      uint8_t rval = 0;
      if (cmd != MD_EMPTY)
        md_table_cmd_resolve(cmd, val, &eff, &rval);

      // A : bascule vers une autre table (ou arrête la lecture de table).
      if (cmd != MD_EMPTY && md_table_cmd_kind(cmd) == MD_CMD_TABLE) {
        md_cmd_table_select(c, val);
        return;   // la table a changé sous nos pieds : on s'arrête ici
      }

      // Les commandes partent vers le moteur d'effets, dans l'emplacement
      // réservé à ce flux.
      //
      // Une ligne SANS commande ne désarme rien : la commande posée plus haut
      // continue d'agir, comme dans LSDJ. Sans ça, un retrig armé une ligne sur
      // seize voyait son compteur remis à zéro à chaque tour et n'atteignait
      // jamais son intervalle. Pour arrêter une commande, on la réécrit avec la
      // valeur 00.
      int slot = MD_EFF_SLOT_TABLE + (s - 1);
      if (cmd != MD_EMPTY) {
        uint8_t e = (eff < 0) ? 0xFF : (uint8_t)eff;
        uint8_t v = (eff < 0) ? 0 : rval;
        // Une valeur 00 ARRÊTE un effet continu, dans une table comme dans une
        // phrase — sinon les paramètres mémorisés le relancent à la vitesse
        // d'avant.
        if (eff >= 0 && v == 0 && md_effect_is_continuous(e)) {
          ch_active_eff[c][slot] = 0xFF;
          ch_active_eff_val[c][slot] = 0;
          ch_table_pos[c][s] = (uint8_t)((row + 1) & (MD_TABLE_ROWS - 1));
          md_table_resolve_hops(c, s, tbl);
          continue;
        }
        // On ne réarme que ce qui change, pour ne pas relancer à chaque tick un
        // effet continu comme le vibrato.
        if (ch_active_eff[c][slot] != e || ch_active_eff_val[c][slot] != v) {
          ch_active_eff[c][slot] = e;
          ch_active_eff_val[c][slot] = v;
          if (eff >= 0)
            md_table_latch_effect(c, e, v);  // sans ça, l'effet n'a rien à jouer
        }
      }
    }

    ch_table_pos[c][s] = (uint8_t)((row + 1) & (MD_TABLE_ROWS - 1));
    // On franchit tout de suite un éventuel H : le repère ne doit jamais
    // s'arrêter sur la ligne qui le porte.
    md_table_resolve_hops(c, s, tbl);
  }
}

static void md_process_tick() {
  if (replay_ticks_left == 0) {
    md_process_row();
    replay_ticks_left = replay_speed;

    // Restore base frequency and volume for all channels in case the previous
    // row had temporary tick effects (Arpeggio, Vibrato, Tremolo, Tremor) that
    // normally do not persist into the next empty row.
    if (current_module) {
      for (int c = 0; c < current_module->num_channels; ++c) {
        md_write_freq(c, ch_base_fnum[c]);
        md_write_volume(c);
      }
    }
  } else {
    // Process continuous effects (ticks 1 to speed-1)
    if (current_module) {
      int current_tick = replay_speed - replay_ticks_left;

      // Les tables d'instrument avancent d'une ligne, avant le traitement des
      // effets : ce qu'elles posent dans leurs emplacements est donc pris en
      // compte dès ce tick-ci.
      for (int c = 0; c < MD_TOTAL_VOICES; ++c)
        md_table_tick(c);

      // Pre-tick Global Effects Accumulation
      int8_t tick_gl_volslide = 0;
      int16_t tick_gl_fslide =
          0; // Renamed from temposlide to match usage below

      for (int c = 0; c < current_module->num_channels; ++c) {
        for (int e = 0; e < MD_EFF_SLOTS; e++) {
          uint8_t eff = ch_active_eff[c][e];
          uint8_t val = ch_active_eff_val[c][e];
          if (eff == MD_EFF_GLOBAL_FSLIDE_UP)
            tick_gl_fslide -= val; // logic uses - for UP
          else if (eff == MD_EFF_GLOBAL_FSLIDE_DOWN)
            tick_gl_fslide += val;
          else if (eff == MD_EFF_EXTENDED2) {
            uint8_t ext_t = val >> 4;
            uint8_t ext_v = val & 0x0F;
            if (ext_t == MD_EX2_GL_VOL_SLIDE_UP)
              tick_gl_volslide += ext_v;
            else if (ext_t == MD_EX2_GL_VOL_SLIDE_DN)
              tick_gl_volslide -= ext_v;
            else if (ext_t == MD_EX2_GL_VOL_SLIDE_UP_F && current_tick == 0)
              tick_gl_volslide += ext_v;
            else if (ext_t == MD_EX2_GL_VOL_SLIDE_DN_F && current_tick == 0)
              tick_gl_volslide -= ext_v;
            else if (ext_t == MD_EX2_GL_VOL_SLIDE_UP_XF) {
              // Extra Fine: applies every 4 ticks or similar?
              // Actually, in AT2 XF is / 4 per tick.
              if ((current_tick & 3) == 0)
                tick_gl_volslide += ext_v;
            } else if (ext_t == MD_EX2_GL_VOL_SLIDE_DN_XF) {
              if ((current_tick & 3) == 0)
                tick_gl_volslide -= ext_v;
            }
          }
        }
      }

      // Apply accumulated global slides
      if (tick_gl_volslide != 0) {
        int new_vol = (int)replay_global_vol + tick_gl_volslide;
        if (new_vol > 63)
          new_vol = 63;
        if (new_vol < 0)
          new_vol = 0;
        replay_global_vol = (uint8_t)new_vol;
      }
      if (tick_gl_fslide != 0) {
        int new_tempo = (int)replay_tempo + tick_gl_fslide;
        if (new_tempo < 18)
          new_tempo = 18;
        if (new_tempo > 255)
          new_tempo = 255;
        replay_tempo = (uint8_t)new_tempo;
        // Update irq timings
        int irq_freq = replay_tempo * (current_module->macro_speedup > 0
                                           ? current_module->macro_speedup
                                           : 1);
        if (irq_freq == 0)
          irq_freq = 50;
        samples_per_tick = (int)((double)replay_sample_rate / ((double)irq_freq * md_finetune_scale()));
        if (samples_per_tick < 1) samples_per_tick = 1;
      }

      for (int c = 0; c < current_module->num_channels; ++c) {
        // Les QUATRE emplacements : 0-1 pour la phrase, 2-3 pour les deux
        // colonnes CMD de la table. Les oublier revenait à ignorer toutes les
        // commandes posées dans une table.
        for (int e = 0; e < MD_EFF_SLOTS; ++e) {
          uint8_t eff = ch_active_eff[c][e];
          if (eff == 0xFF)
            continue;

          // ── Décomposition des effets COMBINÉS AT2 ──────────────────────────
          // Un combo « X + volume slide » fait tourner DEUX comportements : le
          // volume slide (nibbles du paramètre) et X (freq slide / arpège /
          // porta / vibrato, dont la vitesse vient de la MÉMOIRE, pas du param).
          //   • vsl_cont / vsl_fine : volume slide continu (tick>0) ou fin (tick0)
          //   • fsl_*_cont / fsl_*_fine : freq slide montant/descendant, continu/fin
          bool vsl_cont = (eff == MD_EFF_VOL_SLIDE || eff == MD_EFF_VIBRATO_VSLIDE ||
                           eff == MD_EFF_TPORTA_VSLIDE || eff == MD_EFF_ARPEGGIO_VSLIDE ||
                           eff == MD_EFF_FSLIDE_UP_VSLIDE || eff == MD_EFF_FSLIDE_DOWN_VSLIDE ||
                           eff == MD_EFF_FSLIDE_UP_FINE_VSLIDE ||
                           eff == MD_EFF_FSLIDE_DOWN_FINE_VSLIDE);
          bool vsl_fine = (eff == MD_EFF_VOL_SLIDE_FINE || eff == MD_EFF_ARPEGGIO_VSLIDE_FINE ||
                           eff == MD_EFF_TPORTA_VSLIDE_FINE || eff == MD_EFF_VIBRATO_VSLIDE_FINE ||
                           eff == MD_EFF_FSLIDE_UP_VSLF || eff == MD_EFF_FSLIDE_DOWN_VSLF ||
                           eff == MD_EFF_FSLIDE_UP_FINE_VSLF || eff == MD_EFF_FSLIDE_DOWN_FINE_VSLF);
          bool fsl_up_cont = (eff == MD_EFF_FSLIDE_UP || eff == MD_EFF_FSLIDE_UP_VSLIDE ||
                              eff == MD_EFF_FSLIDE_UP_VSLF);
          bool fsl_dn_cont = (eff == MD_EFF_FSLIDE_DOWN || eff == MD_EFF_FSLIDE_DOWN_VSLIDE ||
                              eff == MD_EFF_FSLIDE_DOWN_VSLF);
          bool fsl_up_fine = (eff == MD_EFF_FSLIDE_UP_FINE || eff == MD_EFF_FSLIDE_UP_FINE_VSLIDE ||
                              eff == MD_EFF_FSLIDE_UP_FINE_VSLF);
          bool fsl_dn_fine = (eff == MD_EFF_FSLIDE_DOWN_FINE || eff == MD_EFF_FSLIDE_DOWN_FINE_VSLIDE ||
                              eff == MD_EFF_FSLIDE_DOWN_FINE_VSLF);

          if (eff == MD_EFF_RETRIG_NOTE) {
            uint8_t speed = ch_retrig_speed[c];
            if (speed > 0) {
              ch_retrig_count[c]++;
              if (ch_retrig_count[c] >= speed) {
                ch_retrig_count[c] = 0;
                md_key_off(c);
                md_key_on(c);
              }
            }
          }

          // Multi retrigger note (effet 26) — spec AdlibTracker2 ef_MultiRetrigNote :
          //   • intervalle de retrig = nibble HAUT du paramètre
          //   • opération de volume  = nibble BAS (table 0..15)
          // Implémentation maison (aucune ligne copiée d'AT2). volC&0x3F est une
          // ATTÉNUATION (0 = fort, 63 = silence) → on travaille en "loudness"
          // (loud = 63 - atténuation) pour appliquer +/- et les ratios.
          if (eff == MD_EFF_MULTI_RETRIG_NOTE) {
            uint8_t param = ch_active_eff_val[c][e];
            uint8_t interval = param >> 4;
            uint8_t vol_op = param & 0x0F;
            if (interval > 0) {
              ch_retrig_count[c]++;
              if (ch_retrig_count[c] >= interval) {
                ch_retrig_count[c] = 0;

                // Applique l'opération de volume sur la porteuse.
                int loud = 63 - (ch_fmpar[c].volC & 0x3F);
                switch (vol_op) {
                  case 1:  loud -= 1;  break;
                  case 2:  loud -= 2;  break;
                  case 3:  loud -= 4;  break;
                  case 4:  loud -= 8;  break;
                  case 5:  loud -= 16; break;
                  case 9:  loud += 1;  break;
                  case 10: loud += 2;  break;
                  case 11: loud += 4;  break;
                  case 12: loud += 8;  break;
                  case 13: loud += 16; break;
                  case 6:  loud = (loud * 2) / 3; break;
                  case 7:  loud = loud / 2;       break;
                  case 14: loud = (loud * 3) / 2; break;
                  case 15: loud = loud * 2;       break;
                  default: break; // 0 et 8 : pas de changement
                }
                if (loud < 0) loud = 0;
                if (loud > 63) loud = 63;
                ch_fmpar[c].volC = (ch_fmpar[c].volC & 0xC0) | (uint8_t)(63 - loud);

                // Réécrit le volume, puis retrigger (key off → key on).
                md_write_volume(c);
                md_key_off(c);
                md_key_on(c);
              }
            }
          }

          if (eff == MD_EFF_EXTENDED2) {
            uint8_t ext_type = ch_active_eff_val[c][e] >> 4;
            uint8_t ext_val = ch_active_eff_val[c][e] & 0x0F;
            if (ext_type == MD_EX2_NOTE_CUT && current_tick == ext_val) {
              md_key_off(c); // note cut
            }
            if (ext_type == MD_EX2_NOTE_DELAY && current_tick == ext_val) {
              md_event_t ev_tick = md_channel_event(c);
              if (ev_tick.note > 0 && ev_tick.note <= MD_MAX_NOTE) {
                md_play_note(c, ev_tick.note);
              }
            }
          }

          if (vsl_cont || vsl_fine) {
            uint8_t val = ch_volslide_val[c];
            uint8_t up = val >> 4;
            uint8_t down = val & 0x0F;

            // Fine Volume Slide ONLY on tick 0, Standard Volume Slide ONLY on
            // tick > 0
            bool should_process = vsl_fine ? (current_tick == 0)
                                           : (current_tick > 0);

            if (!should_process) {
              up = 0;
              down = 0;
            }

            uint8_t current_vol = ch_fmpar[c].volC & 0x3F;
            if (up > 0) {
              if (current_vol >= up)
                current_vol -= up;
              else
                current_vol = 0;
            } else if (down > 0) {
              current_vol += down;
              if (current_vol > 63)
                current_vol = 63;
            }
            ch_fmpar[c].volC = (ch_fmpar[c].volC & 0xC0) | current_vol;

            md_write_volume(c);
          }

          if (fsl_up_cont || fsl_dn_cont || fsl_up_fine || fsl_dn_fine) {
            uint8_t speed = ch_fslide_speed[c];
            bool is_up = fsl_up_cont || fsl_up_fine;
            bool is_fine = fsl_up_fine || fsl_dn_fine;

            // Fine slides ONLY on tick 0, Standard slides ONLY on tick > 0
            bool should_process = is_fine ? (current_tick == 0)
                                          : (current_tick > 0);

            if (should_process) {
              // The F-Slide speed parameter in AT2 is added exactly linearly
              // per tick
              int16_t scaled_speed = speed;

              uint16_t current_fnum = ch_base_fnum[c];
              if (is_up) {
                // AT2 UP means increasing frequency number
                current_fnum = md_slide_frequency(current_fnum, scaled_speed);
              } else {
                // AT2 DOWN means decreasing frequency number
                current_fnum =
                    md_slide_frequency(current_fnum, -scaled_speed);
              }
              ch_base_fnum[c] = current_fnum;
            }
            uint16_t out_fnum = ch_base_fnum[c];
            // potential modification

            md_write_freq(c, out_fnum);
          }

          if (eff == MD_EFF_TONE_PORTAMENTO || eff == MD_EFF_TPORTA_VSLIDE ||
              eff == MD_EFF_TPORTA_VSLIDE_FINE) {
            uint8_t speed = ch_porta_speed[c];
            uint16_t current_fnum = ch_base_fnum[c];
            uint16_t target = ch_porta_target[c];

            // AT2 slides use absolute shift comparisons natively supported by
            // `md_slide_frequency`
            int16_t scaled_speed = speed;

            if (current_fnum > target) {
              // Current Pitch > Target Pitch, go UP (negative speed for F-num)
              current_fnum = md_slide_frequency(current_fnum, -scaled_speed);
              if (current_fnum < target)
                current_fnum = target;
            } else if (current_fnum < target) {
              // Current Pitch < Target Pitch, go DOWN (positive speed for
              // F-num)
              current_fnum = md_slide_frequency(current_fnum, scaled_speed);
              if (current_fnum > target)
                current_fnum = target;
            }
            ch_base_fnum[c] = current_fnum;

            uint16_t out_fnum = current_fnum;

            md_write_freq(c, out_fnum);
          }

          if (eff == MD_EFF_VIBRATO || eff == MD_EFF_VIBRATO_VSLIDE ||
              eff == MD_EFF_VIBRATO_VSLIDE_FINE) {
            uint8_t speed = ch_vibrato_speed[c];
            uint8_t depth = ch_vibrato_depth[c];
            ch_vibrato_pos[c] += speed;

            uint8_t pos = ch_vibrato_pos[c];
            uint8_t table_val = vibtrem_table[pos & 31];
            uint8_t dir = (pos & 32) ? 1 : 0;

            uint16_t shift = (depth * table_val) >> 7;

            uint16_t fnum = ch_base_fnum[c];
            if (dir == 0) {
              fnum = md_slide_frequency(fnum, -(int16_t)shift);
            } else {
              fnum = md_slide_frequency(fnum, (int16_t)shift);
            }

            md_write_freq(c, fnum);
          }

          if (eff == MD_EFF_TREMOLO) {
            uint8_t speed = ch_tremolo_speed[c];
            uint8_t depth = ch_tremolo_depth[c];
            ch_tremolo_pos[c] += speed;

            uint8_t pos = ch_tremolo_pos[c];
            uint8_t table_val = vibtrem_table[pos & 31];
            uint8_t dir = (pos & 32) ? 1 : 0;

            uint16_t shift = (depth * table_val) >> 7;

            uint8_t current_vol = ch_fmpar[c].volC & 0x3F;
            if (dir == 0) { // Vol down -> Attenuation UP
              current_vol += shift;
              if (current_vol > 63)
                current_vol = 63;
            } else {
              if (current_vol >= shift)
                current_vol -= shift;
              else
                current_vol = 0;
            }

            md_write_carrier_volume(c, current_vol);
          }

          if (eff == MD_EFF_TREMOR) {
            uint8_t val = ch_tremor_val[c];
            uint8_t on_time = val >> 4;
            uint8_t off_time = val & 0x0F;

            if (on_time > 0 && off_time > 0) {
              ch_tremor_count[c]++;
              if (ch_tremor_count[c] >= on_time + off_time)
                ch_tremor_count[c] = 0;

              if (ch_tremor_count[c] >= on_time) {
                md_write_carrier_volume(c, 63); // phase OFF : silence
              } else {
                md_write_volume(c);             // phase ON : volume normal
              }
            }
          }

          if (eff == MD_EFF_ARPEGGIO || eff == MD_EFF_ARPEGGIO_VSLIDE ||
              eff == MD_EFF_ARPEGGIO_VSLIDE_FINE) {
            apply_arpeggio_tick(c);
          }
        }
      }
    }
  }

  // Process instrument macros every tick
  md_psg_env_tick();

  replay_ticks_left--;
}

bool md_replayer_play() {
  md_lock(&replayer_lock);

  if (!current_module) {
    md_unlock(&replayer_lock);
    return false;
  }
  // Chaque canal repart de la ligne de song demandée (F5 = 0, F8 = curseur),
  // puis suit SA propre colonne.
  memset(ch_table_forced, 0, sizeof(ch_table_forced));
  for (int c = 0; c < MD_MAX_CHANNELS; c++) {
    ch_song_row[c] = song_start_row;
    ch_chain[c] = -1;
    ch_chain_row[c] = 0;
    ch_phrase[c] = -1;
    ch_phrase_row[c] = 0;
    ch_row_transpose[c] = 0;
    ch_pending_row[c] = -1;

    if (play_scope != MD_SCOPE_SONG) {
      // Lecture limitée : seule la colonne du curseur sonne, et le mode LIVE
      // ne s'applique pas — on veut entendre immédiatement ce qu'on édite.
      if (c != scope_channel) {
        ch_running[c] = false;
        continue;
      }
      ch_running[c] = true;
      if (play_scope == MD_SCOPE_PHRASE) {
        ch_phrase[c] = scope_id;
        ch_row_transpose[c] = 0;
      } else {
        ch_chain[c] = scope_id;
        ch_chain_row[c] = (scope_start_row >= 0 &&
                           scope_start_row < MD_ROWS_PER_CHAIN)
                              ? scope_start_row : 0;
        md_chain_seek_in_scope(c);
      }
      continue;
    }

    // En LIVE, rien ne démarre tout seul : chaque colonne est lancée à la main.
    ch_running[c] = !live_mode;
    if (!ch_running[c]) continue;

    // La lecture ENTRE sur la ligne du curseur, mais son bloc — donc sa boucle
    // — commence en haut du bloc contigu qui la contient. Et si la case du
    // curseur est vide pour ce canal, ce canal-là ne joue rien du tout.
    ch_block_top[c] = md_block_top_row(c, song_start_row);
    if (song_start_row < 0 || song_start_row >= MD_SONG_ROWS ||
        current_module->song[c][song_start_row] == MD_EMPTY) {
      ch_running[c] = false;
      ch_phrase[c] = -1;
      continue;
    }
    md_channel_seek(c);
  }
  {
    // Trace de démarrage : d'où part chaque colonne, et sur quel bloc.
    MD_TRACE("[PLAY] moteur: portee=%d song_start_row=%d\n", play_scope, song_start_row);
    for (int c = 0; c < current_module->num_channels; c++)
      MD_TRACE("[PLAY]   canal %d : case=%02X hautDeBloc=%d ligne=%d chaine=%d "
             "phrase=%d actif=%d\n",
             c,
             (song_start_row >= 0 && song_start_row < MD_SONG_ROWS)
                 ? current_module->song[c][song_start_row] : 0xFF,
             ch_block_top[c], ch_song_row[c], ch_chain[c], ch_phrase[c],
             ch_running[c] ? 1 : 0);
    fflush(stdout);
  }
  md_current_row = 0;

  replay_speed = current_module->initial_speed;
  replay_tempo = current_module->initial_tempo;
  if (replay_tempo < 18)
    replay_tempo = 18;

  int irq_freq = replay_tempo * current_module->macro_speedup;
  if (irq_freq == 0)
    irq_freq = 50; // Fallback

  samples_per_tick = (int)((double)replay_sample_rate / ((double)irq_freq * md_finetune_scale()));
        if (samples_per_tick < 1) samples_per_tick = 1;
  if (samples_per_tick < 1) samples_per_tick = 1;
  samples_until_next_tick = 0;
  replay_ticks_left = 0; // Trigger row immediately

  play_status = MD_PLAYING;

  // Remise à zéro complète des deux puces
  md_chip_reset(replay_sample_rate);
  memset(psg_env, 0, sizeof(psg_env));

  memset(ch_b0_reg, 0, sizeof(ch_b0_reg));
  memset(ch_panning, 0, sizeof(ch_panning));

  // Reset persistent effect states so they don't leak into new playbacks
  memset(ch_fslide_speed, 0, sizeof(ch_fslide_speed));
  memset(ch_porta_speed, 0, sizeof(ch_porta_speed));
  memset(ch_porta_target, 0, sizeof(ch_porta_target));
  memset(ch_vibrato_speed, 0, sizeof(ch_vibrato_speed));
  memset(ch_vibrato_depth, 0, sizeof(ch_vibrato_depth));
  memset(ch_vibrato_pos, 0, sizeof(ch_vibrato_pos));
  memset(ch_arpeggio_val, 0, sizeof(ch_arpeggio_val));
  memset(ch_arpeggio_state, 0, sizeof(ch_arpeggio_state));
  memset(ch_volslide_val, 0, sizeof(ch_volslide_val));
  memset(ch_active_eff_val, 0, sizeof(ch_active_eff_val));
  memset(ch_active_eff, 0xFF, sizeof(ch_active_eff));
  memset(ch_mdcmd_on, 0, sizeof(ch_mdcmd_on));
  // Le volume GLOBAL revient au maximum. Il n'était remis qu'au démarrage de
  // l'application : une commande M survolée en éditant — M 00 met tout à zéro —
  // assourdissait donc TOUTES les voies, et aucun stop/play n'y pouvait rien.
  replay_global_vol = 63;
  memset(ch_instr_blocked, 0, sizeof(ch_instr_blocked));
  memset(ch_mdcmd_val, 0, sizeof(ch_mdcmd_val));

  // Le volume du canal fait partie de cet état-là. Sans cette ligne, un
  // glissando de volume descendu jusqu'au silence n'était JAMAIS relevé :
  // ni par un stop, ni par un nouveau morceau, ni même par un redémarrage du
  // moteur — ch_fmpar n'était remis à zéro nulle part. Le tracker restait
  // muet jusqu'à la fermeture de l'application.
  memset(ch_fmpar, 0, sizeof(ch_fmpar));

  global_volslide_val = 0;
  global_fslide_speed = 0;

  md_unlock(&replayer_lock);
  return true;
}

void md_replayer_stop() {
  md_chip_pcm_stop();
  play_status = MD_STOPPED;
  md_current_row = 0;
  for (int c = 0; c < MD_MAX_CHANNELS; c++) {
    ch_running[c] = false;
    ch_phrase[c] = -1;
    ch_chain[c] = -1;
    ch_pending_row[c] = -1;
  }

  // Key-off en douceur sur toutes les voies (y compris l'audition) pour que
  // les queues de release puissent s'éteindre naturellement.
  for (int c = 0; c < MD_TOTAL_VOICES; c++) {
    md_key_off(c);
  }
}

void md_replayer_update(uint8_t *stream, int length_bytes) {
  // Try to acquire lock. If the UI thread is currently mutating the module/opl
  // state (e.g. md_chip_reset or replacing the module), output pure silence to
  // avoid EXC_BAD_ACCESS!
  if (!md_trylock(&replayer_lock)) {
    memset(stream, 0, length_bytes);
    return;
  }

  int16_t *buffer = (int16_t *)stream;
  int samples_to_generate =
      length_bytes / 4; // 16-bit stereo (4 bytes per sample frame)

  while (samples_to_generate > 0) {
    if (play_status == MD_PLAYING) {
      if (samples_until_next_tick <= 0) {
        md_process_tick();
        samples_until_next_tick = samples_per_tick;
      }

      int chunk_samples = samples_to_generate;
      if (chunk_samples > samples_until_next_tick) {
        chunk_samples = samples_until_next_tick;
      }
      // Garde-fou : si le nombre d'échantillons par tic tombait à zéro — un
      // tempo aberrant, une division qui déraille — ce bloc consommerait zéro
      // échantillon et la boucle tournerait POUR TOUJOURS, moteur audio bloqué.
      // C'est ce qui figeait le tracker sur Nintendo DS. Mieux vaut un morceau
      // qui joue trop vite qu'une machine qui ne répond plus.
      if (chunk_samples <= 0) chunk_samples = 1;

      md_chip_generate(buffer, chunk_samples);

      buffer += chunk_samples * 2; // Stereo 16-bit
      samples_to_generate -= chunk_samples;
      samples_until_next_tick -= chunk_samples;
    } else {
      // Free playing without sequencer — still need to process macros
      // for test/piano notes. Use default tick rate based on module tempo.
      if (current_module && samples_per_tick > 0) {
        while (samples_to_generate > 0) {
          if (samples_until_next_tick <= 0) {
            md_psg_env_tick();
            // Les tables d'instrument tournent aussi hors lecture : c'est ce
            // qui permet d'entendre la table qu'on édite en jouant une note.
            for (int c = 0; c < MD_TOTAL_VOICES; c++)
              md_table_tick(c);
            // Preview arpeggio: drive the 0xy effect on the piano's test
            // channel so an auditioned note sounds exactly like the command.
            if (test_arp_channel >= 0)
              apply_arpeggio_tick(test_arp_channel);
            samples_until_next_tick = samples_per_tick;
          }
          int chunk = samples_to_generate;
          if (chunk > samples_until_next_tick)
            chunk = samples_until_next_tick;
          md_chip_generate(buffer, chunk);
          buffer += chunk * 2;
          samples_to_generate -= chunk;
          samples_until_next_tick -= chunk;
        }
      } else {
        md_chip_generate(buffer, samples_to_generate);
      }
      break;
    }
  }

  md_unlock(&replayer_lock);
}

    const char *md_replayer_get_instr_name(int index) {
      if (!current_module || index < 1 || index > MD_MAX_INSTRUMENTS)
        return "";
      return current_module->instr_names[index - 1];
    }

    void md_replayer_set_instr_name(int index, const char *name) {
      if (!current_module || index < 1 || index > MD_MAX_INSTRUMENTS || !name)
        return;
      strncpy(current_module->instr_names[index - 1], name, 42);
      current_module->instr_names[index - 1][42] = '\0';
    }

    // Recopie un instrument dans un autre emplacement, réglages ET nom. Sert au
    // clonage profond (SELECT+B+A) : dupliquer un motif n'a d'intérêt que si les
    // instruments qu'il emploie sont dupliqués aussi, sinon on retouche l'original.
    void md_replayer_copy_instrument(int src, int dst) {
      if (!current_module || src < 1 || src > MD_MAX_INSTRUMENTS ||
          dst < 1 || dst > MD_MAX_INSTRUMENTS || src == dst)
        return;
      current_module->instruments[dst - 1] = current_module->instruments[src - 1];
      memcpy(current_module->instr_names[dst - 1],
             current_module->instr_names[src - 1],
             sizeof(current_module->instr_names[0]));
    }

    md_module_t *md_replayer_get_module(void) { return current_module; }

    // ── Accès SONG / CHAIN / PHRASE pour l'interface ────────────────────────

    uint8_t md_replayer_get_song(int channel, int row) {
      if (!current_module || channel < 0 || channel >= MD_MAX_CHANNELS ||
          row < 0 || row >= MD_SONG_ROWS)
        return MD_EMPTY;
      return current_module->song[channel][row];
    }

    void md_replayer_set_song(int channel, int row, uint8_t chain) {
      if (!current_module || channel < 0 || channel >= MD_MAX_CHANNELS ||
          row < 0 || row >= MD_SONG_ROWS)
        return;
      if (chain != MD_EMPTY && chain >= MD_MAX_CHAINS)
        return;
      current_module->song[channel][row] = chain;
    }

    void md_replayer_get_chain(int chain, int row, uint8_t *phrase,
                               int8_t *transpose) {
      if (phrase) *phrase = MD_EMPTY;
      if (transpose) *transpose = 0;
      if (!current_module || chain < 0 || chain >= MD_MAX_CHAINS ||
          row < 0 || row >= MD_ROWS_PER_CHAIN)
        return;
      if (phrase) *phrase = current_module->chains[chain].rows[row].phrase;
      if (transpose) *transpose = current_module->chains[chain].rows[row].transpose;
    }

    void md_replayer_set_chain(int chain, int row, uint8_t phrase,
                               int8_t transpose) {
      if (!current_module || chain < 0 || chain >= MD_MAX_CHAINS ||
          row < 0 || row >= MD_ROWS_PER_CHAIN)
        return;
      if (phrase != MD_EMPTY && phrase >= MD_MAX_PHRASES)
        return;
      current_module->chains[chain].rows[row].phrase = phrase;
      current_module->chains[chain].rows[row].transpose = transpose;
    }

    void md_replayer_get_phrase(int phrase, int row, uint8_t *note,
                                uint8_t *instr, uint8_t *vel, uint8_t *cmd,
                                uint8_t *cmdval, uint8_t *mdcmd,
                                uint8_t *mdval) {
      if (note) *note = 0;
      if (instr) *instr = 0;
      if (vel) *vel = MD_EMPTY;
      if (cmd) *cmd = MD_EMPTY;
      if (cmdval) *cmdval = 0;
      if (mdcmd) *mdcmd = MD_EMPTY;
      if (mdval) *mdval = 0;
      if (!current_module || phrase < 0 || phrase >= MD_MAX_PHRASES ||
          row < 0 || row >= MD_ROWS_PER_PHRASE)
        return;
      const md_phrase_row_t *pr = &current_module->phrases[phrase].rows[row];
      if (note) *note = pr->note;
      if (instr) *instr = pr->instr;
      if (vel) *vel = pr->vel;
      if (cmd) *cmd = pr->cmd;
      if (cmdval) *cmdval = pr->cmdval;
      if (mdcmd) *mdcmd = pr->mdcmd;
      if (mdval) *mdval = pr->mdval;
    }

    void md_replayer_set_phrase(int phrase, int row, uint8_t note, uint8_t instr,
                                uint8_t vel, uint8_t cmd, uint8_t cmdval,
                                uint8_t mdcmd, uint8_t mdval) {
      if (!current_module || phrase < 0 || phrase >= MD_MAX_PHRASES ||
          row < 0 || row >= MD_ROWS_PER_PHRASE)
        return;
      md_phrase_row_t *pr = &current_module->phrases[phrase].rows[row];
      pr->note = note;
      pr->instr = instr;
      pr->vel = vel;
      pr->cmd = cmd;
      pr->cmdval = cmdval;
      pr->mdcmd = mdcmd;
      pr->mdval = mdval;
    }

    // ── Têtes de lecture ────────────────────────────────────────────────────

    static bool md_channel_live(int c) {
      return play_status == MD_PLAYING && c >= 0 && c < MD_MAX_CHANNELS &&
             ch_running[c];
    }
    int md_replayer_play_song_row(int c) {
      return md_channel_live(c) ? ch_song_row[c] : -1;
    }
    int md_replayer_play_chain(int c) {
      return md_channel_live(c) ? ch_chain[c] : -1;
    }
    int md_replayer_play_chain_row(int c) {
      return md_channel_live(c) ? ch_chain_row[c] : -1;
    }
    int md_replayer_play_phrase(int c) {
      return md_channel_live(c) ? ch_phrase[c] : -1;
    }
    int md_replayer_play_phrase_row(int c) {
      return md_channel_live(c) ? ch_phrase_row[c] : -1;
    }

    void md_replayer_set_live_mode(bool live) {
      live_mode = live;
      for (int c = 0; c < MD_MAX_CHANNELS; c++) ch_pending_row[c] = -1;
    }
    bool md_replayer_is_live_mode(void) { return live_mode; }

    void md_replayer_live_arm(int channel, int song_row) {
      if (channel < 0 || channel >= MD_MAX_CHANNELS) return;
      if (song_row < 0) {                      // désarmer / couper la colonne
        ch_pending_row[channel] = -1;
        ch_running[channel] = false;
        ch_phrase[channel] = -1;
        md_key_off(channel);
        return;
      }
      if (song_row >= MD_SONG_ROWS) song_row = MD_SONG_ROWS - 1;
      ch_pending_row[channel] = song_row;
      // Si rien ne tourne encore, le canal démarre sans attendre de quantisation.
      if (play_status != MD_PLAYING) {
        ch_song_row[channel] = song_row;
        ch_pending_row[channel] = -1;
        ch_chain[channel] = -1; ch_chain_row[channel] = 0;
        ch_phrase[channel] = -1; ch_phrase_row[channel] = 0;
        ch_running[channel] = true;
        md_channel_seek(channel);
      }
    }

    int md_replayer_live_pending(int channel) {
      if (channel < 0 || channel >= MD_MAX_CHANNELS) return -1;
      return ch_pending_row[channel];
    }

    // Ligne du curseur dans l'écran CHAIN, d'où START doit partir.
void md_replayer_set_scope_row(int row) {
  scope_start_row = row;
}

void md_replayer_play_from(int song_row) {
  MD_TRACE("[PLAY] md_replayer_play_from(%d)\n", song_row);
      if (song_row < 0) song_row = 0;
      if (song_row >= MD_SONG_ROWS) song_row = MD_SONG_ROWS - 1;
      song_start_row = song_row;
      // Les boucles H des phrases repartent a zero : sans ca, une boucle deja
      // consommee au passage precedent ne rejouerait pas.
      for (int c = 0; c < MD_TOTAL_VOICES; c++)
        md_replayer_reset_phrase_hops(c);
    }

    bool md_replayer_is_playing() { return play_status == MD_PLAYING; }

    // Instrument YM2612 par défaut : algorithme 4 (deux piles de 2 opérateurs
    // en parallèle), feedback moyen. Les quatre opérateurs ont une enveloppe
    // active pour que TOUTE modification faite dans l'éditeur s'entende tout de
    // suite.
    //
    // ⚠️ POINT IMPORTANT — pourquoi D2R (« sustain rate ») n'est PAS à zéro :
    // sur OPL3, le bit EG-TYP de chaque opérateur permet une enveloppe dite
    // « percussive » : la note redescend toute seule jusqu'au silence même si
    // la touche reste enfoncée. C'est ce que fait l'instrument par défaut du
    // tracker OPL3, et c'est pour ça qu'on n'y écrit jamais de note-off.
    // Le YM2612 n'a pas ce bit, mais il a D2R : une fois le niveau de maintien
    // (D1L) atteint, l'enveloppe continue de descendre à la vitesse D2R, touche
    // toujours enfoncée. D2R = 0 → la note tient indéfiniment (il faut alors un
    // note-off) ; D2R > 0 → elle s'éteint seule. On part donc d'un D2R non nul,
    // pour retrouver exactement le confort d'écriture d'OPL3-Tracker.
    // (Mesuré : D2R 14 ≈ 1,9 s d'extinction, D2R 18 ≈ 1,4 s, D2R 9 ≈ 5,4 s.)
    void md_replayer_init_default_instrument(int index) {
      if (!current_module || index < 0 || index >= MD_MAX_INSTRUMENTS)
        return;

      md_instr_t *ins = &current_module->instruments[index];
      memset(ins, 0, sizeof(*ins));
      ins->pcm_volume = 127;            // un échantillon neuf sort à plein
      // AUCUN échantillon choisi. Le zéro désignait le PREMIER de la banque :
      // un instrument PCM neuf naissait donc avec le son du précédent — le kick
      // déjà chargé — au lieu d'être vierge.
      ins->pcm_sample = MD_EMPTY;

      // OP1 — modulateur de la première pile.
      // Son niveau est volontairement assez haut : sur le YM2612 le FEEDBACK
      // n'agit QUE sur OP1, et son intensité est proportionnelle au niveau de
      // sortie d'OP1. Avec un OP1 trop atténué, bouger le fader Feedback ne
      // s'entend pas. (Mesuré : TL 28 → 49 % d'écart entre feedback 0 et 7 ;
      // TL 22 → 104 %.)
      ins->op[0] = (md_op_params_t){.detune = 0,
                                    .multiple = 1,
                                    .total_level = 22,
                                    .key_scale = 0,
                                    .attack = 31,
                                    .decay = 8,
                                    .sustain_rate = 12,
                                    .sustain_level = 2,
                                    .release = 8,
                                    .am_enable = 0,
                                    .ssg_eg = 0};
      // OP2 — porteuse de la première pile
      ins->op[1] = (md_op_params_t){.detune = 0,
                                    .multiple = 1,
                                    .total_level = 4,
                                    .key_scale = 0,
                                    .attack = 31,
                                    .decay = 6,
                                    .sustain_rate = 14,
                                    .sustain_level = 2,
                                    .release = 8,
                                    .am_enable = 0,
                                    .ssg_eg = 0};
      // OP3 — modulateur de la seconde pile (légèrement désaccordé)
      ins->op[2] = (md_op_params_t){.detune = 1,
                                    .multiple = 2,
                                    .total_level = 30,
                                    .key_scale = 0,
                                    .attack = 31,
                                    .decay = 8,
                                    .sustain_rate = 12,
                                    .sustain_level = 2,
                                    .release = 8,
                                    .am_enable = 0,
                                    .ssg_eg = 0};
      // OP4 — porteuse de la seconde pile
      ins->op[3] = (md_op_params_t){.detune = 0,
                                    .multiple = 1,
                                    .total_level = 4,
                                    .key_scale = 0,
                                    .attack = 31,
                                    .decay = 6,
                                    .sustain_rate = 14,
                                    .sustain_level = 2,
                                    .release = 8,
                                    .am_enable = 0,
                                    .ssg_eg = 0};

      ins->algorithm = 4;
      ins->feedback = 4;
      ins->ams = 0;
      ins->pms = 0;
      ins->lfo_enable = 0;
      ins->lfo_freq = 0;
      ins->panning = 0; // centre
      ins->fine_tune = 0;
      // Bruit blanc « verrouillé sur le ton 3 » (mode 7). C'est le SEUL mode du
      // SN76489 où la hauteur du bruit suit la note : les trois autres périodes
      // sont fixes, et toutes les notes y sonnent identiques. Le générateur de
      // bruit est alors cadencé par le registre de période de la voie PSG3.
      ins->psg_noise = 0x07;

      // Enveloppe PSG par défaut : attaque immédiate au maximum, léger déclin
      // vers un maintien nul — la note s'éteint donc toute seule, sans qu'on
      // ait à semer des note-off dans les phrases.
      // ENV. D8 / 00 / -- : on part fort et on descend vers le silence à
      // vitesse moyenne, puis on y reste. La note s'éteint donc toute seule,
      // sans qu'on ait à semer des note-off dans les phrases.
      ins->env_amp[0] = 0xD; ins->env_speed[0] = 8;
      ins->env_amp[1] = 0;   ins->env_speed[1] = 0;
      ins->env_amp[2] = MD_EMPTY; ins->env_speed[2] = 0;

      // Aucune table attachée : 0 est un numéro de table VALIDE, l'absence se
      // note MD_EMPTY. Le memset ci-dessus aurait sinon collé la table 00 à
      // tous les instruments.
      ins->table = MD_EMPTY;

      snprintf(current_module->instr_names[index], 43, "Instrument %02d",
               index + 1);
    }

    void md_replayer_new_empty() {
      md_lock(&replayer_lock);

      md_replayer_shut();
      current_module = (md_module_t *)calloc(1, sizeof(md_module_t));
      if (current_module) {
        current_module->num_channels = MD_MAX_CHANNELS;
        current_module->initial_tempo = 125;
        current_module->initial_speed = 6;
        current_module->macro_speedup = 1;
        current_module->bpm_rows_per_beat = 4;
        current_module->bpm_tempo_finetune = 0;

        // Morceau vierge : toutes les cases du SONG et tous les chains/phrases
        // sont marqués « vide » (0xFF), pas zéro — 00 est un numéro valide.
        memset(current_module->song, MD_EMPTY, sizeof(current_module->song));
        for (int i = 0; i < MD_MAX_CHAINS; i++)
          for (int r = 0; r < MD_ROWS_PER_CHAIN; r++) {
            current_module->chains[i].rows[r].phrase = MD_EMPTY;
            current_module->chains[i].rows[r].transpose = 0;
          }
        // Idem pour les tables : « VOL 0 » est le silence, pas l'absence de
        // consigne. Une table neuve doit donc être remplie de MD_EMPTY.
        for (int i = 0; i < MD_MAX_TABLES; i++)
          for (int r = 0; r < MD_TABLE_ROWS; r++) {
            md_table_row_t *tr = &current_module->tables[i].rows[r];
            tr->vol = 0;          // 0 = ne touche pas au volume
            tr->transpose = 0;
            tr->cmd1 = MD_EMPTY;
            tr->val1 = 0;
            tr->cmd2 = MD_EMPTY;
            tr->val2 = 0;
            tr->mdcmd = MD_EMPTY;
            tr->mdval = 0;
          }

        for (int i = 0; i < MD_MAX_PHRASES; i++)
          for (int r = 0; r < MD_ROWS_PER_PHRASE; r++) {
            md_phrase_row_t *pr = &current_module->phrases[i].rows[r];
            pr->note = 0; pr->instr = 0;
            pr->vel = MD_EMPTY; pr->cmd = MD_EMPTY; pr->cmdval = 0;
            // Sans cette ligne, mdcmd restait à 0 — l'indice de la PREMIÈRE
            // commande, pas « vide ». Chaque ligne portait donc un « A 00 »
            // fantôme qui forçait l'algorithme 0 à chaque note et écrasait
            // celui de l'instrument.
            pr->mdcmd = MD_EMPTY; pr->mdval = 0;
          }
        song_start_row = 0;

        // Remet les deux puces dans un état propre.
        md_chip_reset(replay_sample_rate);
        memset(psg_env, 0, sizeof(psg_env));
        for (int c = 0; c < MD_MAX_CHANNELS; c++)
          ch_hw_voice[c] = (uint8_t)c;
        ch_hw_voice[MD_TEST_CHANNEL] = MD_NUM_FM_CHANNELS - 1;

        memset(ch_b0_reg, 0, sizeof(ch_b0_reg));
        memset(ch_current_instr, 0, sizeof(ch_current_instr));

        for (int i = 0; i < MD_MAX_INSTRUMENTS; i++)
          md_replayer_init_default_instrument(i);

        // Pré-affecte l'instrument 1 à la voie d'audition pour que les réglages
        // soient audibles avant même la première note jouée.
        ch_current_instr[MD_TEST_CHANNEL] = 1;
        md_set_instrument(MD_TEST_CHANNEL, &current_module->instruments[0]);
      }

      md_unlock(&replayer_lock);
    }

    int md_replayer_get_tempo() {
      return current_module ? current_module->initial_tempo : 125;
    }

    void md_replayer_set_tempo(int tempo) {
      if (tempo < 18)
        tempo = 18;
      if (current_module)
        current_module->initial_tempo = tempo;
      replay_tempo = tempo;
      // IMPORTANT : recalculer la cadence de tick, sinon la vitesse de lecture
      // ne change pas (samples_per_tick restait figé à l'ancien tempo).
      int macro = (current_module && current_module->macro_speedup > 0)
                      ? current_module->macro_speedup : 1;
      int irq_freq = replay_tempo * macro;
      if (irq_freq == 0)
        irq_freq = 50; // Fallback
      if (replay_sample_rate > 0)
        samples_per_tick = (int)((double)replay_sample_rate / ((double)irq_freq * md_finetune_scale()));
        if (samples_per_tick < 1) samples_per_tick = 1;
    }

    int md_replayer_get_speed() {
      return current_module ? current_module->initial_speed : 6;
    }

    int md_replayer_get_rows_per_beat() {
      // Sert au calcul du BPM réel (BPM = tempo*60/(speed*rows_per_beat)).
      if (!current_module || current_module->bpm_rows_per_beat == 0)
        return 4; // défaut AT2
      return current_module->bpm_rows_per_beat;
    }

    // BPM réel AT2 = tempo*60/(speed*rows_per_beat) * (base+finetune)/base.
    double md_replayer_get_bpm() {
      if (!current_module) return 125.0;
      int tempo = current_module->initial_tempo > 0 ? current_module->initial_tempo : 50;
      int speed = current_module->initial_speed > 0 ? current_module->initial_speed : 6;
      int rpb = md_replayer_get_rows_per_beat();
      double baseBPM = (double)tempo * 60.0 / ((double)speed * (double)rpb);
      return baseBPM * md_finetune_scale();
    }

    // Règle le BPM en ajustant le finetune (tempo/speed/rows_per_beat inchangés),
    // puis recalcule immédiatement la cadence de lecture.
    void md_replayer_set_bpm(double bpm) {
      if (!current_module) return;
      int tempo = current_module->initial_tempo > 0 ? current_module->initial_tempo : 50;
      int speed = current_module->initial_speed > 0 ? current_module->initial_speed : 6;
      int macro = current_module->macro_speedup > 0 ? current_module->macro_speedup : 1;
      int rpb = md_replayer_get_rows_per_beat();
      int base = at2_base_irq_freq(tempo, macro);
      double baseBPM = (double)tempo * 60.0 / ((double)speed * (double)rpb);
      if (baseBPM < 0.001) return;
      double ft = (double)base * (bpm / baseBPM - 1.0);
      int finetune = (int)(ft < 0 ? ft - 0.5 : ft + 0.5);
      if (base + finetune < 50) finetune = 50 - base;     // IRQ >= 50 Hz
      if (base + finetune > 1000) finetune = 1000 - base; // IRQ <= 1000 Hz
      current_module->bpm_tempo_finetune = (int16_t)finetune;
      int irq_freq = tempo * macro;
      if (irq_freq == 0) irq_freq = 50;
      if (replay_sample_rate > 0)
        samples_per_tick = (int)((double)replay_sample_rate / ((double)irq_freq * md_finetune_scale()));
        if (samples_per_tick < 1) samples_per_tick = 1;
      if (samples_per_tick < 1) samples_per_tick = 1;
    }

    // Lignes par temps : sert au calcul du BPM affiché. DefleMask range la
    // même idée dans son « surlignage A ».
    void md_replayer_set_rows_per_beat(int rpb) {
      if (!current_module) return;
      if (rpb < 1) rpb = 1;
      if (rpb > 32) rpb = 32;
      current_module->bpm_rows_per_beat = (uint8_t)rpb;
    }

    void md_replayer_set_speed(int speed) {
      if (current_module)
        current_module->initial_speed = speed;
      replay_speed = speed;
    }

    void md_replayer_play_test_note(int note, int instr, int channel) {
      if (!current_module || instr < 1 || instr > MD_MAX_INSTRUMENTS ||
          channel < 0 || channel >= MD_TOTAL_VOICES)
        return;
      // La voie d'audition vole une voie FM libre à chaque note.
      if (channel == MD_TEST_CHANNEL)
        md_assign_test_voice();
      ch_current_instr[channel] = instr;
      md_set_instrument((uint8_t)channel, &current_module->instruments[instr - 1]);
      md_play_note((uint8_t)channel, (uint8_t)note);
    }

    // Enable/disable the preview arpeggio on a test channel. `val` is the raw
    // 0xy param (high nibble = 1st semitone offset, low nibble = 2nd); 0 = off.
    void md_replayer_set_test_arpeggio(int channel, int val) {
      if (channel < 0 || channel >= MD_TOTAL_VOICES) {
        test_arp_channel = -1;
        return;
      }
      if (val == 0) {
        ch_arpeggio_val[channel] = 0;
        ch_arpeggio_state[channel] = 0;
        if (test_arp_channel == channel)
          test_arp_channel = -1;
        return;
      }
      ch_arpeggio_val[channel] = (uint8_t)(val & 0xFF);
      ch_arpeggio_state[channel] = 0;
      // Force the free-play loop to apply the arpeggio on its VERY NEXT audio
      // chunk (instead of waiting up to ~20 ms for the previous tick window to
      // expire). Without this, a fresh-project first note didn't audibly tick
      // the arpeggio before F5 had ever run.
      samples_until_next_tick = 0;
      test_arp_channel = channel;
    }

    void md_replayer_stop_test_note(int channel) {
      if (channel < 0 || channel >= MD_TOTAL_VOICES)
        return;
      // La voie d'audition PARTAGE son matériel avec une voie du morceau dès
      // que toutes les voies FM sont prises — elle en vole une. Envoyer un
      // key-off là-dessus pendant la lecture éteignait la note du MORCEAU, et
      // sur une note tenue elle ne revenait qu'au prochain déclenchement,
      // c'est-à-dire jamais. C'est ce qui coupait la FM en modifiant une note :
      // le relâchement du bouton A envoie ce key-off à chaque fois.
      if (channel == MD_TEST_CHANNEL) {
        uint8_t hw = ch_hw_voice[MD_TEST_CHANNEL];
        for (int c = 0; c < MD_MAX_CHANNELS; c++)
          if (ch_hw_voice[c] == hw && ch_running[c])
            return;                       // cette voix n'est pas la nôtre
      }
      // Key-OFF only: clear the key-on bit so the envelope enters its release
      // phase. We deliberately DO NOT cancel the preview arpeggio here, so the
      // pitch keeps modulating while the released note fades out. The arpeggio
      // is reset/cleared on the next note (md_replayer_set_test_arpeggio).
      md_key_off(channel);
    }

    // Pré-affecte un instrument à la voie d'audition pour que les réglages de
    // l'éditeur soient audibles avant même la première note jouée.
    void md_replayer_prepare_test_channel(int instr) {
      if (!current_module || instr < 1 || instr > MD_MAX_INSTRUMENTS)
        return;
      // La voie doit être choisie AVANT d'y écrire l'instrument, sinon on
      // enverrait un patch FM sur une voie PSG (ou l'inverse).
      md_assign_test_voice();
      ch_current_instr[MD_TEST_CHANNEL] = instr;
      md_set_instrument(MD_TEST_CHANNEL, &current_module->instruments[instr - 1]);
    }

    // ── Propriétés d'instrument pilotées par l'éditeur ──────────────────────
    // Les identifiants (md_op_prop_t / md_gen_prop_t) sont ceux attendus par
    // l'UI. Toute modification est immédiatement repoussée vers les canaux qui
    // utilisent l'instrument, y compris la voie d'audition.

    static void md_push_instrument_update(int ins_idx) {
      if (!current_module)
        return;
      for (int c = 0; c < MD_TOTAL_VOICES; c++) {
        if (ch_current_instr[c] == ins_idx)
          md_set_instrument((uint8_t)c,
                            &current_module->instruments[ins_idx - 1]);
      }
    }

    static uint8_t md_clamp_u8(int v, int max) {
      if (v < 0)
        v = 0;
      if (v > max)
        v = max;
      return (uint8_t)v;
    }

    void md_replayer_set_instr_op_val(int ins_idx, int op_idx, int prop_id,
                                      uint8_t val) {
      if (!current_module || ins_idx < 1 || ins_idx > MD_MAX_INSTRUMENTS)
        return;
      if (op_idx < 0 || op_idx > 3)
        return;
      md_op_params_t *o = &current_module->instruments[ins_idx - 1].op[op_idx];

      switch (prop_id) {
      case MD_OP_PROP_MULTIPLE:
        o->multiple = md_clamp_u8(val, 15);
        break;
      case MD_OP_PROP_KEY_SCALE:
        o->key_scale = md_clamp_u8(val, 3);
        break;
      case MD_OP_PROP_AM:
        o->am_enable = val ? 1 : 0;
        break;
      case MD_OP_PROP_DETUNE:
        o->detune = md_clamp_u8(val, 7);
        break;
      case MD_OP_PROP_SSG_EG:
        o->ssg_eg = md_clamp_u8(val, 15);
        break;
      case MD_OP_PROP_SUSTAIN_RATE:
        o->sustain_rate = md_clamp_u8(val, 31);
        break;
      case MD_OP_PROP_TOTAL_LEVEL:
        o->total_level = md_clamp_u8(val, 127);
        break;
      case MD_OP_PROP_ATTACK:
        o->attack = md_clamp_u8(val, 31);
        break;
      case MD_OP_PROP_DECAY:
        o->decay = md_clamp_u8(val, 31);
        break;
      case MD_OP_PROP_SUSTAIN_LEVEL:
        o->sustain_level = md_clamp_u8(val, 15);
        break;
      case MD_OP_PROP_RELEASE:
        o->release = md_clamp_u8(val, 15);
        break;
      default:
        return;
      }

      md_push_instrument_update(ins_idx);
    }

    uint8_t md_replayer_get_instr_op_val(int ins_idx, int op_idx, int prop_id) {
      if (!current_module || ins_idx < 1 || ins_idx > MD_MAX_INSTRUMENTS)
        return 0;
      if (op_idx < 0 || op_idx > 3)
        return 0;
      const md_op_params_t *o =
          &current_module->instruments[ins_idx - 1].op[op_idx];

      switch (prop_id) {
      case MD_OP_PROP_MULTIPLE:
        return o->multiple;
      case MD_OP_PROP_KEY_SCALE:
        return o->key_scale;
      case MD_OP_PROP_AM:
        return o->am_enable;
      case MD_OP_PROP_DETUNE:
        return o->detune;
      case MD_OP_PROP_SSG_EG:
        return o->ssg_eg;
      case MD_OP_PROP_SUSTAIN_RATE:
        return o->sustain_rate;
      case MD_OP_PROP_TOTAL_LEVEL:
        return o->total_level;
      case MD_OP_PROP_ATTACK:
        return o->attack;
      case MD_OP_PROP_DECAY:
        return o->decay;
      case MD_OP_PROP_SUSTAIN_LEVEL:
        return o->sustain_level;
      case MD_OP_PROP_RELEASE:
        return o->release;
      default:
        return 0;
      }
    }

    void md_replayer_set_instr_gen_val(int ins_idx, int prop_id, int val) {
      if (!current_module || ins_idx < 1 || ins_idx > MD_MAX_INSTRUMENTS)
        return;
      md_instr_t *ins = &current_module->instruments[ins_idx - 1];

      switch (prop_id) {
      case MD_GEN_PROP_ALGORITHM:
        ins->algorithm = md_clamp_u8(val, 7);
        break;
      case MD_GEN_PROP_FEEDBACK:
        ins->feedback = md_clamp_u8(val, 7);
        break;
      case MD_GEN_PROP_PANNING:
        ins->panning = md_clamp_u8(val, 2);
        break;
      case MD_GEN_PROP_FINE_TUNE:
        ins->fine_tune = (int8_t)val;
        break;
      case MD_GEN_PROP_AMS:
        ins->ams = md_clamp_u8(val, 3);
        break;
      case MD_GEN_PROP_PMS:
        ins->pms = md_clamp_u8(val, 7);
        break;
      case MD_GEN_PROP_LFO_ENABLE:
        ins->lfo_enable = val ? 1 : 0;
        break;
      case MD_GEN_PROP_LFO_FREQ:
        ins->lfo_freq = md_clamp_u8(val, 7);
        break;
      case MD_GEN_PROP_PSG_NOISE:
        ins->psg_noise = md_clamp_u8(val, 7);
        break;
      default:
        return;
      }

      md_push_instrument_update(ins_idx);
    }

    int md_replayer_get_instr_gen_val(int ins_idx, int prop_id) {
      if (!current_module || ins_idx < 1 || ins_idx > MD_MAX_INSTRUMENTS)
        return 0;
      const md_instr_t *ins = &current_module->instruments[ins_idx - 1];

      switch (prop_id) {
      case MD_GEN_PROP_ALGORITHM:
        return ins->algorithm;
      case MD_GEN_PROP_FEEDBACK:
        return ins->feedback;
      case MD_GEN_PROP_PANNING:
        return ins->panning;
      case MD_GEN_PROP_FINE_TUNE:
        return ins->fine_tune;
      case MD_GEN_PROP_AMS:
        return ins->ams;
      case MD_GEN_PROP_PMS:
        return ins->pms;
      case MD_GEN_PROP_LFO_ENABLE:
        return ins->lfo_enable;
      case MD_GEN_PROP_LFO_FREQ:
        return ins->lfo_freq;
      case MD_GEN_PROP_PSG_NOISE:
        return ins->psg_noise;
      default:
        return 0;
      }
    }

    void md_replayer_set_audition_channel(int channel) {
      if (channel < 0 || channel >= MD_MAX_CHANNELS)
        channel = 0;
      audition_channel = channel;
    }

    // Active ou desactive une voie. Desactivee, elle est coupee net puis
    // ignoree : plus aucune note ne l'atteint, donc le YM2612 cesse de la
    // calculer. Le morceau, lui, garde ses notes intactes.
    // Coupe net les effets qui tournent sur une voie.
    //
    // Un effet continu — vibrato, glissando, arpege — dure jusqu'a ce qu'on le
    // reecrive avec la valeur 00. EFFACER la case, elle, laisse 0xFF sur la
    // ligne, que le moteur ignore : l'effet continuait donc de tourner jusqu'a
    // l'arret de la lecture. L'interface appelle ceci quand on efface une
    // commande, pour que la suppression s'entende tout de suite.
    void md_replayer_clear_active_effects(int channel) {
      if (channel < 0 || channel >= MD_TOTAL_VOICES)
        return;
      for (int e = 0; e < MD_EFF_SLOTS; e++) {
        ch_active_eff[channel][e] = 0xFF;
        ch_active_eff_val[channel][e] = 0;
      }
    }

    void md_replayer_disable_channel(int channel, bool disabled) {
      if (channel < 0 || channel >= MD_TOTAL_VOICES)
        return;
      md_channel_disabled[channel] = disabled;
      if (disabled) {
        md_key_off(channel);
        if (channel == MD_PCM_CHANNEL)
          md_chip_pcm_stop();
      }
    }

    bool md_replayer_is_channel_disabled(int channel) {
      if (channel < 0 || channel >= MD_TOTAL_VOICES)
        return false;
      return md_channel_disabled[channel];
    }

    void md_replayer_mute_channel(int channel, bool muted) {
      if (channel >= 0 && channel < 32) {
        md_channel_muted[channel] = muted;
        // Le convertisseur n'a pas de registre de volume et ne passe pas par
        // md_write_volume : on l'arrête net, sinon la voie PCM continuait de
        // jouer alors qu'elle était coupée.
        if (muted && channel == MD_PCM_CHANNEL)
          md_chip_pcm_stop();
        // If we just muted it while it's playing, force the current volume to 0x3F
        // immediately
        if (channel < MD_TOTAL_VOICES) {
          // Applique (ou lève) immédiatement l'atténuation sur la voie.
          md_write_volume(channel);
        }
      }
    }

// ── Table de volumes des voies PSG (accès pour l'éditeur d'instrument) ──────
// L'index d'instrument est 1…MD_MAX_INSTRUMENTS, comme pour les accesseurs FM.
// Les écritures se font sous le verrou : la table est relue à chaque tick par
// le thread audio.

// ── Banque d'échantillons ───────────────────────────────────────────────────

int md_replayer_add_sample(const char *name, const uint8_t *data, uint32_t len,
                           int loop, int base_note) {
  if (!current_module || !data || len == 0)
    return -1;
  int slot = -1;
  for (int i = 0; i < MD_MAX_SAMPLES; i++)
    if (current_module->samples[i].length == 0) { slot = i; break; }
  if (slot < 0)
    return -1;                                  // plus d'emplacement
  if (current_module->pcm_used + len > MD_PCM_BANK_BYTES)
    return -1;                                  // banque pleine
  md_sample_t *sm = &current_module->samples[slot];
  memset(sm, 0, sizeof(*sm));
  if (name) { strncpy(sm->name, name, sizeof(sm->name) - 1); }
  sm->offset = current_module->pcm_used;
  sm->length = len;
  sm->loop = (loop >= 0 && (uint32_t)loop < len) ? loop : -1;
  sm->base_note = (uint8_t)(base_note > 0 && base_note <= MD_MAX_NOTE ? base_note : 49);
  memcpy(current_module->pcm + sm->offset, data, len);
  current_module->pcm_used += len;
  return slot;
}

int md_replayer_sample_count(void) {
  if (!current_module) return 0;
  int n = 0;
  for (int i = 0; i < MD_MAX_SAMPLES; i++)
    if (current_module->samples[i].length) n++;
  return n;
}

bool md_replayer_get_sample(int idx, char *name, int name_cap,
                            uint32_t *length, int *loop, int *base_note) {
  if (!current_module || idx < 0 || idx >= MD_MAX_SAMPLES) return false;
  const md_sample_t *sm = &current_module->samples[idx];
  if (name && name_cap > 0) {
    strncpy(name, sm->name, (size_t)name_cap - 1);
    name[name_cap - 1] = 0;
  }
  if (length) *length = sm->length;
  if (loop) *loop = sm->loop;
  if (base_note) *base_note = sm->base_note;
  return sm->length > 0;
}

const uint8_t *md_replayer_sample_data(int idx, uint32_t *length) {
  if (!current_module || idx < 0 || idx >= MD_MAX_SAMPLES) return NULL;
  const md_sample_t *sm = &current_module->samples[idx];
  if (length) *length = sm->length;
  return sm->length ? (current_module->pcm + sm->offset) : NULL;
}

void md_replayer_set_sample_base(int idx, int base_note) {
  if (!current_module || idx < 0 || idx >= MD_MAX_SAMPLES) return;
  if (base_note < 1) base_note = 1;
  if (base_note > MD_MAX_NOTE) base_note = MD_MAX_NOTE;
  current_module->samples[idx].base_note = (uint8_t)base_note;
}

void md_replayer_set_sample_loop(int idx, int loop) {
  if (!current_module || idx < 0 || idx >= MD_MAX_SAMPLES) return;
  md_sample_t *sm = &current_module->samples[idx];
  sm->loop = (loop >= 0 && (uint32_t)loop < sm->length) ? loop : -1;
}

void md_replayer_clear_samples(void) {
  if (!current_module) return;
  md_chip_pcm_stop();
  memset(current_module->samples, 0, sizeof(current_module->samples));
  current_module->pcm_used = 0;
}

uint32_t md_replayer_pcm_used(void) {
  return current_module ? current_module->pcm_used : 0;
}
uint32_t md_replayer_pcm_capacity(void) { return MD_PCM_BANK_BYTES; }

void md_replayer_set_instr_sample(int ins_idx, int sample_idx) {
  if (!current_module || ins_idx < 1 || ins_idx > MD_MAX_INSTRUMENTS) return;
  // MD_EMPTY = aucun échantillon, et c'est une valeur légitime.
  if (sample_idx != MD_EMPTY && (sample_idx < 0 || sample_idx >= MD_MAX_SAMPLES))
    sample_idx = MD_EMPTY;
  current_module->instruments[ins_idx - 1].pcm_sample = (uint8_t)sample_idx;
}
void md_replayer_set_instr_pcm_volume(int ins_idx, int volume) {
  if (!current_module || ins_idx < 1 || ins_idx > MD_MAX_INSTRUMENTS) return;
  if (volume < 0) volume = 0;
  if (volume > 255) volume = 255;   // 7F = unité, jusqu'à FF pour pousser
  current_module->instruments[ins_idx - 1].pcm_volume = (uint8_t)volume;
}
int md_replayer_get_instr_pcm_volume(int ins_idx) {
  if (!current_module || ins_idx < 1 || ins_idx > MD_MAX_INSTRUMENTS) return 127;
  return current_module->instruments[ins_idx - 1].pcm_volume;
}
int md_replayer_get_instr_sample(int ins_idx) {
  if (!current_module || ins_idx < 1 || ins_idx > MD_MAX_INSTRUMENTS) return 0;
  return current_module->instruments[ins_idx - 1].pcm_sample;
}

void md_replayer_set_instr_kind(int ins_idx, int kind) {
  if (!current_module || ins_idx < 1 || ins_idx > MD_MAX_INSTRUMENTS) return;
  if (kind < 0 || kind > MD_INSTR_KIND_PCM) kind = MD_INSTR_KIND_ANY;
  current_module->instruments[ins_idx - 1].kind = (uint8_t)kind;
}

int md_replayer_get_instr_kind(int ins_idx) {
  if (!current_module || ins_idx < 1 || ins_idx > MD_MAX_INSTRUMENTS)
    return MD_INSTR_KIND_ANY;
  return current_module->instruments[ins_idx - 1].kind;
}

void md_replayer_set_psg_vol_macro(int ins_idx, const uint8_t *vals, int len,
                                   int loop) {
  if (!current_module || ins_idx < 1 || ins_idx > MD_MAX_INSTRUMENTS) return;
  md_instr_t *ins = &current_module->instruments[ins_idx - 1];
  if (len < 0) len = 0;
  if (len > MD_PSG_MACRO_MAX) len = MD_PSG_MACRO_MAX;
  ins->psg_vol_len = (uint8_t)len;
  ins->psg_vol_loop = (loop >= 0 && loop < len) ? (uint8_t)loop : MD_EMPTY;
  for (int i = 0; i < MD_PSG_MACRO_MAX; i++)
    ins->psg_vol_mac[i] = (vals && i < len) ? (vals[i] > 15 ? 15 : vals[i]) : 0;
}

int md_replayer_get_psg_vol_macro(int ins_idx, uint8_t *out, int max_len,
                                  int *loop) {
  if (!current_module || ins_idx < 1 || ins_idx > MD_MAX_INSTRUMENTS) return 0;
  const md_instr_t *ins = &current_module->instruments[ins_idx - 1];
  int n = ins->psg_vol_len;
  if (out) for (int i = 0; i < n && i < max_len; i++) out[i] = ins->psg_vol_mac[i];
  if (loop) *loop = ins->psg_vol_loop;
  return n;
}

void md_replayer_set_psg_arp_macro(int ins_idx, const int8_t *vals, int len,
                                   int loop, bool fixed) {
  if (!current_module || ins_idx < 1 || ins_idx > MD_MAX_INSTRUMENTS) return;
  md_instr_t *ins = &current_module->instruments[ins_idx - 1];
  if (len < 0) len = 0;
  if (len > MD_PSG_MACRO_MAX) len = MD_PSG_MACRO_MAX;
  ins->psg_arp_len = (uint8_t)len;
  ins->psg_arp_loop = (loop >= 0 && loop < len) ? (uint8_t)loop : MD_EMPTY;
  ins->psg_arp_fixed = fixed ? 1 : 0;
  for (int i = 0; i < MD_PSG_MACRO_MAX; i++)
    ins->psg_arp_mac[i] = (vals && i < len) ? vals[i] : 0;
}

int md_replayer_get_psg_arp_macro(int ins_idx, int8_t *out, int max_len,
                                  int *loop, bool *fixed) {
  if (!current_module || ins_idx < 1 || ins_idx > MD_MAX_INSTRUMENTS) return 0;
  const md_instr_t *ins = &current_module->instruments[ins_idx - 1];
  int n = ins->psg_arp_len;
  if (out) for (int i = 0; i < n && i < max_len; i++) out[i] = ins->psg_arp_mac[i];
  if (loop) *loop = ins->psg_arp_loop;
  if (fixed) *fixed = ins->psg_arp_fixed != 0;
  return n;
}

static md_instr_t *md_psg_instr_at(int ins_idx) {
  if (!current_module || ins_idx < 1 || ins_idx > MD_MAX_INSTRUMENTS)
    return NULL;
  return &current_module->instruments[ins_idx - 1];
}

// ── Enveloppe des voies PSG (écran ENV.) ────────────────────────────────────
// Trois points « amplitude / vitesse ». Amplitude MD_EMPTY = point désactivé,
// ce qui désactive aussi ceux qui suivent. Vitesse 0 = tenir, 1 = le plus
// rapide, 15 = le plus lent.

int md_replayer_get_env_amp(int ins_idx, int point) {
  const md_instr_t *ins = md_psg_instr_at(ins_idx);
  if (!ins || point < 0 || point >= MD_ENV_POINTS)
    return MD_EMPTY;
  return ins->env_amp[point];
}

int md_replayer_get_env_speed(int ins_idx, int point) {
  const md_instr_t *ins = md_psg_instr_at(ins_idx);
  if (!ins || point < 0 || point >= MD_ENV_POINTS)
    return 0;
  return ins->env_speed[point];
}

void md_replayer_set_env_amp(int ins_idx, int point, int amp) {
  if (point < 0 || point >= MD_ENV_POINTS)
    return;
  md_lock(&replayer_lock);
  md_instr_t *ins = md_psg_instr_at(ins_idx);
  if (ins) {
    if (amp == MD_EMPTY || amp < 0) {
      // Désactiver un point désactive la suite : une enveloppe ne peut pas
      // avoir de trou.
      if (point == 0) {
        ins->env_amp[0] = 0;          // le premier point existe toujours
      } else {
        for (int i = point; i < MD_ENV_POINTS; i++)
          ins->env_amp[i] = MD_EMPTY;
      }
    } else {
      ins->env_amp[point] = (uint8_t)(amp > 15 ? 15 : amp);
      // Réactiver un point réactive ceux qui le précèdent.
      for (int i = 0; i < point; i++)
        if (ins->env_amp[i] == MD_EMPTY)
          ins->env_amp[i] = 0;
    }
  }
  md_unlock(&replayer_lock);
}

void md_replayer_set_env_speed(int ins_idx, int point, int speed) {
  if (point < 0 || point >= MD_ENV_POINTS)
    return;
  if (speed < 0) speed = 0;
  if (speed > 15) speed = 15;
  md_lock(&replayer_lock);
  md_instr_t *ins = md_psg_instr_at(ins_idx);
  if (ins)
    ins->env_speed[point] = (uint8_t)speed;
  md_unlock(&replayer_lock);
}

// ── Macro de mode de bruit (accès pour l'éditeur) ──────────────────────────
// Même contrat que la table de volumes : écritures sous verrou, `loop` à
// MD_EMPTY quand il n'y a pas de bouclage.

int md_replayer_get_psg_noise_macro(int ins_idx, uint8_t *out, int max_len,
                                    uint8_t *loop) {
  if (loop) *loop = MD_EMPTY;
  const md_instr_t *ins = md_psg_instr_at(ins_idx);
  if (!ins)
    return 0;
  int len = ins->psg_noise_len;
  if (len > MD_PSG_ENV_MAX)
    len = MD_PSG_ENV_MAX;
  if (out && max_len > 0) {
    int n = len < max_len ? len : max_len;
    for (int i = 0; i < n; i++)
      out[i] = ins->psg_noise_mac[i];
  }
  if (loop) *loop = ins->psg_noise_loop;
  return len;
}

void md_replayer_set_psg_noise_step(int ins_idx, int index, int value) {
  if (index < 0 || index >= MD_PSG_ENV_MAX)
    return;
  md_lock(&replayer_lock);
  md_instr_t *ins = md_psg_instr_at(ins_idx);
  if (ins) {
    ins->psg_noise_mac[index] = (uint8_t)md_clamp_u8(value, 7);
    if (index >= ins->psg_noise_len)
      ins->psg_noise_len = (uint8_t)(index + 1);
  }
  md_unlock(&replayer_lock);
}

void md_replayer_set_psg_noise_len(int ins_idx, int len) {
  if (len < 0) len = 0;
  if (len > MD_PSG_ENV_MAX) len = MD_PSG_ENV_MAX;
  md_lock(&replayer_lock);
  md_instr_t *ins = md_psg_instr_at(ins_idx);
  if (ins) {
    ins->psg_noise_len = (uint8_t)len;
    if (ins->psg_noise_loop != MD_EMPTY && ins->psg_noise_loop >= len)
      ins->psg_noise_loop = MD_EMPTY;
  }
  md_unlock(&replayer_lock);
}

void md_replayer_set_psg_noise_loop(int ins_idx, int loop) {
  md_lock(&replayer_lock);
  md_instr_t *ins = md_psg_instr_at(ins_idx);
  if (ins)
    ins->psg_noise_loop =
        (loop < 0 || loop >= ins->psg_noise_len) ? MD_EMPTY : (uint8_t)loop;
  md_unlock(&replayer_lock);
}

// ── Tables d'instrument ─────────────────────────────────────────────────────
// Une table se déroule d'une ligne par tick tant que la note dure, puis
// reboucle. Elle s'applique à n'importe quel canal, FM comme PSG : la colonne
// VOL fixe le volume, TSP transpose la note, et les deux colonnes CMD passent
// leurs commandes au moteur d'effets par son second emplacement (le premier
// reste à la phrase, exactement comme dans un tracker).

// ── Commandes des colonnes CMD ──────────────────────────────────────────────
// L'ordre fixe la valeur stockée dans les fichiers : ne pas réordonner sans
// convertir les .mdm existants.
//
// Les lettres sont celles de LSDJ (manuel 9.2.6, chapitre 4) ; l'effet est
// celui du moteur Mega Drive. Certaines commandes doivent transformer leur
// valeur avant de la passer au moteur : les effets « étendus » codent leur
// sous-commande dans le quartet HAUT, si bien qu'un K03 (couper après 3 ticks)
// doit devenir 0x33. D'où `val_mask` et `val_or`.
//
// `special` marque les commandes que la table ou la phrase traitent
// elles-mêmes, sans passer par le moteur d'effets.
static const struct {
  char letter;
  int effect;          // -1 si `kind` != MD_CMD_PLAIN
  uint8_t val_mask;    // masque appliqué à la valeur saisie
  uint8_t val_or;      // bits ajoutés ensuite (sous-commande étendue)
  md_cmd_kind_t kind;
} md_table_cmds[] = {
    {'A', -1,                        0xFF, 0x00, MD_CMD_TABLE},
    {'C', MD_EFF_ARPEGGIO,           0xFF, 0x00, MD_CMD_PLAIN},
    {'D', MD_EFF_EXTENDED2,          0x0F, MD_EX2_NOTE_DELAY << 4, MD_CMD_PLAIN},
    {'H', -1,                        0xFF, 0x00, MD_CMD_HOP},
    {'K', MD_EFF_EXTENDED2,          0x0F, MD_EX2_NOTE_CUT << 4, MD_CMD_PLAIN},
    {'L', MD_EFF_TONE_PORTAMENTO,    0xFF, 0x00, MD_CMD_PLAIN},
    {'M', MD_EFF_SET_GLOBAL_VOLUME,  0xFF, 0x00, MD_CMD_PLAIN},
    {'O', MD_EFF_EXTENDED,           0x0F, MD_EX_SET_PANNING_POS << 4, MD_CMD_PLAIN},
    {'P', -1,                        0xFF, 0x00, MD_CMD_PITCH},
    {'R', MD_EFF_RETRIG_NOTE,        0xFF, 0x00, MD_CMD_PLAIN},
    {'T', MD_EFF_SET_TEMPO,          0xFF, 0x00, MD_CMD_PLAIN},
    {'V', MD_EFF_VIBRATO,            0xFF, 0x00, MD_CMD_PLAIN},
    {'Z', MD_EFF_TREMOLO,            0xFF, 0x00, MD_CMD_PLAIN},
    // ── Effets standard de DefleMask sans équivalent LSDJ ────────────────
    {'B', MD_EFF_VOL_SLIDE,          0xFF, 0x00, MD_CMD_PLAIN},  // Axy
    {'E', MD_EFF_TPORTA_VSLIDE,      0xFF, 0x00, MD_CMD_PLAIN},  // 5xy
    {'F', MD_EFF_VIBRATO_VSLIDE,     0xFF, 0x00, MD_CMD_PLAIN},  // 6xy
    {'S', MD_EFF_SET_SPEED,          0xFF, 0x00, MD_CMD_PLAIN},  // 9xx / Fxx
    {'J', MD_EFF_POSITION_JUMP,      0xFF, 0x00, MD_CMD_PLAIN},  // Bxx
    {'N', MD_EFF_PATTERN_BREAK,      0xFF, 0x00, MD_CMD_PLAIN},  // Dxx
    {'W', MD_EFF_EXTENDED,           0x0F, MD_EX_SET_VIB_DEPTH << 4, MD_CMD_PLAIN}, // E4xx
    {'U', -1,                        0xFF, 0x00, MD_CMD_FINETUNE},                  // E5xx
};

int md_table_cmd_count(void) {
  return (int)(sizeof(md_table_cmds) / sizeof(md_table_cmds[0]));
}

char md_table_cmd_letter(int i) {
  if (i < 0 || i >= md_table_cmd_count())
    return 0;
  return md_table_cmds[i].letter;
}

int md_table_cmd_effect(int i) {
  if (i < 0 || i >= md_table_cmd_count())
    return -1;
  return md_table_cmds[i].effect;
}

// Valeur réellement transmise au moteur d'effets pour cette commande.
uint8_t md_table_cmd_value(int i, uint8_t val) {
  if (i < 0 || i >= md_table_cmd_count())
    return val;
  return (uint8_t)((val & md_table_cmds[i].val_mask) | md_table_cmds[i].val_or);
}

// Résout une commande en (effet, valeur) réellement transmis au moteur.
//
// Nécessaire pour P, dont la valeur est SIGNÉE comme dans LSDJ : 01-7F fait
// monter la hauteur, 80-FF la fait descendre (FE = -2). Une seule commande
// suffit donc pour les deux sens.
void md_table_cmd_resolve(int i, uint8_t val, int *out_eff, uint8_t *out_val) {
  int eff = md_table_cmd_effect(i);
  uint8_t v = md_table_cmd_value(i, val);
  if (i >= 0 && i < md_table_cmd_count() &&
      md_table_cmds[i].kind == MD_CMD_FINETUNE) {
    // Comme le E5xx de DefleMask : 80 est le centre, au-dessus on monte, en
    // dessous on descend. Notre effet étendu n'a que 4 bits, l'écart sature.
    int d = (int)val - 0x80;
    int mag = d < 0 ? -d : d;
    if (mag > 15) mag = 15;
    if (d == 0) {
      eff = -1; v = 0;
    } else {
      eff = MD_EFF_EXTENDED2;
      v = (uint8_t)(((d > 0 ? MD_EX2_FINE_TUNE_UP : MD_EX2_FINE_TUNE_DOWN) << 4)
                    | mag);
    }
  } else if (i >= 0 && i < md_table_cmd_count() &&
      md_table_cmds[i].kind == MD_CMD_PITCH) {
    if (val == 0) {
      eff = -1;                       // P00 arrête le bend
      v = 0;
    } else if (val < 0x80) {
      eff = MD_EFF_FSLIDE_UP;
      v = val;
    } else {
      eff = MD_EFF_FSLIDE_DOWN;
      v = (uint8_t)(256 - val);
    }
  }
  if (out_eff) *out_eff = eff;
  if (out_val) *out_val = v;
}

// La commande est-elle traitée par la table elle-même plutôt que par le moteur
// d'effets ? (H pour les sauts, A pour démarrer/arrêter une table.)
int md_table_cmd_kind(int i) {
  if (i < 0 || i >= md_table_cmd_count())
    return MD_CMD_PLAIN;
  return (int)md_table_cmds[i].kind;
}

void md_replayer_get_table_row(int table, int row, uint8_t *vol,
                               int8_t *transpose, uint8_t *cmd1, uint8_t *val1,
                               uint8_t *cmd2, uint8_t *val2,
                               uint8_t *mdcmd, uint8_t *mdval) {
  if (vol) *vol = MD_EMPTY;
  if (transpose) *transpose = 0;
  if (cmd1) *cmd1 = MD_EMPTY;
  if (val1) *val1 = 0;
  if (cmd2) *cmd2 = MD_EMPTY;
  if (val2) *val2 = 0;
  if (mdcmd) *mdcmd = MD_EMPTY;
  if (mdval) *mdval = 0;
  if (!current_module || table < 0 || table >= MD_MAX_TABLES || row < 0 ||
      row >= MD_TABLE_ROWS)
    return;
  const md_table_row_t *r = &current_module->tables[table].rows[row];
  if (vol) *vol = r->vol;
  if (transpose) *transpose = r->transpose;
  if (cmd1) *cmd1 = r->cmd1;
  if (val1) *val1 = r->val1;
  if (cmd2) *cmd2 = r->cmd2;
  if (val2) *val2 = r->val2;
  if (mdcmd) *mdcmd = r->mdcmd;
  if (mdval) *mdval = r->mdval;
}

// Une table vient d'être modifiée : les canaux qui la jouent doivent repartir
// de l'instrument PROPRE, sinon un réglage de registre posé par la ligne qu'on
// vient de changer reste écrit dans la copie de travail. On n'entendait alors
// sa modification — ou son retrait — qu'après un stop/play, qui seul remettait
// tout à plat.
//
// Ce qui vient d'une PHRASE est réappliqué juste après : lui doit survivre,
// c'est la règle des commandes qui tiennent d'une note à l'autre.
static void md_refresh_table(int table) {
  if (!current_module) return;

  // La vitesse et le tempo reviennent à ceux du MORCEAU. Une commande de
  // vitesse survolée en éditant ralentit la lecture tant qu'elle est écrite —
  // c'est normal, une Game Boy fait pareil — mais dès qu'on passe à autre
  // chose tout doit repartir, sans avoir à relancer la lecture.
  // Le volume global aussi : une commande M qu'on vient de retirer ne doit pas
  // laisser le morceau assourdi.
  replay_global_vol = 63;
  replay_speed = current_module->initial_speed > 0 ? current_module->initial_speed : 6;
  replay_tempo = current_module->initial_tempo >= 18 ? current_module->initial_tempo : 125;
  {
    int macro = current_module->macro_speedup > 0 ? current_module->macro_speedup : 1;
    int irq = replay_tempo * macro;
    if (irq <= 0) irq = 50;
    samples_per_tick =
        (int)((double)replay_sample_rate / ((double)irq * md_finetune_scale()));
    if (samples_per_tick < 1) samples_per_tick = 1;
    // La ligne EN COURS garde son décompte : après un « S FF » elle tiendrait
    // encore 255 ticks avant de céder, et la lecture semblerait rester bloquée
    // malgré le retour à la vitesse normale.
    if (replay_ticks_left > replay_speed) replay_ticks_left = replay_speed;
  }

  for (int c = 0; c < MD_TOTAL_VOICES; c++) {
    // Une voie DÉTOURNÉE compte aussi, même si elle ne joue plus la table
    // éditée. En modifiant une commande on passe forcément par le premier cran
    // de la liste — la commande A, qui change de table — et un « A 00 » survolé
    // au passage verrouillait la voie sur la table 00 : les repères
    // disparaissaient, l'effet ne s'entendait plus, et seul un stop/play
    // défaisait le verrou. On rend donc la voie à la table de son instrument ;
    // si un A y est encore écrit, il se réappliquera au prochain passage.
    if (ch_table_forced[c]) {
      ch_table_forced[c] = false;
      md_table_start(c);
    }
    if (ch_table[c] != table || !ch_table_running[c]) continue;
    for (int i = 0; i < MD_MDCMD_SLOTS; i++)
      if (ch_mdcmd_from_table[c][i]) { ch_mdcmd_on[c][i] = false; ch_mdcmd_from_table[c][i] = false; }
    int ins = ch_current_instr[c];
    if (ins >= 1 && ins <= MD_MAX_INSTRUMENTS)
      md_set_instrument((uint8_t)c, &current_module->instruments[ins - 1]);
    md_reapply_mdcmds(c);
    // Et les effets de la table se réarmeront au prochain passage.
    for (int sl = 0; sl < MD_TABLE_STREAMS - 1; sl++)
      ch_active_eff[c][MD_EFF_SLOT_TABLE + sl] = 0xFF;
  }
}

void md_replayer_set_table_row(int table, int row, uint8_t vol,
                               int8_t transpose, uint8_t cmd1, uint8_t val1,
                               uint8_t cmd2, uint8_t val2,
                               uint8_t mdcmd, uint8_t mdval) {
  if (table < 0 || table >= MD_MAX_TABLES || row < 0 || row >= MD_TABLE_ROWS)
    return;
  md_lock(&replayer_lock);
  if (current_module) {
    md_table_row_t *r = &current_module->tables[table].rows[row];
    r->vol = vol;
    r->transpose = transpose;
    r->cmd1 = cmd1;
    r->val1 = val1;
    r->cmd2 = cmd2;
    r->val2 = val2;
    r->mdcmd = mdcmd;
    r->mdval = mdval;
    md_refresh_table(table);
  }
  md_unlock(&replayer_lock);
}

bool md_replayer_table_is_empty(int table) {
  if (!current_module || table < 0 || table >= MD_MAX_TABLES)
    return true;
  const md_table_t *t = &current_module->tables[table];
  for (int r = 0; r < MD_TABLE_ROWS; r++) {
    const md_table_row_t *row = &t->rows[r];
    if (row->vol != 0 || row->transpose != 0 ||
        row->cmd1 != MD_EMPTY || row->cmd2 != MD_EMPTY ||
        row->mdcmd != MD_EMPTY)
      return false;
  }
  return true;
}

int md_replayer_get_instr_table(int ins_idx) {
  const md_instr_t *ins = md_psg_instr_at(ins_idx);
  return ins ? ins->table : MD_EMPTY;
}

void md_replayer_set_instr_table(int ins_idx, int table) {
  md_lock(&replayer_lock);
  md_instr_t *ins = md_psg_instr_at(ins_idx);
  if (ins)
    ins->table = (table < 0 || table >= MD_MAX_TABLES) ? MD_EMPTY
                                                       : (uint8_t)table;
  md_unlock(&replayer_lock);
}

int md_replayer_play_table(int channel) {
  if (channel < 0 || channel >= MD_TOTAL_VOICES)
    return -1;
  return ch_table_running[channel] ? ch_table[channel] : -1;
}

int md_replayer_play_table_pos(int channel, int stream) {
  if (channel < 0 || channel >= MD_TOTAL_VOICES || stream < 0 ||
      stream >= MD_TABLE_STREAMS)
    return -1;
  return ch_table_running[channel] ? ch_table_pos[channel][stream] : -1;
}

// ── Portée de la lecture ────────────────────────────────────────────────────
// Appelée par l'interface juste avant de lancer la lecture, selon l'écran où se
// trouve le curseur. Ne prend effet qu'au prochain démarrage.
void md_replayer_set_play_scope(int scope, int channel, int id) {
  md_lock(&replayer_lock);
  if (scope < MD_SCOPE_SONG || scope > MD_SCOPE_PHRASE)
    scope = MD_SCOPE_SONG;
  play_scope = (md_play_scope_t)scope;
  scope_channel = (channel < 0 || channel >= MD_MAX_CHANNELS) ? 0 : channel;
  if (play_scope == MD_SCOPE_PHRASE)
    scope_id = (id < 0 || id >= MD_MAX_PHRASES) ? 0 : id;
  else if (play_scope == MD_SCOPE_CHAIN)
    scope_id = (id < 0 || id >= MD_MAX_CHAINS) ? 0 : id;
  else
    scope_id = 0;
  md_unlock(&replayer_lock);
}

// ── Modèle de puce FM ───────────────────────────────────────────────────────
// Simple relais vers la couche puce, pour que l'interface n'ait pas à inclure
// md_chip.h (qui est du C++ derrière son extern "C").
void md_replayer_set_ladder(bool enabled) { md_chip_set_ladder(enabled); }
bool md_replayer_get_ladder(void) { return md_chip_get_ladder(); }

// ── Commandes « MD » : les réglages de la machine ────────────────────────────
//
// Elles reprennent les paramètres que GenMDM expose en MIDI CC — algorithme,
// feedback, niveaux et enveloppes des quatre opérateurs, LFO, stéréo, bruit
// PSG — mais écrits « lettre + deux chiffres » comme le reste du tracker.
//
// Pour un paramètre d'OPÉRATEUR, le premier chiffre est le numéro d'opérateur
// (1-4) et le second la valeur ; pour un paramètre de CANAL, les deux chiffres
// forment la valeur. Un paramètre d'opérateur n'a donc que 4 bits de
// résolution : le Total Level, qui va de 0 à 127, avance par pas de 8. C'est le
// prix d'une case à trois caractères, et ça reste largement suffisant pour
// balayer un timbre en cours de note.
typedef enum {
  // ── Le jeu de DefleMask pour Megadrive (manuel v2.0.0) ────────────────
  MD_P_LFO = 0,   // 10xy
  MD_P_FB,        // 11xx
  MD_P_TL1, MD_P_TL2, MD_P_TL3, MD_P_TL4,   // 12xx…15xx
  MD_P_MUL,       // 16xy
  MD_P_ARALL,     // 19xx
  MD_P_AR1, MD_P_AR2, MD_P_AR3, MD_P_AR4,   // 1Axx…1Dxx
  MD_P_NOISE,     // 20xy
  // ── Au-delà de DefleMask : ce que la puce sait faire et qu'ils n'ont pas
  //    prévu. Rien n'oblige à s'en servir, mais rien ne justifiait de les
  //    retirer — ils n'entrent en conflit avec aucun effet du .dmf.
  MD_P_ALG, MD_P_PAN, MD_P_PMS, MD_P_AMS,
  MD_P_DT, MD_P_RS, MD_P_D1R, MD_P_D2R, MD_P_D1L, MD_P_RR, MD_P_AM, MD_P_SSG,
  MD_P_EFFECT,   // pas un registre : un effet, traité par le moteur d'effets
} md_param_t;

static const struct {
  uint8_t code;        // code d'effet DefleMask, tel qu'écrit dans son manuel
  md_param_t param;    // MD_P_EFFECT : effet, sinon registre de la puce
  bool per_op;         // le premier chiffre est un numéro d'opérateur
  char equiv;          // commande CMD qui fait doublon (0 : aucune)
  int effect;          // les quatre champs suivants ne servent qu'aux effets
  uint8_t val_mask;
  uint8_t val_or;
  md_cmd_kind_t kind;
} md_mdcmds[] = {
    // ── Effets standard de DefleMask ─────────────────────────────────────
    // `equiv` nomme la commande CMD qui fait déjà la même chose. Quand les
    // deux colonnes la portent sur une même ligne, c'est CMD qui gagne et
    // celle-ci est ignorée : un .dmf importé peut donc tout déverser ici
    // sans jamais entrer en conflit avec ce qui est écrit à la main.
    {0x00, MD_P_EFFECT, false, 'C', MD_EFF_ARPEGGIO,        0xFF, 0x00, MD_CMD_PLAIN},
    {0x01, MD_P_EFFECT, false, 'P', MD_EFF_FSLIDE_UP,       0xFF, 0x00, MD_CMD_PLAIN},
    {0x02, MD_P_EFFECT, false, 'P', MD_EFF_FSLIDE_DOWN,     0xFF, 0x00, MD_CMD_PLAIN},
    {0x03, MD_P_EFFECT, false, 'L', MD_EFF_TONE_PORTAMENTO, 0xFF, 0x00, MD_CMD_PLAIN},
    {0x04, MD_P_EFFECT, false, 'V', MD_EFF_VIBRATO,         0xFF, 0x00, MD_CMD_PLAIN},
    {0x05, MD_P_EFFECT, false, 'E', MD_EFF_TPORTA_VSLIDE,   0xFF, 0x00, MD_CMD_PLAIN},
    {0x06, MD_P_EFFECT, false, 'F', MD_EFF_VIBRATO_VSLIDE,  0xFF, 0x00, MD_CMD_PLAIN},
    {0x07, MD_P_EFFECT, false, 'Z', MD_EFF_TREMOLO,         0xFF, 0x00, MD_CMD_PLAIN},
    {0x08, MD_P_PAN,    false, 'O', -1,                     0xFF, 0x00, MD_CMD_PLAIN},
    {0x09, MD_P_EFFECT, false, 'S', MD_EFF_SET_SPEED,       0xFF, 0x00, MD_CMD_PLAIN},
    {0x0A, MD_P_EFFECT, false, 'B', MD_EFF_VOL_SLIDE,       0xFF, 0x00, MD_CMD_PLAIN},
    {0x0B, MD_P_EFFECT, false, 'J', MD_EFF_POSITION_JUMP,   0xFF, 0x00, MD_CMD_PLAIN},
    {0x0C, MD_P_EFFECT, false, 'R', MD_EFF_RETRIG_NOTE,     0xFF, 0x00, MD_CMD_PLAIN},
    {0x0D, MD_P_EFFECT, false, 'N', MD_EFF_PATTERN_BREAK,   0xFF, 0x00, MD_CMD_PLAIN},
    {0x0F, MD_P_EFFECT, false, 'S', MD_EFF_SET_SPEED,       0xFF, 0x00, MD_CMD_PLAIN},
    // ── Commandes étendues ───────────────────────────────────────────────
    {0xE4, MD_P_EFFECT, false, 'W', MD_EFF_EXTENDED,  0x0F, MD_EX_SET_VIB_DEPTH << 4, MD_CMD_PLAIN},
    {0xE5, MD_P_EFFECT, false, 'U', -1,               0xFF, 0x00, MD_CMD_FINETUNE},
    {0xEC, MD_P_EFFECT, false, 'K', MD_EFF_EXTENDED2, 0x0F, MD_EX2_NOTE_CUT << 4,     MD_CMD_PLAIN},
    {0xED, MD_P_EFFECT, false, 'D', MD_EFF_EXTENDED2, 0x0F, MD_EX2_NOTE_DELAY << 4,   MD_CMD_PLAIN},
    // ── Effets Megadrive de DefleMask ────────────────────────────────────
    {0x10, MD_P_LFO,   false, 0, -1, 0xFF, 0x00, MD_CMD_PLAIN},  // x marche, y vitesse
    {0x11, MD_P_FB,    false, 0, -1, 0xFF, 0x00, MD_CMD_PLAIN},  // feedback 0-7
    {0x12, MD_P_TL1,   false, 0, -1, 0xFF, 0x00, MD_CMD_PLAIN},  // TL op 1, 00-7F
    {0x13, MD_P_TL2,   false, 0, -1, 0xFF, 0x00, MD_CMD_PLAIN},
    {0x14, MD_P_TL3,   false, 0, -1, 0xFF, 0x00, MD_CMD_PLAIN},
    {0x15, MD_P_TL4,   false, 0, -1, 0xFF, 0x00, MD_CMD_PLAIN},
    {0x16, MD_P_MUL,   true,  0, -1, 0xFF, 0x00, MD_CMD_PLAIN},  // x opérateur, y multiple
    {0x19, MD_P_ARALL, false, 0, -1, 0xFF, 0x00, MD_CMD_PLAIN},  // attaque des quatre
    {0x1A, MD_P_AR1,   false, 0, -1, 0xFF, 0x00, MD_CMD_PLAIN},
    {0x1B, MD_P_AR2,   false, 0, -1, 0xFF, 0x00, MD_CMD_PLAIN},
    {0x1C, MD_P_AR3,   false, 0, -1, 0xFF, 0x00, MD_CMD_PLAIN},
    {0x1D, MD_P_AR4,   false, 0, -1, 0xFF, 0x00, MD_CMD_PLAIN},
    {0x20, MD_P_NOISE, false, 0, -1, 0xFF, 0x00, MD_CMD_PLAIN},  // mode de bruit PSG
    // ── En plus : la puce le sait, DefleMask ne l'expose pas ─────────────
    // Codes pris au-dessus de 20, que DefleMask n'utilise pas : aucun risque
    // de collision avec un effet d'un fichier importé.
    {0x30, MD_P_ALG,   false, 0, -1, 0xFF, 0x00, MD_CMD_PLAIN},  // algorithme 0-7
    {0x31, MD_P_PAN,   false, 0, -1, 0xFF, 0x00, MD_CMD_PLAIN},  // 0 centre, 1 g., 2 d.
    {0x32, MD_P_PMS,   false, 0, -1, 0xFF, 0x00, MD_CMD_PLAIN},
    {0x33, MD_P_AMS,   false, 0, -1, 0xFF, 0x00, MD_CMD_PLAIN},
    {0x34, MD_P_DT,    true,  0, -1, 0xFF, 0x00, MD_CMD_PLAIN},
    {0x35, MD_P_RS,    true,  0, -1, 0xFF, 0x00, MD_CMD_PLAIN},
    {0x36, MD_P_D1R,   true,  0, -1, 0xFF, 0x00, MD_CMD_PLAIN},
    {0x37, MD_P_D2R,   true,  0, -1, 0xFF, 0x00, MD_CMD_PLAIN},
    {0x38, MD_P_D1L,   true,  0, -1, 0xFF, 0x00, MD_CMD_PLAIN},
    {0x39, MD_P_RR,    true,  0, -1, 0xFF, 0x00, MD_CMD_PLAIN},
    {0x3A, MD_P_AM,    true,  0, -1, 0xFF, 0x00, MD_CMD_PLAIN},
    {0x3B, MD_P_SSG,   true,  0, -1, 0xFF, 0x00, MD_CMD_PLAIN},
};

int md_mdcmd_count(void) {
  return (int)(sizeof(md_mdcmds) / sizeof(md_mdcmds[0]));
}
// Le code DefleMask de la commande : c'est lui qui s'affiche, sur deux
// chiffres hexadécimaux. Vingt-cinq lettres ne pouvaient pas porter les
// quarante-quatre effets, et ce codage est de toute façon celui du manuel.
uint8_t md_mdcmd_code(int i) {
  return (i < 0 || i >= md_mdcmd_count()) ? 0xFF : md_mdcmds[i].code;
}

// Retrouve une commande par son code DefleMask (-1 si inconnue).
int md_mdcmd_index_of_code(uint8_t code) {
  for (int i = 0; i < md_mdcmd_count(); i++)
    if (md_mdcmds[i].code == code)
      return i;
  return -1;
}

// Vrai si la commande passe par le moteur d'effets plutôt que par les
// registres de la puce.
bool md_mdcmd_is_effect(int i) {
  return (i < 0 || i >= md_mdcmd_count()) ? false
                                          : md_mdcmds[i].param == MD_P_EFFECT;
}

// La commande CMD qui ferait doublon avec celle-ci, 0 si aucune.
char md_mdcmd_equivalent(int i) {
  return (i < 0 || i >= md_mdcmd_count()) ? 0 : md_mdcmds[i].equiv;
}

// Même travail que md_table_cmd_resolve, pour la colonne MD CMD.
void md_mdcmd_resolve(int i, uint8_t val, int *out_eff, uint8_t *out_val) {
  int eff = -1;
  uint8_t v = val;
  if (i >= 0 && i < md_mdcmd_count() && md_mdcmds[i].param == MD_P_EFFECT) {
    eff = md_mdcmds[i].effect;
    v = (uint8_t)((val & md_mdcmds[i].val_mask) | md_mdcmds[i].val_or);
    if (md_mdcmds[i].kind == MD_CMD_FINETUNE) {
      // E5xx : 80 est le centre, au-dessus on monte, en dessous on descend.
      int d = (int)val - 0x80;
      int mag = d < 0 ? -d : d;
      if (mag > 15) mag = 15;
      if (d == 0) {
        eff = -1; v = 0;
      } else {
        eff = MD_EFF_EXTENDED2;
        v = (uint8_t)(((d > 0 ? MD_EX2_FINE_TUNE_UP : MD_EX2_FINE_TUNE_DOWN) << 4)
                      | mag);
      }
    }
  }
  if (out_eff) *out_eff = eff;
  if (out_val) *out_val = v;
}
bool md_mdcmd_per_operator(int i) {
  return (i < 0 || i >= md_mdcmd_count()) ? false : md_mdcmds[i].per_op;
}

// Applique une commande MD au canal : on modifie la copie de travail de
// l'instrument puis on repousse les registres. Le réglage vaut jusqu'à la
// prochaine note, qui rechargera l'instrument — c'est le comportement de
// GenMDM, où un CC agit sur la voix en cours.
static bool md_noise_forced(int c) {
  if (c < 0 || c >= MD_TOTAL_VOICES) return false;
  for (int i = 0; i < MD_MDCMD_SLOTS && i < md_mdcmd_count(); i++)
    if (ch_mdcmd_on[c][i] && md_mdcmds[i].param == MD_P_NOISE) return true;
  return false;
}

static void md_apply_mdcmd(int c, uint8_t cmd, uint8_t val,
                           uint8_t row_cmd, int slot) {
  if (c < 0 || c >= MD_TOTAL_VOICES || cmd == MD_EMPTY ||
      cmd >= (uint8_t)md_mdcmd_count())
    return;

  // Priorité à la commande LSDJ. Les deux colonnes savent décrire le même
  // effet ; quand elles le font sur la même ligne, c'est CMD qui compte et
  // MD CMD se tait. C'est ce qui permet à un .dmf d'écrire tous ses effets
  // ici sans jamais contredire ce que tu as posé à la main.
  char eq = md_mdcmds[cmd].equiv;
  if (eq && row_cmd != MD_EMPTY && md_table_cmd_letter(row_cmd) == eq)
    return;

  // Les effets standard partent au moteur d'effets, dans l'emplacement
  // réservé à cette colonne : la commande CMD de la même ligne garde le sien.
  if (md_mdcmds[cmd].param == MD_P_EFFECT) {
    int eff; uint8_t v;
    md_mdcmd_resolve(cmd, val, &eff, &v);
    uint8_t e = (eff < 0) ? 0xFF : (uint8_t)eff;
    uint8_t ev = (eff < 0) ? 0 : v;
    // Même règle que pour la colonne CMD : une valeur nulle ARRÊTE un effet
    // continu. Cette colonne-ci ne l'avait pas du tout, si bien qu'un « 02 00 »
    // écrit en MD CMD n'annulait jamais rien.
    if (eff >= 0 && ev == 0 && md_effect_is_continuous(e)) {
      if (slot >= 0 && slot < MD_EFF_SLOTS) {
        ch_active_eff[c][slot] = 0xFF;
        ch_active_eff_val[c][slot] = 0;
      }
      return;
    }
    if (slot >= 0 && slot < MD_EFF_SLOTS &&
        (ch_active_eff[c][slot] != e || ch_active_eff_val[c][slot] != ev)) {
      ch_active_eff[c][slot] = e;
      ch_active_eff_val[c][slot] = ev;
      if (eff >= 0)
        md_table_latch_effect(c, e, ev);
    }
    return;
  }

  // Ce réglage vaut jusqu'à ce qu'on le change : on le retient pour pouvoir le
  // rejouer après chaque note, qui recharge l'instrument.
  if (cmd < MD_MDCMD_SLOTS) {
    ch_mdcmd_val[c][cmd] = val;
    ch_mdcmd_on[c][cmd] = true;
    // D'où vient ce réglage ? Une TABLE qu'on édite doit pouvoir défaire les
    // siens ; ceux posés dans une phrase, eux, doivent survivre.
    ch_mdcmd_from_table[c][cmd] = (slot == MD_EFF_SLOT_TABLE_MD);
  }

  md_instr_t *sh = &ch_instr_shadow[c];
  md_param_t P = md_mdcmds[cmd].param;
  bool per_op = md_mdcmds[cmd].per_op;
  // 08xx de DefleMask ne code pas la stéréo comme nous : 01 à droite,
  // 10 à gauche, 11 des deux côtés. On traduit vers nos 0/1/2.
  if (md_mdcmds[cmd].code == 0x08)
    val = (val == 0x10) ? 1 : (val == 0x01) ? 2 : 0;
  int op = ((val >> 4) & 0x0F) - 1;   // 1-4 à l'écran, 0-3 en interne
  uint8_t y = val & 0x0F;
  if (per_op && (op < 0 || op > 3))
    return;

  switch (P) {
  case MD_P_EFFECT: return;   // déjà traité plus haut ; ici pour le compilateur
  // ── Le jeu DefleMask ─────────────────────────────────────────────────
  case MD_P_LFO:
    sh->lfo_enable = ((val >> 4) & 0x0F) ? 1 : 0;
    sh->lfo_freq = y & 0x07;
    break;
  case MD_P_FB:  sh->feedback = val & 0x07; break;
  // TL sur un octet plein : c'est tout l'intérêt d'un effet par opérateur.
  case MD_P_TL1: sh->op[0].total_level = val & 0x7F; break;
  case MD_P_TL2: sh->op[1].total_level = val & 0x7F; break;
  case MD_P_TL3: sh->op[2].total_level = val & 0x7F; break;
  case MD_P_TL4: sh->op[3].total_level = val & 0x7F; break;
  case MD_P_MUL: sh->op[op].multiple = y; break;
  // « Values higher than 0x1F will be ignored », dit le manuel.
  case MD_P_ARALL:
    if (val <= 0x1F) for (int k = 0; k < 4; k++) sh->op[k].attack = val;
    break;
  case MD_P_AR1: if (val <= 0x1F) sh->op[0].attack = val; break;
  case MD_P_AR2: if (val <= 0x1F) sh->op[1].attack = val; break;
  case MD_P_AR3: if (val <= 0x1F) sh->op[2].attack = val; break;
  case MD_P_AR4: if (val <= 0x1F) sh->op[3].attack = val; break;
  case MD_P_NOISE: {
    // DefleMask : x = mode étendu (le bruit suit le ton 3), y = blanc.
    // Nos huit modes rangent le type dans le bit 2 et la période dans les
    // bits 0-1, « suit le ton 3 » étant la période 3.
    uint8_t x = (val >> 4) & 0x0F;
    sh->psg_noise = (uint8_t)((y ? 4 : 0) | (x ? 3 : 0));
    if (md_hw_is_psg(ch_hw_voice[c]) && md_hw_psg_index(ch_hw_voice[c]) == 3)
      md_chip_psg_set_noise(sh->psg_noise);
    return;
  }
  // ── En plus ──────────────────────────────────────────────────────────
  case MD_P_ALG: sh->algorithm = val & 0x07; break;
  case MD_P_PAN:
    sh->panning = (val > 2) ? 0 : val;
    ch_panning[c] = sh->panning;
    md_apply_panning(c);
    return;
  case MD_P_PMS: sh->pms = val & 0x07; break;
  case MD_P_AMS: sh->ams = val & 0x03; break;
  case MD_P_DT:  sh->op[op].detune = y & 0x07; break;
  case MD_P_RS:  sh->op[op].key_scale = y & 0x03; break;
  case MD_P_D1R: sh->op[op].decay = (uint8_t)(y * 31 / 15); break;
  case MD_P_D2R: sh->op[op].sustain_rate = (uint8_t)(y * 31 / 15); break;
  case MD_P_D1L: sh->op[op].sustain_level = y; break;
  case MD_P_RR:  sh->op[op].release = y; break;
  case MD_P_AM:  sh->op[op].am_enable = y ? 1 : 0; break;
  case MD_P_SSG: sh->op[op].ssg_eg = y; break;
  }
  md_apply_instrument((uint8_t)c);
}

// ── Correspondance des effets DefleMask ─────────────────────────────────────
// Depuis que MD CMD porte les codes du manuel, la traduction se réduit à
// chercher le code. Renvoie false pour ce que le moteur ne sait pas faire :
// 17 (DAC), 18 (mode EXT. CH3), E0, E1, E2, E3, EA, EB, EE, EF.
bool md_dmf_effect_map(uint8_t dmf_eff, uint8_t dmf_val,
                       int *out_cmd, uint8_t *out_val, bool *out_is_md) {
  int i = md_mdcmd_index_of_code(dmf_eff);
  if (i < 0)
    return false;
  if (out_cmd) *out_cmd = i;
  if (out_val) *out_val = dmf_val;
  if (out_is_md) *out_is_md = true;   // tout va dans la colonne MD CMD
  return true;
}
