# Ivy C : audit de durcissement préalable

Cette note recense les points de qualité et de durcissement à traiter avant une
refonte plus ambitieuse vers une API réentrante et multi-thread.

L'objectif n'est pas de réécrire `ivy-c`, mais de réduire les risques hérités
du style C/POSIX des années 90 : sorties brutales depuis une bibliothèque,
parsing permissif, buffers statiques, conversions implicites, API non
réentrantes et outils de compilation trop peu stricts.

## Méthode

Commandes exécutées pendant cet audit :

```sh
make -C src
```

Résultat : build OK avec les flags actuels.

```sh
gcc -std=gnu99 ... -Wall -Wextra -Wformat=2 -Wconversion \
    -Wsign-conversion -Wcast-align -Wstrict-prototypes \
    -Wmissing-prototypes -Wwrite-strings -Wundef -Wshadow \
    -fsyntax-only ...
```

Résultat : compilation syntaxique OK, mais environ 740 lignes de warnings,
notamment prototypes anciens, conversions signées/non signées, conversions de
tailles, paramètres inutilisés et boucles suspectes.

```sh
gcc-16 -std=gnu99 ... -fanalyzer -fsyntax-only ...
```

Résultat : pas de diagnostic profond supplémentaire sur ce passage, seulement
des warnings déjà visibles.

```sh
scan-build-20 --use-analyzer /bin/clang-20 --use-cc /bin/clang-20 \
    --status-bugs -o /tmp/ivy-scanbuild make -C src -B static-libs
```

Résultat : 4 diagnostics clang static analyzer :

- possible `NULL` passé à `strncmp()` dans `IvyBindingFilter()` ;
- dead store mineur dans `SocketWaitForReply()` ;
- division par zéro signalée dans `uthash` via `HASH_ADD_KEYPTR()` ;
- possible use-after-free dans `delOneClient()`.

Le diagnostic `uthash` doit être trié comme possible faux positif de l'analyseur
sur macro, mais les autres points méritent une correction ou au minimum une
clarification du code.

```sh
clang-20 --analyze -std=gnu99 ...
```

Résultat cohérent avec `scan-build-20`.

```sh
make -C src -B CC=/bin/clang-20 CFLAGS='... -fsanitize=address,undefined ...'
make -C tools -B CC=/bin/clang-20 CFLAGS='... -fsanitize=address,undefined ...' ivyprobe
ASAN_OPTIONS=detect_leaks=0 ./tools/ivyprobe -v
```

Résultat : build ASan/UBSan OK. Le smoke test `ivyprobe -v`, exécuté hors
sandbox pour autoriser les sockets, déclenche un UBSan réel dans `IvyStart()` :

```text
runtime error: left shift of 255 by 24 places cannot be represented in type 'int'
```

Le code a ensuite été reconstruit en mode normal avec `make -C src -B`.

Outil non exploitable dans cet environnement :

- `cppcheck` est installé, mais son installation est cassée (`std.cfg` absent).

## État après première passe de durcissement

La branche `phase1` contient une première passe de correction dans :

```text
b2c372e hardening: fix pre-MT audit safety issues
```

Commandes exécutées sur ce commit :

```sh
git diff --check
make -C src
tests/run_audit_hardening.sh
tests/run_phase1.sh
```

Points corrigés dans cette passe :

- `IvyFifoSendSocket()` ne draine plus la FIFO après un `send()` en erreur ;
- les allocations de FIFO sont vérifiées ;
- `BroadcastReceive()` borne les champs `appid` et `appname` ;
- `IvyStart()` initialise `addr`, borne la copie et remplace `atoi()` par
  `strtoul()` pour le port du bus ;
- le parsing IPv4 de `IvyStart()` utilise des temporaires non signés ;
- `IvyBindingFilter()` vérifie le token avant `strncmp()` ;
- `delOneClient()` ne parcourt plus `srcRegList` pendant qu'une fonction
  appelée peut aussi la modifier ;
- `make_message()` préserve l'ancien buffer si `realloc()` échoue ;
- les chemins socket ne transmettent plus une longueur négative après erreur
  de formatage ;
