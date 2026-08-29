#ifndef MD_REPLAYER_H
#define MD_REPLAYER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// MDTracker — replayer Mega Drive
//
// Le séquenceur (patterns, ordre, tempo/speed, effets) est repris tel quel du
// tracker OPL3 d'origine : mêmes numéros d'effets, mêmes comportements, même
// représentation interne de la hauteur (block/F-Num 10 bits façon OPL3). Seule
// la couche « puce » change : on écrit désormais dans un YM2612 (6 voies FM) et
// un SN76489 (3 tons + 1 bruit) au lieu d'un OPL3.
//
// Répartition des canaux :
//   0-5 : FM 1-6 (YM2612)
//   6-8 : PSG ton 1-3 (SN76489)
//   9   : PSG bruit
// ============================================================================

#define MD_NUM_FM_CHANNELS 6
#define MD_NUM_PSG_CHANNELS 4
// ── Traces de lecture ───────────────────────────────────────────────────────
// Elles ont servi à débusquer les défauts de portée (SONG / CHAIN / PHRASE) et
// restent utiles pour ça — mais elles s'impriment à CHAQUE lecture, ce qui n'a
// rien à faire dans une app publiée. On les garde, éteintes : passer ce drapeau
// à 1 les rallume sans avoir à les réécrire.
#ifndef MD_TRACE_PLAY
#define MD_TRACE_PLAY 0
#endif
#if MD_TRACE_PLAY
#include <stdio.h>
#define MD_TRACE(...) do { printf(__VA_ARGS__); fflush(stdout); } while (0)
#else
#define MD_TRACE(...) do { } while (0)
#endif

#define MD_MAX_CHANNELS (MD_NUM_FM_CHANNELS + MD_NUM_PSG_CHANNELS) // 10
#define MD_PSG_FIRST_CHANNEL MD_NUM_FM_CHANNELS                    // 6
#define MD_PSG_NOISE_CHANNEL (MD_MAX_CHANNELS - 1)                 // 9

// Voie d'audition (piano / prévisualisation d'instrument). Elle ne fait pas
// partie de la chanson : elle emprunte une voie du même type que le canal édité
// (cf. md_replayer_set_audition_channel) — une voie FM libre si le curseur est
// sur FM1-6, la voie PSG correspondante s'il est sur PSG1-3 ou NOIS.
#define MD_TEST_CHANNEL MD_MAX_CHANNELS // 10
#define MD_TOTAL_VOICES (MD_MAX_CHANNELS + 1)

#define MD_MAX_INSTRUMENTS 255

// Longueur maximale de la table de volumes d'une voie PSG (un pas = un tick).
#define MD_PSG_ENV_MAX 32
// Longueur maximale des macros PSG importées de DefleMask (il en autorise 127).
#define MD_PSG_MACRO_MAX 128

// Puce visée par un instrument (champ `kind`).
#define MD_INSTR_KIND_ANY 0   // écrit à la main : joue sur toutes les colonnes
#define MD_INSTR_KIND_FM  1   // YM2612 seulement
#define MD_INSTR_KIND_PSG 2   // SN76489 seulement
#define MD_INSTR_KIND_PCM 3   // échantillon, sur la voie PCM (FM6)

// ── Voie PCM ────────────────────────────────────────────────────────────────
// La voie PCM, c'est la 6ᵉ voie FM : le registre 2B bit 7 la débranche de la
// synthèse pour en faire le convertisseur.
#define MD_PCM_CHANNEL (MD_NUM_FM_CHANNELS - 1)

// ── Atténuation générale des PORTEUSES FM, en pas de Total Level ───────────
//
// Un pas de TL vaut 0,75 dB : 16 pas = 12 dB. Elle ne s'applique QU'AUX
// porteuses de l'algorithme en cours, exactement comme la colonne VEL.
//
// À quoi elle sert. Le bruit du SN76489 et le convertisseur du YM2612 ont un
// PLAFOND matériel : dans le morceau d'essai, l'instrument de bruit est déjà à
// 14 sur 15 et les échantillons à 91-99 % de la pleine échelle. Impossible,
// donc, de les monter davantage sur une vraie machine. Le seul geste légal
// pour qu'ils ressortent est de BAISSER la FM — et c'est du Total Level, ce
// qu'un pilote Mega Drive écrit sans difficulté. Une ROM exportée reproduira
// donc l'équilibre entendu ici, ce que les relèvements de mixage qu'on avait
// d'abord posés (bruit ×4, PCM ×1,8) ne pouvaient pas faire : ils n'existent
// que dans notre mélangeur.
//
// Ce qu'elle coûte, mesuré et pas supposé. Les modulateurs ne sont pas touchés,
// donc la modulation — la vraie source du grain FM — est intacte. Restent deux
// effets secondaires, tous deux réels sur la puce :
//
//  1. Atténuer DANS la puce plutôt que dans le mixage ajoute environ 26 dB
//     sous le signal de bruit large bande, sa quantification. Constant quelle
//     que soit la profondeur (-26,2 dB à 4,5 dB d'atténuation, -25,1 dB à
//     13,5 dB) : il apparaît dès le premier pas et n'empire pas ensuite.
//
//  2. Le ladder effect des Mega Drive 1, lui, a une amplitude CONSTANTE : il
//     ne suit pas le signal, donc il ressort quand la FM baisse. Mesuré : 196
//     de valeur efficace sans atténuation, 201 avec — soit -28,9 dB puis
//     -15,2 dB sous la FM. C'est pour ça que la console émulée par défaut est
//     une Mega Drive 2, qui n'a pas ce défaut (bouton MD1/MD2 dans Config).
//
// ⚠️ À ZÉRO, et c'est délibéré. Cette atténuation n'existait que pour compenser
// un rapport FM/PSG faux dans notre mélangeur : le PSG y sortait dix décibels
// sous une voie FM, alors que sur la console les deux se tiennent. Le rapport
// est corrigé (voir kPSGLevel), donc il n'y a plus rien à compenser — et on
// évite du même coup ce qu'elle coûtait : le grain de quantification, le cas
// tordu de la porteuse bouclée, et une valeur choisie sur UN morceau, ce qui
// n'a pas de sens pour les autres.
//
// Elle reste là pour qui veut délibérément faire ressortir le PSG et le PCM au
// détriment de la FM, en sachant que c'est un choix global et non un fait.
#define MD_FM_CARRIER_ATTEN 0

