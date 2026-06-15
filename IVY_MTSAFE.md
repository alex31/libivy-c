# Ivy C : réentrance et sûreté multi-thread

Cette note rassemble les constats et une direction de conception pour rendre
`ivy-c` capable de gérer plusieurs bus Ivy dans un même processus, puis de
tolérer des appels API depuis plusieurs threads applicatifs sur un même bus.

Le point essentiel : ce chantier doit être traité comme une migration
d'architecture, pas comme l'ajout ponctuel de quelques mutex autour de l'état
global existant.

## État d'avancement

État au terme de la branche `FEATURE/multi_bus-MT_safe_phase9` :

- phase 1 terminée : l'état mutable principal de `src/ivy.c` est porté par
  `IvyContext`, avec un contexte legacy construit paresseusement ;
- durcissement préalable terminé pour les points critiques corrigés dans
  `b2c372e`, notamment FIFO, parsing bus/broadcast, erreurs de formatage et
  fuite du buffer socket ;
- phase 2 terminée : `IvyStatus`, `IvyContextState`, `IvyGetLastError()` et la
  sémantique post-stop sont disponibles ;
- phase 3 terminée : verrous de base, `bindings_rwlock`, verrou d'envoi par
  client, buffers locaux dans `IvySendMsg()` et tests multi-thread/multiprocess ;
- phase 4 terminée : wakeup POSIX de la main loop, file de contrôle, stop
  synchrone depuis un worker et routage des changements writable vers la loop ;
- phase 5 terminée : les callbacks utilisateur ne sont plus appelés sous
  `ctx->mutex`, `bindings_rwlock` ou `client->send_lock`, les pointeurs de
  callback sont snapshotés avant appel, et un compteur de callbacks actifs
  protège la durée de vie du contexte ;
- phase 6 terminée pour la boucle select principale : `ivyloop.c`,
  `ivysocket.c` et `timer.c` disposent maintenant d'états contextuels
  indépendants, le contexte legacy reste explicitement raccordé aux wrappers
  historiques, et `IvyContextMainLoop()` / `IvyContextIdle()` exécutent la
  boucle propriétaire d'un contexte donné ;
- API contextuelle publique étendue : bind/change/unbind, send/error/direct,
  die, ping, callbacks bind/pong et queries applicatives existent maintenant
  en variantes `IvyContext*`; les wrappers legacy s'appuient sur le contexte
  courant, initialisé par le `IvyStart()` historique ;
- `ivyprobe` est multi-bus pour la boucle select : `IVYBUS` est démarré s'il
  existe, chaque option `-b` ajoute un bus, les envois sont diffusés sur tous
  les contextes, et les regexps sont posées sur tous les bus configurés ;
- phase 6.5 testée : `tests/run_phase6.sh` vérifie maintenant aussi l'API
  publique multibus sans hook `IVY_TESTING`, avec deux bus, deux pairs par bus,
  queries, send, direct, ping, change/unbind et arrêt indépendant d'un bus ;
- correctif outil associé : `ivythroughput -b` réalloue maintenant la chaîne
  du bus au lieu d'écraser le buffer alloué pour la valeur par défaut.
- phase 8 terminée : `IvyContextTimerRepeatAfter()` expose un timer
  contextuel minimal, et `ivyprobe -t` utilise un timer unique qui diffuse les
  messages de test sur tous les bus configurés ;
- phase 9 terminée pour la boucle `select` principale : le wakeup Windows est
  maintenant compatible `select()` via une paire de sockets TCP loopback
  émulant un `socketpair`, et le test `tests/run_phase9_select_wakeup.sh`
  couvre le cas `IvyContextStop()` appelé depuis un autre thread pendant que la
  loop est bloquée.

Limites encore présentes :

- le filtrage global de `ivybind.c` n'est pas encore contextualisé ;
- les backends de boucle alternatifs GLib, Xt, Tcl et GLUT conservent leur
  modèle global historique ;
- les callbacks issus du dispatch réseau restent exécutés dans le thread de
  loop, mais les événements congestion/FIFO produits par un appel `IvySendMsg()`
  depuis un worker sont seulement sortis des verrous internes à ce stade ; leur
  repost systématique vers la loop propriétaire reste une optimisation/garantie
  à formaliser avec la contextualisation complète ;
- les API legacy de query gardent encore leurs buffers et handles historiques.
- les outils historiques autres que `ivyprobe` restent à inventorier et à
  porter, quand cela apporte un exemple utile d'API contextuelle.

## Situation actuelle

L'implémentation historique supposait implicitement :

- un seul bus Ivy par processus ;
- une seule boucle d'événements propriétaire ;
- des callbacks utilisateur appelés directement depuis le dispatch réseau ;
- des pointeurs internes exposés comme handles publics.

Les branches `FEATURE/multi_bus-MT_safe_phase6` à
`FEATURE/multi_bus-MT_safe_phase9` ont levé les trois premières
frontières globales pour la boucle select principale. L'état Ivy principal est
porté par `IvyContext`; la boucle, les sockets et les timers disposent d'états
contextuels séparés. Les wrappers legacy restent une façade sur un contexte par
défaut et conservent donc une partie du modèle historique pour la compatibilité.

État des principales zones historiquement globales :

- `src/ivy.c` : identité applicative, callbacks, sockets TCP/UDP, binds locaux,
  clients connectés, dictionnaires de regexps, ready message et mode IPv4/IPv6
  sont dans `IvyContext`; les globals restants concernent le contexte legacy,
  l'erreur thread-local et le contexte courant thread-local ;
- `src/ivyloop.c` : les channels, `fd_set`, `highestFd`, `MainLoop`, hooks et
  wakeup POSIX/Windows sont portés par `IvyChannelState`; un état par défaut
  subsiste pour l'API legacy ;
- `src/ivysocket.c` : les listes de sockets serveur/client sont portées par
  `SocketState`, associé à sa boucle propriétaire ;
- `src/timer.c` : la liste de timers et le timeout de `select()` sont portés
  par `IvyTimerState`, lui-même attaché à la boucle ;
- `src/ivybind.c` : la table de filtrage des regexps reste globale.

