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
#include <calico/nds/scfg.h>
#include <calico/nds/system.h>
#include <maxmod9.h>
#include <stdio.h>
#include <string.h>

#include "md_font.h"
#include "morceau_dmf.h"

extern "C" {
#include "MegaDrive/md_chip.h"
}

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
static const char kHex[] = "0123456789ABCDEF";

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

// Efface une zone de texte avant de la reecrire.
static void efface(int col, int lig, int n) {
  // On efface UNE COLONNE DE PIXELS DE PLUS a gauche : le pave du curseur
  // deborde d'un pixel de ce cote, et sans ca il laissait une trainee derriere
  // lui a chaque deplacement.
  int x0 = col * MD_CELL_W - 1, y0 = lig * MD_CELL_H;
  for (int y = y0; y < y0 + MD_CELL_H; y++)
    for (int x = x0; x < x0 + n * MD_CELL_W + 1; x++)
      pixel(x, y, (y & 1) ? rvb(0, 1, 1) : kFond);
}

// ── Le son, en direct ───────────────────────────────────────────────────────
// maxmod a ete essaye et abandonne : en mode manuel il ne reclamait qu'un
// SEIZIEME des echantillons necessaires, quelle que soit la cadence et quel que
// soit le nombre d'appels ; en mode automatique il n'appelait jamais rien. La
// puce comblait le manque en rejouant son tampon — d'ou un son ralenti et un
// echo. On pilote donc les voies materielles nous-memes : c'est plus de code,
// mais chaque etape est verifiable.
//
// Deux voies mono, l'une a gauche l'autre a droite, qui bouclent chacune sur un
// anneau. On y ecrit en avance sur la lecture ; l'horloge, c'est le retour
// vertical, dont la cadence est exactement celle de la console.
//
// Le moteur remplit depuis la BOUCLE PRINCIPALE, jamais depuis une
// interruption : c'est ce que suppose md_lock.h, qui n'installe aucun verrou
// sur DS. Si ca changeait, il faudrait le corriger la-bas.
// La cadence de sortie est celle de la PUCE, pas 32 768.
//
// Avec la puce a horloge/288 = 26 633 Hz, sortir a 32 768 obligeait a
// interpoler vers le haut : un travail par echantillon de sortie, pour aucun
// detail supplementaire — il n'y a rien au-dessus de 13 kHz a restituer. En
// sortant a la cadence de la puce, c'est un pour un : plus d'interpolation, et
// 19 % d'echantillons de sortie en moins a produire.
#define SON_HZ      (MD_YM2612_CLOCK / MD_YM_DIVISEUR)
#define SON_IMAGE   (SON_HZ / 60)          // echantillons par image
// L'anneau est une PUISSANCE DE DEUX : l'ARM9 n'a pas d'instruction de
// division, donc un modulo par une taille quelconque appelle une routine
// logicielle — a chaque echantillon, et deux fois (gauche et droite). Avec une
// puissance de deux, le compilateur le remplace par un simple masque.
#define SON_ANNEAU  16384                   // ~0,5 s de reserve
#define SON_MASQUE  (SON_ANNEAU - 1)

static s16 g_gauche[SON_ANNEAU] __attribute__((aligned(32)));
static s16 g_droite[SON_ANNEAU] __attribute__((aligned(32)));
static s16 g_melange[SON_IMAGE * 2 * 2];    // rendu brut, stereo entrelace
static unsigned g_ecrit = 0;                // echantillons produits en tout
static unsigned g_livres = 0;               // pour la mesure, remis a zero
                                            // chaque seconde

// Produit `n` echantillons et les range dans l'anneau, en desentrelacant.
static void son_remplir(int n) {
  while (n > 0) {
    int bloc = n > SON_IMAGE * 2 ? SON_IMAGE * 2 : n;
    md_replayer_update((uint8_t *)g_melange, bloc * 4);
    for (int i = 0; i < bloc; i++) {
      unsigned pos = (g_ecrit + i) & SON_MASQUE;
      g_gauche[pos] = g_melange[i * 2 + 0];
      g_droite[pos] = g_melange[i * 2 + 1];
    }
    // La puce lit la memoire principale sans passer par le cache du processeur :
    // sans ce vidage, elle rejouerait ce qui s'y trouvait avant.
    unsigned deb = g_ecrit & SON_MASQUE;
    if (deb + bloc <= SON_ANNEAU) {
      DC_FlushRange(&g_gauche[deb], bloc * 2);
      DC_FlushRange(&g_droite[deb], bloc * 2);
    } else {
      DC_FlushRange(g_gauche, SON_ANNEAU * 2);
      DC_FlushRange(g_droite, SON_ANNEAU * 2);
    }
    g_ecrit += bloc; g_livres += bloc; n -= bloc;
  }
}
// Combien d'echantillons le moteur a REELLEMENT livres. Il en faut 32 768 par
// seconde ; tout ce qui manque, la puce le remplit en rejouant ce qu'elle a
// deja — d'ou l'echo et l'impression de ralenti.

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

// ── Edition, calquee sur LSDJModel de l'iPad ────────────────────────────────
// A pose une valeur ; un DOUBLE appui cree un element neuf ; A + croix modifie,
// pas de 1 a gauche/droite, grand pas en haut/bas (16, ou 12 demi-tons sur une
// note). C'est la convention de LSDJ, et celle du tracker iPad.