// ── Garder le grain d'une voix bouclée ────────────────────────────────────
//
// Sur le YM2612, le Total Level d'OP1 règle À LA FOIS son niveau et
// l'intensité du feedback. Atténuer une voix dont OP1 est porteuse et bouclée
// — « Ins 0 » du morceau d'essai, algorithme 7, feedback 7 — lui retire donc
// son mordant : elle sort étouffée, sans dynamique. Mesuré : dès 6 dB la
// ressemblance du spectre tombe à 0,60 et le centre spectral perd 59 %, et ça
// ne bouge plus ensuite. C'est une falaise, pas une pente, et aucune valeur
// d'atténuation n'y échappe.
//
// À 1, OP1 est épargnée dans ce cas précis et ses voisines encaissent
// l'atténuation. Le timbre est préservé (ressemblance 0,98) et la voix descend
// quand même, puisque ses autres porteuses baissent — simplement moins que le
// reste. C'est le contournement des musiciens sur la vraie machine, et il
// s'exporte comme le reste : ce ne sont que des Total Level.
//
// Contrepartie, mesurée sur le morceau d'essai : l'écart entre cette voix et
// l'accompagnement se creuse à mesure qu'on atténue — 2,8 dB sans atténuation,
// 5,2 dB à 7,5 dB, 9,5 dB à 13,5 dB. C'est ce qui borne MD_FM_CARRIER_ATTEN
// bien plus que le bruit lui-même.
#define MD_FM_KEEP_FEEDBACK_GRIT 1

// ── Le cas de la porteuse bouclée ─────────────────────────────────────────
//
// Épargner OP1 sauve son grain, mais laissait la voix debout pendant que tout
// l'orchestre descendait : FM2 tombait à -5,2 dB de FM1 là où elle est à
// -2,8 dB sans atténuation — l'équilibre voulu par le compositeur.
//
// La compensation appliquée à ses VOISINES n'est donc pas une constante mais un
// CALCUL, fait dans md_write_carrier_volume à partir des Total Level de
// l'instrument (voir là-bas). Une valeur fixe ne pouvait pas convenir : elle
// dépend de la répartition des niveaux, propre à chaque patch. Réglée à la main
// sur celui du morceau d'essai, elle laissait 3 dB d'écart sur un autre.
//
// Résultat mesuré, cinq familles d'instruments, cible -7,5 dB :
//   ALG 0 FB 0  -7,54    ALG 4 FB 0  -7,52    ALG 6 FB 7  -7,51
//   ALG 7 FB 0  -7,55    ALG 7 FB 7 (bouclée)  -7,76
// — soit un quart de décibel d'écart, contre trois avant le calcul.
//
// Ce qui reste : OP1 n'encaisse rien, c'est tout l'intérêt.
#define MD_FM_FEEDBACK_OP1_ATTEN 0


// Cadence du PILOTE, pas des données : sur une vraie Mega Drive c'est un Z80 à
// 3,58 MHz qui écrit le DAC octet par octet, et l'intervalle qu'il tient EST la
// fréquence d'échantillonnage.
//
// La banque n'a qu'UNE cadence, comme une cartouche : le pilote débite les
// octets à ce rythme et rien d'autre. Tout ce qui entre y est ramené —
// un fichier audio chargé par l'utilisateur comme un échantillon de .dmf, qui
// arrive lui à 22 050 ou 32 000 Hz. La hauteur est conservée par le
// rééchantillonnage, donc cette constante ne règle PAS l'accord : seulement la
// bande passante et la place occupée.
//
// 32 kHz parce que c'est la plus haute cadence qu'emploient les échantillons
// de DefleMask sur Mega Drive : en dessous, on jetterait de l'aigu à
// l'importation ; au-dessus, on gonflerait la banque sans rien gagner.
//
// J'ai perdu du temps sur ce réglage en croyant que la cadence du pilote
// portait la hauteur — elle ne la porte que si l'on NE rééchantillonne PAS,
// ce qui était mon erreur précédente.
//
// Changer cette valeur désaccorde les échantillons des morceaux DÉJÀ
// enregistrés : le fichier ne la mémorise pas.
#define MD_PCM_RATE 32000
#define MD_MAX_SAMPLES 32
#define MD_PCM_BANK_BYTES (512 * 1024)   // ~16 secondes à 32 kHz

// Nombre de points de rupture de l'enveloppe PSG (l'écran ENV. de LSDJ en
// affiche trois, sous forme de trois paires « amplitude / vitesse »).
#define MD_ENV_POINTS 3

// ── Tables d'instrument (façon LSDJ) ────────────────────────────────────────
// Une table est une petite séquence de 16 lignes déroulée un pas par tick tant
// que la note dure, et rebouclée. Chaque ligne peut fixer un volume, transposer
// la note, et déclencher jusqu'à deux commandes. C'est le mécanisme qui remplace
// l'arpégiateur : une table 0/4/7 rejoue un accord parfait, mais elle sait faire
// bien plus (fondus, coupures, sauts d'octave, effets).
// Une table s'attache à n'importe quel instrument, FM comme PSG.
#define MD_MAX_TABLES 32
#define MD_TABLE_ROWS 16

typedef struct {
  // Volume sur un octet plein, comme la colonne VOL de LSDJ : le chiffre de
  // gauche est le poids fort, celui de droite le poids faible.
  // ⚠️ 0 veut dire « ne touche pas au volume », exactement comme TSP 0 veut
  // dire « ne transpose pas ». Une table neuve est donc entièrement neutre.
  uint8_t vol;
  int8_t transpose;   // demi-tons ajoutés à la note en cours (0 = neutre)
  uint8_t cmd1, val1; // MD_EMPTY = pas de commande
  uint8_t cmd2, val2;
  // Troisième colonne : les commandes MACHINE (registres YM2612 / SN76489),
  // le même vocabulaire que la colonne MD CMD d'une phrase. Elle est portée
  // par le même flux que la seconde colonne CMD.
  uint8_t mdcmd, mdval;
} md_table_row_t;

