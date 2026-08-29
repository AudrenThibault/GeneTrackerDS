//
//  md_chip.h
//  MDTracker
//
//  Couche « puce » de la Mega Drive : YM2612 (FM, 6 voies) + SN76489 (PSG,
//  3 tons + 1 bruit), mixés et rééchantillonnés vers la fréquence de sortie
//  de CoreAudio.
//
//  Le YM2612 provient de ymfm (Aaron Giles) — licence BSD 3-Clause.
//  Le SN76489 provient d'emu76489 (Mitsutaka Okazaki) — licence MIT.
//
//  Copyright © 2026 Audren Thibault. All rights reserved.
//

#ifndef MD_CHIP_H
#define MD_CHIP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Horloge du YM2612 sur Mega Drive NTSC (master 53.693175 MHz / 7).
#define MD_YM2612_CLOCK 7670454

// ── Cadence de la puce, divisee par deux (specialisation DS) ────────────────
// Le YM2612 tourne nativement a horloge/144, soit 53 267 Hz. Sur l'ARM9 de la
// DSi, emuler ca demande le double du temps disponible : mesure, on produit
// ~17 000 echantillons par seconde pour 32 768 necessaires.
//
// On fait donc tourner la puce a horloge/288, soit 26 633 Hz.
//
// ⚠️ ON NE PEUT PAS DESCENDRE PLUS BAS, et ce n'est pas une question de gout.
// La hauteur du YM2612 s'encode en `block` (0 a 7) + `F-Num` (0 a 2047).
// Reduire la cadence decale toute cette plage vers le haut de log2(diviseur/144)
// octaves, et md_hz_to_fnum ECRETE silencieusement au-dela de block 7 : les
// notes aigues sortent alors a une hauteur fausse.
//
//   diviseur 144 : plafond de hauteur 13,3 kHz  (aucune note musicale au-dela)
//   diviseur 288 : plafond  6,6 kHz             (encore au-dessus des notes)
//   diviseur 480 : plafond  4,0 kHz             (ECRETE des le do8 — essaye,
//                                                le FM devenait horrible alors
//                                                que PSG et PCM restaient nets)
//
// 288 est donc le maximum utilisable. Il laisse 90 % du temps reel : ca suffit
// presque, mais pas tout a fait. Le reste doit venir du coeur FM. Deux compensations sont indispensables, sans quoi
// tout serait faux :
//   - la HAUTEUR : le moteur calcule ses F-Num a partir de cette cadence, donc
//     il faut lui donner la meme constante (voir md_replayer.c) ;
//   - les ENVELOPPES : elles avancent par image de puce, donc elles seraient
//     trop lentes dans le rapport de la reduction. Le YM2612 double sa vitesse
//     tous les +4 sur le registre de vitesse : pour un rapport de 384/144 =
//     3,33, il faut +4 x log2(3,33) = +6,95, arrondi a +7 (voir
//     md_chip_ym_write). L'arrondi ne laisse que 0,9 % d'erreur.
//
// Le prix, assume : le plafond de frequences tombe de 26 a 13 kHz, et ce qui
// vit au-dessus se REPLIE dans l'audible. Ca s'entend sur une FM tres modulee.
// Remettre 144 ici quand le coeur FM sera assez rapide.
#define MD_YM_DIVISEUR 288

// Horloge du PSG sur Mega Drive NTSC (master / 15).
#define MD_PSG_CLOCK 3579545

// (Ré)initialise les deux puces et le rééchantillonneur.
// `output_sample_rate` = fréquence de CoreAudio (typiquement 44100).
void md_chip_reset(int output_sample_rate);

// Écriture d'un registre YM2612.
// `part` = 0 → registres 0x00-0xB6 (voies 1-3)
// `part` = 1 → registres 0x00-0xB6 (voies 4-6)
void md_chip_ym_write(uint8_t part, uint8_t reg, uint8_t val);

// Key-on / key-off d'une voie FM (0-5).
//
// Le key-on est VOLONTAIREMENT retardé de quelques échantillons. Le générateur
// d'enveloppe du YM2612 ne compare l'état de la touche qu'une fois par
// échantillon : si on écrit key-off puis key-on dans la foulée, il ne voit
// jamais l'état intermédiaire, ne repasse pas en release, et donc NE REDÉCLENCHE
// PAS l'attaque — la note suivante reste muette. En espaçant les deux écritures
// on retrouve le comportement attendu d'un tracker : toute nouvelle note relance
// l'enveloppe, note-off ou pas.
void md_chip_ym_key(int fm_channel, bool on);

