| ============================================================================
|  Lecteur embarqué dans la ROM Mega Drive.
|
|  Il ne rejoue PAS le tracker : il rejoue ce que le tracker a ÉCRIT DANS LES
|  PUCES, image par image. La cartouche entend donc exactement ce que l'iPad
|  fait entendre, sans réécrire le moteur en 68000 — et sans risquer d'écart
|  entre deux implémentations.
|
|  Le journal est produit par l'application (md_rom.c) et posé à MD_DATA.
|  Format, tout en gros-boutiste :
|      par image : entête 16 bits = nombre d'écritures, bit 15 = un bloc de
|                  convertisseur suit ; puis autant de triplets
|                  [port, a, b] :  port 0 = YM2612 banc 0 (a=registre, b=valeur)
|                                  port 1 = YM2612 banc 1
|                                  port 2 = SN76489 (a = octet, b ignoré)
|                  ...puis, si le bit 15 était levé, 444 octets pour le
|                  convertisseur, à débiter régulièrement pendant l'image.
|      fin du morceau : entête = 0xFFFF -> on reboucle.
|
|  Assemblé par m68k-elf-as. L'assembleur ne part PAS dans l'application : il
|  ne sert qu'à fabriquer les octets, qui sont embarqués tels quels.
| ============================================================================

        .text
        .globl  _start

| ── Adresses matérielles ───────────────────────────────────────────────────
        .equ    YM_A0,   0xA04000        | banc 0 : adresse
        .equ    YM_D0,   0xA04001        | banc 0 : donnée
        .equ    YM_A1,   0xA04002        | banc 1 : adresse
        .equ    YM_D1,   0xA04003        | banc 1 : donnée
        .equ    PSG,     0xC00011        | SN76489
        .equ    VDP_CTRL,0xC00004        | contrôle VDP (état en lecture)
        .equ    Z80_BUS, 0xA11100
        .equ    Z80_RST, 0xA11200
        .equ    MD_DATA, 0x00020000      | le journal, posé par le constructeur