typedef struct {
  md_table_row_t rows[MD_TABLE_ROWS];
} md_table_t;

// ── Structure du morceau, à la LSDJ ────────────────────────────────────────
// SONG   : 256 lignes × 10 canaux, chaque case contient un numéro de CHAIN.
// CHAIN  : 16 lignes, chacune un numéro de PHRASE + une transposition.
// PHRASE : 16 lignes, chacune note / instrument / vélocité / commande.
// Chaque canal parcourt SA PROPRE colonne indépendamment des autres — c'est
// ce qui fait qu'une piste peut boucler sur 1 chain pendant qu'une autre en
// enchaîne dix.
#define MD_SONG_ROWS 256
#define MD_MAX_CHAINS 128
#define MD_MAX_PHRASES 255
#define MD_ROWS_PER_CHAIN 16
#define MD_ROWS_PER_PHRASE 16

// Valeur « case vide » pour tous les champs 8 bits du morceau.
#define MD_EMPTY 0xFF

// ── Tessiture ───────────────────────────────────────────────────────────────
// Le YM2612 code la hauteur en « block » (l'octave, 0-7) + F-Num. Le bloc 0
// donne le do le plus grave que la puce sache produire : 16.35 Hz. C'est ce do
// qui est nommé C-0 dans le tracker — on ne peut donc pas descendre plus bas,
// et C-0 est bien la note la plus grave de la Megadrive.
// Vers l'aigu, le F-Num sature à 2047 dans le bloc 7, soit ~6.6 kHz : la
// tessiture utile couvre 9 octaves, C-0 à B-8.
// (Le PSG, lui, plafonne par construction vers 109 Hz : sa période n'a que
//  10 bits. En dessous, les canaux PSG restent bloqués sur cette note.)
#define MD_MAX_NOTE 108   // 9 octaves × 12 demi-tons : C-0 … B-8
#define MD_MAX_BLOCK 8    // octave la plus haute atteignable

// True si le canal est une voie PSG.
#define MD_IS_PSG_CHANNEL(c) ((c) >= MD_PSG_FIRST_CHANNEL && (c) < MD_MAX_CHANNELS)

// ============================================================================
// Instruments
// ============================================================================

// Paramètres d'un opérateur du YM2612.
typedef struct {
  uint8_t detune;       // DT1  0-7   (4-7 = détune négatif)
  uint8_t multiple;     // MUL  0-15
  uint8_t total_level;  // TL   0-127 (atténuation, 0 = fort)
  uint8_t key_scale;    // RS   0-3
  uint8_t attack;       // AR   0-31
  uint8_t decay;        // D1R  0-31
  uint8_t sustain_rate; // D2R  0-31
  uint8_t sustain_level;// D1L  0-15
  uint8_t release;      // RR   0-15
  uint8_t am_enable;    // AM   0/1
  uint8_t ssg_eg;       // SSG-EG 0-15 (bit 3 = enable)
} md_op_params_t;

#pragma pack(push, 1)
typedef struct {
  md_op_params_t op[4]; // opérateurs 1 à 4

  uint8_t algorithm;  // ALG 0-7
  uint8_t feedback;   // FB  0-7
  uint8_t ams;        // AMS 0-3
  uint8_t pms;        // PMS 0-7
  uint8_t lfo_enable; // 0/1
  uint8_t lfo_freq;   // 0-7

  uint8_t panning;   // 0 = centre, 1 = gauche, 2 = droite
  int8_t fine_tune;  // -128..127 (ajouté au F-Num)
  uint8_t psg_noise; // mode bruit PSG 0-7 (n'agit que sur le canal de bruit)

  // ── Enveloppe des voies PSG (écran ENV. de LSDJ) ──────────────────────────
  //
  // Le SN76489 n'a aucune enveloppe matérielle : son seul contrôle de niveau
  // est un registre d'atténuation sur 4 bits, que le CPU réécrit à chaque tick.
  // C'est ce que font SMPS, GEMS ou Echo sur une vraie Megadrive.
  //
  // Le modèle est celui de LSDJ, mot pour mot d'après son manuel : trois points
  // de rupture, affichés en trois paires « amplitude / vitesse » —
  //
  //     ENV.  88 / 00 / --
  //
  //   • le PREMIER digit fixe l'amplitude atteinte à ce point (0-F) ;
  //   • le SECOND fixe la vitesse pour rejoindre l'amplitude du point SUIVANT.
  //
  // ⚠️ La vitesse est inversée par rapport à l'intuition : 1 est la plus rapide,
  // F la plus lente, et 0 veut dire « tenir » — on reste à cette amplitude
  // indéfiniment. Une paire à MD_EMPTY est désactivée, et désactive les
  // suivantes.
  //
  // Exemple du manuel, 32/AD/10 : attaque rapide de l'amplitude 3 vers A,
  // déclin lent vers 1, puis maintien infini.
  uint8_t env_amp[MD_ENV_POINTS];      // 0-15, MD_EMPTY = point désactivé
  uint8_t env_speed[MD_ENV_POINTS];    // 0 = tenir, 1 = le plus rapide, 15 = le plus lent
  uint8_t env_pad[29];                 // ancien emplacement de la table de volumes

  // Table attachée à l'instrument (MD_EMPTY = aucune). Elle remplace l'ancien
  // arpégiateur : sa colonne TSP fait le même travail, en mieux.
  uint8_t table;
  uint8_t table_pad[33];               // ancien emplacement de la macro d'arpège

  // ── Macro de mode de bruit (canal NOISE uniquement) ───────────────────────
  // Une valeur 0-7 par tick, écrite dans le registre de bruit du SN76489.
  // Permet les bruits qui changent de grain pendant la note.
  uint8_t psg_noise_len;
  uint8_t psg_noise_loop;
  uint8_t psg_noise_mac[MD_PSG_ENV_MAX];

  // ── Macros PSG, au sens de DefleMask ──────────────────────────────────────
  // Un pas par tick, contrairement à l'enveloppe LSDJ ci-dessus qui interpole
  // entre trois points. Les deux coexistent : l'enveloppe sert à ce qu'on écrit
  // à la main, les macros à ce qui vient d'un fichier importé. Quand une macro
  // de volume existe, c'est ELLE qui commande le niveau.
  //
  // Une table attachée à l'instrument continue de s'appliquer PAR-DESSUS :
  // sa colonne TSP s'ajoute à l'arpège de la macro, sa colonne VOL écrase le
  // niveau pour le tick où elle est posée.
  uint8_t psg_vol_len;                    // 0 = pas de macro de volume
  uint8_t psg_vol_loop;                   // MD_EMPTY = pas de bouclage
  uint8_t psg_vol_mac[MD_PSG_MACRO_MAX];  // niveaux 0-15

  uint8_t psg_arp_len;                    // 0 = pas de macro d'arpège
  uint8_t psg_arp_loop;                   // MD_EMPTY = pas de bouclage
  uint8_t psg_arp_fixed;                  // 1 = notes absolues, 0 = écarts
  int8_t  psg_arp_mac[MD_PSG_MACRO_MAX];  // demi-tons signés

  // ── Puce à laquelle cet instrument est destiné ────────────────────────────
  // Un instrument importé d'un .dmf garde le type qu'il avait dans DefleMask.
  // Un instrument PSG n'a AUCUN paramètre FM : posé sur une voie FM, DefleMask
  // le laisse muet, et nous devons faire pareil — sinon on entend un son que
  // le morceau d'origine ne produit pas.
  //
  // MD_INSTR_KIND_ANY est la valeur des instruments écrits à la main : ils se
  // jouent sur n'importe quelle colonne, comme avant.
  uint8_t kind;

  // Échantillon joué par cet instrument (index 0…MD_MAX_SAMPLES-1), quand
  // `kind` vaut MD_INSTR_KIND_PCM.
  uint8_t pcm_sample;

  // Volume de l'échantillon. 7F = unité ; au-delà, jusqu'à FF, l'échantillon
  // est POUSSÉ et ce qui dépasse est écrêté. Un coup percussif a une valeur
  // efficace dix décibels sous une note FM tenue à crête égale — c'est ce qui
  // le fait paraître faible — et le pousser est le seul geste qui remonte cette
  // valeur efficace sans toucher au reste. Un pilote fait pareil avant d'écrire
  // ses octets, donc une ROM le reproduit. Le DAC du YM2612 n'a AUCUN
  // registre de volume : c'est le pilote qui met les octets à l'échelle avant
  // de les écrire, et c'est exactement ce que fait notre voie PCM. Sans ce
  // réglage, la seule façon de baisser un échantillon était la colonne VEL,
  // ligne par ligne.
  uint8_t pcm_volume;

  uint8_t reserved[6];
} md_instr_t;
#pragma pack(pop)

