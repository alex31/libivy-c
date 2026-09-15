# Complément de l’API C++ 3.18 — journal et préparation des commits

## Périmètre accepté

Implémenter les informations sur les applications, les messages de contrôle,
les timers ponctuels/limités et la validation autonome des regexps ancrées.
La mainloop, les canaux et les hooks restent hors de ce lot.

Conventions : C++23, résultats `std::expected`, opérations `noexcept`, exemples
sans gestion d’exceptions, documentation Doxygen, `#pragma once`. La méthode
`Bus::application(peer)` retourne une paire de chaînes `(nom, hôte)` possédées
par l’appelant. Les handles `IvyClientPtr` restent empruntés.

Les copies cohérentes sous verrou sont effectuées dans le cœur C. Leurs
déclarations restent dans un en-tête interne non installé, sans enrichir
`ivy.h`. Quelques symboles de liaison sont nécessaires entre les bibliothèques
C et C++, mais ne constituent pas une API publique stable.

## État initial de ce lot

Base Git de comparaison : `85b2c27686faffeeec6873d257f76af4eaff9936`.

Le répertoire de travail contient déjà des changements non commités des étapes
précédentes : découpage du wrapper, résultats sans exceptions propagées,
documentation Doxygen, callbacks pong/abonnements distants, timers périodiques,
filtres par contexte et `set_filters` acceptant packs et ranges.

Les fichiers `AGY_DEMO/`, `REVIEW_MASTER_HEAD.md` et `diff_to_review.patch`
préexistent à ce travail et ne font pas partie des changements à regrouper.
Aucun commit ni staging n’est effectué par ce lot.

## Découpage proposé pour les nouveaux commits

0. **C — validation des pairs des messages de contrôle**
   Les hunks de `IvyContextSendDieMsg` et `IvyContextSendErrorV` dans `ivy.c`.
   Ils doivent précéder les tests de snapshots, qui vérifient aussi le rejet
   des contrôles adressés à un pair d’un autre contexte.
1. **C — copies privées des informations d’application**
   Ajouter le contrat interne et les copies sous verrou, avec libération dédiée,
   et les tests C de propriété, erreurs et allocations. Ce commit doit pouvoir
   être construit indépendamment du wrapper.
2. **C++ — requêtes sur les applications**
   Adapter les copies C en `pair<string, string>` et `vector<string>` ; ajouter
   la recherche optionnelle d’un pair, les tests et le branchement au build.
   Dépend du commit C précédent.
3. **C++ — messages de contrôle**
   Ajouter `send_die()` et les formes brute/formatée de `send_error()`.
4. **C++ — validation autonome**
   Ajouter `validate_anchored_regexp()` brut/formaté et ses erreurs vérifiables.
5. **C++ — timers ponctuels et limités**
   Ajouter `after()` et `every(period, count)`, expiration et captures,
   maintien du nombre restant lors de `set_period()`, tests de concurrence.

Les exemples, commentaires Doxygen et paragraphes README propres à chaque
fonctionnalité doivent accompagner leur commit. Plusieurs lots touchent
`ivy.hpp`, `ivy_detail.hpp`, le README et les tests partagés : les sélectionner
par hunks plutôt que d’ajouter ces fichiers entiers en une fois.

## Journal d’exécution

- État de départ et contrats C/C++ inspectés ; plan de découpage enregistré.
- **C, terminé** : `src/ivy_query_internal.h` définit le tableau compté de chaînes
  possédées et sa libération. Les trois fonctions de copie sont dans `src/ivy.c`,
  sous verrou de bindings, avec contrôle du contexte du pair et nettoyage sur erreur.
  `src/Makefile` suit la dépendance de cet en-tête ; il n’est pas installé.
- **C, garde des messages de contrôle** : les fonctions natives `SendDieMsg` et
  `SendError` vérifient l’appartenance du pair et protègent l’envoi avec le verrou
  de bindings, comme `SendDirectMsg`. À isoler dans le commit C numéro 0 ; les
  tests de snapshots suivants couvrent aussi ces deux rejets. Aucun nouveau
  symbole public n’est ajouté pour les contrôles.
- **Validation C réussie** : `tests/run_query_snapshots.sh` ; copies indépendantes
  après changements, déconnexion et destruction, erreurs de contexte, allocations
  partielles, libération répétable, lectures pendant des modifications de regexps.
