// ============================================================================
//  LE CODEC DU FORMAT COMPACT MEGA DRIVE — copie conforme.
//
//  ⚠️ COPIE, PAS DEPENDANCE. Ce fichier vient de
//  « native megadrive Tracker/moteur/morceau/md_codec.c ». Les deux projets
//  restent independants ; en echange, une correction la-bas doit etre reportee
//  ici, sinon les fichiers echanges ne se relisent plus.
//
//  Seule difference avec l'original : l'en-tete des constantes s'appelle
//  md_compact.h ici, et md_song.h la-bas.
// ============================================================================
#include "md_compact_codec.h"
#include "md_compact.h"
#include <string.h>
#include <stddef.h>


// La ligne de phrase NEUTRE, champ par champ. Une ligne égale à celle-ci
// n'existe pas dans la forme comprimée. ⚠️ 0 et MDC_VIDE ne sont pas
// interchangeables : 0 veut dire « rien », MDC_VIDE veut dire « note coupée »
// pour la note et « pas de commande » pour les autres.
static const uint8_t NEUTRE[7] = {0, 0, MDC_VIDE, MDC_VIDE, 0, MDC_VIDE, 0};

// ── Étage 1 : le codage creux ──────────────────────────────────────────────
static uint32_t creux(const uint8_t *s, uint8_t *o) {
  uint32_t n = 0;
  for (int k = 0; k < 32; k++) o[n++] = s[k];      // l'en-tête utile

  // SONG : longueur utile puis valeurs brutes. Il est DENSE.
  for (int c = 0; c < MDC_CANAUX; c++) {
    const uint8_t *col = s + MDC_OFF_SONG + c * MDC_SONG_LIGNES;
    int len = 0;
    for (int l = 0; l < MDC_SONG_LIGNES; l++) if (col[l] != MDC_VIDE) len = l + 1;
    o[n++] = (uint8_t)(len >> 8); o[n++] = (uint8_t)len;
    for (int l = 0; l < len; l++) o[n++] = col[l];
  }

  // CHAINS : un masque de seize bits, puis les lignes présentes.
  uint32_t pos = n; n++;
  int compte = 0;
  for (int i = 0; i < MDC_MAX_CHAINS; i++) {
    const uint8_t *ch = s + MDC_OFF_CHAINS + i * 32;
    uint16_t mk = 0;
    for (int r = 0; r < 16; r++) if (ch[r*2] != MDC_VIDE) mk |= (uint16_t)(1u << r);
    if (!mk) continue;
    o[n++] = (uint8_t)i; o[n++] = (uint8_t)mk; o[n++] = (uint8_t)(mk >> 8);
    for (int r = 0; r < 16; r++)
      if (mk >> r & 1) { o[n++] = ch[r*2]; o[n++] = ch[r*2+1]; }
    compte++;
  }
  o[pos] = (uint8_t)compte;

  // PHRASES : masque de lignes, puis un masque de CHAMPS par ligne. Une ligne
  // moyenne ne pose que deux champs sur sept — c'est là qu'est le gros du gain.
  pos = n; n++; compte = 0;
  for (int i = 0; i < MDC_MAX_PHRASES; i++) {
    const uint8_t *ph = s + MDC_OFF_PHRASES + i * 112;
    uint16_t mk = 0;
    for (int r = 0; r < 16; r++) {
      int vide = 1;
      for (int k = 0; k < 7; k++) if (ph[r*7+k] != NEUTRE[k]) { vide = 0; break; }
      if (!vide) mk |= (uint16_t)(1u << r);
    }
    if (!mk) continue;
    o[n++] = (uint8_t)i; o[n++] = (uint8_t)mk; o[n++] = (uint8_t)(mk >> 8);
    for (int r = 0; r < 16; r++) {
      if (!(mk >> r & 1)) continue;
      uint8_t fm = 0;
      for (int k = 0; k < 7; k++) if (ph[r*7+k] != NEUTRE[k]) fm |= (uint8_t)(1u << k);
      o[n++] = fm;
      for (int k = 0; k < 7; k++) if (fm >> k & 1) o[n++] = ph[r*7+k];
    }
    compte++;
  }
  o[pos] = (uint8_t)compte;

  // INSTRUMENTS : l'ÉCART au patch le plus répandu. Un morceau n'en modifie
  // qu'une poignée de champs par instrument.
  const uint8_t *dft = s + MDC_OFF_INSTR;   // le premier fait référence
  pos = n; n++; compte = 0;
  uint32_t posdft = n;
  for (int k = 0; k < MDC_INSTR_OCTETS; k++) o[n++] = dft[k];
  for (int i = 1; i < MDC_MAX_INSTR; i++) {
    const uint8_t *r = s + MDC_OFF_INSTR + i * MDC_INSTR_OCTETS;
    if (!memcmp(r, dft, MDC_INSTR_OCTETS)) continue;
    o[n++] = (uint8_t)i;
    uint32_t pm = n;
    for (int k = 0; k < MDC_INSTR_OCTETS / 8; k++) o[n++] = 0;
    for (int k = 0; k < MDC_INSTR_OCTETS; k++)
      if (r[k] != dft[k]) { o[pm + k/8] |= (uint8_t)(1u << (k & 7)); o[n++] = r[k]; }
    compte++;
  }
  o[pos] = (uint8_t)compte;
  (void)posdft;

  // TABLES
  pos = n; n++; compte = 0;
  for (int i = 0; i < MDC_MAX_TABLES; i++) {
    const uint8_t *tb = s + MDC_OFF_TABLES + i * 128;
    uint16_t mk = 0;
    for (int r = 0; r < 16; r++) {
      const uint8_t *l = tb + r * 8;
      if (l[0] || l[1] || l[2] != MDC_VIDE || l[4] != MDC_VIDE || l[6] != MDC_VIDE)
        mk |= (uint16_t)(1u << r);
    }
    if (!mk) continue;
    o[n++] = (uint8_t)i; o[n++] = (uint8_t)mk; o[n++] = (uint8_t)(mk >> 8);
    for (int r = 0; r < 16; r++)
      if (mk >> r & 1) for (int k = 0; k < 8; k++) o[n++] = tb[r*8+k];
    compte++;
  }
  o[pos] = (uint8_t)compte;
  return n;
}

