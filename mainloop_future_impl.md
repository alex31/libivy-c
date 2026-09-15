# Reprise : intégration des mainloops dans le wrapper C++23

Relevé initial du 14 septembre 2026, mis à jour le 15 septembre après
implémentation du pilotage natif C++ et de la création simplifiée avec GLib.
`Bus::run()`, `IvyContextRun()`, `ivy::glib::create_bus()` et la variante
`ivy-cpp-glib` sont maintenant implémentés. Le complément Qt livre également
`Bus::request_stop()` et `ivy::LoopThread` dans `ivy_thread.hpp`. Les tâches,
canaux, hooks et autres adaptateurs ci-dessous restent des propositions de suite.
Le périmètre exclut toujours toute API C++ d’itération manuelle.

## 1. Objectif et contexte utilisateur

Compléter le wrapper C++23 avec le pilotage bloquant de la boucle native Ivy,
les canaux d'entrée/sortie, les hooks, le réveil et la transmission de travail
depuis d'autres threads, ainsi que l’intégration aux boucles d’événements hôtes.
Un simple habillage de `IvyContextMainLoop()` ne termine donc pas ce chantier.

### Choix utilisateur du 15 septembre 2026

- Ne fournir aucune API C++ d’itération manuelle : ni `idle()`, ni `poll()`,
  ni équivalent de `IvyContextIdle()`.
- Privilégier la boucle native bloquante, sur le thread appelant ou dans un
  thread explicitement créé, et l’attachement aux boucles hôtes qui permettent
  d’enregistrer les sources I/O, les timers et le réveil nécessaires.
- L’intégration à une boucle hôte ne doit pas reposer sur un appel périodique
  à `IvyContextIdle()`. GLib est le premier backend C déjà disponible ; les
  autres frameworks restent à sélectionner et leurs adaptateurs à valider.
- La correction des timers d’`IvyContextIdle()` côté C est différée, hors de
  ce chantier et de ses prérequis. L’API C existante reste disponible.

Préférences déjà exprimées et à conserver :

- C++23 idiomatique : `std::expected`, `std::error_code`, `std::chrono`, callbacks
  acceptant des captures non copiables, objets de durée de vie explicite.
- Pas d'exceptions propagées par le wrapper ; pas de `throw`, `try` ou `catch`
  dans les exemples d'application. Les interceptions internes restent nécessaires
  pour convertir les échecs de la bibliothèque standard et des callbacks.
- Toutes les erreurs récupérables doivent être consultables. Conserver les
  détails de `SendReport` et le mécanisme `take_callback_error()`.
- Isolation entre bus : aucune nouvelle configuration globale au processus.
- `ivy.hpp` doit rester une porte d'entrée lisible, avec son exemple complet
  annoté. Conserver la documentation Doxygen et les exemples dans les petits
  en-têtes thématiques ; ne pas les réduire pour raccourcir les fichiers.
- Utiliser `#pragma once` dans les nouveaux en-têtes, en respectant le mécanisme
  d'assemblage des fragments existants décrit plus bas.
- Les helpers C++ de durée de vie appartiennent à `src/cpp/`. Des primitives C
  peuvent être ajoutées si elles sont nécessaires au bon fonctionnement. Le
  passage en version 3.18 autorise les ruptures nécessaires ; cela ne demande
  pas de reconfirmation systématique.
- Documenter les modifications pour préparer des commits incrémentaux C puis
  C++. Ne pas commiter ou indexer automatiquement les modifications antérieures.

La demande ayant produit ce fichier était uniquement de préparer la reprise.
Les signatures, adaptateurs précis et choix d’organisation ci-dessous restent
des propositions techniques. Le périmètre du 15 septembre ci-dessus est une
décision utilisateur acquise.

## 2. Reprendre le bon arbre de travail

Le HEAD au moment de la rédaction est
`85b2c27686faffeeec6873d257f76af4eaff9936`, version déclarée 3.18.
L'essentiel des dernières évolutions C/C++ est encore **non commité**, avec
plusieurs fichiers nouveaux non suivis. Un clone de ce HEAD et ce seul document
ne suffisent pas pour retrouver l'état décrit ici.

Sur l'autre PC, reprendre également les modifications et nouveaux fichiers du
wrapper, du cœur C, des tests, des exemples, du build et de la documentation.
`git diff` seul n'emporte pas les fichiers non suivis. Utiliser le transfert de
travail ou les commits préparés par l'utilisateur, puis vérifier :

```sh
git status --short
git rev-parse HEAD
git diff --stat
```

Lire [CPP_API_WORKLOG.md](CPP_API_WORKLOG.md) pour le journal et le découpage des
lots précédents. Il décrit notamment les filtres par contexte, les copies
privées C pour les requêtes C++, les timers et le découpage des en-têtes.

Les fichiers/répertoires `AGY_DEMO/`, `REVIEW_MASTER_HEAD.md` et
`diff_to_review.patch` préexistaient à ces travaux. Les laisser hors des commits
de cette tâche, sauf instruction ultérieure de l'utilisateur.