// Écriture brute sur le port du PSG (protocole latch/data).
void md_chip_psg_write(uint8_t data);

// Accès directs au PSG (plus lisibles côté replayer). Ils passent tous par le
// protocole latch/data réel de la puce.
void md_chip_psg_set_period(int ch, uint16_t period);
void md_chip_psg_set_volume(int ch, uint8_t attenuation); // 0 = fort, 15 = muet
void md_chip_psg_set_noise(uint8_t control);              // 3 bits
uint8_t md_chip_psg_noise_mode(void);                     // dernier mode écrit

// ── Enregistrement des écritures, pour l'export ─────────────────────────────
// Une ROM Mega Drive ne rejoue pas notre moteur : elle rejoue ce que notre
// moteur ÉCRIT DANS LES PUCES. On se branche donc à la sortie, au seul endroit
// par où tout passe — ainsi la cartouche entend exactement ce que l'iPad fait
// entendre, sans réécrire le tracker en 68000.
//
// `chip` : 0 = YM2612 banc 0, 1 = YM2612 banc 1, 2 = SN76489 (un seul octet),
//          3 = valeur du DAC, à la cadence native du YM2612 (`a` = l'octet,
//              `b` = 1 si la voie PCM joue). Contrairement aux trois autres, ce
//              flux part À CHAQUE image de la puce et pas seulement quand la
//              valeur change : une cartouche doit pouvoir le rejouer à cadence
//              FIXE, ce qu'un journal de changements ne permet pas.
typedef void (*md_write_hook_t)(int chip, uint8_t a, uint8_t b, void *ctx);
void md_chip_set_write_hook(md_write_hook_t hook, void *ctx);
// Panning : passe par le registre stéréo (extension Game Gear de la même puce ;
// sur Mega Drive le PSG est mono, on l'utilise pour donner un panning au tracker).
void md_chip_psg_set_pan(int ch, uint8_t pan); // 0 centre, 1 gauche, 2 droite

// Génère `num_frames` échantillons stéréo entrelacés 16 bits signés.
void md_chip_generate(int16_t *stereo_out, int num_frames);

// Modèle de puce FM : vrai = YM2612 discrète des Mega Drive 1, avec son défaut
// de convertisseur (le « ladder effect », qui donne le grain sale de la
// console) ; faux = YM3438 CMOS des Model 2 tardives, sans ce défaut.
// Bascule à chaud, l'état de la puce n'est pas touché.
// ── Voie PCM (le DAC du YM2612) ─────────────────────────────────────────────
// Le registre 2B bit 7 débranche FM6 de la synthèse : chaque octet écrit dans
// le registre 2A sort alors directement au haut-parleur. Il n'y a AUCUNE
// cadence automatique — c'est le processeur qui écrit octet après octet, et
// l'intervalle qu'il tient EST la fréquence d'échantillonnage.
//
// On reproduit ça exactement : les données sont déjà à la cadence du pilote
// (MD_PCM_RATE), et on n'écrit une nouvelle valeur qu'au rythme voulu. Écrire
// la même valeur plus souvent ne change rien au son.
//
// `step` = combien de pas de l'échantillon on avance par image du YM2612.
// `vol` va de 0 à 255 (255 = pleine échelle) : le vrai matériel n'a pas de
// registre de volume ici, ce sont les DONNÉES qu'on met à l'échelle — c'est ce
// que font SMPS, GEMS et Echo, et c'est donc compatible cartouche.
// `vol` : 255 = unité. Au-delà l'échantillon est POUSSÉ, avec écrêtage.
void md_chip_pcm_play(const uint8_t *data, uint32_t len, int32_t loop,
                      double rate_hz, int vol);
void md_chip_pcm_stop(void);
bool md_chip_pcm_active(void);
// Branche ou débranche FM6 de la synthèse (registre 2B bit 7).
void md_chip_pcm_enable(bool on);

// Nombre d'echantillons ecretes depuis le dernier appel (diagnostic).
uint32_t md_chip_ecretes_et_remet_a_zero(void);
void md_chip_set_ladder(bool enabled);
bool md_chip_get_ladder(void);

#ifdef __cplusplus
}
#endif

#endif // MD_CHIP_H