// Identifiants de propriété utilisés par l'UI (éditeur d'instrument).
// Par opérateur (md_replayer_set_instr_op_val) :
typedef enum {
  MD_OP_PROP_MULTIPLE = 0,      // 0-15
  MD_OP_PROP_KEY_SCALE = 1,     // RS   0-3
  MD_OP_PROP_AM = 2,            // 0/1
  MD_OP_PROP_DETUNE = 3,        // 0-7
  MD_OP_PROP_SSG_EG = 4,        // 0-15
  MD_OP_PROP_SUSTAIN_RATE = 5,  // D2R  0-31
  MD_OP_PROP_TOTAL_LEVEL = 6,   // TL   0-127
  MD_OP_PROP_ATTACK = 7,        // AR   0-31
  MD_OP_PROP_DECAY = 8,         // D1R  0-31
  MD_OP_PROP_SUSTAIN_LEVEL = 9, // D1L  0-15
  MD_OP_PROP_RELEASE = 10,      // RR   0-15
} md_op_prop_t;

// Globaux (md_replayer_set_instr_gen_val) :
typedef enum {
  MD_GEN_PROP_ALGORITHM = 0, // 0-7
  MD_GEN_PROP_FEEDBACK = 1,  // 0-7
  MD_GEN_PROP_PANNING = 2,   // 0-2
  MD_GEN_PROP_FINE_TUNE = 3, // -128..127
  MD_GEN_PROP_AMS = 4,       // 0-3
  MD_GEN_PROP_PMS = 5,       // 0-7
  MD_GEN_PROP_LFO_ENABLE = 6,// 0/1
  MD_GEN_PROP_LFO_FREQ = 7,  // 0-7
  MD_GEN_PROP_PSG_NOISE = 8, // 0-7
} md_gen_prop_t;