Ne pas dépendre des exécutables, comparaisons Doxygen ou scripts temporaires du
PC précédent dans `/tmp` : ils ne font pas partie du dépôt. Sur une autre
architecture, reconstruire les objets ; le Makefile Linux utilise par défaut
`-march=x86-64-v3` sur x86-64, surchargeable avec `X86_64_CFLAGS`.

## 3. Ce qui fonctionne déjà : ne pas le réimplémenter

Le point d'entrée public est [src/cpp/ivy.hpp](src/cpp/ivy.hpp), installé sous
`Ivy/ivy.hpp`. Le wrapper est maintenant dans `src/cpp`, malgré les mentions
historiques de `./cpp` dans la conversation.

| Fonctionnalité déjà couverte | Interface C++ / emplacement |
| --- | --- |
| Boucle bloquante et arrêt sans attente | `Bus::run`, `request_stop` ; `api/mainloop.hpp`, `ivy_mainloop.cpp` |
| Helper de thread optionnel | `LoopThread::create`, `request_stop`, `join` ; `ivy_thread.hpp`, `ivy_thread.cpp` |
| Création sur GLib, contexte courant ou explicite | `ivy::glib::create_bus()` ; `ivy_glib.hpp`, `ivy_glib.cpp`, package `ivy-cpp-glib` |
| Création, déplacement, destruction, start/stop, état, handle natif | `Bus::create`, `start`, `stop`, `state`, `native_handle` ; `api/lifecycle.hpp` |
| Messages par regexp, modification, désabonnement | `bind`, `bind_unanchored`, `Subscription::change/unbind` ; `api/messages.hpp`, `api/subscriptions.hpp` |
| Messages directs | `bind(callback)` et `send(peer, id, text)` ; `api/messages.hpp`, `api/send.hpp` |
| Broadcast avec bilan détaillé | `send`, `send_report` ; `api/send.hpp`, `api/results.hpp` |
| Événements application et demande de terminaison | Callbacks de `Bus::create` ; `api/lifecycle.hpp` |
| Pong et abonnements distants | `bind(callback, ivy::pong/remote_bindings)` ; `api/callbacks.hpp` |
| Erreurs de transport | `set_transport_error_callback` ; `api/callbacks.hpp` |
| Contrôles | `send_ping`, `send_die`, `send_error` ; `api/send.hpp` |
| Requêtes sur les pairs | `find_application`, `application`, `applications`, `application_regexps` ; `api/applications.hpp` |
| Filtres indépendants par bus | `set_filters` pack/range/liste, `add_filter`, `remove_filter`, `clear_filters` ; `api/filters.hpp` |
| Timers périodiques, limités, ponctuels | `bind(callback, every(period[, count]))`, `bind(callback, after(delay))` ; `api/timers.hpp`, `api/timer_types.hpp` |
| Validation autonome de regexp | `validate_anchored_regexp` ; `api/regexp.hpp` |

`Bus::application(peer)` renvoie une paire de chaînes possédées `(nom, hôte)`.
Les handles `IvyClientPtr` restent empruntés à une connexion. Les chaînes et
captures de messages reçues comme vues ne sont valables que pendant le callback.
Une tâche différée doit copier les données dont elle aura besoin.

Les timers disposent déjà d'un token mobile `TimerSubscription`, avec
`unbind()`, `is_bound()` et `set_period()`. Ne pas créer un second système de
timers uniquement pour l'intégration de la boucle.

Les exemples utilisent désormais `bus.run()` et vérifient son résultat, puis
`bus.take_callback_error()`. L’exemple `examples/cpp/glib.cpp` conserve une
boucle GLib applicative qui continue après l’arrêt d’Ivy.

Le wrapper ne lance aucun thread automatiquement. Sa documentation demande
d'arrêter et de joindre le thread de boucle avant destruction ou remplacement
du bus par affectation de déplacement. Détruire le bus depuis un callback est
interdit. Ces contraintes s’appliquent aussi à `run()`.

## 4. Carte du code à modifier

