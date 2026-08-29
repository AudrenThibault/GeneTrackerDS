//
//  md_chip.cpp
//  MDTracker
//
//  Voir md_chip.h.
//

#include "md_chip.h"

#include "../ymfm/ymfm_opn.h"
#include "../emu76489/emu76489.h"

#include <cstring>
#include <new>

namespace {

// Interface minimale exigée par ymfm : on n'a besoin ni des timers ni des IRQ
// (on ne pilote pas la puce depuis un 68000, on écrit les registres nous-mêmes).
class md_ymfm_interface : public ymfm::ymfm_interface {};

md_ymfm_interface g_intf;

// Le YM2612 de ymfm est construit une seule fois puis simplement reset().
// ── Modèle de puce FM ───────────────────────────────────────────────────────
// Le YM2612 des Mega Drive 1 est une puce discrète dont le convertisseur a un
// défaut célèbre, le « ladder effect » : autour de zéro, sa conversion a un
// décrochage qui injecte une distorsion de croisement permanente. C'est une
// bonne part du grain sale qu'on associe à la console.
// La YM3438 des Model 2 tardives est une version CMOS qui n'a pas ce défaut.
//
// ymfm modélise les deux : `ym3438` dérive de `ym2612` et ne redéfinit que
// `generate()`. L'état interne étant strictement le même, on garde UNE seule
// puce et on appelle l'une ou l'autre version selon le réglage — pas besoin de
// dupliquer ni de réinitialiser quoi que ce soit quand on bascule.
// Faux = Mega Drive 2 (YM3438), le convertisseur propre — c'est notre défaut.
// Le ladder effect des Model 1 a une amplitude CONSTANTE : dès qu'on baisse la
// FM, il ressort. Voir le commentaire de ladderEffectEnabled côté Swift.
bool g_ladder = false;

ymfm::ym3438 &ym() {
  static ymfm::ym3438 chip(g_intf);
  return chip;
}

SNG *g_psg = nullptr;

// Copie du registre stéréo (extension Game Gear) : bits 0-3 = voie à DROITE,
// bits 4-7 = voie à GAUCHE, pour les voies ton1, ton2, ton3 et bruit.
uint8_t g_psg_stereo = 0xFF;

// ── Rééchantillonnage ──────────────────────────────────────────────────────
// Le YM2612 tourne nativement à clock/144 (~53267 Hz). CoreAudio veut 44100.
// On interpole linéairement entre deux échantillons source consécutifs.
int g_output_rate = 44100;
double g_src_rate = 53267.0;   // fixé une fois au démarrage, jamais dans la boucle
// Avance dans le flux source, en RAPPORT EXACT d'entiers.
//
// Le pas vaut cadence_source / cadence_sortie, soit 53267/44100 — un nombre
// qu'aucune virgule fixe ne représente exactement. Et l'erreur ne se contente
// pas d'exister : elle s'ACCUMULE. En Q24 elle vaut 2,4 x 10^-8, ce qui fait
// 0,06 échantillon de décalage par minute — trop peu pour décaler le rendu d'un
// échantillon entier, assez pour déformer l'onde de plus en plus. Mesuré : un
// écart qui monte régulièrement de -54 à -35 dB au fil du morceau.
//
// On ne l'approxime donc pas. Le compteur avance de `src` et retombe de `out` :
// c'est de l'arithmétique entière exacte, sans dérive possible, aussi longtemps
// que le morceau dure. Seul le POIDS d'interpolation est approché — et cette
// erreur-là ne s'accumule pas, elle se refait à neuf à chaque échantillon.
constexpr int kQR = 24;
uint32_t g_src_rate_i = 53267;   // cadence source, en entier
uint32_t g_out_rate_i = 44100;   // cadence de sortie, en entier
uint32_t g_frac_n = 0;           // reste, dans [0, g_out_rate_i[
int64_t  g_recip_q40 = 0;        // 2^40 / g_out_rate_i, pour éviter une division
                                 // (l'ARM9 de la DSi n'a pas d'instruction de
                                 //  division : elle coûterait des dizaines de
                                 //  cycles à chaque échantillon)
int32_t g_prevL = 0, g_prevR = 0;
int32_t g_curL = 0, g_curR = 0;
bool g_primed = false;

// ── Bloqueur de composante continue ────────────────────────────────────────
// Le YM2612 sort une tension de repos non nulle (la « discontinuité DAC » que
// ymfm reproduit fidèlement) : au silence, le mixage vaut ~+380 au lieu de 0.
// Laissée telle quelle, cette continue mange de la dynamique et fait claquer
// les enceintes au démarrage/arrêt. On la retire avec un passe-haut du premier
// ordre à ~8 Hz, inaudible sur le signal utile.
//
// ── Virgule fixe ────────────────────────────────────────────────────────────
// Tout ce qui suit tournait en `double`. Sur un appareil SANS unité flottante
// — la Nintendo DSi, où ce moteur doit aussi tourner — chaque multiplication
// flottante devient un APPEL DE ROUTINE d'une centaine de cycles au lieu d'une
// instruction. Ici on est dans la boucle par échantillon : ~18 opérations
// 53 267 fois par seconde, soit près de la totalité du processeur rien que
// pour le mélange.
//
// On garde donc les mêmes nombres, mais avec la virgule SOUS-ENTENDUE : 2,71
// devient l'entier 11100, avec la convention que la virgule est après 12 bits
// (11100 / 4096 = 2,710938). L'erreur est de 0,03 pour mille, soit 0,0008 dB.
// Les produits passent par des entiers 64 bits, que l'ARM fait en une
// instruction (smull) — aucun débordement possible.
constexpr int kQ = 12;
constexpr int32_t kOne = 1 << kQ;
#define MD_Q(x) ((int32_t)((x) * (double)kOne + ((x) < 0 ? -0.5 : 0.5)))

int32_t g_dcL_x = 0, g_dcR_x = 0;      // entrée précédente, en unités du signal
int64_t g_dcL_y = 0, g_dcR_y = 0;      // sortie précédente, en Q12
// Le pôle a SA PROPRE précision, bien plus fine que le reste : c'est un filtre
// récursif dont le gain interne vaut 1/(1-0,999) = 1000, si bien qu'une erreur
// sur ce coefficient-là est multipliée par mille. En Q12 il vaudrait 0,999023
// au lieu de 0,999 et l'écart au flottant montait à -36 dB ; en Q28 il est
// exact à 4 x 10^-9 près.
constexpr int kQDC = 28;
constexpr int64_t kDCPoleQ = (int64_t)(0.999 * (double)((int64_t)1 << kQDC) + 0.5);

// L'état de sortie est gardé en Q12 et non arrondi à chaque tour : sans ces
// bits de garde, un filtre récursif de ce type se met à osciller sur son
// dernier bit et laisse une continue résiduelle — précisément ce qu'il est
// chargé d'enlever.
inline int32_t dc_block(int32_t x, int32_t &prev_x, int64_t &prev_y) {
  int64_t y = ((int64_t)(x - prev_x) << kQ) + ((kDCPoleQ * prev_y) >> kQDC);
  prev_x = x;
  prev_y = y;
  return (int32_t)(y >> kQ);
}

// ── Key-on différé, par voie FM ────────────────────────────────────────────
// Compte à rebours en échantillons source ; 0 = rien en attente. 4 échantillons
// à ~53 kHz = 75 µs, largement au-dessus du seul échantillon nécessaire pour
// que la puce voie le key-off, et parfaitement inaudible.
constexpr int kKeyOnDelaySamples = 4;
int g_key_pending[6] = {0, 0, 0, 0, 0, 0};

// Code de voie attendu par le registre 0x28 : 0,1,2 puis 4,5,6.
inline uint8_t fm_key_code(int ch) { return (uint8_t)((ch < 3) ? ch : (ch + 1)); }

inline int16_t clamp16(int32_t v) {
  if (v > 32767)
    return 32767;
  if (v < -32768)
    return -32768;
  return (int16_t)v;
}

// Produit un échantillon à la fréquence native (YM + PSG mixés).
// ── Voie PCM ────────────────────────────────────────────────────────────────
// Position en virgule fixe 32.32 : un échantillon peut durer plusieurs images
// du YM2612 (à la cadence du pilote, une valeur tient ~3,3 images), et il faut
// pouvoir monter ou descendre la note sans dérive.
const uint8_t *g_pcm_data = nullptr;
uint32_t g_pcm_len = 0;
int32_t g_pcm_loop = -1;          // -1 = pas de bouclage
// Position et pas de lecture en Q24 (16 777 216 = 1,0), sur 64 bits : la partie
// entière peut donc atteindre 2^39, bien au-delà de tout échantillon, et le pas
// est précis à 6 x 10^-8 près.
constexpr int kQP = 24;
constexpr int64_t kOneP = (int64_t)1 << kQP;
int64_t g_pcm_pos = 0;
int64_t g_pcm_step = 0;
int g_pcm_vol = 255;   // 255 = unité ; au-delà, on pousse
// Le même volume, prêt à l'emploi : v * g_pcm_vol_q >> 16 vaut v * vol / 255.
//
// Il y avait une DIVISION par 255 a chaque echantillon. L'ARM9 de la DS n'a pas
// d'instruction de division : le compilateur y appelait une routine logicielle,
// 53 267 fois par seconde des que le PCM jouait. C'est ce qui faisait tomber le
// debit de 82 % a 54 % pendant les passages avec batterie.
uint32_t g_pcm_vol_q = (255u * 65536u + 127u) / 255u;
bool g_pcm_playing = false;
uint8_t g_pcm_last = 0x80;        // dernière valeur écrite, silence = 0x80

// Le crochet d'export. Déclaré ici, et non plus près de son accesseur, parce
// que le flux du convertisseur part depuis la génération d'échantillons, plus
// haut dans le fichier.
md_write_hook_t g_hook = nullptr;
void *g_hook_ctx = nullptr;

// Fait avancer la voie d'UNE image du YM2612 et écrit le DAC si besoin.
inline void pcm_tick() {
  if (!g_pcm_playing || !g_pcm_data || g_pcm_len == 0)
    return;
  uint32_t i = (uint32_t)(g_pcm_pos >> kQP);
  if (i >= g_pcm_len) {
    if (g_pcm_loop >= 0 && (uint32_t)g_pcm_loop < g_pcm_len) {
      g_pcm_pos = (int64_t)g_pcm_loop << kQP;
      i = (uint32_t)g_pcm_loop;
    } else {
      // Fin sans boucle : on retombe au repos, pas sur un claquement.
      g_pcm_playing = false;
      if (g_pcm_last != 0x80) { ym().write(0, 0x2a); ym().write(1, 0x80); g_pcm_last = 0x80; }
      return;
    }
  }
  // Mise à l'échelle du volume, autour du zéro (0x80 = silence).
  //
  // Le gain peut DÉPASSER l'unité (255) : c'est ce qui permet de pousser un
  // échantillon percussif, dont la valeur efficace est très basse face à une
  // note tenue. Ce qui sort de l'échelle est écrêté — exactement ce que ferait
  // un pilote qui pousse ses octets avant de les écrire, et ce qu'on retrouve
  // donc à l'identique dans une ROM.
  int v = (int)g_pcm_data[i] - 128;
  v = (int)(((int64_t)v * (int32_t)g_pcm_vol_q) >> 16);
  if (v > 127) v = 127;
  if (v < -128) v = -128;
  uint8_t out = (uint8_t)(v + 128);
  // On n'écrit que si la valeur CHANGE : réécrire la même ne s'entend pas et
  // c'est ce que fait le pilote, qui n'écrit qu'à sa propre cadence.
  if (out != g_pcm_last) {
    ym().write(0, 0x2a);
    ym().write(1, out);
    g_pcm_last = out;
  }
  g_pcm_pos += g_pcm_step;
}

/// Relèvement du PCM. Le DAC est DÉJÀ à son maximum — les octets écrits
/// couvrent toute l'échelle 8 bits et les échantillons importés culminent à
/// 91-99 % de la pleine échelle — donc on ne peut pas monter à la source : on
/// ajoute au mélange une part proportionnelle à ce qui vient d'être écrit dans
/// le DAC.
///
/// Mesuré sur le morceau d'essai : ×1,4 = +2,9 dB sans aucun écrêtage, ×1,8 =
/// +5,1 dB pour deux échantillons écrêtés sur 1,2 million, ×2,2 = +6,6 dB pour
/// treize.
///
/// ⚠️ REVENU À 1,0, et il doit y rester. Ce relèvement n'existe que dans notre
/// mélangeur : une vraie Mega Drive ne saurait pas le reproduire, le DAC y
/// étant déjà à son maximum. C'est MD_FM_CARRIER_ATTEN qui fait le travail
/// désormais, en baissant la FM — un geste que le matériel, lui, sait faire.
constexpr int32_t kPCMBoost = MD_Q(1.0);

/// Ce que vaut UNE unité du DAC en sortie de puce, mesuré : un carré pleine
/// échelle sort à 6474 de valeur efficace, soit 3237 avant le niveau FM (×2),
/// pour un écart de ±127 unités.
constexpr int32_t kDACUnitQ = MD_Q(25.5);

/// Relèvement du seul canal de BRUIT du SN76489 (voir le mélange plus bas).
///
/// Mesuré sur le morceau d'essai, chaque famille de voix rendue seule :
///
///   relèvement   bruit / FM   bruit / PCM   marge avant saturation
///     ×2           -5,9 dB      +3,7 dB           2,8 dB
///     ×3           -3,5 dB      +6,1 dB           1,7 dB
///     ×4           -1,5 dB      +8,1 dB           0,7 dB   <- retenu
///     ×5           +0,2 dB      +9,8 dB           0,0 dB   (ça écrête)
///
/// « FM » est ici la somme des CINQ voies FM : le bruit, voix unique, tient
/// donc largement tête à chacune d'elles prise à part. Au-delà de ×5 le mixage
/// dépasse la pleine échelle sur ce morceau.
///
/// ⚠️ REVENU À 1,0, et il doit y rester : le SN76489 n'a pas de réglage plus
/// fort que son maximum, et dans le morceau d'essai l'instrument de bruit y est
/// déjà presque (14 sur 15). C'est MD_FM_CARRIER_ATTEN qui fait ressortir le
/// bruit maintenant, en baissant la FM.
constexpr int32_t kPSGNoiseBoost = MD_Q(1.0);

// ── Travail par lots pour le YM2612 ─────────────────────────────────────────
// ymfm etait appele UN echantillon a la fois, 53 267 fois par seconde. Entre
// deux tics du tracker aucun registre n'est ecrit : on peut donc lui demander
// un lot d'un coup, ce qui amortit le prologue et garde ses six voies et ses
// vingt-quatre operateurs dans le cache d'un echantillon a l'autre.
//
// Le lot est vide a chaque entree dans md_chip_generate, donc a chaque tic,
// pour qu'aucune ecriture de registre ne soit prise en compte trop tard. Et il
// est desactive quand la voie PCM joue, puisqu'elle ecrit le registre 0x2A a
// chaque echantillon.
constexpr int kLotFM = 64;
ymfm::ym2612::output_data g_lot[kLotFM];
int g_lot_reste = 0, g_lot_pos = 0;
inline void lot_vider() { g_lot_reste = 0; g_lot_pos = 0; }

inline void render_source_sample(int32_t &outL, int32_t &outR) {
  pcm_tick();
  // Key-on en attente : on les libère AVANT de générer l'échantillon, une fois
  // que le key-off a bien été vu par la puce.
  for (int c = 0; c < 6; c++) {
    if (g_key_pending[c] > 0 && --g_key_pending[c] == 0) {
      // Passer par md_chip_ym_write, PAS par ym().write : cette écriture-là est
      // le key-on, et une ROM exportée doit l'entendre comme la puce l'entend.
      // Quand elle contournait le crochet, le journal contenait tous les
      // réglages, toutes les fréquences et tous les key-OFF — et pas une seule
      // note. La cartouche était muette.
      md_chip_ym_write(0, 0x28, (uint8_t)(0xF0 | fm_key_code(c)));
    }
  }

  // Flux du DAC pour l'export ROM : une valeur par image de la puce, jouée ou
  // non. C'est ce qui permet à une cartouche de rejouer le convertisseur à
  // cadence fixe, sans refaire en 68000 le pas de lecture, le volume et
  // l'écrêtage — les trois endroits où elle pourrait diverger de l'app.
  if (g_hook) g_hook(3, g_pcm_last, g_pcm_playing ? 1 : 0, g_hook_ctx);

  ymfm::ym2612::output_data fm;
  // `generate` n'est pas virtuel : on choisit explicitement l'étage de sortie.
  if (g_ladder || g_pcm_playing) {
    if (g_ladder) ym().ymfm::ym2612::generate(&fm, 1);
    else          ym().generate(&fm, 1);
  } else {
    if (g_lot_reste == 0) {
      ym().generate(g_lot, kLotFM);
      g_lot_reste = kLotFM; g_lot_pos = 0;
    }
    fm = g_lot[g_lot_pos++];
    g_lot_reste--;
  }

  // emu76489 gère lui-même sa conversion de fréquence (mode « quality »),
  // on lui demande donc simplement un échantillon à la cadence du YM2612.
  int32_t psg[2] = {0, 0};
  if (g_psg) {
    SNG_calc_stereo(g_psg, psg);
    // ── Le canal de BRUIT, à part ─────────────────────────────────────────
    // Mesuré à vélocité 7F, enveloppe pleine : une voie PSG sort 10 dB sous une
    // voie FM, et 16 dB sous le PCM. Le bruit est au même niveau que les voies
    // à ton (0,7 dB d'écart) — la puce les traite pareil — mais son énergie est
    // étalée sur tout le spectre au lieu d'être empilée sur quelques
    // harmoniques, si bien qu'à niveau égal il s'entend beaucoup moins.
    //
    // On ne relève donc QUE lui : les voies à ton ont déjà été jugées justes.
    // Une seule constante à bouger si c'est encore trop ou trop peu.
    if (kPSGNoiseBoost != kOne) {
      const int32_t nz = (int32_t)(((int64_t)g_psg->ch_out[3]
                                    * (kPSGNoiseBoost - kOne)) >> kQ);
      if ((g_psg->stereo >> 4) & 0x08) psg[0] += nz;
      if (g_psg->stereo & 0x08)        psg[1] += nz;
    }
  }

  // Équilibre FM / PSG.
  //
  // Ce rapport n'existe PAS dans les données : sur une vraie Megadrive, les
  // deux puces sont additionnées par un réseau de résistances sur la carte
  // mère, et ce réseau change selon les révisions de la console — une Model 1
  // et une Model 2 ne mixent pas pareil. Il n'y a donc pas de valeur unique
  // « correcte », et un export VGM est de toute façon insensible à ce choix.
  //
  // À 0.45 le PSG sortait 6 dB sous une voie FM, ce qui le rendait inutilisable
  // en même temps que le FM. À 0.90 une voie PSG tient tête à une voie FM.
  //
  // ⚠️ Aucun limiteur derrière : sur un tutti à dix voies, la somme peut
  // dépasser la pleine échelle et écrêter. C'est assumé — la saturation fait
  // partie du grain recherché.
  // Niveaux de sortie des deux puces.
  //
  // Le FM était réglé (0.75) pour qu'AUCUN écrêtage ne soit possible, même dans
  // le cas absurde où les dix canaux jouent la même note à leur maximum et en
  // phase. Le prix était lourd : un morceau normal plafonnait à -11.7 dBFS et
  // sonnait bien plus faible que le même morceau dans DefleMask.
  //
  // Le rapport ENTRE LES DEUX PUCES est le seul de ces réglages qui existe
  // vraiment sur la console : c'est le réseau de résistances de la carte mère.
  // Il ne vit pas dans les données, donc le changer ne coûte RIEN à une ROM
  // exportée — contrairement à un relèvement voie par voie, qui lui serait
  // impossible à reproduire.
  //
  // Mesuré chez nous, constantes neutralisées : une voie FM à fond culmine à
  // 6040, une voie PSG à 4465. Pour qu'elles se tiennent — ce que fait la
  // machine — il faudrait kPSGLevel = kFMLevel × 6040/4465, soit 2,7. On était
  // à 0,90, c'est-à-dire le PSG près de 10 dB sous une voie FM : d'où
  // l'impression tenace que le bruit ne perçait pas.
  //
  // 2,71 = 2,0 × 6040/4465, c'est-à-dire la PARITÉ : une voie PSG à son maximum
  // sort au même niveau qu'une voie FM à son maximum. C'est un fait mesuré sur
  // les deux puces, pas un goût, et surtout pas un réglage accordé sur un
  // morceau — le premier chiffre que j'avais posé ici (1,80) l'était, ce qui
  // n'a aucune valeur dès qu'on change de musique.
  //
  // ⚠️ Conséquence assumée : sur un passage extrême — les voies FM à leur
  // maximum et en phase — la somme dépasse la pleine échelle et est écrêtée.
  // Une vraie Mega Drive fait pareil, son mélangeur analogique sature.
  constexpr int32_t kFMLevel = MD_Q(2.0);
  constexpr int32_t kPSGLevel = MD_Q(2.71);

  // ── Niveau de sortie général ──────────────────────────────────────────
  // Il s'applique à TOUT de la même façon, donc il ne change aucun équilibre :
  // c'est le bouton de volume, pas un trucage.
  //
  // À 1,0, le critère est vérifiable sans écouter quoi que ce soit : UNE voix à
  // son maximum culmine à -9,6 dBFS (10 863 sur 32 767). Il reste donc de quoi
  // empiler environ trois voix maximales EN PHASE avant de saturer — et bien
  // plus en pratique, des voix réelles n'étant jamais en phase. Le régler sur la
  // crête d'un morceau donné, comme je l'avais fait, ne veut rien dire pour le
  // morceau suivant.
  constexpr int32_t kMasterLevel = MD_Q(1.00);
  // Le supplément de PCM, proportionnel au dernier octet envoyé au DAC. Il
  // vaut zéro dès que la voie se tait, puisque le silence y est 0x80. Il ignore
  // le panoramique de FM6 : la voie PCM est au centre dans la pratique.
  // Le supplément est en Q12 comme le reste, pour rester dans la même unité
  // que les deux produits auxquels il s'ajoute.
  const int64_t pcmExtra =
      (kPCMBoost == kOne)
          ? 0
          : (((int64_t)g_pcm_last - 128) * kDACUnitQ * kFMLevel
             * (kPCMBoost - kOne)) >> (2 * kQ);
  const int64_t mixL =
      ((((int64_t)fm.data[0] * kFMLevel + (int64_t)psg[0] * kPSGLevel)
        + pcmExtra) * kMasterLevel) >> (2 * kQ);
  const int64_t mixR =
      ((((int64_t)fm.data[1] * kFMLevel + (int64_t)psg[1] * kPSGLevel)
        + pcmExtra) * kMasterLevel) >> (2 * kQ);

  outL = dc_block((int32_t)mixL, g_dcL_x, g_dcL_y);
  outR = dc_block((int32_t)mixR, g_dcR_x, g_dcR_y);
}

} // namespace