Plusieurs fonctions utilisent aussi des buffers `static` pour éviter des
allocations répétées. C'est pratique dans une boucle mono-thread historique,
mais incompatible avec une API strictement réentrante. Plusieurs chemins ont
été corrigés, notamment `IvySendMsg()` et les buffers scratch principaux, mais
les queries legacy et leurs variantes contextuelles actuelles retournent encore
des buffers possédés par le contexte. Certaines zones ont un traitement OpenMP
`threadprivate`, mais cela ne couvre qu'un chemin étroit regexp/envoi.

`IvyStop()` est maintenant une façade sur `IvyContextStop()` du contexte
courant. Sur POSIX, la boucle select est réveillée par un pipe non bloquant
porté par son `IvyChannelState`. Sur Windows, elle est réveillée par deux
sockets TCP loopback non bloquantes, afin que le descripteur de réveil reste
compatible avec le `select()` Winsock existant.

## Objectifs

La cible utile serait :

- plusieurs bus Ivy indépendants dans un même processus ;
- un état et une durée de vie explicites pour chaque bus ;
- des appels `bind`, `unbind`, `send`, `ping`, `direct send` et `stop`
  possibles depuis plusieurs threads applicatifs sur un même bus ;
- un arrêt de bus observable par les autres threads qui utilisent ce bus ;
- des threads en attente réveillés pendant l'arrêt, avec un résultat défini
  au lieu d'une course avec de la mémoire libérée ;
- des callbacks utilisateur capables de rappeler l'API Ivy sans deadlock ;
- une API historique conservée comme façade sur un contexte par défaut.

Hors périmètre pour la première étape :

- modifier le protocole réseau Ivy ;
- réécrire le moteur de regexp ;
- rendre chaque objet interne indépendamment concurrent ;
- imposer immédiatement un nouveau framework d'event loop aux applications.

## Modèle public actuel et direction

Le contexte opaque est exposé :

```c
typedef struct IvyContext IvyContext;
```

L'API contextuelle publique actuellement exposée couvre le cycle de vie, la
boucle select, les callbacks, bind/change/unbind, send/direct/die/ping et les
queries applicatives :

```c
typedef enum {
    IVY_OK = 0,
    IVY_ESTOPPED = -1,
    IVY_ESTATE = -2,
    IVY_EINVAL = -3,
    IVY_ENOMEM = -4,
    IVY_EIO = -5
} IvyStatus;

typedef enum {
    IVY_CTX_CREATED,
    IVY_CTX_STARTING,
    IVY_CTX_RUNNING,
    IVY_CTX_STOPPING,
    IVY_CTX_STOPPED,
    IVY_CTX_DESTROYED
} IvyContextState;

IvyContext *IvyContextCreate(
    const char *appname,
    const char *ready,
    IvyApplicationCallback callback,
    void *data,
    IvyDieCallback die_callback,
    void *die_data);

int IvyContextStart(IvyContext *ctx, const char *bus);
int IvyContextStop(IvyContext *ctx);
int IvyContextDestroy(IvyContext *ctx);
void IvyContextMainLoop(IvyContext *ctx);
void IvyContextIdle(IvyContext *ctx);

int IvyContextSetBindCallback(IvyContext *ctx,
    IvyBindCallback bind_callback, void *bind_data);
int IvyContextSetPongCallback(IvyContext *ctx,
    IvyPongCallback pong_callback);

MsgRcvPtr IvyContextBindMsg(IvyContext *ctx,
    MsgCallback callback, void *user_data, const char *fmt_regexp, ...);
MsgRcvPtr IvyContextChangeMsg(IvyContext *ctx,
    MsgRcvPtr msg, const char *fmt_regexp, ...);
int IvyContextUnbindMsg(IvyContext *ctx, MsgRcvPtr msg);

int IvyContextSendMsg(IvyContext *ctx, const char *fmt_message, ...);
int IvyContextBindDirectMsg(IvyContext *ctx,
    MsgDirectCallback callback, void *user_data);
int IvyContextSendDirectMsg(IvyContext *ctx, IvyClientPtr app, int id, char *msg);
int IvyContextSendDieMsg(IvyContext *ctx, IvyClientPtr app);
int IvyContextSendPing(IvyContext *ctx, IvyClientPtr app);

IvyClientPtr IvyContextGetApplication(IvyContext *ctx, char *name);
char *IvyContextGetApplicationList(IvyContext *ctx, const char *sep);
char **IvyContextGetApplicationMessages(IvyContext *ctx, IvyClientPtr app);

IvyContextState IvyContextGetState(IvyContext *ctx);
IvyStatus IvyGetLastError(void);
```

L'API historique peut rester une façade :

```c
static IvyContext *default_ctx;

static IvyContext *IvyGetDefaultContext(void)
{
    if (default_ctx == NULL)
        default_ctx = IvyContextCreateLegacyDefaults();
    return default_ctx;
}

int IvyInit(...)              { return IvyDefaultContextInit(...); }
int IvyStart(const char *bus) { IvySetCurrentContext(IvyGetDefaultContext()); return IvyContextStart(IvyGetDefaultContext(), bus); }
int IvyStop(void)             { return IvyContextStop(IvyGetCurrentContext()); }
MsgRcvPtr IvyBindMsg(...)     { return IvyContextBindMsg(IvyGetCurrentContext(), ...); }
```

Le contexte par défaut de compatibilité doit être construit paresseusement :
un programme qui utilise uniquement l'API contextuelle ne doit pas allouer ni
initialiser `default_ctx`. Ce pointeur global reste le seul état global mutable
accepté pour la compatibilité legacy ; il pointe vers un contexte ordinaire.

Changer les fonctions historiques `void` en `int` ne casse pas les usages
existants qui ignorent simplement la valeur de retour. Les fonctions qui
retournent déjà un `int` conservent leur domaine positif ou nul et réservent
les valeurs négatives aux erreurs Ivy. Les fonctions qui retournent un pointeur
signalent l'échec par `NULL`; `IvyGetLastError()` permet alors de distinguer
une absence normale d'un arrêt du bus.

Convention proposée :

- `IVY_OK` indique le succès ;
- `IVY_ESTOPPED` indique que le contexte est en arrêt ou arrêté ;
- `IVY_ESTATE` indique un appel invalide pour l'état courant ;
- `IVY_EINVAL`, `IVY_ENOMEM` et `IVY_EIO` couvrent les erreurs classiques ;
- `IvySendMsg()` retourne `-1` sur arrêt, car `0` signifie déjà "aucun
  destinataire".