- **C++, terminé et validé** : `ivy_application.cpp` adapte les copies
  et libère les buffers via une garde RAII privée. `find_application` distingue
  absence (`nullopt`) et erreur. Déclarations et docs dans `ivy.hpp`, build dans
  `cpp/Makefile` ; aucune déclaration du pont dans les en-têtes installés.
- **C++, terminé et validé** : contrôles dans `ivy_send.cpp`, validation
  autonome dans `ivy_subscription.cpp`, templates dans `ivy_detail.hpp`. Tests
  de conversion d’erreurs et de formats dans `bus_test`, `anchoring_test` et
  `compile_test`.

- **Timers limités, terminés et validés** : `Every::count`, sélecteur `After`, helpers
  `every(period, count)` et `after(delay)` dans les en-têtes du wrapper. Le compteur
  reste dans `TimerSubscription::State` et n’est décrémenté que lors de la sélection
  effective d’un callback ; les expirations natives pendant une création en cours
  ne consomment pas ce compteur. Aucune modification de `timer.c` ou de la mainloop.
- **Tests ciblés C++ réussis** : erreurs des contrôles/formateurs, budgets de timers,
  changement de période, expiration pendant un changement, annulation, callback
  final concurrent et libération des captures. Suite complète réussie en statique et partagé.
- **Exemples/docs** : `inspection.cpp` ajouté ; toutes les nouvelles opérations sont
  documentées dans `ivy.hpp` et le README. Le script C++ compile aussi cet exemple
  depuis une installation temporaire et vérifie l’absence de l’en-tête privé C.

## Validation finale de ce lot

- `./tests/run_query_snapshots.sh` : réussi, avec sockets locales autorisées.
- `./tests/cpp/run.sh` : réussi ; compilation positive et 21 cas rejetés,
  tests unitaires, PCRE2/validation autonome, intégration multibus en statique
  et en partagé, compilation des trois exemples depuis l’installation temporaire.
- L’installation temporaire ne contient pas `ivy_query_internal.h`.
- Doxygen généré : aucun avertissement pour l’API C++.
- Extraits C++ de l’en-tête : compilation vérifiée sans `throw`, `try` ou `catch`.
- `git diff --check` : réussi.

## Repères de sélection des hunks

| Lot | Fichiers et zones à sélectionner | Dépendances |
| --- | --- | --- |
| C-0 contrôles | `src/ivy.c` : verrou et appartenance dans `IvyContextSendErrorV` et `IvyContextSendDieMsg` | C contextuel existant |
| C-1 copies | `src/ivy_query_internal.h`, bloc `IvyStringSnapshot*`/`IvyContextCopy*` et include dans `src/ivy.c`, dépendance dans `src/Makefile`, `tests/query_snapshot_test.c`, `tests/run_query_snapshots.sh` | C-0 pour les assertions de contrôle du test |
| CPP-1 requêtes | `src/cpp/ivy_application.cpp`, déclarations des quatre méthodes et includes `vector`/`utility` dans `ivy.hpp`, source ajoutée au Makefile, assertions de requêtes dans `integration_test.cpp`, absence du header privé dans `run.sh` | C-1 + wrapper refactoré |
| CPP-2 contrôles | nouvelles méthodes de `ivy_send.cpp`, déclarations `send_die`/`send_error` et template de formatage, mocks/assertions de contrôle dans `bus_test`, appels de contrôle dans `integration_test`, cas de compilation 19 | C-0 + wrapper refactoré |
| CPP-3 validation | fonction libre dans `ivy_subscription.cpp`, déclaration et template, assertions de `anchoring_test`, échecs de formateur dans `bus_test`, cas de compilation 20 | wrapper refactoré, indépendant du pont C |
| CPP-4 timers limités | `Every::count`, `After`/`after`, surcharge `every`, overload `bind`, état/compteur dans `ivy_internal.hpp` et `ivy_timer.cpp`, tests `limited_timers`, cas de compilation 21, compteurs d’intégration | timers périodiques existants |

Pour CPP-4, inclure aussi l’adaptation du mock `IvyContextTimerRepeatAfter`
(période zéro autorisée). Les nouveaux cas 19/20/21 dans `compile_test.cpp`
évoluent avec la liste correspondante de `run.sh`. `inspection.cpp` dépend de
CPP-1 et CPP-4 : l’ajouter avec CPP-4, ainsi que son ajout à la boucle de
compilation des exemples dans `run.sh`.