// Premier chain / phrase / instrument non utilise, pour le double appui.
static int chainLibre(void) {
  bool pris[MD_MAX_CHAINS]; for (int i = 0; i < MD_MAX_CHAINS; i++) pris[i] = false;
  for (int c = 0; c < 10; c++)
    for (int r = 0; r < MD_SONG_ROWS; r++) {
      uint8_t v = md_replayer_get_song(c, r);
      if (v != MD_EMPTY && v < MD_MAX_CHAINS) pris[v] = true;
    }
  for (int i = 0; i < MD_MAX_CHAINS; i++) if (!pris[i]) return i;
  return MD_MAX_CHAINS - 1;
}
static int phraseLibre(void) {
  static bool pris[MD_MAX_PHRASES];
  for (int i = 0; i < MD_MAX_PHRASES; i++) pris[i] = false;
  for (int ch = 0; ch < MD_MAX_CHAINS; ch++)
    for (int r = 0; r < MD_ROWS_PER_CHAIN; r++) {
      uint8_t ph; int8_t t; md_replayer_get_chain(ch, r, &ph, &t);
      if (ph != MD_EMPTY && ph < MD_MAX_PHRASES) pris[ph] = true;
    }
  for (int i = 0; i < MD_MAX_PHRASES; i++) if (!pris[i]) return i;
  return MD_MAX_PHRASES - 1;
}

