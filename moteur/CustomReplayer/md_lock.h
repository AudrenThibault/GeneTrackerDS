#ifndef MD_LOCK_H
#define MD_LOCK_H

// ── Le verrou du moteur, selon la machine ───────────────────────────────────
// Le moteur est PARTAGÉ, tel quel, entre le tracker iPad et le tracker
// Nintendo DS. Sur iOS l'interface et le fil audio touchent le même état en
// même temps, et il faut un vrai verrou. Sur DS il n'y a pas de fil audio :
// l'ARM9 remplit sa réserve de son depuis la boucle principale, entre deux
// images, donc rien ne peut se marcher dessus.
//
// ⚠️ Si un jour la DS venait à générer son son depuis une interruption, ce
// serait FAUX et il faudrait le corriger ici — c'est le seul endroit du moteur
// où cette différence existe, et elle doit y rester.

#if defined(__APPLE__)
  #include <os/lock.h>
  typedef os_unfair_lock md_lock_t;
  #define MD_LOCK_INIT      OS_UNFAIR_LOCK_INIT
  #define md_lock(l)        os_unfair_lock_lock(l)
  #define md_unlock(l)      os_unfair_lock_unlock(l)
  #define md_trylock(l)     os_unfair_lock_trylock(l)
#else
  #include <stdbool.h>
  typedef int md_lock_t;
  #define MD_LOCK_INIT      0
  #define md_lock(l)        ((void)(l))
  #define md_unlock(l)      ((void)(l))
  #define md_trylock(l)     (true)
#endif

#endif