extern "C" {

void md_chip_reset(int output_sample_rate) {
  if (output_sample_rate <= 0)
    output_sample_rate = 44100;

  ym().reset();

  // La voie PCM fait partie de l'état de la puce : sans ça, un échantillon
  // encore en cours survivait à un nouveau morceau, et le DAC restait branché
  // sur FM6 en tenant sa dernière valeur — un résidu audible.
  g_pcm_data = nullptr;
  g_pcm_len = 0;
  g_pcm_loop = -1;
  g_pcm_pos = 0;
  g_pcm_step = 0.0;
  g_pcm_vol = 255;
  g_pcm_vol_q = (255u * 65536u + 127u) / 255u;
  g_pcm_playing = false;
  g_pcm_last = 0x80;

  g_output_rate = output_sample_rate;
  g_src_rate = (double)MD_YM2612_CLOCK / (double)MD_YM_DIVISEUR;
  if (g_src_rate <= 0.0)
    g_src_rate = (double)MD_YM2612_CLOCK / (double)MD_YM_DIVISEUR;

  g_src_rate_i = (uint32_t)(g_src_rate + 0.5);
  g_out_rate_i = (uint32_t)g_output_rate;
  if (g_out_rate_i == 0) g_out_rate_i = 44100;
  g_recip_q40 = ((int64_t)1 << 40) / (int64_t)g_out_rate_i;
  g_frac_n = 0;
  g_prevL = g_prevR = g_curL = g_curR = 0;
  g_primed = false;

  // Le PSG tourne à sa propre horloge et rééchantillonne vers la cadence du
  // YM2612 (mode « quality » = conversion de fréquence interne).
  if (!g_psg)
    g_psg = SNG_new(MD_PSG_CLOCK, (uint32_t)g_src_rate);
  if (g_psg) {
    SNG_set_rate(g_psg, (uint32_t)g_src_rate);
    SNG_set_quality(g_psg, 1);
    SNG_reset(g_psg);
    g_psg_stereo = 0xFF; // toutes les voies au centre
    SNG_writeGGIO(g_psg, g_psg_stereo);
  }

  g_dcL_x = g_dcL_y = g_dcR_x = g_dcR_y = 0.0;
  for (int c = 0; c < 6; c++)
    g_key_pending[c] = 0;

  // Le YM2612 démarre avec le DAC désactivé et le LFO éteint.
  md_chip_ym_write(0, 0x22, 0x00); // LFO off
  md_chip_ym_write(0, 0x27, 0x00); // mode normal (pas de ch3 spécial)
  md_chip_ym_write(0, 0x2B, 0x00); // DAC off
}

void md_chip_set_write_hook(md_write_hook_t hook, void *ctx) {
  g_hook = hook;
  g_hook_ctx = ctx;
}

// Compense la demi-cadence sur les registres de vitesse d'enveloppe.
// AR (50-5F), D1R (60-6F) et D2R (70-7F) portent une vitesse sur 5 bits :
// +4 double la vitesse. Pour un rapport de MD_YM_DIVISEUR/144 il faut donc
// 4 x log2(rapport), soit +6 pour 384. RR (80-8F) n'a que 4 bits et sa vitesse
// effective vaut 2*RR+1 : la moitie suffit sur ce champ-la.
// +4 par doublement de la reduction : 288 -> +4, 384 -> +6, 576 -> +8.
#define MD_ENV_COMP ((MD_YM_DIVISEUR == 288) ? 4 : (MD_YM_DIVISEUR == 384) ? 6 : \
                     (MD_YM_DIVISEUR == 576) ? 8 : 0)