Les callbacks utilisateur et les callbacks par défaut restent en `void`. Leur
valeur de retour ne doit pas devenir un mécanisme de contrôle Ivy.

## Contenu d'un contexte

La majorité des globals mutables du coeur Ivy ont migré dans `struct
IvyContext` ou dans des états possédés par lui (`IvyChannelState`,
`SocketState`, `IvyTimerState`). Les points encore ouverts concernent surtout
le filtrage global, les backends de boucle alternatifs et les conventions de
handles/buffers héritées.

Au niveau bus :

- nom d'application et identifiant unique ;
- ready message ;
- mode IPv4/IPv6 ;
- serveur TCP applicatif ;
- socket UDP de supervision ;
- ports applicatif et supervision ;
- clients Ivy connectés ;
- bindings locaux de réception ;
- dictionnaire des regexps distantes ;
- callbacks application, bind, die, direct et pong ;
- flags de debug portés par le contexte ;
- état de shutdown ;
- mutex et variables de condition ;
- file de contrôle ;
- canal de réveil de l'event loop.

L'état de boucle est désormais contextuel. Conceptuellement, il correspond à
une structure de ce type, aujourd'hui matérialisée par `IvyChannelState` :

```c
struct IvyLoop {
    Channel channels;
    fd_set read_fds;
    fd_set write_fds;
    IVY_HANDLE highest_fd;
    int running;
    ...
};
```

Les sockets sont associées à la boucle et au contexte qui les possèdent via
`SocketState`. Les timers sont attachés à la boucle via `IvyTimerState`, ce qui
permet à deux bus indépendants d'avoir des échéances et des attentes
indépendantes.

La table de filtrage des regexps dans `ivybind.c` reste la principale décision
ouverte :

- la garder globale seulement pour la compatibilité legacy ;
- préférer une table par contexte pour la nouvelle API ;
- si un défaut global subsiste, documenter qu'il ne concerne que le contexte
  par défaut.

## Modèle de threads

La première étape doit rester simple : rendre un bus existant sûr depuis
plusieurs threads, sans imposer que tous les messages transitent par une file.

Le modèle validé est hybride :

- chaque `IvyContext` a une boucle propriétaire ;
- toute API Ivy peut être appelée depuis n'importe quel thread applicatif ;
- l'état de cycle de vie du contexte est protégé par `ctx->mutex` ;
- le graphe des abonnements distants (`messSndByRegexp`, les bindings compilés
  et les listes de clients attachées à chaque regexp) est protégé par un
  `bindings_rwlock` ;
- `bind`, `unbind`, `change`, la déconnexion client et la destruction du bus
  prennent `bindings_rwlock` en écriture avant toute mutation ou libération ;
- `IvySendMsg()` prend `bindings_rwlock` en lecture pendant le parcours et le
  matching des regexps, afin que plusieurs threads appelants puissent matcher
  en parallèle sur des bindings immuables ;
- l'écriture sur une socket ou dans sa FIFO est sérialisée par un verrou
  d'envoi par client ;
- `direct send`, `ping`, `die` et les messages de protocole prennent aussi ce
  verrou d'envoi du client cible ;
- `stop` prend `ctx->mutex`, bascule l'état de cycle de vie, puis coordonne la
  fermeture avec la boucle propriétaire ;
- les callbacks issus du dispatch réseau sont appelés dans le thread
  propriétaire de la boucle ;
- les opérations qui manipulent la toolkit ou l'event loop sont exécutées dans
  le thread propriétaire de la boucle ;
- l'envoi normal reste direct : il n'est pas posté dans une file de messages.

Cette approche ne cherche pas le parallélisme maximal. Elle garantit surtout
que les tables de regexps ne sont pas modifiées pendant leur parcours, que le
parsing/matching des regexps peut avancer en parallèle entre threads, que les
octets envoyés sur une même socket ne sont pas mélangés, et que les callbacks
restent compatibles avec les toolkits graphiques non thread-safe.

Le chemin chaud `IvySendMsg()` ne doit donc pas poster systématiquement une
commande contenant le message. En régime nominal :

1. le thread appelant formate le message dans un buffer local ou thread-local ;
2. il prend `bindings_rwlock` en lecture ;
3. il parcourt les regexps distantes et exécute les bindings compilés ;
4. pour chaque destinataire correspondant, il prend le verrou d'envoi du client,
   écrit l'identifiant et le payload dans la socket ou la FIFO, puis relâche ce
   verrou ;
5. il relâche `bindings_rwlock` ;
6. il retourne le nombre de destinataires, ou une erreur négative.

Les bindings PCRE2 compilés sont utilisables en parallèle si les résultats de
match restent thread-local. C'est la condition à préserver pour que le
`bindings_rwlock` en lecture ne redevienne pas un verrou global de fait.

Une petite file de contrôle reste nécessaire pour les cas qui doivent être
traités par la boucle propriétaire :

```c
IVY_CTL_DEFERRED_CALLBACK
IVY_CTL_ADD_WRITABLE_WATCH
IVY_CTL_CLEAR_WRITABLE_WATCH
IVY_CTL_STOP
IVY_CTL_CLOSE_CLIENT
```

Cette file est un chemin basse fréquence. Elle ne transporte pas les messages
Ivy ordinaires et n'ajoute donc pas de coût `malloc/free` sur chaque
`IvySendMsg()`.

## Réveil de la boucle

`IvyStop()` et les événements de contrôle postés depuis un autre thread doivent
réveiller l'event loop.

Sur POSIX, utiliser un pipe, un `socketpair`, ou `eventfd` si disponible. Le
côté lecture est enregistré dans le `select()` du bus. Poster un événement de
contrôle écrit un octet ou incrémente l'eventfd. La boucle lit/drain le canal
de réveil, puis draine la file de contrôle.

Sur Windows, utiliser un équivalent compatible avec le mécanisme d'attente
choisi. Comme le code actuel utilise `select()`, l'objet de réveil devrait
être compatible socket. La phase 9 implémente ce chemin par une paire de
sockets TCP loopback non bloquantes : la socket de lecture est ajoutée au
`fd_set`, et `IvyChannelWakeFor()` écrit un octet côté écriture.

Sans ce mécanisme, un `IvyContextStop()` peut rester bloqué jusqu'à l'arrivée
d'un trafic réseau ou d'un timeout de timer.