- `DeleteSocket()` libère le buffer de réception ;
- `SocketGetRemotePort()` retourne le port en ordre hôte ;
- un test de régression couvre le cas `IvyFifoSendSocket()` avec `send()` en
  erreur.

## État après les phases MT-safe 1 à 10

La migration multi-thread et multibus a maintenant avancé jusqu'à
`FEATURE/multi_bus-MT_safe_phase10`.

Points de durcissement ou de sûreté ajoutés depuis la première passe :

- `IvyContext` porte l'état principal de `src/ivy.c`, avec un contexte legacy
  paresseux ;
- les états de cycle de vie et les erreurs publiques `IvyStatus` permettent de
  distinguer arrêt, état invalide, allocation et I/O ;
- `IvySendMsg()` utilise des buffers locaux et un `bindings_rwlock` en lecture ;
- les mutations de bindings et de clients prennent `bindings_rwlock` en
  écriture ;
- les écritures vers un même client sont sérialisées par un verrou d'envoi ;
- la main loop POSIX dispose d'un canal de réveil et d'une file de contrôle ;
- `IvyStop()` appelé depuis un worker thread réveille la loop et attend
  l'état `STOPPED` ;
- les callbacks utilisateur sont appelés via des wrappers qui snapshotent le
  pointeur callback et ne gardent plus `ctx->mutex`, `bindings_rwlock` ou
  `client->send_lock` pendant l'appel ;
- un compteur de callbacks actifs empêche la destruction du contexte pendant
  une callback en cours ;
- `ivyloop.c`, `ivysocket.c` et `timer.c` ont maintenant des états
  contextuels, associés au contexte Ivy propriétaire ;
- l'API publique `IvyContext*` couvre désormais le cycle de vie, la boucle
  `select`, bind/change/unbind, send, direct, die, ping, callbacks bind/pong,
  queries applicatives et timers contextuels ;
- les queries modernes à buffer fourni par l'appelant existent pour éviter les
  buffers partagés dans les nouveaux usages ;
- `ivyprobe` est multibus : `IVYBUS` est démarré s'il existe, chaque `-b`
  ajoute un bus, les messages sortants sont diffusés sur tous les bus et les
  regexps sont posées sur tous les contextes ;
- `ivyprobe -t` utilise un timer contextuel unique qui diffuse sur tous les bus
  configurés ;
- la boucle `select` dispose maintenant d'un wakeup POSIX et d'un wakeup
  Windows compatible Winsock `select()` via sockets TCP loopback ;
- `ivythroughput`, `ivyperf`, `ivytestready`, `ivytranslater` et
  `examples/testUnbind.c` utilisent l'API contextuelle publique ;
- les tests `run_phase1.sh` à `run_phase10_tools.sh`,
  `run_audit_hardening.sh` et `run_phase3_multiprocess.sh` couvrent ces étapes.

Ce document reste utile pour les points non traités : politique globale
`SIGPIPE`, absence de garde `FD_SETSIZE`, isolation complète éventuelle de
`rand()/srand()`, parsers numériques historiques, stockage legacy possédé par
Ivy, types de taille et de temps, `intervalRegexp.c`, `ivytcl.c`, prototypes C
et politique de logging.

## Priorités hautes

### Ne plus appeler `exit()` depuis la bibliothèque

Plusieurs chemins de `src/ivysocket.c`, `src/ivybind.c`, `src/list.h`,
`src/ivyglutloop.c` et `src/ivyxtloop.c` appellent `exit()` en cas d'erreur
d'allocation, de socket, de bind, de listen ou d'initialisation.

Exemples :

- `src/ivysocket.c`: échec `socket`, `setsockopt`, `bind`, `listen`,
  allocation de buffer ;
- `src/list.h`: échec `malloc` dans `IVY_LIST_ADD_END` ;
- `src/ivybind.c`: échec `malloc` lors de la compilation de regexp.

Après revue, tous ces cas ne méritent pas le même niveau de priorité :