static uint8_t md_compense_enveloppe(uint8_t reg, uint8_t val) {
  if (MD_YM_DIVISEUR == 144) return val;           // rien a compenser
  if (reg >= 0x50 && reg <= 0x7F) {
    uint32_t r = (val & 0x1F) + MD_ENV_COMP;
    if (r > 31) r = 31;
    return (uint8_t)((val & 0xE0) | r);
  }
  if (reg >= 0x80 && reg <= 0x8F) {
    uint32_t r = (val & 0x0F) + MD_ENV_COMP / 2;
    if (r > 15) r = 15;
    return (uint8_t)((val & 0xF0) | r);
  }
  return val;
}

void md_chip_ym_write(uint8_t part, uint8_t reg, uint8_t val) {
  val = md_compense_enveloppe(reg, val);
  if (g_hook) g_hook(part ? 1 : 0, reg, val, g_hook_ctx);
  uint32_t base = (part & 1) ? 2 : 0;
  ym().write(base + 0, reg);
  ym().write(base + 1, val);
}

void md_chip_ym_key(int fm_channel, bool on) {
  if (fm_channel < 0 || fm_channel > 5)
    return;
  if (on) {
    // On ne l'écrit pas tout de suite : cf. le commentaire dans md_chip.h.
    g_key_pending[fm_channel] = kKeyOnDelaySamples;
  } else {
    // Le key-off est immédiat et annule tout key-on encore en attente, sinon
    // celui-ci se déclencherait APRÈS la coupure et laisserait une note bloquée.
    g_key_pending[fm_channel] = 0;
    md_chip_ym_write(0, 0x28, fm_key_code(fm_channel));
  }
}