static void creux_inverse(const uint8_t *e, uint8_t *s) {
  uint32_t n = 0;
  md_codec_vide(s);
  for (int k = 0; k < 32; k++) s[k] = e[n++];

  for (int c = 0; c < MDC_CANAUX; c++) {
    const int len = (e[n] << 8) | e[n+1]; n += 2;
    uint8_t *col = s + MDC_OFF_SONG + c * MDC_SONG_LIGNES;
    for (int l = 0; l < len; l++) col[l] = e[n++];
  }

  int compte = e[n++];
  for (int j = 0; j < compte; j++) {
    const int i = e[n++];
    const uint16_t mk = (uint16_t)(e[n] | (e[n+1] << 8)); n += 2;
    uint8_t *ch = s + MDC_OFF_CHAINS + i * 32;
    for (int r = 0; r < 16; r++)
      if (mk >> r & 1) { ch[r*2] = e[n++]; ch[r*2+1] = e[n++]; }
  }

  compte = e[n++];
  for (int j = 0; j < compte; j++) {
    const int i = e[n++];
    const uint16_t mk = (uint16_t)(e[n] | (e[n+1] << 8)); n += 2;
    uint8_t *ph = s + MDC_OFF_PHRASES + i * 112;
    for (int r = 0; r < 16; r++) {
      if (!(mk >> r & 1)) continue;
      const uint8_t fm = e[n++];
      for (int k = 0; k < 7; k++) if (fm >> k & 1) ph[r*7+k] = e[n++];
    }
  }

  compte = e[n++];
  const uint8_t *dft = e + n; n += MDC_INSTR_OCTETS;
  for (int i = 0; i < MDC_MAX_INSTR; i++)
    memcpy(s + MDC_OFF_INSTR + i * MDC_INSTR_OCTETS, dft, MDC_INSTR_OCTETS);
  for (int j = 0; j < compte; j++) {
    const int i = e[n++];
    const uint8_t *pm = e + n; n += MDC_INSTR_OCTETS / 8;
    uint8_t *r = s + MDC_OFF_INSTR + i * MDC_INSTR_OCTETS;
    for (int k = 0; k < MDC_INSTR_OCTETS; k++)
      if (pm[k/8] >> (k & 7) & 1) r[k] = e[n++];
  }

  compte = e[n++];
  for (int j = 0; j < compte; j++) {
    const int i = e[n++];
    const uint16_t mk = (uint16_t)(e[n] | (e[n+1] << 8)); n += 2;
    uint8_t *tb = s + MDC_OFF_TABLES + i * 128;
    for (int r = 0; r < 16; r++)
      if (mk >> r & 1) for (int k = 0; k < 8; k++) tb[r*8+k] = e[n++];
  }
}