Les bibliothèques C/C++ doivent provenir des mêmes sources pour le pont privé.
Recompiler les consommateurs C++ : le descripteur `Every` et l’implémentation
privée des timers ont évolué. Le compteur des timers finis est une notion du
wrapper, sans changement du moteur de timers C.

## Changements antérieurs à séparer également

- **C — filtres par contexte** : `src/ivy.c`, `src/ivy.h`, `src/ivybind.*`,
  adaptations de `ivyprobe`/`ivyperf`, tests `context_filters`, documentation
  multibus. Le remplacement atomique de la liste est une évolution de 3.18.
- **C++ — refactorisation et API de résultats** : séparation des en-têtes et
  sources, factory `Bus::create`, `stop`, récupération d’erreur de callback,
  installation des en-têtes et adaptations des consommateurs.
- **C++ — callbacks et timers périodiques** : sélecteurs, abonnements événementiels,
  `send_ping`, période modifiable, tests et exemple `callbacks.cpp`.
- **C++ — filtres** : méthodes sur `Bus`, packs/ranges, propriété des chaînes,
  tests et documentation ; dépend du lot C des filtres.

Ce document décrit des frontières logiques ; il ne prétend pas que le diff
actuel soit déjà découpé en commits prêts à appliquer.

## CPP-5 — API publique répartie par responsabilité

Ce lot de présentation est à appliquer **après** les lots fonctionnels ci-dessus.
Leurs anciens repères dans `ivy.hpp` désignent maintenant les sections indiquées
ci-dessous. Il ne change ni les appels, ni les types, ni les données des classes,
ni l’implémentation C/C++ des opérations.

- `ivy.hpp` passe de **1 185 lignes / 58 136 octets** à **111 lignes / 4 216 octets**.
  Il contient le sommaire, les includes et la définition unique de `Bus`.
- Sept sections de membres dans `src/cpp/api/` : `lifecycle.hpp`, `messages.hpp`,
  `send.hpp`, `callbacks.hpp`, `timers.hpp`, `applications.hpp`, `filters.hpp`.
- Quatre sections de types/contrats partagés : `results.hpp`, `regexp.hpp`,
  `subscriptions.hpp`, `timer_types.hpp`. Le plus gros fichier est maintenant
  `subscriptions.hpp` : 245 lignes / 9 833 octets.
- Les sept guides et les sept exemples sont déplacés, sans réduction. Les
  déclarations des 58 membres/alias publics de `Bus` sont copiées avec leurs
  commentaires. La comparaison XML Doxygen avant/après vérifie les signatures
  et descriptions complètes de **196 entités publiques**.
- Les sections de membres sont incluses dans `Bus` ; aucun héritage n’est ajouté.
  Leur inclusion directe redirige vers `ivy.hpp`. Le macro d’assemblage reste
  local à l’en-tête et `#pragma once` protège les inclusions répétées.
- `doc/doxygen_cpp_filter.py` assemble ces sections pour Doxygen seulement.
  `Doxyfile` référence également les pages/sections de documentation déplacées.
- `src/cpp/Makefile` suit et installe les en-têtes publics `api/*.hpp` ;
  `tests/cpp/run.sh` vérifie chaque en-tête installé comme premier include,
  les inclusions répétées et l’absence de fuite du macro d’assemblage.
- Le README fournit la même carte de lecture que `ivy.hpp`.

À sélectionner ensemble pour ce commit : `src/cpp/ivy.hpp`, `src/cpp/api/`,
`src/cpp/Makefile`, `Doxyfile`, `doc/doxygen_cpp_filter.py`, les ajouts de
vérification des en-têtes dans `tests/cpp/run.sh`, et la carte de lecture du README.
Les fichiers `ivy_detail.hpp` et les `.cpp` ne sont pas modifiés par ce découpage.

Validation de ce lot réussie :

- mêmes signatures et descriptions pour les 196 entités publiques, mêmes
  descriptions des classes/namespaces et mêmes sept guides dans le XML Doxygen ;
- les sept exemples originaux sont conservés et compilent depuis leurs nouveaux fichiers ;
- aucun avertissement Doxygen pour l’API C++ ;
- chaque section publique installée compile comme premier include ; les inclusions
  répétées et l’absence de fuite du macro interne sont vérifiées ;