// ============================================================================
// Effets — numérotation identique au tracker d'origine pour que l'édition,
// l'aide FX et les raccourcis restent strictement les mêmes.
// ============================================================================
typedef enum {
  MD_EFF_ARPEGGIO = 0,
  MD_EFF_FSLIDE_UP = 1,
  MD_EFF_FSLIDE_DOWN = 2,
  MD_EFF_TONE_PORTAMENTO = 3,
  MD_EFF_VIBRATO = 4,
  MD_EFF_TPORTA_VSLIDE = 5,
  MD_EFF_VIBRATO_VSLIDE = 6,
  MD_EFF_FSLIDE_UP_FINE = 7,
  MD_EFF_FSLIDE_DOWN_FINE = 8,
  MD_EFF_SET_MOD_VOL = 9,   // volume des opérateurs modulateurs
  MD_EFF_VOL_SLIDE = 10,
  MD_EFF_POSITION_JUMP = 11,
  MD_EFF_SET_INS_VOLUME = 12,
  MD_EFF_PATTERN_BREAK = 13,
  MD_EFF_SET_TEMPO = 14,
  MD_EFF_SET_SPEED = 15,
  MD_EFF_TPORTA_VSLIDE_FINE = 16,
  MD_EFF_VIBRATO_VSLIDE_FINE = 17,
  MD_EFF_SET_CAR_VOL = 18,  // volume des opérateurs porteurs
  MD_EFF_SET_ALGORITHM = 19,// (ex « set waveform ») ALG haut / FB bas
  MD_EFF_VOL_SLIDE_FINE = 20,
  MD_EFF_RETRIG_NOTE = 21,
  MD_EFF_TREMOLO = 22,
  MD_EFF_TREMOR = 23,
  MD_EFF_ARPEGGIO_VSLIDE = 24,
  MD_EFF_ARPEGGIO_VSLIDE_FINE = 25,
  MD_EFF_MULTI_RETRIG_NOTE = 26,
  MD_EFF_FSLIDE_UP_VSLIDE = 27,
  MD_EFF_FSLIDE_DOWN_VSLIDE = 28,
  MD_EFF_FSLIDE_UP_FINE_VSLIDE = 29,
  MD_EFF_FSLIDE_DOWN_FINE_VSLIDE = 30,
  MD_EFF_FSLIDE_UP_VSLF = 31,
  MD_EFF_FSLIDE_DOWN_VSLF = 32,
  MD_EFF_FSLIDE_UP_FINE_VSLF = 33,
  MD_EFF_FSLIDE_DOWN_FINE_VSLF = 34,
  MD_EFF_EXTENDED = 35,
  MD_EFF_EXTENDED2 = 36,
  MD_EFF_SET_GLOBAL_VOLUME = 37,
  MD_EFF_SWAP_ARPEGGIO = 38,
  MD_EFF_SWAP_VIBRATO = 39,
  MD_EFF_FORCE_INS_VOLUME = 40,
  MD_EFF_EXTENDED3 = 41,   // réglages YM2612 par opérateur
  MD_EFF_EXTRA_FINE_ARPEGGIO = 42,
  MD_EFF_EXTRA_FINE_VIBRATO = 43,
  MD_EFF_EXTRA_FINE_TREMOLO = 44,
  MD_EFF_SET_CUSTOM_SPEED_TAB = 45,
  MD_EFF_GLOBAL_FSLIDE_UP = 46,
  MD_EFF_GLOBAL_FSLIDE_DOWN = 47
} md_effect_type_t;

typedef enum {
  MD_EX_SET_TREM_DEPTH = 0,
  MD_EX_SET_VIB_DEPTH = 1,
  MD_EX_SET_PANNING_POS = 11,
  MD_EX_PATTERN_LOOP = 12,
  MD_EX_PATTERN_LOOP_REC = 13
} md_ex_effect_type_t;

typedef enum {
  MD_EX2_PAT_DELAY_FRAME = 0,
  MD_EX2_PAT_DELAY_ROW = 1,
  MD_EX2_NOTE_DELAY = 2,
  MD_EX2_NOTE_CUT = 3,
  MD_EX2_FINE_TUNE_UP = 4,
  MD_EX2_FINE_TUNE_DOWN = 5,
  MD_EX2_GL_VOL_SLIDE_UP = 6,
  MD_EX2_GL_VOL_SLIDE_DN = 7,
  MD_EX2_GL_VOL_SLIDE_UP_F = 8,
  MD_EX2_GL_VOL_SLIDE_DN_F = 9,
  MD_EX2_GL_VOL_SLIDE_UP_XF = 10,
  MD_EX2_GL_VOL_SLIDE_DN_XF = 11,
} md_ex2_effect_type_t;

// ============================================================================
// Données du module
// ============================================================================

// Une ligne de CHAIN : le numéro de phrase à jouer + sa transposition.
typedef struct {
  uint8_t phrase;   // MD_EMPTY = case vide
  int8_t transpose; // en demi-tons
} md_chain_row_t;

typedef struct {
  md_chain_row_t rows[MD_ROWS_PER_CHAIN];
} md_chain_t;

// Une ligne de PHRASE. Colonnes affichées : NOTE, INS, VEL, CMD, VAL.
typedef struct {
  uint8_t note;   // 0 = vide, 1-MD_MAX_NOTE = note, MD_EMPTY = note-off
  uint8_t instr;  // 1-255, 0 = vide
  uint8_t vel;    // 0-127 (7F = plein, l'échelle de DefleMask),
                  // MD_EMPTY = vide : on garde le volume courant
  uint8_t cmd;    // indice de commande, MD_EMPTY = vide
  uint8_t cmdval; // paramètre de la commande
  // Seconde colonne de commande : les réglages de la MACHINE, pas de la
  // séquence. Elle pilote en direct les registres du YM2612 et du SN76489,
  // comme le fait GenMDM par messages MIDI CC.
  uint8_t mdcmd;  // indice de commande MD, MD_EMPTY = vide
  uint8_t mdval;
} md_phrase_row_t;

typedef struct {
  md_phrase_row_t rows[MD_ROWS_PER_PHRASE];
} md_phrase_t;

// Événement résolu, transmis au moteur d'effets hérité (qui n'a pas bougé).
typedef struct {
  uint8_t note;
  uint8_t instr;
  struct {
    uint8_t type;
    uint8_t val;
  } effects[2];
} md_event_t;

// ── Un échantillon de la banque ─────────────────────────────────────────────
// Les données sont déjà en 8 bits non signés à MD_PCM_RATE : c'est exactement
// ce que recevra le DAC. La conversion se fait à l'import, une fois pour
// toutes, pour que la lecture n'ait plus rien à calculer.
typedef struct {
  char name[24];
  uint32_t offset;      // position dans la banque
  uint32_t length;      // longueur en octets (= en pas)
  int32_t loop;         // point de bouclage, -1 = aucun
  uint8_t base_note;    // note à laquelle il joue à sa cadence naturelle
  uint8_t reserved[3];
} md_sample_t;

typedef struct {
  char songname[43];
  char composer[43];
  char instr_names[MD_MAX_INSTRUMENTS][43];

  md_instr_t instruments[MD_MAX_INSTRUMENTS];

  // [canal][ligne] → numéro de chain, MD_EMPTY = vide
  uint8_t song[MD_MAX_CHANNELS][MD_SONG_ROWS];
  md_chain_t chains[MD_MAX_CHAINS];
  md_phrase_t phrases[MD_MAX_PHRASES];
  md_table_t tables[MD_MAX_TABLES];

  uint8_t initial_tempo;
  uint8_t initial_speed;
  uint8_t common_flag;
  uint8_t num_channels;
  uint16_t macro_speedup;
  int16_t bpm_tempo_finetune;
  uint8_t bpm_rows_per_beat;

  // ── Banque d'échantillons ─────────────────────────────────────────────────
  md_sample_t samples[MD_MAX_SAMPLES];
  uint32_t pcm_used;                    // octets occupés dans la banque
  uint8_t pcm[MD_PCM_BANK_BYTES];
} md_module_t;