void md_chip_psg_write(uint8_t data) {
  if (g_psg)
    if (g_hook) g_hook(2, data, 0, g_hook_ctx);
  SNG_writeIO(g_psg, data);
}

void md_chip_psg_set_period(int ch, uint16_t period) {
  if (!g_psg || ch < 0 || ch > 2)
    return;
  // Protocole réel : octet de commande (4 bits de poids faible) puis octet de
  // données (6 bits de poids fort).
  if (g_hook) {
    g_hook(2, (uint8_t)(0x80 | (ch << 5) | (period & 0x0F)), 0, g_hook_ctx);
    g_hook(2, (uint8_t)((period >> 4) & 0x3F), 0, g_hook_ctx);
  }
  SNG_writeIO(g_psg, (uint32_t)(0x80 | (ch << 5) | (period & 0x0F)));
  SNG_writeIO(g_psg, (uint32_t)((period >> 4) & 0x3F));
}

void md_chip_psg_set_volume(int ch, uint8_t attenuation) {
  if (!g_psg || ch < 0 || ch > 3)
    return;
  if (g_hook) g_hook(2, (uint8_t)(0x90 | (ch << 5) | (attenuation & 0x0F)), 0, g_hook_ctx);
  SNG_writeIO(g_psg, (uint32_t)(0x90 | (ch << 5) | (attenuation & 0x0F)));
}