- les `exit()` sur échec d'allocation (`IvyContextCreate`,
  `IvyBindingCompile`, `IVY_LIST_ADD_END`, buffers socket, wrappers GUI) sont
  proches d'un scénario OOM. Ils restent hors périmètre phase 11 : si le
  processus ne peut plus allouer ces petites structures, il est généralement
  déjà dans un état que l'application ne saura pas récupérer proprement ;
- les `exit()` dans `SocketServer()` sur `socket()`, `setsockopt()`, `bind()`,
  `getsockname()` ou `listen()` sont des erreurs runtime normales possibles
  pour une bibliothèque. Même si `IvyStart()` utilise généralement `ANYPORT`,
  ces chemins ne devraient pas tuer le processus appelant ;
- les `exit()` sur `TCP_NODELAY` dans les connexions entrantes/sortantes sont
  trop agressifs. La bibliothèque doit échouer proprement la création de la
  connexion et notifier l'appelant par le retour existant quand il y en a un,
  plutôt que continuer silencieusement avec une option de transport non posée ;

Correction recommandée, par priorité :

1. traiter d'abord les `exit()` socket non-OOM (`SocketServer()` et
   `TCP_NODELAY`) ;
2. garder les `exit()` OOM hors périmètre immédiat ;
3. pour la future API à contexte, remplacer les erreurs fatales par :

- des retours d'erreur ;
- une erreur stockée dans le contexte Ivy futur ;
- ou, à défaut temporaire, un hook fatal configurable par l'application.

Ce point ne bloque donc pas la suite immédiate de la migration multi-thread,
mais les `exit()` socket non-OOM doivent être corrigés avant d'exposer une API
réentrante propre.

Statut après phase 11 : corrigé pour la boucle `select` et les sockets non-OOM.
Les échecs `TCP_NODELAY` ne font plus `exit()` : la connexion sortante échoue
par retour `NULL`, et la connexion entrante acceptée est fermée avant d'être
exposée à Ivy. Les erreurs serveur TCP non-OOM de `SocketServerFor()`
(`socket()`, `setsockopt()`, `bind()`, `getsockname()`, `listen()`) retournent
maintenant `NULL`, que `IvyContextStart()` propage en `IVY_EIO`. Les échecs
d'initialisation de la boucle `select` ou de son wakeup remontent aussi via
`IvyChannelInitFor()` / `SocketInitFor()` jusqu'à `IvyContextStart()`. Le test
`tests/run_phase11_runtime_errors.sh` couvre ces chemins par injection de
faute sous `IVY_TESTING`. `rg '\bexit\s*\('` signale encore des appels directs
liés aux OOM, au contexte legacy par défaut, aux macros `uthash` et aux
backends toolkit legacy ; ils restent hors périmètre phase 11.

### Corriger la gestion des retours négatifs de `send()`

Dans `src/ivyfifo.c`, `IvyFifoSendSocket()` stocke le retour de `send()` dans
un `unsigned int` :

```c
unsigned int maxLen, realLen;
realLen = send(fd, f->rptr, maxLen, MSG_DONTWAIT);
IvyFifoDrain(f, realLen);
```

Si `send()` retourne `-1` avec `EWOULDBLOCK`, `EAGAIN` ou `EINTR`, la valeur
est convertie en grand entier non signé, puis utilisée pour avancer le pointeur
de FIFO. C'est un vrai risque de corruption logique, voire mémoire.

Correction recommandée :

- utiliser `ssize_t realLen` ;
- traiter explicitement `-1` selon `errno` ou `WSAGetLastError()` ;
- ne drainer la FIFO que si `realLen > 0` ;
- propager un état congestion/erreur clair.

Statut : corrigé dans `b2c372e`, avec test de régression
`tests/run_audit_hardening.sh`.

### Corriger l'UBSan dans le parsing IPv4 de `IvyStart`

Le smoke test sanitizer de `ivyprobe -v` signale :

```text
ivy.c:1115:25: runtime error: left shift of 255 by 24 places cannot be represented in type 'int'
```

Le code concerné :

```c
mask = (mask ^ (0xff << (8*(3-numelem)))) | (elem << (8*(3-numelem)));
```