typedef enum {
  MD_STOPPED = 0,
  MD_PLAYING = 1,
  MD_PAUSED = 2
} md_play_status_t;

// ============================================================================
// API publique du replayer
// ============================================================================

void md_replayer_init(int sample_rate);
void md_replayer_shut(void);

// Charge un module .mdm depuis la mémoire.
bool md_replayer_load_mem(const uint8_t *data, uint32_t size);

bool md_replayer_play(void);
void md_replayer_stop(void);
bool md_replayer_is_playing(void);

void md_replayer_new_empty(void);
void md_replayer_init_default_instrument(int index);

// Sérialise le module courant au format .mdm. À libérer avec free().
uint8_t *md_replayer_save_mem(uint32_t *out_size);

int md_replayer_get_tempo(void);
void md_replayer_set_tempo(int tempo);
int md_replayer_get_speed(void);
void md_replayer_set_speed(int speed);
int md_replayer_get_rows_per_beat(void);
void md_replayer_set_rows_per_beat(int rpb);
double md_replayer_get_bpm(void);
void md_replayer_set_bpm(double bpm);

// Génère un bloc audio (stéréo 16 bits entrelacé). `length_bytes` = frames * 4.
void md_replayer_update(uint8_t *stream, int length_bytes);

// ── SONG ────────────────────────────────────────────────────────────────
// Numéro de chain posé sur (canal, ligne). MD_EMPTY = case vide.
uint8_t md_replayer_get_song(int channel, int row);
void md_replayer_set_song(int channel, int row, uint8_t chain);

// ── CHAIN ───────────────────────────────────────────────────────────────
void md_replayer_get_chain(int chain, int row, uint8_t *phrase, int8_t *transpose);
void md_replayer_set_chain(int chain, int row, uint8_t phrase, int8_t transpose);

// ── PHRASE ──────────────────────────────────────────────────────────────
// ── Import des fichiers .dmf de DefleMask ───────────────────────────────────
// Le compte rendu sert à dire honnêtement ce qui est passé et ce qui ne l'est
// pas : le format a des choses que ce moteur ne sait pas faire.
typedef struct {
  char song[64];
  char author[64];
  int version;                    // version du format (27 = DefleMask 1.1)
  int system;                     // 0x02 Mega Drive, 0x42 canal 3 éclaté
  bool ext_ch3;                   // morceau en mode CH3 éclaté : non géré
  int orders;                     // lignes de la matrice
  int rows_per_pattern;
  int instruments;
  int instruments_approximated;   // macros PSG plus longues que nos réglages
  int phrases_used;
  int chains_used;
  int effects_unsupported;        // effets sans équivalent (DAC, CH3, …)
  int effects_dropped;            // plus d'un effet sur une même ligne
  int row_divisor;                // lignes DefleMask fondues en une des nôtres
  int rows_merged;                // évènements perdus par cette fusion
  int patterns_missing;           // la matrice renvoyait à un motif absent
  int samples_imported;           // échantillons repris dans la banque
  int samples_ignored;            // échantillons laissés de côté (banque pleine)
  bool truncated;                 // le morceau dépassait nos réserves
} md_dmf_report_t;

// Remplace le morceau courant par le contenu du fichier .dmf DÉCOMPRESSÉ.
// Renvoie false si ce n'est pas un .dmf Mega Drive lisible.
bool md_replayer_import_dmf(const uint8_t *data, uint32_t size,
                            md_dmf_report_t *report);

// ── Correspondance des effets DefleMask ─────────────────────────────────────
// Traduit un effet du manuel DefleMask (Megadrive) vers une commande de ce
// tracker, pour l'import des fichiers .dmf.
//
// Quand un effet a un équivalent LSDJ, c'est LUI qui est choisi : il n'existe
// pas deux façons de faire la même chose dans le tracker, et le fichier
// importé se retrouve écrit dans le vocabulaire du logiciel.
//
// `dmf_eff` est le code d'effet tel que le manuel le nomme (00-20, et E0-EF
// pour les commandes étendues). Renvoie true si l'effet est traduisible.
// `out_is_md` dit dans laquelle des deux colonnes écrire.
bool md_dmf_effect_map(uint8_t dmf_eff, uint8_t dmf_val,
                       int *out_cmd, uint8_t *out_val, bool *out_is_md);

// ── Commandes « MD » : les réglages de la machine ───────────────────────────
// Seconde colonne de commande d'une phrase. Elle reprend les paramètres que
// GenMDM expose en MIDI CC, écrits ici sous la forme « lettre + deux chiffres ».
//
// Pour un paramètre d'OPÉRATEUR, le premier chiffre désigne l'opérateur (1-4)
// et le second la valeur ; pour un paramètre de CANAL, les deux chiffres
// forment la valeur.
int md_mdcmd_count(void);
// La colonne MD CMD identifie ses commandes par le code d'effet DefleMask
// (deux chiffres hexadécimaux), pas par une lettre : elle en porte quarante-
// quatre, l'alphabet n'y suffisait pas — et c'est le codage du manuel.
uint8_t md_mdcmd_code(int i);
int md_mdcmd_index_of_code(uint8_t code);
bool md_mdcmd_is_effect(int i);
char md_mdcmd_equivalent(int i);
void md_mdcmd_resolve(int i, uint8_t val, int *out_eff, uint8_t *out_val);
// Le premier chiffre est-il un numéro d'opérateur ?
bool md_mdcmd_per_operator(int i);

void md_replayer_get_phrase(int phrase, int row, uint8_t *note, uint8_t *instr,
                            uint8_t *vel, uint8_t *cmd, uint8_t *cmdval,
                            uint8_t *mdcmd, uint8_t *mdval);