Les backends GLib, Xt, Tcl et GLUT doivent suivre la même règle s'ils sont
modernisés, mais ils restent aujourd'hui des backends legacy mono-boucle. Un
programme qui veut plusieurs bus dans le même processus ou des appels depuis
workers doit utiliser la boucle `select` contextuelle. L'ajout/retrait effectif
d'une watch toolkit depuis un worker thread n'est donc pas encore garanti pour
ces backends.

## Sémantique d'arrêt

Chaque contexte devrait avoir une machine d'état explicite :

```c
IVY_CTX_CREATED
IVY_CTX_STARTING
IVY_CTX_RUNNING
IVY_CTX_STOPPING
IVY_CTX_STOPPED
IVY_CTX_DESTROYED
```

Règles proposées :

- `IvyContextStart()` est valide depuis `CREATED`; le redémarrage après
  `STOPPED` n'est pas supporté à ce stade ;
- `IvyContextStop()` est idempotent depuis `RUNNING`, `STOPPING` et `STOPPED` ;
- `IvyContextStop()` est synchrone côté API : quand il retourne `IVY_OK`,
  l'arrêt du contexte est terminé ou le contexte était déjà arrêté ;
- les nouveaux `bind/send/...` après `STOPPING` retournent `IVY_ESTOPPED`, ou
  `NULL` pour les fonctions à retour pointeur ;
- les threads déjà entrés dans une API Ivy finissent sous protection du verrou,
  puis observent l'état `STOPPING` ou `STOPPED` lors de leur appel suivant ;
- les threads en attente sur la condition d'arrêt sont réveillés quand l'état
  passe à `STOPPED` ;
- la boucle propriétaire exécute la partie qui touche les channels et la
  toolkit ;
- `IvyContextDestroy()` appelle `stop` si nécessaire, attend les callbacks en
  cours quand l'appel ne vient pas lui-même d'une callback, puis libère le
  contexte.

Pseudo-code hors cas callback :

```c
int IvyContextStop(IvyContext *ctx)
{
    IvyLock(ctx);
    if (ctx->state == IVY_CTX_STOPPED) {
        IvyUnlock(ctx);
        return IVY_OK;
    }

    if (ctx->state != IVY_CTX_STOPPING) {
        ctx->state = IVY_CTX_STOPPING;
        IvyPostControl(ctx, IVY_CTL_STOP);
        IvyWakeLoop(ctx);
    }

    while (ctx->state != IVY_CTX_STOPPED)
        IvyCondWait(&ctx->stop_done, &ctx->mutex);

    IvyUnlock(ctx);
    return IVY_OK;
}
```

Si `IvyContextStop()` est appelé depuis le thread propriétaire de la boucle, il
ne doit pas attendre sur lui-même. Il peut effectuer directement la séquence
d'arrêt si aucun callback utilisateur n'est en cours, ou marquer un stop
demandé et terminer la libération au point sûr immédiatement après le retour du
callback. Dans tous les cas, aucune callback utilisateur supplémentaire ne doit
être appelée après le passage en `STOPPING`.

Séquence d'arrêt dans la boucle :

1. Marquer le contexte `STOPPING`.
2. Refuser les nouvelles opérations utilisateur, sauf requêtes stop/join/destroy.
3. Notifier éventuellement les waiters locaux que le bus s'arrête.
4. Envoyer `Bye` aux clients connectés quand c'est encore possible.
5. Marquer sockets et channels pour suppression.
6. Drainer les suppressions différées dans la boucle.
7. Libérer listes de clients, dictionnaires de regexps, binds locaux, timers et
   événements de contrôle pendants.
8. Marquer le contexte `STOPPED`.
9. Réveiller tous les threads en attente sur la condition du contexte.

Un message Ivy distant `Die` devrait demander ce même arrêt ordonné. Il ne doit
pas libérer le contexte directement depuis la callback de réception alors que
la pile socket/channel le référence encore.

## Règles de callbacks

Aujourd'hui, les callbacks utilisateur sont appelés directement depuis le
dispatch réseau. Certaines arrivent pendant que des listes internes sont
parcourues ou modifiées.

Pour une API thread-safe :

- appeler les callbacks utilisateur uniquement depuis le thread propriétaire de
  la boucle ;
- différer vers la boucle propriétaire toute callback qui serait causée par une
  opération faite depuis un worker thread, par exemple une congestion détectée
  pendant un `IvySendMsg()` ;
- ne jamais appeler un callback utilisateur en tenant le mutex du contexte ;
- ne jamais appeler un callback utilisateur pendant qu'un itérateur interne
  pourrait être invalidé par une opération légale du callback, par exemple
  `unbind` ou `stop` ;
- soit prendre un snapshot des arguments puis relâcher les verrous avant
  l'appel, soit mettre les callbacks dans une file d'événements à dispatcher
  après l'action interne ;
- si un callback appelle `IvyContextStop()`, la demande de stop doit être
  acceptée sans attente sur soi-même. La libération finale est faite au point
  sûr qui suit le retour du callback.

C'est la règle centrale anti-deadlock. Un mutex global récursif masquerait une
partie des deadlocks, mais ne résoudrait ni l'invalidation d'itérateurs ni les
courses de durée de vie.

État actuel : les callbacks utilisateur ordinaires et les callbacks de bind
sont sortis des verrous internes. Les callbacks de congestion/FIFO détectées
sur un envoi depuis un worker thread restent le principal point à durcir : ils
doivent être systématiquement republiés dans la file de contrôle de la boucle
propriétaire avant d'être exposés à l'application.

## Durée de vie des objets

`IvyClientPtr` et `MsgRcvPtr` sont aujourd'hui des pointeurs bruts vers des
structures internes. C'est fragile si un autre thread peut arrêter le bus.

Compatibilité court terme :

- conserver `IvyClientPtr` et `MsgRcvPtr` comme handles opaques bruts ;
- documenter qu'ils sont valides seulement pendant la callback, ou jusqu'à
  unbind explicite selon le contrat existant ;
- garantir que stop attend la fin des callbacks en cours avant de libérer les
  structures qu'elles référencent.

Limite actuelle : ces handles restent utilisables comme pointeurs internes. Un
thread qui conserve un `IvyClientPtr` au-delà de la callback ou sans
coordination avec l'arrêt du contexte reste hors contrat sûr. La migration doit
donc éviter d'étendre cette convention aux nouvelles API.

Direction long terme :

- ajouter des compteurs de génération ou des handles référencés ;
- ajouter des API qui copient une information stable :