`0xff` et `elem` sont promus en `int`, puis décalés dans le bit de signe. C'est
un comportement indéfini en C.

Correction recommandée :

- utiliser des constantes et temporaires non signés (`uint32_t`, `0xffu`) ;
- valider explicitement `elem <= 255` ;
- isoler le parsing IPv4 dans une fonction testable ;
- remplacer si possible ce parsing manuel par `inet_pton()` plus une logique de
  masque/broadcast explicite.

Statut : le comportement indéfini UBSan est corrigé. Le parsing manuel reste en
place, mais il est isolé dans `ParseIvyIPv4Broadcast()` et utilise des types non
signés.

### Clarifier la mutation de `srcRegList` dans `delOneClient`

Clang static analyzer signale un possible use-after-free dans `delOneClient()`.
Le chemin suspect est :

```c
IVY_LIST_EACH_SAFE(client_itr->srcRegList, regxpSrc, next2) {
    delRegexpForOneClient(client_itr, regxpSrc->id);
}
```

`delRegexpForOneClient()` itère et modifie la même liste `srcRegList`. Même si
la macro `IVY_LIST_EACH_SAFE` sauvegarde le `next` avant le corps de boucle, le
code reste fragile : la fonction appelée peut libérer l'élément courant et
potentiellement invalider les hypothèses de l'itération appelante.

Correction recommandée :

- ne pas itérer une liste dans une fonction pendant qu'une fonction appelée
  peut aussi la modifier ;
- soit dépiler explicitement les éléments un par un ;
- soit extraire les ids à supprimer dans un tableau temporaire ;
- soit fusionner la suppression de client et la suppression de regexps dans une
  seule routine propriétaire.

Statut : corrigé dans `b2c372e` en dépilant explicitement les éléments de
`srcRegList`.

### Borner le parsing des messages réseau

Dans `src/ivy.c`, `BroadcastReceive()` lit un datagramme réseau avec :

```c
sscanf(line, "%d %hu %s %[^\n]", &version, &serviceport, appid, appname);
```

`appid` fait 128 octets et `appname` 2048 octets, mais les formats `%s` et
`%[^\n]` n'ont pas de largeur maximale. Un paquet UDP malformé peut dépasser
ces buffers.

Correction recommandée :

```c
sscanf(line, "%d %hu %127s %2047[^\n]", ...);
```

Mieux encore : remplacer `sscanf` par un parseur explicite qui vérifie les
longueurs, les conversions et les champs manquants.

Le même principe vaut pour les autres `sscanf`, par exemple le parsing des
intervalles dans `substituteInterval()`.

Statut : les largeurs de `BroadcastReceive()` sont corrigées dans `b2c372e`.
Le remplacement par un parseur explicite reste une amélioration possible.

### Durcir `IvyBindingFilter`

Clang static analyzer signale un possible passage de `NULL` à `strncmp()` :

```c
IvyBindingMatch(token_extract, expression, 1, &tokenlen, &token);
IVY_LIST_ITER(messages_classes, word,
    strncmp(word->word, token, tokenlen) != 0);
```

Le motif de regexp rend probablement `token` présent dans le chemin nominal,
mais l'API `IvyBindingMatch()` peut retourner `*arg = NULL`. Le code devrait
donc vérifier explicitement `token != NULL` et `tokenlen > 0` avant `strncmp`.

Correction recommandée :

- initialiser `token = NULL` ;
- vérifier le résultat de `IvyBindingMatch()` ou changer son API pour retourner
  un statut ;
- borner/caster proprement `tokenlen` vers `size_t` après validation.

Statut : corrigé dans `b2c372e`.

### Corriger l'initialisation et la copie du bus dans `IvyStart`

Dans `src/ivy.c`, `IvyStart()` déclare :

```c
char addr[1024];
```

Puis `addr` est utilisé pour tester une adresse IPv6. Si la chaîne de bus ne
contient pas de `:`, `addr` peut rester non initialisé. De plus :

```c
strncpy(addr, p, q-p);
addr[q-p] = '\0';
```

ne vérifie pas que `q-p` tient dans `addr`.

Correction recommandée :