void md_replayer_set_phrase(int phrase, int row, uint8_t note, uint8_t instr,
                            uint8_t vel, uint8_t cmd, uint8_t cmdval,
                            uint8_t mdcmd, uint8_t mdval);

// ── Têtes de lecture (pour le repère « > » de chaque écran) ─────────────
// -1 = ce canal ne joue pas.
int md_replayer_play_song_row(int channel);
int md_replayer_play_chain(int channel);   // numéro de chain en cours
int md_replayer_play_chain_row(int channel);
int md_replayer_play_phrase(int channel);  // numéro de phrase en cours
int md_replayer_play_phrase_row(int channel);

// Démarre la lecture à partir d'une ligne du SONG.
void md_replayer_play_from(int song_row);
// Ligne de départ dans l'écran CHAIN (le curseur), avant md_replayer_play().
void md_replayer_set_scope_row(int row);

// ── Portée de la lecture (play contextuel façon LSDJ) ───────────────────────
// Depuis l'écran SONG on joue toute la chanson ; depuis CHAIN, seulement le
// chain pointé ; depuis PHRASE, seulement la phrase pointée. Dans les deux
// derniers cas un seul canal sonne — celui de la colonne où est le curseur —
// et la lecture boucle sur elle-même au lieu de retomber dans le song.
typedef enum {
  MD_SCOPE_SONG = 0,   // toutes les colonnes, déroulement normal
  MD_SCOPE_CHAIN = 1,  // un seul canal, un seul chain, en boucle
  MD_SCOPE_PHRASE = 2, // un seul canal, une seule phrase, en boucle
} md_play_scope_t;

// `id` est le numéro de chain ou de phrase selon la portée ; il est ignoré pour
// MD_SCOPE_SONG. À appliquer AVANT de lancer la lecture.
void md_replayer_set_play_scope(int scope, int channel, int id);

// ── Mode LIVE ───────────────────────────────────────────────────────────
// En mode LIVE, la lecture ne démarre PAS toutes les colonnes d'un coup :
// chaque canal est lancé à la main, quand on l'arme. C'est le mode de jeu
// scénique de LSDJ.
void md_replayer_set_live_mode(bool live);
bool md_replayer_is_live_mode(void);
// Arme un canal pour qu'il démarre à cette ligne du SONG, à la prochaine
// ligne jouée (quantisation). -1 = désarme et coupe le canal.
void md_replayer_live_arm(int channel, int song_row);
// Ligne armée en attente sur ce canal (-1 = rien en attente) : elle clignote.
int md_replayer_live_pending(int channel);

const char *md_replayer_get_instr_name(int index);
void md_replayer_set_instr_name(int index, const char *name);

md_module_t *md_replayer_get_module(void);

extern uint8_t md_current_row;
extern bool md_loop_pattern;

void md_replayer_play_test_note(int note, int instr, int channel);
void md_replayer_stop_test_note(int channel);
void md_replayer_set_test_arpeggio(int channel, int val);
void md_replayer_prepare_test_channel(int instr);

// Canal de la chanson que le curseur d'édition occupe. L'audition (piano,
// saisie de note) jouera sur une voie du même type : FM si le curseur est sur
// FM1-6, la voie PSG correspondante s'il est sur PSG1-3 ou NOIS.
void md_replayer_set_audition_channel(int channel);

// ── Modèle de puce FM ───────────────────────────────────────────────────────
// Vrai = YM2612 discrète des Mega Drive 1, avec son défaut de convertisseur
// (le « ladder effect » : un décrochage autour de zéro qui injecte une
// distorsion de croisement, une bonne part du grain sale de la console).
// Faux = YM3438 CMOS des Model 2 tardives, sans ce défaut.
void md_replayer_set_ladder(bool enabled);
bool md_replayer_get_ladder(void);

void md_replayer_set_instr_op_val(int ins_idx, int op_idx, int prop_id,
                                  uint8_t val);
uint8_t md_replayer_get_instr_op_val(int ins_idx, int op_idx, int prop_id);
void md_replayer_set_instr_gen_val(int ins_idx, int prop_id, int val);
int md_replayer_get_instr_gen_val(int ins_idx, int prop_id);

// ── Table de volumes des voies PSG ──────────────────────────────────────────
// `ins_idx` est un numéro d'instrument 1…MD_MAX_INSTRUMENTS, comme pour les
// accesseurs FM ci-dessus. Les niveaux vont de 0 (silence) à 15 (maximum).
// `loop` et `rel` valent MD_EMPTY quand il n'y a ni boucle ni release.
// Enveloppe PSG : `point` va de 0 à MD_ENV_POINTS-1. L'amplitude vaut 0-15 ou
// MD_EMPTY quand le point est désactivé ; la vitesse vaut 0-15.
int md_replayer_get_env_amp(int ins_idx, int point);
int md_replayer_get_env_speed(int ins_idx, int point);
// `amp` accepte MD_EMPTY pour désactiver le point (et ceux qui suivent).
void md_replayer_set_env_amp(int ins_idx, int point, int amp);
void md_replayer_set_env_speed(int ins_idx, int point, int speed);

// ── Macros d'arpège et de mode de bruit ─────────────────────────────────────
// Mêmes conventions que la table de volumes : `loop` vaut MD_EMPTY quand il n'y
// a pas de bouclage. Les valeurs d'arpège sont des demi-tons signés, celles de
// bruit des modes 0-7.
// ── Tables ──────────────────────────────────────────────────────────────────
// `table` va de 0 à MD_MAX_TABLES-1 ; `row` de 0 à MD_TABLE_ROWS-1.
// Les champs vides valent MD_EMPTY (volume et commandes).
void md_replayer_get_table_row(int table, int row, uint8_t *vol, int8_t *transpose,
                               uint8_t *cmd1, uint8_t *val1,
                               uint8_t *cmd2, uint8_t *val2,
                               uint8_t *mdcmd, uint8_t *mdval);
