// ============================================================================
//  Lecture d'un fichier son, converti pour le convertisseur du YM2612.
//
//  Le moteur n'accepte qu'une seule forme : du 8 bits NON SIGNE a 32 000 Hz,
//  en mono. C'est exactement ce que recoit le DAC, et la conversion se fait
//  ici, une fois pour toutes, pour que la lecture n'ait plus rien a calculer.
//
//  On accepte donc n'importe quel WAV — 8, 16, 24 ou 32 bits, entier ou
//  flottant, mono ou stereo, a n'importe quelle cadence — et on s'occupe de
//  tout ramener a cette forme. Refuser un fichier parce qu'il est en 24 bits
//  serait absurde : l'utilisateur ne choisit pas toujours le format de ses
//  echantillons.
// ============================================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define MD_AUDIO_RATE 32000

static uint32_t lit32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}
static uint16_t lit16(const uint8_t *p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

// Un echantillon du fichier, ramene a l'intervalle [-1, 1] en virgule fixe
// Q15 : assez precis pour du 8 bits en sortie, et sans flottant.
static int32_t echantillon_q15(const uint8_t *p, int bits, int format) {
  if (format == 3) {                       // IEEE 754 sur 32 bits
    uint32_t u = lit32(p);
    float f; memcpy(&f, &u, 4);
    if (f > 1.0f) f = 1.0f;
    if (f < -1.0f) f = -1.0f;
    return (int32_t)(f * 32767.0f);
  }
  switch (bits) {
  case 8:  return ((int32_t)p[0] - 128) << 8;      // le WAV 8 bits est NON signe
  case 16: return (int16_t)lit16(p);
  case 24: { int32_t v = ((int32_t)p[0] << 8) | ((int32_t)p[1] << 16) |
                         ((int32_t)p[2] << 24);
             return v >> 16; }
  case 32: return (int32_t)((int32_t)lit32(p) >> 16);
  default: return 0;
  }
}

// Charge un WAV et rend un tampon 8 bits non signe a 32 kHz, a liberer par
// l'appelant. Rend nullptr si le fichier n'est pas exploitable.
extern "C" uint8_t *md_audio_charge_wav(const char *chemin, uint32_t *sortie_len) {
  if (sortie_len) *sortie_len = 0;
  FILE *f = fopen(chemin, "rb");
  if (!f) return nullptr;
  fseek(f, 0, SEEK_END);
  long taille = ftell(f);
  fseek(f, 0, SEEK_SET);
  // Un echantillon de tracker n'a aucune raison de peser plus de quatre
  // megaoctets, et la DS n'en a pas beaucoup plus a offrir.
  if (taille < 44 || taille > 4 * 1024 * 1024) { fclose(f); return nullptr; }
  uint8_t *brut = (uint8_t *)malloc((size_t)taille);
  if (!brut) { fclose(f); return nullptr; }
  const bool lu = fread(brut, 1, (size_t)taille, f) == (size_t)taille;
  fclose(f);
  if (!lu || memcmp(brut, "RIFF", 4) || memcmp(brut + 8, "WAVE", 4)) {
    free(brut); return nullptr;
  }

  // ── Les morceaux du fichier ──────────────────────────────────────────────
  int format = 0, voies = 0, bits = 0; uint32_t cadence = 0;
  const uint8_t *donnees = nullptr; uint32_t donnees_len = 0;
  uint32_t pos = 12;
  while (pos + 8 <= (uint32_t)taille) {
    const uint8_t *e = brut + pos;
    const uint32_t lg = lit32(e + 4);
    if (!memcmp(e, "fmt ", 4) && lg >= 16) {
      format  = lit16(e + 8);
      voies   = lit16(e + 10);
      cadence = lit32(e + 12);
      bits    = lit16(e + 22);
    } else if (!memcmp(e, "data", 4)) {
      donnees = e + 8;
      donnees_len = lg;
      if (pos + 8 + lg > (uint32_t)taille) donnees_len = (uint32_t)taille - pos - 8;
    }
    pos += 8 + lg + (lg & 1);              // les morceaux sont alignes sur 2
  }
  if (!donnees || voies < 1 || cadence == 0 ||
      (format != 1 && format != 3) ||
      (bits != 8 && bits != 16 && bits != 24 && bits != 32)) {
    free(brut); return nullptr;
  }

  const int octets = bits / 8;
  const uint32_t cadres = donnees_len / (uint32_t)(octets * voies);
  if (cadres == 0) { free(brut); return nullptr; }

  // ── Reechantillonnage vers 32 kHz, en virgule fixe ───────────────────────
  // Interpolation lineaire : suffisante pour du 8 bits en sortie, et sans
  // division dans la boucle.
  const uint64_t sortie = ((uint64_t)cadres * MD_AUDIO_RATE) / cadence;
  if (sortie == 0 || sortie > 8u * 1024u * 1024u) { free(brut); return nullptr; }
  uint8_t *res = (uint8_t *)malloc((size_t)sortie);
  if (!res) { free(brut); return nullptr; }

  const uint32_t pas = (uint32_t)(((uint64_t)cadence << 16) / MD_AUDIO_RATE);
  uint32_t curseur = 0;
  for (uint32_t i = 0; i < sortie; i++) {
    const uint32_t idx = curseur >> 16;
    const uint32_t frac = curseur & 0xFFFF;
    const uint32_t i0 = idx < cadres ? idx : cadres - 1;
    const uint32_t i1 = (idx + 1) < cadres ? idx + 1 : cadres - 1;

    // Les voies sont moyennees : un echantillon stereo joue en mono sur la
    // Mega Drive de toute facon.
    int32_t a = 0, b = 0;
    for (int c = 0; c < voies; c++) {
      a += echantillon_q15(donnees + (size_t)(i0 * voies + c) * octets, bits, format);
      b += echantillon_q15(donnees + (size_t)(i1 * voies + c) * octets, bits, format);
    }
    a /= voies; b /= voies;

    const int32_t v = a + (int32_t)(((int64_t)(b - a) * frac) >> 16);
    int32_t u = (v >> 8) + 128;            // Q15 -> 8 bits non signe
    if (u < 0) u = 0;
    if (u > 255) u = 255;
    res[i] = (uint8_t)u;
    curseur += pas;
  }
  free(brut);
  if (sortie_len) *sortie_len = (uint32_t)sortie;
  return res;
}