| Le pas d'attente entre deux échantillons du convertisseur vit maintenant en
| bas de ce fichier (`dac_delai`), avec la taille du bloc : le constructeur les
| écrit dans la ROM, ce qui permet de CALIBRER par la mesure au lieu de calculer.
| Le calcul, justement, était faux : le commentaire qui tenait ici annonçait
| 39 + 14,2 x pas cycles par échantillon, et donnait 18. La mesure — une ROM de
| diagnostic débitant un carré de période connue, jouée dans un vrai cœur Mega
| Drive — dit 80 + 13,9 x pas. Avec 18 le convertisseur sortait à 23 252 Hz au
| lieu de 26 634 : tous les samples deux demi-tons trop bas. C'est 15 qu'il
| fallait (26 604 Hz mesurés, 0,1 % d'écart).
| Rien n'est réglé sur un morceau : ce sont les cadences de la machine.

_start:
        move.w  #0x2700,%sr              | plus aucune interruption

| ── TMSS ───────────────────────────────────────────────────────────────────
| Sur les Mega Drive récentes (et les émulateurs qui les imitent), le VDP reste
| VERROUILLÉ tant qu'on n'a pas écrit « SEGA » dans $A14000. Sans ça l'écran
| reste noir ET notre attente d'image tourne dans le vide, puisqu'elle lit
| l'état du VDP : la lecture ne démarre jamais. Le registre de version dit si
| la machine a ce verrou ; sur les anciennes, il ne faut PAS écrire.
        move.b  0xA10001,%d0
        andi.b  #0x0F,%d0
        beq.s   no_tmss
        move.l  #0x53454741,0xA14000     | « SEGA »
no_tmss:

| ── Le Z80 se tait ─────────────────────────────────────────────────────────
| On demande son bus et on le garde : sans ça il exécute de la mémoire vide et
| peut parler aux puces en même temps que nous.
        move.w  #0x0100,Z80_BUS
        move.w  #0x0100,Z80_RST
z80_wait:
        btst    #0,Z80_BUS
        bne.s   z80_wait

| ── VDP : écran éteint, aucune interruption ────────────────────────────────
| On ne dessine rien, mais le VDP doit être dans un état défini — et c'est lui
| qui donne la cadence, par son bit de retour vertical.
| ⚠️ L'affichage doit être ALLUMÉ, même si l'on ne dessine rien : quand il est
| éteint, le VDP garde son drapeau de retour vertical LEVÉ en permanence. La
| première attente — celle qui guette sa retombée — ne sortait donc jamais, et
| la lecture s'arrêtait après la toute première image. L'écran reste noir de
| toute façon, puisqu'on n'y met aucune tuile.
        move.w  #0x8004,VDP_CTRL         | reg 0 : pas d'interruption H
        move.w  #0x8174,VDP_CTRL         | reg 1 : AFFICHAGE ALLUMÉ, mode 5
        move.w  #0x8C81,VDP_CTRL         | reg 12 : 40 colonnes
        move.w  #0x8700,VDP_CTRL         | reg 7 : fond = couleur 0 (noir)

| ── Le YM2612 se tait ──────────────────────────────────────────────────────
| Key-off sur les six voies, DAC débranché : on part d'un silence propre.
        lea     YM_A0,%a1
        moveq   #0,%d0
ym_hush:
        move.b  #0x28,(%a1)              | registre de key-on/off
        move.b  %d0,1(%a1)               | voie n, tous opérateurs relâchés
        addq.b  #1,%d0
        cmp.b   #4,%d0
        bne.s   ym_no_skip
        moveq   #4,%d0                   | les voies sont 0-2 puis 4-6
ym_no_skip:
        cmp.b   #7,%d0
        bne.s   ym_hush
        move.b  #0x2B,(%a1)
        move.b  #0x00,1(%a1)             | DAC débranché

| ── Le SN76489 se tait ─────────────────────────────────────────────────────
        lea     PSG,%a2
        move.b  #0x9F,(%a2)
        move.b  #0xBF,(%a2)
        move.b  #0xDF,(%a2)
        move.b  #0xFF,(%a2)

| ── Boucle principale : une image, une passe ───────────────────────────────
song_start:
        lea     MD_DATA,%a0
frame:
        move.w  (%a0)+,%d0               | entête de l'image
        cmp.w   #0xFFFF,%d0
        beq.s   song_start               | fin du morceau : on reboucle
        move.w  %d0,%d4                  | on garde le drapeau du convertisseur
        andi.w  #0x7FFF,%d0              | ...et on isole le nombre d'écritures
        subq.w  #1,%d0
        bmi.s   frame_writes_done        | image sans écriture de registre
write_loop:
        move.b  (%a0)+,%d1               | port
        move.b  (%a0)+,%d2               | a
        move.b  (%a0)+,%d3               | b
        cmp.b   #2,%d1
        beq.s   to_psg
        tst.b   %d1
        bne.s   to_ym1

| ── Écrire dans le YM2612 ──────────────────────────────────────────────────
| Écriture directe : adresse puis donnée, sans interroger le drapeau
| d'occupation de la puce. J'avais ajouté cette interrogation en croyant que des
| écritures perdues expliquaient le son faux ; le test l'a démentie, et une fois
| le lecteur réparé elle rendait la cartouche muette — sur une machine qui ne
| baisse jamais ce bit, chaque attente allait au bout de sa borne. À ce débit
| (19 écritures par image en moyenne), les pilotes Mega Drive écrivent à la
| file, comme ici.
to_ym0:
        move.b  %d2,YM_A0
        move.b  %d3,YM_D0
        bra.s   next_write
to_ym1:
        move.b  %d2,YM_A1
        move.b  %d3,YM_D1
        bra.s   next_write
to_psg:
        move.b  %d2,PSG
next_write:
        dbra    %d0,write_loop

| ── Se remettre sur une adresse paire ──────────────────────────────────────
| Un triplet fait 3 octets. Après un nombre impair d'écritures, %a0 se retrouve
| sur une adresse impaire — et le `move.w` qui lit le compteur de l'image
| suivante déclencherait une ERREUR D'ADRESSE, qui dérouterait le processeur et
| ferait dérailler tout le reste. Le constructeur a complété l'image d'un octet
| dans ce cas : on le saute.
        move.l  %a0,%d7
        btst    #0,%d7
        beq.s   frame_writes_done
        addq.l  #1,%a0

frame_writes_done:
| ── Le convertisseur ───────────────────────────────────────────────────────
| Le PCM ne peut pas passer par le journal d'écritures : il est alimenté à la
| cadence native du YM2612, ce qu'aucun journal n'absorbe. Son flux est donc
| stocké à part, déjà tout calculé par le moteur — pas de lecture, volume,
| écrêtage et bouclage compris. La cartouche ne refait aucun de ces calculs,
| elle débite les octets ; elle ne peut donc pas en diverger.
|
| Le registre 2A est armé UNE fois : dans cette boucle rien d'autre n'écrit
| dans la puce, l'adresse reste donc verrouillée et chaque échantillon ne coûte
| qu'une écriture.
        btst    #15,%d4
        beq.s   frame_end
        move.l  %a0,%a1                  | on retient le début du bloc
        move.b  #0x2A,YM_A0              | on arme le registre du convertisseur
| La taille du bloc n'est plus figee a 444 : elle depend de la cadence du
| journal, 60 images par seconde sur une console NTSC et 50 sur une PAL. Le
| constructeur la pose dans `dac_taille` (voir tout en bas) ; on la lit ici.
        move.w  dac_taille,%d5
        subq.w  #1,%d5                   | dbra compte jusqu'a zero inclus

| Le débit ne se règle pas à la durée : il s'arrête sur le RETOUR VERTICAL, donc
| au bout d'exactement une image, que celle-ci ait porté 4 écritures de
| registres ou 200. Une durée fixe ne pouvait pas tenir : 9 % des images à PCM
| passent 83 écritures, de quoi déborder — et une image débordée en coûte une
| entière. Ici, ce qui n'a pas eu le temps de sortir est simplement sauté ;
| quelques échantillons perdus s'entendent bien moins qu'un trou périodique.
|
| On arrive ici PENDANT le retour vertical (c'est là que l'attente d'image nous
| a laissés). Il faut donc d'abord le laisser finir, puis guetter le suivant.
dac_haut:
        move.b  (%a0)+,YM_D0
        move.w  dac_delai,%d6
1:      subq.w  #1,%d6
        bne.s   1b
        move.w  VDP_CTRL,%d7
        btst    #3,%d7
        beq.s   dac_bascule              | il est retombé : l'image active commence
        dbra    %d5,dac_haut
        bra.s   dac_epuise
dac_bascule:
        dbra    %d5,dac_bas
        bra.s   dac_epuise

dac_bas:
        move.b  (%a0)+,YM_D0
        move.w  dac_delai,%d6
2:      subq.w  #1,%d6
        bne.s   2b
        move.w  VDP_CTRL,%d7
        btst    #3,%d7
        bne.s   dac_fin_image            | le voilà : l'image est finie
        dbra    %d5,dac_bas

dac_epuise:
| Le bloc est sorti avant la fin de l'image : on se recale normalement.
        move.l  %a1,%a0
        adda.w  dac_taille,%a0
        bra.s   frame_end
dac_fin_image:
| L'image s'est achevée : on saute ce qui n'a pas eu le temps de sortir, et on
| repart sans attendre — on EST déjà au début du retour vertical.
        move.l  %a1,%a0
        adda.w  dac_taille,%a0
        bra     frame

frame_end:
| ── Attendre le retour vertical ────────────────────────────────────────────
| Le bit 3 de l'état du VDP est levé pendant le retour vertical. On attend
| qu'il retombe puis qu'il se lève : une image entière, ni plus ni moins.
| Chaque attente est BORNÉE. Si un jour le drapeau ne bougeait pas — machine
| inattendue, mode vidéo particulier —, mieux vaut une lecture qui file trop
| vite qu'une cartouche muette : le son reste, et le défaut s'entend au lieu de
| tout arrêter. La borne le tient VRAIMENT : une image dure ~128 000 cycles et
| un tour de cette boucle ~46, donc ~2 800 tours suffisent largement ; 8 000 en
| couvrent trois. L'ancienne borne de 200 000 valait 1,2 s par image — soit
| précisément la cartouche muette qu'elle prétendait éviter.
        move.l  #8000,%d6
vb_low:
        move.w  VDP_CTRL,%d7
        btst    #3,%d7
        beq.s   vb_low_done
        subq.l  #1,%d6
        bne.s   vb_low
vb_low_done:
        move.l  #8000,%d6
vb_high:
        move.w  VDP_CTRL,%d7
        btst    #3,%d7
        bne.s   vb_high_done
        subq.l  #1,%d6
        bne.s   vb_high
vb_high_done:
        bra     frame

| ── Les deux valeurs que le constructeur ecrit dans le lecteur ─────────────
| Elles DOIVENT rester les deux derniers mots du lecteur : md_rom.c les corrige
| a `sizeof(md_player_bin) - 4` et `- 2`. Ne rien ajouter apres.
|
| `dac_taille` : echantillons par image, 444 a 60 Hz et 533 a 50 Hz.
| `dac_delai`  : le pas d'attente entre deux echantillons. Il a ete CALIBRE, pas
|                calcule : une ROM de diagnostic debitant un carre de periode
|                connue, jouee dans un vrai coeur Mega Drive, donne la cadence
|                reelle. Le modele en commentaire (39 + 14,2 x pas) etait faux
|                de 12 % — le convertisseur sortait a 23 252 Hz au lieu de
|                26 634, soit deux demi-tons trop bas sur tous les samples.
        .align  2
| ⚠️ `dac_taille` doit etre PAIRE. Le bloc est saute d'un coup (`adda.w`) : une
| taille impaire laisse %a0 sur une adresse impaire, le `move.w` de l'image
| suivante declenche une ERREUR D'ADRESSE, le 68000 deroute et la cartouche
| redemarre en boucle. Essaye avec 533 : deux secondes de son sur cent.
        .align  2
dac_taille:
        .word   444
dac_delai:
        .word   15