void md_replayer_set_table_row(int table, int row, uint8_t vol, int8_t transpose,
                               uint8_t cmd1, uint8_t val1,
                               uint8_t cmd2, uint8_t val2,
                               uint8_t mdcmd, uint8_t mdval);
// La table est-elle vide (aucune ligne renseignée) ? Sert à l'affichage.
bool md_replayer_table_is_empty(int table);
// Table attachée à un instrument (1…MD_MAX_INSTRUMENTS). MD_EMPTY = aucune.
int md_replayer_get_instr_table(int ins_idx);
void md_replayer_set_instr_table(int ins_idx, int table);

// Puce visée par l'instrument : MD_INSTR_KIND_ANY / _FM / _PSG.
void md_replayer_set_instr_kind(int ins_idx, int kind);

// ── Échantillons (voie PCM) ─────────────────────────────────────────────────
// `data` est du 8 bits NON SIGNÉ déjà rééchantillonné à MD_PCM_RATE : la
// conversion appartient à l'appelant, qui sait d'où vient le son.
// Renvoie l'index de l'échantillon, ou -1 si la banque est pleine.
int  md_replayer_add_sample(const char *name, const uint8_t *data, uint32_t len,
                            int loop, int base_note);
int  md_replayer_sample_count(void);
bool md_replayer_get_sample(int idx, char *name, int name_cap,
                            uint32_t *length, int *loop, int *base_note);
const uint8_t *md_replayer_sample_data(int idx, uint32_t *length);
void md_replayer_clear_samples(void);
// Réglages d'un échantillon déjà rangé dans la banque.
void md_replayer_set_sample_base(int idx, int base_note);
void md_replayer_set_sample_loop(int idx, int loop);
uint32_t md_replayer_pcm_used(void);
uint32_t md_replayer_pcm_capacity(void);
// Échantillon joué par un instrument PCM.
void md_replayer_set_instr_sample(int ins_idx, int sample_idx);
int  md_replayer_get_instr_sample(int ins_idx);
void md_replayer_copy_instrument(int src, int dst);   // réglages ET nom
void md_replayer_set_instr_pcm_volume(int ins_idx, int volume);   // 0-FF, 7F = unité
int  md_replayer_get_instr_pcm_volume(int ins_idx);
int  md_replayer_get_instr_kind(int ins_idx);
// ── Flux de lecture d'une table ─────────────────────────────────────────────
// Une table ne se déroule pas d'un seul bloc : ses colonnes avancent en TROIS
// flux indépendants, parce que la commande H peut faire boucler l'un sans
// toucher aux autres.
//   0 → la colonne VOL
//   1 → la colonne TSP et la PREMIÈRE colonne CMD
//   2 → la SECONDE colonne CMD
// L'écran TABLE affiche donc trois repères de lecture qui défilent séparément.
#define MD_TABLE_STREAMS 3
// Ligne en cours pour un flux donné (-1 si le canal ne joue pas de table).
int md_replayer_play_table_pos(int channel, int stream);
// Numéro de la table qu'un canal est en train de dérouler (-1 si aucune).
int md_replayer_play_table(int channel);

// ── Commandes des colonnes CMD d'une table ──────────────────────────────────
// Les lettres reprennent celles de LSDJ, mais leur effet est celui du moteur
// Mega Drive. La liste vit ICI pour que le moteur et l'interface ne puissent
// pas diverger : l'interface lit les lettres par md_table_cmd_letter().
// Une case de commande vaut MD_EMPTY quand elle est vide (« --- »).
int md_table_cmd_count(void);
// Lettre affichée pour l'indice `i`, ou 0 si l'indice est hors liste.
char md_table_cmd_letter(int i);
// Numéro d'effet du moteur exécuté par cette commande, ou -1 quand elle est
// traitée directement (H, le saut ; A, démarrer/arrêter une table).
int md_table_cmd_effect(int i);
// Valeur réellement transmise au moteur : certaines commandes doivent coder
// leur sous-commande dans le quartet haut (K03 devient 0x33, par exemple).
uint8_t md_table_cmd_value(int i, uint8_t val);
// Nature d'une commande : la plupart passent au moteur d'effets, deux sont
// traitées directement par la lecture.
typedef enum {
  MD_CMD_PLAIN = 0,   // passe telle quelle au moteur d'effets
  MD_CMD_HOP,         // H : saut dans la table
  MD_CMD_TABLE,       // A : démarre ou arrête une table
  MD_CMD_PITCH,       // P : valeur signée, monte ou descend selon le signe
  MD_CMD_FINETUNE,    // U : centré sur 80, comme le E5xx de DefleMask
} md_cmd_kind_t;

// Résout une commande en (effet, valeur) transmis au moteur. À utiliser plutôt
// que md_table_cmd_effect/value, qui ignorent le signe de P.
void md_table_cmd_resolve(int i, uint8_t val, int *out_eff, uint8_t *out_val);
int md_table_cmd_kind(int i);

int md_replayer_get_psg_noise_macro(int ins_idx, uint8_t *out, int max_len,
                                    uint8_t *loop);
void md_replayer_set_psg_noise_step(int ins_idx, int index, int value);
void md_replayer_set_psg_noise_len(int ins_idx, int len);
void md_replayer_set_psg_noise_loop(int ins_idx, int loop);

// ── Macros PSG façon DefleMask (volume et arpège) ───────────────────────────
// `loop` vaut MD_EMPTY quand la macro ne boucle pas. `len` 0 = pas de macro.
// Les valeurs d'arpège sont des demi-tons signés, ou des notes absolues quand
// `fixed` est vrai.
void md_replayer_set_psg_vol_macro(int ins_idx, const uint8_t *vals, int len,
                                   int loop);
int  md_replayer_get_psg_vol_macro(int ins_idx, uint8_t *out, int max_len,
                                   int *loop);
void md_replayer_set_psg_arp_macro(int ins_idx, const int8_t *vals, int len,
                                   int loop, bool fixed);
int  md_replayer_get_psg_arp_macro(int ins_idx, int8_t *out, int max_len,
                                   int *loop, bool *fixed);

void md_replayer_mute_channel(int channel, bool muted);

#ifdef __cplusplus
}
#endif

#endif // MD_REPLAYER_H