- initialiser `addr` à zéro ;
- vérifier la longueur avant copie ;
- utiliser `memcpy` après contrôle de borne ou `snprintf` ;
- remplacer `atoi(q + 1)` par `strtoul` avec contrôle de plage `1..65535`.

Statut : corrigé dans `b2c372e`.

### Ne pas changer la politique globale `SIGPIPE` depuis la bibliothèque

Plusieurs boucles appellent :

```c
signal(SIGPIPE, SIG_IGN);
```

Modifier un signal est un effet global sur tout le processus. Une bibliothèque
ne devrait pas imposer cette politique à l'application.

Options de remplacement :

- utiliser `send(..., MSG_NOSIGNAL)` là où disponible ;
- utiliser `SO_NOSIGPIPE` sur les plateformes qui le fournissent ;
- laisser l'application gérer `SIGPIPE` ;
- si un réglage global reste nécessaire pour compatibilité, le documenter et
  le rendre optionnel.

Statut après phase 10 : encore ouvert. Les boucles `select`, GLib, Xt, Tcl et
GLUT appellent encore `signal(SIGPIPE, SIG_IGN)`.

### Vérifier `FD_SETSIZE` et envisager `poll`

`ivyloop.c` utilise `select()` et `fd_set` sans vérifier que les descriptors
restent inférieurs à `FD_SETSIZE`.

Risques :

- `FD_SET(fd, ...)` est indéfini si `fd >= FD_SETSIZE` ;
- `highestFd` n'est pas recalculé à la baisse lors des suppressions ;
- `select()` limite naturellement la scalabilité.

Durcissement minimal :

- refuser explicitement un fd trop grand ;
- retourner une erreur propre ;
- recalculer `highestFd` après suppression si nécessaire.

Direction moderne :

- migrer la couche channel vers `poll()` comme étape portable ;
- puis éventuellement `epoll/kqueue` derrière une abstraction.

Statut après phase 10 : encore ouvert. Les états `IvyChannelState` sont
contextualisés, mais `FD_SET()` n'est pas encore protégé contre un fd supérieur
ou égal à `FD_SETSIZE`.

## Priorités moyennes

### Remplacer les conversions `atoi/atol`

Occurrences dans `src/ivy.c`, `src/intervalRegexp.c` et plusieurs outils.

`atoi()` ne permet pas de distinguer une erreur de la valeur `0` et ne vérifie
pas les dépassements.

Préférer :

- `strtol` ou `strtoul` ;
- vérification de `errno` ;
- vérification de `endptr` ;
- contrôle explicite de plage avant cast.

Pour les ports : plage `1..65535`. Pour les tailles et compteurs : type cible
explicite.

Statut après phase 10 : partiellement traité. Le parsing du bus dans
`IvyStart()` utilise `strtoul()`, mais `atoi()/atol()` restent présents dans
`intervalRegexp.c` et certains outils.

### Remplacer `inet_ntoa`

`src/ivy.c` utilise `inet_ntoa()` pour afficher les adresses IPv4.

`inet_ntoa()` retourne un buffer statique et n'est pas réentrant. Préférer
`inet_ntop()` avec un buffer fourni par l'appelant. Cela sera de toute façon
nécessaire pour une API multi-contexte propre.

Statut après phase 11 : corrigé. L'affichage du broadcast IPv4 utilise
maintenant `inet_ntop()` avec un buffer local à l'appel, comme le chemin IPv6.

### Revoir l'identifiant applicatif

`GenApplicationUniqueIdentifier()` utilise :

```c
srand(curtime);
rand();
```

`rand/srand` est global, peu robuste, et perturbe l'état pseudo-aléatoire du
processus appelant. L'objectif ici n'est probablement pas cryptographique, mais
l'unicité doit être plus propre.

Alternatives :

- UUID système si disponible ;
- combinaison monotonic time + pid + compteur atomique + port ;
- générateur local au contexte, sans toucher au PRNG global du processus.

