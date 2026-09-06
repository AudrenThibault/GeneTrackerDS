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
#include <fat.h>
#include <dirent.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>

// Convertit n'importe quel WAV en 8 bits non signe a 32 kHz, la seule forme
// que le convertisseur du YM2612 accepte. Defini dans md_audio.cpp.
extern "C" uint8_t *md_audio_charge_wav(const char *chemin, uint32_t *len);
// La sortie de debogage no$gba : on ecrit le POINTEUR de la chaine en
// 0x04FFFA18 et l'emulateur la recopie sur sa sortie standard, avec un retour
// a la ligne. melonDS l'implemente ; sur une vraie console cette adresse n'est
// pas cablee et l'ecriture ne fait rien. C'est le seul moyen d'obtenir un
// journal COPIABLE quand on teste dans un emulateur, ou il n'y a pas de carte.
static inline void debogLigne(const char *m) {
  *(volatile uint32_t *)0x04FFFA18 = (uint32_t)m;
}
#include <stdio.h>
#include <string.h>

#include "md_font.h"

extern "C" {
#include "MegaDrive/md_chip.h"
}

extern "C" {
#include "CustomReplayer/md_replayer.h"
// Importer un projet depuis une ROM GeneTracker — voir md_rom_projet.h.
#include "MegaDrive/md_rom_projet.h"
#include "ROM/md_rom.h"
#include "ROM/md_vgm.h"
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
// ── La palette du CRT, reprise de LSDJPal cote iPad ────────────────────────
// Ce n'est pas une invention : ce sont les valeurs du tracker de reference,
// converties en cinq bits par composante. Le fond est noir, le texte un violet
// clair, et le repere de lecture un rouge franc.
//   fg      #C4AAFF a 71 %   accent #E94754
static const u16 kFond    = rvb(0, 0, 0);
// Les valeurs de l'iPad sont conservees en TEINTE mais remontees en clarte :
// l'ecran de la DS est bien moins lumineux qu'une dalle d'iPad, et un violet a
// 71 % y devient sourd — les lettres semblent baver faute de contraste avec le
// noir. On garde donc le meme violet, plus franc.
static const u16 kTexte   = rvb(29, 27, 31);
static const u16 kAttenue = rvb(23, 20, 30);
static const u16 kEntete  = rvb(29, 27, 31);
static const u16 kAccent  = rvb(28, 9, 10);

// Les numeros de ligne alternent par GROUPE DE QUATRE — une teinte de texte ET
// de fond par groupe, exactement comme LSDJ. C'est ce qui donne le rythme a
// l'oeil : sans ca, deux cent cinquante-six lignes identiques se ressemblent.
//   A  texte #71BED5 sur fond #8C00FF        B  texte #757DC2 sur fond presque noir
static const u16 kNumTexteA = rvb(14, 23, 26);
static const u16 kNumFondA  = rvb(17,  0, 31);
static const u16 kNumTexteB = rvb(16, 17, 24);
static const u16 kNumFondB  = rvb( 0,  1,  1);
// La selection : un jaune franc, qui ne se confond ni avec le vert du texte ni
// avec le rouge du chevron de lecture.
static const u16 kSelection = rvb(31, 29, 10);

// ── LES TROIS ROLES, et rien d'autre ───────────────────────────────────────
// Toute page passe par ces trois-la : un intitule, une valeur, le curseur.
// Ils existent pour qu'on ne puisse PAS habiller une page autrement qu'une
// autre — c'est arrive, et les pages PSG et PCM ne ressemblaient plus a la
// page FM. Changer l'apparence se fait ici, une fois, pour tout le tracker.
static const u16 kTitre = kEntete;    // les intitules fixes
static const u16 kData  = kAttenue;   // les valeurs qu'on modifie
// kAccent tient le troisieme role : ce que le curseur designe.

// L'ecran vise par les primitives. La police doublee ne laisse que 32
// colonnes : les canaux qui n'y tiennent pas vont sur l'ecran du bas.
static u16 *g_cible = nullptr;
// Decalage horizontal, en COLONNES. La grille du tracker s'en sert pour se
// centrer : les bandes noires se repartissent alors de part et d'autre au lieu
// de tout laisser a droite. En-tete, titre de page et fenetres le remettent a
// zero — eux se calent sur les bords.
static int g_colOrigine = 0;
// Decalage vertical, en POINTS. Il sert a poser les noms de colonnes entre
// deux lignes plutot que sur l'une d'elles : les espaces au-dessus et en
// dessous deviennent alors egaux, ce qu'un decalage d'une ligne entiere ne
// permet pas — il n'y a pas assez de lignes pour s'en offrir une de plus.
static int g_decalYpx = 0;
static inline void ecran(u16 *c) { g_cible = c; }

static void pixel(int x, int y, u16 c) {
  if ((unsigned)x < (unsigned)kEcranL && (unsigned)y < (unsigned)kEcranH)
    g_cible[y * kEcranL + x] = c;
}

// Assombrit tout l'ecran, pour qu'une fenetre posee dessus se detache et que
// l'on comprenne que la page est en attente. On divise chaque composante par
// deux, une seule fois a l'ouverture — le refaire a chaque image finirait par
// tout noircir.
static void assombrit() {
  for (int i = 0; i < kEcranL * kEcranH; i++) {
    const u16 c = g_cible[i];
    const unsigned r = (c & 31) >> 1;
    const unsigned v = ((c >> 5) & 31) >> 1;
    const unsigned b = ((c >> 10) & 31) >> 1;
    g_cible[i] = (u16)(0x8000 | (b << 10) | (v << 5) | r);
  }
}

// Une FENETRE : un panneau plein, borde, pose par-dessus la page. Le contenu
// s'ecrit ensuite dedans. Sans le fond plein, le texte se melait a ce qui
// restait affiche derriere et rien n'etait lisible.
static void fenetre(int col, int lig, int larg, int haut) {
  const int x0 = col * MD_CELL_W - 2, y0 = lig * MD_CELL_H - 2;
  const int x1 = x0 + larg * MD_CELL_W + 4, y1 = y0 + haut * MD_CELL_H + 4;
  for (int y = y0; y < y1; y++)
    for (int x = x0; x < x1; x++) {
      const bool bord = (y == y0 || y == y1 - 1 || x == x0 || x == x1 - 1);
      pixel(x, y, bord ? kEntete : kFond);
    }
}

// Le repere de lecture. Ce n'etait qu'un caractere « > » de la police, donc
// aussi gros qu'un chiffre et trop voyant. On le dessine ici : un petit
// triangle de trois points de large, centre dans la hauteur de la cellule.
static void chevron(int col, int lig, u16 couleur) {
  col += g_colOrigine;
  static const uint8_t forme[5] = {0x4, 0x6, 0x7, 0x6, 0x4};  // 100 110 111 ...
  int x0 = col * MD_CELL_W + 1, y0 = lig * MD_CELL_H + 2;
  for (int r = 0; r < 5; r++)
    for (int k = 0; k < 3; k++)
      if (forme[r] & (1 << (2 - k))) pixel(x0 + k, y0 + r, couleur);
}

// Dessine un caractere a la position (colonne, ligne) en cellules.
static void car(int col, int lig, char c, u16 couleur, bool gros) {
  col += g_colOrigine;
  int code = (unsigned char)c;
  if (code < 32 || code > 95) code = 32;
  const uint8_t *gl = md_font[code - 32];
  int x0 = col * MD_CELL_W, y0 = lig * MD_CELL_H + g_decalYpx;
  // DEUX tailles cohabitent, et c'est voulu.
  //
  // Les intitules fixes — titres, noms de colonnes, libelles de reglages — se
  // dessinent AGRANDIS : ils gagnent en presence et l'agrandissement, qui
  // double certains traits, ne gene pas sur un mot qu'on ne fait que lire.
  //
  // Tout ce qui porte une VALEUR — les cases, les numeros de ligne, les
  // chiffres qu'on modifie — se dessine a la taille d'origine, 5x7 exacts.
  // C'est la ou la nettete compte : un chiffre agrandi devient trapu et deux
  // valeurs voisines se confondent.
  const int lg = gros ? MD_GLYPHE_W : MD_FONT_W;
  const int ht = gros ? MD_GLYPHE_H : MD_FONT_HT;
  // Centre verticalement dans la cellule quand le glyphe est plus petit.
  const int decY = (MD_GLYPHE_H - ht) / 2;
  for (int r = 0; r < ht; r++) {
    const int sr = (r * MD_FONT_HT) / ht;
    for (int k = 0; k < lg; k++) {
      const int sk = (k * MD_FONT_W) / lg;
      if (gl[sr] & (1 << (MD_FONT_W - 1 - sk)))
        pixel(x0 + k, y0 + decY + r, couleur);
    }
  }
}

// Le texte des VALEURS : police d'origine, nette.
static void texte(int col, int lig, const char *s, u16 couleur) {
  for (int i = 0; s[i]; i++) car(col + i, lig, s[i], couleur, false);
}
// Le texte des INTITULES fixes : police agrandie.
static void titre(int col, int lig, const char *s, u16 couleur) {
  for (int i = 0; s[i]; i++) car(col + i, lig, s[i], couleur, true);
}

// Dessine un numero de ligne sur deux chiffres, avec le FOND de son groupe.
// LSDJ colore les numeros par groupes de quatre lignes, en alternant deux
// teintes : c'est ce qui donne le rythme a l'oeil sur deux cent cinquante-six
// lignes. Le fond se peint avant le texte, sur la hauteur du glyphe.
static void numeroLigne(int col, int lig, int idx, bool curseur) {
  col += g_colOrigine;
  const bool fort = ((idx / 4) % 2) == 0;
  const u16 fond  = fort ? kNumFondA  : kNumFondB;
  const u16 encre = curseur ? kTexte : (fort ? kNumTexteA : kNumTexteB);
  const int x0 = col * MD_CELL_W - 1, y0 = lig * MD_CELL_H;
  for (int y = y0; y < y0 + MD_GLYPHE_H + 1; y++)
    for (int x = x0; x < x0 + 2 * MD_CELL_W + 1; x++)
      pixel(x, y, fond);
  char d[3] = { kHex[(idx >> 4) & 15], kHex[idx & 15], 0 };
  const int sauve = g_colOrigine; g_colOrigine = 0;
  texte(col, lig, d, encre);
  g_colOrigine = sauve;
}

// Efface une zone de texte avant de la reecrire.
static void efface(int col, int lig, int n) {
  col += g_colOrigine;
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
// Arrondi, et non tronque : la puce calcule sa cadence source en arrondissant
// (26 634 a MD_YM_DIVISEUR = 288), la sortie la tronquait (26 633). Pour un
// hertz d'ecart, le reechantillonneur tournait a chaque echantillon — deux
// multiplications 64 bits, des decalages et un reste a tenir — au lieu de se
// court-circuiter. Les deux cadences etant desormais identiques, il passe par
// son chemin direct.
#define SON_HZ ((MD_YM2612_CLOCK + MD_YM_DIVISEUR / 2) / MD_YM_DIVISEUR)

// ── La cadence REELLE de la puce, et non celle qu'on demande ───────────────
// libnds convertit une frequence en diviseur materiel ainsi :
//     #define SOUND_FREQ(n)  ((-0x1000000 / (n)))
// Il prend donc 16 777 216 pour horloge, alors qu'elle vaut 16 756 991. Le
// diviseur est un ENTIER, ce qui ajoute encore un arrondi.
//
// Pour notre cadence, le diviseur tombe a 630 et la puce joue a 26 598 Hz
// pendant qu'on produisait pour 26 634 : trente-six echantillons d'ecart par
// seconde. L'ecriture prend donc de l'avance sur la lecture, et au bout de
// sept minutes et demie elle la rattrape et ecrase du son pas encore joue.
// C'est exactement ca, le son qui se degrade « au bout d'un moment » avec un
// retard qui s'accumule.
//
// On calcule donc la cadence que la puce aura VRAIMENT, et c'est elle qui
// sert de reference partout : au moteur comme a l'horloge de remplissage.
#define SON_HORLOGE   16756991
#define SON_DIVISEUR  (0x1000000 / SON_HZ)
#define SON_HZ_REEL   (SON_HORLOGE / SON_DIVISEUR)
#define SON_IMAGE   (SON_HZ_REEL / 60)     // echantillons par image
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
    // Quatre images par appel au moteur, et non deux : chaque appel de
    // md_replayer_update porte un cout fixe — mise en place du rendu, du
    // reechantillonneur, du lot FM — qui ne depend pas du nombre
    // d'echantillons demandes. En doublant la taille du bloc on divise ce
    // cout par deux, sans jamais bloquer longtemps.
    int bloc = n > SON_IMAGE * 4 ? SON_IMAGE * 4 : n;
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
      // Le bloc chevauche la fin de l'anneau : DEUX morceaux, la fin puis le
      // debut. On vidait ici les 64 Ko entiers des deux tampons — dix-huit
      // fois plus que necessaire. Ca tombait deux ou trois fois par seconde et
      // c'etait la pointe a 41 ms qui mettait la production en retard : le
      // reste du temps un bloc coutait le sixieme de ca.
      unsigned fin = SON_ANNEAU - deb;
      DC_FlushRange(&g_gauche[deb], fin * 2);
      DC_FlushRange(&g_droite[deb], fin * 2);
      DC_FlushRange(g_gauche, (bloc - fin) * 2);
      DC_FlushRange(g_droite, (bloc - fin) * 2);
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
// ⚠️ VIDE **ET** NON REFERENCE D'ABORD. Ne regarder que les references
// rendait une chaine qui porte deja des phrases mais que le SONG ne designe
// pas — on croyait en creer une neuve et on retombait sur un brouillon.
// C'est la meme regle que la version du clonage profond, plus bas.
// ── CE QUE FAIT CHAQUE COMMANDE, EN TOUTES LETTRES ───────────────────────
// Une lettre seule ne se retient pas : « U » ne dit pas « fine tune ». Les
// libelles viennent de md_table_cmds (md_replayer.c), qui associe chaque
// lettre a son effet — ce n'est donc pas une interpretation. L'ORDRE EST
// CELUI DE CETTE TABLE, et il ne doit pas en diverger.
static const char *kNomCmd[] = {
  "TABLE", "ARPEGGIO", "NOTE DELAY", "HOP", "NOTE CUT", "TONE PORTAMENTO",
  "GLOBAL VOLUME", "PANNING", "PITCH BEND", "RETRIG NOTE", "TEMPO",
  "VIBRATO", "TREMOLO", "VOLUME SLIDE", "PORTA + VOL SLIDE",
  "VIBRATO + VOL SLIDE", "SPEED", "POSITION JUMP", "PATTERN BREAK",
  "VIBRATO DEPTH", "FINE TUNE"
};
// Les commandes MD, par leur code DefleMask, dans l'ordre de md_mdcmds.
static const char *kNomMdCmd[] = {
  "ARPEGGIO", "PORTA UP", "PORTA DOWN", "TONE PORTA", "VIBRATO",
  "PORTA + VOL", "VIBRATO + VOL", "TREMOLO", "PANNING", "SET SPEED 1",
  "VOLUME SLIDE", "POSITION JUMP", "RETRIG", "PATTERN BREAK", "SET SPEED 2",
  "VIBRATO DEPTH", "FINE TUNE", "NOTE CUT", "NOTE DELAY",
  "LFO", "FEEDBACK", "LEVEL OP1", "LEVEL OP2", "LEVEL OP3",
  "LEVEL OP4", "MULTIPLIER",
  "ATTACK ALL", "ATTACK OP1", "ATTACK OP2", "ATTACK OP3"
};

static int chainLibre(void) {
  bool pris[MD_MAX_CHAINS]; for (int i = 0; i < MD_MAX_CHAINS; i++) pris[i] = false;
  for (int c = 0; c < 10; c++)
    for (int r = 0; r < MD_SONG_ROWS; r++) {
      uint8_t v = md_replayer_get_song(c, r);
      if (v != MD_EMPTY && v < MD_MAX_CHAINS) pris[v] = true;
    }
  for (int i = 0; i < MD_MAX_CHAINS; i++) {
    if (pris[i]) continue;
    bool vide = true;
    for (int r = 0; r < MD_ROWS_PER_CHAIN && vide; r++) {
      uint8_t ph; int8_t tr; md_replayer_get_chain((uint8_t)i, r, &ph, &tr);
      if (ph != MD_EMPTY) vide = false;
    }
    if (vide) return i;
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
  // Vide ET non referencee d'abord — voir chainLibre ci-dessus.
  for (int i = 0; i < MD_MAX_PHRASES; i++) {
    if (pris[i]) continue;
    bool vide = true;
    for (int r = 0; r < MD_ROWS_PER_PHRASE && vide; r++) {
      uint8_t no,i2,ve,cm,cv,mc,mv;
      md_replayer_get_phrase((uint8_t)i, r, &no,&i2,&ve,&cm,&cv,&mc,&mv);
      // La velocite VIDE vaut MD_EMPTY, pas zero : zero est un volume nul.
      if (no || i2 || ve != MD_EMPTY || cm != MD_EMPTY || mc != MD_EMPTY)
        vide = false;
    }
    if (vide) return i;
  }
  for (int i = 0; i < MD_MAX_PHRASES; i++) if (!pris[i]) return i;
  return MD_MAX_PHRASES - 1;
}

static inline int borne(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// La SORTIE est rangee dans un ordre qui n'est pas celui de l'affichage :
// 0 au centre, 1 a gauche, 2 a droite. Ajouter 1 a la valeur brute envoyait
// donc le centre vers la GAUCHE, et sautait le centre au retour ; a fond a
// gauche on n'atteignait jamais L. On se deplace sur le RANG VISIBLE — L C R —
// et on s'arrete aux deux bouts.
static const int kPanRang[3] = { 1, 0, 2 };   // rang visible -> valeur rangee
static int panDeplace(int pan, int sens) {
  int rang = 1;                                // le centre, par defaut
  for (int i = 0; i < 3; i++) if (kPanRang[i] == pan) rang = i;
  return kPanRang[borne(rang + sens, 0, 2)];
}

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
  const bool modeDSi = isDSiMode();
  if (modeDSi) setCpuClock(true);

  // ── Video ─────────────────────────────────────────────────────────────
  // Ecran du haut en bitmap 16 bits : on maitrise chaque pixel, ce qu'il faut
  // pour un rendu de tube. VRAM A lui suffit (256 x 192 x 2 = 96 Ko sur 128).
  powerOn(POWER_ALL_2D);
  videoSetMode(MODE_5_2D);
  vramSetBankA(VRAM_A_MAIN_BG);
  int bg = bgInit(2, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
  g_fond = bgGetGfxPtr(bg);
  ecran(g_fond);

  // Ecran du bas : vide, comme demande.
  videoSetModeSub(MODE_5_2D);
  vramSetBankC(VRAM_C_SUB_BG);
  int bgSub = bgInitSub(2, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
  u16 *bas = bgGetGfxPtr(bgSub);

  // La police doublee ne laisse que 32 colonnes. L'ecran du bas n'est plus un
  // fond noir : il porte les voies qui ne tiennent plus en haut, et la mesure.
  ecran(bas);  trame();
  ecran(g_fond); trame();

  // ── Le moteur, celui de l'iPad, compile pour ARM ──────────────────────
  // Le moteur travaille lui aussi a la cadence REELLE : sinon le tempo serait
  // juste a un millieme pres, mais surtout la production et la consommation
  // divergeraient a nouveau.
  md_replayer_init(SON_HZ_REEL);
  // SANS module, le moteur sort du silence sans rien calculer — ce qui
  // expliquait a la fois l'absence de son ET la charge processeur a zero.
  md_replayer_new_empty();
  // Un projet neuf sortait a 312 BPM : md_replayer_new_empty() pose tempo=125,
  // speed=6, 4 lignes par temps, ce qui fait 125*60/(6*4) = 312,5 BPM reels.
  // Le 125 est une frequence d'interruption AT2, pas un BPM. L'iPad ne s'en
  // apercevait pas parce que sa page de reglages ecrit 125 BPM en s'ouvrant ;
  // ici rien ne le faisait, et la lecture partait au triple de la vitesse.
  md_replayer_set_bpm(125.0);

  // Le morceau de demonstration est EMBARQUE mais ne se charge pas tout seul :
  // on demarre sur un projet vide, et il s'ouvre depuis la page PROJECT.

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
  // Le timer 2, a 32,7 kHz. Les timers 0 et 1 sont laisses libres.
  timerStart(2, ClockDivider_1024, 0, NULL);
  const unsigned kTicksParSeconde = 32727;   // 33,51 MHz / 1024
  unsigned tPrec = timerTick(2), cumul = 0, cumulAudio = 0, tours = 0;
  int fpsVu = 0, partAudio = 0;
  unsigned livresVu = 0, ecretesVu = 0;
  // Les deux facons de rater la cadence, comptees separement : « retard »
  // quand on ecrit trop tard et que la puce rejoue l'anneau (le disque raye),
  // « saut » quand le retard depasse un tour et qu'on se resynchronise.
  unsigned retards = 0, sauts = 0, retardsVu = 0, sautsVu = 0;
  unsigned margeMin = 0xFFFFFFFF, margeVue = 0;
  // ── L'avance de l'anneau, en images, et elle s'ajuste toute seule ────────
  //
  // Une valeur fixe obligeait a choisir pour TOUTES les machines : six images
  // (100 ms) suffisent a une DSi a 134 MHz mais pas a une DS d'origine a
  // 67 MHz, ou l'anneau se vide et la puce rejoue ce qu'elle vient de jouer ;
  // dix-huit images tiennent partout mais font entendre le son un tiers de
  // seconde apres l'affichage, ce qui est penible en edition.
  //
  // On part donc au plus court et on rallonge quand la machine n'y arrive
  // pas, puis on raccourcit quand elle tient. Une DSi restera a six, une DS
  // trouvera son palier — sans que personne ait de reglage a toucher.
  unsigned avance = 6;
  // Ou la puce en est, d'apres l'horloge. Sert a repartir de la au demarrage
  // plutot que d'attendre que l'anneau deja produit se vide.
  unsigned teteLecture = 0;

  // ── L'HISTOIRE des positions de lecture ─────────────────────────────────
  // Le repere rouge montrait ou le MOTEUR en est. Or le moteur calcule en
  // avance — jusqu'a un tiers de seconde — pour que l'anneau ne se vide
  // jamais. L'image etait donc en avance sur le son : la ligne 00 s'entendait
  // quand le repere passait sur la 03.
  //
  // On note donc, a chaque remplissage, QUELLE ligne chaque voie jouait et A
  // QUEL endroit de l'anneau. A l'affichage, on ressort la position qui
  // correspond a ce que la puce est en train de sortir. Le son ne change pas
  // d'un iota : c'est l'image qu'on remet a l'heure.
  struct PositionVue { unsigned pos; int16_t ligne[10]; };
  static PositionVue histo[64];
  int histoTete = 0, histoNb = 0;
  auto noteHistoire = [&]() {
    PositionVue &h = histo[histoTete];
    h.pos = g_ecrit;
    for (int c = 0; c < 10; c++)
      h.ligne[c] = (int16_t)md_replayer_play_song_row(c);
    histoTete = (histoTete + 1) % 64;
    if (histoNb < 64) histoNb++;
  };
  // La ligne qu'on ENTEND sur cette voie, et non celle qu'on calcule.
  auto ligneEntendue = [&](int c) {
    if (histoNb == 0) return md_replayer_play_song_row(c);
    int trouve = -1;
    for (int i = 0; i < histoNb; i++) {
      const PositionVue &h = histo[(histoTete - 1 - i + 64) % 64];
      if (h.pos <= teteLecture) { trouve = h.ligne[c]; break; }
    }
    // Rien d'assez ancien : l'histoire ne remonte pas assez loin, on prend
    // la plus vieille entree plutot que de mentir avec la plus recente.
    if (trouve == -1 && histoNb > 0)
      trouve = histo[(histoTete - histoNb + 64) % 64].ligne[c];
    return trouve;
  };
  const unsigned kAvanceMin = 6, kAvanceMax = 20;
  unsigned secondesPropres = 0;
  // Les DEUX pires durees de la seconde ecoulee, en ticks du timer 2 : celle
  // d'un tour de boucle entier, et celle du seul remplissage. Si le tour est
  // long mais le remplissage court, ce n'est pas le son qui bloque — et c'est
  // cet « autre chose » qu'il faut trouver.
  unsigned pireTour = 0, pireRemp = 0, pireTourVu = 0, pireRempVu = 0;
  // OU s'est produit le pire tour de boucle, et combien de remplissages audio
  // il a enchaines. Sans ca, P dit qu'un tour a dure une demi-seconde mais pas
  // sur quelle page ni a cause de quoi — on en est reduit aux hypotheses.
  char pirePage = '-', pirePageVu = '-';
  // Le tour le plus long ne disait pas ce qu'il avait FAIT. « PAGE J, 1909 ms »
  // laisse deviner, et deviner sur cette histoire-la a deja coute assez de
  // manches : chaque endroit capable de bloquer se nomme, et on note aussi si
  // la lecture etait en cours — un a-coup a l'arret ne s'entend pas.
  const char *causeTour = "-";
  const char *pireCause = "-";
  bool pireEnLecture = false;
  unsigned secondes = 0;   // horloge du journal, en secondes depuis l'allumage
  // Nombre de secondes pendant lesquelles on consigne l'etat de chaque voie
  // apres un depart : c'est la seule facon de voir si une colonne demarre.
  int journalVoies = 0;
  unsigned nbRemplis = 0, pireRemplis = 0, pireRemplisVu = 0;
  unsigned cumulDessin = 0; int partDessin = 0;
  // ── Les maxima, retenus jusqu'a remise a zero ────────────────────────────
  // La mesure par seconde etait illisible : le pic passait et le chiffre
  // redescendait avant qu'on ait eu le temps de le lire. On garde donc le
  // PLUS HAUT vu depuis le demarrage. La touche L les remet a zero — utile
  // pour ignorer le pic de demarrage, qui n'est que le premier remplissage
  // de l'anneau et ne gene rien.
  int aMax = 0, dMax = 0;
  unsigned rMax = 0, sMax = 0, pMax = 0, fMax = 0, vMax = 0, ecMax = 0;
  unsigned horloge = 0, tPrecSon = timerTick(2);

  // Maintenir une direction doit faire defiler vite, comme sur LSDJ. Sans
  // cet appel, libnds garde sa cadence par defaut et le curseur avance au
  // compte-gouttes. Les deux valeurs comptent en appels a scanKeys, c'est-a-
  // dire en images : environ un tiers de seconde avant que ca parte, puis
  // trente pas par seconde.
  keysSetRepeat(20, 2);

  int curCanal = 0, curLigne = 0, haut = 0;
  // Carte des pages, comme sur l'iPad :
  //            PROJECT
  //   SONG   CHAIN   PHRASE   INSTR
  // SELECT + haut monte a PROJECT, SELECT + bas redescend.
  enum { PAGE_SONG = 0, PAGE_CHAIN = 1, PAGE_PHRASE = 2,
         PAGE_INSTR = 3, PAGE_PROJECT = 4, PAGE_TABLE = 5,
         PAGE_APROPOS = 6 };
  // Curseur de la page TABLE : seize lignes, HUIT arrets.
  //   0 VOL   1 TSP
  //   2 lettre CMD 1   3 sa valeur
  //   4 lettre CMD 2   5 sa valeur
  //   6 code MD CMD    7 sa valeur
  // La lettre et sa valeur se reglent SEPAREMENT — c'est la meme regle que
  // dans une phrase : le curseur passe d'abord sur le premier caractere, puis
  // sur les deux suivants, et A + direction ne modifie que ce qui est sous lui.
  int tabLigne = 0, tabCol = 0;
  int repereTab[5] = { -2, -2, -2, -2, -2 };   // ou sont les chevrons
  int tableId = 0;                 // la table en cours d'edition
  int page = PAGE_SONG, pageVue = -1;
  // Chaque page a son propre curseur, comme sur l'iPad. Le chain montre est
  // celui pointe dans SONG ; la phrase montree est celle pointee dans CHAIN.
  int dernierChain = 0, dernierePhrase = 0;
  // La derniere note posee, RETENUE PAR CANAL. Elle etait unique pour tout le
  // morceau : en arrivant sur une nouvelle colonne, la premiere note heritait
  // de celle qu'on venait d'ecrire ailleurs — sur une basse apres un lead, on
  // se retrouvait trois octaves trop haut. Chaque voie part donc de C-4.
  //   la note 49 est le C de la quatrieme octave : (49-1) % 12 = 0, (49-1)/12 = 4
  int derniereNote[10];
  for (int i2 = 0; i2 < 10; i2++) derniereNote[i2] = 49;   // C-4 partout
  // ⚠️ LE PCM NE SE SOUVIENT DE RIEN, ET C'EST VOULU. Un echantillon joue a sa
  // vitesse d'enregistrement en C-4 : c'est la hauteur qu'on veut chaque fois
  // qu'on en pose un, pas celle du sample precedent, qui etait accordee pour
  // un autre son. On la change apres si on veut, mais on part toujours de la.
  auto noteDepart = [&](int voie) {
    return (voie == MD_PCM_CHANNEL) ? 49 : derniereNote[voie];
  };
  // L'instrument retenu pour CHAQUE voie. Sans lui, poser une note sur une
  // nouvelle colonne reprenait l'instrument 1, et deux voies se retrouvaient a
  // jouer exactement la meme chose — indiscernable d'une voie muette.
  int instrCanal[10]; for (int i2 = 0; i2 < 10; i2++) instrCanal[i2] = 0;
  int instrVoie = 0;    // voie d'ou l'on est descendu dans la page INSTR
  int voieCourante = 0; // voie d'ou l'on est descendu dans CHAIN puis PHRASE
  int voieAudition = -1; // voie sur laquelle sonne la note d'audition
  // Le sort du dernier chargement d'echantillon, affiche sur la page PCM : il
  // n'allait que dans le journal, invisible pendant qu'on travaille. Une
  // banque pleine passait donc pour un chargement qui « ne fait rien ».
  static char msgPCM[16] = "";
  // Un message passager en haut de l'ecran — confirmation d'enregistrement,
  // par exemple — avec la seconde a laquelle il doit disparaitre.
  static char msgProjet[40] = "";
  unsigned msgProjetJusqu = 0;

  // ── Le journal ──────────────────────────────────────────────────────────
  // Ecrit a cote du .nds, pour pouvoir etre releve apres coup au lieu de
  // guetter des chiffres a l'ecran. On accumule en memoire et on n'ecrit sur
  // la carte que par a-coups : ecrire pendant la lecture vide l'anneau audio,
  // ce qui creerait le defaut qu'on essaie de mesurer.
  static char journalTampon[8192];
  int journalLong = 0;
  unsigned journalPerdus = 0;   // lignes jetees, tampon plein pendant la lecture
  bool journalSale = false;
  static char journalChemin[192];
  {
    strcpy(journalChemin, "/GENETRACKER.LOG");
    // nds-bootstrap transmet le chemin de la cartouche ; on ecrit a cote.
    if (__system_argv->argvMagic == ARGV_MAGIC &&
        __system_argv->argc > 0 && __system_argv->argv[0]) {
      char tmp[192];
      strncpy(tmp, __system_argv->argv[0], sizeof(tmp) - 20);
      tmp[sizeof(tmp) - 20] = 0;
      char *barre = strrchr(tmp, '/');
      if (barre) { barre[1] = 0; strcat(tmp, "GENETRACKER.LOG");
                   strcpy(journalChemin, tmp); }
    }
  }

  // Le journal part AUSSI sur la sortie de debogage no$gba, que melonDS
  // recopie sur sa sortie standard. Sans carte SD — le cas dans un emulateur —
  // c'est le seul moyen d'obtenir un texte qu'on puisse copier.
  auto journal = [&](const char *ligne) {
    debogLigne(ligne);
    const int n = (int)strlen(ligne);
    // Tampon plein : on jette, on compte, et on le dira au prochain vidage.
    // Bloquer ici pour ecrire sur la carte tuerait le son (voir plus bas).
    if (journalLong + n + 2 >= (int)sizeof(journalTampon)) {
      journalPerdus++; return;
    }
    memcpy(journalTampon + journalLong, ligne, n);
    journalLong += n;
    journalTampon[journalLong++] = '\n';
    journalTampon[journalLong] = 0;
    journalSale = true;
  };

  // Sur DSi, le lecteur par defaut n'est pas forcement celui qu'on croit :
  // libfat monte la carte sous « sd: » et la cartouche sous « fat: », et le
  // chemin nu peut ne designer ni l'un ni l'autre. On essaie donc plusieurs
  // candidats une seule fois, et on retient celui qui repond.
  bool journalChoisi = false;
  auto videJournal = [&]() {
    if (!journalSale || journalLong == 0) return;
    if (!journalChoisi) {
      char aCote[192] = "";
      if (__system_argv->argvMagic == ARGV_MAGIC &&
          __system_argv->argc > 0 && __system_argv->argv[0]) {
        strncpy(aCote, __system_argv->argv[0], sizeof(aCote) - 20);
        aCote[sizeof(aCote) - 20] = 0;
        char *barre = strrchr(aCote, '/');
        if (barre) { barre[1] = 0; strcat(aCote, "GENETRACKER.LOG"); }
        else aCote[0] = 0;
      }
      const char *candidats[4] = { aCote, "sd:/GENETRACKER.LOG",
                                   "fat:/GENETRACKER.LOG", "/GENETRACKER.LOG" };
      for (int i = 0; i < 4 && !journalChoisi; i++) {
        if (!candidats[i][0]) continue;
        FILE *t = fopen(candidats[i], "a");
        if (!t) continue;
        fclose(t);
        strcpy(journalChemin, candidats[i]);
        journalChoisi = true;
      }
      if (!journalChoisi) return;
    }
    FILE *f = fopen(journalChemin, "a");
    if (!f) return;
    fwrite(journalTampon, 1, (size_t)journalLong, f);
    if (journalPerdus) {
      char pj[80];
      siprintf(pj, "  (%u LIGNES PERDUES : TAMPON PLEIN PENDANT LA LECTURE)\n",
               journalPerdus);
      fwrite(pj, 1, strlen(pj), f);
      journalPerdus = 0;
    }
    fclose(f);
    journalLong = 0; journalTampon[0] = 0; journalSale = false;
  };
  bool navigateur = false;
  static char fichiers[80][40];
  static uint8_t typeFic[80];        // 0 autre, 1 ouvrable, 2 dossier
  int nbFichiers = 0, selFichier = 0, hautFichier = 0;
  // Les morceaux vivent dans « Songs », a cote de la cartouche. On cree le
  // dossier s'il manque : c'est la que l'enregistrement ecrit ET ou le
  // navigateur s'ouvre, pour que les deux ne se cherchent jamais.
  static char dossier[128] = "/";
  static char dossierSongs[128] = "/", dossierSamples[128] = "/";
  static char dossierRoms[128] = "/";
  static char dossierVgm[128]  = "/";
  // Deux dossiers distincts, a cote de la cartouche : les morceaux et les
  // echantillons ne se cherchent pas au meme endroit, et le navigateur doit
  // s'ouvrir sur le bon selon ce qu'on est venu chercher.
  // Un dossier n'est retenu que s'il repond VRAIMENT. Le chemin transmis par
  // nds-bootstrap peut porter un prefixe de lecteur — « sd: », « fat: » — ou
  // n'en porter aucun, et tous ne designent pas forcement la meme chose. On
  // essaie donc les variantes et on garde celle qui s'ouvre, au lieu de
  // supposer : un dossier qui n'existe pas faisait retomber l'enregistrement
  // a la racine, sans rien dire.
  // Le chemin transmis par nds-bootstrap est utilise TEL QUEL. Le journal l'a
  // confirme : « sd:/roms/nds/MDTrackerNDS/... » fonctionne. J'avais ajoute une
  // seconde variante, sans le prefixe de lecteur, en supposant qu'il pourrait
  // ne pas convenir — cette exploration a l'aveugle enchainait des opendir et
  // des mkdir sur un chemin qui ne designe rien, et bloquait le demarrage sur
  // la vraie console. On ne devine plus : une seule tentative, celle qui marche.
  if (__system_argv->argvMagic == ARGV_MAGIC &&
      __system_argv->argc > 0 && __system_argv->argv[0]) {
    char base[128];
    strncpy(base, __system_argv->argv[0], sizeof(base) - 12);
    base[sizeof(base) - 12] = 0;
    char *barre = strrchr(base, '/');
    if (barre) {
      barre[1] = 0;
      siprintf(dossierSongs,   "%sSongs", base);
      siprintf(dossierSamples, "%sSamples", base);
      siprintf(dossierRoms,    "%sRoms", base);
      siprintf(dossierVgm,     "%sVGM", base);
      // ── AUCUNE operation sur la carte pendant le demarrage ──────────────
      // Mesure sur la console : la cartouche se figeait ici, sept reperes
      // affiches sur dix. D'abord avec mkdir() sur un dossier deja present,
      // puis avec opendir() a sa place — donc ce n'est pas l'une ou l'autre
      // fonction, c'est le fait de toucher au systeme de fichiers a cet
      // instant du demarrage. L'emulateur, sans vraie carte, ne le montrait
      // jamais.
      //
      // On se contente donc de CALCULER les chemins. Les dossiers seront
      // verifies et crees au moment ou l'on s'en sert vraiment — a
      // l'enregistrement, ou a l'ouverture du navigateur — quand la carte a
      // eu tout le temps de se mettre en route.
      strcpy(dossier, dossierSongs);
    }
  }

  // Cree un dossier s'il manque. Appele au PREMIER USAGE, jamais au
  // demarrage : voir plus haut pourquoi.
  auto creeSiAbsent = [](const char *d) {
    DIR *x = opendir(d);
    if (x) { closedir(x); return true; }
    return mkdir(d, 0777) == 0;
  };

  // ── Parcourir le DOSSIER des echantillons a la croix ────────────────────
  // Le moteur ne joue que ce qui est dans la BANQUE du morceau — c'est ce qui
  // rend un .mdm autonome et lisible sur l'iPad. On concilie les deux : la
  // croix parcourt les fichiers du dossier, et celui qu'on pointe entre en
  // banque au passage. S'il y est deja, on s'y rebranche au lieu d'en ajouter
  // une copie, sinon un aller-retour dans la liste la remplirait.
  //
  // Le dossier n'est lu qu'a la PREMIERE utilisation, jamais au demarrage :
  // c'est un acces au systeme de fichiers pendant le demarrage qui figeait la
  // cartouche, ecran noir, et il a fallu quatre essais pour l'etablir.
  static char echFic[64][40];
  int echNb = 0, echSel = -1;
  bool echScanne = false;
  auto scanneEch = [&]() {
    echScanne = true;
    echNb = 0;
    DIR *d = opendir(dossierSamples);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && echNb < 64) {
      const char *n = e->d_name;
      if (n[0] == '.') continue;              // metadonnees du systeme
      const int L = (int)strlen(n);
      if (L < 5 || strcasecmp(n + L - 4, ".wav")) continue;
      strncpy(echFic[echNb], n, 39);
      echFic[echNb][39] = 0;
      echNb++;
    }
    closedir(d);
  };

  // La page a laquelle revenir en quittant le navigateur : on repart d'ou l'on
  // venait, et non sur PROJECT — charger un echantillon depuis l'ecran
  // instrument y ramenait, ce qui n'a aucun sens.
  int pageRetour = PAGE_PROJECT;

  // ── Le parcours du dossier d'echantillons a ete RETIRE ─────────────────
  // Il faisait planter la cartouche AU DEMARRAGE sur la vraie console : le
  // journal ne portait plus aucune ligne des versions concernees, preuve que
  // le plantage precedait l'ecriture de son en-tete. Je n'ai pas trouve la
  // cause exacte ; l'emulateur, lui, demarrait normalement, faute de vraie
  // carte SD, et je n'avais donc aucun moyen de la reproduire ici.
  //
  // On revient au systeme qui fonctionnait : « LOAD SAMPLE » charge un fichier
  // dans la banque du morceau, et A + gauche/droite parcourt cette banque.
  // A reprendre plus tard, en verifiant a CHAQUE etape sur la console au lieu
  // d'ajouter plusieurs mecanismes d'un coup.

  // Les quatre visages de la page instrument, comme sur l'iPad.
  enum { GENRE_FM, GENRE_PSG, GENRE_BRUIT, GENRE_PCM };
  // La table attachee a un instrument : OFF, puis 00, 01, 02... Un seul
  // endroit la change, pour les quatre genres d'instrument.
  // Ouvrir la table d'un instrument. S'il n'en a pas, on lui attribue la
  // premiere LIBRE plutot que de montrer un ecran vide — c'est ce que fait
  // l'iPad.
  // La premiere table VIDE. C'est ce que double-A pose sur le champ TABLE,
  // exactement comme double-A pose un chain ou une phrase neufs ailleurs.
  auto tableLibre = []() {
    for (int i = 0; i < MD_MAX_TABLES; i++)
      if (md_replayer_table_is_empty(i)) return i;
    return 0;
  };
  int derniereTable = 0;        // la derniere table employee, rappelee par A
  // La derniere commande posee dans une table, rappelee par A sur une case
  // vide. Sans ce rappel il faut traverser toute la liste a chaque fois.
  uint8_t derCmd = 0, derCmdVal = 0, derMdVal = 0;
  uint8_t derMdCmd = (uint8_t)md_mdcmd_code(0);   // un code VALIDE, pas zero
  auto tableDeLInstrument = [](int ins) {
    int t = md_replayer_get_instr_table(ins);
    if (t < 0 || t == MD_EMPTY) {
      t = 0;
      for (int i = 0; i < MD_MAX_TABLES; i++)
        if (md_replayer_table_is_empty(i)) { t = i; break; }
      md_replayer_set_instr_table(ins, t);
    }
    return t;
  };
  auto changeTable = [](int ins, int sens) {
    const int t = md_replayer_get_instr_table(ins);
    const int actuel = (t < 0 || t == MD_EMPTY) ? -1 : t;
    const int n = actuel + sens;
    md_replayer_set_instr_table(ins,
        n < 0 ? -1 : (n >= MD_MAX_TABLES ? MD_MAX_TABLES - 1 : n));
  };
  // Son affichage : « OFF » quand il n'y en a pas.
  auto texteTable = [](int ins, char *d) {
    const int t = md_replayer_get_instr_table(ins);
    if (t < 0 || t == MD_EMPTY) { d[0]='O'; d[1]='F'; d[2]='F'; d[3]=0; }
    else { d[0]=kHex[(t>>4)&15]; d[1]=kHex[t&15]; d[2]=0; }
  };
  auto genreInstr = [&](int voie, int ins) {
    if (voie == 9) return GENRE_BRUIT;
    if (voie >= 6) return GENRE_PSG;
    // La 6e voie FM est double : echantillon par defaut, mais elle redevient
    // une voie FM quand son instrument repasse en synthese.
    if (voie == MD_PCM_CHANNEL &&
        md_replayer_get_instr_kind(ins) != MD_INSTR_KIND_FM) return GENRE_PCM;
    return GENRE_FM;
  };
  int derA = -1; unsigned derAt = 0;   // pour detecter le double appui
  // Numeros RETENUS au moment ou l'on descend, comme chainId / phraseId sur
  // l'iPad. On ne les recalcule pas depuis le curseur : sinon la page CHAIN
  // afficherait toujours le chain de la case actuellement pointee dans SONG.
  int chainId = 0, phraseId = 0, instrId = 1;
  int chLigne = 0, chCol = 0;      // CHAIN : 16 lignes, 2 colonnes (phrase, tsp)
  int phLigne = 0, phCol = 0;      // PHRASE : 16 lignes, 6 colonnes
  int inLigne = 0, inCol = 0;      // INSTR : parametres en lignes, 4 operateurs
  bool enLecture = false;
  int repereVu[10]; for (int i2 = 0; i2 < 10; i2++) repereVu[i2] = -1;
  int repereLigne = -1;   // CHAIN / PHRASE : une seule tete de lecture visible
  int bpmVu = -1;         // dernier BPM affiche, pour ne redessiner qu'au besoin
  // Seize lignes a l'ecran avec la police doublee ; l'en-tete en prend quatre.
  // Le nombre de lignes visibles se DEDUIT de la taille de l'ecran et de la
  // police, au lieu d'etre fixe : en changeant MD_ZOOM on changeait le nombre
  // de lignes tenant a l'ecran, et la derniere se retrouvait coupee — sur
  // PHRASE, la ligne 0F disparaissait purement et simplement.
  //   ligne 0 : nom du tracker et titre de page
  //   ligne 1 : noms des colonnes
  //   ligne 2 : respiration
  //   a partir de 3 : les donnees
  const int kLignesVues = kLignes - 3;
  // Les dix voies tiennent sur l'ecran du haut : deux colonnes pour le numero
  // de ligne, puis quatre par voie — une pour le chevron, trois pour la
  // valeur. Soit 2 + 40 = 42 colonnes, exactement la largeur disponible.
  // L'ecran du bas ne porte plus que la mesure.
  const int kColVoie = 3, kPasVoie = 4;

  // ── Les trois voies supplementaires ────────────────────────────────────
  // FM5, PSG2 et PSG3. Les calculer coute environ un sixieme du travail pour
  // la seule FM5 — quatre operateurs sur vingt-quatre — et c'est ce sixieme
  // qui fait la difference entre une lecture propre et une lecture qui saute.
  // Un projet neuf demarre donc sans elles.
  //
  // On les DESACTIVE, on ne les efface jamais du morceau : les notes restent
  // dans le fichier et le meme projet rouvert sur iPad sonne complet. La DS ne
  // mutile rien, elle refuse seulement de jouer.
  static const int kVoiesSupp[3] = {4, 7, 8};   // FM5, PS2, PS3
  bool voiesSupp = false;

  // curCanal designe une colonne AFFICHEE ; visibles[] donne la voie reelle.
  int visibles[10], nbVisibles = 0;
  auto majVisibles = [&]() {
    nbVisibles = 0;
    for (int c = 0; c < 10; c++)
      if (!md_replayer_is_channel_disabled(c)) visibles[nbVisibles++] = c;
    if (curCanal >= nbVisibles) curCanal = nbVisibles - 1;
  };
  auto appliqueVoiesSupp = [&](bool actives) {
    voiesSupp = actives;
    for (int k = 0; k < 3; k++)
      md_replayer_disable_channel(kVoiesSupp[k], !actives);
    majVisibles();
    pageVue = -1;   // force le repeint complet : le nombre de colonnes change
  };
  auto reel = [&](int i) { return visibles[borne(i, 0, nbVisibles - 1)]; };
  appliqueVoiesSupp(false);

  // Le morceau qu'on vient d'ouvrir se sert-il des voies supplementaires ?
  // On regarde les DONNEES, pas un reglage : un reglage enregistre dans le
  // fichier ne vaudrait que pour ce morceau-la.
  auto morceauUtiliseVoiesSupp = [&]() {
    for (int k = 0; k < 3; k++)
      for (int r = 0; r < MD_SONG_ROWS; r++)
        if (md_replayer_get_song(kVoiesSupp[k], r) != MD_EMPTY) return true;
    return false;
  };
  bool demandeVoies = false;
  // Creer un morceau neuf efface celui qui est en memoire, et rien n'est
  // encore enregistre sur la carte : on demande confirmation.
  bool demandeNouveau = false;
  // ⚠️ APRES UN IMPORT DE SAUVEGARDE, LES SONS MANQUENT ET IL FAUT LE DIRE.
  // La cartouche n'a que 32 Ko : ses echantillons vivent dans la ROM, jamais
  // dans la sauvegarde. Le morceau arrive donc complet avec une voie PCM
  // muette, et rien n'expliquait pourquoi. On pose la question tout de suite,
  // et on emmene directement au choix de la ROM.
  bool demandeEchantillons = false;
  int  manqueEchantillons = 0;
  // 0 = importer le projet d'une ROM, 1 = n'en prendre que les echantillons.
  int  romPour = 0;
  int menuProjet = 0;   // ligne pointee dans le menu de la page PROJECT
  // La console visee par l'export. Une Mega Drive PAL affiche 49,70 images par
  // seconde et une NTSC 59,92 : le lecteur de la cartouche avance d'une image
  // de journal par retour vertical, donc c'est CETTE cadence qui fixe le tempo.
  // Un journal NTSC joue sur une PAL traine de 20 %, ce qui s'entend enormement.
  // On part sur PAL : c'est la machine vendue en Europe.
  bool romPal = true;

  // ── Ouvrir un projet depuis la carte SD ─────────────────────────────────
  // Le format est le .mdm du moteur, exactement celui qu'ecrit l'iPad : c'est
  // le contrat entre les deux projets, un morceau fait la-bas doit s'ouvrir
  // ici. Les .dmf de DefleMask sont acceptes aussi, par le meme importeur que
  // la demo.
  const bool carteOK = fatInitDefault();



  // On montre TOUT ce que contient le dossier, pas seulement les morceaux :
  // sans les autres fichiers on ne sait pas ou l'on se trouve sur la carte.
  // Les morceaux ouvrables sont en clair, le reste en attenue.
  // Le navigateur sert a deux choses : ouvrir un MORCEAU, ou charger un
  // ECHANTILLON. Ce qu'il met en avant depend de ce qu'on est venu chercher.
  // Le navigateur sert a TROIS choses maintenant : ouvrir un morceau, charger
  // un echantillon, ou choisir une ROM Mega Drive a importer.
  int navGenre = 0;   // 0 = morceaux, 1 = echantillons, 2 = ROMs,
                      // 3 = les morceaux d'une SAUVEGARDE de cartouche
  // ⚠️ Une sauvegarde tient plusieurs morceaux, et le bon n'est presque jamais
  // le premier — sur la cartouche d'essai il est au rang 1. Alors au lieu
  // d'en choisir un a l'aveugle, on RECHARGE la liste du navigateur avec les
  // morceaux qu'elle contient : meme affichage, meme defilement, meme B pour
  // revenir. L'image reste en memoire entre les deux ecrans, c'est tout ce que
  // ce detour coute.
  static uint8_t *sauveImg = 0;
  static uint32_t sauveLg = 0;
  static int sauveRangs[MD_SAUVE_MAX];
  static char navEntete[128] = "";
  auto estOuvrable = [&](const char *n) {
    const int L = (int)strlen(n);
    if (L < 5) return false;
    if (navGenre == 1) return !strcasecmp(n + L - 4, ".wav");
    if (navGenre == 2) return !strcasecmp(n + L - 4, ".bin");
    return !strcasecmp(n + L - 4, ".mdm") || !strcasecmp(n + L - 4, ".dmf");
  };

  auto scanne = [&]() {
    nbFichiers = 0; selFichier = 0; hautFichier = 0;
    DIR *d = opendir(dossier);
    if (!d) { dossier[0] = '/'; dossier[1] = 0; d = opendir(dossier); }
    if (!d) return;
    // Remonter d'un cran, sauf a la racine.
    if (strcmp(dossier, "/") != 0) {
      strcpy(fichiers[0], ".."); typeFic[0] = 2; nbFichiers = 1;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL && nbFichiers < 80) {
      const char *n = e->d_name;
      if (!strcmp(n, ".") || !strcmp(n, "..")) continue;
      if (n[0] == '.') continue;   // fichiers caches du systeme : du bruit
      char chemin[192];
      const int L = (int)strlen(dossier);
      siprintf(chemin, "%s%s%s", dossier, (L && dossier[L-1] == '/') ? "" : "/", n);
      struct stat st;
      const bool dir = (stat(chemin, &st) == 0) && S_ISDIR(st.st_mode);
      strncpy(fichiers[nbFichiers], n, 39);
      fichiers[nbFichiers][39] = 0;
      typeFic[nbFichiers] = dir ? 2 : (estOuvrable(n) ? 1 : 0);
      nbFichiers++;
    }
    closedir(d);
  };

  // Entre dans un sous-dossier, ou remonte d'un cran sur « .. ».
  auto entre = [&](const char *nom) {
    if (!strcmp(nom, "..")) {
      char *barre = strrchr(dossier, '/');
      if (barre == dossier) dossier[1] = 0;      // on revient a la racine
      else if (barre) *barre = 0;
    } else {
      const int L = (int)strlen(dossier);
      if (L + (int)strlen(nom) + 2 >= (int)sizeof(dossier)) return;
      if (!(L && dossier[L-1] == '/')) strcat(dossier, "/");
      strcat(dossier, nom);
    }
    scanne();
  };

  // Enregistre le morceau au format .mdm — celui du moteur, donc celui de
  // l'iPad. On ne demande pas de nom : il n'y a pas de clavier, et surtout on
  // n'ECRASE JAMAIS un fichier existant. Le premier numero libre est pris.
  static char dernierNom[64] = "";
  // Le nom du morceau courant, sans chemin ni extension. Il suit le fichier
  // ouvert : enregistrer proposera ce nom-la, et non celui d'un projet
  // precedent qu'on ecraserait sans le vouloir.
  static char nomProjet[16] = "";
  // Le nom se saisit dans une petite fenetre : pas de clavier sur une DS, on
  // fait donc defiler les lettres une par une.
  bool dialogueNom = false, assombrirDemande = false;
  // A quoi sert la fenetre de nom en ce moment. Elle etait cablee sur
  // l'enregistrement du projet ; les deux exports s'en servent aussi, pour
  // qu'on puisse nommer la cartouche ou le VGM au lieu de subir le nom du
  // projet.
  // NOM_ROM nomme la ROM LECTEUR ; NOM_ROM_TRK la ROM GeneTracker editable.
  enum { NOM_PROJET = 0, NOM_ROM = 1, NOM_VGM = 2, NOM_ROM_TRK = 3 };
  int nomPour = NOM_PROJET;
  static char sauveNom[9] = "";
  int nomLig = 0, nomCol = 0;   // position dans la grille de caracteres


  auto enregistre = [&](const char *nom) {
    uint32_t taille = 0;
    uint8_t *donnees = md_replayer_save_mem(&taille);
    if (!donnees || taille == 0) { if (donnees) free(donnees); return false; }

    // Ou ecrire : a cote de la cartouche si on connait son chemin, sinon la
    // carte, sinon la racine. Les memes candidats que pour le journal.
    char base[192] = "";
    if (__system_argv->argvMagic == ARGV_MAGIC &&
        __system_argv->argc > 0 && __system_argv->argv[0]) {
      strncpy(base, __system_argv->argv[0], sizeof(base) - 24);
      base[sizeof(base) - 24] = 0;
      char *barre = strrchr(base, '/');
      if (barre) barre[1] = 0; else base[0] = 0;
    }
    // On ecrit dans le dossier des MORCEAUX, jamais dans celui ou le
    // navigateur s'est arrete.
    //
    // Les deux partageaient la meme variable : apres avoir charge un
    // echantillon, elle pointait sur « Samples », et le morceau suivant y
    // atterrissait. Le journal l'a montre noir sur blanc —
    // « ENREGISTRE : .../Samples/MONMORCEAU.MDM ».
    creeSiAbsent(dossierSongs);        // au premier enregistrement
    char courant[136];
    const int Ld = (int)strlen(dossierSongs);
    siprintf(courant, "%s%s", dossierSongs,
             (Ld && dossierSongs[Ld-1] == '/') ? "" : "/");
    const char *racines[4] = { courant, base, "sd:/", "/" };

    // Le nom vient de l'utilisateur : les espaces de fin sont de la place
    // laissee vide dans la fenetre, pas des caracteres voulus.
    char propre[10]; int np = 0;
    for (int i2 = 0; i2 < 8 && nom[i2]; i2++)
      if (nom[i2] != ' ') propre[np++] = nom[i2];
    propre[np] = 0;
    if (np == 0) { strcpy(propre, "SONG"); }

    char chemin[224];
    bool ok = false;
    for (int r = 0; r < 4 && !ok; r++) {
      if (!racines[r][0]) continue;
      siprintf(chemin, "%s%s.MDM", racines[r], propre);
      FILE *f = fopen(chemin, "wb");
      if (!f) continue;                             // racine non inscriptible
      ok = fwrite(donnees, 1, taille, f) == taille;
      fclose(f);
      if (ok) { strncpy(dernierNom, chemin, 63); dernierNom[63] = 0;
                strncpy(nomProjet, propre, 15); nomProjet[15] = 0; }
    }
    free(donnees);
    return ok;
  };

  // ── Exporter une cartouche Mega Drive ───────────────────────────────────
  // Le morceau devient une vraie ROM, jouable sur la console d'origine : ce
  // que la Mega Drive jouera est le journal de TOUT ce que le moteur envoie
  // aux puces, image par image, rejoue par un petit lecteur 68000. Il n'y a
  // donc pas deux moteurs qui pourraient diverger.
  //
  // L'export DEROULE le morceau une fois en entier. A la cadence de la
  // console, cela prend l'equivalent d'une lecture complete — parfois plus.
  // On ne peut pas figer l'ecran pendant ce temps-la : l'export avance par
  // paquets d'images, on affiche l'avancement entre deux, et B annule.
  //
  // Le son se tait pendant l'operation : le moteur ne remplit plus l'anneau
  // — il travaille pour la cartouche — et laisser la puce tourner en rond sur
  // le dernier demi-seconde donnerait un bourdonnement pendant des minutes.
  auto exporteRom = [&](const char *nomVoulu) {
    if (!carteOK) { strcpy(msgProjet, "NO SD CARD"); return false; }
    creeSiAbsent(dossierRoms);

    // Le nom demande a l'export, pas celui du projet : on exporte souvent
    // plusieurs versions d'un meme morceau.
    char nom2[10]; int np = 0;
    const char *src = (nomVoulu && nomVoulu[0]) ? nomVoulu : nomProjet;
    for (int i2 = 0; i2 < 8 && src[i2]; i2++) nom2[np++] = src[i2];
    if (np == 0) { strcpy(nom2, "SONG"); np = 4; }
    nom2[np] = 0;

    char chemin[224];
    const int Ld = (int)strlen(dossierRoms);
    siprintf(chemin, "%s%s%s.BIN", dossierRoms,
             (Ld && dossierRoms[Ld-1] == '/') ? "" : "/", nom2);

    // Le silence, avant tout : l'anneau tourne en boucle materielle.
    memset(g_gauche, 0, sizeof(g_gauche));
    memset(g_droite, 0, sizeof(g_droite));
    DC_FlushRange(g_gauche, sizeof(g_gauche));
    DC_FlushRange(g_droite, sizeof(g_droite));

    // La cartouche joue le MORCEAU, pas la phrase ou la chaine en cours.
    md_replayer_stop();
    md_replayer_set_play_scope(MD_SCOPE_SONG, 0, 0);
    md_replayer_play_from(0);
    md_replayer_play();

    md_rom_export_t *ex = md_rom_export_begin(chemin, nom2,
                              SON_HZ_REEL, romPal ? 50 : 60);
    if (!ex) { md_replayer_stop(); strcpy(msgProjet, "ROM EXPORT FAILED");
               return false; }

    // Le panneau d'avancement, dessine une fois ; seules les valeurs bougent.
    ecran(g_fond);
    const int sauveCol = g_colOrigine, sauveY = g_decalYpx;
    g_colOrigine = 0; g_decalYpx = 0;
    const int fw = 28, fh = 7;
    const int fc = (kCols - fw) / 2 > 0 ? (kCols - fw) / 2 : 0;
    const int fl = (kLignes - fh) / 2 > 0 ? (kLignes - fh) / 2 : 0;
    fenetre(fc, fl, fw, fh);
    titre(fc + 2, fl + 1, "EXPORTING ROM", kTitre);
    titre(fc + 2, fl + 5, "B  CANCEL", kTitre);

    int etat = 1;
    char vu[40] = "";
    while (etat > 0) {
      // Un paquet court : assez pour avancer, assez bref pour que B reponde.
      etat = md_rom_export_step(ex, 12);

      const int f = md_rom_export_frames(ex);
      char ligne[40];
      // Ce compteur est la duree DU MORCEAU deja gravee, pas le temps passe :
      // sans l'etiquette on croit voir une horloge qui n'en finit pas. Une
      // image ne vaut pas 1/60 s en PAL : on compte dans la cadence choisie.
      const int parSec = romPal ? 50 : 60;
      siprintf(ligne, "SONG %d:%02d  %lu KB", f / (parSec * 60), (f / parSec) % 60,
               (unsigned long)(md_rom_export_bytes(ex) / 1024));
      if (strcmp(ligne, vu)) {
        strcpy(vu, ligne);
        efface(fc + 2, fl + 3, fw - 4);
        texte(fc + 2, fl + 3, ligne, kData);
      }

      swiWaitForVBlank();
      scanKeys();
      if (keysDown() & KEY_B) {
        md_rom_export_abort(ex);
        md_replayer_stop();
        g_colOrigine = sauveCol; g_decalYpx = sauveY;
        strcpy(msgProjet, "EXPORT CANCELLED");
        return false;
      }
    }
    g_colOrigine = sauveCol; g_decalYpx = sauveY;

    md_rom_report_t r;
    const bool ok = (etat == 0) && md_rom_export_end(ex, &r);
    md_replayer_stop();

    if (!ok) { strcpy(msgProjet, "ROM EXPORT FAILED"); return false; }

    char e[170];
    siprintf(e, "ROM EXPORTEE : %s  %lu OCTETS  %d IMAGES A %d HZ  %ld ECRITURES  "
                "BLOC DAC %d  %s",
             chemin, (unsigned long)r.rom_bytes, r.frames, r.hz, r.writes,
             r.dac_block, r.dac_frames ? "AVEC PCM" : "SANS PCM");
    journal(e);
    siprintf(msgProjet, "ROM SAVED : %s.BIN  %luKB  %s%s", nom2,
             (unsigned long)(r.rom_bytes / 1024), romPal ? "PAL" : "NTSC",
             r.truncated ? "  (TRUNCATED)" : "");
    return true;
  };

  // ── Exporter un fichier VGM ─────────────────────────────────────────────
  // Meme journal que la cartouche, dans un contenant ouvert : les lecteurs VGM,
  // les emulateurs et les outils qui fabriquent des ROM de jukebox le lisent.
  // Le morceau peut donc voyager a cote de VGM recuperes ailleurs.
  auto exporteVgm = [&](const char *nomVoulu) {
    if (!carteOK) { strcpy(msgProjet, "NO SD CARD"); return false; }
    creeSiAbsent(dossierVgm);

    // Le nom demande a l'export, pas celui du projet : on exporte souvent
    // plusieurs versions d'un meme morceau.
    char nom2[10]; int np = 0;
    const char *src = (nomVoulu && nomVoulu[0]) ? nomVoulu : nomProjet;
    for (int i2 = 0; i2 < 8 && src[i2]; i2++) nom2[np++] = src[i2];
    if (np == 0) { strcpy(nom2, "SONG"); np = 4; }
    nom2[np] = 0;

    char chemin[224];
    const int Ld = (int)strlen(dossierVgm);
    siprintf(chemin, "%s%s%s.VGM", dossierVgm,
             (Ld && dossierVgm[Ld-1] == '/') ? "" : "/", nom2);

    memset(g_gauche, 0, sizeof(g_gauche));
    memset(g_droite, 0, sizeof(g_droite));
    DC_FlushRange(g_gauche, sizeof(g_gauche));
    DC_FlushRange(g_droite, sizeof(g_droite));

    md_replayer_stop();
    md_replayer_set_play_scope(MD_SCOPE_SONG, 0, 0);
    md_replayer_play_from(0);
    md_replayer_play();

    // Le VGM suit la MEME region que la cartouche.
    //
    // J'avais fige 60 Hz ici, en me disant qu'un VGM se lit sur ordinateur. Il
    // finit en fait dans une ROM de jukebox, jouee sur la vraie console : un
    // VGM a 60 Hz joue sur une machine PAL traine de 20 % ET decale le PCM,
    // parce que l'outil de conversion avance les echantillons d'un nombre
    // d'images qui depend, lui aussi, de la region. C'est exactement ce qu'on
    // entendait : plus lent, et le PCM a cote.
    md_vgm_export_t *ex = md_vgm_export_begin(chemin, nom2, SON_HZ_REEL,
                                              romPal ? 50 : 60);
    if (!ex) { md_replayer_stop(); strcpy(msgProjet, "VGM EXPORT FAILED");
               return false; }

    ecran(g_fond);
    const int sauveCol = g_colOrigine, sauveY = g_decalYpx;
    g_colOrigine = 0; g_decalYpx = 0;
    const int fw = 28, fh = 7;
    const int fc = (kCols - fw) / 2 > 0 ? (kCols - fw) / 2 : 0;
    const int fl = (kLignes - fh) / 2 > 0 ? (kLignes - fh) / 2 : 0;
    fenetre(fc, fl, fw, fh);
    titre(fc + 2, fl + 1, "EXPORTING VGM", kTitre);
    titre(fc + 2, fl + 5, "B  CANCEL", kTitre);

    int etat = 1;
    char vu[40] = "";
    while (etat > 0) {
      etat = md_vgm_export_step(ex, 12);
      const int f = md_vgm_export_frames(ex);
      char ligne[40];
      siprintf(ligne, "SONG %d:%02d  %lu KB", f / 3600, (f / 60) % 60,
               (unsigned long)(md_vgm_export_bytes(ex) / 1024));
      if (strcmp(ligne, vu)) {
        strcpy(vu, ligne);
        efface(fc + 2, fl + 3, fw - 4);
        texte(fc + 2, fl + 3, ligne, kData);
      }
      swiWaitForVBlank();
      scanKeys();
      if (keysDown() & KEY_B) {
        md_vgm_export_abort(ex); md_replayer_stop();
        g_colOrigine = sauveCol; g_decalYpx = sauveY;
        strcpy(msgProjet, "EXPORT CANCELLED");
        return false;
      }
    }
    g_colOrigine = sauveCol; g_decalYpx = sauveY;

    md_vgm_report_t r;
    const bool ok = (etat == 0) && md_vgm_export_end(ex, &r);
    md_replayer_stop();
    if (!ok) { strcpy(msgProjet, "VGM EXPORT FAILED"); return false; }

    char e[170];
    siprintf(e, "VGM EXPORTE : %s  %lu OCTETS  %d IMAGES  %lu OCTETS DE PCM",
             chemin, (unsigned long)r.bytes, r.frames,
             (unsigned long)r.pcm_bytes);
    journal(e);
    siprintf(msgProjet, "VGM SAVED : %s.VGM  %luKB", nom2,
             (unsigned long)(r.bytes / 1024));
    return true;
  };

  auto ouvre = [&](const char *nom) {
    char chemin[128];
    const int L = (int)strlen(dossier);
    siprintf(chemin, "%s%s%s", dossier, (L && dossier[L-1] == '/') ? "" : "/", nom);
    FILE *f = fopen(chemin, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long taille = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (taille <= 0 || taille > 2 * 1024 * 1024) { fclose(f); return false; }
    uint8_t *tampon = (uint8_t *)malloc((size_t)taille);
    if (!tampon) { fclose(f); return false; }
    const bool lu = fread(tampon, 1, (size_t)taille, f) == (size_t)taille;
    fclose(f);
    bool ok = false;
    if (lu) {
      const int Ln = (int)strlen(nom);
      if (Ln > 4 && !strcasecmp(nom + Ln - 4, ".dmf")) {
        md_dmf_report_t r; ok = md_replayer_import_dmf(tampon, (uint32_t)taille, &r);
      } else {
        ok = md_replayer_load_mem(tampon, (uint32_t)taille);
      }
    }
    free(tampon);
    return ok;
  };
  // Declare AVANT la boucle : les fonctions de copie et de collage, definies
  // ici, doivent pouvoir signaler qu'elles ont modifie le morceau.
  bool modifie = false;

  // ── Selection et presse-papier, regles reprises de l'iPad ───────────────
  // SELECT+B ARME une selection : il ne la bascule pas. C'est ce que fait la
  // reference, et c'est ce qui rend le copier-coller utilisable — sinon, en
  // gardant SELECT enfonce, chaque B annulait la selection qu'on venait de
  // faire. Une selection se termine en copiant, en collant, ou en changeant
  // d'ecran.
  bool selActive = false;
  int selAncreLigne = 0, selAncreCol = 0;

  static uint8_t clipSong[10][MD_SONG_ROWS];
  static uint8_t clipChPh[MD_ROWS_PER_CHAIN];
  static int8_t  clipChTr[MD_ROWS_PER_CHAIN];
  static uint8_t clipPhr[MD_ROWS_PER_PHRASE][7];
  int clipPage = -1, clipLignes = 0, clipCol0 = 0, clipCols = 0;

  // Ligne et colonne du curseur sur la page courante.
  auto curL = [&]() { return page == PAGE_SONG ? curLigne
                           : page == PAGE_CHAIN ? chLigne : phLigne; };
  auto curC = [&]() { return page == PAGE_SONG ? curCanal
                           : page == PAGE_CHAIN ? chCol : phCol; };
  auto poseCurseur = [&](int l, int c) {
    if (page == PAGE_SONG)       { curLigne = l; curCanal = c; }
    else if (page == PAGE_CHAIN) { chLigne = l;  chCol = c; }
    else                         { phLigne = l;  phCol = c; }
  };

  // La case (ligne, colonne) fait-elle partie de la selection en cours ?
  auto estSelectionne = [&](int l, int c) {
    if (!selActive) return false;
    const int l0 = selAncreLigne < curL() ? selAncreLigne : curL();
    const int l1 = selAncreLigne < curL() ? curL() : selAncreLigne;
    const int c0 = selAncreCol   < curC() ? selAncreCol   : curC();
    const int c1 = selAncreCol   < curC() ? curC()        : selAncreCol;
    return l >= l0 && l <= l1 && c >= c0 && c <= c1;
  };

  auto copier = [&]() {
    if (!selActive) return;
    const int l0 = selAncreLigne < curL() ? selAncreLigne : curL();
    const int l1 = selAncreLigne < curL() ? curL() : selAncreLigne;
    const int c0 = selAncreCol   < curC() ? selAncreCol   : curC();
    const int c1 = selAncreCol   < curC() ? curC()        : selAncreCol;
    clipPage = page; clipLignes = l1 - l0 + 1;
    clipCol0 = c0;   clipCols = c1 - c0 + 1;
    for (int l = 0; l < clipLignes; l++) {
      if (page == PAGE_SONG) {
        for (int c = 0; c < clipCols; c++)
          clipSong[c][l] = md_replayer_get_song(reel(c0 + c), l0 + l);
      } else if (page == PAGE_CHAIN) {
        md_replayer_get_chain((uint8_t)chainId, l0 + l, &clipChPh[l], &clipChTr[l]);
      } else {
        uint8_t *d = clipPhr[l];
        md_replayer_get_phrase((uint8_t)phraseId, l0 + l,
                               &d[0],&d[1],&d[2],&d[3],&d[4],&d[5],&d[6]);
      }
    }
    // Le curseur revient a l'ancre, comme dans la reference.
    poseCurseur(selAncreLigne, selAncreCol);
    selActive = false;
    modifie = true;
  };

  // ── Le CLONE PROFOND, regles reprises telles quelles de l'iPad ──────────
  // Coller duplique des NUMEROS : les copies pointent sur les memes chains,
  // les memes phrases, les memes instruments — retoucher la copie retouche
  // l'original. Le clone profond recopie ce qui est dessous dans des
  // emplacements libres, pour obtenir une variation modifiable sans rien
  // casser.
  //
  // Ce qu'il duplique depend de l'ecran :
  //   SONG   la chain visee, ET toutes les phrases qu'elle emploie
  //   CHAIN  la phrase visee
  //   PHRASE l'instrument, et seulement sur sa colonne
  //
  // Une source qui revient plusieurs fois dans la selection n'est clonee
  // QU'UNE FOIS : les lignes qui la partageaient partagent aussi la copie.
  auto chainLibre = [&]() {
    static bool pris[MD_MAX_CHAINS];
    memset(pris, 0, sizeof(pris));
    for (int c = 0; c < 10; c++)
      for (int r = 0; r < MD_SONG_ROWS; r++) {
        uint8_t v = md_replayer_get_song(c, r);
        if (v != MD_EMPTY && v < MD_MAX_CHAINS) pris[v] = true;
      }
    for (int id = 0; id < MD_MAX_CHAINS; id++) {
      if (pris[id]) continue;
      bool vide = true;
      for (int r = 0; r < MD_ROWS_PER_CHAIN && vide; r++) {
        uint8_t ph; int8_t tr;
        md_replayer_get_chain((uint8_t)id, r, &ph, &tr);
        if (ph != MD_EMPTY) vide = false;
      }
      if (vide) return id;
    }
    // Aucun vide : on prend le premier inutilise et on le nettoie.
    for (int id = 0; id < MD_MAX_CHAINS; id++)
      if (!pris[id]) {
        for (int r = 0; r < MD_ROWS_PER_CHAIN; r++)
          md_replayer_set_chain((uint8_t)id, r, MD_EMPTY, 0);
        return id;
      }
    return MD_MAX_CHAINS - 1;
  };

  auto phraseLibre = [&]() {
    static bool pris[MD_MAX_PHRASES];
    memset(pris, 0, sizeof(pris));
    for (int ch = 0; ch < MD_MAX_CHAINS; ch++)
      for (int r = 0; r < MD_ROWS_PER_CHAIN; r++) {
        uint8_t ph; int8_t tr;
        md_replayer_get_chain((uint8_t)ch, r, &ph, &tr);
        if (ph != MD_EMPTY && ph < MD_MAX_PHRASES) pris[ph] = true;
      }
    for (int id = 0; id < MD_MAX_PHRASES; id++) {
      if (pris[id]) continue;
      bool vide = true;
      for (int r = 0; r < MD_ROWS_PER_PHRASE && vide; r++) {
        uint8_t no,i2,ve,cm,cv,mc,mv;
        md_replayer_get_phrase((uint8_t)id, r, &no,&i2,&ve,&cm,&cv,&mc,&mv);
        // Attention a la velocite : sa valeur VIDE est MD_EMPTY (0xFF), pas
        // zero — zero est un volume nul, donc le silence. En la lisant comme
        // une donnee presente, toutes les phrases paraissaient occupees ;
        // phraseLibre tombait dans sa branche de secours, effacait la phrase
        // choisie en y ecrivant velocite zero, et la voie restait muette.
        if (no || i2 || ve != MD_EMPTY || cm != MD_EMPTY || mc != MD_EMPTY)
          vide = false;
      }
      if (vide) return id;
    }
    for (int id = 0; id < MD_MAX_PHRASES; id++)
      if (!pris[id]) {
        for (int r = 0; r < MD_ROWS_PER_PHRASE; r++)
          md_replayer_set_phrase((uint8_t)id, r, 0, 0, MD_EMPTY,
                                 MD_EMPTY, 0, MD_EMPTY, 0);
        return id;
      }
    return MD_MAX_PHRASES - 1;
  };

  auto instrLibre = [&]() {
    static bool pris[MD_MAX_INSTRUMENTS + 1];
    memset(pris, 0, sizeof(pris));
    for (int ph = 0; ph < MD_MAX_PHRASES; ph++)
      for (int r = 0; r < MD_ROWS_PER_PHRASE; r++) {
        uint8_t no,i2,ve,cm,cv,mc,mv;
        md_replayer_get_phrase((uint8_t)ph, r, &no,&i2,&ve,&cm,&cv,&mc,&mv);
        if (i2 && i2 <= MD_MAX_INSTRUMENTS) pris[i2] = true;
      }
    for (int id = 1; id <= MD_MAX_INSTRUMENTS; id++)
      if (!pris[id]) return id;
    return MD_MAX_INSTRUMENTS;
  };

  auto cloneProfond = [&]() {
    if (!selActive) return;
    const int l0 = selAncreLigne < curL() ? selAncreLigne : curL();
    const int l1 = selAncreLigne < curL() ? curL() : selAncreLigne;
    // Sur PHRASE, il n'y a rien « dessous » ailleurs que sur la colonne
    // INSTRUMENT : le clone n'y a aucun sens.
    if (page == PAGE_PHRASE && phCol != 1) { selActive = false; return; }

    static int mapPh[MD_MAX_PHRASES];
    static int mapIn[MD_MAX_INSTRUMENTS + 1];

    if (page == PAGE_SONG) {
      for (int i = 0; i < MD_MAX_PHRASES; i++) mapPh[i] = -1;
      const int col = reel(curCanal);
      for (int r = l0; r <= l1; r++) {
        uint8_t src = md_replayer_get_song(col, r);
        if (src == MD_EMPTY) continue;
        int nc = chainLibre();
        for (int k = 0; k < MD_ROWS_PER_CHAIN; k++) {
          uint8_t ph; int8_t tr;
          md_replayer_get_chain(src, k, &ph, &tr);
          if (ph != MD_EMPTY) {
            if (mapPh[ph] < 0) {
              int np = phraseLibre();
              for (int pr = 0; pr < MD_ROWS_PER_PHRASE; pr++) {
                uint8_t no,i2,ve,cm,cv,mc,mv;
                md_replayer_get_phrase(ph, pr, &no,&i2,&ve,&cm,&cv,&mc,&mv);
                md_replayer_set_phrase((uint8_t)np, pr, no,i2,ve,cm,cv,mc,mv);
              }
              mapPh[ph] = np;
            }
            ph = (uint8_t)mapPh[ph];
          }
          md_replayer_set_chain((uint8_t)nc, k, ph, tr);
        }
        md_replayer_set_song(col, r, (uint8_t)nc);
      }
    } else if (page == PAGE_CHAIN) {
      for (int i = 0; i < MD_MAX_PHRASES; i++) mapPh[i] = -1;
      for (int r = l0; r <= l1; r++) {
        uint8_t ph; int8_t tr;
        md_replayer_get_chain((uint8_t)chainId, r, &ph, &tr);
        if (ph == MD_EMPTY) continue;
        if (mapPh[ph] < 0) {
          int np = phraseLibre();
          for (int pr = 0; pr < MD_ROWS_PER_PHRASE; pr++) {
            uint8_t no,i2,ve,cm,cv,mc,mv;
            md_replayer_get_phrase(ph, pr, &no,&i2,&ve,&cm,&cv,&mc,&mv);
            md_replayer_set_phrase((uint8_t)np, pr, no,i2,ve,cm,cv,mc,mv);
          }
          mapPh[ph] = np;
        }
        md_replayer_set_chain((uint8_t)chainId, r, (uint8_t)mapPh[ph], tr);
      }
    } else {
      for (int i = 0; i <= MD_MAX_INSTRUMENTS; i++) mapIn[i] = -1;
      for (int r = l0; r <= l1; r++) {
        uint8_t no,i2,ve,cm,cv,mc,mv;
        md_replayer_get_phrase((uint8_t)phraseId, r, &no,&i2,&ve,&cm,&cv,&mc,&mv);
        if (!i2) continue;
        if (mapIn[i2] < 0) {
          int ni = instrLibre();
          md_replayer_copy_instrument(i2, ni);
          mapIn[i2] = ni;
        }
        md_replayer_set_phrase((uint8_t)phraseId, r, no,(uint8_t)mapIn[i2],ve,cm,cv,mc,mv);
        instrId = mapIn[i2];
      }
    }
    selActive = false;
    modifie = true;
  };

  auto coller = [&]() {
    if (clipPage != page || clipLignes <= 0) return;
    const int l0 = curL(), c0 = curC();
    const int n = clipLignes;

    // ── SUR SONG, ET SUR SONG SEULEMENT, LE COLLAGE INSERE ────────────────
    // Tout ce qui se trouve sous le curseur descend d'autant de lignes que le
    // presse-papier en contient, et ce qui deborde en bas est perdu : coller
    // une mesure ne doit pas effacer la suivante, elle doit la repousser.
    // TOUTES les colonnes descendent, pas seulement celles qu'on a copiees —
    // decaler une seule voie desynchroniserait le morceau.
    //
    // ⚠️ NULLE PART AILLEURS. Une chain et une phrase ont seize rangees fixes
    // qui font une mesure : y inserer decalerait le rythme au lieu de
    // remplacer ce qu'on vise. Le collage y ecrase.
    if (page == PAGE_SONG) {
      for (int c = 0; c < 10; c++)
        for (int r = MD_SONG_ROWS - 1; r >= l0 + n; r--)
          md_replayer_set_song(c, r, md_replayer_get_song(c, r - n));
      for (int r = l0; r < l0 + n && r < MD_SONG_ROWS; r++)
        for (int c = 0; c < 10; c++) md_replayer_set_song(c, r, MD_EMPTY);
      for (int l = 0; l < n; l++) {
        if (l0 + l >= MD_SONG_ROWS) break;
        for (int c = 0; c < clipCols; c++)
          if (c0 + c < nbVisibles)
            md_replayer_set_song(reel(c0 + c), l0 + l, clipSong[c][l]);
      }
    } else if (page == PAGE_CHAIN) {
      for (int l = 0; l < n; l++) {
        if (l0 + l >= MD_ROWS_PER_CHAIN) break;
        md_replayer_set_chain((uint8_t)chainId, l0 + l, clipChPh[l], clipChTr[l]);
      }
    } else {
      for (int l = 0; l < n; l++) {
        if (l0 + l >= MD_ROWS_PER_PHRASE) break;
        const uint8_t *d3 = clipPhr[l];
        md_replayer_set_phrase((uint8_t)phraseId, l0 + l,
                               d3[0],d3[1],d3[2],d3[3],d3[4],d3[5],d3[6]);
      }
    }
    modifie = true;
  };

  auto entretienSon = [&]() {
      unsigned tAv = timerTick(2);
      unsigned tSon = tAv;
      horloge += (unsigned short)(tSon - tPrecSon);
      tPrecSon = tSon;
      teteLecture = (unsigned)((unsigned long long)horloge * SON_HZ_REEL / kTicksParSeconde);
      unsigned cible = (unsigned)((unsigned long long)horloge * SON_HZ_REEL / kTicksParSeconde)
                       + SON_IMAGE * avance;
      // Six images d'avance.
      //
      // Douze donnaient 200 ms, soit 1,7 ligne de retard a 125 BPM : le son
      // arrivait audiblement apres l'affichage. Six donnent 100 ms, moins d'une
      // ligne. C'est le curseur du compromis : trop bas, la puce rejoue l'anneau
      // (le « disque raye ») quand un tour de boucle deborde ; trop haut, on
      // edite en decale. Le vrai remede est de remplir sous interruption VBlank,
      // pour que le dessin ne puisse plus retarder l'audio.
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
      // Marge : ce qu'il reste d'avance avant que la puce rattrape l'ecriture.
      // C'est LE chiffre qui dit si le processeur suit. Quand il touche zero,
      // l'anneau se rejoue — c'est le disque raye.
      {
        unsigned tete = cible > SON_IMAGE * avance ? cible - SON_IMAGE * avance : 0;
        unsigned marge = (g_ecrit > tete) ? g_ecrit - tete : 0;
        if (marge < margeMin) margeMin = marge;
        if (marge == 0) {
          retards++;
          // On a laisse l'anneau se vider : on s'octroie plus de reserve, tout
          // de suite. Deux images a la fois, pour trouver le palier vite.
          if (avance < kAvanceMax) avance += 2;
          secondesPropres = 0;
        }
      }
      unsigned seuil = SON_IMAGE / 4;
      if (cible > g_ecrit && cible - g_ecrit >= seuil) {
        unsigned manque = cible - g_ecrit;
        // Rattraper au-dela d'un anneau n'a aucun sens : on reecrirait du son
        // deja joue. Dans ce cas on se resynchronise, quitte a sauter.
        if (manque > SON_ANNEAU) {
          sauts++;
          g_ecrit = cible - SON_IMAGE * 4;
          manque = SON_IMAGE * 4;
        }
        // ── Un remplissage ne doit JAMAIS etre long ─────────────────────
        //
        // C'etait le defaut de fond, et il s'auto-entretenait : plus on avait
        // de retard, plus on demandait d'echantillons d'un coup, plus l'appel
        // durait, plus on prenait de retard. Mesure sur la console au moment
        // ou le son se degradait : un remplissage a 149 ms, et un tour de
        // boucle au-dela d'une seconde — parce que sept de ces remplissages
        // geants s'enchainaient pendant un seul repeint d'ecran.
        //
        // On borne donc chaque appel a deux images de son. Le rattrapage se
        // fait alors sur plusieurs appels au lieu d'un seul : a soixante tours
        // par seconde on produit deux fois ce qu'il faut, donc on rattrape
        // quand meme — mais sans jamais bloquer plus de quelques millisecondes.
        if (manque > SON_IMAGE * 8) manque = SON_IMAGE * 8;
        unsigned tR = timerTick(2);
        son_remplir((int)manque);
        unsigned dR = (unsigned short)(timerTick(2) - tR);
        if (dR > pireRemp) pireRemp = dR;
        nbRemplis++;
        noteHistoire();
      }
      cumulAudio += (unsigned short)(timerTick(2) - tAv);
  };

  {
    char e[120];
    journal("");
    // La DATE DE COMPILATION dans l'en-tete : sans elle, impossible de savoir
    // si un journal vient de la derniere ROM ou d'une ancienne restee sur la
    // carte — ce qui a deja fait chercher un defaut la ou il n'y en avait pas.
    siprintf(e, "=== GENETRACKERDS  %s  %d HZ  DIVISEUR %d  BUILD %s %s ===",
             modeDSi ? "DSI 134MHZ" : "DS 67MHZ", (int)SON_HZ, MD_YM_DIVISEUR,
             __DATE__, __TIME__);
    journal(e);
    // Les dossiers retenus, pour qu'on n'ait plus a deviner ou un morceau est
    // parti — ni pourquoi il n'est pas la ou on l'attendait.
    if (__system_argv->argvMagic == ARGV_MAGIC && __system_argv->argc > 0 &&
        __system_argv->argv[0]) {
      char e2[190];
      siprintf(e2, "  CARTOUCHE %s", __system_argv->argv[0]);
      journal(e2);
    }
    { char e3[190];
      siprintf(e3, "  SONGS %s", dossierSongs);   journal(e3);
      siprintf(e3, "  SAMPLES %s", dossierSamples); journal(e3); }
  }

  // Le grand remplissage initial est derriere nous : on repart d'un compteur
  // propre, sinon il se lisait comme un premier tour de boucle interminable.
  tPrec = timerTick(2);

  while (true) {
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
    entretienSon();
    // cumulAudio est deja accumule plus haut, autour du seul remplissage.
    // Il l'etait AUSSI ici, depuis tA qui est en tete de boucle — donc sur le
    // tour entier. TPS restait colle a 99 % quoi qu'on fasse et ne mesurait
    // rien. Ici on ne compte plus que la duree totale du tour.
    unsigned tB = timerTick(2);
    unsigned dTour = (unsigned short)(tB - tPrec);
    if (dTour > pireTour) {
      pireTour = dTour;
      pirePage = "SCPIJT"[page];     // Song Chain Phrase Instr proJect Table
      pireRemplis = nbRemplis;
      pireCause = causeTour;
      pireEnLecture = enLecture;
    }
    causeTour = "-";                 // le tour suivant repart vierge
    nbRemplis = 0;
    cumul += dTour;                          // soustraction 16 bits : le
    tPrec = tB;                              // bouclage du compteur est gere
    tours++;
    // Moyenne sur HUIT secondes, pas une.
    //
    // Le morceau n'occupe pas le processeur de la meme facon d'un passage a
    // l'autre : selon le nombre de voies qui sonnent, la mesure sur une seconde
    // varie de plus ou moins 3 %. Toute optimisation gagnant moins que ca etait
    // donc indiscernable du bruit — j'ai failli en juger une comme une
    // regression sur ce seul motif.
    if (cumul >= kTicksParSeconde) {
      fpsVu = (int)tours;
      partAudio = (int)((unsigned long long)cumulAudio * 100 / cumul);
      livresVu = g_livres; g_livres = 0;
      ecretesVu = md_chip_ecretes_et_remet_a_zero();
      // Une seconde entiere sans un seul retard : on peut sans doute rendre
      // un peu de latence. On redescend d'une image a la fois, bien plus
      // lentement qu'on ne monte — mieux vaut un peu de retard qu'un son sale.
      if (retards == 0 && sauts == 0) {
        if (++secondesPropres >= 3 && avance > kAvanceMin) {
          avance--; secondesPropres = 0;
        }
      } else secondesPropres = 0;
      retardsVu = retards; sautsVu = sauts;
      margeVue = (margeMin == 0xFFFFFFFF) ? 0 : margeMin;
      retards = 0; sauts = 0; margeMin = 0xFFFFFFFF;
      // On ne consigne QUE les secondes ou quelque chose a lache : un journal
      // qui note tout devient illisible et use la carte pour rien.
      // Etat vivant de chaque voie, juste apres un depart.
      if (journalVoies > 0) {
        journalVoies--;
        char l[160]; int n = 0;
        n += siprintf(l + n, "T%04u  VOIES:", (unsigned)secondes);
        for (int c = 0; c < 10; c++) {
          const int ch = md_replayer_play_chain(c);
          const int ph = md_replayer_play_phrase(c);
          const int ro = md_replayer_play_phrase_row(c);
          n += siprintf(l + n, " %s:c%d/p%d/l%d", noms[c], ch, ph, ro);
        }
        journal(l);
      }
      // L'ECRETAGE compte autant que les retards. Un melange qui depasse ce
      // qu'un entier 16 bits peut porter est rabote : c'est une distorsion
      // franche, sans le moindre retard — donc invisible jusqu'ici.
      // On consigne des que l'un des maxima MONTE — donc exactement quand le
      // temoin passe au rouge. Auparavant le temoin regardait les maxima et le
      // journal la seconde en cours : les deux pouvaient se contredire, et un
      // temoin rouge sans ligne de journal ne renseignait sur rien.
      const bool aMonte = (retardsVu > rMax) || (sautsVu > sMax) ||
                          (ecretesVu > ecMax);
      if (aMonte) {
        char e[140];
        siprintf(e,
          "T%04u  DEFAUT  R%u/%u  ECRETAGES %u  AUDIO %d%%  DESSIN %d%%"
          "  AVANCE %u  PIRE TOUR %ums (PAGE %c, %u REMPLIS, %s%s)"
          "  PIRE REMPLI %ums",
          // retardsVu et sautsVu, pas retards et sauts : ces deux-la viennent
          // d'etre remis a zero quelques lignes plus haut, et toutes les
          // lignes du journal annoncaient donc « R0/0 » quoi qu'il arrive.
          (unsigned)(secondes), retardsVu, sautsVu, ecretesVu, partAudio,
          partDessin, avance, pireTour * 1000 / kTicksParSeconde, pirePage,
          pireRemplis, pireCause, pireEnLecture ? ", EN LECTURE" : "",
          pireRemp * 1000 / kTicksParSeconde);
        journal(e);
      }
      secondes++;
      pirePageVu = pirePage; pireRemplisVu = pireRemplis;
      pireTourVu = pireTour * 1000 / kTicksParSeconde;   // en millisecondes
      pireRempVu = pireRemp * 1000 / kTicksParSeconde;
      pireTour = 0; pireRemp = 0;
      partDessin = (int)((unsigned long long)cumulDessin * 100 / cumul);
      cumulDessin = 0;
      if (partAudio  > aMax) aMax = partAudio;
      if (partDessin > dMax) dMax = partDessin;
      if (ecretesVu  > ecMax) ecMax = ecretesVu;
      if (retardsVu  > rMax) rMax = retardsVu;
      if (sautsVu    > sMax) sMax = sautsVu;
      if (pireTourVu > pMax) pMax = pireTourVu;
      if (pireRempVu > fMax) fMax = pireRempVu;
      if (avance     > vMax) vMax = avance;
      cumul = 0; cumulAudio = 0; tours = 0;
    }

    scanKeys();
    // Toute modification leve ce drapeau. Sans lui, la page SONG — qui ne se
    // redessine que sur deplacement du curseur — n'affichait pas ce qu'on
    // venait de poser tant qu'on n'avait pas bouge.
    modifie = false;
    const int appui = keysDownRepeat();
    const int frappe = keysDown();
    // L et R valent SELECT : sur une DS, SELECT est mal place pour un accord
    // tenu, alors que les gachettes tombent sous les index.
    const bool selTenu = (keysHeld() & (KEY_SELECT | KEY_L | KEY_R)) != 0;
    const int relache = keysUp();

    // ── La region, dans la fenetre d'export ────────────────────────────────
    // Elle se choisit AU MOMENT ou l'on exporte, pas seulement dans le menu :
    // le reglage du menu sert de defaut, mais tout le monde n'est pas en
    // Europe, et on exporte parfois les deux versions du meme morceau.
    //
    // L et R, et non la croix : celle-ci promene le curseur dans la grille des
    // lettres. Presses SEULS ils ne declenchent rien d'autre — l'accord de
    // changement de page demande SELECT ET une direction.
    if (dialogueNom && nomPour != NOM_PROJET) {
      if (frappe & KEY_L) { romPal = true;  pageVue = -1; }
      if (frappe & KEY_R) { romPal = false; pageVue = -1; }
    }

    // L'accord SELECT+B reste valable tant que SELECT est tenu, meme si B a
    // deja ete relache : sinon il faudrait garder trois doigts poses pour le
    // clone profond. C'est ce que fait la reference.
    static bool accordSelB = false;
    if (!selTenu) accordSelB = false;

    // B sert a trois choses : armer une selection avec SELECT, sauter des
    // lignes ou regler le tempo avec la croix, et copier tout seul. On ne peut
    // donc pas copier des l'APPUI — on copie au RELACHEMENT, et seulement si
    // aucune direction n'a ete pressee entre-temps.
    static bool bUtilise = false;
    if (keysHeld() & KEY_B) {
      if (appui & (KEY_UP|KEY_DOWN|KEY_LEFT|KEY_RIGHT)) bUtilise = true;
    }
    if (frappe & KEY_B) bUtilise = false;

    if (selTenu) {
      // SELECT + croix : on change de PAGE.
      // Rangee du bas : SONG - CHAIN - PHRASE. SELECT + haut monte a PROJECT,
      // SELECT + bas en redescend.
      static int derniereRangee = PAGE_SONG;
      if (frappe & KEY_UP)   { if (page != PAGE_PROJECT) derniereRangee = page;
                               page = PAGE_PROJECT; }
      if (frappe & KEY_DOWN) {
        if (page == PAGE_PROJECT) {
          // Quitter la page REPOND aux questions restees en suspens, par la
          // reponse prudente : « non » pour un nouveau morceau, « laisse
          // eteint » pour les voies supplementaires — qui est deja l'etat
          // applique, donc rien ne change dans le dos de personne.
          //
          // Sans ca la question restait posee : apres avoir charge la demo et
          // etre parti sans repondre, chaque retour sur PROJECT retombait sur
          // « THIS SONG USES FM5 PS2 PS3 » au lieu du menu.
          if (demandeNouveau || demandeVoies) {
            if (demandeVoies) journal("VOIES FM5 PS2 PS3 : QUESTION QUITTEE, "
                                      "ELLES RESTENT ETEINTES");
            demandeNouveau = false; demandeVoies = false;
          }
          page = derniereRangee;
        }
      }
      if (page != PAGE_PROJECT) {
        // SELECT + droite = drillIn : on DESCEND DANS la case pointee, et on
        // refuse si elle est vide. C'est ce que fait LSDJ ; passer betement a
        // l'ecran suivant afficherait toujours le chain 00.
        if (frappe & KEY_RIGHT) {
          if (page == PAGE_SONG) {
            uint8_t v = md_replayer_get_song(reel(curCanal), curLigne);
            // On retient la COLONNE : c'est elle qui sonnera en solo depuis
            // CHAIN et PHRASE. Prendre le curseur SONG du moment faisait jouer
            // une AUTRE voie que celle qu'on edite — sur les colonnes PSG et
            // bruit, on n'entendait donc rien.
            if (v != MD_EMPTY) { chainId = v; chLigne = 0; chCol = 0;
                                 voieCourante = reel(curCanal);
                                 page = PAGE_CHAIN; }
          } else if (page == PAGE_CHAIN) {
            uint8_t ph; int8_t tsp; md_replayer_get_chain(chainId, chLigne, &ph, &tsp);
            if (ph != MD_EMPTY) { phraseId = ph; phLigne = 0; phCol = 0; page = PAGE_PHRASE; }
          } else if (page == PAGE_PHRASE) {
            uint8_t no,i2,vel,cmd,cv,mc,mv;
            md_replayer_get_phrase(phraseId, phLigne, &no,&i2,&vel,&cmd,&cv,&mc,&mv);
            // La page d'instrument depend de la COLONNE d'ou l'on descend,
            // pas de l'instrument : un meme instrument pose sur une colonne FM
            // ou PSG ne pilote pas la meme puce, et on ne montre que les
            // reglages qui agissent reellement. C'est la regle de l'iPad.
            if (i2) { instrId = i2; instrVoie = reel(curCanal);
                      inLigne = 0; inCol = 0; page = PAGE_INSTR; }
          } else if (page == PAGE_INSTR) {
            // La page « T » de LSDJ : la table de l'instrument, accessible
            // depuis N'IMPORTE QUELLE page d'instrument — FM, PCM, PSG, bruit.
            tableId = tableDeLInstrument(instrId);
            tabLigne = 0; tabCol = 0;
            page = PAGE_TABLE; pageVue = -1;
          }
        }
        // SELECT + gauche = drillOut : on remonte d'un cran. Depuis la TABLE
        // on revient a l'instrument dont elle depend, pas a la page d'avant.
        if ((frappe & KEY_LEFT) && page == PAGE_TABLE) { page = PAGE_INSTR;
                                                         pageVue = -1; }
        else if ((frappe & KEY_LEFT) && page > PAGE_SONG && page < PAGE_PROJECT)
          page--;
        if (frappe & (KEY_LEFT | KEY_RIGHT | KEY_UP | KEY_DOWN)) selActive = false;

        // SELECT + B : ARME une selection. Il ne la bascule pas — sinon,
        // SELECT reste tenu et chaque B annulerait celle qu'on vient de faire.
        if ((frappe & KEY_B) && page <= PAGE_PHRASE) {
          if (!selActive) {
            selActive = true;
            selAncreLigne = curL(); selAncreCol = curC();
          }
          accordSelB = true;
          bUtilise = true;    // ce B-la n'est pas une copie
        }
        // SELECT + A : coller. Avec une selection armee et l'accord B en
        // cours, c'est le clone profond.
        if ((frappe & KEY_A) && page <= PAGE_PHRASE) {
          if (selActive && (accordSelB || (keysHeld() & KEY_B))) cloneProfond();
          else coller();
        }
      }
    } else {
      const bool aTenu = (keysHeld() & KEY_A) != 0;

      // Le relachement de A coupe la note d'audition, sur la voie ou elle a
      // ete jouee.
      if ((relache & KEY_A) && voieAudition >= 0) {
        md_replayer_stop_test_note(voieAudition);
        voieAudition = -1;
      }

      // B relache sans avoir servi a autre chose : on copie la selection.
      // Sans selection armee, il ne se passe rien — c'est ce que fait
      // copySelection() cote iPad, qui sort sur « NOTHING SELECTED ».
      if ((relache & KEY_B) && !bUtilise && page <= PAGE_PHRASE) copier();

      // ── B + haut/bas : le grand saut dans SONG ─────────────────────────
      // Et RIEN D'AUTRE. B + croix reglait aussi le tempo depuis n'importe
      // quelle page : on changeait le BPM du morceau par accident en voulant
      // faire tout autre chose. Le tempo se regle dans PROJECT, la ou il est
      // ecrit.
      if ((keysHeld() & KEY_B) &&
          (appui & (KEY_UP|KEY_DOWN|KEY_LEFT|KEY_RIGHT))) {
        if (page == PAGE_SONG && (appui & (KEY_UP|KEY_DOWN))) {
          // Sur SONG, B + haut/bas fait un GRAND saut : seize lignes d'un
          // coup, soit une page hexadecimale. La chanson en compte 256, les
          // parcourir ligne par ligne etait interminable.
          //
          // Le signe est CALCULE ICI, pas repris de `sens`. Celui-la sert au
          // reglage d'une valeur, ou haut veut dire PLUS ; ici on se DEPLACE,
          // et la ligne 00 est en haut de l'ecran : monter, c'est diminuer le
          // numero. Partager le meme signe inversait les deux directions.
          curLigne = borne(curLigne + ((appui & KEY_UP) ? -16 : 16),
                           0, MD_SONG_ROWS - 1);
        }
      } else

      // ── A + croix : modifier la valeur sous le curseur ─────────────────
      // Gauche/droite = pas de 1, haut/bas = grand pas. Comme sur l'iPad.
      // La page PROJECT est EXCLUE d'ici : elle a ses propres reglages a la
      // croix — le tempo, les lettres du nom de fichier — et elle les traite
      // dans sa branche, plus bas. Sans cette exclusion, tenir A et presser une
      // fleche entrait ici, ne trouvait aucune page correspondante, et sautait
      // toute la branche PROJECT : ni le tempo ni le choix des lettres ne
      // repondaient.
      if (page != PAGE_PROJECT &&
          aTenu && (appui & (KEY_UP|KEY_DOWN|KEY_LEFT|KEY_RIGHT))) {
        const int sens = (appui & (KEY_UP|KEY_RIGHT)) ? 1 : -1;
        const bool grand = (appui & (KEY_UP|KEY_DOWN)) != 0;
        if (page == PAGE_SONG) {
          uint8_t v = md_replayer_get_song(reel(curCanal), curLigne);
          if (v != MD_EMPTY) {
            int n = borne((int)v + sens * (grand ? 16 : 1), 0, MD_MAX_CHAINS - 1);
            md_replayer_set_song(reel(curCanal), curLigne, (uint8_t)n); dernierChain = n;
          } else if (sens > 0) md_replayer_set_song(reel(curCanal), curLigne, 0);
        } else if (page == PAGE_CHAIN) {
          {
            const uint8_t noChain = (uint8_t)chainId;
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
          const int ins = instrId;
          const int pas = sens * (grand ? 16 : 1);
          const int g = genreInstr(instrVoie, ins);

          if (g == GENRE_PCM) {
            // Meme ordre que l'affichage. « LOAD SAMPLE » ne reagit pas a la
            // croix : c'est A qui l'actionne, plus bas.
            const int ech = md_replayer_get_instr_sample(ins);
            if (inLigne == 1) {
              // On parcourt le DOSSIER ; -1 vaut « aucun echantillon ».
              if (!echScanne) scanneEch();
              const int p2 = borne(echSel + sens, -1, echNb - 1);
              echSel = p2;
              if (p2 < 0) md_replayer_set_instr_sample(ins, -1);
              else {
                // Deja en banque ? On s'y rebranche.
                int idx = -1;
                for (int i3 = 0; i3 < md_replayer_sample_count(); i3++) {
                  char nm[24]; uint32_t lg2; int bcl2, base2;
                  if (md_replayer_get_sample(i3, nm, 23, &lg2, &bcl2, &base2) &&
                      !strcasecmp(nm, echFic[p2])) { idx = i3; break; }
                }
                if (idx < 0) {
                  char chemin[224];
                  const int Ld2 = (int)strlen(dossierSamples);
                  siprintf(chemin, "%s%s%s", dossierSamples,
                           (Ld2 && dossierSamples[Ld2-1] == '/') ? "" : "/",
                           echFic[p2]);
                  uint32_t lg2 = 0;
                  causeTour = "SAMPLE";
                  uint8_t *pcm2 = md_audio_charge_wav(chemin, &lg2);
                  if (pcm2 && lg2) {
                    idx = md_replayer_add_sample(echFic[p2], pcm2, lg2, -1, 49);
                    strcpy(msgPCM, idx >= 0 ? "LOADED" : "BANK FULL");
                  } else strcpy(msgPCM, "BAD WAV");
                  if (pcm2) free(pcm2);
                }
                if (idx >= 0) {
                  md_replayer_set_instr_sample(ins, idx);
                  md_replayer_set_instr_kind(ins, MD_INSTR_KIND_PCM);
                  // On l'ENTEND en le choisissant : feuilleter une liste
                  // d'echantillons sans les entendre n'a aucun interet.
                  // A l'arret seulement, comme pour les notes : une audition
                  // par-dessus la lecture volerait une voie au morceau.
                  if (!enLecture) {
                    voieAudition = instrVoie;
                    md_replayer_play_test_note(49, ins, voieAudition);
                  }
                }
              }
            } else if (inLigne == 4) {
              md_replayer_set_instr_pcm_volume(ins,
                  borne(md_replayer_get_instr_pcm_volume(ins) + pas, 0, 255));
            } else if (inLigne == 5) {
              md_replayer_set_instr_gen_val(ins, MD_GEN_PROP_PANNING,
                  panDeplace(md_replayer_get_instr_gen_val(ins,
                                 MD_GEN_PROP_PANNING), sens));
            } else if (inLigne == 6) {
              changeTable(ins, sens);           // la table, en bas comme ailleurs
            } else if (ech >= 0 && (inLigne == 2 || inLigne == 3)) {
              // Ces deux-la appartiennent a l'ECHANTILLON, pas a l'instrument :
              // sans echantillon charge, il n'y a rien a regler — c'est pour ca
              // qu'elles semblaient mortes.
              char nm[24]; uint32_t lg = 0; int bcl = -1, base = 60;
              md_replayer_get_sample(ech, nm, 23, &lg, &bcl, &base);
              if (inLigne == 2)
                md_replayer_set_sample_base(ech,
                    borne(base + sens * (grand ? 12 : 1), 1, MD_MAX_NOTE));
              else
                md_replayer_set_sample_loop(ech,
                    borne(bcl + sens * (grand ? 1024 : 16), -1, (int)lg));
            }
          } else if (g != GENRE_FM) {
            // MEME numerotation que l'affichage et la navigation, calculee de
            // la meme facon : c'est la seule garantie qu'elles ne divergent
            // jamais. Elles l'avaient deja fait une fois, d'un cran, sur la
            // voie de bruit.
            //
            // Ordre canonique, TABLE EN DERNIER :
            //   0 ENV   1 OUTPUT   2 FINETUNE   3 NOISE MODE (bruit seul)
            //   4 VOL LEN  5 VOL LOOP  6 VOL   7 ARP LEN  8 ARP LOOP
            //   9 ARP FIXED  10 ARP   11 TABLE
            // Une voie PSG saute le mode de bruit, d'ou le +1 au-dela de 3.
            const bool bruit = (g == GENRE_BRUIT);
            const int canon = (bruit || inLigne < 3) ? inLigne : inLigne + 1;

            uint8_t vol[MD_PSG_MACRO_MAX]; int bcl = MD_EMPTY;
            int nvol = md_replayer_get_psg_vol_macro(ins, vol,
                                                     MD_PSG_MACRO_MAX, &bcl);
            int8_t arp[MD_PSG_MACRO_MAX]; int bcla = MD_EMPTY; bool fixe = false;
            int narp = md_replayer_get_psg_arp_macro(ins, arp,
                                          MD_PSG_MACRO_MAX, &bcla, &fixe);
            switch (canon) {
            case 0: {  // enveloppe : haut/bas l'amplitude, gauche/droite la vitesse
              const int pt = borne(inCol, 0, 2);
              const int amp = md_replayer_get_env_amp(ins, pt);
              const int vit = md_replayer_get_env_speed(ins, pt);
              if (grand) {
                const int a2 = (amp == MD_EMPTY ? -1 : amp) + sens;
                if (a2 < 0) md_replayer_set_env_amp(ins, pt, MD_EMPTY);
                else        md_replayer_set_env_amp(ins, pt, borne(a2, 0, 15));
              } else {
                md_replayer_set_env_speed(ins, pt, borne(vit + sens, 0, 15));
              }
              break;
            }
            case 1:    // sortie : gauche, centre, droite
              md_replayer_set_instr_gen_val(ins, MD_GEN_PROP_PANNING,
                  panDeplace(md_replayer_get_instr_gen_val(ins,
                                 MD_GEN_PROP_PANNING), sens));
              break;
            case 2:    // desaccord fin
              md_replayer_set_instr_gen_val(ins, MD_GEN_PROP_FINE_TUNE,
                  borne(md_replayer_get_instr_gen_val(ins,
                        MD_GEN_PROP_FINE_TUNE) + pas, 0, 127));
              break;
            case 3:    // mode de bruit, propre a la voie NO
              md_replayer_set_instr_gen_val(ins, MD_GEN_PROP_PSG_NOISE,
                  borne(md_replayer_get_instr_gen_val(ins,
                        MD_GEN_PROP_PSG_NOISE) + sens, 0, 7));
              break;
            case 4: nvol = borne(nvol + sens, 0, 16);
                    md_replayer_set_psg_vol_macro(ins, vol, nvol, bcl); break;
            case 5: { int b = (bcl == MD_EMPTY ? -1 : bcl) + sens;
                      md_replayer_set_psg_vol_macro(ins, vol, nvol,
                          b < 0 ? MD_EMPTY : borne(b, 0, nvol ? nvol - 1 : 0));
                    } break;
            case 6: if (inCol < nvol) {
                      vol[inCol] = (uint8_t)borne(vol[inCol] + sens, 0, 15);
                      md_replayer_set_psg_vol_macro(ins, vol, nvol, bcl);
                    } break;
            case 7: narp = borne(narp + sens, 0, 16);
                    md_replayer_set_psg_arp_macro(ins, arp, narp, bcla, fixe);
                    break;
            case 8: { int b = (bcla == MD_EMPTY ? -1 : bcla) + sens;
                      md_replayer_set_psg_arp_macro(ins, arp, narp,
                          b < 0 ? MD_EMPTY : borne(b, 0, narp ? narp - 1 : 0),
                          fixe);
                    } break;
            case 9: md_replayer_set_psg_arp_macro(ins, arp, narp, bcla, !fixe);
                     break;
            case 10: if (inCol < narp) {
                      arp[inCol] = (int8_t)borne(arp[inCol] + sens, -128, 127);
                      md_replayer_set_psg_arp_macro(ins, arp, narp, bcla, fixe);
                    } break;
            default: changeTable(ins, sens); break;   // 11 : la table, en bas
            }
          } else {
          if (inLigne == 18) {
            md_replayer_set_instr_gen_val(ins, MD_GEN_PROP_PANNING,
                  panDeplace(md_replayer_get_instr_gen_val(ins,
                                 MD_GEN_PROP_PANNING), sens));
          } else if (inLigne == 19) {
            changeTable(ins, sens);             // la table, en bas comme ailleurs
          } else if (inLigne >= 13) {
            // Les cinq reglages qui manquaient : la modulation d'amplitude et
            // de frequence du LFO, le LFO lui-meme et sa vitesse, le desaccord.
            // Ils existaient dans le moteur sans qu'aucun ecran ne les montre.
            static const int glob5[5] = {
              MD_GEN_PROP_AMS, MD_GEN_PROP_PMS, MD_GEN_PROP_LFO_ENABLE,
              MD_GEN_PROP_LFO_FREQ, MD_GEN_PROP_FINE_TUNE };
            static const int maxi5[5] = { 3, 7, 1, 7, 127 };
            const int k5 = inLigne - 13;
            md_replayer_set_instr_gen_val(ins, glob5[k5],
                borne(md_replayer_get_instr_gen_val(ins, glob5[k5])
                      + (k5 == 4 ? pas : sens), 0, maxi5[k5]));
          } else if (inLigne < 2) {
            static const int glob[2] = { MD_GEN_PROP_ALGORITHM, MD_GEN_PROP_FEEDBACK };
            int v = borne(md_replayer_get_instr_gen_val(ins, glob[inLigne]) + pas, 0, 7);
            md_replayer_set_instr_gen_val(ins, glob[inLigne], v);
          } else {
            static const int prop[11] = {
              MD_OP_PROP_MULTIPLE, MD_OP_PROP_DETUNE, MD_OP_PROP_TOTAL_LEVEL,
              MD_OP_PROP_ATTACK, MD_OP_PROP_DECAY, MD_OP_PROP_SUSTAIN_LEVEL,
              MD_OP_PROP_SUSTAIN_RATE, MD_OP_PROP_RELEASE, MD_OP_PROP_KEY_SCALE,
              MD_OP_PROP_AM, MD_OP_PROP_SSG_EG };
            static const int maxi[11] = { 15, 7, 127, 31, 31, 15, 31, 15, 3, 1, 15 };
            // Le Total Level du YM2612 est une ATTENUATION : 0 est le maximum
            // de niveau, 127 le silence. Affiche brut, le reglage marchait a
            // l'envers — a fond il ne restait rien. On l'inverse, comme le
            // fait l'iPad (inverted: true sur ce meme parametre).
        // Les deux ATTENUATIONS sont a l'envers sur la puce : zero est le
            // maximum, le maximum est le silence. Ce sont le Total Level et le
            // niveau de sustain (D1L). Sous un libelle qui dit « niveau », il
            // faut donc les retourner.
            //
            // Les quatre autres — attaque, decay, sustain rate, release — sont
            // des VITESSES : monter le nombre accelere. Ce n'est pas a l'envers,
            // mais « DECAY » tout court laissait croire a une duree. Elles
            // s'appellent maintenant RATE, ce qui leve l'ambiguite.
            //
            // L'iPad n'inverse que le Total Level et laisse le sustain a
            // l'envers : c'est un defaut de l'iPad, pas une regle a recopier.
            // ⚠️ LES QUATRE TEMPS DE L'ENVELOPPE SONT DES VITESSES DANS LA
            // PUCE, et se lisent comme des DUREES a l'ecran : monter la valeur
            // allonge le son. Le declin de maintien echappait a la regle — il
            // marchait donc a l'envers des trois autres. Il est desormais
            // inverse lui aussi, et s'appelle SUSTAIN DECAY.
            static const bool inverse[11] = { false, false, true, true, true,
                                             true, true, true, false,
                                             false, false };
            const int k = inLigne - 2;
            int brut = md_replayer_get_instr_op_val(ins, inCol, prop[k]);
            int vu = inverse[k] ? maxi[k] - brut : brut;
            vu = borne(vu + pas, 0, maxi[k]);
            md_replayer_set_instr_op_val(ins, inCol, prop[k],
                                         (uint8_t)(inverse[k] ? maxi[k] - vu : vu));
          }
          }
        } else if (page == PAGE_TABLE) {
          uint8_t vol,c1,v1,c2,v2,mc,mv; int8_t tsp;
          md_replayer_get_table_row(tableId, tabLigne, &vol, &tsp,
                                    &c1, &v1, &c2, &v2, &mc, &mv);
          const int nbc = md_table_cmd_count();
          // Chaque arret ne modifie QUE ce qu'il montre. La liste des lettres
          // est bornee par le moteur : sans borne haute on posait un indice
          // inexistant, la case affichait « ? » et la commande ne faisait rien.
          switch (tabCol) {
            case 0: vol = (uint8_t)borne((int)vol + sens * (grand ? 16 : 1), 0, 255); break;
            case 1: tsp = (int8_t)borne((int)tsp + sens * (grand ? 12 : 1), -128, 127); break;
            case 2: { int n = (c1 == MD_EMPTY ? -1 : (int)c1) + sens;
                      if (n >= nbc) n = nbc - 1;
                      c1 = (n < 0) ? MD_EMPTY : (uint8_t)n; } break;
            case 3: v1 = (uint8_t)borne((int)v1 + sens * (grand ? 16 : 1), 0, 255); break;
            case 4: { int n = (c2 == MD_EMPTY ? -1 : (int)c2) + sens;
                      if (n >= nbc) n = nbc - 1;
                      c2 = (n < 0) ? MD_EMPTY : (uint8_t)n; } break;
            case 5: v2 = (uint8_t)borne((int)v2 + sens * (grand ? 16 : 1), 0, 255); break;
            case 6: { int n = (mc == MD_EMPTY ? -1 : (int)mc)
                              + sens * (grand ? 16 : 1);
                      mc = (n < 0) ? MD_EMPTY : (uint8_t)borne(n, 0, 255); } break;
            default: mv = (uint8_t)borne((int)mv + sens * (grand ? 16 : 1), 0, 255); break;
          }
          md_replayer_set_table_row(tableId, tabLigne, vol, tsp,
                                    c1, v1, c2, v2, mc, mv);
        } else if (page == PAGE_PHRASE) {
          {
            const uint8_t ph = (uint8_t)phraseId;
            uint8_t no,ins,vel,cmd,cv,mc,mv;
            md_replayer_get_phrase(ph, phLigne, &no,&ins,&vel,&cmd,&cv,&mc,&mv);
            switch (phCol) {
              case 0: { int n = (no && no != MD_EMPTY ? (int)no
                                 : noteDepart(voieCourante))
                                + sens * (grand ? 12 : 1);
                        no = (uint8_t)borne(n, 1, MD_MAX_NOTE);
                        derniereNote[voieCourante] = no;
                        if (!ins) ins = 1; } break;
              case 1: ins = (uint8_t)borne((int)ins + sens * (grand ? 16 : 1), 1, 255); break;
              case 2: vel = (uint8_t)borne((int)(vel ? vel : 127) + sens * (grand ? 16 : 1), 0, 127); break;
              // 3 = la LETTRE de commande, 4 = sa valeur ; 5 = le CODE
              // machine, 6 = sa valeur. Chacun sous son propre curseur.
              // La liste des commandes est BORNEE par le moteur. Sans borne
              // haute on depassait la derniere lettre et on posait un indice
              // qui n'existe pas : la case affichait « ? » et la commande ne
              // faisait rien. C'est ce qui donnait « aucune commande ne
              // marche » — le moteur, lui, les execute toutes.
              case 3: { const int nb = md_table_cmd_count();
                        int c2 = (cmd == MD_EMPTY ? -1 : (int)cmd) + sens;
                        if (c2 >= nb) c2 = nb - 1;
                        cmd = (c2 < 0) ? MD_EMPTY : (uint8_t)c2; } break;
              case 4: cv = (uint8_t)borne((int)cv + sens * (grand ? 16 : 1), 0, 255); break;
              // Le CODE machine, sur sa propre colonne. Il etait absent du
              // switch : le « default » l'attrapait et modifiait la VALEUR.
              // Curseur sur les deux premiers chiffres, ce sont les deux
              // derniers qui bougeaient.
              case 5: { int m2 = (mc == MD_EMPTY ? -1 : (int)mc)
                                 + sens * (grand ? 16 : 1);
                        mc = (m2 < 0) ? MD_EMPTY : (uint8_t)borne(m2, 0, 255); } break;
              default: mv = (uint8_t)borne((int)mv + sens * (grand ? 16 : 1), 0, 255); break;
            }
            md_replayer_set_phrase(ph, phLigne, no,ins,vel,cmd,cv,mc,mv);
          }
        }
      // Le curseur BUTE aux extremites, il n'enroule pas.
      //
      // J'avais mis un modulo partout : arrive en ligne 00, une pression vers
      // le haut renvoyait en FF, a l'autre bout de la chanson. Le modele iPad
      // borne partout (max(0, min(n-1, x))) et ne fait jamais ca.
      } else if (page == PAGE_SONG) {
        if (appui & KEY_LEFT)  curCanal = borne(curCanal - 1, 0, nbVisibles - 1);
        if (appui & KEY_RIGHT) curCanal = borne(curCanal + 1, 0, nbVisibles - 1);
        if (appui & KEY_UP)    curLigne = borne(curLigne - 1, 0, MD_SONG_ROWS - 1);
        if (appui & KEY_DOWN)  curLigne = borne(curLigne + 1, 0, MD_SONG_ROWS - 1);
      } else if (page == PAGE_CHAIN) {
        if (appui & KEY_UP)    chLigne = borne(chLigne - 1, 0, MD_ROWS_PER_CHAIN - 1);
        if (appui & KEY_DOWN)  chLigne = borne(chLigne + 1, 0, MD_ROWS_PER_CHAIN - 1);
        if (appui & KEY_LEFT)  chCol = 0;   // deux colonnes : phrase, transpose
        if (appui & KEY_RIGHT) chCol = 1;
      } else if (page == PAGE_PHRASE) {
        if (appui & KEY_UP)    phLigne = borne(phLigne - 1, 0, MD_ROWS_PER_PHRASE - 1);
        if (appui & KEY_DOWN)  phLigne = borne(phLigne + 1, 0, MD_ROWS_PER_PHRASE - 1);
        // SEPT positions, pas six : note, instrument, velocite, lettre de
        // commande, valeur de commande, CODE machine, valeur machine. La borne
        // etait a 5 : les deux derniers chiffres de MD CMD etaient
        // INATTEIGNABLES, alors qu'ils sont dessines.
        if (appui & KEY_LEFT)  phCol = borne(phCol - 1, 0, 6);
        if (appui & KEY_RIGHT) phCol = borne(phCol + 1, 0, 6);
      } else if (page == PAGE_TABLE) {
        if (appui & KEY_UP)    tabLigne = borne(tabLigne - 1, 0, MD_TABLE_ROWS - 1);
        if (appui & KEY_DOWN)  tabLigne = borne(tabLigne + 1, 0, MD_TABLE_ROWS - 1);
        if (appui & KEY_LEFT)  tabCol = borne(tabCol - 1, 0, 7);
        if (appui & KEY_RIGHT) tabCol = borne(tabCol + 1, 0, 7);
      } else if (page == PAGE_INSTR) {
        // Le nombre de champs depend de la page : onze en FM (deux globaux
        // puis neuf par operateur), sept en PSG, huit sur le bruit qui a en
        // plus son mode, quatre en PCM.
        const int g = genreInstr(instrVoie, instrId);
        // Un champ de plus en PSG et sur le bruit : l'enveloppe, en tete.
        // PSG : onze champs ; bruit : douze, avec son mode. Les deux ont
        // desormais table, sortie et desaccord, comme sur l'iPad.
        // Un champ de plus PARTOUT : la table, tout en bas. Elle etait absente
        // des pages FM et PCM, et coincee en haut sur les pages PSG.
        // La SORTIE (L C R) existait sur PSG et sur le bruit, mais pas en FM
        // ni en PCM : ces deux voies-la ne pouvaient donc pas etre placees
        // dans le stereo, alors que la puce le permet sur toutes.
        // Ordre canonique de la page FM, TABLE EN DERNIER :
        //   0 ALGORITHM  1 FEEDBACK  2..12 les onze parametres d'operateur
        //   13 AM SENS  14 PM SENS  15 LFO  16 LFO SPEED  17 FINETUNE
        //   18 OUTPUT   19 TABLE
        // L'ecran du haut n'a que dix-neuf rangees et la grille les remplit :
        // tout ce qui vient apres se dessine sur l'ecran du BAS.
        const int nbChamps = (g == GENRE_FM) ? 20 : (g == GENRE_PSG) ? 11
                           : (g == GENRE_BRUIT) ? 12 : 7;
        if (appui & KEY_UP)    inLigne = borne(inLigne - 1, 0, nbChamps - 1);
        if (appui & KEY_DOWN)  inLigne = borne(inLigne + 1, 0, nbChamps - 1);
        // Les colonnes ne servent que la ou il y a plusieurs valeurs de front :
        // les quatre operateurs en FM, les seize pas d'une macro ailleurs.
        int nbCol = 0;
        if (g == GENRE_FM && inLigne >= 2 && inLigne < 13) nbCol = 4;
        else if (g == GENRE_PSG || g == GENRE_BRUIT) {
          // Meme numerotation commune que l'affichage et l'edition.
          const int canon = (g == GENRE_BRUIT || inLigne < 3) ? inLigne
                                                              : inLigne + 1;
          if (canon == 0) nbCol = 3;                        // les trois points
          else if (canon == 6 || canon == 10) nbCol = 16;   // les pas d'une macro
        }
        if (nbCol > 0) {
          if (appui & KEY_LEFT)  inCol = borne(inCol - 1, 0, nbCol - 1);
          if (appui & KEY_RIGHT) inCol = borne(inCol + 1, 0, nbCol - 1);
        } else inCol = 0;
      } else if (page == PAGE_PROJECT) {
        // La fenetre de nom prend la main sur toute la page.
        if (dialogueNom) {
          static const char *grilleT[4] = { "0123456789", "ABCDEFGHIJ",
                                            "KLMNOPQRST", "UVWXYZ-_<*" };
          if (appui & KEY_LEFT)  nomCol = borne(nomCol - 1, 0, 9);
          if (appui & KEY_RIGHT) nomCol = borne(nomCol + 1, 0, 9);
          if (appui & KEY_UP)    nomLig = borne(nomLig - 1, 0, 3);
          if (appui & KEY_DOWN)  nomLig = borne(nomLig + 1, 0, 3);
          if (frappe & KEY_A) {
            const char c = grilleT[nomLig][nomCol];
            const int n = (int)strlen(sauveNom);
            if (c == '<') { if (n > 0) sauveNom[n - 1] = 0; }
            else if (c == '*') {
              dialogueNom = false; pageVue = -1;
              if (nomPour == NOM_ROM) {
                enLecture = false; msgProjet[0] = 0;
                exporteRom(sauveNom);
                msgProjetJusqu = secondes + 4;
              } else if (nomPour == NOM_VGM) {
                enLecture = false; msgProjet[0] = 0;
                exporteVgm(sauveNom);
                msgProjetJusqu = secondes + 4;
              } else {
                char e[90];
                // Un message a l'ecran, pas seulement dans le journal : sans
                // retour visible, on ne sait pas si l'enregistrement a eu lieu.
                causeTour = "PROJET";
                if (enregistre(sauveNom)) {
                  siprintf(e, "ENREGISTRE : %s", dernierNom);
                  siprintf(msgProjet, "SAVED %s", nomProjet);
                } else {
                  siprintf(e, "ECHEC DE L'ENREGISTREMENT");
                  strcpy(msgProjet, "SAVE FAILED");
                }
                journal(e);
                msgProjetJusqu = secondes + 3;   // trois secondes a l'ecran
              }
            } else if (n < 8) { sauveNom[n] = c; sauveNom[n + 1] = 0; }
          }
          if (frappe & KEY_B) { dialogueNom = false; pageVue = -1; }
        }
        // Le navigateur de fichiers prend la main sur toute la page.
        else if (navigateur) {
          if (appui & KEY_UP)   selFichier = borne(selFichier - 1, 0, nbFichiers ? nbFichiers - 1 : 0);
          if (appui & KEY_DOWN) selFichier = borne(selFichier + 1, 0, nbFichiers ? nbFichiers - 1 : 0);
          if (frappe & KEY_B) {
            if (navGenre == 3) {
              // On remonte a la liste des fichiers, pas hors du navigateur :
              // s'etre trompe de morceau ne doit pas obliger a tout refaire.
              if (sauveImg) { free(sauveImg); sauveImg = 0; sauveLg = 0; }
              navEntete[0] = 0; navGenre = 2; scanne();
            } else { navigateur = false; page = pageRetour; pageVue = -1; }
          }
          if ((frappe & KEY_A) && nbFichiers > 0 && typeFic[selFichier] == 2) {
            entre(fichiers[selFichier]);
          }
          // ── UNE ROM GENETRACKER : ON EN TIRE LE PROJET ──────────────
          // ⚠️ On la lit ENTIEREMENT en memoire — un demi-megaoctet, la DS en
          // a quatre. La lire par morceaux obligerait a suivre le plan a
          // travers des lectures dispersees, pour rien.
          else if ((frappe & KEY_A) && nbFichiers > 0
                   && typeFic[selFichier] == 1 && navGenre == 2) {
            char chemin[224];
            const int Ld = (int)strlen(dossier);
            siprintf(chemin, "%s%s%s", dossier,
                     (Ld && dossier[Ld-1] == '/') ? "" : "/",
                     fichiers[selFichier]);
            causeTour = "IMPORT ROM";
            char e[130];
            bool resteNav = false;   // vrai quand on passe a la liste des morceaux
            FILE *fr = fopen(chemin, "rb");
            if (!fr) { strcpy(msgProjet, "ROM UNREADABLE"); }
            else {
              fseek(fr, 0, SEEK_END);
              const long lg = ftell(fr);
              fseek(fr, 0, SEEK_SET);
              uint8_t *rom = (lg > 0 && lg <= 4*1024*1024)
                           ? (uint8_t *)malloc((size_t)lg) : 0;
              if (rom && fread(rom, 1, (size_t)lg, fr) == (size_t)lg) {
                md_rom_plan_t plan;
                // ── LA ROM N'EST LA QUE POUR SES SONS ──────────────────
                // On vient d'importer un morceau depuis une sauvegarde et il
                // lui manque ses echantillons. Verser le morceau de la ROM
                // par-dessus effacerait justement ce qu'on vient de
                // recuperer : on ne prend que les sons.
                if (romPour == 1 && md_rom_plan_lit(rom, (uint32_t)lg, &plan)) {
                  const int n =
                      md_rom_echantillons_seuls(rom, &plan, MD_MAX_INSTRUMENTS);
                  romPour = 0;
                  modifie = true;
                  if (n > 0) siprintf(msgProjet, "%d SAMPLES TAKEN FROM ROM", n);
                  else       strcpy(msgProjet, "THIS ROM HAS NONE OF THEM");
                  siprintf(e, "ECHANTILLONS DEPUIS ROM : %d", n);
                } else if (!md_rom_plan_lit(rom, (uint32_t)lg, &plan)) {
                  // ── PAS UNE ROM. ALORS UNE SAUVEGARDE ? ────────────────
                  // ⚠️ CE CAS-LA N'EST PAS UN REPLI, C'EST LE CAS COURANT.
                  // La ROM ne contient que ce qu'on y a grave depuis un
                  // ordinateur ; tout ce qu'on compose SUR LA CONSOLE va dans
                  // la sauvegarde, que l'EverDrive recopie dans EDMD/SAVE.
                  // Les deux fichiers finissent en .bin, et refuser le second
                  // revenait a refuser precisement le travail qu'on cherchait
                  // a rapatrier.
                  static char noms[MD_SAUVE_MAX][11];
                  const int ns = md_sauve_lit(rom, (uint32_t)lg, noms, sauveRangs);
                  if (ns > 0) {
                    // La liste du navigateur devient la liste des morceaux.
                    if (sauveImg) free(sauveImg);
                    sauveImg = rom; sauveLg = (uint32_t)lg; rom = 0;
                    for (int k = 0; k < ns; k++) {
                      siprintf(fichiers[k], "%02d  %s", sauveRangs[k], noms[k]);
                      typeFic[k] = 1;
                    }
                    nbFichiers = ns; selFichier = 0; hautFichier = 0;
                    navGenre = 3; resteNav = true;
                    siprintf(navEntete, "SAVE : %d SONG(S)", ns);
                    siprintf(e, "SAUVEGARDE : %d morceau(x)", ns);
                  } else if (ns == 0) {
                    // C'EST une sauvegarde, mais rien n'y a ete enregistre :
                    // dire « ce n'est pas du GeneTracker » enverrait chercher
                    // un autre fichier alors qu'il faut retourner enregistrer
                    // sur la console.
                    strcpy(msgProjet, "SAVE HOLDS NO SONG");
                    siprintf(e, "IMPORT : SAUVEGARDE VIDE");
                  } else {
                    strcpy(msgProjet, "NOT A GENETRACKER ROM OR SAVE");
                    siprintf(e, "IMPORT : NI PLAN NI GTLIB1 DANS %s",
                             fichiers[selFichier]);
                  }
                } else {
                  const int n = md_rom_morceaux(rom, &plan);
                  // ⚠️ Le premier morceau, et on le DIT quand il y en a
                  // plusieurs : choisir sans le savoir serait pire que tout.
                  if (n <= 0) { strcpy(msgProjet, "ROM HAS NO SONG");
                                siprintf(e, "IMPORT : AUCUN MORCEAU"); }
                  else {
                    md_replayer_stop(); enLecture = false;
                    if (md_rom_projet_importe(rom, &plan, 0)) {
                      char nomM[11]; md_rom_nom(rom, &plan, 0, nomM);
                      // ⚠️ LE NOM PREND UN SUFFIXE « MD », ET C'EST VOULU.
                      // Le meme morceau existe presque toujours des deux
                      // cotes : sans le suffixe, enregistrer la version qui
                      // vient de la cartouche ecraserait la version DS, qui
                      // porte le meme nom : « X » devient donc « XMD ».
                      // On coupe a six caracteres pour que les deux lettres
                      // tiennent dans les huit d'un nom de fichier.
                      { int k = 0;
                        while (k < 6 && nomM[k]) { nomProjet[k] = nomM[k]; k++; }
                        nomProjet[k++] = 'M'; nomProjet[k++] = 'D';
                        nomProjet[k] = 0; }
                      modifie = true;
                      // On annonce le nom SOUS LEQUEL il s'enregistrera, pas
                      // celui qu'il portait dans la ROM : c'est celui-la que
                      // l'utilisateur va retrouver.
                      if (n > 1) siprintf(msgProjet, "IMPORTED AS %s (1/%d)", nomProjet, n);
                      else       siprintf(msgProjet, "IMPORTED AS %s", nomProjet);
                      siprintf(e, "IMPORT ROM : %s, %d morceau(x)", nomM, n);
                    } else { strcpy(msgProjet, "IMPORT FAILED");
                             siprintf(e, "IMPORT : ECHEC"); }
                  }
                }
              } else { strcpy(msgProjet, "ROM UNREADABLE");
                       siprintf(e, "IMPORT : LECTURE IMPOSSIBLE"); }
              if (rom) free(rom);
              fclose(fr);
              journal(e);
            }
            msgProjetJusqu = secondes + 4;
            if (!resteNav) { navigateur = false; page = pageRetour; pageVue = -1; }
          }
          // ── UN MORCEAU CHOISI DANS LA SAUVEGARDE ───────────────────────
          else if ((frappe & KEY_A) && nbFichiers > 0 && navGenre == 3
                   && sauveImg) {
            causeTour = "IMPORT SAVE";
            char e[130];
            const int rang = sauveRangs[selFichier];
            md_replayer_stop(); enLecture = false;
            if (md_sauve_importe(sauveImg, sauveLg, rang)) {
              // Le nom est derriere « NN  » dans l'entree affichee.
              const char *nomM = fichiers[selFichier] + 4;
              // Meme suffixe « MD » qu'a l'import de ROM, et pour la meme
              // raison : le morceau existe des deux cotes sous le meme nom.
              { int k = 0;
                while (k < 6 && nomM[k]) { nomProjet[k] = nomM[k]; k++; }
                nomProjet[k++] = 'M'; nomProjet[k++] = 'D';
                nomProjet[k] = 0; }
              modifie = true;
              // ⚠️ ON LE DIT QUAND LA BANQUE EST VIDE. Une sauvegarde ne porte
              // pas les echantillons : ils sont dans la ROM. Les instruments
              // gardent leur numero d'echantillon, qui ne designe rien tant
              // qu'on n'a pas importe la ROM ou charge les WAV.
              siprintf(msgProjet, "IMPORTED AS %s", nomProjet);
              siprintf(e, "IMPORT SAUVEGARDE : rang %d, %s", rang, nomProjet);
              // ⚠️ ON DEMANDE LA ROM TOUT DE SUITE. Le morceau est complet,
              // mais ses echantillons sont restes dans la cartouche : sans
              // cette question on repart avec une voie PCM muette sans savoir
              // qu'il manque quelque chose.
              manqueEchantillons = md_echantillons_manquants(MD_MAX_INSTRUMENTS);
              if (manqueEchantillons > 0) demandeEchantillons = true;
            } else { strcpy(msgProjet, "IMPORT FAILED");
                     siprintf(e, "IMPORT SAUVEGARDE : ECHEC rang %d", rang); }
            journal(e);
            free(sauveImg); sauveImg = 0; sauveLg = 0;
            navEntete[0] = 0; navGenre = 2;
            msgProjetJusqu = secondes + 4;
            navigateur = false; page = pageRetour; pageVue = -1;
          }
          else if ((frappe & KEY_A) && nbFichiers > 0
                   && typeFic[selFichier] == 1 && navGenre == 1) {
            // Un echantillon : on le convertit, on le range dans la banque du
            // morceau, et on l'attache a l'instrument en cours d'edition.
            char chemin[224];
            const int Ld = (int)strlen(dossier);
            siprintf(chemin, "%s%s%s", dossier,
                     (Ld && dossier[Ld-1] == '/') ? "" : "/",
                     fichiers[selFichier]);
            uint32_t lg = 0;
            causeTour = "SAMPLE";
            uint8_t *pcm = md_audio_charge_wav(chemin, &lg);
            char e[110];
            if (pcm && lg) {
              // C-4 par defaut, la note a laquelle l'echantillon joue a sa
              // vitesse naturelle. 60 valait B-4 : tout partait un demi-ton
              // trop haut et d'une octave decalee.
              const int idx = md_replayer_add_sample(fichiers[selFichier],
                                                     pcm, lg, -1, 49);
              if (idx >= 0) {
                md_replayer_set_instr_sample(instrId, idx);
                md_replayer_set_instr_kind(instrId, MD_INSTR_KIND_PCM);
                siprintf(e, "ECHANTILLON %s CHARGE : %lu OCTETS",
                         fichiers[selFichier], (unsigned long)lg);
                strcpy(msgPCM, "LOADED");
              } else { siprintf(e, "BANQUE PLEINE : %s", fichiers[selFichier]);
                       strcpy(msgPCM, "BANK FULL"); }
            } else { siprintf(e, "WAV ILLISIBLE : %s", fichiers[selFichier]);
                     strcpy(msgPCM, "BAD WAV"); }
            journal(e);
            if (pcm) free(pcm);
            // On revient a la page d'ou l'on venait — la page instrument —
            // et non sur la liste des fichiers : le choix est fait.
            navigateur = false; page = pageRetour; pageVue = -1;
          }
          else if ((frappe & KEY_A) && nbFichiers > 0
                   && typeFic[selFichier] == 1) {
            md_replayer_stop(); enLecture = false;
            { char e[80]; siprintf(e, "OUVERTURE %s", fichiers[selFichier]);
              journal(e); }
            causeTour = "PROJET";
            if (ouvre(fichiers[selFichier])) {
              // Le morceau ouvert donne son nom au projet : c'est sous ce
              // nom-la qu'on voudra le reenregistrer.
              { const char *n = fichiers[selFichier]; int j = 0;
                for (; j < 15 && n[j] && n[j] != '.'; j++) {
                  char c2 = n[j];
                  if (c2 >= 'a' && c2 <= 'z') c2 = (char)(c2 - 'a' + 'A');
                  nomProjet[j] = c2;
                }
                nomProjet[j] = 0;
                strncpy(sauveNom, nomProjet, 8); sauveNom[8] = 0; }
              curLigne = 0; haut = 0; curCanal = 0;
              chainId = 0; phraseId = 0; instrId = 1;
              chLigne = chCol = phLigne = phCol = inLigne = inCol = 0;
              selActive = false; clipPage = -1;
              appliqueVoiesSupp(false);
              // Meme regle qu'a l'ouverture de la demo : si le morceau se sert
              // des voies supplementaires, on le signale et on demande.
              if (morceauUtiliseVoiesSupp()) demandeVoies = true;
            }
            navigateur = false; page = pageRetour; pageVue = -1;
          }
        }
        // Tant qu'une question est posee, A et B lui repondent.
        else if (demandeNouveau) {
          if (frappe & KEY_A) {
            md_replayer_stop(); enLecture = false;
            md_replayer_new_empty();
            md_replayer_set_bpm(125.0);
            appliqueVoiesSupp(false);
            curLigne = 0; haut = 0; curCanal = 0;
            chainId = 0; phraseId = 0; instrId = 1;
            chLigne = chCol = phLigne = phCol = inLigne = inCol = 0;
            selActive = false; clipPage = -1;
            demandeNouveau = false; pageVue = -1;
            journal("NOUVEAU MORCEAU");
          }
          if (frappe & KEY_B) { demandeNouveau = false; pageVue = -1; }
        }
        else if (demandeEchantillons) {
          if (frappe & KEY_A) {
            // On ouvre le navigateur sur les ROMs, en mode « echantillons
            // seuls » : la ROM choisie ne remplacera pas le morceau importe.
            demandeEchantillons = false;
            romPour = 1;
            navGenre = 2; scanne(); navigateur = true;
            pageRetour = PAGE_PROJECT; pageVue = -1;
          }
          if (frappe & KEY_B) { demandeEchantillons = false; pageVue = -1;
                                journal("ECHANTILLONS : REMIS A PLUS TARD"); }
        }
        else if (demandeVoies) {
          if (frappe & KEY_A) { appliqueVoiesSupp(true);  demandeVoies = false;
                                journal("VOIES FM5 PS2 PS3 ACTIVEES PAR L'UTILISATEUR"); }
          if (frappe & KEY_B) { appliqueVoiesSupp(false); demandeVoies = false;
                                journal("VOIES FM5 PS2 PS3 LAISSEES ETEINTES"); }
        }
        // ── Le menu ────────────────────────────────────────────────────
        // Haut/bas pour choisir la ligne, A pour l'actionner. Sur la ligne du
        // tempo, A ne fait rien tout seul : c'est A + gauche/droite qui change
        // la valeur, et A seul ne doit donc pas declencher d'action.
        else {
          if (appui & KEY_UP)   menuProjet = borne(menuProjet - 1, 0, 8);
          if (appui & KEY_DOWN) menuProjet = borne(menuProjet + 1, 0, 8);

          if (menuProjet == 0) {
            if ((keysHeld() & KEY_A) && (appui & (KEY_LEFT | KEY_RIGHT))) {
              // ── On avance jusqu'au PROCHAIN tempo REELLEMENT atteignable ──
              // Le moteur regle le BPM par le finetune, qui est un entier :
              // pres de 125 BPM une unite vaut deux BPM et demi. Demander
              // « un de plus » retombait donc dans le meme cran et ne changeait
              // rien — le tempo semblait bloque entre 123 et 127.
              //
              // On demande donc des valeurs croissantes jusqu'a ce que le
              // moteur en rende une differente : chaque pression deplace d'un
              // cran, quel qu'en soit l'ecart.
              const int d = (appui & KEY_RIGHT) ? 1 : -1;
              const double avant = md_replayer_get_bpm();
              for (int essai = 1; essai <= 40; essai++) {
                const int vise = borne((int)(avant + 0.5) + d * essai, 32, 255);
                md_replayer_set_bpm((double)vise);
                const double apres = md_replayer_get_bpm();
                const double ecart = apres > avant ? apres - avant : avant - apres;
                if (ecart > 0.4) break;
                // Bute sur une borne : inutile d'insister.
                if (vise <= 32 || vise >= 255) break;
              }
            }
          } else if (frappe & KEY_A) {
            if (menuProjet == 1) {
              // La fenetre s'ouvre sur le nom du projet courant.
              strncpy(sauveNom, nomProjet, 8); sauveNom[8] = 0;
              // Le curseur se pose d'emblee sur OK : reenregistrer un projet
              // qui a deja son nom se fait alors en deux touches.
              nomPour = NOM_PROJET;
              dialogueNom = true; nomLig = 3; nomCol = 9;
              assombrirDemande = true;
            } else if (menuProjet == 2) {
              if (carteOK) { creeSiAbsent(dossierSongs);
                             strcpy(dossier, dossierSongs);
                             causeTour = "FICHIERS";
                             navGenre = 0; scanne(); navigateur = true;
                             pageRetour = PAGE_PROJECT; pageVue = -1; }
            } else if (menuProjet == 3) {
              demandeNouveau = true; pageVue = -1;
            } else if (menuProjet == 5) {
              // On NOMME d'abord : la meme fenetre que pour enregistrer un
              // projet, avec le nom du projet propose par defaut.
              strncpy(sauveNom, nomProjet, 8); sauveNom[8] = 0;
              nomPour = NOM_VGM;
              dialogueNom = true; nomLig = 3; nomCol = 9;
              assombrirDemande = true; pageVue = -1;
            } else if (menuProjet == 4) {
              strncpy(sauveNom, nomProjet, 8); sauveNom[8] = 0;
              nomPour = NOM_ROM;
              dialogueNom = true; nomLig = 3; nomCol = 9;
              assombrirDemande = true; pageVue = -1;
            } else if (menuProjet == 6) {
              // La ROM GeneTracker : on la NOMME d'abord, comme les autres.
              strncpy(sauveNom, nomProjet, 8); sauveNom[8] = 0;
              nomPour = NOM_ROM_TRK;
              dialogueNom = true; nomLig = 3; nomCol = 9;
              assombrirDemande = true; pageVue = -1;
            } else if (menuProjet == 8) {
              page = PAGE_APROPOS; pageVue = -1;
            } else if (menuProjet == 7) {
              // Importer : on choisit une ROM sur la carte.
              if (carteOK) { creeSiAbsent(dossierRoms);
                             strcpy(dossier, dossierRoms);
                             causeTour = "ROMS";
                             // Entree par le MENU : on veut le projet entier,
                             // pas seulement les sons.
                             romPour = 0;
                             navGenre = 2; scanne(); navigateur = true;
                             pageRetour = PAGE_PROJECT; pageVue = -1; }
            }
          }
        }
      }
      // ── A seul : poser une valeur ─────────────────────────────────────
      // A avec B tenu ne pose RIEN : cet accord-la efface, plus bas. Sans ce
      // garde, A posait une valeur que l'effacement retirait aussitot — et sur
      // la colonne des notes il la faisait entendre au passage.
      if ((frappe & KEY_A) && page != PAGE_PROJECT && !(keysHeld() & KEY_B)) {
        // inLigne entre dans la cle : sans lui, tous les champs d'une page
        // d'instrument partageaient le meme repere et deux A sur deux reglages
        // differents passaient pour un double-A.
        const int cle = page * 100000 + curCanal * 10000 + curLigne * 20
                        + chLigne + phLigne * 3 + phCol + inLigne * 7;
        const unsigned t = timerTick(2);
        const bool doubleA = (cle == derA) && ((unsigned short)(t - derAt) < 16000);
        derA = cle; derAt = t;

        // ── Le champ TABLE d'un instrument ──────────────────────────────
        // Reprise mot pour mot des regles de l'iPad :
        //   double-A          une table VIDE, la premiere libre
        //   A, champ sur OFF  la derniere table employee revient
        //   A, table posee    on la retient, pour ce rappel-la
        if (page == PAGE_INSTR) {
          const int g3 = genreInstr(instrVoie, instrId);
          const int champTable = (g3 == GENRE_PCM) ? 6
                               : (g3 == GENRE_PSG) ? 10
                               : (g3 == GENRE_FM)  ? 19 : 11;
          if (inLigne == champTable) {
            const int t3 = md_replayer_get_instr_table(instrId);
            const bool aucune = (t3 < 0 || t3 == MD_EMPTY);
            if (doubleA) {
              const int neuf = tableLibre();
              md_replayer_set_instr_table(instrId, neuf);
              derniereTable = neuf;
            } else if (aucune) {
              md_replayer_set_instr_table(instrId, derniereTable);
            } else {
              derniereTable = t3;
            }
          }
        }

        if (page == PAGE_SONG) {
          uint8_t v = md_replayer_get_song(reel(curCanal), curLigne);
          if (doubleA || v == MD_EMPTY) {
            int id = doubleA ? chainLibre() : dernierChain;
            md_replayer_set_song(reel(curCanal), curLigne, (uint8_t)id); dernierChain = id;
          } else dernierChain = v;
        } else if (page == PAGE_CHAIN && chCol == 0) {
          {
            const uint8_t noChain = (uint8_t)chainId;
            uint8_t ph; int8_t tsp; md_replayer_get_chain(noChain, chLigne, &ph, &tsp);
            if (doubleA || ph == MD_EMPTY) {
              int id = doubleA ? phraseLibre() : dernierePhrase;
              md_replayer_set_chain(noChain, chLigne, (uint8_t)id, tsp);
              dernierePhrase = id;
            } else dernierePhrase = ph;
          }
        } else if (page == PAGE_PHRASE) {
          {
            const uint8_t ph = (uint8_t)phraseId;
            uint8_t no,ins,vel,cmd,cv,mc,mv;
            md_replayer_get_phrase(ph, phLigne, &no,&ins,&vel,&cmd,&cv,&mc,&mv);
            switch (phCol) {
              case 0: if (no && no != MD_EMPTY) derniereNote[voieCourante] = no;
                      else {
                        no = (uint8_t)noteDepart(voieCourante);
                        // La note prend l'instrument DU CANAL, pas le premier
                        // venu : c'est ce qui donne un timbre par colonne.
                        if (!ins) {
                          const int voie = reel(curCanal);
                          if (!instrCanal[voie]) {
                            const int ni = instrLibre();
                            md_replayer_init_default_instrument(ni - 1);
                            if (voie == MD_PCM_CHANNEL)
                              md_replayer_set_instr_kind(ni, MD_INSTR_KIND_PCM);
                            instrCanal[voie] = ni;
                          }
                          ins = (uint8_t)instrCanal[voie];
                        }
                      } break;
              case 1: {
                // Colonne INSTRUMENT, regles de l'iPad :
                //   double A  -> un instrument NEUF, initialise, retenu ;
                //   A simple  -> l'instrument de la case, ou celui du canal.
                const int voie = reel(curCanal);
                if (doubleA) {
                  const int ni = instrLibre();
                  md_replayer_init_default_instrument(ni - 1);
                  // Sur la colonne PCM, un instrument neuf est un ECHANTILLON,
                  // sinon il serait joue en synthese FM.
                  if (voie == MD_PCM_CHANNEL)
                    md_replayer_set_instr_kind(ni, MD_INSTR_KIND_PCM);
                  ins = (uint8_t)ni;
                  instrCanal[voie] = ni;
                  instrId = ni;
                } else if (ins) {
                  instrCanal[voie] = ins;
                  instrId = ins;
                } else {
                  if (!instrCanal[voie]) {
                    const int ni = instrLibre();
                    md_replayer_init_default_instrument(ni - 1);
                    if (voie == MD_PCM_CHANNEL)
                      md_replayer_set_instr_kind(ni, MD_INSTR_KIND_PCM);
                    instrCanal[voie] = ni;
                  }
                  ins = (uint8_t)instrCanal[voie];
                  instrId = ins;
                }
                break;
              }
              case 2: if (vel == MD_EMPTY) vel = 127; break;
              case 3: case 4: if (cmd == MD_EMPTY) cmd = 0; break;
              // Le CODE de la premiere commande machine, et non zero :
              // zero n'est pas forcement un code valide.
              default: if (mc == MD_EMPTY) mc = md_mdcmd_code(0); break;
            }
            md_replayer_set_phrase(ph, phLigne, no,ins,vel,cmd,cv,mc,mv);

            // On ENTEND la note qu'on vient de poser, comme sur l'iPad.
            // Uniquement a l'arret : une note d'audition par-dessus la lecture
            // volerait une voie et brouillerait ce qu'on ecoute.
            //
            // Sur LA VOIE DE LA COLONNE, et non sur la voie d'essai : celle-ci
            // est cablee sur FM6, si bien qu'une note posee sur une colonne
            // PSG ou sur le bruit s'auditionnait en synthese FM — on entendait
            // un son qui n'avait rien a voir avec ce qu'on ecrivait. La voie
            // reelle achemine vers la bonne puce. C'est sans risque puisqu'on
            // est a l'arret : elle ne joue rien d'autre a cet instant.
            if (phCol == 0 && no && no != MD_EMPTY && !enLecture) {
              voieAudition = voieCourante;
              md_replayer_play_test_note(no, ins ? ins : 1, voieAudition);
            }
          }
        }
      }

      // ── X : effacer ───────────────────────────────────────────────────
      // Comportement de LSDJ, repris de deleteCell sur l'iPad : sur une case
      // PLEINE on l'efface en laissant le trou ; sur une case VIDE on remonte
      // toute la colonne d'un cran, si bien qu'appuyer plusieurs fois au meme
      // endroit « aspire » la colonne vers le haut.
      // A+B efface, dans les deux ordres, exactement comme X — c'est ce que
      // fait l'iPad : B presse pendant qu'on tient A, ou A presse pendant
      // qu'on tient B. Le B de l'accord ne doit pas declencher de copie au
      // relachement, d'ou le drapeau.
      bool effacer = (frappe & KEY_X) != 0;
      if (((frappe & KEY_A) && (keysHeld() & KEY_B)) ||
          ((frappe & KEY_B) && (keysHeld() & KEY_A))) {
        effacer = true;
        bUtilise = true;
      }
      if (effacer && page != PAGE_PROJECT) {
        if (page == PAGE_INSTR) {
          // Effacer le champ TABLE, c'est le remettre sur OFF.
          const int g4 = genreInstr(instrVoie, instrId);
          const int champTable = (g4 == GENRE_PCM) ? 6
                               : (g4 == GENRE_PSG) ? 10
                               : (g4 == GENRE_FM)  ? 19 : 11;
          if (inLigne == champTable) md_replayer_set_instr_table(instrId, -1);
        } else if (page == PAGE_TABLE) {
          uint8_t vol,c1,v1,c2,v2,mc,mv; int8_t tsp;
          md_replayer_get_table_row(tableId, tabLigne, &vol, &tsp,
                                    &c1, &v1, &c2, &v2, &mc, &mv);
          // Effacer une lettre emmene sa valeur : les deux ne veulent rien
          // dire l'une sans l'autre.
          switch (tabCol) {
            case 0: vol = 0; break;              // 0 = « ne touche pas au volume »
            case 1: tsp = 0; break;              // 0 = « ne transpose pas »
            case 2: case 3: c1 = MD_EMPTY; v1 = 0;
                    md_replayer_clear_active_effects(voieCourante); break;
            case 4: case 5: c2 = MD_EMPTY; v2 = 0;
                    md_replayer_clear_active_effects(voieCourante); break;
            default: mc = MD_EMPTY; mv = 0; break;
          }
          md_replayer_set_table_row(tableId, tabLigne, vol, tsp,
                                    c1, v1, c2, v2, mc, mv);
        } else if (page == PAGE_SONG) {
          if (md_replayer_get_song(reel(curCanal), curLigne) == MD_EMPTY) {
            for (int r = curLigne; r < MD_SONG_ROWS - 1; r++)
              md_replayer_set_song(reel(curCanal), r, md_replayer_get_song(reel(curCanal), r + 1));
            md_replayer_set_song(reel(curCanal), MD_SONG_ROWS - 1, MD_EMPTY);
          } else md_replayer_set_song(reel(curCanal), curLigne, MD_EMPTY);
        } else if (page == PAGE_CHAIN) {
          uint8_t noChain = md_replayer_get_song(reel(curCanal), curLigne);
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
          {
            const uint8_t ph = (uint8_t)phraseId;
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
                // Effacer la NOTE emporte l'instrument : les deux ne veulent
                // rien dire l'un sans l'autre. Une case laissee avec un
                // instrument mais sans note ne joue rien, et la ligne semblait
                // pourtant occupee.
                case 0: no = 0; ins = 0; break;
                case 1: ins = 0; break;
                case 2: vel = MD_EMPTY; break;   // vide, pas volume nul
                // Effacer une commande doit S'ENTENDRE tout de suite. Un
                // effet continu tourne jusqu'a ce qu'on le reecrive a 00 ;
                // l'effacement laisse 0xFF, que le moteur ignore, si bien que
                // l'effet survivait jusqu'a l'arret de la lecture.
                case 3: case 4: cmd = MD_EMPTY; cv = 0;
                                md_replayer_clear_active_effects(voieCourante);
                                break;
                default: mc = MD_EMPTY; mv = 0;
                         md_replayer_clear_active_effects(voieCourante);
                         break;
              }
              md_replayer_set_phrase(ph, phLigne, no,ins,vel,cmd,cv,mc,mv);
            }
          }
        }
      }

      // A seul sur la page TABLE : la case vide prend une valeur de depart,
      // comme dans une phrase. Sur une lettre de commande, la premiere de la
      // liste ; ailleurs, zero suffit puisque zero y est deja le neutre.
      if ((frappe & KEY_A) && page == PAGE_TABLE && !(keysHeld() & KEY_B) &&
          !selTenu) {
        uint8_t vol,c1,v1,c2,v2,mc,mv; int8_t tsp;
        md_replayer_get_table_row(tableId, tabLigne, &vol, &tsp,
                                  &c1, &v1, &c2, &v2, &mc, &mv);
        // Une case de commande VIDE rappelle la DERNIERE posee, avec sa
        // valeur — comme sur l'iPad. Repartir de la premiere lettre obligeait a
        // traverser toute la liste, et on declenchait au passage un changement
        // de table ou de tempo.
        if (tabCol == 2 || tabCol == 3) {
          if (c1 == MD_EMPTY) { c1 = derCmd; v1 = derCmdVal; }
          else { derCmd = c1; derCmdVal = v1; }
        } else if (tabCol == 4 || tabCol == 5) {
          if (c2 == MD_EMPTY) { c2 = derCmd; v2 = derCmdVal; }
          else { derCmd = c2; derCmdVal = v2; }
        } else if (tabCol >= 6) {
          if (mc == MD_EMPTY) { mc = derMdCmd; mv = derMdVal; }
          else { derMdCmd = mc; derMdVal = mv; }
        }
        md_replayer_set_table_row(tableId, tabLigne, vol, tsp,
                                  c1, v1, c2, v2, mc, mv);
      }

      // A sur « LOAD SAMPLE » ouvre le navigateur, regle sur les WAV.
      if ((frappe & KEY_A) && page == PAGE_INSTR && !(keysHeld() & KEY_B) &&
          genreInstr(instrVoie, instrId) == GENRE_PCM && inLigne == 0 &&
          carteOK) {
        // Le navigateur s'ouvre dans le dossier des ECHANTILLONS, a cote de la
        // cartouche, et non la ou l'on cherchait un morceau la derniere fois.
        creeSiAbsent(dossierSamples);
        strcpy(dossier, dossierSamples);
        navGenre = 1; scanne(); navigateur = true;
        pageRetour = PAGE_INSTR;
        page = PAGE_PROJECT; pageVue = -1;
      }

      // Une touche d'edition a-t-elle ete frappee ? Si oui, la page SONG doit
      // se redessiner meme si le curseur n'a pas bouge.
      if (frappe & (KEY_A | KEY_X)) modifie = true;
      if (aTenu && (appui & (KEY_UP|KEY_DOWN|KEY_LEFT|KEY_RIGHT))) modifie = true;

      // START lance et arrete la lecture, depuis n'importe quelle page.
      if (frappe & KEY_START) {
        { char e[110];
          siprintf(e, "T%04u  %s  PAGE %c  LIGNE %02X", (unsigned)secondes,
                   enLecture ? "STOP" : "PLAY", "SCPIJT"[page], curLigne);
          journal(e);
          if (!enLecture) {
            // Ce que le SONG contient sur la ligne de depart, voie par voie.
            char l[120]; int n = 0;
            n += siprintf(l + n, "  SONG:");
            for (int c = 0; c < 10; c++) {
              uint8_t v = md_replayer_get_song(c, curLigne);
              n += siprintf(l + n, " %s=%s%02X", noms[c],
                            md_replayer_is_channel_disabled(c) ? "x" : "",
                            v == MD_EMPTY ? 0xFF : v);
            }
            journal(l);

            // Ce que chaque voie va REELLEMENT jouer : le chain, la phrase,
            // sa premiere ligne, et l'instrument avec ses parametres vitaux.
            // Un instrument dont l'attaque est a zero ne s'ouvre jamais : la
            // voie tourne et reste muette, ce qui ressemble a une panne.
            for (int c = 0; c < 10; c++) {
              const uint8_t vc = md_replayer_get_song(c, curLigne);
              if (vc == MD_EMPTY) continue;
              uint8_t ph = MD_EMPTY; int8_t tr = 0;
              md_replayer_get_chain(vc, 0, &ph, &tr);
              uint8_t no=0,i2=0,ve=0,cm=MD_EMPTY,cv=0,mc=MD_EMPTY,mv=0;
              if (ph != MD_EMPTY)
                md_replayer_get_phrase(ph, 0, &no,&i2,&ve,&cm,&cv,&mc,&mv);
              char e[150];
              int n = siprintf(e, "  %s CHAIN %02X PHRASE %02X L0 NOTE %02X"
                                  " INS %02X VEL %02X",
                               noms[c], vc, ph, no, i2, ve);
              if (i2) {
                n += siprintf(e + n, "  TL");
                for (int o = 0; o < 4; o++)
                  n += siprintf(e + n, " %d",
                                md_replayer_get_instr_op_val(i2, o,
                                                             MD_OP_PROP_TOTAL_LEVEL));
                n += siprintf(e + n, "  AR");
                for (int o = 0; o < 4; o++)
                  n += siprintf(e + n, " %d",
                                md_replayer_get_instr_op_val(i2, o,
                                                             MD_OP_PROP_ATTACK));
                n += siprintf(e + n, "  ALG %d KIND %d",
                              md_replayer_get_instr_gen_val(i2, MD_GEN_PROP_ALGORITHM),
                              md_replayer_get_instr_kind(i2));
              }
              journal(e);
            }
            journalVoies = 2;
          }
        }
        if (enLecture) { md_replayer_stop(); enLecture = false; }
        else {
          // ── La lecture est CONTEXTUELLE, comme sur LSDJ et sur l'iPad ─────
          // Depuis SONG on joue toute la chanson ; depuis CHAIN, seulement le
          // chain pointe ; depuis PHRASE, seulement la phrase pointee. Dans
          // ces deux cas une seule voie sonne — celle de la colonne ou etait
          // le curseur — et la lecture boucle sur elle-meme.
          //
          // Le moteur savait deja le faire : je n'avais jamais branche
          // md_replayer_set_play_scope, et START jouait donc toujours la
          // chanson entiere, quelle que soit la page.
          if (page == PAGE_CHAIN) {
            md_replayer_set_play_scope(MD_SCOPE_CHAIN, voieCourante, chainId);
            md_replayer_set_scope_row(chLigne);
            md_replayer_play_from(0);
          } else if (page == PAGE_PHRASE || page == PAGE_INSTR ||
                     page == PAGE_TABLE) {
            // INSTR joue comme PHRASE : en SOLO, la phrase d'ou l'on est
            // descendu, sur sa seule voie. On regle un instrument pour
            // l'entendre dans son contexte — lancer la chanson entiere n'a
            // aucun sens ici, et c'est pourtant ce que faisait la branche
            // « sinon » ou la page INSTR tombait.
            md_replayer_set_play_scope(MD_SCOPE_PHRASE, voieCourante, phraseId);
            md_replayer_set_scope_row(page == PAGE_PHRASE ? phLigne : 0);
            // Depuis la TABLE aussi : on y regle une table pour l'entendre
            // dans sa phrase, pas pour lancer la chanson entiere.
            md_replayer_play_from(0);
          } else {
            md_replayer_set_play_scope(MD_SCOPE_SONG, 0, 0);
            md_replayer_play_from(page == PAGE_SONG ? curLigne : 0);
          }
          // Le demarrage doit etre franc, mais SANS toucher au pointeur
          // d'ecriture : le ramener en arriere creait une discontinuite dans
          // l'anneau — on entendait la lecture s'emballer sur les premieres
          // lignes avant de reprendre sa cadence.
          //
          // On se contente de revenir a la reserve MINIMALE. L'avance avait pu
          // monter a vingt images, soit un tiers de seconde d'attente ; six
          // suffisent pour partir, et elle remontera d'elle-meme si la machine
          // en a besoin.
          avance = kAvanceMin;
          histoNb = 0; histoTete = 0;   // l'histoire d'avant n'a plus cours
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

    // Le dessin est chronometre a part. Le temps audio qui s'y trouve
    // imbrique — les appels d'entretien places au milieu des longs repeints —
    // est retranche, sinon on le compterait deux fois. D dit donc la part du
    // processeur consommee par le SEUL affichage.
    unsigned tD = timerTick(2), audioAvantD = cumulAudio;
    // ── Le centrage, valable pour TOUTES les pages ──────────────────────
    // Chaque page a sa largeur utile ; on la centre en repartissant l'espace
    // restant de part et d'autre. L'en-tete et le titre de page, eux, se
    // calent sur les bords et remettent le decalage a zero.
    {
      int largeur;
      switch (page) {
      case PAGE_SONG:    largeur = kColVoie + nbVisibles * kPasVoie; break;
      // CHAIN et PHRASE restent CALES A GAUCHE : leurs deux ou cinq colonnes
      // etroites, centrees, flottaient au milieu d'un grand vide et l'oeil
      // perdait le bord de reference.
      case PAGE_CHAIN:   largeur = kCols; break;
      case PAGE_PHRASE:  largeur = kCols; break;
      case PAGE_INSTR:   largeur = 31; break;   // libelles + quatre operateurs
      case PAGE_TABLE:   largeur = kCols; break;   // collee a gauche
      default:           largeur = 30; break;   // le menu du projet
      }
      const int c2 = (kCols - largeur) / 2;
      g_colOrigine = c2 > 0 ? c2 : 0;
    }

    // ── Redessin ────────────────────────────────────────────────────────
    static int vuCanal = -1, vuLigne = -1, vuHaut = -1;

    // Vrai quand trame() vient d'effacer l'ecran. Les blocs qui ne repeignent
    // que sur changement DOIVENT le savoir : tester « pageVue != page » ne
    // servait a rien, puisque pageVue est mis a jour dans ce bloc-ci, avant
    // eux. C'est ce qui laissait la page CHAIN vide en y revenant.
    bool ecranEfface = false;
    if (page != pageVue) {
      ecranEfface = true;
      // Changement de page : on repeint tout, en-tete compris.
      const int decalSauve = g_colOrigine;
      g_colOrigine = 0;                  // l'en-tete se cale sur les bords

      // ⚠️ trame() ne repeint que l'ecran DU HAUT. L'ecran du bas garde donc
      // ce que la page precedente y avait laisse — et la page instrument FM y
      // ecrit ses reglages generaux. En changeant de page ils restaient
      // affiches sous une page qui n'a rien a voir.
      //
      // On le vide donc ICI, au moment precis du changement, plutot que de le
      // deduire d'un drapeau garde d'une image sur l'autre. La rangee 2 est
      // epargnee : elle porte le nom du morceau, qui appartient a cet ecran.
      if (!(page == PAGE_INSTR && genreInstr(instrVoie, instrId) == GENRE_FM)) {
        ecran(bas);
        for (int l = 3; l < 19; l++) efface(0, l, 41);
        ecran(g_fond);
      }

      trame();
      entretienSon();   // trame() couvre l'ecran entier : on rend la main au son
      titre(0, 0, "GENETRACKERDS", kTitre);
      // Le titre de page se colle au bord DROIT : il ne doit pas subir le
      // decalage de centrage, sinon il flotte au milieu de nulle part.
      switch (page) {
      case PAGE_SONG:   titre(kCols - 4, 0, "SONG",    kTitre); break;
      case PAGE_CHAIN:  titre(kCols - 5, 0, "CHAIN",   kTitre); break;
      case PAGE_PHRASE: titre(kCols - 6, 0, "PHRASE",  kTitre); break;
      case PAGE_INSTR:  titre(kCols - 5, 0, "INSTR",   kTitre); break;
      case PAGE_TABLE:  titre(kCols - 5, 0, "TABLE",   kTitre); break;
      default:          titre(kCols - 7, 0, "PROJECT", kTitre); break;
      }
      g_colOrigine = decalSauve;         // le contenu de la page, lui, est centre

      // Le decalage vertical des noms de colonnes, a poser JUSTE autour de
      // leur rangee. Laisse actif pour tout le bloc, il decalait aussi les
      // intitules des pages INSTR — qui se retrouvaient une demi-ligne plus
      // bas que leurs valeurs, dessinees ailleurs sans ce decalage.
      const int decalNoms = (MD_GLYPHE_H + 3 * MD_CELL_H) / 2 - MD_GLYPHE_H / 2
                            - MD_CELL_H;

      if (page == PAGE_SONG) {
        // Cinq en haut, cinq en bas, chacune a la meme colonne sur son ecran.
        g_decalYpx = decalNoms;
        // Les noms de colonnes en police FINE : agrandis ils ecrasaient la
        // rangee et donnaient trop de poids a une simple etiquette.
        for (int c = 0; c < nbVisibles; c++)
          texte(kColVoie + c * kPasVoie, 1, noms[reel(c)], kTitre);
        g_decalYpx = 0;
      } else if (page == PAGE_CHAIN) {
        // PHR et TSP, cote a cote : ce sont les intitules de l'iPad, et deux
        // colonnes de deux chiffres n'ont pas a etre eloignees.
        // Colle au bord gauche : numero de ligne, chevron, puis les deux
        // colonnes de deux chiffres separees d'une seule colonne de vide.
        g_decalYpx = decalNoms;
        texte(3, 1, "PHR", kTitre);
        texte(6, 1, "TSP", kTitre);
        g_decalYpx = 0;
      } else if (page == PAGE_INSTR) {
        // Les libelles FM ne sont poses que si la page EST une page FM : sur
        // une colonne PSG ou PCM ils resteraient affiches au-dessus de reglages
        // qui n'ont rien a voir.
        if (genreInstr(instrVoie, instrId) == GENRE_FM) {
        titre(0, 4, "ALGORITHM", kTitre);
        titre(0, 5, "FEEDBACK", kTitre);
        titre(0, 7, "OP", kTitre);
        for (int o = 0; o < 4; o++) {
          char t[2] = { (char)('1'+o), 0 };
          titre(15 + o * 4, 7, t, kTitre);
        }
        // En toutes lettres, et avec les memes noms que l'iPad. « MUL »,
        // « D1L », « RS » ne parlent qu'a qui connait deja le YM2612 ; la
        // colonne des valeurs commence en 20, il y a la place.
        // ATTACK, DECAY et RELEASE sont maintenant des DUREES : plus haut =
        // plus long. Sur la puce ce sont des vitesses, ou monter le nombre
        // raccourcit la note — exactement l'inverse de ce que le mot annonce.
        // SUSTAIN RATE garde son sens de cadence, son nom le dit.
        static const char *par[11] = {
          "MULTIPLIER", "DETUNE", "OUTPUT LEVEL", "ATTACK", "DECAY",
          "SUSTAIN LEVEL", "SUSTAIN DECAY", "RELEASE", "RATE SCALING",
          "AM ENABLE", "SSG-EG" };
        for (int k = 0; k < 11; k++) titre(0, 8 + k, par[k], kTitre);
        }
      } else if (page == PAGE_PHRASE) {
        // Les mots entiers ne tiennent plus : la police doublee coute la
        // moitie de la largeur. On revient aux abreviations — c'est le prix
        // de la lisibilite des chiffres, qui etaient illisibles avant.
        g_decalYpx = decalNoms;
        texte(3,  1, "NOTE", kTitre);
        texte(8,  1, "INS", kTitre);
        texte(12, 1, "VEL", kTitre);
        texte(16, 1, "CMD", kTitre);
        texte(22, 1, "MDCMD", kTitre);
        g_decalYpx = 0;
      } else {
        // Le menu se redessine a chaque image, plus bas : il change avec le
        // curseur. Rien d'autre ici — les touches se trouvent toutes seules.
      }
      g_decalYpx = 0;
      pageVue = page;
      vuCanal = -1; vuLigne = -1; vuHaut = -1;
      for (int c = 0; c < 10; c++) repereVu[c] = -1;
      repereLigne = -1;   // trame() vient d'effacer l'ancien chevron
      bpmVu = -1;
    }

    // ── La fenetre de nom ───────────────────────────────────────────────
        if (page == PAGE_PROJECT && navigateur) {
      g_colOrigine = 0;   // une liste de fichiers occupe toute la largeur
      // On ne repeint que si la liste ou le curseur ont bouge. Redessinee a
      // chaque image, elle raye l'ecran de bandes pendant la lecture — les
      // ecritures courent apres le balayage — et vole du temps au son.
      static int vuSel = -1, vuHautF = -1, vuNb = -1, vuGenre = -1;
      const bool refaireListe = ecranEfface || selFichier != vuSel ||
                                hautFichier != vuHautF || nbFichiers != vuNb ||
                                navGenre != vuGenre;
      vuSel = selFichier; vuHautF = hautFichier; vuNb = nbFichiers;
      vuGenre = navGenre;
      if (refaireListe) {
      // La liste tient en douze lignes ; elle defile avec le curseur.
      const int kVues = 12;
      if (selFichier < hautFichier) hautFichier = selFichier;
      if (selFichier > hautFichier + kVues - 1) hautFichier = selFichier - kVues + 1;
      efface(0, 2, 41);
      texte(0, 2, navEntete[0] ? navEntete : dossier, kEntete);
      for (int i = 0; i < kVues; i++) {
        const int k = hautFichier + i;
        efface(0, 4 + i, 41);
        if (k >= nbFichiers) continue;
        char l[44];
        l[0] = (k == selFichier) ? '>' : ' ';
        l[1] = (typeFic[k] == 2) ? '/' : ' ';   // les dossiers se voient
        // En MAJUSCULES : la police ne contient que l'espace au souligne, et
        // toute minuscule s'affichait comme un vide. « Boot.nds » devenait
        // « B » suivi de trous, ce qui rendait la liste incomprehensible.
        int j = 0;
        for (; j < 40 && fichiers[k][j]; j++) {
          char c2 = fichiers[k][j];
          if (c2 >= 'a' && c2 <= 'z') c2 = (char)(c2 - 'a' + 'A');
          else if ((unsigned char)c2 < 32 || (unsigned char)c2 > 95) c2 = '?';
          l[2 + j] = c2;
        }
        l[2 + j] = 0;
        // Un morceau ouvrable ou un dossier ressort ; le reste est la pour
        // qu'on sache ou l'on est, pas pour etre choisi.
        const u16 couleur = (k == selFichier) ? kAccent
                          : (typeFic[k] != 0) ? kSelection : kAttenue;
        texte(0, 4 + i, l, couleur);
      }
      if (nbFichiers == 0) titre(0, 4, "EMPTY", kAttenue);
      }
    }
    // Les messages prennent la place du menu, pas le bas de l'ecran, et ont
    // leur propre effacement. Ils ne se repeignent QUE s'ils changent : une
    // question figee redessinee a chaque image donnait les memes bandes de
    // vieux televiseur que le menu, si la lecture tournait derriere.
    else if (page == PAGE_PROJECT
             && (demandeNouveau || demandeVoies || demandeEchantillons)) {
      static int vuQuestion = -1;
      const int question = demandeNouveau ? 1 : demandeVoies ? 2 : 3;
      const bool refaireQ = ecranEfface || question != vuQuestion;
      vuQuestion = question;
      if (!refaireQ) { /* rien n'a bouge */ } else {
      g_colOrigine = 0;
      static const char *msgNouveau[6] = {
        "START A NEW SONG ?", "",
        "THE SONG IN MEMORY WILL BE",
        "ERASED AND CANNOT BE GOT BACK.", "",
        "A YES          B NO" };
      static const char *msgVoies[6] = {
        "THIS SONG USES FM5 PS2 PS3", "",
        "THEY ARE OFF TO KEEP",
        "PLAYBACK SMOOTH. TURNING THEM",
        "ON MAY MAKE IT STUTTER.",
        "A TURN ON      B LEAVE OFF" };
      // Le nombre d'instruments concernes se met dans la phrase : « quelques
      // instruments » ne dit pas s'il faut s'en soucier.
      static char l2[42];
      siprintf(l2, "%d INSTRUMENT%s NO SOUND.", manqueEchantillons,
               manqueEchantillons > 1 ? "S HAVE" : " HAS");
      const char *msgEch[6] = {
        "SAMPLES ARE MISSING", "",
        l2,
        "A SAVE HOLDS NO SAMPLES : THEY",
        "LIVE IN THE MD ROM.",
        "A PICK THE ROM   B LATER" };
      const char **m = demandeNouveau ? msgNouveau
                     : demandeVoies   ? msgVoies : msgEch;
      for (int i = 0; i < 6; i++) {
        efface(0, 4 + i, 41);
        titre(0, 4 + i, m[i], (i == 0 || i == 5) ? kEntete : kAttenue);
      }
      }
    }
    // ── LA PAGE ABOUT ─────────────────────────────────────────────────
    // ⚠️ CE QUI EST ECRIT ICI N'EST PAS DECORATIF, C'EST L'AVIS LEGAL.
    // La GNU GPL v3, article 5(d) : si un programme affiche des « Appropriate
    // Legal Notices », TOUTE VERSION MODIFIEE DOIT CONTINUER A LES AFFICHER.
    // C'est ce qui rend le credit opposable — ni une licence permissive ni la
    // GPL seule ne l'obtiennent. Le terme additionnel 7(b) y ajoute le nom et
    // le lien. On peut ajouter des lignes ici ; on n'en retire pas.
    else if (page == PAGE_APROPOS) {
      static bool vuApropos = false;
      if (ecranEfface || !vuApropos) {
        vuApropos = true;
        static const char *lignes[] = {
          "GENETRACKERDS",
          "A MUSIC TRACKER FOR THE NINTENDO DSI, PLAYING THE",
          "SEGA MEGA DRIVE SOUND CHIPS.",
          "",
          "COPYRIGHT (C) 2026 AUDREN THIBAULT",
          "",
          "GITHUB.COM/AUDRENTHIBAULT/MDTRACKERDS",
          "",
          "THIS PROGRAM COMES WITH ABSOLUTELY NO WARRANTY. IT IS",
          "FREE SOFTWARE UNDER THE GNU GPL VERSION 3, AND YOU ARE",
          "WELCOME TO REDISTRIBUTE IT UNDER ITS TERMS. THE FULL",
          "LICENCE IS IN THE FILE NAMED LICENSE, IN THE SOURCE",
          "REPOSITORY ABOVE.",
          "",
          "GPL 7(B) TERM : KEEP THE AUTHOR NAME AND THE LINK",
          "ABOVE, IN THE SOURCE AND ON THIS PAGE.",
          "",
          "FM EMULATION : YMFM BY AARON GILES, BSD 3-CLAUSE.",
          "PSG EMULATION : EMU76489 BY MITSUTAKA OKAZAKI, MIT.",
          "SEE TIERS.MD FOR THEIR NOTICES.",
          "",
          "B  BACK"
        };
        ecran(g_fond);
        const int sauve = g_colOrigine; g_colOrigine = 0;
        titre(2, 1, "ABOUT", kEntete);
        for (int i = 0; i < (int)(sizeof(lignes) / sizeof(lignes[0])); i++)
          texte(2, 4 + i, lignes[i],
                (i == 0) ? kAccent : (i == 4 || i == 6) ? kTitre : kData);
        g_colOrigine = sauve;
      }
      if (frappe & KEY_B) { page = PAGE_PROJECT; pageVue = -1; vuApropos = false; }
    }
    else if (page == PAGE_PROJECT && !dialogueNom) {
      // ── Un vrai menu : on s'y deplace, et A actionne la ligne pointee ────
      // Les lignes sont espacees de deux, sinon on ne distingue rien.
      // La REGION n'est PAS ici : elle ne compte qu'au moment d'exporter, et
      // c'est la qu'on la demande. Un reglage qui ne sert qu'a l'export n'a
      // rien a faire dans un menu ou on peut le changer par megarde — et il y
      // tombait dans le « sinon » qui charge la demo.
      // ⚠️ DEUX EXPORTS ROM, ET CE NE SONT PAS LES MEMES.
      //   EXPORT PLAYER ROM  la cartouche qui JOUE le morceau, telle qu'elle
      //                      existe depuis toujours : elle enregistre le flux
      //                      de registres, rien n'y est modifiable.
      //   EXPORT PROJECT TO MD ROM  le projet lui-meme, grave dans une
      //                      cartouche GeneTracker : il reste EDITABLE, on le
      //                      recharge et on le retouche sur la Mega Drive.
      // On choisit ; l'un ne remplace pas l'autre.
      //
      // Les deux intitules disent ce qui se DEPLACE et vers ou : un projet
      // part vers une ROM, un projet revient d'une ROM. « EXPORT ROM » tout
      // court ne disait pas laquelle des deux ROMs, ni ce qu'elle contenait.
      // ⚠️ PLUS DE « LOAD DEMO ». Le morceau de demonstration etait une
      // composition de l'auteur, embarquee dans le binaire : elle n'a pas sa
      // place dans un depot public, et une version publiee n'a pas a imposer
      // la musique de quelqu'un a qui la telecharge. Un morceau se charge par
      // LOAD SONG, et par nulle autre porte.
      const char *entrees[9] = { "TEMPO", "SAVE SONG", "LOAD SONG",
                                 "NEW SONG",
                                 "EXPORT PLAYER ROM", "EXPORT VGM",
                                 "EXPORT PROJECT TO MD ROM", "IMPORT PROJECT FROM MD ROM",
                                 "ABOUT" };
      // ── On ne repeint QUE si quelque chose a change ────────────────────
      // Ces huit lignes plus leurs valeurs etaient redessinees a CHAQUE image,
      // curseur immobile compris. Les ecritures couraient alors apres le
      // balayage de l'ecran : d'ou les bandes de vieux televiseur qui
      // traversent la page des qu'on lance la lecture. Meme remede que sur les
      // pages CHAIN et PHRASE, qui avaient exactement le meme defaut.
      const int bpmVu2 = (int)(md_replayer_get_bpm() + 0.5);
      static int vuMenu = -1, vuBpm = -1, vuCarte = -1;
      static char vuNom[16] = "\x01";
      const bool refaireMenu = ecranEfface || menuProjet != vuMenu ||
                               bpmVu2 != vuBpm || (int)carteOK != vuCarte ||
                               strncmp(vuNom, nomProjet, sizeof(vuNom) - 1) != 0;
      if (refaireMenu) {
      vuMenu = menuProjet; vuBpm = bpmVu2; vuCarte = carteOK;
      strncpy(vuNom, nomProjet, sizeof(vuNom) - 1); vuNom[sizeof(vuNom) - 1] = 0;
      // ⚠️ UNE RANGEE PAR ENTREE, PAS UNE SUR DEUX.
      // L'ecran ne montre que 19 rangees : 192 pixels divises par une cellule
      // de 10. Avec dix entrees espacees, la derniere tombait en rangee 22 —
      // les deux dernieres, IMPORT ROM PROJECT et ABOUT, etaient dessinees
      // hors de l'ecran et donc introuvables. C'est l'ESPACEMENT qui debordait,
      // pas la taille des lettres : les serrer les garde lisibles, les
      // rapetisser ne ferait que les rendre penibles.
      for (int i = 0; i < 9; i++) {
        const int lig = 4 + i;
        efface(0, lig, 34);
        texte(0, lig, (i == menuProjet) ? ">" : " ",
              (i == menuProjet) ? kEntete : kData);
        titre(2, lig, entrees[i], (i == menuProjet) ? kAccent : kData);
      }
      // La valeur du tempo, en face de sa ligne.
      { int b = (int)(md_replayer_get_bpm() + 0.5);
        char d[10];
        d[0] = (char)('0' + (b / 100) % 10);
        d[1] = (char)('0' + (b / 10) % 10);
        d[2] = (char)('0' + b % 10);
        d[3] = ' '; d[4] = 'B'; d[5] = 'P'; d[6] = 'M'; d[7] = 0;
        texte(14, 4, d, (menuProjet == 0) ? kEntete : kData); }
      efface(14, 6, 26);
      texte(14, 6, nomProjet[0] ? nomProjet : "(UNNAMED)", kData);
      texte(14, 8, carteOK ? "" : "NO SD CARD", kData);
      }
    }

    if (page == PAGE_INSTR) {
      // L'instrument montre est celui RETENU quand on est descendu depuis la
      // phrase, comme instrId sur l'iPad. Le recalculer depuis le curseur SONG
      // faisait changer d'instrument des qu'on bougeait ce curseur.
      const int ins = instrId;

      // On ne redessine que ce qui a CHANGE. Les trente-six valeurs etaient
      // repeintes a chaque image, curseur immobile compris — c'est ce que
      // mesurait le D17 releve pendant l'edition d'un instrument.
      static int vuGlob[2] = {-1, -1}, vuOp[11][4];
      static int vuIns = -1, vuLig = -1, vuCol = -1;
      const bool toutRefaire = ecranEfface || (ins != vuIns) ||
                               (inLigne != vuLig) || (inCol != vuCol);
      if (ins != vuIns) {
        for (int k = 0; k < 11; k++)
          for (int o = 0; o < 4; o++) vuOp[k][o] = -1;
        vuGlob[0] = vuGlob[1] = -1;
      }
      vuIns = ins; vuLig = inLigne; vuCol = inCol;

      // Meme raison que sur CHAIN et PHRASE : on ne le redessine qu'au
      // changement, sinon il raye l'ecran et coute du temps a chaque image.
      if (toutRefaire) {
        char t[8];
        efface(14, 0, 8);
        t[0]='I'; t[1]='N'; t[2]='S'; t[3]=' ';
        t[4]=kHex[(ins>>4)&15]; t[5]=kHex[ins&15]; t[6]=0;
        texte(14, 0, t, kEntete);
      }

      const int genre = genreInstr(instrVoie, ins);

      // ── Les pages PSG, BRUIT et PCM ──────────────────────────────────
      // Elles n'ont rien a voir avec le YM2612 : une voie PSG se regle par
      // des MACROS — une suite de pas deroulee note apres note — et la voie
      // PCM par le choix d'un echantillon. Montrer des operateurs FM sur ces
      // colonnes n'avait aucun sens, puisqu'ils n'y agissent sur rien.
      if (genre != GENRE_FM) {
        static int genreVu = -1, insVu2 = -1;
        const bool refaire = toutRefaire || genre != genreVu || ins != insVu2;
        genreVu = genre; insVu2 = ins;
        if (refaire) for (int l = 3; l < 20; l++) efface(0, l, 41);

        auto val2 = [&](int col, int lig, int v, bool ici) {
          char d[3] = { kHex[(v >> 4) & 15], kHex[v & 15], 0 };
          efface(col, lig, 2);
          texte(col, lig, d, ici ? kEntete : kData);
        };

        // Meme raison : cette page etait entierement redessinee a chaque
        // image, d'ou les bandes pendant la lecture.
        static int vuEch = -2, vuLigP = -1, vuVol = -1, vuTabP = -2;
        static int vuPanP = -2;
        const int echVu = md_replayer_get_instr_sample(ins);
        const int tabVu = md_replayer_get_instr_table(ins);
        const int panVu = md_replayer_get_instr_gen_val(ins, MD_GEN_PROP_PANNING);
        const bool refairePCM = toutRefaire || echVu != vuEch ||
                                inLigne != vuLigP || tabVu != vuTabP ||
                                panVu != vuPanP ||
                                md_replayer_get_instr_pcm_volume(ins) != vuVol;
        vuEch = echVu; vuLigP = inLigne; vuTabP = tabVu; vuPanP = panVu;
        vuVol = md_replayer_get_instr_pcm_volume(ins);
        if (genre == GENRE_PCM && !refairePCM) {
          // rien a redessiner
        } else if (genre == GENRE_PCM) {
          // ── La page ECHANTILLON ──────────────────────────────────────
          // Le curseur se pose sur la VALEUR, jamais sur l'intitule : c'est la
          // valeur qu'on modifie. « LOAD SAMPLE » fait exception, c'est une
          // action et non un reglage, donc c'est le mot lui-meme qui s'allume.
          //
          //   0 LOAD SAMPLE   ouvre le navigateur
          //   1 SAMPLE        lequel, par son NOM
          //   2 BASE NOTE     la note a laquelle il joue a sa vitesse naturelle
          //   3 LOOP          point de bouclage, -- si aucun
          //   4 VOLUME
          //   5 OUTPUT       la place dans le stereo, comme sur les pages PSG
          //   6 TABLE        la table attachee, comme sur les autres pages
          static const char *nom[7] = { "LOAD SAMPLE", "SAMPLE", "BASE NOTE",
                                        "LOOP", "VOLUME", "OUTPUT", "TABLE" };
          // On efface avant d'ecrire : « LOAD SAMPLE » etait redessine par
          // dessus lui-meme a chaque image, en changeant de couleur selon le
          // curseur. Les deux teintes se superposaient une image sur deux, ce
          // qui se voit exactement comme un clignotement.
          for (int k = 0; k < 7; k++) {
            efface(0, 5 + k * 2, 13);
            titre(0, 5 + k * 2, nom[k],
                  (k == 0 && inLigne == 0) ? kAccent : kTitre);
          }

          const int ech = md_replayer_get_instr_sample(ins);
          uint32_t lg = 0; int bcl = -1, base = 60; char nomEch[24] = "";
          if (ech >= 0) md_replayer_get_sample(ech, nomEch, 23, &lg, &bcl, &base);

          // Le rang DANS LA BANQUE, puis le nom. Sans ce rang, on ne voyait
          // pas qu'on parcourt les echantillons DEJA CHARGES dans le morceau —
          // et non les fichiers du dossier. Avec un seul charge, A + droite ne
          // fait donc rien, ce qui est juste mais restait incomprehensible.
          // Le rang dans le DOSSIER, pas dans la banque : c'est ce que la
          // croix parcourt, et c'est ce que l'utilisateur a sous les yeux.
          // Le rang dans le DOSSIER : c'est ce que la croix parcourt.
          { char d5[36];
            if (ech < 0) siprintf(d5, "--/%02d (NONE)", echNb);
            else siprintf(d5, "%02d/%02d %s",
                          (echSel >= 0 ? echSel + 1 : 0), echNb, nomEch);
            efface(14, 7, 26);
            texte(14, 7, d5, (inLigne == 1) ? kAccent : kData); }

          // Sans echantillon, ces deux lignes n'ont AUCUNE valeur a montrer.
          // Elles retombaient sur une note par defaut, si bien qu'en passant
          // sous le premier echantillon on voyait la note de base changer
          // toute seule — on croyait la modifier alors qu'on changeait
          // simplement d'echantillon.
          char d2[6];
          static const char *gam2[12] = {"C-","C#","D-","D#","E-","F-",
                                         "F#","G-","G#","A-","A#","B-"};
          if (ech < 0) { d2[0]='-'; d2[1]='-'; d2[2]='-'; d2[3]=0; }
          else if (base > 0 && base <= MD_MAX_NOTE) {
            const char *g2 = gam2[(base - 1) % 12];
            d2[0]=g2[0]; d2[1]=g2[1]; d2[2]=(char)('0'+((base-1)/12)); d2[3]=0;
          } else { d2[0]='-'; d2[1]='-'; d2[2]='-'; d2[3]=0; }
          efface(14, 9, 4);
          texte(14, 9, d2, (inLigne == 2) ? kAccent : kData);

          if (ech < 0 || bcl < 0) { d2[0]='-'; d2[1]='-'; d2[2]=0; }
          else siprintf(d2, "%05d", bcl);
          efface(14, 11, 6);
          texte(14, 11, d2, (inLigne == 3) ? kAccent : kData);

          { const int v = md_replayer_get_instr_pcm_volume(ins);
            char d3[3] = { kHex[(v >> 4) & 15], kHex[v & 15], 0 };
            efface(14, 13, 2);
            texte(14, 13, d3, (inLigne == 4) ? kAccent : kData); }

          // La sortie, dessinee comme sur la page PSG : les trois lettres
          // restent lisibles, seule celle qui est choisie est en couleur. Une
          // valeur qui defile ne dirait pas qu'il y a trois positions.
          { const int pan = md_replayer_get_instr_gen_val(ins,
                                MD_GEN_PROP_PANNING);
            static const char *lcr[3] = { "L", "C", "R" };
            static const int val[3] = { 1, 0, 2 };   // gauche, centre, droite
            for (int i2 = 0; i2 < 3; i2++) {
              efface(14 + i2 * 2, 15, 1);
              texte(14 + i2 * 2, 15, lcr[i2],
                    (inLigne == 5) ? ((pan == val[i2]) ? kAccent : kData)
                                   : ((pan == val[i2]) ? kTitre : kData));
            } }

          { char d4[4]; texteTable(ins, d4);
            efface(14, 17, 3);
            texte(14, 17, d4, (inLigne == 6) ? kAccent : kData); }

          // La ligne « BANK 0/32 0K/512K » est PARTIE : quatre nombres sans
          // phrase, que rien sur cette page ne permettait de rattacher a quoi
          // que ce soit. L'occupation de la banque a sa place ailleurs, dans
          // un ecran qui parle de la banque — pas au milieu des reglages d'un
          // instrument.
          efface(0, 16, 38);
        } else {
          // ── PSG et BRUIT, la page COMPLETE de l'iPad ──────────────────
          // Elle ne portait que l'enveloppe et les macros : la table, la
          // sortie et le desaccord manquaient, alors qu'ils existent des deux
          // cotes. L'ordre est celui de LSDJInstrumentPanel.swift.
          //
          // UNE seule numerotation, partagee avec l'edition et la navigation :
          //   0 ENV (3 points)   1 TABLE   2 OUTPUT   3 FINETUNE
          //   4 NOISE MODE (voie de bruit seulement)
          //   5 VOL LEN   6 VOL LOOP   7 VOL (16 pas)
          //   8 ARP LEN   9 ARP LOOP  10 ARP FIXED  11 ARP (16 pas)
          const bool bruit = (genre == GENRE_BRUIT);
          // ⚠️ Le seuil est 3, pas 4 : c'est le rang de NOISE MODE, la ligne
          // que la voie PSG saute. L'edition et la navigation comptaient
          // deja ainsi ; seul le dessin etait decale, si bien que sur PSG le
          // curseur s'allumait une ligne PLUS BAS que celle qu'on reglait.
          const int canon = (bruit || inLigne < 3) ? inLigne : inLigne + 1;

          uint8_t vol[MD_PSG_MACRO_MAX]; int bcl = MD_EMPTY;
          const int nvol = md_replayer_get_psg_vol_macro(ins, vol,
                                                     MD_PSG_MACRO_MAX, &bcl);
          int8_t arp[MD_PSG_MACRO_MAX]; int bcla = MD_EMPTY; bool fixe = false;
          const int narp = md_replayer_get_psg_arp_macro(ins, arp,
                                        MD_PSG_MACRO_MAX, &bcla, &fixe);

          auto val2 = [&](int col, int lig, int v, bool ici) {
            char d[3] = { kHex[(v >> 4) & 15], kHex[v & 15], 0 };
            efface(col, lig, 2);
            texte(col, lig, d, ici ? kAccent : kData);
          };

          int lig = 4;
          titre(0, lig, "ENV", kTitre);
          for (int pt = 0; pt < 3; pt++) {
            const int amp = md_replayer_get_env_amp(ins, pt);
            const int vit = md_replayer_get_env_speed(ins, pt);
            char d[3];
            if (amp < 0 || amp == MD_EMPTY) { d[0] = '-'; d[1] = '-'; }
            else { d[0] = kHex[amp & 15]; d[1] = kHex[vit & 15]; }
            d[2] = 0;
            efface(10 + pt * 4, lig, 2);
            texte(10 + pt * 4, lig, d,
                  (canon == 0 && inCol == pt) ? kAccent : kData);
          }
          lig += 2;

          // La sortie : les trois positions restent lisibles, seule celle qui
          // est choisie est en pleine couleur — c'est ainsi que LSDJ montre un
          // choix, et ca evite de faire defiler une valeur invisible.
          titre(0, lig, "OUTPUT", kTitre);
          { const int pan = md_replayer_get_instr_gen_val(ins,
                                MD_GEN_PROP_PANNING);
            static const char *lcr[3] = { "L", "C", "R" };
            static const int val[3] = { 1, 0, 2 };   // gauche, centre, droite
            for (int i2 = 0; i2 < 3; i2++) {
              efface(10 + i2 * 2, lig, 1);
              texte(10 + i2 * 2, lig, lcr[i2],
                    (canon == 1) ? ((pan == val[i2]) ? kAccent : kData)
                                 : ((pan == val[i2]) ? kTitre : kData));
            } }
          lig++;

          titre(0, lig, "FINETUNE", kTitre);
          val2(10, lig, md_replayer_get_instr_gen_val(ins,
                            MD_GEN_PROP_FINE_TUNE), canon == 2);
          lig++;

          if (bruit) {
            titre(0, lig, "NOISE", kTitre);
            static const char *nz[8] = { "P-HI","P-MD","P-LO","P-T3",
                                         "W-HI","W-MD","W-LO","W-T3" };
            const int nm2 = borne(md_replayer_get_instr_gen_val(ins,
                                      MD_GEN_PROP_PSG_NOISE), 0, 7);
            efface(10, lig, 5);
            texte(10, lig, nz[nm2], (canon == 3) ? kAccent : kData);
            lig++;
          }
          lig++;

          titre(0, lig, "VOL LEN", kTitre);
          val2(10, lig, nvol, canon == 4); lig++;
          titre(0, lig, "VOL LOOP", kTitre);
          val2(10, lig, bcl & 0xFF, canon == 5); lig++;
          titre(0, lig, "VOL", kTitre);
          for (int i2 = 0; i2 < 16; i2++) {
            efface(10 + i2 * 2, lig, 1);
            char c1[2] = { kHex[i2 < nvol ? (vol[i2] & 15) : 0], 0 };
            texte(10 + i2 * 2, lig, i2 < nvol ? c1 : "-",
                  (canon == 6 && inCol == i2) ? kAccent : kData);
          }
          lig += 2;

          titre(0, lig, "ARP LEN", kTitre);
          val2(10, lig, narp, canon == 7); lig++;
          titre(0, lig, "ARP LOOP", kTitre);
          val2(10, lig, bcla & 0xFF, canon == 8); lig++;
          titre(0, lig, "ARP FIXED", kTitre);
          val2(10, lig, fixe ? 1 : 0, canon == 9); lig++;
          titre(0, lig, "ARP", kTitre);
          for (int i2 = 0; i2 < 16; i2++) {
            efface(10 + i2 * 2, lig, 2);
            if (i2 < narp) {
              const uint8_t u = (uint8_t)arp[i2];
              char d[3] = { kHex[(u >> 4) & 15], kHex[u & 15], 0 };
              texte(10 + i2 * 2, lig, d,
                    (canon == 10 && inCol == i2) ? kAccent : kData);
            } else texte(10 + i2 * 2, lig, "-", kData);
          }
          lig += 2;

          // La table attachee, TOUT EN BAS : « OFF » quand il n'y en a pas.
          // A + droite l'attache et fait monter le numero ; A seul ouvre son
          // ecran.
          titre(0, lig, "TABLE", kTitre);
          { char d[4]; texteTable(ins, d);
            efface(10, lig, 3);
            texte(10, lig, d, (canon == 11) ? kAccent : kData); }
        }
      } else {

      // Les deux parametres globaux.
      static const int glob[2] = { MD_GEN_PROP_ALGORITHM, MD_GEN_PROP_FEEDBACK };
      for (int k = 0; k < 2; k++) {
        int v = md_replayer_get_instr_gen_val(ins, glob[k]);
        if (v == vuGlob[k] && !toutRefaire) continue;
        vuGlob[k] = v;
        char d[3] = { kHex[(v>>4)&15], kHex[v&15], 0 };
        efface(15, 4 + k, 2);
        texte(15, 4 + k, d, (inLigne == k) ? kAccent : kData);
      }

      // Les ONZE parametres, pour chacun des quatre operateurs. AM ENABLE et
      // SSG-EG existaient dans le moteur sans qu'aucun ecran ne les montre.
      static const int prop[11] = {
        MD_OP_PROP_MULTIPLE, MD_OP_PROP_DETUNE, MD_OP_PROP_TOTAL_LEVEL,
        MD_OP_PROP_ATTACK, MD_OP_PROP_DECAY, MD_OP_PROP_SUSTAIN_LEVEL,
        MD_OP_PROP_SUSTAIN_RATE, MD_OP_PROP_RELEASE, MD_OP_PROP_KEY_SCALE,
        MD_OP_PROP_AM, MD_OP_PROP_SSG_EG };
      static const int maxiVu[11] = { 15, 7, 127, 31, 31, 15, 31, 15, 3, 1, 15 };
      // Meme table que pour l'edition — voir le commentaire la-bas.
      static const bool inverseVu[11] = { false, false, true, true, true,
                                          true, true, true, false,
                                          false, false };
      for (int k = 0; k < 11; k++)
        for (int o = 0; o < 4; o++) {
          int v = md_replayer_get_instr_op_val(ins, o, prop[k]);
          if (inverseVu[k]) v = maxiVu[k] - v;
          if (v == vuOp[k][o] && !toutRefaire) continue;
          vuOp[k][o] = v;
          char d[3] = { kHex[(v>>4)&15], kHex[v&15], 0 };
          efface(15 + o * 4, 8 + k, 2);
          texte(15 + o * 4, 8 + k, d,
                (inLigne == k + 2 && inCol == o) ? kAccent : kData);
        }

      // ── Le bas de la page, sur l'ECRAN DU BAS ─────────────────────────
      // L'ecran du haut n'a que dix-neuf rangees et la grille des operateurs
      // les remplit jusqu'a la derniere. Tout ce qui suit va donc en dessous,
      // dans le meme ordre : les reglages generaux, la sortie, une rangee de
      // vide, puis la table — toujours la derniere.
      { static int vuTabFM = -2, vuLigFM = -1, vuPanFM = -2, vuG5[5] = {-2,-2,-2,-2,-2};
        static const int glob5[5] = {
          MD_GEN_PROP_AMS, MD_GEN_PROP_PMS, MD_GEN_PROP_LFO_ENABLE,
          MD_GEN_PROP_LFO_FREQ, MD_GEN_PROP_FINE_TUNE };
        static const char *nom5[5] = {
          "AM SENS", "PM SENS", "LFO", "LFO SPEED", "FINETUNE" };
        const int t2 = md_replayer_get_instr_table(ins);
        const int pan = md_replayer_get_instr_gen_val(ins, MD_GEN_PROP_PANNING);
        bool bouge5 = false;
        for (int k = 0; k < 5; k++)
          if (md_replayer_get_instr_gen_val(ins, glob5[k]) != vuG5[k]) bouge5 = true;

        if (toutRefaire || bouge5 || t2 != vuTabFM || inLigne != vuLigFM ||
            pan != vuPanFM) {
          vuTabFM = t2; vuLigFM = inLigne; vuPanFM = pan;

          const int sauveCol = g_colOrigine;
          g_colOrigine = 0;
          ecran(bas);
          // L'ecran du bas porte la grille de SONG quand on vient de la : il
          // faut le vider en arrivant, sinon les deux pages se superposent.
          // ⚠️ On n'efface QUE sous le nom du morceau : la rangee 2 lui
          // appartient, et une rangee de vide l'en separe.
          if (toutRefaire) for (int l = 3; l < 19; l++) efface(0, l, 41);
          // Deux rangees de vide en tete : les deux ecrans se touchent sans
          // separation, et colle en rangee zero ce bloc se lirait comme la
          // suite de la grille.
          for (int k = 0; k < 5; k++) {
            const int v = md_replayer_get_instr_gen_val(ins, glob5[k]);
            vuG5[k] = v;
            efface(0, 4 + k, 30);
            titre(0, 4 + k, nom5[k], kTitre);
            char d[3] = { kHex[(v >> 4) & 15], kHex[v & 15], 0 };
            texte(15, 4 + k, d, (inLigne == 13 + k) ? kAccent : kData);
          }

          efface(0, 9, 30);
          titre(0, 9, "OUTPUT", kTitre);
          { static const char *lcr[3] = { "L", "C", "R" };
            static const int val[3] = { 1, 0, 2 };
            for (int i2 = 0; i2 < 3; i2++)
              texte(15 + i2 * 2, 9, lcr[i2],
                    (inLigne == 18) ? ((pan == val[i2]) ? kAccent : kData)
                                    : ((pan == val[i2]) ? kTitre : kData));
          }

          // Une rangee de vide avant la table, comme entre FEEDBACK et OP.
          efface(0, 10, 30);
          efface(0, 11, 30);
          titre(0, 11, "TABLE", kTitre);
          { char d[4]; texteTable(ins, d);
            texte(15, 11, d, (inLigne == 19) ? kAccent : kData); }

          ecran(g_fond);
          g_colOrigine = sauveCol;
        } }
      }
    }

    // ── La page TABLE ───────────────────────────────────────────────────
    // La page « T » de LSDJ : seize lignes, cinq colonnes, HUIT arrets de
    // curseur — la lettre d'une commande et sa valeur se reglent separement,
    // comme dans une phrase.
    //
    // RÈGLE VALABLE POUR TOUTE PAGE : on ne repeint QUE ce qui a change. Une
    // page redessinee a chaque image fait courir les ecritures apres le
    // balayage de l'ecran — les bandes de vieux televiseur — et vole au son le
    // temps qu'il lui faut. Les reperes de lecture, qui bougent en permanence,
    // sont donc traites A PART de la grille : eux seuls sont repeints quand ils
    // se deplacent.
    if (page == PAGE_TABLE) {
      // Les cinq colonnes, leur largeur, et le chevron qui les precede.
      static const int col[8]   = { 4, 8, 12, 13, 17, 18, 22, 24 };
      static const int larg[8]  = { 2, 2,  1,  2,  1,  2,  2,  2 };
      // Les cinq groupes affichés : ou poser leur chevron, et quel FLUX de
      // lecture ils suivent. Une table ne se deroule pas d'un bloc : la
      // commande H fait boucler un flux sans toucher aux autres, d'ou cinq
      // reperes qui se promenent chacun de leur cote.
      static const int chevCol[5]  = { 3, 7, 11, 16, 21 };
      static const int chevFlux[5] = { 0, 1,  1,  2,  2 };

      static int vuTabId = -1, vuTabL = -1, vuTabC = -1;
      const bool refaireTable = ecranEfface || modifie ||
                                tableId != vuTabId || tabLigne != vuTabL ||
                                tabCol != vuTabC;
      if (refaireTable) {
        vuTabId = tableId; vuTabL = tabLigne; vuTabC = tabCol;

        char t9[28];
        siprintf(t9, "TABLE %02X  INSTR %02X", tableId & 0xFF, instrId & 0xFF);
        efface(0, 0, 22); texte(0, 0, t9, kEntete);

        const int decalT = (MD_GLYPHE_H + 3 * MD_CELL_H) / 2 - MD_GLYPHE_H / 2
                           - MD_CELL_H;
        g_decalYpx = decalT;
        texte(4, 1, "VOL", kTitre);
        texte(8, 1, "TSP", kTitre);
        texte(12, 1, "CMD", kTitre);
        texte(17, 1, "CMD", kTitre);
        texte(22, 1, "MD CMD", kTitre);
        g_decalYpx = 0;

        // Le nom de la commande pointee, centre sous la grille — meme regle
        // que sur la page PHRASE. Les colonnes 2-3 et 4-5 portent les deux
        // commandes a lettre, 6-7 la commande MD.
        { uint8_t vo, c1, v1, c2, v2, mc2, mv2; int8_t tr;
          md_replayer_get_table_row(tableId, tabLigne, &vo, &tr,
                                    &c1, &v1, &c2, &v2, &mc2, &mv2);
          const uint8_t nNom  = (uint8_t)(sizeof(kNomCmd) / sizeof(kNomCmd[0]));
          const uint8_t nNomM = (uint8_t)(sizeof(kNomMdCmd) / sizeof(kNomMdCmd[0]));
          const char *nomT = 0;
          if ((tabCol == 2 || tabCol == 3) && c1 != MD_EMPTY && c1 < nNom)
            nomT = kNomCmd[c1];
          else if ((tabCol == 4 || tabCol == 5) && c2 != MD_EMPTY && c2 < nNom)
            nomT = kNomCmd[c2];
          else if ((tabCol == 6 || tabCol == 7) && mc2 != MD_EMPTY && mc2 < nNomM)
            nomT = kNomMdCmd[mc2];
          efface(0, 20, kCols);
          if (nomT) {
            int lg = 0; while (nomT[lg]) lg++;
            titre((kCols - lg) / 2, 20, nomT, kTitre);
          } }

        for (int l = 0; l < MD_TABLE_ROWS; l++) {
          if ((l & 7) == 0) entretienSon();
          const int lig = 3 + l;
          numeroLigne(0, lig, l, l == tabLigne);

          uint8_t vol,c1,v1,c2,v2,mc,mv; int8_t tsp;
          md_replayer_get_table_row(tableId, l, &vol, &tsp,
                                    &c1, &v1, &c2, &v2, &mc, &mv);
          char sv[3], st[3], sl1[2], sa1[3], sl2[2], sa2[3], sm[3], smv[3];
          sv[0]=kHex[(vol>>4)&15]; sv[1]=kHex[vol&15]; sv[2]=0;
          { const uint8_t u = (uint8_t)tsp;
            st[0]=kHex[(u>>4)&15]; st[1]=kHex[u&15]; st[2]=0; }
          // Une commande vide s'ecrit « - -- », comme dans une phrase.
          if (c1 == MD_EMPTY) { sl1[0]='-'; sa1[0]='-'; sa1[1]='-'; }
          else { const char x = md_table_cmd_letter(c1); sl1[0] = x ? x : '?';
                 sa1[0]=kHex[(v1>>4)&15]; sa1[1]=kHex[v1&15]; }
          sl1[1]=0; sa1[2]=0;
          if (c2 == MD_EMPTY) { sl2[0]='-'; sa2[0]='-'; sa2[1]='-'; }
          else { const char x = md_table_cmd_letter(c2); sl2[0] = x ? x : '?';
                 sa2[0]=kHex[(v2>>4)&15]; sa2[1]=kHex[v2&15]; }
          sl2[1]=0; sa2[2]=0;
          if (mc == MD_EMPTY) { sm[0]='-'; sm[1]='-'; smv[0]='-'; smv[1]='-'; }
          else { sm[0]=kHex[(mc>>4)&15]; sm[1]=kHex[mc&15];
                 smv[0]=kHex[(mv>>4)&15]; smv[1]=kHex[mv&15]; }
          sm[2]=0; smv[2]=0;
          const char *txt[8] = { sv, st, sl1, sa1, sl2, sa2, sm, smv };

          for (int k = 0; k < 8; k++) {
            efface(col[k], lig, larg[k]);
            const bool ici = (l == tabLigne && k == tabCol);
            // Le curseur est un PAVE ROUGE, comme sur toutes les autres pages.
            if (ici) {
              const int x0 = (col[k] + g_colOrigine) * MD_CELL_W;
              const int y0 = lig * MD_CELL_H;
              for (int y = y0; y < y0 + MD_GLYPHE_H; y++)
                for (int x = x0 - 1; x < x0 + larg[k] * MD_CELL_W; x++)
                  pixel(x, y, kAccent);
            }
            texte(col[k], lig, txt[k], ici ? kFond : kData);
          }
        }
        for (int g = 0; g < 5; g++) repereTab[g] = -2;   // a redessiner
      }

      // ── Les cinq reperes de lecture ────────────────────────────────────
      // Un chevron devant chaque colonne, a la ligne qu'elle est en train de
      // lire. On ne touche QUE ceux qui ont bouge : les repeindre tous a chaque
      // image, c'etait la page entiere qui scintillait.
      for (int g = 0; g < 5; g++) {
        const int r = enLecture
            ? md_replayer_play_table_pos(voieCourante, chevFlux[g]) : -1;
        if (r == repereTab[g]) continue;
        if (repereTab[g] >= 0 && repereTab[g] < MD_TABLE_ROWS)
          efface(chevCol[g], 3 + repereTab[g], 1);
        if (r >= 0 && r < MD_TABLE_ROWS)
          chevron(chevCol[g], 3 + r, kAccent);
        repereTab[g] = r;
      }
    }

    if (page == PAGE_CHAIN || page == PAGE_PHRASE) {
      // ── Quel chain, quelle phrase ? ──────────────────────────────────
      // Le chain montre est celui pointe dans SONG ; la phrase montree est
      // celle pointee dans CHAIN. C'est la chaine de navigation de LSDJ.
      const uint8_t noChain = (uint8_t)chainId;
      const uint8_t noPhrase = (uint8_t)phraseId;

      // Cet en-tete se reecrivait a CHAQUE image, comme les lignes le
      // faisaient avant : d'ou les memes bandes de vieux televiseur dessus, et
      // le meme temps vole au son. On ne le redessine que s'il change.
      static int vuMontre = -1, vuPageT = -1;
      char t[8];
      const uint8_t montre = (page == PAGE_CHAIN) ? noChain : noPhrase;
      if (ecranEfface || montre != vuMontre || page != vuPageT) {
      vuMontre = montre; vuPageT = page;
      efface(14, 0, 8);
      t[0] = (page == PAGE_CHAIN) ? 'C' : 'P';
      t[1] = (page == PAGE_CHAIN) ? 'H' : 'H';
      t[2] = ' ';
      t[3] = kHex[(montre >> 4) & 15]; t[4] = kHex[montre & 15]; t[5] = 0;
      texte(14, 0, t, kEntete);
      }

      const int nl = (page == PAGE_CHAIN) ? MD_ROWS_PER_CHAIN : MD_ROWS_PER_PHRASE;

      // ── On ne repeint QUE si quelque chose a change ────────────────────
      // Ces seize lignes etaient redessinees a CHAQUE image, curseur immobile
      // et donnees inchangees comprises. Deux consequences : les ecritures
      // couraient apres le balayage de l'ecran — d'ou les bandes noires qui
      // traversent l'image, comme sur un vieux televiseur — et tout ce temps
      // etait vole a la production du son. C'est pour ca que le son se degrade
      // surtout sur ces pages.
      const int idCourant = (page == PAGE_CHAIN) ? chainId : phraseId;
      const int ligCour   = (page == PAGE_CHAIN) ? chLigne : phLigne;
      const int colCour   = (page == PAGE_CHAIN) ? chCol   : phCol;
      static int vuPage2 = -1, vuId = -1, vuLig2 = -1, vuCol2 = -1;
      static bool selVue2 = false;
      const bool selFinie = selVue2 && !selActive;
      selVue2 = selActive;
      const bool refaireGrille = ecranEfface || modifie || selActive ||
                                 selFinie || (page != vuPage2) ||
                                 (idCourant != vuId) || (ligCour != vuLig2) ||
                                 (colCour != vuCol2);
      vuPage2 = page; vuId = idCourant; vuLig2 = ligCour; vuCol2 = colCour;

      // ── LE NOM DE LA COMMANDE, SOUS LA GRILLE ─────────────────────────
      // ⚠️ Une lettre seule ne se retient pas. Tant que le curseur est sur une
      // colonne de commande, son nom s'ecrit CENTRE sous la grille, separe
      // d'elle par une rangee vide, et disparait des qu'on en sort. La grille
      // s'arrete a la rangee 18 sur les trente-deux de l'ecran : la place est
      // la, inutile d'aller sur l'ecran du bas.
      if (refaireGrille) {
        const char *nomC = 0;
        if (page == PAGE_PHRASE && noPhrase != MD_EMPTY) {
          uint8_t no,i2,ve,cm,cv,mc,mv;
          md_replayer_get_phrase(noPhrase, phLigne, &no,&i2,&ve,&cm,&cv,&mc,&mv);
          // ⚠️ On borne sur la TAILLE DU TABLEAU DE NOMS, pas sur le nombre
          // de commandes : si la table du moteur grandit sans qu'on ajoute le
          // libelle, on ne lit pas a cote.
          const uint8_t nNom  = (uint8_t)(sizeof(kNomCmd) / sizeof(kNomCmd[0]));
          const uint8_t nNomM = (uint8_t)(sizeof(kNomMdCmd) / sizeof(kNomMdCmd[0]));
          if ((phCol == 3 || phCol == 4) && cm != MD_EMPTY && cm < nNom)
            nomC = kNomCmd[cm];
          else if ((phCol == 5 || phCol == 6) && mc != MD_EMPTY && mc < nNomM)
            nomC = kNomMdCmd[mc];
        }
        efface(0, 20, kCols);
        if (nomC) {
          int lg = 0; while (nomC[lg]) lg++;
          titre((kCols - lg) / 2, 20, nomC, kTitre);
        }
      }

      for (int l = 0; refaireGrille && l < nl; l++) {
        if (l == 0) entretienSon();
        int lig = 3 + l;
        numeroLigne(0, lig, l,
                    (page == PAGE_CHAIN ? l == chLigne : l == phLigne));

        if (page == PAGE_CHAIN) {
          uint8_t ph = MD_EMPTY; int8_t tsp = 0;
          if (noChain != MD_EMPTY) md_replayer_get_chain(noChain, l, &ph, &tsp);
          char a[3], b[3];
          if (ph == MD_EMPTY) { a[0]='-'; a[1]='-'; } else { a[0]=kHex[(ph>>4)&15]; a[1]=kHex[ph&15]; }
          a[2]=0;
          uint8_t u = (uint8_t)tsp;
          b[0]=kHex[(u>>4)&15]; b[1]=kHex[u&15]; b[2]=0;
          // Meme regle que sur SONG : pendant une selection, le rouge tient
          // l'ancre et le jaune s'etend.
          const bool r0 = selActive ? (l == selAncreLigne && selAncreCol == 0)
                                    : (l == chLigne && chCol == 0);
          const bool r1 = selActive ? (l == selAncreLigne && selAncreCol == 1)
                                    : (l == chLigne && chCol == 1);
          efface(3, lig, 2);
          texte(3, lig, a, r0 ? kAccent
                          : estSelectionne(l, 0) ? kSelection : kData);
          efface(6, lig, 2);
          texte(6, lig, b, r1 ? kAccent
                          : estSelectionne(l, 1) ? kSelection : kData);
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
          char si[3]={'-','-',0}, sv2[3]={'-','-',0};
          if (ins) { si[0]=kHex[(ins>>4)&15]; si[1]=kHex[ins&15]; }
          if (vel != MD_EMPTY) { sv2[0]=kHex[(vel>>4)&15]; sv2[1]=kHex[vel&15]; }
          // La colonne CMD porte un INDICE dans la table des commandes, pas
          // une lettre : celle-ci se lit par md_table_cmd_letter(). En
          // affichant l'indice tel quel, l'indice 0 donnait le caractere nul,
          // qui termine la chaine — la case disparaissait entierement.
          char sl[2]={'-',0}, sc[3]={'-','-',0};
          char smc[3]={'-','-',0}, smv[3]={'-','-',0};
          if (cmd != MD_EMPTY) {
            const char l2 = md_table_cmd_letter(cmd);
            sl[0] = l2 ? l2 : '?';
            sc[0]=kHex[(cv>>4)&15]; sc[1]=kHex[cv&15];
          }
          // MD CMD porte un CODE DefleMask sur deux chiffres, suivi de sa
          // valeur : quatre chiffres en DEUX moities.
          if (mc != MD_EMPTY) {
            smc[0]=kHex[(mc>>4)&15]; smc[1]=kHex[mc&15];
            smv[0]=kHex[(mv>>4)&15]; smv[1]=kHex[mv&15];
          }
          // SEPT positions de curseur, chacune sur ce qu'elle modifie
          // vraiment : la lettre de commande et sa valeur separement, le code
          // machine et sa valeur separement. Les quatre chiffres de MD CMD se
          // parcouraient d'un bloc, ce qui obligeait a retaper la commande
          // pour en changer la valeur.
          const int cols[7] = {3, 8, 12, 16, 17, 22, 24};
          const int larg[7] = {3, 2, 2,  1,  2,  2,  2};
          const char *txt[7] = {nt, si, sv2, sl, sc, smc, smv};
          for (int k = 0; k < 7; k++) {
            efface(cols[k], lig, larg[k]);
            const bool rk = selActive ? (l == selAncreLigne && selAncreCol == k)
                                      : (l == phLigne && phCol == k);
            texte(cols[k], lig, txt[k],
                  rk ? kAccent
                  : estSelectionne(l, k) ? kSelection : kData);
          }
        }
      }
    }

    if (page == PAGE_SONG) {

    // Dessine une case : le fond, le curseur eventuel, puis la valeur.
    auto dessineCase = [&](int c, int ligne, bool ici) {
      int l = ligne - haut;
      if (l < 0 || l >= kLignesVues) return;
      uint8_t v = md_replayer_get_song(reel(c), ligne);
      char cel[3];
      if (v == MD_EMPTY) { cel[0]='-'; cel[1]='-'; }
      else { cel[0]=kHex[(v>>4)&15]; cel[1]=kHex[v&15]; }
      cel[2] = 0;
      int col = kColVoie + c * kPasVoie;
      efface(col, 3 + l, 2);
      const bool sel = estSelectionne(ligne, c);
      // Pendant une selection, le rouge marque l'ANCRE — le point fixe d'ou
      // part la plage — et c'est le jaune qui s'etend. Faire courir le rouge
      // avec le curseur donnait deux reperes mobiles et on ne savait plus
      // lequel lisait quoi.
      if (selActive)
        ici = (ligne == selAncreLigne && c == selAncreCol);
      if (ici) {
        int x0 = (col + g_colOrigine) * MD_CELL_W, y0 = (3 + l) * MD_CELL_H;
        for (int y = y0; y < y0 + MD_GLYPHE_H; y++)
          // ROUGE : en violet clair le curseur se confondait avec le texte.
          for (int x = x0 - 1; x < x0 + 2 * MD_CELL_W; x++) pixel(x, y, kAccent);
      }
      texte(col, 3 + l, cel, ici ? kFond : (sel ? kSelection : kData));
    };
    auto dessineNumero = [&](int ligne) {
      int l = ligne - haut;
      if (l < 0 || l >= kLignesVues) return;
      numeroLigne(0, 3 + l, ligne, ligne == curLigne);
    };

    // Pendant une selection, la plage change des que le curseur bouge : on
    // repeint tout. Le dessin ne coute presque rien (mesure sur console : D00).
    //
    // Et UNE IMAGE DE PLUS apres qu'elle se termine : sans ca, le jaune et le
    // rouge restaient affiches apres la copie, puisque plus rien ne demandait
    // le repeint.
    static bool selVue = false;
    if (selActive || selVue) vuHaut = -1;
    selVue = selActive;
    // Une modification repeint TOUTE la grille, pas seulement la colonne du
    // curseur. Coller ecrit sur autant de colonnes que la selection copiee en
    // comptait, et le clone profond aussi : ce qu'on venait de poser a cote
    // restait invisible jusqu'a ce qu'on passe le curseur dessus.
    if (haut != vuHaut || modifie) {
      for (int l = 0; l < kLignesVues; l++) {
        // Une ligne sur quatre, on entretient l'anneau : sans ca le defilement
        // monopolisait le processeur assez longtemps pour vider le son.
        if ((l & 7) == 0) entretienSon();
        dessineNumero(haut + l);
        for (int c = 0; c < nbVisibles; c++) {
          // La colonne du chevron AUSSI. dessineCase ne nettoie que les trois
          // colonnes de la valeur : en defilant, les anciens chevrons rouges
          // restaient a l'ecran, et comme on remet les reperes a -1 juste
          // apres, plus personne ne savait qu'il fallait les effacer. On se
          // retrouvait avec un chevron sur presque chaque ligne.
          efface(kColVoie - 1 + c * kPasVoie, 3 + l, 1);
          dessineCase(c, haut + l, c == curCanal && haut + l == curLigne);
        }
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
    for (int c = 0; c < nbVisibles; c++) {
      int r = enLecture ? ligneEntendue(reel(c)) : -1;
      if (r == repereVu[c]) continue;
      if (repereVu[c] >= 0) {
        int l = repereVu[c] - haut;
        if (l >= 0 && l < kLignesVues)
          efface(kColVoie - 1 + c * kPasVoie, 3 + l, 1);
      }
      if (r >= 0) {
        int l = r - haut;
        if (l >= 0 && l < kLignesVues) {
          efface(kColVoie - 1 + c * kPasVoie, 3 + l, 1);
          chevron(kColVoie - 1 + c * kPasVoie, 3 + l, kAccent);
        }
      }
      repereVu[c] = r;
    }

    }  // fin de la page SONG

    // ── Le repere de lecture des pages CHAIN et PHRASE ────────────────
    // Il n'existait que sur SONG : on voyait donc la lecture avancer dans la
    // chanson, mais jamais dans le chain ni dans la phrase qu'on editait.
    // Le chevron ne s'allume que si le chain / la phrase AFFICHE est bien
    // celui que le canal joue — sinon il montrerait la ligne d'un autre.
    if (page == PAGE_CHAIN || page == PAGE_PHRASE) {
      int r = -1;
      if (enLecture) {
        // voieCourante, et non curCanal : ce dernier est un indice de COLONNE
        // AFFICHEE sur la page SONG, qui ne coincide pas avec le numero de
        // voie des que des colonnes sont masquees. Sur PCM, PSG et le bruit,
        // le repere ne trouvait donc jamais rien et restait invisible.
        if (page == PAGE_CHAIN) {
          if (md_replayer_play_chain(voieCourante) == chainId)
            r = md_replayer_play_chain_row(voieCourante);
        } else {
          if (md_replayer_play_phrase(voieCourante) == phraseId)
            r = md_replayer_play_phrase_row(voieCourante);
        }
      }
      const int colRep = 2;   // le chevron, juste apres le numero de ligne
      if (r != repereLigne) {
        if (repereLigne >= 0) efface(colRep, 3 + repereLigne, 1);
        if (r >= 0) { efface(colRep, 3 + r, 1);
                      chevron(colRep, 3 + r, kAccent); }
        repereLigne = r;
      }
    }

    cumulDessin += (unsigned short)(timerTick(2) - tD) - (cumulAudio - audioAvantD);

    // ── L'etat du morceau, sur l'ecran du bas ───────────────────────────
    // La mesure qui vivait ici n'a plus lieu d'etre : tout ce qu'elle disait
    // part maintenant dans le journal de la carte, ou on peut le relire. On
    // rend la place a ce qui sert pendant qu'on travaille, et on la prend en
    // BAS pour degager le haut de l'ecran.
    {
      // ⚠️ « DSI 134MHZ  BPM xxx » est PARTI : c'etait une mesure de mise au
      // point posee sous les yeux en permanence. Le BPM se regle et se lit
      // dans PROJECT, la frequence n'interesse que le journal. Il ne reste
      // que le nom du morceau.
      static char vuBas[3][42] = { "", "", "" };
      char l1[42];
      siprintf(l1, "%s", nomProjet[0] ? nomProjet : "(UNNAMED)");
      if (ecranEfface || strcmp(vuBas[0], l1)) {
        strcpy(vuBas[0], l1);
        // L'ecran du BAS a ses propres coordonnees : il ne doit pas heriter du
        // centrage de la page affichee en haut. Il en heritait, et l'effacement
        // commencait alors trois colonnes trop loin — les premieres lettres du
        // nom precedent survivaient a gauche du nouveau. « LA DIFFE » ecrit
        // par-dessus les restes de lui-meme donnait « LALA DIFFE ».
        const int decalHaut = g_colOrigine;
        g_colOrigine = 0;
        // Pas sur les premieres lignes de l'ecran du bas : la grille de SONG
        // occupe toute la hauteur du haut, et les deux ecrans se touchent sans
        // separation — colle en ligne 0, ce texte se lisait comme la suite de
        // l'ecran du haut. Deux lignes de vide levent le doute.
        ecran(bas);
        efface(0, 2, 41); texte(0, 2, l1, kEntete);
        ecran(g_fond);
        g_colOrigine = decalHaut;
      }

      // ── En QUITTANT la page FM, on rend l'ecran du bas ────────────────
      // Elle y ecrit ses reglages generaux ; sans ce nettoyage ils restaient
      // affiches sous la page suivante, qui n'a rien a voir. Le nom du
      // morceau, lui, appartient a l'ecran du bas et reste.
      { static bool basPrisParFM = false;
        const bool fmIci = (page == PAGE_INSTR &&
                            genreInstr(instrVoie, instrId) == GENRE_FM);
        if (basPrisParFM && !fmIci) {
          const int decalH2 = g_colOrigine;
          g_colOrigine = 0;
          ecran(bas);
          for (int l = 3; l < 19; l++) efface(0, l, 41);
          ecran(g_fond);
          g_colOrigine = decalH2;
          vuBas[0][0] = 0;         // le nom sera repose a l'image suivante
        }
        basPrisParFM = fmIci; }
    }

    // On n'ecrit sur la carte que lecture ARRETEE.
    //
    // Il y avait ici une exception « sauf si le tampon est presque plein :
    // mieux vaut un a-coup qu'une perte ». C'etait un mauvais marche : mesure
    // sur la console, l'a-coup fait jusqu'a 1,9 SECONDE — l'anneau se vide, la
    // lecture saute, et c'est exactement le defaut qu'on cherchait a consigner.
    // Perdre quelques lignes de journal ne coute rien en comparaison ; elles
    // sont comptees et le total est ecrit des qu'on s'arrete.
    if (journalSale && !enLecture) { causeTour = "JOURNAL"; videJournal(); }

    // ── La fenetre de nom, dessinee EN DERNIER ──────────────────────────
    // Elle etait au milieu de la chaine des pages, et une partie de son
    // contenu se faisait repeindre par-dessus par un autre bloc : le cadre et
    // la grille survivaient, le titre et le champ du nom disparaissaient.
    // Une fenetre se pose au-dessus de tout, donc elle se dessine en dernier.
    if (page == PAGE_PROJECT && dialogueNom) {
      // Une fenetre pose ses PROPRES coordonnees : elle ne doit pas heriter du
      // centrage de la page. Le texte passe par texte(), qui applique ce
      // decalage ; les rectangles sont traces en points bruts, qui ne
      // l'appliquent pas. Les deux se retrouvaient decales de trois colonnes,
      // et le curseur d'ecriture tombait au milieu du mot.
      const int decalFenetre = g_colOrigine;
      g_colOrigine = 0;

      // La grille de caracteres, comme dans LSDJ : on s'y promene a la croix
      // et A pose la lettre. C'est bien plus lisible que de faire defiler un
      // alphabet sous un curseur, ou l'on ne voit jamais ce qui vient.
      // Les deux dernieres cases sont le retour arriere et la validation.
      static const char *grille[4] = { "0123456789",
                                       "ABCDEFGHIJ",
                                       "KLMNOPQRST",
                                       "UVWXYZ-_<*" };
      if (assombrirDemande) { assombrit(); assombrirDemande = false; }

      // La position se DEDUIT de la hauteur de l'ecran : elle etait ecrite en
      // dur pour 21 lignes, et l'agrandissement de la police l'a ramenee a 19
      // — la rangee du OK tombait alors hors du cadre. On cale la fenetre sur
      // le bas, une ligne de marge, quelle que soit la taille de police.
      const int fh = 12;
      const int fl = (kLignes - fh - 1) > 1 ? (kLignes - fh - 1) : 1;
      const int fc = 4, fw = 32;
      fenetre(fc, fl, fw, fh);
      // La grille.
      for (int r = 0; r < 4; r++)
        for (int c2 = 0; c2 < 10; c2++) {
          const int cx = fc + 2 + c2 * 3, cy = fl + 5 + r * 2;
          const bool ici = (nomLig == r && nomCol == c2);
          // La derniere case porte OK sur deux caracteres, comme dans LSDJ.
          // Elle est un peu plus large, d'ou le pave elargi quand elle est
          // pointee.
          const bool estOK = (grille[r][c2] == '*');
          if (ici) {
            const int x0 = cx * MD_CELL_W - 1, y0 = cy * MD_CELL_H - 1;
            const int lg = (estOK ? 2 : 1) * MD_CELL_W + 2;
            for (int y = y0; y < y0 + MD_GLYPHE_H + 3; y++)
              for (int x = x0; x < x0 + lg; x++) pixel(x, y, kAccent);
          }
          char c1[2] = { grille[r][c2], 0 };
          texte(cx, cy, estOK ? "OK" : c1, ici ? kFond : kData);
        }

      // Le titre et le champ du nom se dessinent EN DERNIER. Places avant la
      // grille, ils n'apparaissaient pas : quelque chose repeignait cette
      // bande entre-temps. Les mettre a la fin les met hors d'atteinte.
      // L'intitule dit CE QU'ON NOMME : la meme fenetre sert au projet, a la
      // cartouche et au VGM, et sans ca on ne sait plus ce qu'on est en train
      // de faire.
      titre(fc + 2, fl + 1,
            (nomPour == NOM_ROM)     ? "EXPORT PLAYER ROM AS:"
          : (nomPour == NOM_ROM_TRK) ? "EXPORT PROJECT TO MD ROM AS:"
          : (nomPour == NOM_VGM)     ? "EXPORT VGM AS:" : "SAVE AS:", kTitre);
      // La region est montree ET modifiable ici : c'est elle qui decide de la
      // cadence gravee, et se tromper rend le morceau 20 % trop lent sur la
      // console d'en face.
      if (nomPour != NOM_PROJET) {
        // ⚠️ PAS sur la ligne fl+3 : c'est celle du champ du nom, et les deux
        // textes se recouvraient — on ne lisait plus ni l'un ni l'autre.
        // La region va SOUS le nom, avec les DEUX choix affiches : celui qui
        // est retenu en pleine couleur, l'autre eteint. Montrer seulement le
        // choisi ne dit pas qu'il y en a un autre, ni comment y aller.
        texte(fc + 18, fl + 1, "L=PAL R=NTSC", kAttenue);
        efface(fc + 2, fl + 4, fw - 4);
        titre(fc + 2,  fl + 4, "PAL",  romPal ? kAccent : kAttenue);
        titre(fc + 7,  fl + 4, "NTSC", romPal ? kAttenue : kAccent);
        texte(fc + 14, fl + 4, romPal ? "49.70HZ" : "59.92HZ", kData);
      }

      // Le champ du nom, sur fond plein, avec le curseur d'ecriture.
      { const int x0 = (fc + 2) * MD_CELL_W - 1, y0 = (fl + 3) * MD_CELL_H - 1;
        const int x1 = x0 + 14 * MD_CELL_W, y1 = y0 + MD_GLYPHE_H + 3;
        for (int y = y0; y < y1; y++)
          for (int x = x0; x < x1; x++) pixel(x, y, kAttenue); }
      { char n2[10]; int j = 0;
        for (; j < 8 && sauveNom[j]; j++) n2[j] = sauveNom[j];
        n2[j] = 0;
        texte(fc + 2, fl + 3, n2, kFond);
        // Le pave d'ecriture, la ou la prochaine lettre ira.
        const int cx = (fc + 2 + j) * MD_CELL_W;
        const int cy = (fl + 3) * MD_CELL_H;
        for (int y = cy; y < cy + MD_GLYPHE_H; y++)
          for (int x = cx; x < cx + MD_GLYPHE_W; x++) pixel(x, y, kFond);
        // L'extension REELLE du fichier qu'on va ecrire. Elle etait figee sur
        // « .MDM », l'extension d'un projet : on lisait donc « .MDM » en
        // exportant une cartouche ou un VGM.
        titre(fc + 17, fl + 3,
              (nomPour == NOM_ROM || nomPour == NOM_ROM_TRK) ? ".BIN"
            : (nomPour == NOM_VGM) ? ".VGM" : ".MDM", kAttenue); }
      g_colOrigine = decalFenetre;
    }

    // ── Le message passager ─────────────────────────────────────────────
    // Il etait pose sur la LIGNE 0, a partir de la colonne 12 : le nom du
    // fichier exporte passait donc par-dessus le titre de la page, ecrit a
    // droite de cette meme ligne. Il vit maintenant une ligne plus bas, et
    // centre : il ne recouvre plus rien et se lit d'un coup d'oeil.
    {
      static char vuMsg[40] = "";
      char aVoir[40] = "";
      if (msgProjet[0] && secondes < msgProjetJusqu) strcpy(aVoir, msgProjet);
      else msgProjet[0] = 0;
      if (strcmp(aVoir, vuMsg)) {
        strcpy(vuMsg, aVoir);
        const int sauve = g_colOrigine, sauveY = g_decalYpx;
        g_colOrigine = 0; g_decalYpx = 0;
        efface(0, 1, kCols);
        if (aVoir[0]) {
          // Centre : la police des titres est plus large, on compte en cellules.
          const int lg = (int)strlen(aVoir);
          const int c0 = (kCols - lg) / 2;
          titre(c0 > 0 ? c0 : 0, 1, aVoir, kAccent);
        }
        g_colOrigine = sauve; g_decalYpx = sauveY;
      }
    }

    swiWaitForVBlank();
  }
  return 0;
}
