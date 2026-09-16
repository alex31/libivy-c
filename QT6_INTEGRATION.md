# Qt6 : choix d’intégration pour Ivy

Mise à jour du 15 septembre 2026 : l’exemple Qt6 est maintenant modernisé avec
l’API C++ et remplace entièrement sa version C. Le backend QtCore décrit plus
bas reste une piste facultative ; l’exemple livré utilise la boucle native Ivy.

## Recommandation retenue : envois directs, notifications mises en file

Choix précisé par l’utilisateur le 15 septembre : conserver `bus.run()` dans
un thread Ivy dédié, envoyer les messages directement depuis le thread Qt avec
`bus.send()` / `bus.send_report()`, et utiliser une seule file interthread pour
les notifications Ivy vers l’interface : la file d’événements Qt existante.

| Sens | Mécanisme retenu |
| --- | --- |
| Qt vers Ivy | Appel direct à l’API d’envoi MT-safe ; résultat consultable dans le thread Qt |
| Ivy vers Qt | Données copiées, puis signal/slot avec `Qt::QueuedConnection` ou invocation mise en file |

Le code d’envoi s’exécute dans le thread appelant, avec les verrous du cœur Ivy ;
la réception est traitée par le thread de boucle Ivy. La sûreté multithread ne
transforme pas l’envoi en tâche exécutée par le thread Ivy. L’exemple
`examples/ivyqt/window.cpp` utilise cette organisation avec l’API C++.

Le lot livré comprend la passerelle par signaux Qt, les abonnements C++ et le
helper générique `ivy::LoopThread` dans `src/cpp/ivy_thread.hpp`. Le helper
emprunte un Bus déjà démarré et garde son adresse stable. Sa factory convertit
les échecs de création de thread en `std::expected`. `request_stop()` demande
l’arrêt sans attendre la fin du dispatch ; `join()` rapporte l’erreur du pilote
ou du callback de fin. Les erreurs des callbacks Ivy restent consultables avec
`Bus::take_callback_error()`. Le C fournit `IvyContextRequestStop()` pour cette
coordination. Aucun en-tête Qt n’est requis par le helper.

La fermeture normale désactive les envois et continue à traiter les événements
Qt jusqu’aux notifications de fin du thread Ivy et du pool de démonstration.
Elle joint ensuite les producteurs et autorise la fermeture. La destruction
directe garde un arrêt/join de secours synchrone.
 `Bus::post()` et une file
de commandes Qt vers Ivy ne sont pas des prérequis pour cet usage. Les captures
reçues doivent être copiées avant publication à Qt ; le bus doit rester vivant
pendant les appels, et l’arrêt/join doit précéder sa destruction.

Le backend QtCore étudié plus bas reste une possibilité future. Son intérêt
serait l’intégration native aux sources Qt, au-delà du besoin actuel satisfait
par la boucle Ivy dédiée. Cette étude est un choix d’architecture, pas le résultat
d’un benchmark.

## Moniteur de messages et de pings livré

L’exemple souscrit à `(.*)` avec `bind_raw_unanchored()` et reçoit également les
messages directs. Son journal conserve le texte complet, l’adresse numérique,
le port TCP annoncé, le nom du pair et le timestamp pris avant la mise en file
Qt. `Bus::application_info()` ajoute le snapshot C++ possédé correspondant,
avec un pont privé C sous verrou et sans résolution DNS.

Un panneau supplémentaire utilise `bind_convert` sur
`^QT_CONVERT (\S+) (\S+) (\S+) (\S+)$`, avec les captures `long`, `double`,
`std::string_view` et `bool`. La regexp, les types et des exemples pour ivyprobe
restent visibles. Le panneau affiche les valeurs ou le diagnostic de conversion,
copié avant la mise en file Qt. Les messages `QT_CONVERT` mal formés sont signalés
par l’abonnement brut, car une regexp non satisfaite ne déclenche pas sa callback.