- suite `tests/cpp/run.sh` complète réussie, y compris liens statique/partagé,
  installation et intégration multibus ;
- `git diff --check` réussi.

La génération Doxygen utilise Python 3 pour le filtre d’entrée ; la compilation
et l’utilisation de la bibliothèque ne dépendent pas de ce filtre.

## Complément CPP-5 — exemple annoté dans ivy.hpp

À la demande de l’utilisateur, le point d’entrée garde maintenant un exemple
complet directement lisible, en plus du sommaire. Les sept exemples précédents
restent dans `api/` sans réduction.

- Exemple autonome : création, filtres, bind regexp/direct, consultation du nom
  et de l’hôte, ACK direct, ping/pong, observation des abonnements distants,
  erreurs de transport, trois broadcasts avec bilan, timers et arrêt après cinq secondes.
- Chaque opération Ivy renvoie par un commentaire à son en-tête de référence.
  Les types de résultats et durées de vie renvoient aussi aux sections dédiées.
- Résultats vérifiés sans `throw` ni `try/catch` ; tous les tokens restent vivants
  jusqu’à la fin de la boucle native.
- Fichiers concernés : `src/cpp/ivy.hpp`, introduction du README et ce journal.
  Aucune déclaration ou implémentation d’API n’est modifiée.
- Vérification réussie : l’exemple est extrait directement du commentaire de
  `ivy.hpp`, compilé puis exécuté en deux instances sur un bus loopback. Les
  réceptions HELLO, les ACK directs et les pongs sont observés ; les deux instances
  s’arrêtent normalement après cinq secondes.
- Doxygen ne signale aucun avertissement C++ et conserve les 196 entités publiques
  documentées ainsi que les sept guides précédents, avec le nouvel exemple en plus.

## Complément CPP-5 — déclarations privées hors du point d’entrée

Les champs et méthodes privés de `Bus` sont déplacés à l’identique dans
`src/cpp/ivy_bus_private.hpp`, inclus à la fin de sa déclaration. Le fichier
est installé car le compilateur du consommateur en a besoin, comme pour
`ivy_detail.hpp`. L’état interne partagé entre les `.cpp` demeure dans
`ivy_internal.hpp`, non installé.

Le Makefile installe et suit le nouveau fichier ; le filtre Doxygen assemble
également ce fragment. `ivy.hpp` montre désormais l’exemple annoté, la carte
publique et les includes. Aucun changement de layout ou de comportement.

Validation après déplacement des déclarations privées : suite C++ complète
réussie, en-têtes installés et inclusions répétées compris. Le point d’entrée
fait maintenant **188 lignes / environ 8 Ko**, avec son exemple complet, et
ne présente plus les champs ni les méthodes privés. Pour le commit de ce
complément, inclure aussi `ivy_bus_private.hpp`, la liste d’installation dans
`src/cpp/Makefile` et son assemblage dans `doc/doxygen_cpp_filter.py`.

## Périmètre mainloop — décision du 15 septembre 2026

Le wrapper C++ privilégiera la boucle native bloquante et l’attachement aux
boucles d’événements hôtes compatibles. Aucune API C++ `idle()`, `poll()` ou
équivalent d’`IvyContextIdle()` n’est retenue, y compris comme mécanisme
périodique d’intégration à une boucle externe. La correction des timers d’Idle
côté C est différée et ne conditionne pas ce chantier. Pour GLib, privilégier
la réutilisation du backend C et le pilotage de la boucle hôte par son API C ;
un éventuel accesseur de handle GLib reste à définir, distinct du
`IvyContext*` actuellement retourné par `native_handle()`.

Le plan, les lots et les critères de validation sont mis à jour dans
[mainloop_future_impl.md](mainloop_future_impl.md). Le README et la documentation
de `Bus` reflètent cette direction. Cette mise à jour est documentaire ; aucune
API ni implémentation de boucle n’est ajoutée.


## ML natif et GLib — implémentation du 15 septembre 2026

Périmètre de ce lot : `Bus::run()` bloquant et factories GLib renvoyant le même
`ivy::Bus`, avec distribution séparée. Aucune méthode d’itération manuelle,
aucun thread implicite et aucun adaptateur Qt ajouté.

### C — pilotage et coordination avec les timers

- `IvyContextRun()` retourne un `IvyStatus` explicite ; la fonction historique
  `IvyContextMainLoop()` conserve sa signature et lui délègue le pilotage.
