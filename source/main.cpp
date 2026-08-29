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
#include <maxmod9.h>
#include <stdio.h>
#include <string.h>

#include "md_font.h"
#include "morceau_dmf.h"

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
  int x0 = col * MD_CELL_W, y0 = lig * MD_CELL_H;
  for (int y = y0; y < y0 + MD_CELL_H; y++)
    for (int x = x0; x < x0 + n * MD_CELL_W; x++)
      pixel(x, y, (y & 1) ? rvb(0, 1, 1) : kFond);
}

// ── Le son ──────────────────────────────────────────────────────────────────
// Le moteur remplit le tampon depuis la BOUCLE PRINCIPALE, jamais depuis une
// interruption : maxmod est ouvert en mode « manuel » et c'est nous qui
// appelons mmStreamUpdate. C'est ce que suppose md_lock.h, qui n'installe
// aucun verrou sur DS — si un jour ça changeait, il faudrait le corriger la-bas.
static mm_word flux_demande(mm_word longueur, mm_addr dest, mm_stream_formats f) {
  (void)f;
  md_replayer_update((uint8_t *)dest, (int)longueur * 4);   // stereo 16 bits
  return longueur;
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
  md_replayer_init(32768);          // cadence de sortie de la DS
  // SANS module, le moteur sort du silence sans rien calculer — ce qui
  // expliquait a la fois l'absence de son ET la charge processeur a zero.
  md_replayer_new_empty();

  // Le morceau de l'iPad, embarque tel quel. C'est LE test : si ce .dmf
  // s'ouvre et sonne ici, la compatibilite des projets entre les deux trackers
  // est prouvee, et la mesure de charge porte enfin sur de la vraie musique.
  md_dmf_report_t rapport;
  bool charge = md_replayer_import_dmf(morceau_dmf, morceau_dmf_len, &rapport);

  // ── Le son ────────────────────────────────────────────────────────────
  mm_ds_system sys;
  sys.mod_count = 0; sys.samp_count = 0; sys.mem_bank = 0;
  mmInit(&sys);

  mm_stream flux;
  flux.sampling_rate = 32768;
  // 4096 echantillons, soit 125 ms de reserve.
  //
  // Mesure a l'appui : le rendu occupe 87 % de l'ARM9 et arrive par paquets de
  // ~72 ms. Avec les 31 ms que donnaient 1024 echantillons, le tampon se vidait
  // entre deux paquets — c'est ce qui hachait le son. Il faut plus de reserve
  // que le plus gros paquet, sinon aucune cadence de remplissage ne suffit.
  flux.buffer_length = 4096;
  flux.callback      = flux_demande;
  flux.format        = MM_STREAM_16BIT_STEREO;
  flux.timer         = MM_TIMER0;
  flux.manual        = true;        // c'est NOUS qui remplissons, depuis la boucle
  mmStreamOpen(&flux);

  if (charge) {
    md_replayer_set_play_scope(MD_SCOPE_SONG, 0, 0);
    md_replayer_play_from(0);
    md_replayer_play();
  }

  // ── Page SONG ─────────────────────────────────────────────────────────
  // Dix canaux : six FM (dont FM6 qui devient le PCM) et quatre PSG.
  static const char *noms[10] = {"FM1","FM2","FM3","FM4","FM5","PCM",
                                 "SQ1","SQ2","SQ3","NOI"};
  texte(0, 0, charge ? "MD TRACKER DS" : "DMF REFUSE", kEntete);
  {
    // Le tempo relu du morceau : s'il est aberrant, c'est lui qui mettait le
    // nombre d'echantillons par tic a zero.
    int bpm = (int)md_replayer_get_bpm();
    if (bpm < 0) bpm = 0; if (bpm > 999) bpm = 999;
    char t[10] = {'B','P','M',' ',
                  (char)('0'+(bpm/100)%10), (char)('0'+(bpm/10)%10),
                  (char)('0'+bpm%10), 0};
    texte(34, 0, t, kEntete);
  }
  texte(52, 0, "SONG", kEntete);
  for (int c = 0; c < 10; c++) texte(4 + c * 6, 2, noms[c], kEntete);

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

  int curCanal = 0, curLigne = 0, haut = 0;
  const int kLignesVues = 26;

  // On ne SORT PAS de main() sur une console : il n'y a nulle part ou revenir,
  // et la cartouche plante. pmMainLoop() gere l'ouverture du clapet et
  // l'extinction ; on tourne dedans jusqu'a ce qu'elle dise stop.
  while (pmMainLoop()) {
    unsigned tA = timerTick(2);
    // UNE seule fois par image. J'avais essaye deux passes pour donner de la
    // marge au tampon : ca a bloque la boucle avant meme le premier dessin.
    // A ce rythme la, le moteur n'a deja pas le temps d'en faire une.
    mmStreamUpdate();
    unsigned tB = timerTick(2);
    cumulAudio += (unsigned short)(tB - tA);   // soustraction 16 bits : le
    cumul     += (unsigned short)(tB - tPrec); // bouclage du compteur est gere
    tPrec = tB;
    tours++;
    if (cumul >= kTicksParSeconde) {
      fpsVu = (int)tours;
      partAudio = (int)((unsigned long long)cumulAudio * 100 / cumul);
      cumul = 0; cumulAudio = 0; tours = 0;
    }

    scanKeys();
    int appui = keysDownRepeat();
    if (appui & KEY_LEFT)  curCanal = (curCanal + 9) % 10;
    if (appui & KEY_RIGHT) curCanal = (curCanal + 1) % 10;
    if (appui & KEY_UP)    curLigne = (curLigne + MD_SONG_ROWS - 1) % MD_SONG_ROWS;
    if (appui & KEY_DOWN)  curLigne = (curLigne + 1) % MD_SONG_ROWS;
    // La vue suit le curseur sans jamais le coller au bord.
    if (curLigne < haut + 2) haut = curLigne - 2;
    if (curLigne > haut + kLignesVues - 3) haut = curLigne - kLignesVues + 3;
    if (haut < 0) haut = 0;
    if (haut > MD_SONG_ROWS - kLignesVues) haut = MD_SONG_ROWS - kLignesVues;

    // ── Redessin ────────────────────────────────────────────────────────
    int f = fpsVu > 99 ? 99 : fpsVu, a = partAudio > 99 ? 99 : partAudio;
    char m[16] = {'I','P','S',' ',
                  (char)('0'+f/10), (char)('0'+f%10),
                  ' ','S','O','N',' ',
                  (char)('0'+a/10), (char)('0'+a%10), '%', 0};
    efface(16, 0, 14);
    texte(16, 0, m, fpsVu < 50 ? rvb(31, 10, 8) : kEntete);

    // On ne repeint QUE si quelque chose a bouge.
    //
    // Mesure a l'appui : en repeignant les 260 cases a chaque tour, la boucle
    // tombait a 11 tours par seconde au lieu de 60, dont 11 % seulement dans le
    // rendu audio. Ce n'est donc PAS l'emulation du YM2612 qui coute cher —
    // c'est ce dessin, fait pixel par pixel. Et un tampon audio nourri 11 fois
    // par seconde se vide : c'est ce qui rendait le son inecoutable.
    static int vuCanal = -1, vuLigne = -1, vuHaut = -1;
    const bool aChange =
        (curCanal != vuCanal || curLigne != vuLigne || haut != vuHaut);
    vuCanal = curCanal; vuLigne = curLigne; vuHaut = haut;

    for (int l = 0; aChange && l < kLignesVues; l++) {
      int ligne = haut + l;
      char num[3];
      num[0] = kHex[(ligne >> 4) & 15]; num[1] = kHex[ligne & 15]; num[2] = 0;
      efface(0, 4 + l, 2);
      texte(0, 4 + l, num, ligne == curLigne ? kEntete : kAttenue);
      for (int c = 0; c < 10; c++) {
        uint8_t v = md_replayer_get_song(c, ligne);
        char cel[4];
        if (v == MD_EMPTY) { cel[0]='-'; cel[1]=' '; cel[2]='-'; }
        else { cel[0]=kHex[(v>>4)&15]; cel[1]=' '; cel[2]=kHex[v&15]; }
        cel[3] = 0;
        int col = 4 + c * 6;
        bool ici = (c == curCanal && ligne == curLigne);
        efface(col, 4 + l, 3);
        if (ici) {
          // Le curseur : un pave plein, comme dans LSDJ.
          int x0 = col * MD_CELL_W, y0 = (4 + l) * MD_CELL_H;
          for (int y = y0; y < y0 + MD_FONT_HT; y++)
            for (int x = x0 - 1; x < x0 + 3 * MD_CELL_W; x++) pixel(x, y, kEntete);
        }
        texte(col, 4 + l, cel, ici ? kFond : kAttenue);
      }
    }

    swiWaitForVBlank();
  }
  return 0;
}