Le champ Send transmet le texte UTF-8 depuis le thread Qt (bouton ou Entrée).
Le suivi des pings reste dans le thread Ivy : un registre par connexion est
alimenté par les callbacks connect/disconnect et un timer Ivy programme les
mesures. Les noms homonymes ne confondent pas les agents. La GUI n’obtient ni
pointeur de pair ni vue empruntée. Le cœur répond déjà aux pings du protocole ;
la table des agents affiche les RTT, l’attente ou l’absence de réponse.

Les tests Qt couvrent les deux sens de ping, les agents silencieux/homonymes,
la périodicité, les timeouts et la reprise, les colonnes du journal, Send/Entrée,
le texte UTF-8, les messages directs, la copie et la fermeture. Ils couvrent aussi
les quatre conversions, les erreurs de type ou de format et la reprise après
erreur. Voir le
[README de l’exemple](examples/ivyqt/README.md) pour les détails d’utilisation.

## Le point déterminant dans le cœur actuel

Dans [SocketConnectAddrFor](src/ivysocket.c), `connect()` est appelé avant
l’activation d’`O_NONBLOCK` / `FIONBIO`. Ce chemin est utilisé depuis
`BroadcastReceive()` dans `src/ivy.c`. Un pair lent ou inaccessible peut donc
retenir le thread de dispatch pendant la connexion.

`SocketGetRemoteHost()` et `SocketGetPeerHost()` utilisent aussi `getnameinfo()`
sans `NI_NUMERICHOST`. Le premier intervient notamment à l’acceptation d’un pair ;
le second dans les requêtes d’informations d’application. Ces résolutions peuvent
attendre. Les traiter avant de recommander le dispatch Ivy dans le thread GUI :
connexion asynchrone avec suivi d’écriture et deadline, puis noms numériques ou
résolution asynchrone avec cache selon le contrat souhaité.

Un callback applicatif court ne suffit donc pas à garantir aujourd’hui une GUI
réactive. Cette propriété du cœur doit guider le choix du thread pour tous les
backends.

## Comparaison

| Organisation | Intérêt | Travail / limite |
| --- | --- | --- |
| `bus.run()` natif dans un thread | Disponible, isole les attentes réseau de la GUI | Copie des notifications, arrêt/join à coordonner |
| Backend QtCore dans la GUI | Une boucle, callbacks près des widgets | Connexion/résolution à rendre asynchrones, dispatch à borner |
| Backend QtCore dans un `QThread` | Intégration Qt et isolation de la GUI | Pont de notifications et cycle de vie explicite |

