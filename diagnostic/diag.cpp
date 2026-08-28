// ============================================================================
//  Cartouche de DIAGNOSTIC. Elle ne fait rien d'autre qu'afficher un degrade
//  sur l'ecran du haut, par le chemin le plus direct qui existe sur DS :
//  MODE_FB0, qui envoie la VRAM A directement a l'ecran, sans couche de fond,
//  sans moteur, sans rien.
//
//  Si ce degrade APPARAIT et que le tracker reste blanc, le defaut est dans ma
//  mise en place des couches de fond.
//  S'il reste blanc LUI AUSSI, le defaut est ailleurs : amorcage, emulateur, ou
//  reglages de celui-ci.
// ============================================================================
#include <nds.h>
#include <calico/system/thread.h>

int main(void) {
  powerOn(POWER_ALL_2D);
  lcdMainOnTop();
  videoSetMode(MODE_FB0);
  vramSetBankA(VRAM_A_LCD);
  for (int y = 0; y < 192; y++)
    for (int x = 0; x < 256; x++)
      VRAM_A[y * 256 + x] = ARGB16(1, (x >> 3) & 31, (y >> 3) & 31, 8);
  while (1) threadWaitForVBlank();
  return 0;
}