| Fichier | Rôle et points d'entrée utiles |
| --- | --- |
| [src/ivy.c](src/ivy.c) | Définition privée de `IvyContext`, champ `ivy_loop`, `IvyContextMainLoop`, `IvyContextIdle`, `IvyContextStop`, `IvyContextStopInLoop`, `IvyContextDestroy`, création des timers contextuels |
| [src/ivy.h](src/ivy.h) | API C publique ; `IvyContextRun` retourne un statut, `MainLoop` et `Idle` gardent leur signature `void` |
| [src/ivychannel.h](src/ivychannel.h) | États opaques, canaux, contrôles, réveil et identification du thread de boucle |
| [src/ivyloop.h](src/ivyloop.h) | Boucle, itération, notifications d'écriture, hooks avant/après `select` |
| [src/ivyloop.c](src/ivyloop.c) | Backend `select`, pipe/socket de réveil, file des contrôles et destruction différée des canaux |
| [src/timer.c](src/timer.c), [src/timer.h](src/timer.h) | Timers du backend `select`, calcul de prochaine échéance et `TimerScanFor` |
| [src/ivyglibloop.c](src/ivyglibloop.c), [src/ivyglibtimer.c](src/ivyglibtimer.c) | Backend GLib contextuel déjà implémenté en C |
| [src/ivysocket.c](src/ivysocket.c) | Utilisation interne des canaux ; vérifier les effets des modifications de suppression/dispatch |
| [src/cpp/ivy_internal.hpp](src/cpp/ivy_internal.hpp) | `Bus::Impl`, mutex, stockage des callbacks et relais ; non installé |
| [src/cpp/ivy.cpp](src/cpp/ivy.cpp) | Cycle de vie, conversion des erreurs, `save_callback_error`, trampolines application/die/transport |
| [src/cpp/ivy_subscription.cpp](src/cpp/ivy_subscription.cpp), [src/cpp/ivy_events.cpp](src/cpp/ivy_events.cpp) | Modèles de tokens et de remplacement sûr de callbacks |
| [src/cpp/ivy_timer.cpp](src/cpp/ivy_timer.cpp) | Modèle à lire avant toute modification de concurrence ou de scheduling |
| [src/cpp/ivy_detail.hpp](src/cpp/ivy_detail.hpp) | Templates, conversion des exceptions et construction protégée des callbacks |
| [src/ivy_query_internal.h](src/ivy_query_internal.h) | Exemple de pont C privé entre cœur et wrapper ; non installé |
| [src/Makefile](src/Makefile), [src/cpp/Makefile](src/cpp/Makefile), [src/cpp/ivy-cpp.pc.in](src/cpp/ivy-cpp.pc.in) | Compilation, bibliothèques, installation et choix du backend à revoir |

Pour retrouver les fonctions sans dépendre des numéros de ligne :

```sh
rg -n 'IvyContextMainLoop|IvyContextIdle|IvyContextStopInLoop|IvyContextTimerRepeatAfter' src/ivy.c
rg -n 'IvyMainLoopFor|IvyIdleFor|IvyChannelPostControlFor|IvyChannelRemove|IvySet.*HookFor' src/ivyloop.c src/ivyglibloop.c
```

La structure `IvyContext` est définie uniquement dans `ivy.c`. Il n'existe pas
d'accesseur public donnant son `IvyChannelState*`. `native_handle()` ne suffit
donc pas, en l'état, pour appeler les fonctions `...For` sur les canaux du bus.
Ne pas créer un état de canaux indépendant en croyant obtenir celui du bus, et
ne pas utiliser le contexte par défaut comme raccourci.

## 5. Constats sur lesquels bâtir la suite

### 5.1 Boucle `select` : pilotage natif vérifié, `Idle` différé

`IvyContextRun()` vérifie l’état du bus et acquiert le pilote sous le mutex du
contexte. La nouvelle interface privée `ivy_loop_internal.h` sépare acquisition,
exécution et libération ; les créations de timers contextuels partagent cette
synchronisation. Un second pilote et une réentrée sont refusés. L’arrêt ne peut
plus être annulé par une réinitialisation à l’entrée de la boucle. Une commande
de création de timer acceptée avant stop reçoit toujours une réponse, même si
elle est traitée pendant la sortie. Les erreurs d’attente remontent explicitement
et le contexte est arrêté avant libération du pilote. `IvyContextMainLoop()`
délègue à cette entrée en conservant sa signature.

Les callbacks timers cessent d’être sélectionnés après l’arrêt de la boucle ;
les canaux retirés sont ignorés lors des dispatchs suivants du même passage.
Les hooks continuent d’encadrer l’attente. `Bus::run()` conserve les erreurs de
callbacks séparées, accessibles avec `take_callback_error()`.

Le backend reste basé sur `select`. L’itération C `IvyContextIdle()` ne scanne
pas les timers et sa correction reste différée. Aucune méthode C++ `idle()` ou
`poll()` n’est fournie. Les tests `tests/cpp/mainloop_test.cpp` couvrent l’arrêt,
la réentrée, le double pilotage, les erreurs et les créations de timers en course
avec l’entrée/sortie de la boucle.

### 5.2 Arrêt, retour de boucle et destruction sont distincts

`IvyContextStopInLoop` met le contexte à `STOPPED` et signale la condition d'arrêt.
Le thread peut encore être en train de terminer le dispatch ou de sortir de sa
boucle. Sur POSIX, `IvyContextStop` poste généralement un contrôle et attend cet
état lorsqu'il est appelé depuis un autre thread ; la branche Windows diffère.
Un retour de `stop()` n'est donc pas un `join()`.

`IvyContextDestroy` attend les callbacks suivis par le cœur. Les callbacks
application/messages passent par son suivi `IvyCallbackEnter/Leave`, mais ne
pas supposer que tout nouveau hook, contrôle ou callback de canal bénéficie de
ce suivi. Examiner aussi les timers, dont le callback est transmis au backend.

Dans `Bus::Impl::~Impl`, une destruction C refusée provoque `std::terminate()`
pour éviter de libérer des pointeurs `user_data` encore utilisés. Une future
garde de boucle doit organiser l'ordre arrêt → fin du dispatch → retour/join →
destruction, et ne jamais joindre son propre thread.