Qt permet une boucle d’événements par thread. L’affinité d’un `QObject` détermine
le thread qui reçoit ses événements ; ses enfants suivent la même affinité.
Créer le bus et ses objets Qt dans leur thread propriétaire simplifierait le
contrat. Déplacer le wrapper C++ ne devrait jamais signifier déplacer un bus Qt
actif vers un autre thread. [Documentation QObject](https://doc.qt.io/qt-6/qobject.html).

## Architecture proposée pour le backend QtCore

1. **Canaux Ivy → `QSocketNotifier`.** Conserver les sockets et le protocole du
   cœur. Surveiller lecture et écriture avec des notifiers distincts ; activer
   l’écriture seulement lorsqu’une FIFO doit être vidée. Retirer les notifiers
   avant fermeture/réutilisation du descripteur. Le type `Exception` ne remplace
   pas la gestion des erreurs par le transport.
   [Documentation QSocketNotifier](https://doc.qt.io/qt-6/qsocketnotifier.html).
2. **Timers Ivy → scheduling Qt.** Réutiliser les tokens et contrats `after` /
   `every`, avec un déclenchement Qt et un calcul d’échéance monotone. Choisir
   `Qt::PreciseTimer` si le contrat interdit une expiration anticipée. `QTimer`
   doit être démarré/arrêté dans son thread ; gérer explicitement les durées qui
   dépassent son intervalle entier, et conserver le compteur Ivy de callbacks.
   [Documentation QTimer](https://doc.qt.io/qt-6/qtimer.html).
3. **Commandes de contrôle → événements Qt.** Sérialiser les mutations de
   notifiers/timers sur leur propriétaire. Une publication Qt réveille ce thread.
   Conserver le chemin d’envoi Ivy protégé par ses verrous pour les appels déjà
   thread-safe ; ne pas imposer une allocation Qt à chaque broadcast ordinaire.
   Les commandes applicatives qui demandent un traitement asynchrone pourraient
   passer par une façade Qt séparée.
4. **API commune et façade optionnelle.** Garder `ivy::Bus` comme propriétaire C++
   ordinaire. Un adaptateur `QObject` pourrait le posséder et exposer des signaux
   à Qt, sans rendre `Bus` lui-même héritier de `QObject`. Fixer une seule chaîne
   de propriété pour éviter une destruction à la fois par un parent Qt et par
   RAII. La signature exacte de la factory reste à concevoir avec ce contrat.
5. **Arrêt limité au bus.** Désactiver ses sources, terminer ou annuler les
   commandes en attente et libérer les captures après le dernier dispatch.
   Arrêter un bus ne doit appeler ni `QCoreApplication::quit()` ni le `quit()`
   d’un thread partagé. Pour un thread dédié, l’application coordonne ensuite
   fin de boucle, join et destruction ; une attente potentiellement longue doit
   rester hors du chemin de traitement de la GUI.
6. **Distribution optionnelle.** Une variante `ivy-cpp-qt6` est une piste cohérente
   avec le choix GLib. Elle nécessiterait un véritable backend de canaux/timers
   exportant les primitives attendues par le cœur. QtCore suffirait pour cette
   couche. Les symboles C partagés entre backends imposent encore un seul backend
   Ivy par processus ; charger deux variantes ne constitue pas une sélection par bus.

## Communication entre threads

Transmettre des valeurs possédées, par exemple `QString`, `QByteArray` ou des
structures copiées, via une connexion mise en file ou une invocation associée
à un objet destinataire. Ne pas transporter les `string_view` / `span` empruntés
aux callbacks Ivy. Un `IvyClientPtr` brut ne devient pas durable en le plaçant
sur la file Qt ; les commandes différées visant un pair nécessiteraient une
identité stable et une résolution dans le thread Ivy.

Les connexions mises en file exécutent le traitement dans le thread du
récepteur, lorsque sa boucle reprend la main. Les connexions bloquantes entre
threads compliqueraient inutilement l’arrêt d’Ivy.
[Documentation threads et QObjects](https://doc.qt.io/qt-6/threads-qobject.html).

Ne pas mettre `bus.run()` dans un slot de worker tout en attendant que ce même
thread traite ses slots Qt : l’appel bloquant occuperait sa boucle. Le modèle
natif dédié communique vers Qt ; le futur backend Qt laisse `QThread::exec()`
traiter les événements du worker. L’objet `QThread` lui-même vit généralement
dans le thread qui l’a créé, pas dans le thread qu’il lance.
[Documentation QThread](https://doc.qt.io/qt-6/qthread.html).

## Validation du lot Qt6

`tests/cpp/run_qt.sh` compile l’exemple avec CMake/C++23 contre une installation
temporaire d’Ivy et le vérifie hors écran avec Qt 6.4.2. Les tests exercent de
vrais échanges réseau : ON/OFF depuis la GUI, messages du pool de workers,
réception UTF-8, erreur de démarrage, fermeture avec notifications/travaux en
attente, demande `Die` et destruction directe de la fenêtre.

`tests/cpp/thread_test.cpp` vérifie le helper en statique/partagé avec les
backends natif et GLib : callback long pendant une demande d’arrêt sans attente,
captures mobiles, erreurs de callbacks, déplacement, refus du join sur soi-même
et arrêt/join de secours. La suite native injecte aussi un échec de création de
thread (`pthread_create` / `EAGAIN`) et vérifie sa conversion en résultat.

Le backend QtCore resterait un lot séparé, à mesurer face aux attentes longues
du cœur avant de recommander son exécution sur le thread graphique. Aucune
interrogation périodique d’`IvyContextIdle()` n’est ajoutée.
