# Ivy C : réentrance et sûreté multi-thread

Cette note rassemble les constats et une direction de conception pour rendre
`ivy-c` capable de gérer plusieurs bus Ivy dans un même processus, puis de
tolérer des appels API depuis plusieurs threads applicatifs sur un même bus.

Le point essentiel : ce chantier doit être traité comme une migration
d'architecture, pas comme l'ajout ponctuel de quelques mutex autour de l'état
global existant.

## Situation actuelle

L'implémentation historique suppose implicitement :

- un seul bus Ivy par processus ;
- une seule boucle d'événements propriétaire ;
- des callbacks utilisateur appelés directement depuis le dispatch réseau ;
- des pointeurs internes exposés comme handles publics.

Les principaux états globaux mutables sont aujourd'hui dans :

- `src/ivy.c` : identité applicative, callbacks, sockets TCP/UDP, binds locaux,
  clients connectés, dictionnaires de regexps, ready message, mode IPv4/IPv6 ;
- `src/ivyloop.c` : liste de channels, `fd_set`, `highestFd`, `MainLoop`,
  hooks avant/après `select()` ;
- `src/ivysocket.c` : listes globales de sockets serveur et client ;
- `src/timer.c` : liste globale des timers et timeout courant de `select()` ;
- `src/ivybind.c` : table globale de filtrage des regexps et regexp
  d'extraction de token.

Plusieurs fonctions utilisent aussi des buffers `static` pour éviter des
allocations répétées. C'est pratique dans une boucle mono-thread historique,
mais incompatible avec des appels réentrants. Certaines zones ont un traitement
OpenMP `threadprivate`, mais cela ne couvre qu'un chemin étroit
regexp/envoi et ne rend pas l'API du bus réentrante.

`IvyStop()` appelle actuellement `IvyChannelStop()`, qui pose seulement un
flag global de boucle à faux. Cela ne réveille pas forcément un thread bloqué
dans `select()`, ne cible pas un bus précis, ne définit pas le protocole
d'arrêt pour les autres threads et ne clarifie pas la durée de vie des
ressources.

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

## Modèle public proposé

Introduire un contexte opaque :

```c
typedef struct IvyContext IvyContext;
```

Ajouter une API contextuelle :

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
int IvyContextJoin(IvyContext *ctx);
int IvyContextDestroy(IvyContext *ctx);

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
int IvyStart(const char *bus) { return IvyContextStart(IvyGetDefaultContext(), bus); }
int IvyStop(void)             { return IvyContextStop(IvyGetDefaultContext()); }
MsgRcvPtr IvyBindMsg(...)     { return IvyContextBindMsg(IvyGetDefaultContext(), ...); }
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

La majorité des globals mutables doivent migrer dans `struct IvyContext`.

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
- flags de debug actuellement globaux ;
- état de shutdown ;
- mutex et variables de condition ;
- file de contrôle ;
- canal de réveil de l'event loop.

L'état de boucle devrait aussi être contextuel, probablement dans une structure
embarquée :

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

Les sockets doivent être associées à la boucle ou au contexte qui les possède.
Les timers doivent également être contextuels, ou au minimum attachés à la
boucle, car une liste globale de timers empêche deux bus indépendants d'avoir
des échéances et des attentes indépendantes.

La table de filtrage des regexps dans `ivybind.c` demande une décision :

- la garder globale seulement pour la compatibilité legacy ;
- préférer une table par contexte pour la nouvelle API ;
- si un défaut global subsiste, documenter qu'il ne concerne que le contexte
  par défaut.

## Modèle de threads

La première étape doit rester simple : rendre un bus existant sûr depuis
plusieurs threads, sans imposer que tous les messages transitent par une file.

Le modèle validé est hybride :

- chaque bus ou futur `IvyContext` a une boucle propriétaire ;
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
- les callbacks utilisateur sont toujours appelés dans le thread propriétaire
  de la boucle ;
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
idéalement être compatible socket.