static uint8_t g_psg_noise_mode = 0xFF;   // dernier mode écrit (relecture)

void md_chip_psg_set_noise(uint8_t control) {
  g_psg_noise_mode = (uint8_t)(control & 0x07);
  if (!g_psg)
    return;
  if (g_hook) g_hook(2, (uint8_t)(0xE0 | (control & 0x07)), 0, g_hook_ctx);
  SNG_writeIO(g_psg, (uint32_t)(0xE0 | (control & 0x07)));
}

/// Le dernier mode de bruit écrit (0xFF si aucun). Sert aux essais : sans lui,
/// impossible de vérifier qui, de la commande ou de l'instrument, a le dernier
/// mot sans écouter.
uint8_t md_chip_psg_noise_mode(void) { return g_psg_noise_mode; }

void md_chip_psg_set_pan(int ch, uint8_t pan) {
  if (!g_psg || ch < 0 || ch > 3)
    return;
  uint8_t right = (uint8_t)(1 << ch);        // bits 0-3
  uint8_t left = (uint8_t)(1 << (ch + 4));   // bits 4-7
  g_psg_stereo &= (uint8_t)~(left | right);
  switch (pan) {
  case 1:
    g_psg_stereo |= left;
    break;
  case 2:
    g_psg_stereo |= right;
    break;
  default:
    g_psg_stereo |= (uint8_t)(left | right);
    break;
  }
  SNG_writeGGIO(g_psg, g_psg_stereo);
}