```c
int IvyContextGetApplicationInfo(
    IvyContext *ctx,
    IvyClientPtr app,
    IvyApplicationInfo *out);
```

- éviter les pointeurs vers buffers statiques pour les listes d'applications et
  de messages. Préférer des buffers fournis par l'appelant ou des objets
  résultat alloués/libérés explicitement.

## Buffers statiques

Plusieurs fonctions utilisent des buffers `static` pour limiter les
allocations. Ces buffers ne sont pas compatibles avec la réentrance.

Corrections préférées :

- utiliser des `IvyBuffer` locaux à la pile quand la durée de vie est locale ;
- stocker des scratch buffers dans `IvyContext` s'ils sont utilisés uniquement
  par le thread de boucle ;
- utiliser du thread-local seulement pour des données vraiment par thread et
  indépendantes de la durée de vie du bus ;
- remplacer les buffers de retour statiques par des API à buffer appelant pour
  les nouvelles fonctions contextuelles.

L'API legacy peut conserver ses conventions de propriété mémoire historiques,
mais elle doit alors être documentée comme couche de compatibilité avec
limitations.

État actuel : `IvyContextGetApplicationList()` et
`IvyContextGetApplicationMessages()` verrouillent le parcours, mais retournent
encore des buffers possédés par le contexte. C'est suffisant pour `ivyprobe` et
les tests phase 6.5, mais ce n'est pas le contrat final d'une API MT-safe
réentrante.

## Verrouillage

Garder peu de verrous, avec des responsabilités explicites :

- `ctx->mutex` : protège l'état du contexte, la file de contrôle et la
  condition de shutdown ;
- `bindings_rwlock` : protège `messSndByRegexp`, les listes de clients par
  regexp, les bindings compilés et leur durée de vie ;
- `client->send_lock` : protège l'ordre des octets envoyés vers une socket, la
  FIFO de congestion, et les transitions ajout/retrait de watch writable ;
- verrou récursif transitoire, si nécessaire : permet à une callback Ivy de
  rappeler l'API sans deadlock immédiat, le temps de retirer progressivement
  les callbacks des zones verrouillées ;
- compteur de callbacks en cours, optionnel : permet à stop/destroy d'attendre
  que les callbacks soient revenues.

À éviter :

- tenir `ctx->mutex`, `bindings_rwlock` en écriture, ou `client->send_lock`
  pendant un callback utilisateur ;
- tenir `bindings_rwlock` en écriture pendant une compilation de regexp ou une
  I/O socket si le travail peut être préparé hors verrou ;
- appeler une API GLib/Xt/Tcl/GLUT depuis un worker thread ;
- poster tous les messages Ivy ordinaires dans une file intermédiaire ;
- attendre la fin d'une boucle depuis son propre thread propriétaire.

## Optimisations ultérieures

La première version peut garder `bindings_rwlock` en lecture pendant toute la
durée de `IvySendMsg()`, y compris pendant les écritures socket protégées par
`client->send_lock`. C'est simple et suffisant pour autoriser plusieurs threads
à matcher les regexps en parallèle.

Une optimisation ultérieure consiste à découper `IvySendMsg()` en deux phases :

1. sous `bindings_rwlock` en lecture, matcher les regexps et construire une
   liste de travaux d'envoi contenant le client cible, l'id de binding et les
   arguments déjà extraits ;
2. prendre une référence sur chaque client cible ou sur un objet d'envoi stable ;
3. relâcher `bindings_rwlock` ;
4. parcourir les travaux et envoyer sous `client->send_lock` uniquement ;
5. relâcher les références client après envoi ou abandon.

Cette variante évite qu'un thread bloqué par une socket congestionnée empêche
un `bind`, `unbind`, `change` ou une déconnexion de prendre `bindings_rwlock`
en écriture. Elle demande en revanche un vrai protocole de durée de vie :
refcount client, état `closing`, et garantie qu'une socket ou sa FIFO ne sont
pas libérées tant qu'un travail d'envoi les référence encore.

Cette optimisation doit aussi éviter les callbacks utilisateur sous verrou.
Les événements de congestion, FIFO pleine ou erreur d'envoi doivent être
enregistrés pendant l'envoi puis dispatchés après libération de `client->send_lock`,
idéalement par la boucle propriétaire.

## Plan de migration

### Phase 1 : extraction mécanique du contexte

- Ajouter `IvyContext` comme structure interne.
- Déplacer l'état global de `src/ivy.c` dans ce contexte.
- Déplacer les callbacks, le ready message, les flags de debug, les listes de
  clients, le dictionnaire des regexps et les buffers scratch dans le contexte
  ou dans des objets possédés par lui.
- Garder un `default_ctx` global strictement limité à la compatibilité legacy.
- Construire `default_ctx` paresseusement au premier appel d'une API legacy qui
  en a besoin.
- Convertir les fonctions internes pour recevoir `IvyContext *ctx`.
- Conserver le comportement mono-bus via l'API historique.

Cette phase doit être essentiellement mécanique. Elle prépare les verrous en
leur donnant déjà un propriétaire clair : le contexte du bus.

Statut : implémentée dans `bf6a56d` avec `tests/run_phase1.sh`.

### Phase 2 : statut public et cycle de vie

- Ajouter `IvyStatus` et `IvyGetLastError()`.
- Changer les fonctions publiques `void` en `int` quand elles représentent une
  opération Ivy.
- Réserver les retours négatifs aux erreurs Ivy.
- Introduire l'état `CREATED/RUNNING/STOPPING/STOPPED`.
- Faire retourner `IVY_ESTOPPED` ou `NULL` aux appels faits après `STOPPING`.
- Garder l'API historique comme façade sur le contexte par défaut legacy.

Cette phase donne déjà aux threads un moyen simple et peu coûteux de découvrir
qu'un autre thread a stoppé Ivy.

Statut : implémentée dans `67f1857` avec `tests/run_phase2.sh`.

### Phase 3 : verrous de base et owner thread

- Ajouter `ctx->mutex` pour l'état de cycle de vie et la file de contrôle.
- Ajouter `bindings_rwlock` pour protéger le dictionnaire des regexps et les
  listes de clients par regexp.
- Ajouter un verrou d'envoi par client pour sérialiser les écritures socket et
  les FIFO de congestion.