// ── Un morceau NEUF ────────────────────────────────────────────────────────
// Chaque champ a son propre neutre : un memset serait faux partout.
// ⚠️ L'INSTRUMENT NEUF EST CELUI DE LA DS, octet pour octet.
// Voir md_replayer_init_default_instrument() : deux piles de deux
// opérateurs, algorithme 4. Le niveau d'OP1 est volontairement haut (22)
// parce que le FEEDBACK du YM2612 n'agit QUE sur OP1 et proportionnellement
// à son niveau : trop atténué, bouger le feedback ne s'entend pas.
//
// Ordre des onze octets d'un opérateur :
//   0 détune  1 multiple  2 niveau  3 échelle  4 attaque  5 déclin
//   6 cadence de maintien  7 niveau de maintien  8 relâchement
//   9 modulation d'amplitude  10 SSG-EG
void md_instr_defaut(uint8_t *b) {
  static const uint8_t OP_DEFAUT[4][11] = {
    { 0, 1, 22, 0, 31, 8, 12, 2, 8, 0, 0 },   // OP1, modulateur
    { 0, 1,  4, 0, 31, 6, 14, 2, 8, 0, 0 },   // OP2, porteuse
    { 1, 2, 30, 0, 31, 8, 12, 2, 8, 0, 0 },   // OP3, modulateur désaccordé
    { 0, 1,  4, 0, 31, 6, 14, 2, 8, 0, 0 },   // OP4, porteuse
  };

  for (int op = 0; op < 4; op++)
    for (int k = 0; k < 11; k++) b[op*11 + k] = OP_DEFAUT[op][k];
  b[44] = 4;             // algorithme
  b[45] = 4;             // feedback
  b[46] = 0;             // AM sens
  b[47] = 0;             // PM sens
  b[48] = 0;             // LFO éteint
  b[49] = 0;             // vitesse du LFO
  b[50] = 0;             // sortie : centre
  b[51] = 0;             // désaccord fin
  // Bruit blanc verrouillé sur le ton de la voie 3 : le SEUL mode du SN76489
  // où la hauteur du bruit suit la note écrite.
  b[52] = 7;
  // Enveloppe PSG : on part fort et on descend vers le silence, pour que la
  // note s'éteigne seule sans qu'on ait à semer des note-off.
  b[53] = 0xD; b[54] = 8;
  b[55] = 0;   b[56] = 0;
  b[57] = MDC_VIDE; b[58] = 0;
  b[59] = MDC_VIDE;       // aucune table
  // AUCUN échantillon : zéro désignerait le PREMIER de la banque, et un
  // instrument PCM neuf naîtrait avec le son du précédent.
  b[61] = MDC_VIDE;
  b[62] = 127;           // volume PCM

  // ── Les trois macros PSG : aucune, et SANS point de bouclage ────────────
  // ⚠️ Un point de bouclage à zéro n'est pas « pas de bouclage » : c'est
  // « reboucle sur le premier pas ». Une macro de volume écrite à la main
  // repartirait donc de son attaque indéfiniment et la note ne s'éteindrait
  // jamais. MDC_VIDE veut dire « tiens la dernière valeur », comme sur la DS.
  b[MDC_OFF_VOL_LEN] = 0; b[MDC_OFF_VOL_BOUCLE] = MDC_VIDE;
  b[MDC_OFF_ARP_LEN] = 0; b[MDC_OFF_ARP_BOUCLE] = MDC_VIDE;
  b[MDC_OFF_ARP_FIXE] = 0;
  b[MDC_OFF_NZ_LEN]  = 0; b[MDC_OFF_NZ_BOUCLE]  = MDC_VIDE;
}

void md_codec_vide(uint8_t *s) {
  memset(s, 0, MDC_TAILLE_TOTALE);
  memset(s + MDC_OFF_SONG, MDC_VIDE, MDC_TAILLE_SONG);
  for (int i = 0; i < MDC_MAX_CHAINS; i++)
    for (int r = 0; r < 16; r++) s[MDC_OFF_CHAINS + i*32 + r*2] = MDC_VIDE;
  for (int i = 0; i < MDC_MAX_PHRASES; i++)
    for (int r = 0; r < 16; r++)
      for (int k = 0; k < 7; k++) s[MDC_OFF_PHRASES + i*112 + r*7 + k] = NEUTRE[k];
  for (int i = 0; i < MDC_MAX_TABLES; i++)
    for (int r = 0; r < 16; r++) {
      uint8_t *l = s + MDC_OFF_TABLES + i*128 + r*8;
      l[2] = l[4] = l[6] = MDC_VIDE;
    }
  for (int i = 0; i < MDC_MAX_INSTR; i++)
    md_instr_defaut(s + MDC_OFF_INSTR + i * MDC_INSTR_OCTETS);
}

