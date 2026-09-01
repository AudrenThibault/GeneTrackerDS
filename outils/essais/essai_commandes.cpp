// TOUTES les colonnes porteuses de commandes, pas seulement la premiere :
//   phrase  : CMD          et MD CMD
//   table   : CMD 1, CMD 2 et MD CMD
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "md_replayer.h"
#define RATE 26640
#define IMAGES 120
static int16_t buf[RATE];

enum { OU_RIEN, OU_PH_CMD, OU_PH_MD, OU_TB_CMD1, OU_TB_CMD2, OU_TB_MD };

static void rendu(int ou, int idx, uint8_t val, double *out) {
  md_replayer_init(RATE);
  md_replayer_new_empty();
  md_replayer_set_bpm(125.0);
  for (int r = 0; r < 16; r++)
    md_replayer_set_table_row(2, r, 0, 0, MD_EMPTY, 0, MD_EMPTY, 0, MD_EMPTY, 0);
  if (ou >= OU_TB_CMD1) {
    md_replayer_set_table_row(2, 0, 0, 0,
        ou == OU_TB_CMD1 ? (uint8_t)idx : MD_EMPTY, ou == OU_TB_CMD1 ? val : 0,
        ou == OU_TB_CMD2 ? (uint8_t)idx : MD_EMPTY, ou == OU_TB_CMD2 ? val : 0,
        ou == OU_TB_MD   ? (uint8_t)idx : MD_EMPTY, ou == OU_TB_MD   ? val : 0);
    md_replayer_set_instr_table(1, 2);
  } else if (ou == OU_RIEN) {
    md_replayer_set_instr_table(1, -1);
  } else {
    md_replayer_set_instr_table(1, -1);
  }
  for (int r = 0; r < 16; r++)
    md_replayer_set_phrase(0, r, (r % 2 == 0) ? (uint8_t)(45 + r) : 0,
        (r % 2 == 0) ? 1 : 0,
        (r % 2 == 0) ? 0x60 : 0,
        (ou == OU_PH_CMD && r == 0) ? (uint8_t)idx : MD_EMPTY,
        (ou == OU_PH_CMD && r == 0) ? val : 0,
        (ou == OU_PH_MD  && r == 0) ? (uint8_t)idx : MD_EMPTY,
        (ou == OU_PH_MD  && r == 0) ? val : 0);
  for (int r = 0; r < 16; r++)
    md_replayer_set_phrase(1, r, (r % 2 == 0) ? (uint8_t)(60 - r) : 0,
                           (r % 2 == 0) ? 1 : 0, (r % 2 == 0) ? 0x60 : 0,
                           MD_EMPTY, 0, MD_EMPTY, 0);
  md_replayer_set_chain(0, 0, 0, 0);
  md_replayer_set_chain(0, 1, 1, 0);
  md_replayer_set_song(0, 0, 0);
  md_replayer_set_song(0, 1, 0);
  md_replayer_set_play_scope(MD_SCOPE_SONG, 0, 0);
  md_replayer_play_from(0); md_replayer_play();
  const int par = RATE / 60;
  int n = 0;
  for (int f = 0; f < IMAGES; f++) {
    md_replayer_update((uint8_t *)buf, (unsigned)(par * 4));
    for (int i = 0; i < par * 2; i++) out[n++] = (double)buf[i];
  }
}
#define ECHS (IMAGES * (RATE / 60) * 2)
static double ecart(const double *a, const double *b) {
  double n = 0, d = 0;
  for (int i = 0; i < ECHS; i++) { n += fabs(a[i] - b[i]); d += fabs(b[i]); }
  return d > 0 ? 100.0 * n / d : 0.0;
}
static int differe(const double *a, const double *b) { return ecart(a, b) > 0.5; }
static uint8_t valPour(char l) {
  switch (l) { case 'A': return 0x05; case 'H': return 0x00; case 'C': return 0x47;
    case 'D': return 0x03; case 'K': return 0x02; case 'L': return 0x40;
    case 'M': return 0x40; case 'P': return 0x20; case 'R': return 0x03;
    case 'T': return 0x50; case 'V': return 0x44; case 'Z': return 0x44;
    case 'B': return 0x40; case 'E': return 0x41; case 'F': return 0x41;
    case 'S': return 0x04; case 'J': return 0x00; case 'N': return 0x04;
    case 'W': return 0x03; case 'U': return 0xA0; default: return 0x24; }
}

int main(void) {
  static double refPh[ECHS], refTb[ECHS], a[ECHS];
  rendu(OU_RIEN, 0, 0, refPh);
  rendu(OU_TB_CMD1, MD_EMPTY, 0, refTb);      // table attachee mais vide

  int ko = 0;
  printf("── colonnes CMD ──────────────────────────────────\n");
  printf("%-6s %-9s %-9s %-9s\n", "LETTRE", "PHRASE", "TABLE 1", "TABLE 2");
  for (int i = 0; i < md_table_cmd_count(); i++) {
    const char l = md_table_cmd_letter(i);
    const uint8_t v = valPour(l);
    rendu(OU_PH_CMD, i, v, a);   const int p = differe(a, refPh);
    rendu(OU_TB_CMD1, i, v, a);  const int t1 = differe(a, refTb);
    rendu(OU_TB_CMD2, i, v, a);  const int t2 = differe(a, refTb);
    if (!p || !t1 || !t2) ko++;
    printf("%-6c %-9s %-9s %-9s\n", l, p ? "agit" : "RIEN",
           t1 ? "agit" : "RIEN", t2 ? "agit" : "RIEN");
    if (!p || !t1 || !t2) {
      rendu(OU_PH_CMD, i, v, a);   const double ep = ecart(a, refPh);
      rendu(OU_TB_CMD1, i, v, a);  const double e1 = ecart(a, refTb);
      rendu(OU_TB_CMD2, i, v, a);  const double e2 = ecart(a, refTb);
      printf("       ecarts mesures : phrase %.2f %%  table1 %.2f %%  table2 %.2f %%\n",
             ep, e1, e2);
    }
  }

  printf("\n── colonnes MD CMD (codes DefleMask) ─────────────\n");
  printf("%-6s %-9s %-9s\n", "CODE", "PHRASE", "TABLE");
  int koMd = 0, nMd = 0;
  for (int i = 0; i < md_mdcmd_count(); i++) {
    const int code = md_mdcmd_code(i);
    nMd++;
    const uint8_t v = 0x22;
    rendu(OU_PH_MD, code, v, a);  const int p = differe(a, refPh);
    rendu(OU_TB_MD, code, v, a);  const int t = differe(a, refTb);
    if (!p || !t) koMd++;
    printf("%02X     %-9s %-9s\n", code, p ? "agit" : "RIEN", t ? "agit" : "RIEN");
  }
  printf("\nCMD sans effet : %d sur %d   |   MD CMD sans effet : %d sur %d\n",
         ko, md_table_cmd_count(), koMd, nMd);
  return 0;
}
