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
int IvyInit(...)              { return IvyDefaultContextInit(...); }
int IvyStart(const char *bus) { return IvyContextStart(default_ctx, bus); }
int IvyStop(void)             { return IvyContextStop(default_ctx); }
MsgRcvPtr IvyBindMsg(...)     { return IvyContextBindMsg(default_ctx, ...); }
```

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
- l'état interne Ivy est sérialisé par un verrou de contexte, initialement un
  verrou global récursif pour limiter la taille du patch ;
- `bind`, `unbind`, `change`, `send`, `direct send`, `ping` et `stop` prennent
  ce verrou, vérifient l'état du bus, puis effectuent l'opération ou retournent
  une erreur ;
- les callbacks utilisateur sont toujours appelés dans le thread propriétaire
  de la boucle ;
- les opérations qui manipulent la toolkit ou l'event loop sont exécutées dans
  le thread propriétaire de la boucle ;
- l'envoi normal reste direct sous verrou pour éviter une allocation et une
  copie de chaîne par message.

Cette approche ne cherche pas le parallélisme maximal. Elle garantit surtout
qu'aucun état Ivy mutable n'est modifié concurremment, que les octets envoyés
sur une socket ne sont pas mélangés entre threads, et que les callbacks restent
compatibles avec les toolkits graphiques non thread-safe.

Le chemin chaud `IvySendMsg()` ne doit donc pas poster systématiquement une
commande contenant le message. En régime nominal :

1. le thread appelant prend le verrou Ivy ;
2. le message est formaté dans un buffer protégé par ce verrou ;
3. les regexps distantes sont parcourues ;
4. les écritures socket sont faites en série ;
5. la fonction retourne le nombre de destinataires, ou une erreur négative.

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

- `ctx->mutex` : protège l'état du contexte, les listes/hash Ivy, les buffers
  statiques historiques utilisés comme scratch, la file de contrôle, la
  condition de shutdown et les FIFO d'envoi ;
- verrou récursif dans une première implémentation : permet à une callback Ivy
  de rappeler l'API sans deadlock immédiat, le temps de retirer progressivement
  les callbacks des zones verrouillées ;
- verrou d'envoi par client, optionnel et seulement comme optimisation
  ultérieure : le verrou global suffit à sérialiser les octets dans la première
  version ;
- compteur de callbacks en cours, optionnel : permet à stop/destroy d'attendre
  que les callbacks soient revenues.

À éviter :

- tenir `ctx->mutex` pendant une I/O socket potentiellement bloquante ;
- tenir `ctx->mutex` pendant un callback utilisateur ;
- appeler une API GLib/Xt/Tcl/GLUT depuis un worker thread ;
- poster tous les messages Ivy ordinaires dans une file intermédiaire ;
- appeler `IvyContextJoin()` depuis le thread de boucle.

## Plan de migration

### Phase 1 : statut public et cycle de vie

- Ajouter `IvyStatus` et `IvyGetLastError()`.
- Changer les fonctions publiques `void` en `int` quand elles représentent une
  opération Ivy.
- Réserver les retours négatifs aux erreurs Ivy.
- Introduire l'état `CREATED/RUNNING/STOPPING/STOPPED`.
- Faire retourner `IVY_ESTOPPED` ou `NULL` aux appels faits après `STOPPING`.
- Garder l'API historique comme façade sur l'état global actuel.

Cette phase donne déjà aux threads un moyen simple et peu coûteux de découvrir
qu'un autre thread a stoppé Ivy.

### Phase 2 : verrou global récursif et owner thread

- Ajouter un verrou global récursif pour protéger l'état Ivy existant.
- Enregistrer le thread propriétaire de la loop.
- Prendre le verrou dans les API publiques et dans les callbacks channel.
- Garantir que les callbacks utilisateur restent appelés par le thread de loop.
- Vérifier que les callbacks peuvent rappeler l'API Ivy sans deadlock.

### Phase 3 : wakeup et file de contrôle

- Ajouter le canal de réveil à la boucle.
- Ajouter une file basse fréquence pour les événements de contrôle.
- Router par cette file les callbacks différées, les changements de watch
  toolkit et le stop demandé depuis un worker thread.
- Ne pas router les messages Ivy ordinaires dans cette file.
- Rendre `IvyStop()` synchrone : il réveille la loop, attend `STOPPED`, puis
  retourne.

### Phase 4 : sécurité des callbacks

- Retirer les callbacks utilisateur des sections de mutation/verrouillage
  interne.
- Ajouter des snapshots d'événements callback si nécessaire.
- Ajouter un compteur de callbacks en cours ou un mécanisme équivalent de garde
  de durée de vie.
- Définir explicitement le comportement de `stop` et `unbind` appelés depuis
  une callback.

### Phase 5 : extraction mécanique du contexte

- Ajouter `IvyContext` comme structure interne.
- Déplacer l'état global de `src/ivy.c` dans ce contexte.
- Garder un `default_ctx` global.
- Convertir les fonctions internes pour recevoir `IvyContext *ctx`.
- Conserver le comportement mono-bus via l'API historique.

Cette phase peut arriver après le durcissement MT-safe du bus par défaut. Elle
prépare le vrai multi-bus sans imposer tout le changement dans le même patch.

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

### Phase 1 : Statut public et cycle de vie
- **Séquence nominale :** Appeler `IvyInit()`, vérifier que l'état passe à `CREATED`. Appeler `IvyStart()`, vérifier le passage à `RUNNING`.
- **Sémantique post-arrêt :** Appeler `IvyStop()` (état `STOPPED`), puis vérifier que les appels ultérieurs à `IvySendMsg()` échouent immédiatement en renvoyant `IVY_ESTOPPED` ou via `IvyGetLastError()`.
- **Compatibilité ABI :** Vérifier que les applications C legacy ignorant le code de retour compilent toujours sans avertissement bloquant.

### Phase 2 : Verrou global récursif et owner thread
- **Anti-Data Race (TSAN) :** Lancer la boucle dans un thread, et forcer l'appel concurrent intensif à `IvySendMsg()` depuis 3 worker threads. TSAN ne doit signaler aucune course aux données sur les buffers statiques.
- **Réentrance :** S'abonner à un message. Dans son callback de réception, appeler à nouveau `IvySendMsg()`. Le verrou récursif doit permettre l'opération sans deadlock.
- **Vérification du Owner Thread :** Ajouter un `assert(pthread_self() == owner_thread)` (ou équivalent) avant tout callback applicatif pour garantir que l'exécution réseau n'échappe jamais à la boucle principale.

### Phase 3 : Wakeup et file de contrôle
- **Réveil asynchrone :** Plonger la boucle dans un `select()` sans aucun trafic réseau. Appeler `IvyStop()` depuis un thread secondaire. Prouver (en mesurant le temps de réaction) que la boucle est débloquée instantanément par le canal de réveil (pipe ou socketpair).
- **File de contrôle :** Générer une congestion d'écriture depuis un worker thread, et valider que l'événement (`IVY_CTL_ADD_WRITABLE_WATCH`) est correctement posté dans la file, puis dépilé par le thread de la boucle sans erreur.

### Phase 4 : Sécurité des callbacks
- **Invalidation d'itérateur :** Dans le callback d'une expression régulière, appeler `IvyUnbindMsg()` sur une autre regexp de la liste. Valider que la boucle interne d'itération (dans `ClientCall`) ne segfault pas, validant ainsi la stratégie des snapshots.
- **Auto-destruction :** Appeler `IvyStop()` ou `IvyContextStop()` directement depuis une callback de réception de message. Vérifier que la boucle procède à un arrêt différé propre, sans deadlock avec le mutex courant.

### Phase 5 : Extraction mécanique du contexte
- **Analyse des symboles globaux :** Vérifier via les outils binaires (`nm` ou `readelf`) qu'aucune des variables globales mutables historiques (`msg_recv`, `allClients`, etc.) ne subsiste dans la section `.bss`, à l'exception notable du pointeur de compatibilité `default_ctx`.
- **Validation Legacy :** Faire passer l'intégralité de la suite de tests legacy existante. Elle doit utiliser l'API de façade et réussir à 100%.

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

Pour les buffers statiques historiques, le verrou global les rend utilisables
dans une première version MT-safe, mais seulement jusqu'au prochain appel Ivy
dans le même processus. Les nouvelles API contextuelles devront proposer des
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
> interne est sérialisé par un verrou de contexte. Les callbacks utilisateur et
> les opérations toolkit/event-loop sont exécutées uniquement dans le thread
> propriétaire de la boucle. L'envoi normal reste direct sous verrou ; seuls les
> événements de contrôle passent par une file de réveil.

Cet invariant limite la taille du premier patch, évite le coût d'une file de
messages pour chaque `IvySendMsg()`, et donne à `stop` un protocole clair :
passer en `STOPPING`, réveiller la boucle, libérer dans le bon thread, marquer
`STOPPED`, puis réveiller les threads en attente. Les autres threads découvrent
l'arrêt de façon asynchrone lors de leur prochain appel Ivy, par une valeur de
retour négative ou un pointeur `NULL` accompagné de `IvyGetLastError()`.