- `ivy_loop_internal.h` sépare acquisition, exécution et libération du pilote.
  L’acquisition partage le verrou de contexte avec la création des timers et
  l’arrêt. Double pilotage et réentrée sont refusés. `Destroy` refuse un `Run`
  encore actif : l’appelant doit arrêter et joindre le thread avant destruction.
- Le backend select conserve le contrat d’initialisation du contexte legacy,
  mais le pilote vérifié ne réinitialise pas une boucle déjà initialisée. Il
  protège l’état d’arrêt, ignore les fd retirés pendant un dispatch, interrompt
  la sélection des callbacks timers à l’arrêt et remonte les erreurs de select.
- Les commandes de création de timer acceptées avant stop sont achevées ou
  refusées avec `IVY_ESTOPPED` lors du drainage de sortie, sans laisser leur
  appelant attendre un pilote déjà sorti.
- Le pilote GLib rejette la réentrée depuis un dispatch et remonte les erreurs
  de polling. Son chemin vérifié utilise les primitives prepare/query/poll/check/
  dispatch de GLib. L’arrêt ne quitte pas la boucle hôte.
- Fichiers : `ivy.c`, `ivy.h`, `ivyloop.c`, `ivyloop.h`, `ivyglibloop.c`,
  `timer.c`, les deux nouveaux en-têtes internes `ivy_loop_internal.h` et
  `ivy_timer_internal.h`, dépendances Makefile et export `libIvy.def`.

### C++ — API et distribution

- `Bus::run()` dans `api/mainloop.hpp` / `ivy_mainloop.cpp`, résultat
  `expected<void, error_code>`, `noexcept`. Les erreurs de callbacks restent
  exclusivement récupérées avec `take_callback_error()`.
- `ivy_glib.hpp` / `ivy_glib.cpp` exposent `ivy::glib::create_bus()` avec contexte
  courant/par défaut ou explicite, et les mêmes callbacks mobiles que la factory
  habituelle. Le contexte thread-default est restauré sur tous les retours ;
  un contexte possédé par un autre thread donne `IVY_ESTATE` sans attente.
- `ivy-cpp` lie `libivy`; `ivy-cpp-glib` lie `libglibivy`. Archives, bibliothèques
  partagées, SONAME et pkg-config séparés, même API Bus. Un processus choisit une
  seule variante. Les types GLib restent dans l’en-tête optionnel.
- Cibles `cpp-glib` / `install-cpp-glib` ajoutées au Makefile Linux. Les headers
  privés C restent non installés. La dépendance C++23 reste optionnelle.
- Exemples C++ existants convertis à `run()`. `examples/cpp/glib.cpp` illustre
  la boucle hôte qui continue après l’arrêt d’Ivy. Doxygen inclut les nouveaux
  guides ; `ivy.hpp` conserve son exemple annoté et sa carte de lecture.

### Validation

- `tests/cpp/run.sh` : contrôles de compilation, frontière C simulée, erreurs
  de callback, PCRE2, intégration multibus, en-têtes installés et exemples ;
  `mainloop_test.cpp` ajoute timers sans trafic, double pilotage/réentrée,
  stop interthread, courses timer/run/stop et erreur native de select. Statique
  et partagé sont exercés depuis une installation temporaire.
- `tests/cpp/run_glib.sh` : factories et restauration des contextes sur succès/
  échec, captures mobiles, contexte déjà possédé, threads distincts, erreur de
  polling, échanges réels entre deux bus sur une boucle hôte, timers applicatifs
  encore actifs après l’arrêt d’un bus, bibliothèques statiques/partagées,
  dépendances ELF et exemple compilé/exécuté depuis l’installation temporaire.
- Régressions C réussies : phases 2, 4, 5, 6, 9 select/wakeup, phase 11 runtime
  errors, backend GLib et erreurs de transport (normal, OpenMP et GLib).
- Les tests réseau ont été exécutés avec l’autorisation de sockets locales ;
  la sandbox par défaut refuse leur création avec `EPERM`.

Ces groupes peuvent être séparés en commits C, C++ natif, puis C++ GLib avec leurs
exemples/tests/docs respectifs. Ne pas inclure automatiquement les modifications
antérieures présentes dans les fichiers partagés. Aucun commit ni staging effectué.

