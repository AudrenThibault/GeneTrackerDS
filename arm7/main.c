// ============================================================================
//  Le noyau ARM7 de MD Tracker DS.
//
//  Pourquoi ce fichier existe : la DS a deux processeurs. L'ARM9 fait le
//  tracker ; l'ARM7 tient le son, les touches, l'ecran tactile et le bouton
//  d'alimentation, et parle a l'ARM9 par une file de messages (le FIFO).
//
//  libnds 2.0 fournissait ce noyau tout fait (les binaires calico
//  ds7_*.elf). On est revenu a libnds 1.8.3, qui ne le fournit pas : a cette
//  epoque chaque projet compilait le sien. C'est du code d'ossature, identique
//  d'un homebrew a l'autre.
//
//  Le retour a libnds 1.8.3 n'est pas un caprice : nds-bootstrap — le
//  chargeur que TWiLight Menu++ utilise sur la vraie console — ne sait pas
//  demarrer les cartouches produites par libnds 2.0. C'est un defaut connu et
//  toujours ouvert (DS-Homebrew/nds-bootstrap, issue #1777, fevrier 2025), et
//  il touche aussi bien l'exemple officiel de devkitPro que ce tracker.
// ============================================================================
#include <nds.h>

// Rien a faire au retour de balayage : c'est l'ARM9 qui dessine. L'interruption
// doit exister quand meme, sinon swiWaitForVBlank() ne serait jamais reveille.
static void vblankHandler(void) {}

// La position de l'ecran tactile et l'etat des boutons sont lus ici, puis
// envoyes a l'ARM9 : lui n'a pas acces au materiel d'entree.
static void vcountHandler(void) { inputGetAndSend(); }

static volatile bool arret = false;
static void boutonAlim(void) { arret = true; }

int main(void) {
  // Les reglages de la console (langue, nom, calibrage tactile) avant tout :
  // le calibrage sert des la premiere lecture de l'ecran tactile.
  readUserSettings();
  irqInit();
  fifoInit();

  // DEUX services distincts, et c'est le piege : installSystemFIFO() donne
  // l'alimentation, le stockage et l'heure, mais PAS le son. Le son a son
  // propre installateur. Sans lui, soundPlaySample() cote ARM9 envoie sa
  // demande et attend une reponse qui ne vient jamais : la cartouche se fige,
  // ecran de fond dessine et rien d'autre.
  installSoundFIFO();
  installSystemFIFO();

  // Le compteur de ligne qui declenche la lecture des entrees.
  SetYtrigger(80);

  irqSet(IRQ_VCOUNT, vcountHandler);
  irqSet(IRQ_VBLANK, vblankHandler);
  irqEnable(IRQ_VBLANK | IRQ_VCOUNT);

  setPowerButtonCB(boutonAlim);

  while (!arret) swiWaitForVBlank();
  return 0;
}