Sans ce mécanisme, un `IvyContextStop()` peut rester bloqué jusqu'à l'arrivée
d'un trafic réseau ou d'un timeout de timer.

Les backends GLib, Xt, Tcl et GLUT doivent suivre la même règle : un thread
applicatif peut demander une opération, mais l'ajout/retrait effectif d'une
watch toolkit se fait dans le thread propriétaire de la boucle. Cela concerne
en particulier `IvyChannelAddWritableEvent()` et
`IvyChannelClearWritableEvent()` quand un envoi depuis un worker thread fait
entrer ou sortir une socket de congestion.

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

- `IvyContextStart()` est valide depuis `CREATED`, et éventuellement depuis
  `STOPPED` si le redémarrage est supporté ;
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
- `IvyContextDestroy()` est valide seulement après `STOPPED`, ou appelle
  lui-même stop puis join.

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

## Durée de vie des objets

`IvyClientPtr` et `MsgRcvPtr` sont aujourd'hui des pointeurs bruts vers des
structures internes. C'est fragile si un autre thread peut arrêter le bus.

Compatibilité court terme :

- conserver `IvyClientPtr` et `MsgRcvPtr` comme handles opaques bruts ;
- documenter qu'ils sont valides seulement pendant la callback, ou jusqu'à
  unbind explicite selon le contrat existant ;
- garantir que stop attend la fin des callbacks en cours avant de libérer les
  structures qu'elles référencent.

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
- appeler `IvyContextJoin()` depuis le thread de boucle.

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

### Phase 4 : wakeup et file de contrôle

- Ajouter le canal de réveil à la boucle.
- Ajouter une file basse fréquence pour les événements de contrôle.
- Router par cette file les callbacks différées, les changements de watch
  toolkit et le stop demandé depuis un worker thread.
- Ne pas router les messages Ivy ordinaires dans cette file.
- Rendre `IvyStop()` synchrone : il réveille la loop, attend `STOPPED`, puis
  retourne.

### Phase 5 : sécurité des callbacks

- Retirer les callbacks utilisateur des sections de mutation/verrouillage
  interne.
- Ajouter des snapshots d'événements callback si nécessaire.
- Ajouter un compteur de callbacks en cours ou un mécanisme équivalent de garde
  de durée de vie.
- Définir explicitement le comportement de `stop` et `unbind` appelés depuis
  une callback.

### Phase 6 : contextualiser loop, sockets et timers

- Introduire un état de channels par contexte ou par boucle.
- Faire enregistrer les sockets sur leur boucle propriétaire.
- Sortir `servers_list` et `clients_list` des globals de `ivysocket.c`.
- Sortir l'état timer des globals de `timer.c`.
- Vérifier que deux contextes ne partagent plus leurs channels ni leurs timers.

### Phase 7 : nettoyage de l'API publique

- Ajouter des fonctions contextuelles de query sans buffers statiques.
- Ajouter des codes d'erreur explicites.
- Documenter les anciennes API comme wrappers sur le contexte par défaut.
- Documenter précisément quelles fonctions sont thread-safe et quelles limites
  restent liées aux signatures legacy.

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

### Phase 6 : Contextualiser loop, sockets et timers
- **Étanchéité Multi-Bus :** Instancier deux contextes (A et B) sur deux bus/ports distincts dans le même processus. Émettre un message sur le bus B et garantir par une assertion stricte que les clients abonnés du bus A ne reçoivent aucun callback croisé.
- **Indépendance des cycles :** Créer un timer sur le bus A. Stopper et détruire complètement le bus B. Le timer du bus A doit continuer à s'exécuter normalement, sans interférence.

### Phase 7 : Nettoyage de l'API publique
- **Tests Mémoire (ASAN/Valgrind) :** Créer, démarrer, stopper et détruire de multiples contextes en boucle. Valider par Valgrind (ou équivalent) l'absence absolue de fuite mémoire ou de descripteurs de fichiers non fermés (0 bytes leaked).

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
- Quel primitif de réveil Windows utiliser avec le `select()` actuel ?
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