// ── Étage 2 : LZSS ─────────────────────────────────────────────────────────
// Un octet de drapeaux pour huit éléments : bit à 1 = littéral, bit à 0 =
// couple (recul sur 12 bits, longueur sur 4 bits, de 3 à 18).
//
// ⚠️ La recherche est GUIDÉE PAR UNE TABLE DE HACHAGE. Une recherche naïve
// compare chaque position à tout ce qui précède : sur 4 300 octets avec une
// fenêtre de 4 Ko, ça fait près de neuf millions de comparaisons, soit une
// dizaine de secondes sur un 68000. Inacceptable pour un geste de sauvegarde.
// Avec la table on ne visite que les positions qui commencent par les deux
// mêmes octets, et on s'arrête après trente-deux candidats.
#define FENETRE   2048   // mesuré : passer de 4096 à 2048 coûte 22 octets
                         // sur 2 738, et rend 8 Ko de RAM. Le choix est vite fait.
#define LONG_MAX  18
#define HACHE     2048
// Mesuré : passer de 32 candidats à 8 coûte 56 octets sur 2 760 — deux pour
// cent — et divise la recherche par quatre. Sur un 68000 c'est la différence
// entre une pause qu'on remarque et une qu'on ne remarque pas.
// Mesuré : 8 candidats au lieu de 32 coûtent 56 octets sur 2 760 — deux pour
// cent — pour une recherche quatre fois plus courte.
#define CANDIDATS 8

static uint16_t tete[HACHE];
static uint16_t chaine[FENETRE];

static uint16_t hache(const uint8_t *p) {
  return (uint16_t)(((p[0] << 4) ^ p[1]) & (HACHE - 1));
}

static uint32_t lzss(const uint8_t *d, uint32_t n, uint8_t *o, uint32_t place) {
  for (int i = 0; i < HACHE; i++) tete[i] = 0xFFFF;
  uint32_t s = 0, i = 0;
  while (i < n) {
    const uint32_t pd = s; if (s >= place) return 0;
    o[s++] = 0;
    uint8_t drap = 0;
    for (int b = 0; b < 8 && i < n; b++) {
      uint32_t ml = 0, mp = 0;
      if (i + 1 < n) {
        uint16_t p = tete[hache(d + i)];
        for (int c = 0; c < CANDIDATS && p != 0xFFFF; c++) {
          const uint32_t pos = p;
          if (i - pos > FENETRE) break;
          uint32_t l = 0;
          while (l < LONG_MAX && i + l < n && d[pos + l] == d[i + l]) l++;
          if (l > ml) { ml = l; mp = pos; if (l == LONG_MAX) break; }
          p = chaine[pos & (FENETRE - 1)];
        }
      }
      if (ml >= 3) {
        const uint32_t rec = i - mp;
        if (s + 2 > place) return 0;
        o[s++] = (uint8_t)(rec >> 4);
        o[s++] = (uint8_t)(((rec & 0xF) << 4) | (ml - 3));
      } else {
        drap |= (uint8_t)(1u << b);
        if (s >= place) return 0;
        o[s++] = d[i]; ml = 1;
      }
      for (uint32_t k = 0; k < ml; k++) {
        if (i + 1 < n) {
          const uint16_t h = hache(d + i);
          chaine[i & (FENETRE - 1)] = tete[h];
          tete[h] = (uint16_t)i;
        }
        i++;
      }
    }
    o[pd] = drap;
  }
  return s;
}

static uint32_t lzss_inverse(const uint8_t *e, uint32_t n, uint8_t *o) {
  uint32_t s = 0, i = 0;
  while (i < n) {
    const uint8_t drap = e[i++];
    for (int b = 0; b < 8 && i < n; b++) {
      if (drap >> b & 1) { o[s++] = e[i++]; }
      else {
        const uint32_t rec = ((uint32_t)e[i] << 4) | (e[i+1] >> 4);
        const uint32_t len = (e[i+1] & 0xF) + 3;
        i += 2;
        for (uint32_t k = 0; k < len; k++) { o[s] = o[s - rec]; s++; }
      }
    }
  }
  return s;
}

// ── Interface ──────────────────────────────────────────────────────────────
// La forme creuse d'un morceau ORDINAIRE tient largement là-dedans : LA DIFFE,
// qui est un vrai morceau, en occupe 4 302. On borne quand même, et on ÉCHOUE
// franchement si ça déborde — la RAM de la console est comptée, et un
// dépassement silencieux écraserait le morceau lui-même.
#define TAMPON_MAX 12288
static uint8_t tampon[TAMPON_MAX];

uint32_t md_codec_comprime(const uint8_t *morceau, uint8_t *sortie, uint32_t place) {
  const uint32_t n = creux(morceau, tampon);
  if (n > TAMPON_MAX) return 0;   // morceau trop chargé : on le dit
  return lzss(tampon, n, sortie, place);
}

uint32_t md_codec_decomprime(const uint8_t *entree, uint32_t taille, uint8_t *morceau) {
  const uint32_t n = lzss_inverse(entree, taille, tampon);
  creux_inverse(tampon, morceau);
  return n;
}