void md_chip_generate(int16_t *stereo_out, int num_frames) {
  if (!stereo_out || num_frames <= 0)
    return;

  lot_vider();   // un tic vient peut-etre d'ecrire des registres

  if (!g_primed) {
    render_source_sample(g_prevL, g_prevR);
    render_source_sample(g_curL, g_curR);
    g_frac_n = 0;
    g_primed = true;
  }

  for (int i = 0; i < num_frames; i++) {
    // Interpolation linéaire : le produit tient sur 64 bits, une seule
    // instruction sur ARM comme sur x86.
    // reste / cadence_sortie, ramené en Q24 par une multiplication.
    const int32_t t = (int32_t)(((int64_t)g_frac_n * g_recip_q40) >> 16);
    int32_t l = g_prevL + (int32_t)(((int64_t)(g_curL - g_prevL) * t) >> kQR);
    int32_t r = g_prevR + (int32_t)(((int64_t)(g_curR - g_prevR) * t) >> kQR);

    stereo_out[i * 2 + 0] = clamp16(l);
    stereo_out[i * 2 + 1] = clamp16(r);

    g_frac_n += g_src_rate_i;
    while (g_frac_n >= g_out_rate_i) {
      g_frac_n -= g_out_rate_i;
      g_prevL = g_curL;
      g_prevR = g_curR;
      render_source_sample(g_curL, g_curR);
    }
  }
}

} // extern "C"