- Enregistrer le thread propriétaire de la loop.
- Prendre `bindings_rwlock` en lecture dans `IvySendMsg()` et en écriture dans
  `bind`, `unbind`, `change`, déconnexion client et cleanup.
- Garantir que les callbacks utilisateur restent appelés par le thread de loop.
- Vérifier que les callbacks peuvent rappeler l'API Ivy sans deadlock.

Statut : implémentée dans `7496636` avec `tests/run_phase3.sh` et
`tests/run_phase3_multiprocess.sh`. La garantie complète "callback toujours
loop thread" reste à préciser pour les événements congestion/FIFO générés par
un worker thread.

### Phase 4 : wakeup et file de contrôle

- Ajouter le canal de réveil à la boucle.
- Ajouter une file basse fréquence pour les événements de contrôle.
- Router par cette file les callbacks différées, les changements de watch
  toolkit et le stop demandé depuis un worker thread.
- Ne pas router les messages Ivy ordinaires dans cette file.
- Rendre `IvyStop()` synchrone : il réveille la loop, attend `STOPPED`, puis
  retourne.

Statut : implémentée dans `3180a8f` avec `tests/run_phase4.sh`. Deux commits
liés complètent cette phase côté outils de test : `6a19d7e` pour la tolérance
aux doublons d'acknowledgements de vérification, et `ebfcf41` pour la commande
`.thread` de `ivyprobe`.

### Phase 5 : sécurité des callbacks

- Retirer les callbacks utilisateur des sections de mutation/verrouillage
  interne.
- Ajouter des snapshots d'événements callback si nécessaire.
- Ajouter un compteur de callbacks en cours ou un mécanisme équivalent de garde
  de durée de vie.
- Définir explicitement le comportement de `stop` et `unbind` appelés depuis
  une callback.

Statut : implémentée dans `aac2a4e` avec `tests/run_phase5.sh`. Les callbacks
peuvent appeler `IvyUnbindMsg()` et `IvyStop()` sans deadlock dans les cas
couverts. `IvyContextDestroy()` attend la fin des callbacks en cours quand il
est appelé hors callback ; depuis une callback, il retourne `IVY_ESTATE`.

### Phase 6 : contextualiser loop, sockets et timers

- Introduire un état de channels par contexte ou par boucle.
- Faire enregistrer les sockets sur leur boucle propriétaire.
- Sortir `servers_list` et `clients_list` des globals de `ivysocket.c`.
- Sortir l'état timer des globals de `timer.c`.
- Vérifier que deux contextes ne partagent plus leurs channels ni leurs timers.

Statut : implémentée dans `FEATURE/multi_bus-MT_safe_phase6` pour la boucle
select principale, avec wrappers legacy conservés sur un état par défaut,
test d'infrastructure `tests/phase6_context_loop_test.c` et test API publique
multibus `tests/phase65_public_multibus_api_test.c`.

### Phase 7 : nettoyage de l'API publique

- Ajouter des fonctions contextuelles de query sans buffers possédés par le
  contexte, avec buffer fourni par l'appelant. Ne pas introduire d'objet
  résultat ni de modèle d'allocation supplémentaire tant qu'un besoin réel ne
  l'impose.
- Conserver `IvyClientPtr` et `MsgRcvPtr` comme handles publics existants.
  Clarifier simplement leur durée de vie et éviter d'ajouter des handles
  référencés/générationnels sans bug concret à résoudre.
- Ajuster seulement les signatures qui posent un vrai problème d'usage ou de
  sûreté, en gardant les wrappers legacy et les fonctions contextuelles déjà
  exposées.
- Documenter les anciennes API comme wrappers sur le contexte par défaut.
- Documenter précisément quelles fonctions sont thread-safe et quelles limites
  restent liées aux signatures legacy.
- Documenter l'API publique directement dans `ivy.h` avec Doxygen, en montrant
  les exemples d'utilisation de l'API contextuelle pour les nouveaux projets et
  en marquant les wrappers legacy comme compatibilité.
- Ajouter des tests ciblés sur les queries à buffer appelant et sur les appels
  concurrents réalistes, sans batterie abstraite disproportionnée.

Statut : implémentée dans `FEATURE/multi_bus-MT_safe_phase7` pour le besoin
concret identifié : queries publiques à buffer fourni par l'appelant
(`IvyContextGetApplicationListBuffer()`,
`IvyContextGetApplicationMessagesBuffer()` et wrappers legacy), getters
contextuels de nom/hôte d'application, et `ivyprobe` recâblé pour ne plus
utiliser les wrappers legacy dans son code. L'API publique de `ivy.h` est
documentée en Doxygen avec exemples par fonction et un `Doxyfile` minimal
génère la documentation depuis ce header. Le tout est couvert par
`tests/run_phase7.sh` sur deux bus réels, complété par le test existant
`tests/run_phase6_ivyprobe.sh` pour le probe multibus. Les fonctions à buffer
retournent directement la taille du buffer à fournir, terminateur `NUL` inclus,
pour éviter un `+1` répété côté appelant. Les handles publics historiques
restent `IvyClientPtr` et `MsgRcvPtr`; aucun modèle de handles référencés n'a
été ajouté.

### Phase 8 : outils et timers multi-bus

- Ajouter une API minimale de timer contextuel :
  `IvyContextTimerRepeatAfter()`, qui crée le timer dans l'état timer associé à
  la boucle du contexte.
- Recâbler `ivyprobe -t` sur cette API timer contextuelle.
- Sémantique retenue pour le mode timer multi-bus : un timer applicatif unique,
  attaché à un contexte propriétaire, diffuse explicitement les messages
  `TEST TIMER n` sur tous les bus configurés. Il n'y a pas un timer indépendant
  par bus.
- Ajouter un test automatisé qui démarre `ivyprobe` sur au moins deux bus avec
  `-t` et vérifie que les messages `TEST TIMER 1` / `TEST TIMER 5` sont émis
  selon la sémantique retenue.
- Repasser les outils d'exemple et de diagnostic sur l'API contextuelle quand
  ils ont une raison métier d'être multi-bus.

Statut : implémentée dans `FEATURE/multi_bus-MT_safe_phase8` pour
`ivyprobe -t`. Le timer est créé via `IvyContextTimerRepeatAfter()` sur le
premier contexte de `ivyprobe`, puis le callback utilise `ProbeSendMsgAll()`
pour diffuser `TEST TIMER 1` à `TEST TIMER 5` sur tous les bus. Le test
`tests/run_phase8_ivyprobe_timer.sh` démarre deux peers, un par bus, et vérifie
que les deux reçoivent le début et la fin du flux timer. Le portage des autres
outils reste prévu pour la phase 10.