static inline int borne(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

int main(void) {
  // ── Horloge : passer la DSi a 134 MHz ─────────────────────────────────
  // La DSi peut faire tourner son ARM9 deux fois plus vite qu'une DS, mais
  // PERSONNE ne l'enclenche : ni calico, ni libnds. Le registre existe, il
  // faut l'ecrire soi-meme. Sans ca, une cartouche marquee DSi tourne quand
  // meme a 67 MHz — c'est a cette vitesse-la qu'ont ete faites toutes les
  // mesures precedentes, et c'est ce qui les rendait trop pessimistes.
  //
  // En mode DS pur le registre n'existe pas : on ne l'ecrit que si la console
  // est bien en mode TWL, sinon on planterait.
  const bool modeDSi = systemIsTwlMode();
  if (modeDSi) REG_SCFG_CLK |= SCFG_CLK_CPU_134MHz;

  // ── Video ─────────────────────────────────────────────────────────────
  // Ecran du haut en bitmap 16 bits : on maitrise chaque pixel, ce qu'il faut
  // pour un rendu de tube. VRAM A lui suffit (256 x 192 x 2 = 96 Ko sur 128).
  powerOn(POWER_ALL_2D);
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
  md_replayer_init(SON_HZ);
  // SANS module, le moteur sort du silence sans rien calculer — ce qui
  // expliquait a la fois l'absence de son ET la charge processeur a zero.
  md_replayer_new_empty();

  // Le morceau de demonstration est EMBARQUE mais ne se charge pas tout seul :
  // on demarre sur un projet vide, et il s'ouvre depuis la page PROJECT.
  bool demoChargee = false;

  // ── Le son ────────────────────────────────────────────────────────────
  soundEnable();
  son_remplir(SON_ANNEAU);            // l'anneau plein avant de lancer
  soundPlaySample(g_gauche, SoundFormat_16Bit, SON_ANNEAU * 2, SON_HZ, 127, 0,   true, 0);
  soundPlaySample(g_droite, SoundFormat_16Bit, SON_ANNEAU * 2, SON_HZ, 127, 127, true, 0);

  // Pas de lecture automatique : c'est START qui la lance.

  static const char *noms[10] = {"FM1","FM2","FM3","FM4","FM5","PCM",
                                 "PS1","PS2","PS3","NOI"};

  // ── Mesure ────────────────────────────────────────────────────────────
  // La precedente mesurait un pourcentage et affichait toujours 000 : elle ne
  // valait rien. Celle-ci compte deux choses verifiables : combien de tours de
  // boucle passent REELLEMENT en une seconde (60 = on tient la cadence), et
  // quelle part de ce temps part dans le rendu audio.
  // Le timer 0 appartient a maxmod ; on prend le 2, libre, a 32,7 kHz.
  timerStart(2, ClockDivider_1024, 0, NULL);
  const unsigned kTicksParSeconde = 32727;   // 33,51 MHz / 1024
  unsigned tPrec = timerTick(2), cumul = 0, cumulAudio = 0, tours = 0;
  int fpsVu = 0, partAudio = 0;
  unsigned livresVu = 0, ecretesVu = 0;
  unsigned horloge = 0, tPrecSon = timerTick(2);

  int curCanal = 0, curLigne = 0, haut = 0;
  // Carte des pages, comme sur l'iPad :
  //            PROJECT
  //   SONG   CHAIN   PHRASE   INSTR
  // SELECT + haut monte a PROJECT, SELECT + bas redescend. Les pages CHAIN,
  // PHRASE et INSTR n'existent pas encore sur DS.
  enum { PAGE_SONG = 0, PAGE_CHAIN = 1, PAGE_PHRASE = 2,
         PAGE_INSTR = 3, PAGE_PROJECT = 4 };
  int page = PAGE_SONG, pageVue = -1;
  // Chaque page a son propre curseur, comme sur l'iPad. Le chain montre est
  // celui pointe dans SONG ; la phrase montree est celle pointee dans CHAIN.
  int dernierChain = 0, dernierePhrase = 0, derniereNote = 48;  // C-4
  int derA = -1; unsigned derAt = 0;   // pour detecter le double appui
  int chLigne = 0, chCol = 0;      // CHAIN : 16 lignes, 2 colonnes (phrase, tsp)
  int phLigne = 0, phCol = 0;      // PHRASE : 16 lignes, 6 colonnes
  int inLigne = 0, inCol = 0;      // INSTR : parametres en lignes, 4 operateurs
  bool enLecture = false;
  int repereVu[10]; for (int i2 = 0; i2 < 10; i2++) repereVu[i2] = -1;
  const int kLignesVues = 26;

  // On ne SORT PAS de main() sur une console : il n'y a nulle part ou revenir,
  // et la cartouche plante. pmMainLoop() gere l'ouverture du clapet et
  // l'extinction ; on tourne dedans jusqu'a ce qu'elle dise stop.
  while (pmMainLoop()) {
    unsigned tA = timerTick(2);
    // Attendre la ligne 0 puis remplir, comme le fait l'exemple officiel :
    // c'est ce rendez-vous regulier qui donne a maxmod sa notion du temps.
    // On produit ce que la puce a REELLEMENT consomme depuis le dernier tour.
    //
    // Ni une quantite fixe par tour — la boucle ne tient pas 60 images par
    // seconde, produire 548 echantillons en prend deja 11 ms, donc elle tourne
    // a 30 et il en faut 1096 — ni un rattrapage sans borne, qui s'auto-
    // entretient. Le temps ecoule est la seule reference juste.
    //
    // Le moteur n'a jamais ete en cause : mesure, il produit 51 137
    // echantillons par seconde pour 32 768 necessaires.
    unsigned tAv = timerTick(2);
    unsigned tSon = tAv;
    horloge += (unsigned short)(tSon - tPrecSon);
    tPrecSon = tSon;
    unsigned cible = (unsigned)((unsigned long long)horloge * SON_HZ / kTicksParSeconde)
                     + SON_IMAGE * 12;         // ~200 ms d'avance
    // Douze images d'avance, et non quatre.
    //
    // Quand on ecrit trop tard, la puce ne se tait pas : elle REJOUE l'anneau,
    // ce qui s'entend comme un disque raye. Quatre images (67 ms) ne
    // suffisaient pas a absorber un tour de boucle un peu long. Douze donnent
    // 200 ms de reserve, pour 615 ms d'anneau — il reste donc de la place.
    //
    // Le prix : 200 ms de latence. Perceptible en edition, acceptable en
    // lecture. A rediscuter quand le moteur aura de la marge.
    // On ne remplit QUE par paquets d'au moins un quart d'image.
    //
    // Sans ce seuil, la boucle appelait le moteur tres souvent avec quelques
    // echantillons a la fois, et le cout fixe de chaque appel — mise en place
    // du rendu, du reechantillonneur, du lot FM — finissait par dominer le
    // travail utile. Symptome : baisser la cadence de la puce ne liberait
    // AUCUN temps, la part passee a produire restait collee a 99 %.
    unsigned seuil = SON_IMAGE / 4;
    if (cible > g_ecrit && cible - g_ecrit >= seuil) {
      unsigned manque = cible - g_ecrit;
      // Rattraper au-dela d'un anneau n'a aucun sens : on reecrirait du son
      // deja joue. Dans ce cas on se resynchronise, quitte a sauter.
      if (manque > SON_ANNEAU) {
        g_ecrit = cible - SON_IMAGE * 4;
        manque = SON_IMAGE * 4;
      }
      son_remplir((int)manque);
    }
    cumulAudio += (unsigned short)(timerTick(2) - tAv);
    // cumulAudio est deja accumule plus haut, autour du seul remplissage.
    // Il l'etait AUSSI ici, depuis tA qui est en tete de boucle — donc sur le
    // tour entier. TPS restait colle a 99 % quoi qu'on fasse et ne mesurait
    // rien. Ici on ne compte plus que la duree totale du tour.
    unsigned tB = timerTick(2);
    cumul += (unsigned short)(tB - tPrec);   // soustraction 16 bits : le
    tPrec = tB;                              // bouclage du compteur est gere
    tours++;
    // Moyenne sur HUIT secondes, pas une.
    //
    // Le morceau n'occupe pas le processeur de la meme facon d'un passage a
    // l'autre : selon le nombre de voies qui sonnent, la mesure sur une seconde
    // varie de plus ou moins 3 %. Toute optimisation gagnant moins que ca etait
    // donc indiscernable du bruit — j'ai failli en juger une comme une
    // regression sur ce seul motif.
    if (cumul >= kTicksParSeconde * 8) {
      fpsVu = (int)(tours / 8);
      partAudio = (int)((unsigned long long)cumulAudio * 100 / cumul);
      livresVu = g_livres / 8; g_livres = 0;
      ecretesVu = md_chip_ecretes_et_remet_a_zero() / 8;
      cumul = 0; cumulAudio = 0; tours = 0;
    }

    scanKeys();
    const int appui = keysDownRepeat();
    const int frappe = keysDown();
    const bool selTenu = (keysHeld() & KEY_SELECT) != 0;

    if (selTenu) {
      // SELECT + croix : on change de PAGE.
      // Rangee du bas : SONG - CHAIN - PHRASE. SELECT + haut monte a PROJECT,
      // SELECT + bas en redescend.
      static int derniereRangee = PAGE_SONG;
      if (frappe & KEY_UP)   { if (page != PAGE_PROJECT) derniereRangee = page;
                               page = PAGE_PROJECT; }
      if (frappe & KEY_DOWN) { if (page == PAGE_PROJECT) page = derniereRangee; }
      if (page != PAGE_PROJECT) {
        if ((frappe & KEY_RIGHT) && page < PAGE_INSTR) page++;
        if ((frappe & KEY_LEFT)  && page > PAGE_SONG)   page--;
      }
    } else {
      const bool aTenu = (keysHeld() & KEY_A) != 0;

      // ── A + croix : modifier la valeur sous le curseur ─────────────────
      // Gauche/droite = pas de 1, haut/bas = grand pas. Comme sur l'iPad.
      if (aTenu && (appui & (KEY_UP|KEY_DOWN|KEY_LEFT|KEY_RIGHT))) {
        const int sens = (appui & (KEY_UP|KEY_RIGHT)) ? 1 : -1;
        const bool grand = (appui & (KEY_UP|KEY_DOWN)) != 0;
        if (page == PAGE_SONG) {
          uint8_t v = md_replayer_get_song(curCanal, curLigne);
          if (v != MD_EMPTY) {
            int n = borne((int)v + sens * (grand ? 16 : 1), 0, MD_MAX_CHAINS - 1);
            md_replayer_set_song(curCanal, curLigne, (uint8_t)n); dernierChain = n;
          } else if (sens > 0) md_replayer_set_song(curCanal, curLigne, 0);
        } else if (page == PAGE_CHAIN) {
          uint8_t noChain = md_replayer_get_song(curCanal, curLigne);
          if (noChain != MD_EMPTY) {
            uint8_t ph; int8_t tsp; md_replayer_get_chain(noChain, chLigne, &ph, &tsp);
            if (chCol == 0) {
              int n = (ph == MD_EMPTY ? -1 : (int)ph) + sens * (grand ? 16 : 1);
              if (n < 0) md_replayer_set_chain(noChain, chLigne, MD_EMPTY, tsp);
              else { n = borne(n, 0, MD_MAX_PHRASES - 1);
                     md_replayer_set_chain(noChain, chLigne, (uint8_t)n, tsp);
                     dernierePhrase = n; }
            } else {
              int t = borne((int)tsp + sens * (grand ? 12 : 1), -128, 127);
              md_replayer_set_chain(noChain, chLigne, ph, (int8_t)t);
            }
          }
        } else if (page == PAGE_INSTR) {
          int ins = 1;
          uint8_t noChain = md_replayer_get_song(curCanal, curLigne);
          uint8_t ph = MD_EMPTY; int8_t tz = 0;
          if (noChain != MD_EMPTY) md_replayer_get_chain(noChain, chLigne, &ph, &tz);
          if (ph != MD_EMPTY) {
            uint8_t no,i2,vel,cmd,cv,mc,mv;
            md_replayer_get_phrase(ph, phLigne, &no,&i2,&vel,&cmd,&cv,&mc,&mv);
            if (i2) ins = i2;
          }
          const int pas = sens * (grand ? 16 : 1);
          if (inLigne < 2) {
            static const int glob[2] = { MD_GEN_PROP_ALGORITHM, MD_GEN_PROP_FEEDBACK };
            int v = borne(md_replayer_get_instr_gen_val(ins, glob[inLigne]) + pas, 0, 7);
            md_replayer_set_instr_gen_val(ins, glob[inLigne], v);
          } else {
            static const int prop[9] = {
              MD_OP_PROP_MULTIPLE, MD_OP_PROP_DETUNE, MD_OP_PROP_TOTAL_LEVEL,
              MD_OP_PROP_ATTACK, MD_OP_PROP_DECAY, MD_OP_PROP_SUSTAIN_LEVEL,
              MD_OP_PROP_SUSTAIN_RATE, MD_OP_PROP_RELEASE, MD_OP_PROP_KEY_SCALE };
            static const int maxi[9] = { 15, 7, 127, 31, 31, 15, 31, 15, 3 };
            const int k = inLigne - 2;
            int v = borne(md_replayer_get_instr_op_val(ins, inCol, prop[k]) + pas,
                          0, maxi[k]);
            md_replayer_set_instr_op_val(ins, inCol, prop[k], v);
          }
        } else if (page == PAGE_PHRASE) {
          uint8_t noChain = md_replayer_get_song(curCanal, curLigne);
          uint8_t ph = MD_EMPTY; int8_t t0 = 0;
          if (noChain != MD_EMPTY) md_replayer_get_chain(noChain, chLigne, &ph, &t0);
          if (ph != MD_EMPTY) {
            uint8_t no,ins,vel,cmd,cv,mc,mv;
            md_replayer_get_phrase(ph, phLigne, &no,&ins,&vel,&cmd,&cv,&mc,&mv);
            switch (phCol) {
              case 0: { int n = (no && no != MD_EMPTY ? no : derniereNote)
                                + sens * (grand ? 12 : 1);
                        no = (uint8_t)borne(n, 1, MD_MAX_NOTE); derniereNote = no;
                        if (!ins) ins = 1; } break;
              case 1: ins = (uint8_t)borne((int)ins + sens * (grand ? 16 : 1), 1, 255); break;
              case 2: vel = (uint8_t)borne((int)(vel ? vel : 127) + sens * (grand ? 16 : 1), 0, 127); break;
              case 3: { int c2 = (cmd == MD_EMPTY ? -1 : (int)cmd) + sens;
                        cmd = (c2 < 0) ? MD_EMPTY : (uint8_t)c2; } break;
              case 4: cv = (uint8_t)borne((int)cv + sens * (grand ? 16 : 1), 0, 255); break;
              default: mv = (uint8_t)borne((int)mv + sens * (grand ? 16 : 1), 0, 255); break;
            }
            md_replayer_set_phrase(ph, phLigne, no,ins,vel,cmd,cv,mc,mv);
          }
        }
      } else if (page == PAGE_SONG) {
        if (appui & KEY_LEFT)  curCanal = (curCanal + 9) % 10;
        if (appui & KEY_RIGHT) curCanal = (curCanal + 1) % 10;
        if (appui & KEY_UP)    curLigne = (curLigne + MD_SONG_ROWS - 1) % MD_SONG_ROWS;
        if (appui & KEY_DOWN)  curLigne = (curLigne + 1) % MD_SONG_ROWS;
      } else if (page == PAGE_CHAIN) {
        if (appui & KEY_UP)    chLigne = (chLigne + MD_ROWS_PER_CHAIN - 1) % MD_ROWS_PER_CHAIN;
        if (appui & KEY_DOWN)  chLigne = (chLigne + 1) % MD_ROWS_PER_CHAIN;
        if (appui & KEY_LEFT)  chCol = (chCol + 1) % 2;
        if (appui & KEY_RIGHT) chCol = (chCol + 1) % 2;
      } else if (page == PAGE_PHRASE) {
        if (appui & KEY_UP)    phLigne = (phLigne + MD_ROWS_PER_PHRASE - 1) % MD_ROWS_PER_PHRASE;
        if (appui & KEY_DOWN)  phLigne = (phLigne + 1) % MD_ROWS_PER_PHRASE;
        if (appui & KEY_LEFT)  phCol = (phCol + 5) % 6;
        if (appui & KEY_RIGHT) phCol = (phCol + 1) % 6;
      } else if (page == PAGE_INSTR) {
        // 2 lignes globales (algorithme, retroaction) puis 9 par operateur.
        if (appui & KEY_UP)    inLigne = (inLigne + 10) % 11;
        if (appui & KEY_DOWN)  inLigne = (inLigne + 1) % 11;
        if (inLigne >= 2) {
          if (appui & KEY_LEFT)  inCol = (inCol + 3) % 4;
          if (appui & KEY_RIGHT) inCol = (inCol + 1) % 4;
        }
      } else if (page == PAGE_PROJECT) {
        // A : charger le morceau de demonstration.
        if ((frappe & KEY_A) && !demoChargee) {
          md_dmf_report_t rapport;
          demoChargee = md_replayer_import_dmf(morceau_dmf, morceau_dmf_len, &rapport);
          curLigne = 0; haut = 0; curCanal = 0;
        }
      }
      // ── A seul : poser une valeur ─────────────────────────────────────
      if ((frappe & KEY_A) && page != PAGE_PROJECT) {
        const int cle = page * 100000 + curCanal * 10000 + curLigne * 20
                        + chLigne + phLigne * 3 + phCol;
        const unsigned t = timerTick(2);
        const bool doubleA = (cle == derA) && ((unsigned short)(t - derAt) < 16000);
        derA = cle; derAt = t;

        if (page == PAGE_SONG) {
          uint8_t v = md_replayer_get_song(curCanal, curLigne);
          if (doubleA || v == MD_EMPTY) {
            int id = doubleA ? chainLibre() : dernierChain;
            md_replayer_set_song(curCanal, curLigne, (uint8_t)id); dernierChain = id;
          } else dernierChain = v;
        } else if (page == PAGE_CHAIN && chCol == 0) {
          uint8_t noChain = md_replayer_get_song(curCanal, curLigne);
          if (noChain != MD_EMPTY) {
            uint8_t ph; int8_t tsp; md_replayer_get_chain(noChain, chLigne, &ph, &tsp);
            if (doubleA || ph == MD_EMPTY) {
              int id = doubleA ? phraseLibre() : dernierePhrase;
              md_replayer_set_chain(noChain, chLigne, (uint8_t)id, tsp);
              dernierePhrase = id;
            } else dernierePhrase = ph;
          }
        } else if (page == PAGE_PHRASE) {
          uint8_t noChain = md_replayer_get_song(curCanal, curLigne);
          uint8_t ph = MD_EMPTY; int8_t t0 = 0;
          if (noChain != MD_EMPTY) md_replayer_get_chain(noChain, chLigne, &ph, &t0);
          if (ph != MD_EMPTY) {
            uint8_t no,ins,vel,cmd,cv,mc,mv;
            md_replayer_get_phrase(ph, phLigne, &no,&ins,&vel,&cmd,&cv,&mc,&mv);
            switch (phCol) {
              case 0: if (no && no != MD_EMPTY) derniereNote = no;
                      else { no = (uint8_t)derniereNote; if (!ins) ins = 1; } break;
              case 1: if (!ins) ins = 1; break;
              case 2: if (!vel) vel = 127; break;
              case 3: case 4: if (cmd == MD_EMPTY) cmd = 0; break;
              default: if (mc == MD_EMPTY) mc = 0; break;
            }
            md_replayer_set_phrase(ph, phLigne, no,ins,vel,cmd,cv,mc,mv);
          }
        }
      }

      // ── X : effacer ───────────────────────────────────────────────────
      // Comportement de LSDJ, repris de deleteCell sur l'iPad : sur une case
      // PLEINE on l'efface en laissant le trou ; sur une case VIDE on remonte
      // toute la colonne d'un cran, si bien qu'appuyer plusieurs fois au meme
      // endroit « aspire » la colonne vers le haut.
      if ((frappe & KEY_X) && page != PAGE_PROJECT) {
        if (page == PAGE_SONG) {
          if (md_replayer_get_song(curCanal, curLigne) == MD_EMPTY) {
            for (int r = curLigne; r < MD_SONG_ROWS - 1; r++)
              md_replayer_set_song(curCanal, r, md_replayer_get_song(curCanal, r + 1));
            md_replayer_set_song(curCanal, MD_SONG_ROWS - 1, MD_EMPTY);
          } else md_replayer_set_song(curCanal, curLigne, MD_EMPTY);
        } else if (page == PAGE_CHAIN) {
          uint8_t noChain = md_replayer_get_song(curCanal, curLigne);
          if (noChain != MD_EMPTY) {
            uint8_t ph; int8_t tsp; md_replayer_get_chain(noChain, chLigne, &ph, &tsp);
            const bool vide = (chCol == 0) ? (ph == MD_EMPTY) : (tsp == 0);
            const int dern = MD_ROWS_PER_CHAIN - 1;
            if (vide) {
              for (int r = chLigne; r < dern; r++) {
                uint8_t p1,p2; int8_t t1,t2;
                md_replayer_get_chain(noChain, r, &p1, &t1);
                md_replayer_get_chain(noChain, r + 1, &p2, &t2);
                if (chCol == 0) md_replayer_set_chain(noChain, r, p2, t1);
                else            md_replayer_set_chain(noChain, r, p1, t2);
              }
              uint8_t pl; int8_t tl; md_replayer_get_chain(noChain, dern, &pl, &tl);
              if (chCol == 0) md_replayer_set_chain(noChain, dern, MD_EMPTY, tl);
              else            md_replayer_set_chain(noChain, dern, pl, 0);
            } else if (chCol == 0) md_replayer_set_chain(noChain, chLigne, MD_EMPTY, tsp);
            else                   md_replayer_set_chain(noChain, chLigne, ph, 0);
          }
        } else if (page == PAGE_PHRASE) {
          uint8_t noChain = md_replayer_get_song(curCanal, curLigne);
          uint8_t ph = MD_EMPTY; int8_t t0 = 0;
          if (noChain != MD_EMPTY) md_replayer_get_chain(noChain, chLigne, &ph, &t0);
          if (ph != MD_EMPTY) {
            uint8_t no,ins,vel,cmd,cv,mc,mv;
            md_replayer_get_phrase(ph, phLigne, &no,&ins,&vel,&cmd,&cv,&mc,&mv);
            bool vide;
            switch (phCol) {
              case 0: vide = (no == 0); break;
              case 1: vide = (ins == 0); break;
              case 2: vide = (vel == 0); break;
              case 3: case 4: vide = (cmd == MD_EMPTY); break;
              default: vide = (mc == MD_EMPTY); break;
            }
            const int dern = MD_ROWS_PER_PHRASE - 1;
            if (vide) {
              for (int r = phLigne; r < dern; r++) {
                uint8_t a1,b1,c1,d1,e1,f1,g1, a2,b2,c2,d2,e2,f2,g2;
                md_replayer_get_phrase(ph, r,   &a1,&b1,&c1,&d1,&e1,&f1,&g1);
                md_replayer_get_phrase(ph, r+1, &a2,&b2,&c2,&d2,&e2,&f2,&g2);
                switch (phCol) {
                  case 0: a1 = a2; b1 = b2; break;   // la note emmene son instrument
                  case 1: b1 = b2; break;
                  case 2: c1 = c2; break;
                  case 3: case 4: d1 = d2; e1 = e2; break;
                  default: f1 = f2; g1 = g2; break;
                }
                md_replayer_set_phrase(ph, r, a1,b1,c1,d1,e1,f1,g1);
              }
              uint8_t a,b,c,d,e,f,g;
              md_replayer_get_phrase(ph, dern, &a,&b,&c,&d,&e,&f,&g);
              switch (phCol) {
                case 0: a = 0; b = 0; break;
                case 1: b = 0; break;
                case 2: c = 0; break;
                case 3: case 4: d = MD_EMPTY; e = 0; break;
                default: f = MD_EMPTY; g = 0; break;
              }
              md_replayer_set_phrase(ph, dern, a,b,c,d,e,f,g);
            } else {
              switch (phCol) {
                case 0: no = 0; break;
                case 1: ins = 0; break;
                case 2: vel = 0; break;
                case 3: case 4: cmd = MD_EMPTY; cv = 0; break;
                default: mc = MD_EMPTY; mv = 0; break;
              }
              md_replayer_set_phrase(ph, phLigne, no,ins,vel,cmd,cv,mc,mv);
            }
          }
        }
      }

      // START lance et arrete la lecture, depuis n'importe quelle page.
      if (frappe & KEY_START) {
        if (enLecture) { md_replayer_stop(); enLecture = false; }
        else {
          md_replayer_set_play_scope(MD_SCOPE_SONG, 0, 0);
          md_replayer_play_from(page == PAGE_SONG ? curLigne : 0);
          md_replayer_play();
          enLecture = true;
        }
      }
    }
    // La vue suit le curseur sans jamais le coller au bord.
    if (curLigne < haut + 2) haut = curLigne - 2;
    if (curLigne > haut + kLignesVues - 3) haut = curLigne - kLignesVues + 3;
    if (haut < 0) haut = 0;
    if (haut > MD_SONG_ROWS - kLignesVues) haut = MD_SONG_ROWS - kLignesVues;

    // ── Redessin ────────────────────────────────────────────────────────
    static int vuCanal = -1, vuLigne = -1, vuHaut = -1;

    if (page != pageVue) {
      // Changement de page : on repeint tout, en-tete compris.
      trame();
      texte(0, 0, "MD TRACKER DS", kEntete);
      texte(0, 1, modeDSi ? "DSI 134MHZ" : "DS 67MHZ",
            modeDSi ? rvb(10, 31, 14) : rvb(31, 20, 8));
      if (page == PAGE_SONG) {
        texte(52, 0, "SONG", kEntete);
        for (int c = 0; c < 10; c++) texte(4 + c * 6, 2, noms[c], kEntete);
      } else if (page == PAGE_CHAIN) {
        texte(51, 0, "CHAIN", kEntete);
        texte(4, 2, "PHRASE", kEntete);
        texte(14, 2, "TSP", kEntete);
      } else if (page == PAGE_INSTR) {
        texte(51, 0, "INSTR", kEntete);
        texte(2, 4, "ALGORITHME", kEntete);
        texte(2, 5, "RETROACTION", kEntete);
        texte(2, 7, "OP", kEntete);
        for (int o = 0; o < 4; o++) {
          char t[2] = { (char)('1'+o), 0 };
          texte(20 + o * 8, 7, t, kEntete);
        }
        static const char *par[9] = {"MUL","DET","TL ","AR ","D1R","D1L","D2R",
                                     "RR ","RS "};
        for (int k = 0; k < 9; k++) texte(2, 8 + k, par[k], kEntete);
      } else if (page == PAGE_PHRASE) {
        texte(50, 0, "PHRASE", kEntete);
        texte(4,  2, "NOTE", kEntete);
        texte(10, 2, "INS", kEntete);
        texte(15, 2, "VEL", kEntete);
        texte(20, 2, "CMD", kEntete);
        texte(28, 2, "MD CMD", kEntete);
      } else {
        texte(50, 0, "PROJECT", kEntete);
        texte(2, 5, "DEMO", kEntete);
        texte(9, 5, demoChargee ? "CHARGEE" : "A POUR CHARGER", kAttenue);
        texte(2, 7, "SELECT + BAS   RETOUR A SONG", kAttenue);
        texte(2, 8, "START          JOUER / ARRETER", kAttenue);
      }
      pageVue = page;
      vuCanal = -1; vuLigne = -1; vuHaut = -1;
      for (int c = 0; c < 10; c++) repereVu[c] = -1;
    }

    if (page == PAGE_PROJECT) {
      efface(9, 5, 14);
      texte(9, 5, demoChargee ? "CHARGEE" : "A POUR CHARGER", kAttenue);
    }

    if (page == PAGE_INSTR) {
      // L'instrument montre est celui de la ligne de phrase sous le curseur,
      // ou a defaut le premier. C'est la chaine de navigation de LSDJ.
      int ins = 1;
      {
        uint8_t noChain = md_replayer_get_song(curCanal, curLigne);
        uint8_t ph = MD_EMPTY; int8_t t0 = 0;
        if (noChain != MD_EMPTY) md_replayer_get_chain(noChain, chLigne, &ph, &t0);
        if (ph != MD_EMPTY) {
          uint8_t no,i2,vel,cmd,cv,mc,mv;
          md_replayer_get_phrase(ph, phLigne, &no,&i2,&vel,&cmd,&cv,&mc,&mv);
          if (i2) ins = i2;
        }
      }
      char t[8];
      efface(20, 0, 10);
      t[0]='I'; t[1]='N'; t[2]='S'; t[3]=' ';
      t[4]=kHex[(ins>>4)&15]; t[5]=kHex[ins&15]; t[6]=0;
      texte(20, 0, t, kEntete);

      // Les deux parametres globaux.
      static const int glob[2] = { MD_GEN_PROP_ALGORITHM, MD_GEN_PROP_FEEDBACK };
      for (int k = 0; k < 2; k++) {
        int v = md_replayer_get_instr_gen_val(ins, glob[k]);
        char d[3] = { kHex[(v>>4)&15], kHex[v&15], 0 };
        efface(20, 4 + k, 2);
        texte(20, 4 + k, d, (inLigne == k) ? kEntete : kAttenue);
      }

      // Les neuf parametres, pour chacun des quatre operateurs.
      static const int prop[9] = {
        MD_OP_PROP_MULTIPLE, MD_OP_PROP_DETUNE, MD_OP_PROP_TOTAL_LEVEL,
        MD_OP_PROP_ATTACK, MD_OP_PROP_DECAY, MD_OP_PROP_SUSTAIN_LEVEL,
        MD_OP_PROP_SUSTAIN_RATE, MD_OP_PROP_RELEASE, MD_OP_PROP_KEY_SCALE };
      for (int k = 0; k < 9; k++)
        for (int o = 0; o < 4; o++) {
          int v = md_replayer_get_instr_op_val(ins, o, prop[k]);
          char d[3] = { kHex[(v>>4)&15], kHex[v&15], 0 };
          efface(20 + o * 8, 8 + k, 2);
          texte(20 + o * 8, 8 + k, d,
                (inLigne == k + 2 && inCol == o) ? kEntete : kAttenue);
        }
    }

    if (page == PAGE_CHAIN || page == PAGE_PHRASE) {
      // ── Quel chain, quelle phrase ? ──────────────────────────────────
      // Le chain montre est celui pointe dans SONG ; la phrase montree est
      // celle pointee dans CHAIN. C'est la chaine de navigation de LSDJ.
      uint8_t noChain = md_replayer_get_song(curCanal, curLigne);
      uint8_t noPhrase = MD_EMPTY; int8_t tsp0 = 0;
      if (noChain != MD_EMPTY)
        md_replayer_get_chain(noChain, chLigne, &noPhrase, &tsp0);

      char t[8];
      efface(20, 0, 12);
      if (page == PAGE_CHAIN) {
        t[0]='C'; t[1]='H'; t[2]=' ';
        if (noChain == MD_EMPTY) { t[3]='-'; t[4]='-'; }
        else { t[3]=kHex[(noChain>>4)&15]; t[4]=kHex[noChain&15]; }
        t[5]=0; texte(20, 0, t, kEntete);
      } else {
        t[0]='P'; t[1]='H'; t[2]=' ';
        if (noPhrase == MD_EMPTY) { t[3]='-'; t[4]='-'; }
        else { t[3]=kHex[(noPhrase>>4)&15]; t[4]=kHex[noPhrase&15]; }
        t[5]=0; texte(20, 0, t, kEntete);
      }

      const int nl = (page == PAGE_CHAIN) ? MD_ROWS_PER_CHAIN : MD_ROWS_PER_PHRASE;
      for (int l = 0; l < nl; l++) {
        int lig = 4 + l;
        char num[3] = { kHex[(l>>4)&15], kHex[l&15], 0 };
        efface(0, lig, 2);
        texte(0, lig, num,
              (page == PAGE_CHAIN ? l == chLigne : l == phLigne) ? kEntete : kAttenue);

        if (page == PAGE_CHAIN) {
          uint8_t ph = MD_EMPTY; int8_t tsp = 0;
          if (noChain != MD_EMPTY) md_replayer_get_chain(noChain, l, &ph, &tsp);
          char a[3], b[3];
          if (ph == MD_EMPTY) { a[0]='-'; a[1]='-'; } else { a[0]=kHex[(ph>>4)&15]; a[1]=kHex[ph&15]; }
          a[2]=0;
          uint8_t u = (uint8_t)tsp;
          b[0]=kHex[(u>>4)&15]; b[1]=kHex[u&15]; b[2]=0;
          efface(5, lig, 2);  texte(5, lig, a, (l==chLigne && chCol==0) ? kEntete : kAttenue);
          efface(14, lig, 2); texte(14, lig, b, (l==chLigne && chCol==1) ? kEntete : kAttenue);
        } else {
          uint8_t no=0,ins=0,vel=0,cmd=MD_EMPTY,cv=0,mc=MD_EMPTY,mv=0;
          if (noPhrase != MD_EMPTY)
            md_replayer_get_phrase(noPhrase, l, &no,&ins,&vel,&cmd,&cv,&mc,&mv);
          static const char *gam[12] = {"C-","C#","D-","D#","E-","F-",
                                        "F#","G-","G#","A-","A#","B-"};
          char nt[4];
          if (no == 0) { nt[0]='-'; nt[1]='-'; nt[2]='-'; }
          else if (no == MD_EMPTY) { nt[0]='O'; nt[1]='F'; nt[2]='F'; }
          else { const char *g = gam[(no-1)%12]; nt[0]=g[0]; nt[1]=g[1];
                 nt[2]=(char)('0'+((no-1)/12)); }
          nt[3]=0;
          char si[3]={'-','-',0}, sv[3]={'-','-',0};
          if (ins) { si[0]=kHex[(ins>>4)&15]; si[1]=kHex[ins&15]; }
          if (vel) { sv[0]=kHex[(vel>>4)&15]; sv[1]=kHex[vel&15]; }
          char sc[4]={'-','-','-',0}, sm[5]={'-','-','-','-',0};
          if (cmd != MD_EMPTY) { sc[0]=(char)cmd; sc[1]=kHex[(cv>>4)&15]; sc[2]=kHex[cv&15]; }
          if (mc != MD_EMPTY) { sm[0]=kHex[(mc>>4)&15]; sm[1]=kHex[mc&15];
                                sm[2]=kHex[(mv>>4)&15]; sm[3]=kHex[mv&15]; }
          // Six positions de curseur pour cinq groupes visuels : la commande
          // se parcourt en deux temps (la lettre, puis la valeur), comme sur
          // l'iPad, et la colonne MD de meme.
          const int cols[5] = {4, 10, 15, 20, 28};
          const int larg[5] = {3, 2, 2, 3, 4};
          const char *txt[5] = {nt, si, sv, sc, sm};
          const int groupe[6] = {0, 1, 2, 3, 3, 4};
          for (int k = 0; k < 5; k++) {
            efface(cols[k], lig, larg[k]);
            texte(cols[k], lig, txt[k],
                  (l==phLigne && groupe[phCol]==k) ? kEntete : kAttenue);
          }
        }
      }
    }

    if (page == PAGE_SONG) {

    // Dessine une case : le fond, le curseur eventuel, puis la valeur.
    auto dessineCase = [&](int c, int ligne, bool ici) {
      int l = ligne - haut;
      if (l < 0 || l >= kLignesVues) return;
      uint8_t v = md_replayer_get_song(c, ligne);
      char cel[4];
      if (v == MD_EMPTY) { cel[0]='-'; cel[1]=' '; cel[2]='-'; }
      else { cel[0]=kHex[(v>>4)&15]; cel[1]=' '; cel[2]=kHex[v&15]; }
      cel[3] = 0;
      int col = 4 + c * 6;
      efface(col, 4 + l, 3);
      if (ici) {
        int x0 = col * MD_CELL_W, y0 = (4 + l) * MD_CELL_H;
        for (int y = y0; y < y0 + MD_FONT_HT; y++)
          for (int x = x0 - 1; x < x0 + 3 * MD_CELL_W; x++) pixel(x, y, kEntete);
      }
      texte(col, 4 + l, cel, ici ? kFond : kAttenue);
    };
    auto dessineNumero = [&](int ligne) {
      int l = ligne - haut;
      if (l < 0 || l >= kLignesVues) return;
      char num[3];
      num[0] = kHex[(ligne >> 4) & 15]; num[1] = kHex[ligne & 15]; num[2] = 0;
      efface(0, 4 + l, 2);
      texte(0, 4 + l, num, ligne == curLigne ? kEntete : kAttenue);
    };

    if (haut != vuHaut) {
      for (int l = 0; l < kLignesVues; l++) {
        dessineNumero(haut + l);
        for (int c = 0; c < 10; c++)
          dessineCase(c, haut + l, c == curCanal && haut + l == curLigne);
      }
      for (int c = 0; c < 10; c++) repereVu[c] = -1;
    } else if (curCanal != vuCanal || curLigne != vuLigne) {
      if (vuCanal >= 0) { dessineCase(vuCanal, vuLigne, false); dessineNumero(vuLigne); }
      dessineCase(curCanal, curLigne, true);
      dessineNumero(curLigne);
    }
    vuCanal = curCanal; vuLigne = curLigne; vuHaut = haut;

    // ── Les reperes de lecture ────────────────────────────────────────
    // Un chevron ROUGE devant la case que chaque canal est en train de jouer,
    // comme sur l'iPad. On ne redessine que ceux qui ont bouge.
    for (int c = 0; c < 10; c++) {
      int r = enLecture ? md_replayer_play_song_row(c) : -1;
      if (r == repereVu[c]) continue;
      if (repereVu[c] >= 0) {
        int l = repereVu[c] - haut;
        if (l >= 0 && l < kLignesVues) efface(3 + c * 6, 4 + l, 1);
      }
      if (r >= 0) {
        int l = r - haut;
        if (l >= 0 && l < kLignesVues) {
          efface(3 + c * 6, 4 + l, 1);
          texte(3 + c * 6, 4 + l, ">", rvb(31, 6, 6));
        }
      }
      repereVu[c] = r;
    }

    }  // fin de la page SONG

    swiWaitForVBlank();
  }
  return 0;
}