void md_chip_pcm_enable(bool on) {
  // Registre 2B bit 7 : FM6 quitte la synthèse pour devenir le DAC.
  // Par md_chip_ym_write, donc journalisé : une ROM qui embarque le flux du
  // convertisseur doit brancher et débrancher celui-ci aux mêmes instants que
  // l'app, sinon la voie 6 joue de la FM là où elle devrait taire, ou l'inverse.
  md_chip_ym_write(0, 0x2b, on ? 0x80 : 0x00);
  if (!on) { g_pcm_playing = false; g_pcm_last = 0x80; }
}

void md_chip_pcm_play(const uint8_t *data, uint32_t len, int32_t loop,
                      double rate_hz, int vol) {
  // On raisonne en hertz ; la conversion en pas par image du YM2612 se fait
  // ici, où la cadence source est connue.
  // Converti ici, à l'attaque de la note : ce calcul-là n'est PAS dans la
  // boucle par échantillon, un flottant n'y coûte rien.
  const int64_t step = (g_src_rate > 0.0)
      ? (int64_t)(rate_hz / g_src_rate * (double)kOneP + 0.5) : 0;
  g_pcm_data = data;
  g_pcm_len = len;
  g_pcm_loop = loop;
  g_pcm_step = step > 0 ? step : 0;
  g_pcm_vol = vol;
  g_pcm_vol_q = ((uint32_t)(vol < 0 ? 0 : vol) * 65536u + 127u) / 255u;
  g_pcm_pos = 0.0;
  g_pcm_playing = (data != nullptr && len > 0 && step > 0);
}

void md_chip_pcm_stop(void) {
  g_pcm_playing = false;
  if (g_pcm_last != 0x80) { ym().write(0, 0x2a); ym().write(1, 0x80); g_pcm_last = 0x80; }
}

bool md_chip_pcm_active(void) { return g_pcm_playing; }

void md_chip_set_ladder(bool enabled) { g_ladder = enabled; }
bool md_chip_get_ladder(void) { return g_ladder; }
