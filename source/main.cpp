// ============================================================================
//  MD Tracker DS — le meme tracker Mega Drive, natif Nintendo DSi.
//
//  Ecran du HAUT : l'ecran CRT, plein cadre. Rien autour : ni meuble, ni
//  panneaux, ni liste d'instruments — c'est le parti pris, comme LSDJ.
//  Ecran du BAS : vide pour l'instant.
//
//  Tout se pilote a la croix et aux boutons, a la LSDJ. L'ecran tactile n'est
//  pas utilise.
// ============================================================================
#include <nds.h>
#include <calico/system/thread.h>
#include <stdio.h>
#include <string.h>

#include "md_font.h"

extern "C" {
#include "CustomReplayer/md_replayer.h"
}

// L'ecran DS fait 256 x 192. Avec une cellule de 4 x 6 on obtient 64 colonnes
// sur 32 lignes — la densite qu'il faut pour dix canaux.
static const int kEcranL = 256;
static const int kEcranH = 192;
static const int kCols   = kEcranL / MD_CELL_W;   // 64
static const int kLignes = kEcranH / MD_CELL_H;   // 32

static u16 *g_fond = nullptr;

// ── Couleurs du CRT ─────────────────────────────────────────────────────────
// Le vert du tracker, sur un noir tres legerement bleute : un noir pur fait
// « ecran eteint », alors qu'un tube en a toujours un peu sous la main.
static inline u16 rvb(int r, int v, int b) { return ARGB16(1, r, v, b); }
static const u16 kFond    = rvb(1, 2, 3);
static const u16 kTexte   = rvb(12, 31, 16);
static const u16 kAttenue = rvb(6, 15, 8);
static const u16 kEntete  = rvb(20, 31, 24);

static void pixel(int x, int y, u16 c) {
  if ((unsigned)x < (unsigned)kEcranL && (unsigned)y < (unsigned)kEcranH)
    g_fond[y * kEcranL + x] = c;
}

// Dessine un caractere a la position (colonne, ligne) en cellules.
static void car(int col, int lig, char c, u16 couleur) {
  int code = (unsigned char)c;
  if (code < 32 || code > 95) code = 32;
  const uint8_t *gl = md_font[code - 32];
  int x0 = col * MD_CELL_W, y0 = lig * MD_CELL_H;
  for (int r = 0; r < MD_FONT_HT; r++)
    for (int k = 0; k < MD_FONT_W; k++)
      if (gl[r] & (1 << (MD_FONT_W - 1 - k)))
        pixel(x0 + k, y0 + r, couleur);
}

static void texte(int col, int lig, const char *s, u16 couleur) {
  for (int i = 0; s[i]; i++) car(col + i, lig, s[i], couleur);
}

// ── Trame du CRT ────────────────────────────────────────────────────────────
// Une ligne sur deux legerement assombrie : c'est ce qui donne l'oeil du tube.
// Elle est appliquee au fond avant d'ecrire le texte, pas apres, pour que les
// caracteres restent nets.
static void trame() {
  for (int y = 0; y < kEcranH; y++) {
    u16 c = (y & 1) ? rvb(0, 1, 1) : kFond;
    for (int x = 0; x < kEcranL; x++) g_fond[y * kEcranL + x] = c;
  }
}

int main(void) {
  // ── Video ─────────────────────────────────────────────────────────────
  // Ecran du haut en bitmap 16 bits : on maitrise chaque pixel, ce qu'il faut
  // pour un rendu de tube. VRAM A lui suffit (256 x 192 x 2 = 96 Ko sur 128).
  powerOn(POWER_ALL_2D);
  lcdMainOnTop();
  videoSetMode(MODE_5_2D);
  vramSetBankA(VRAM_A_MAIN_BG);
  int bg = bgInit(2, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
  g_fond = bgGetGfxPtr(bg);

  // Ecran du bas : vide, comme demande.
  videoSetModeSub(MODE_5_2D);
  vramSetBankC(VRAM_C_SUB_BG);
  int bgSub = bgInitSub(2, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
  u16 *bas = bgGetGfxPtr(bgSub);
  for (int i = 0; i < kEcranL * kEcranH; i++) bas[i] = ARGB16(1, 0, 0, 0);

  trame();

  // ── Le moteur, celui de l'iPad, compile pour ARM ──────────────────────
  md_replayer_init(32768);   // cadence de sortie de la DS

  // ── Page SONG ─────────────────────────────────────────────────────────
  // Dix canaux : six FM (dont FM6 qui devient le PCM) et quatre PSG.
  static const char *noms[10] = {"FM1","FM2","FM3","FM4","FM5","PCM",
                                 "SQ1","SQ2","SQ3","NOI"};
  texte(0, 0, "MD TRACKER DS", kEntete);
  texte(52, 0, "SONG", kEntete);

  for (int c = 0; c < 10; c++) texte(4 + c * 6, 2, noms[c], kEntete);

  for (int l = 0; l < 26; l++) {
    char num[4];
    num[0] = "0123456789ABCDEF"[(l >> 4) & 15];
    num[1] = "0123456789ABCDEF"[l & 15];
    num[2] = 0;
    texte(0, 4 + l, num, kAttenue);
    for (int c = 0; c < 10; c++) texte(4 + c * 6, 4 + l, "- -", kAttenue);
  }

  while (1) {
    swiWaitForVBlank();
    scanKeys();
    if (keysDown() & KEY_START) break;
  }
  return 0;
}