Statut après phase 11 : mitigé pour la sûreté multi-thread. Les appels à
`srand()` et `rand()` sont sérialisés par un verrou global Ivy, `srand()` n'est
plus appelé qu'une seule fois, et l'identifiant ajoute un compteur monotone
protégé par le même verrou afin que deux démarrages parallèles dans le même
processus ne produisent pas le même identifiant seulement parce qu'ils tombent
dans la même milliseconde. Ce choix conserve le PRNG C global pour compatibilité
et simplicité ; une génération totalement locale à Ivy reste une amélioration
possible si l'on veut éviter toute interaction avec le PRNG du processus.

### Supprimer les buffers statiques retournés

Exemples :

- `IvyGetApplicationList()` retourne `static char applist[4096]` ;
- `IvyGetApplicationMessages()` retourne un tableau statique ;
- `SocketGetPeerHost()` et `SocketGetRemoteHost()` utilisent des buffers
  statiques pour `getnameinfo`.

Ces APIs ne sont pas réentrantes et exposent des limites silencieuses.

Pour l'API legacy, garder le comportement si nécessaire. Pour une API durcie :

- fournir une variante à buffer appelant ;
- ou retourner un objet alloué/libéré explicitement ;
- ou copier l'information dans une structure résultat.

Statut après phase 11 : partiellement traité. Les variantes modernes à buffer
fourni par l'appelant existent pour les listes d'applications et de messages.
Les buffers `static` de `SocketGetPeerHost()`, `SocketGetRemoteHost()` et du
message d'erreur `IvyBindingCompile()` sont maintenant en TLS, ce qui évite
l'écrasement entre threads sans changer l'API legacy. Les wrappers legacy et
les queries contextuelles historiques gardent encore du stockage possédé par
Ivy ; les nouveaux usages doivent continuer à préférer les variantes à buffer
appelant, dont le retour indique la taille nécessaire terminateur `NUL` inclus.

### Remplacer les concaténations manuelles non bornées

`src/ivytcl.c` construit des scripts Tcl avec `strcpy` et `strcat`. Même quand
la taille de départ semble calculée, cela reste fragile, difficile à auditer et
ne gère pas correctement l'échappement Tcl.

`src/ivy.c` concatène aussi dans un buffer statique pour
`IvyGetApplicationList()`.

Préférer :

- `IvyBuffer` ou équivalent ;
- `snprintf` avec contrôle du retour ;
- API Tcl de construction de listes/objets si cette intégration reste
  maintenue.

Statut après phase 10 : encore ouvert pour `ivytcl.c`.

### Sécuriser `make_message`

`src/ivybuffer.c` utilise `vsnprintf`, ce qui est une bonne base, mais :

- `realloc` est assigné directement à `buffer->data`, ce qui perd l'ancien
  pointeur en cas d'échec ;
- les tailles sont des `int`, alors que les API C utilisent `size_t` ;
- plusieurs appelants ne vérifient pas le retour négatif ;
- le comportement Windows `_vsnprintf` historique diffère de C99.

Correction recommandée :

- utiliser un pointeur temporaire pour `realloc` ;
- convertir `IvyBuffer.size` et `IvyBuffer.offset` en `size_t` à terme ;
- vérifier systématiquement les retours ;
- encapsuler les différences Windows dans une fonction unique testée.

Statut : `realloc` utilise maintenant un pointeur temporaire dans `b2c372e`,
et les principaux chemins socket vérifient le retour négatif de
`make_message()`. La conversion de `IvyBuffer.size/offset` vers `size_t` reste
à faire.

### Normaliser les types de taille et de temps

Les warnings stricts montrent beaucoup de conversions :

- `long` vers `size_t` ;
- `ssize_t` vers `int` ou `unsigned int` ;
- `timeval` vers `unsigned long` ;
- `strlen()` vers `int`.

Ce n'est pas toujours un bug immédiat, mais cela rend les bornes et les erreurs
difficiles à raisonner.

Recommandations :

- tailles mémoire : `size_t` ;
- retours I/O : `ssize_t` ;
- ports : `uint16_t` côté stockage, `unsigned long` temporaire au parsing ;
- temps monotone : `uint64_t` en millisecondes ou nanosecondes ;
- éviter les casts implicites dans les chemins socket/FIFO.