### 5.3 Les contrôles ne possèdent pas leur `user_data`

`IvyChannelPostControlFor` ajoute `{callback, data}` à une file et réveille la
boucle ; il retourne 0 ou -1, sans distinguer précisément toutes les causes.
Le callback n'est pas exécuté directement par cette fonction, mais un thread
de boucle concurrent peut le sélectionner avant le retour de l'appelant.

À la destruction d'un état, les deux backends libèrent les maillons restants
sans appeler de destructeur sur `data`. Le backend `select` draine aussi des
contrôles à la sortie normale de sa boucle. Publier un `new Callable` et compter
uniquement sur son exécution pour le libérer créerait donc une fuite en cas
d'abandon ; le libérer immédiatement à l'arrêt pourrait créer un pointeur pendant.

Un futur `post()` doit définir et tester l'acceptation, l'exécution, l'abandon à
l'arrêt et la libération de chaque capture exactement une fois. Une solution
possible est un stockage possédé par le wrapper avec relais stables et handlers
annulables ; une autre est une primitive C avec nettoyage explicite. Choisir
un contrat et le documenter. Éviter les cycles de `shared_ptr`.

Ne pas introduire une attente synchrone sur la boucle sans couvrir les courses
avec son démarrage et son arrêt. La création C d'un timer contextuel constitue
déjà un exemple de commande postée avec attente ; elle n'est pas une preuve
automatique que tous les nouveaux cas seront sûrs.

### 5.4 Les canaux et les hooks demandent une vraie gestion de durée de vie

Le backend `select` ajoute/modifie directement les listes et `fd_set` lors de
`IvyChannelAddFor`. `IvyChannelRemove` marque simplement une suppression différée.
Ces opérations ne sont pas toutes synchronisées comme les demandes d'écriture.
Les setters de hooks `select` affectent directement fonction et pointeur utilisateur.

Le callback C nommé `handle_delete` peut être appelé sur un événement exceptionnel
et lors de la suppression. Ne pas le traiter sans vérification comme un
destructeur appelé exactement une fois. Après une suppression pendant un callback
de lecture, un callback d'écriture déjà prêt peut encore être sélectionné par
le code `select` actuel ; GLib vérifie explicitement le drapeau de suppression.

À prévoir : sérialiser les mutations natives sur la boucle, désactiver immédiatement
le handler C++ lors d'une annulation, conserver un relais tant que C peut l'utiliser,
et définir clairement qui ferme le descripteur. Respecter `IVY_HANDLE` : `int`
sur POSIX, socket sur Windows, avec les limites de `select`.

Les hooks peuvent servir à relâcher/reprendre un verrou applicatif autour de
l'attente. Le contrat doit préciser l'ordre de dispatch des contrôles, timers et
I/O. Intercepter une erreur dans le hook « avant » ne doit pas laisser un verrou
dans un état incohérent ni empêcher l'étape « après » nécessaire au nettoyage.

### 5.5 GLib existe déjà, le choix de bibliothèque C++ reste à faire

Le backend C GLib est dans `ivyglibloop.c` et `ivyglibtimer.c`, produit
`libglibivy` / pkg-config `ivy-glib` et requiert GLib >= 2.36 dans le build actuel.
Chaque contexte capture le `GMainContext` thread-default au moment de sa création,
ou le contexte global par défaut. Une boucle GLib/GTK externe peut déjà le piloter.

- Plusieurs bus peuvent partager un même `GMainContext`.
- Des threads de boucle distincts doivent utiliser des contextes GLib distincts.
- Arrêter un bus désactive ses sources et timers, sans quitter la boucle hôte ni
  arrêter les autres bus.
- L'itération GLib actuelle traite les timers. Les hooks Ivy encadrent le polling
  des entrées `IvyMainLoopFor`/`IvyIdleFor`, pas les appels arbitraires de la boucle
  externe à `g_main_loop_run`.
- Une itération d'un `GMainContext` peut dispatcher d'autres sources que celles
  du bus considéré. Le préciser dans le contrat C++.

Deux variantes C++ sont maintenant disponibles : `ivy-cpp` / `libivy-cpp`
liée à `libivy`, et `ivy-cpp-glib` / `libivy-cpp-glib` liée à `libglibivy`.
Les builds, SONAME, archives et fichiers pkg-config sont distincts. Un processus
choisit une seule variante : les backends C exportent les mêmes symboles.
Les cibles Linux sont `cpp`, `install-cpp`, `cpp-glib`, `install-cpp-glib`.

`ivy::glib::create_bus()` est défini dans l’en-tête optionnel `ivy_glib.hpp`.
Ses surcharges acceptent le contexte courant/par défaut ou un `GMainContext*`
explicite, puis les arguments habituels de `Bus::create()`. Le résultat reste
`Bus::CreateResult`, avec captures non copiables. Le helper acquiert le contexte
sans attente, le pousse temporairement pendant la création et restaure le
contexte précédent sur tous les retours. Un contexte possédé par un autre thread
est refusé avec `IVY_ESTATE`. Le backend C conserve ses références GLib.