Validation finale du lot : les deux suites C++ (native et GLib) ont réussi,
ainsi que les régressions C listées ci-dessus. `git diff --check` est propre.
Doxygen est généré sans avertissement C++ ; les avertissements C préexistants
restent hors de ce lot. [QT6_INTEGRATION.md](QT6_INTEGRATION.md) consigne ensuite
l’étude Qt6 et l’essai des primitives QtCore 6.4.2, sans adaptateur Ivy/Qt livré.


## Qt6 — remplacement complet de l’exemple par l’API C++23

Périmètre utilisateur : remplacer l’ancienne implémentation de `examples/ivyqt`,
conserver les envois MT-safe directs depuis Qt et une file de notifications Ivy
vers Qt, avec un helper dans `src/cpp` lorsque cela simplifie l’utilisation.

### Bibliothèque

- `IvyContextRequestStop()` partage le chemin d’arrêt existant sans attendre la
  condition de fin du pilote. `IvyContextStop()` conserve son attente habituelle.
  Déclaration C et export Windows ajoutés ; `Bus::request_stop()` expose le
  résultat avec les conventions C++ existantes.
- `ivy_thread.hpp` / `ivy_thread.cpp` ajoutent `ivy::LoopThread`. Sa factory
  accepte un Bus déjà démarré et un callback de fin optionnel, avec captures
  mobiles. Les erreurs de construction/allocation/thread sont converties en
  `expected`. Le helper possède le thread, emprunte le Bus à adresse stable,
  propose `request_stop()` et `join()`, et assure arrêt/join dans son destructeur.
- Le callback de fin s’exécute sur le worker après `run()`, éventuellement avant
  le retour de `create()`. Il peut publier une notification Qt ; il ne doit pas
  attendre que la GUI le joigne. Les erreurs du pilote/de ce callback sont
  conservées pour `join()`. Celles des callbacks Ivy restent dans
  `Bus::take_callback_error()`.
- Le helper est installé avec les variantes native et GLib. Il ne dépend pas
  de Qt et ne crée pas de nouvelle variante de backend ou de file vers Ivy.

### Exemple Qt6

- `main.cpp` est remplacé par le démarrage C++23 vérifié ; `window.hpp` et
  `window.cpp` portent l’interface et les connexions Qt. Aucune classe IvyBridge
  historique ni aucun appel à l’API C / `native_handle()` ne subsiste.
- L’interface conserve ON/OFF, réception du texte capturé et envois depuis des
  workers. Le pool de démonstration utilise `QThreadPool`, limité à quatre threads
  réutilisables ; les messages donnent le vrai thread appelant et une séquence.
- Les réceptions copient les vues Ivy en `QString` avant d’émettre les signaux
  connectés avec `Qt::QueuedConnection`. Les envois GUI/workers appellent
  directement `bus.send()` et vérifient le résultat d’acceptation locale.
- La fermeture normale demande l’arrêt sans attendre, continue à traiter Qt,
  puis joint les producteurs à réception de leurs notifications finales. Une
  destruction directe applique un arrêt/join de secours avant la destruction
  des tokens et du Bus. Qt retire les notifications restantes du destinataire.
- CMake >= 3.20, C++23, Qt >= 6.4 Widgets et pkg-config `ivy-cpp`. Qt Test est
  optionnel, activé par `IVY_QT_BUILD_TESTS`. Le README est réécrit pour cette version.

### Vérifications

- `tests/cpp/run_qt.sh` réussi : construction CMake contre une installation
  temporaire, Qt 6.4.2 hors écran, vrai pair Ivy, envois GUI/workers, réception
  UTF-8, erreur de démarrage, fermeture avec tâches/notifications en attente,
  demande distante `Die`, destruction directe et contrôle d’absence d’API C.
- `tests/cpp/run.sh` réussi en statique/partagé, avec les nouvelles vérifications
  `thread_test.cpp` : captures mobiles, arrêt sans attente pendant un callback
  actif, refus de join sur soi-même, déplacement, fermeture RAII, séparation des
  erreurs. `thread_failure.c` injecte `pthread_create` / `EAGAIN` pour vérifier
  que la factory retourne bien un `expected` d’erreur système.
- `tests/cpp/run_glib.sh` réussi, y compris ces scénarios de thread en
  statique/partagé sur des contextes GLib distincts.