Statut après phase 10 : encore ouvert, hors corrections ciblées déjà listées.

### Revoir `intervalRegexp.c`

Les warnings stricts signalent des boucles du type :

```c
for (i = nbDigitsMin - 1; i >= 0; i--)
```

avec `i` non signé. La condition `i >= 0` est toujours vraie. Le code peut
fonctionner grâce à des `break`, mais c'est fragile et mérite une correction.

Le même fichier contient aussi :

```c
vsprintf(buffer, fmt, args);
```

à remplacer par `vsnprintf`.

Statut après phase 10 : encore ouvert.

## Priorités basses mais utiles

### Moderniser les prototypes C

Plusieurs headers déclarent encore :

```c
void TimerScan();
```

En C, cela ne signifie pas "aucun argument", mais "arguments non spécifiés".
Préférer :

```c
void TimerScan(void);
```

Cela déclenche des warnings `-Wstrict-prototypes` et facilite l'analyse
statique.

Statut après phase 10 : encore ouvert. Des prototypes de type `TimerScan()`,
`TimerGetSmallestTimeout()` ou `IvyBindingGetFilterCount()` restent présents.

### Documenter la dépendance GNU `typeof`

`src/list.h` dépend de `typeof`, donc d'un dialecte GNU C. C'est acceptable si
le projet assume `gnu99`, mais il faut alors le documenter.

Comme l'objectif n'est pas de viser un C strict/pédant, ce n'est pas une
priorité de portabilité immédiate. Options possibles à terme :

- remplacer ces macros par des fonctions ou macros plus explicites ;
- utiliser une liste intrusive avec helpers typés par module ;
- ou accepter officiellement `-std=gnu99` et l'ajouter aux flags.

Statut après phase 10 : inchangé.

### Clarifier la politique de logging

Le code mélange `printf`, `fprintf(stderr, ...)`, `perror` et `TRACE`.

Pour une bibliothèque :

- éviter l'écriture directe sur stdout ;
- concentrer les logs via un callback ou une macro configurable ;
- retourner les erreurs à l'appelant quand c'est possible.

Statut après phase 10 : encore ouvert.

## Observations complémentaires

### Fuite probable des buffers socket

`DeleteSocket()` ferme le fd, détruit la FIFO et enlève le client de la liste,
mais le buffer de réception `client->buffer` alloué à la création du client
n'est pas libéré dans cette fonction.

À vérifier par test dédié, car la macro de liste libère la structure `client`,
mais pas ses champs alloués.

Statut : corrigé dans `b2c372e`. `DeleteSocket()` libère maintenant
`client->buffer`.

### `SocketGetRemotePort()` semble retourner le port en ordre réseau

Pour IPv4 comme IPv6, `SocketGetRemotePort()` retourne `sin_port` sans
`ntohs()`, contrairement à `SocketGetRemoteHost()`.

Ce point peut perturber les comparaisons avec les ports reçus dans le protocole
Ivy, qui sont manipulés comme entiers hôte. À vérifier avec un test de double
connexion.

Statut : corrigé dans `b2c372e`.

### `IvyFifoNew()` ne vérifie pas l'échec de `malloc`

`IvyFifoNew()` alloue la structure puis appelle `IvyFifoInit()` sans vérifier
que la structure a bien été allouée. `IvyFifoRealloc()` ne vérifie pas non plus
l'allocation du nouveau buffer avant de l'utiliser.

Ces cas sont rares en pratique, mais ils vont dans la même catégorie que les
`exit()` : il faut définir une politique d'erreur mémoire cohérente.

Statut : corrigé dans `b2c372e`. Les allocations de FIFO peuvent maintenant
échouer sans dereferencer un pointeur nul. La politique d'erreur mémoire globale
reste à définir.

### `ivythroughput -b` pouvait dépasser le buffer du bus

Pendant la vérification multiprocess de la phase 5, `ivythroughput` a aborté
avant même le démarrage du bus avec :

```text
*** buffer overflow detected ***: terminated
```

La pile montrait un `strcpy()` de l'argument `-b` dans un buffer alloué par
`strdup()` à partir de la valeur par défaut :