L’application conserve l’API C de GLib pour piloter sa boucle. Aucun accesseur
GLib supplémentaire n’est nécessaire ; `native_handle()` reste un `IvyContext*`.
`Bus::run()` fonctionne aussi avec cette variante, avec rejet de réentrée depuis
un dispatch GLib et remontée des erreurs de polling. `tests/cpp/run_glib.sh`
vérifie les contextes explicites/par défaut, la restauration sur échec, les
threads propriétaires, les échanges multibus, l’arrêt indépendant, l’installation
et les dépendances des bibliothèques statiques/partagées.

### 5.6 Autres intégrations : distinguer code historique et support actuel

`ivyxtloop.c`, `ivyglutloop.c` et la partie canaux de `ivytcl.c` contiennent encore
des implémentations historiques globales. Elles ne fournissent pas toutes les
primitives contextuelles modernes ; leurs cibles ne sont pas activées dans le
build Linux par défaut. Leur présence ne prouve pas une compatibilité multibus.

[examples/ivyqt](examples/ivyqt/README.md) est une démonstration Qt utilisant
l'API C avec un thread Ivy séparé et des notifications Qt mises en file. Ce n'est
pas une intégration native du polling Ivy à la boucle Qt. La note
`src/ivyqtloop.readme` est historique et ne décrit pas le nouveau wrapper.