Découpage possible : C (requête d’arrêt), C++ (API et helper), puis exemple/tests
Qt6 avec leurs documents. Préserver les changements antérieurs dans les fichiers
partagés. Aucun commit ni indexation effectué.

Validation finale du complément Qt : phases C 2, 4 et 9 select/wakeup réussies ;
`git diff --check` propre. Doxygen généré sans avertissement C++ après ajout des
contrats du helper. Les avertissements C préexistants restent hors de ce lot.


## Qt6 — moniteur universel, informations d’agent, saisie et ping

Demandes utilisateur : abonnement `(.*)`, affichage de tous les messages avec
adresse, port, nom et timestamp de réception ; réponse aux pings, ping de tous
les autres agents avec affichage du délai ; champ texte et bouton Send.

### C / C++

- `ApplicationInfo { name, address, port }` dans `api/application_types.hpp` et
  `Bus::application_info(peer)` dans `api/applications.hpp` / `ivy_application.cpp`.
  Copie possédée du nom, de l’IP numérique et du port TCP annoncé par le pair.
  La méthode existante `application(peer)` conserve sa paire nom/hostname.
- Le pont privé `IvyContextCopyApplicationInfoInternal` copie les champs sous
  le verrou de bindings ; `IvySocketCopyPeerAddressInternal` utilise
  getpeername/getnameinfo avec `NI_NUMERICHOST`, sans résolution inverse.
  Les erreurs laissent les sorties vides et le port à zéro ; aucune déclaration
  du pont n’est installée. Mise à jour des dépendances C du Makefile.
- Le port affiché est `app_port` annoncé par Ivy, pas le port UDP du bus ni
  le port éphémère d’une connexion sortante. Il peut être zéro avant handshake.

### Exemple

- `bind_unanchored(..., "(.*)")` conserve le message complet et un abonnement
  direct ajoute aussi les messages directs, avec leur identifiant.
- Journal via un modèle Qt de lignes possédées : IP, port, application,
  réception locale avec millisecondes, type, message. Le timestamp est pris
  dans le callback avant publication à Qt. Historique conservé pendant la
  session, suivi du bas de la vue et copie des lignes sélectionnées par Ctrl+C.
- Liste séparée des agents connectés, y compris silencieux et homonymes. Le
  registre de connexions n’est manipulé que par les callbacks/timers Ivy ; seuls
  identifiants locaux et copies sont transmis à Qt. Déconnexion : retrait de
  l’agent, conservation des messages déjà reçus.
- Ping Ivy initial puis toutes les deux secondes environ, une requête en attente
  par connexion, timeout de trois secondes et nouvelle tentative après deux
  secondes. RTT natif en microsecondes converti en millisecondes à l’affichage.
  Les pongs périmés sont ignorés. Le cœur répond déjà automatiquement aux pings.
- Champ libre UTF-8 et bouton Send / touche Entrée ; envoi direct depuis Qt,
  espaces et texte préservés, champ vidé après succès, conservé en cas d’erreur.
  Les contrôles ON/OFF/workers et la fermeture coordonnée restent disponibles.

### Validation

- `tests/cpp/run_qt.sh` réussi avec Qt 6.4.2 : abonnement universel, messages
  hors de l’ancien préfixe, journal complet, métadonnées et timestamp, UTF-8,
  copie, messages directs, Send/Entrée, ping dans les deux sens, agent muet et
  agents homonymes, répétition des mesures, timeout puis reprise, déconnexion
  et fermeture. Capture hors écran relue pour vérifier les colonnes.
- `tests/run_query_snapshots.sh` réussi : IP numérique, port après handshake,
  pairs invalides/étrangers, échecs d’allocation, sorties vidées, conservation
  des copies après déconnexion/destruction et état arrêté.
- `tests/cpp/run.sh` vérifie aussi la nouvelle API et ses copies dans les
  scénarios multibus en statique/partagé. Le test GLib exerce le même snapshot
  depuis les callbacks de sa boucle hôte.

Séparer le pont C et l’API C++ avant le commit de l’exemple Qt et de ses tests.
Aucun commit ni indexation effectué ; préserver les modifications antérieures.

Validation finale du moniteur : suites C++ native et GLib réussies en statique
et partagé, en plus des tests Qt et snapshots C. Doxygen ne signale aucun
avertissement C++ ; `git diff --check` est propre. Les avertissements C
préexistants restent hors de ce lot.