```c
bus = strdup("127.0.0.1:2000");
...
strcpy(bus, optarg);
```

Une adresse de bus plus longue que la valeur par défaut écrasait donc le
buffer. Ce bug était dans l'outil de test, pas dans la bibliothèque, mais il
masquait les tests réseau multiprocess.

Statut : corrigé dans `73e4d10`. L'option `-b` libère l'ancienne chaîne,
duplique `optarg` et vérifie l'échec d'allocation.

## Ordre de travail recommandé

1. Ajouter une cible d'audit de compilation, sans changer le build release :

```sh
make audit
```

avec au minimum :

```sh
-std=gnu99 -Wall -Wextra -Wformat=2 -Wstrict-prototypes
-Wmissing-prototypes -Wwrite-strings -Wundef -Wshadow
```

Garder `-Wconversion` et `-Wsign-conversion` dans une cible séparée au départ,
car le bruit est important.

2. Corriger les défauts sûreté haute priorité.

Statut : première passe effectuée dans `b2c372e` pour :

- `IvyFifoSendSocket()` et retours négatifs de `send()` ;
- `BroadcastReceive()` et largeurs de parsing ;
- `IvyStart()` et parsing/copie du bus ;
- fuite de `client->buffer` ;
- `realloc` direct dans `make_message`.

À compléter ensuite par des tests plus intégrés, notamment fermeture répétée de
clients et paquets broadcast longs.

3. Remplacer seulement les `exit()` non-OOM de la bibliothèque par des retours
d'erreur ou un hook fatal temporaire.

Priorité courte :

- `SocketServer()` : `socket()`, `setsockopt()`, `bind()`, `getsockname()`,
  `listen()` ;
- `TCP_NODELAY` sur connexion acceptée ou sortante.

Statut après phase 11 : traité pour la boucle `select` et les sockets non-OOM.
`SocketServerFor()` retourne `NULL` pour les erreurs runtime normales, les
échecs `TCP_NODELAY` échouent la connexion concernée sans tuer le processus, et
les échecs d'initialisation de wakeup remontent en `IVY_EIO` via
`IvyContextStart()`. Les `exit()` sur échec d'allocation restent hors périmètre
immédiat.

4. Moderniser les parsers numériques (`strtol/strtoul`) et les types de
taille (`size_t`, `ssize_t`).

5. Remplacer les API non réentrantes ou globales :

- `inet_ntoa` vers `inet_ntop` ;
- `rand/srand` vers génération locale ou, à défaut legacy, verrou + seed unique ;
- `signal(SIGPIPE, SIG_IGN)` vers politique configurable ou `MSG_NOSIGNAL`.

6. Ajouter des tests ciblés de régression :

- paquet broadcast trop long ;
- bus sans `:port` ;
- congestion socket avec `send()` retournant `EWOULDBLOCK` ;
- fermeture répétée de clients sous valgrind ;
- fd supérieur ou égal à `FD_SETSIZE` si testable ;
- interval regexp avec bornes limites.

7. Continuer la migration `IvyContext` décrite dans `IVY_MTSAFE.md`.

Statut : les phases 1 à 10 sont maintenant implémentées pour la boucle
`select` et les outils maintenus dans cette ligne. La priorité suivante, côté
qualité pure, n'est plus la migration multibus elle-même mais la fermeture des
points d'audit encore ouverts : `SIGPIPE`, `FD_SETSIZE`, parsers historiques,
isolation complète éventuelle de `rand()/srand()` et nettoyage des backends
legacy.

## Ligne directrice

Le durcissement doit préserver le comportement réseau existant, mais remplacer
les hypothèses implicites par des contrats explicites :

- une bibliothèque ne tue pas le processus sur une erreur runtime normale ;
- un parser borne ses entrées ;
- une API standard est appelée avec les bons types ;
- une erreur système est propagée ou loggée de façon contrôlée ;
- un buffer a un propriétaire clair ;
- les fonctions legacy peuvent rester compatibles, mais les nouvelles variantes
  doivent être vérifiables et réentrantes.