### Phase 9 : portabilité et backends de boucle alternatifs

- Implémenter ou valider un wakeup Windows compatible avec le `select()`
  existant, par exemple via socketpair émulé ou Winsock event.
- Contextualiser les backends GLib, Xt, Tcl et GLUT, ou documenter explicitement
  qu'ils restent limités au modèle legacy mono-boucle.
- Ajouter des tests de compilation et, si possible, des smoke tests pour les
  backends activables dans l'arbre.
- Vérifier que les changements de watch writable déclenchés par un worker
  thread sont toujours exécutés dans le thread propriétaire du backend concerné.

Statut : implémentée dans `FEATURE/multi_bus-MT_safe_phase9` pour la boucle
`select` principale. Le wakeup Windows utilise une paire de sockets TCP
loopback non bloquantes compatible Winsock `select()`. Les backends GLib, Xt,
Tcl et GLUT restent explicitement documentés comme legacy mono-boucle ; leur
portage n'est pas inclus parce qu'il impliquerait de redéfinir leur contrat
d'intégration toolkit au-delà du besoin multibus actuel. Le test
`tests/run_phase9_select_wakeup.sh` vérifie le cas concret d'un
`IvyContextStop()` appelé depuis un thread pendant que la loop est bloquée.

### Phase 10 : outils et exemples restants

- Inventorier les outils fournis (`ivythroughput`, `ivyperf`, `ivytestready`,
  probes alternatifs éventuels) et décider lesquels doivent devenir des
  exemples modernes MT-safe.
- Porter les outils retenus vers l'API contextuelle publique, sans utiliser les
  wrappers legacy sauf dans des tests de compatibilité explicitement nommés.
- Remplacer les timers legacy par `IvyContextTimerRepeatAfter()` quand l'outil
  possède un contexte.
- Ajouter des smoke tests simples pour les outils portés : lancement, connexion
  sur un bus local, action principale minimale et arrêt propre.
- Mettre à jour les manpages et exemples associés pour montrer l'API
  contextuelle dans les nouveaux usages.

## Protocole de test continu (TDD)

Afin de garantir l'absence de régressions lors de cette refonte architecturale complexe, il est fortement recommandé de développer les protocoles de tests en parallèle de la mise à jour du code. Chaque phase de la migration doit être validée par des tests automatisés, idéalement exécutés sous ThreadSanitizer (TSAN) et AddressSanitizer (ASAN).

### Phase 1 : Extraction mécanique du contexte
- **Analyse des symboles globaux :** Vérifier via les outils binaires (`nm` ou `readelf`) qu'aucune des variables globales mutables historiques (`msg_recv`, `allClients`, etc.) ne subsiste dans la section `.bss`, à l'exception notable du pointeur de compatibilité `default_ctx`.
- **Initialisation paresseuse legacy :** Démarrer un programme qui utilise seulement `IvyContextCreate()` et vérifier que `default_ctx` reste nul. Appeler ensuite une API legacy et vérifier qu'elle crée le contexte par défaut une seule fois.
- **Validation Legacy :** Faire passer l'intégralité de la suite de tests legacy existante. Elle doit utiliser l'API de façade et réussir à 100%.

### Phase 2 : Statut public et cycle de vie
- **Séquence nominale :** Appeler `IvyInit()`, vérifier que l'état passe à `CREATED`. Appeler `IvyStart()`, vérifier le passage à `RUNNING`.
- **Sémantique post-arrêt :** Appeler `IvyStop()` (état `STOPPED`), puis vérifier que les appels ultérieurs à `IvySendMsg()` échouent immédiatement en renvoyant `IVY_ESTOPPED` ou via `IvyGetLastError()`.
- **Compatibilité ABI :** Vérifier que les applications C legacy ignorant le code de retour compilent toujours sans avertissement bloquant.

### Phase 3 : Verrous de base et owner thread
- **Anti-Data Race (TSAN) :** Lancer la boucle dans un thread, et forcer l'appel concurrent intensif à `IvySendMsg()` depuis 3 worker threads. TSAN ne doit signaler aucune course aux données sur les buffers scratch, les bindings, les listes de clients, les FIFO ou les sockets.
- **Parallélisme de matching :** Instrumenter temporairement `IvyBindingExec()` ou utiliser des regexps coûteuses pour vérifier que deux threads appelant `IvySendMsg()` peuvent matcher simultanément sous `bindings_rwlock` en lecture.
- **Sérialisation d'envoi :** Envoyer depuis plusieurs threads vers le même client et vérifier que chaque trame Ivy conserve l'ordre `id + payload` sans mélange d'octets.
- **Réentrance :** S'abonner à un message. Dans son callback de réception, appeler à nouveau `IvySendMsg()`. L'appel ne doit ni deadlocker ni exécuter un callback utilisateur sous `bindings_rwlock` ou `client->send_lock`.
- **Vérification du Owner Thread :** Ajouter un `assert(pthread_self() == owner_thread)` (ou équivalent) avant tout callback applicatif pour garantir que l'exécution réseau n'échappe jamais à la boucle principale.

### Phase 4 : Wakeup et file de contrôle
- **Réveil asynchrone :** Plonger la boucle dans un `select()` sans aucun trafic réseau. Appeler `IvyStop()` depuis un thread secondaire. Prouver (en mesurant le temps de réaction) que la boucle est débloquée instantanément par le canal de réveil (pipe ou socketpair).
- **File de contrôle :** Générer une congestion d'écriture depuis un worker thread, et valider que l'événement (`IVY_CTL_ADD_WRITABLE_WATCH`) est correctement posté dans la file, puis dépilé par le thread de la boucle sans erreur.

### Phase 5 : Sécurité des callbacks
- **Invalidation d'itérateur :** Dans le callback d'une expression régulière, appeler `IvyUnbindMsg()` sur une autre regexp de la liste. Valider que la boucle interne d'itération (dans `ClientCall`) ne segfault pas, validant ainsi la stratégie des snapshots.
- **Auto-destruction :** Appeler `IvyStop()` ou `IvyContextStop()` directement depuis une callback de réception de message. Vérifier que la boucle procède à un arrêt différé propre, sans deadlock avec le mutex courant.