Priorité proposée : terminer `select` et GLib, puis inventorier explicitement
les adaptateurs à porter. Une intégration native Qt ou à une autre boucle demande
soit un backend de canaux/timers, soit une interface C d'intégration externe
(préparation de l'attente, intérêts I/O, échéance, dispatch). Ces primitives
servent à attacher les sources à la boucle hôte ; l’appel périodique d’`idle()`
est exclu des intégrations C++ retenues.
Ne pas annoncer « toutes les boucles sont supportées » sans compilation et test
des adaptateurs correspondants. Le choix exact des autres frameworks reste ouvert.

## 6. Proposition d'API C++ et contrats à fixer

`run()` et les factories GLib sont implémentés. Les autres noms décrivent
l’intention et ne sont pas encore présents dans les en-têtes.
Rester cohérent avec `Bus`, les surcharges de `bind` et les tokens existants.

| Besoin | Proposition | Contrat à préciser avant de coder |
| --- | --- | --- |
| Exécution bloquante — implémentée | `bus.run()` → `expected<void, error_code>` | Bus démarré, un seul pilote, sortie normale sur stop, erreur native explicite |
| Réveil | `bus.wakeup()` | Réveiller sans arrêter ; préciser les états où l'opération est utile/permise |
| Travail depuis un worker | `bus.post(callable)` → `expected<void, error_code>` | Callable mobile `void()`, mise en file, absence de garantie d'achèvement par le seul retour |
| État du pilote | `loop_running()`, `on_loop_thread()` si utiles | Valeurs d'observation, pas un verrou ni une autorisation d'accès concurrent |
| Surveillance I/O | `bus.bind(callbacks, sélecteur_de_canal)` → token mobile | Lecture/écriture/fermeture, activation de l'écriture, propriété du descripteur et annulation |
| Hooks | `bind(callback, before_wait/after_wait)` ou une paire de hooks | Remplacement sûr et portée ; tenir compte des paires de verrouillage |
| Thread optionnel — implémenté | `LoopThread::create(bus[, completion])` | Emprunte le Bus stable, création faillible, arrêt sans attente et join ; pas de thread implicite dans `Bus::start` |
| Boucle externe | Adaptateur optionnel ou variante de backend | Attachement des sources I/O, timers et réveil à la boucle hôte ; propriété du contexte, références, sources et thread de dispatch |

Décisions recommandées pour avancer sans redécouvrir le problème :

1. `run()` est implémenté pour le pilotage bloquant, sans thread implicite.
   L’exécution simultanée et la réentrée sur le même bus sont rejetées ; les
   états déplacé, non démarré et arrêté ont des résultats documentés.
2. Le résultat de `run()` décrit uniquement le pilote de boucle.
   `take_callback_error()` conserve séparément la première erreur de callback,
   sans consommation implicite par `run()`. `save_callback_error()` demande l’arrêt.
3. Pour `post()`, préférer une mise en file même depuis le thread de boucle afin
   d'éviter une réentrée surprise. Un autre thread peut néanmoins commencer la
   tâche avant le retour de `post()`. Définir les publications avant `run()` et
   pendant/après `stop()` ; proposer l'annulation des handlers utilisateur encore
   en attente à l'arrêt, distincte des contrôles internes nécessaires au nettoyage.
4. Pour les canaux, proposer par défaut un descripteur emprunté : le token retire
   la surveillance, la fermeture reste à l'application selon un ordre documenté.
   Si une variante possédante est utile, rendre cette propriété explicite.
5. L'expiration ou destruction d'un token doit empêcher toute nouvelle invocation
   utilisateur. Une invocation déjà sélectionnée peut finir ; les captures restent
   valides jusque-là. Un ancien token ne doit pas désinstaller son remplaçant.
6. Le helper `LoopThread` convertit les erreurs de création en `expected`,
   réveille Ivy par `request_stop()` et joint son thread avant destruction.
   Le Bus doit lui survivre à adresse stable. La destruction sur le thread
   possédé est interdite ; les erreurs sont consultables par `join()` et celles
   des callbacks Ivy par `take_callback_error()` séparément.
7. Garder les types des frameworks hors de l'en-tête commun et la dépendance C++23
   hors du build C par défaut. Ne pas imposer GTK/Qt aux utilisateurs de `select`.

Les helpers RAII restent côté C++. Pour accéder à la boucle d'un `IvyContext`,
préférer des opérations contextuelles C de haut niveau si elles rendent aussi
l'API C cohérente. Un pont privé non installé convient pour un besoin strictement
interne au wrapper. Ne pas exposer la disposition mémoire du contexte ni recopier
sa définition dans un en-tête C++.

## 7. Contraintes d'implémentation du wrapper existant

`Bus::Impl` est possédé par `shared_ptr`. Les états de tokens ont un propriétaire
faible ; `Impl` conserve des relais stables jusqu'à la destruction du contexte C,
car C peut avoir copié un `user_data` avant un remplacement/désabonnement. Les
handlers peuvent être libérés plus tôt quand aucune invocation ne les utilise.
Reprendre ce modèle sans retenir indéfiniment les captures des tâches terminées.

Ne jamais appeler un callback utilisateur avec un mutex interne C++ maintenu.
Ne pas maintenir `subscriptions_mutex` ou `callback_mutex` pendant un appel C
susceptible de poster une commande et d'attendre la boucle. C'est une contrainte
déjà appliquée dans `TimerSubscription::State::schedule`.

Le système de timers actuel protège plusieurs courses difficiles : relais
`pending` avant retour de création native, compteur décrémenté sur les callbacks
effectivement sélectionnés, remplacement de période sans réarmer un timer épuisé.
Les anciens timers natifs sont supprimés depuis leur callback ou à la destruction
du contexte. `TimerModify/TimerRemove` du backend `select` ne sont pas rendus sûrs
depuis tous les threads par le simple fait qu'ils soient accessibles en C.
Préserver ces garanties si l'intégration change leur chemin d'exécution.

Les callbacks sont construits à l'intérieur de `detail::guard`, par des templates
recevant `Callback&&`. Une conversion en `std::move_only_function` faite avant
l'entrée dans la fonction `noexcept` pourrait lancer hors de la protection.
Éviter aussi de contraindre toutes les surcharges de `bind` par la signature du
callback : cela avait provoqué l'instanciation de corps de lambdas génériques
pour des surcharges sans rapport. Choisir d'abord la surcharge grâce au sélecteur.

`api/mainloop.hpp` et `ivy_mainloop.cpp` contiennent le pilotage livré.
Ajouter un fichier de types/tokens et `ivy_channels.cpp` si les futurs canaux
le justifient. `ivy_glib.hpp` / `ivy_glib.cpp` isolent l’intégration GLib.
Les déclarations privées de `Bus` vont dans `ivy_bus_private.hpp`, les détails
compilés dans `ivy_internal.hpp`, les templates dans `ivy_detail.hpp` ou un détail
thématique inclus par celui-ci.

Les fragments `api/*.hpp` utilisent ce mécanisme particulier :

```cpp
#if !defined(IVY_CPP_API_HEADERS)
#include "../ivy.hpp"
#else
#pragma once
// IVY_CPP_API_BEGIN
// Déclarations au niveau namespace ou membres de Bus selon le fragment.
// IVY_CPP_API_END
#endif
```

Ne pas déplacer `#pragma once` avant la condition : l'inclusion directe d'un
fragment doit pouvoir repasser par `ivy.hpp` et réinclure le fragment pour former
la classe complète. L'assemblage ne doit pas laisser fuiter sa macro.

[doc/doxygen_cpp_filter.py](doc/doxygen_cpp_filter.py) assemble uniquement pour
Doxygen les déclarations comprises entre les marqueurs. Les guides restent dans
chaque fichier. Mettre à jour le filtre si de nouveaux chemins d'inclusion
sortent de son motif actuel. Ajouter la nouvelle rubrique à la carte de lecture,
à l'exemple de `ivy.hpp` et au README, sans supprimer les guides existants.
Le quickstart `cpp_quickstart` se trouve dans `api/lifecycle.hpp`.

## 8. Découpage de travail et de commits proposé

Documenter au fur et à mesure les fichiers/hunks et commandes de validation dans
ce fichier ou un journal complémentaire, en distinguant C et C++. Ne pas englober
les changements précédents simplement parce qu'ils touchent les mêmes fichiers.

| Lot | Résultat concret | Dépendances |
| --- | --- | --- |
| ML-1 — C, pilotage natif et cycle de vie | Boucle bloquante `select`, erreurs fiables, contrôle de propriété/réentrée et respect de stop ; tests C ciblés | État C actuel |
| ML-2 — C, primitives contextuelles | Accès/opérations nécessaires pour post, wake, canaux et hooks ; contrats de nettoyage et mutations concurrentes sur les backends maintenus | ML-1 selon primitives |
| ML-3 — C++, pilotage natif | `run` bloquant, erreurs, documentation, exemple sans appel C pour piloter la boucle | ML-1 |
| ML-4 — C++, tâches et réveil | Publication de captures mobiles, nettoyage/arrêt, absence de réentrée, tests de courses | ML-2, ML-3 |
| ML-5 — C++, canaux et hooks | Tokens, annulation/remplacement, intérêt écriture et durée de vie | ML-2 ; ML-4 si sérialisation réutilisée |
| ML-6 — C++, durée de vie d'un thread — livré | `LoopThread`, création faillible, arrêt sans attente, join et exemple Qt6 | ML-3, contrats d'arrêt stabilisés |
| ML-7 — Backend GLib et distribution C++ | Choix de bibliothèque explicite, éventuel adaptateur, tests externes multibus, pkg-config statique/partagé et installation | Lots C/C++ nécessaires |
| ML-8 — Autres intégrations | Inventaire terminé ; portage et validation des adaptateurs retenus, limites des backends historiques documentées | Contrat externe et sélection des frameworks |

Les corrections natives requises par une fonctionnalité doivent précéder le
commit C++ qui les utilise. Mettre les tests et la documentation de chaque lot
avec son implémentation. Ajouter les sources à `src/cpp/Makefile` et, si le contrat
C change, examiner les autres Makefiles, `src/libIvy.def` et les fichiers de
distribution/pkg-config concernés. `src/Makefile` contient des commentaires dans
un encodage historique : éviter de réencoder tout le fichier pour une petite édition.

## 9. Validation à reprendre et à compléter

Les validations du pilotage natif et de GLib sont consignées dans
`CPP_API_WORKLOG.md`. Elles ne couvrent pas les futures API de tâches, canaux,
hooks et helpers de thread. Les validations de ce chantier portent sur la
boucle native bloquante et les boucles hôtes retenues.
Les tests d’une correction des timers d’`IvyContextIdle()` relèveront du lot C
différé ; ils ne sont pas un critère de fin du wrapper.

Prérequis usuels : compilateur et bibliothèque standard offrant C++23 avec
`std::expected`, `std::format` et `std::move_only_function`, make, pkg-config,
PCRE2, pthreads ; GLib pour son backend ; Python 3 et Doxygen pour la documentation.
Certains anciens scripts font `make -C src` et construisent donc aussi les outils,
qui requièrent readline/history. Le build limité aux bibliothèques évite ces outils.

Commandes existantes à choisir selon les fichiers modifiés, depuis la racine :

```sh
make -C src static-libs shared-libs
make -C src cpp
./tests/cpp/run.sh
./tests/run_phase4.sh
./tests/run_phase6.sh
./tests/run_phase9_select_wakeup.sh
./tests/run_glib_backend.sh
```

Régressions utiles si le cycle de vie, les callbacks ou le cœur sont touchés :

```sh
./tests/run_phase2.sh
./tests/run_phase5.sh
./tests/run_phase11_runtime_errors.sh
./tests/run_transport_errors.sh
./tests/run_context_filters.sh
./tests/run_query_snapshots.sh
```

`tests/cpp/run.sh` couvre déjà : compilation positive et 21 cas négatifs,
frontière C simulée dans `bus_test.cpp`, vraie validation PCRE2, intégration
multibus statique/partagée, installation temporaire et compilation de chaque
en-tête `api/*.hpp` en première inclusion. Il vérifie aussi les dépendances ELF
du wrapper. Adapter ce dernier contrôle aux variantes de backend retenues.
Si de nouveaux symboles C sont appelés par le wrapper, compléter les stubs de
`bus_test.cpp` ; ne pas contourner ses tests en le liant au cœur réel.

Réutiliser les scénarios de `phase4_wakeup_control_test.c`,
`phase6_context_loop_test.c`, `phase9_select_wakeup_test.c` et
`glib_backend_test.c`. Le test GLib exerce déjà une vraie boucle hôte, des
contrôles, canaux, timers, hooks et plusieurs contextes, en statique et partagé.

Scénarios de validation (pilotage/GLib couverts par les nouvelles suites ;
tâches/canaux/hooks à compléter lors de leur implémentation) :

- Boucle native : les timers sont exécutés sans trafic réseau et l’attente est
  ajustée à leur échéance ; hooks et contrôles ont l’ordre documenté.
- Exécution : stop depuis callback et depuis worker, réveil d'une attente sans
  échéance, arrêt avant entrée dans la boucle, double pilotage et réentrée.
- Erreurs : échec natif du polling/initialisation rapporté ; bus déplacé et états
  invalides ; échec de construction de callback et de création de thread.
- Publication : capture non copiable, identité du thread, publication depuis un
  callback, capture libérée sur exécution/échec/abandon, absence de fuite et de
  double appel pendant une course avec stop/destroy.
- Canaux : lecture/écriture, désactivation d'écriture, retrait depuis son callback
  et depuis un worker, retrait empêchant un second callback déjà prêt, fermeture
  du descripteur selon le contrat, handles invalides et limites `select`.
- Tokens/hooks : remplacement puis destruction de l'ancien token, capture conservée
  pendant un appel sélectionné, désabonnement répété, token survivant au bus,
  maintien des invariants de la paire avant/après en cas d'erreur.
- Timers existants : `after(0)`, compteur limité, dernier callback, changement de
  période et création depuis un worker continuent de fonctionner avec la boucle
  native et chaque intégration à une boucle hôte retenue.
- Multibus : aucune tâche, surveillance ou demande d'arrêt ne vise le contexte par
  défaut ou un autre bus par accident.
- GLib : deux bus sur la même boucle externe, arrêt d'un seul, minuterie/source
  applicative toujours active ; contextes distincts sur threads distincts ; durée
  de vie des références GLib et absence de dispatch Ivy après destruction.
- Distribution : consommateur installé pour chaque backend maintenu, bon SONAME
  et bonne dépendance C ; absence de chargement accidentel des deux backends C.

Utiliser barrières/conditions et échéances bornées pour les tests de concurrence,
plutôt que de longs sleeps supposés résoudre les courses. Les tests réseau locaux
ont nécessité une autorisation de sandbox sur le PC précédent ; une erreur
`Operation not permitted` à la création d'une socket n'est pas un diagnostic de
régression de l'API. Distinguer les limitations de l'environnement des échecs du code.

Documentation et hygiène finale :

```sh
doxygen Doxyfile
git diff --check
```

Vérifier les liens/sections Doxygen, la présence de toutes les méthodes de `Bus`,
les exemples sans exceptions et l'inclusion directe des nouveaux fragments.
Compiler et exécuter les nouveaux exemples ; pour un exemple de boucle hôte,
vérifier qu'elle continue à vivre après l'arrêt de son bus Ivy.

## 10. Critères de fin et premières actions

Le chantier est terminé lorsque le pilotage natif et les opérations Ivy retenues
sont accessibles en C++, avec des erreurs récupérables, des tokens et captures
dont la durée de vie est maîtrisée, et les scénarios statiques/partagés
ainsi que multibus validés. L’application peut continuer à piloter sa boucle
hôte avec l’API native du framework, notamment l’API C de GLib. Les adaptateurs
annoncés doivent être testés ; les backends historiques non portés doivent être nommés comme limites restantes.
Le helper de thread doit être optionnel, et l'arrêt d'Ivy ne doit pas arrêter
une boucle externe appartenant à l'application. Aucune méthode C++ d’itération
manuelle n’est prévue ; la correction C d’`Idle` reste hors de ces critères.

À la reprise :

1. Confirmer que l'arbre complet a été transféré, lire les instructions locales
   éventuelles et ce document, puis `CPP_API_WORKLOG.md`.
2. Construire le wrapper et exécuter sa suite existante pour établir la base sur
   le nouveau PC ; vérifier le backend GLib si disponible.
3. Lire les fonctions ciblées de `ivyloop.c` et fixer le contrat de pilotage
   bloquant, d’arrêt et d’erreur. Cibler les tests sur ce contrat, puis sur
   l’attachement des sources à la boucle hôte pour chaque adaptateur retenu.
4. Avancer par lots C puis C++ selon le tableau, en consignant les décisions et
   validations. Conserver l’intégration aux boucles hôtes dans le périmètre.

Ce relevé donne un point de départ concret ; il ne prétend pas être un audit
exhaustif de toutes les courses des backends historiques.


## Lot livré le 15 septembre 2026 — natif C++ et GLib

L’implémentation de ce lot couvre ML-1 et ML-3, ainsi que la création GLib et la
distribution de ML-7. ML-2, ML-4 et ML-5 restent à concevoir/implémenter.
ML-6 a ensuite été réalisé par le complément Qt6 décrit plus bas. Le backend
QtCore reste une piste séparée de l’exemple Qt utilisant la boucle native.
Voir `CPP_API_WORKLOG.md` pour les fichiers et les validations finales.

Étude Qt6 disponible dans [QT6_INTEGRATION.md](QT6_INTEGRATION.md). Le choix
utilisateur retient la boucle native dans un thread dédié, les envois directs
MT-safe depuis Qt et une seule file de notifications Ivy vers Qt. `Bus::post()`
n’est pas requis pour cette intégration. Le backend QtCore reste une possibilité
future, au-delà du besoin actuel.


## Complément livré — exemple Qt6 et helper de thread

L’ancien exemple `examples/ivyqt` est remplacé par une application C++23 utilisant
`ivy::Bus`, des tokens d’abonnement et `ivy::LoopThread`. Ce complément réalise
ML-6 avec un Bus emprunté à adresse stable, une factory faillible, un callback
de fin optionnel, un arrêt sans attente et un join vérifié. Le C conserve la
sémantique existante de `IvyContextStop` et ajoute `IvyContextRequestStop`.
L’exemple appelle directement l’API d’envoi depuis Qt et les workers, avec des
signaux mis en file pour les notifications à la GUI. Le backend QtCore demeure
facultatif ; voir `QT6_INTEGRATION.md` et `CPP_API_WORKLOG.md` pour les validations.
