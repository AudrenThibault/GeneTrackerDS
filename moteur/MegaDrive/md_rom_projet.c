#include "md_rom_projet.h"
#include "md_compact.h"
#include "md_compact_codec.h"
#include "../CustomReplayer/md_replayer.h"
#include <string.h>

// ── Lire du gros-boutien ─────────────────────────────────────────────────
// La ROM vient d'un 68000 : l'octet de poids fort est en premier. L'ARM range
// l'inverse. Un cast donnerait des adresses absurdes, alors on assemble.
static uint32_t be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
       | ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}
static uint16_t be16(const uint8_t *p) {
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

int md_rom_plan_lit(const uint8_t *rom, uint32_t taille, md_rom_plan_t *p) {
  const char *m = MD_ROM_MARQUE;
  const uint32_t lm = (uint32_t)strlen(m);
  if (taille < lm + 80) return 0;
  for (uint32_t i = 0; i + lm + 80 <= taille; i++) {
    if (memcmp(rom + i, m, lm) != 0) continue;
    // Seize champs, apres la marque (16 octets) et la version (4).
    const uint8_t *v = rom + i + 16 + 4;
    uint32_t *d = &p->morceaux_n;
    for (int k = 0; k < 16; k++) d[k] = be32(v + k * 4);
    // Un plan credible : les zones tombent dans l'image.
    if (p->morceaux_data + p->morceaux_capacite > taille) return 0;
    if (p->pcm_banque + p->pcm_capacite > taille) return 0;
    return 1;
  }
  return 0;
}

int md_rom_morceaux(const uint8_t *rom, const md_rom_plan_t *p) {
  const int n = rom[p->morceaux_n];
  return (n < 0 || (uint32_t)n > p->morceaux_max) ? 0 : n;
}

void md_rom_nom(const uint8_t *rom, const md_rom_plan_t *p, int i, char nom[11]) {
  const uint8_t *s = rom + p->morceaux_nom + (uint32_t)i * 11;
  int k = 0;
  while (k < 10 && s[k]) { nom[k] = (char)s[k]; k++; }
  nom[k] = 0;
}

// ── Le morceau compact, verse dans le tracker ────────────────────────────
// Le bloc de 32 576 octets a la disposition decrite dans md_compact.h. On le
// recopie champ par champ vers le replayer : c'est le seul endroit ou les deux
// representations se rencontrent.
static uint8_t compact[MDC_TAILLE_TOTALE];

static void verse_song(const uint8_t *s) {
  for (int c = 0; c < MDC_CANAUX; c++)
    for (int r = 0; r < MDC_SONG_LIGNES; r++)
      md_replayer_set_song(c, r, s[MDC_OFF_SONG + c * MDC_SONG_LIGNES + r]);
}

static void verse_chains(const uint8_t *s) {
  for (int c = 0; c < MDC_MAX_CHAINS; c++)
    for (int r = 0; r < MDC_LIGNES_CHAIN; r++) {
      const uint8_t *e = s + MDC_OFF_CHAINS + (c * MDC_LIGNES_CHAIN + r) * 2;
      md_replayer_set_chain(c, r, e[0], (int8_t)e[1]);
    }
}

static void verse_phrases(const uint8_t *s) {
  for (int p = 0; p < MDC_MAX_PHRASES; p++)
    for (int r = 0; r < MDC_LIGNES_PHRASE; r++) {
      const uint8_t *e = s + MDC_OFF_PHRASES
                       + (p * MDC_LIGNES_PHRASE + r) * MDC_PHRASE_OCTETS;
      md_replayer_set_phrase(p, r, e[0], e[1], e[2], e[3], e[4], e[5], e[6]);
    }
}

static void verse_tables(const uint8_t *s) {
  for (int t = 0; t < MDC_MAX_TABLES; t++)
    for (int r = 0; r < MDC_LIGNES_TABLE; r++) {
      const uint8_t *e = s + MDC_OFF_TABLES
                       + (t * MDC_LIGNES_TABLE + r) * MDC_TABLE_OCTETS;
      md_replayer_set_table_row(t, r, e[0], (int8_t)e[1],
                                e[2], e[3], e[4], e[5], e[6], e[7]);
    }
}

// ⚠️ L'ORDRE DES 59 PREMIERS OCTETS EST CELUI DE LA SERIALISATION DU .MDM,
// pas celui de la structure en memoire. Quatre operateurs de onze octets, puis
// les globaux. S'y tromper melange les enveloppes — c'est deja arrive.
static void verse_instruments(const uint8_t *s, const int *remap) {
  static const int PROP[11] = {
    MD_OP_PROP_DETUNE, MD_OP_PROP_MULTIPLE, MD_OP_PROP_TOTAL_LEVEL,
    MD_OP_PROP_KEY_SCALE, MD_OP_PROP_ATTACK, MD_OP_PROP_DECAY,
    MD_OP_PROP_SUSTAIN_RATE, MD_OP_PROP_SUSTAIN_LEVEL, MD_OP_PROP_RELEASE,
    MD_OP_PROP_AM, MD_OP_PROP_SSG_EG
  };
  static const int GEN[6] = {
    MD_GEN_PROP_ALGORITHM, MD_GEN_PROP_FEEDBACK, MD_GEN_PROP_AMS,
    MD_GEN_PROP_PMS, MD_GEN_PROP_LFO_ENABLE, MD_GEN_PROP_LFO_FREQ
  };
  for (int i = 0; i < MDC_MAX_INSTR; i++) {
    const uint8_t *b = s + MDC_OFF_INSTR + (uint32_t)i * MDC_INSTR_OCTETS;
    for (int op = 0; op < 4; op++)
      for (int k = 0; k < 11; k++)
        md_replayer_set_instr_op_val(i + 1, op, PROP[k], b[op * 11 + k]);
    for (int k = 0; k < 6; k++)
      md_replayer_set_instr_gen_val(i + 1, GEN[k], b[44 + k]);
    md_replayer_set_instr_gen_val(i + 1, MD_GEN_PROP_PANNING,   b[50]);
    md_replayer_set_instr_gen_val(i + 1, MD_GEN_PROP_FINE_TUNE, (int8_t)b[51]);
    md_replayer_set_instr_gen_val(i + 1, MD_GEN_PROP_PSG_NOISE, b[52]);

    // L'enveloppe PSG : amplitude et vitesse ENTRELACEES, (53,54) (55,56)
    // (57,58). C'est la serialisation qui fait foi, pas la structure.
    for (int pt = 0; pt < 3; pt++) {
      md_replayer_set_env_amp(i + 1, pt, b[53 + pt * 2]);
      md_replayer_set_env_speed(i + 1, pt, b[54 + pt * 2]);
    }

    md_replayer_set_instr_table(i + 1, b[59]);
    md_replayer_set_instr_kind(i + 1, b[60]);
    // ⚠️ Le numero d'echantillon est celui de la ROM : il faut le traduire
    // vers l'emplacement que l'echantillon a pris dans la banque de la DS.
    const uint8_t ech = b[61];
    md_replayer_set_instr_sample(i + 1,
        (ech < MDC_MAX_INSTR && remap[ech] >= 0) ? remap[ech] : -1);
    md_replayer_set_instr_pcm_volume(i + 1, b[62]);

    // Les trois macros PSG.
    { uint8_t v[MDC_MACRO_VOL_PAS];
      const int n = b[MDC_OFF_VOL_LEN] > MDC_MACRO_VOL_PAS
                  ? MDC_MACRO_VOL_PAS : b[MDC_OFF_VOL_LEN];
      for (int k = 0; k < n; k++) v[k] = b[MDC_OFF_VOL_MAC + k];
      md_replayer_set_psg_vol_macro(i + 1, v, n, b[MDC_OFF_VOL_BOUCLE]); }
    { int8_t v[MDC_MACRO_ARP_PAS];
      const int n = b[MDC_OFF_ARP_LEN] > MDC_MACRO_ARP_PAS
                  ? MDC_MACRO_ARP_PAS : b[MDC_OFF_ARP_LEN];
      for (int k = 0; k < n; k++) v[k] = (int8_t)b[MDC_OFF_ARP_MAC + k];
      md_replayer_set_psg_arp_macro(i + 1, v, n, b[MDC_OFF_ARP_BOUCLE],
                                    b[MDC_OFF_ARP_FIXE] != 0); }
    { const int n = b[MDC_OFF_NZ_LEN] > MDC_MACRO_NZ_PAS
                  ? MDC_MACRO_NZ_PAS : b[MDC_OFF_NZ_LEN];
      md_replayer_set_psg_noise_len(i + 1, n);
      md_replayer_set_psg_noise_loop(i + 1, b[MDC_OFF_NZ_BOUCLE]);
      for (int k = 0; k < n; k++)
        md_replayer_set_psg_noise_step(i + 1, k, b[MDC_OFF_NZ_MAC + k]); }

    char nom[MDC_NOM_OCTETS + 1];
    int k = 0;
    while (k < MDC_NOM_OCTETS && b[MDC_OFF_NOM + k]) { nom[k] = (char)b[MDC_OFF_NOM + k]; k++; }
    nom[k] = 0;
    if (nom[0]) md_replayer_set_instr_name(i + 1, nom);
  }
}

// ── Les echantillons de la ROM entrent dans la banque de la DS ───────────
// On ne prend QUE ceux qu'un instrument emploie : recopier les trente-deux
// remplirait la banque de sons dont le morceau n'a que faire.
static void verse_echantillons(const uint8_t *rom, const md_rom_plan_t *p,
                               const uint8_t *s, int *remap) {
  for (int k = 0; k < 32; k++) remap[k] = -1;
  for (int i = 0; i < MDC_MAX_INSTR; i++) {
    const uint8_t *b = s + MDC_OFF_INSTR + (uint32_t)i * MDC_INSTR_OCTETS;
    const uint8_t e = b[61];
    if (e >= 32 || remap[e] >= 0) continue;
    const uint32_t off = be32(rom + p->pcm_offset   + (uint32_t)e * 4);
    const uint32_t len = be32(rom + p->pcm_longueur + (uint32_t)e * 4);
    if (!len || off + len > p->pcm_capacite) continue;
    char nom[18];
    int k = 0;
    const uint8_t *sn = rom + p->pcm_nom + (uint32_t)e * 17;
    while (k < 16 && sn[k]) { nom[k] = (char)sn[k]; k++; }
    nom[k] = 0;
    const int boucle = (int)be32(rom + p->pcm_boucle + (uint32_t)e * 4);
    const int idx = md_replayer_add_sample(nom, rom + p->pcm_banque + off, len,
                                           boucle, rom[p->pcm_note + e]);
    if (idx < 0) continue;
    remap[e] = idx;
  }
}

int md_rom_projet_importe(const uint8_t *rom, const md_rom_plan_t *p, int i) {
  if (i < 0 || i >= md_rom_morceaux(rom, p)) return 0;
  const uint32_t off = be32(rom + p->morceaux_offset + (uint32_t)i * 4);
  const uint16_t len = be16(rom + p->morceaux_taille + (uint32_t)i * 2);
  if (!len || off + len > p->morceaux_capacite) return 0;

  md_codec_decomprime(rom + p->morceaux_data + off, len, compact);

  int remap[32];
  verse_echantillons(rom, p, compact, remap);
  verse_song(compact);
  verse_chains(compact);
  verse_phrases(compact);
  verse_tables(compact);
  verse_instruments(compact, remap);
  return 1;
}