Statut : couvert par `tests/run_phase5.sh`, qui lance deux processus Ivy sur un
bus réel. Le callback de réception du receiver désabonne une autre regexp puis
appelle `IvyStop()`, tandis que le sender appelle `IvySendMsg()` et `IvyStop()`
depuis son callback applicatif de connexion.

### Phase 6 : Contextualiser loop, sockets et timers
- **Étanchéité Multi-Bus :** Instancier deux contextes (A et B) sur deux bus/ports distincts dans le même processus. Émettre un message sur le bus B et garantir par une assertion stricte que les clients abonnés du bus A ne reçoivent aucun callback croisé.
- **Indépendance des cycles :** Créer un timer sur le bus A. Stopper et détruire complètement le bus B. Le timer du bus A doit continuer à s'exécuter normalement, sans interférence.

### Phase 7 : Nettoyage de l'API publique
- **Queries sans buffers partagés :** Appeler les nouvelles APIs de query depuis
  plusieurs threads et vérifier que les résultats restent stables sans
  écrasement croisé.
- **Contrat des handles existants :** Vérifier que la documentation et les tests
  couvrent les usages réels : handle reçu dans une callback, query pendant que
  le contexte est vivant, et échec propre après arrêt.
- **Tests Mémoire ciblés :** Créer, démarrer, stopper et détruire plusieurs
  contextes dans les tests existants. Réserver ASAN/Valgrind à une passe de
  validation dédiée, pas à une expansion systématique de l'API.

### Phase 8 : Outils et timers multi-bus
- **Timer ivyprobe multi-bus :** Lancer `ivyprobe -t` sur deux bus et vérifier
  que le flux timer applicatif unique est diffusé vers tous les bus.
- **Arrêt propre des timers :** Quitter `ivyprobe` pendant qu'un timer est armé
  et vérifier que tous les contextes s'arrêtent sans callback tardif, fuite de
  timer ou accès à un contexte détruit.

### Phase 9 : Portabilité et backends alternatifs
- **Wakeup Windows :** Déclencher `IvyContextStop()` depuis un worker thread
  pendant que la boucle Windows est bloquée dans `select()`. L'arrêt doit
  réveiller la boucle sans attendre de trafic réseau.
- **Backends toolkit :** Pour chaque backend compilable, poster un changement
  writable depuis un worker thread et vérifier que l'opération effective est
  exécutée par le thread propriétaire du backend.

Statut : le chemin `select` dispose du wakeup POSIX et Windows. Le test
automatisé phase 9 est exécuté sur la plateforme courante ; une passe Windows
native reste nécessaire pour valider Winsock en conditions réelles. Les
backends toolkit restent hors périmètre MT-safe et ne doivent pas être présentés
comme exemples multibus.

### Phase 10 : Outils et exemples restants
- **Smoke tests outils :** Pour chaque outil porté, lancer l'outil sur un bus
  local, vérifier son comportement nominal minimal puis l'arrêter proprement.
- **Exemples MT-safe :** Vérifier par grep ou test de compilation que les outils
  choisis comme exemples modernes n'utilisent plus les wrappers legacy de bus.

## Compatibilité

Certains comportements legacy sont difficiles à rendre parfaitement sûrs sans
nouvelles API :

- `IvyGetApplicationList()` retourne un buffer statique ;
- `IvyGetApplicationMessages()` retourne un tableau statique ;
- `IvyClientPtr` est un pointeur interne brut ;
- `NULL` est déjà un résultat métier possible pour certaines fonctions de
  recherche.

Pour les fonctions historiques actuellement `void`, les passer à `int` est le
meilleur compromis : les anciens appels qui ignorent le retour restent valides,
les nouveaux appels peuvent tester `IVY_OK` ou une erreur négative.

Pour les fonctions à retour pointeur, `NULL` doit rester le signal d'échec ou
d'absence. `IvyGetLastError()` thread-local permet de distinguer :

- absence normale : `IvyGetApplication()` ne trouve pas l'application ;
- arrêt du bus : l'API retourne `NULL` et `IvyGetLastError()` vaut
  `IVY_ESTOPPED` ;
- appel invalide : l'API retourne `NULL` et `IvyGetLastError()` vaut
  `IVY_EINVAL` ou `IVY_ESTATE`.

Pour les buffers statiques historiques, le verrouillage ne suffit pas toujours :
un `bindings_rwlock` en lecture autorise plusieurs threads dans `IvySendMsg()`.
Les buffers scratch du chemin d'envoi doivent donc devenir locaux, thread-local
ou portés par le contexte. Les nouvelles API contextuelles devront proposer des
buffers fournis par l'appelant ou des objets résultat à libération explicite.

## Questions ouvertes

- Chaque contexte doit-il posséder son propre thread de boucle, ou les
  applications doivent-elles continuer à appeler explicitement
  `IvyContextMainLoop(ctx)` ?
- Les callbacks doivent-elles rester sur le thread de boucle ou passer par une
  file/un thread de dispatch séparé ?
- Un contexte arrêté doit-il pouvoir redémarrer, ou est-il single-use ?
- Les filtres de regexps doivent-ils être uniquement contextuels ?
- Faut-il conserver le parallélisme OpenMP regexp pendant la migration, ou le
  désactiver jusqu'à stabilisation du modèle contextuel ?

## Principe directeur

L'invariant de conception devrait être :

> Toute API Ivy peut être appelée depuis n'importe quel thread. L'état Ivy
> interne est protégé par des verrous aux responsabilités distinctes : cycle de
> vie du contexte, graphe des bindings et écriture socket. Les callbacks
> utilisateur et les opérations toolkit/event-loop sont exécutées uniquement
> dans le thread propriétaire de la boucle. L'envoi normal reste direct ; seuls
> les événements de contrôle passent par une file de réveil.

Cet invariant limite la taille du premier patch, évite le coût d'une file de
messages pour chaque `IvySendMsg()`, et donne à `stop` un protocole clair :
passer en `STOPPING`, réveiller la boucle, libérer dans le bon thread, marquer
`STOPPED`, puis réveiller les threads en attente. Les autres threads découvrent
l'arrêt de façon asynchrone lors de leur prochain appel Ivy, par une valeur de
retour négative ou un pointeur `NULL` accompagné de `IvyGetLastError()`.
