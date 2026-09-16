# Moniteur Qt6 + Ivy C++23

L’application utilise `ivy::Bus`, les tokens d’abonnement et `ivy::LoopThread`.
La boucle Ivy tourne dans son thread natif ; les valeurs reçues sont copiées
avant publication dans la file d’événements Qt. Les envois depuis l’interface
restent des appels directs à l’API MT-safe.

## Messages reçus

L’abonnement du journal est exactement `(.*)`, enregistré par `bind_raw_unanchored()` pour
respecter le choix explicite des regexps non ancrées dans le wrapper C++.
Le journal conserve tous les messages reçus pendant la session, y compris les
messages ready et les messages directs, avec les colonnes :

| Colonne | Contenu |
| --- | --- |
| Adresse | Adresse IP numérique IPv4/IPv6 de l’émetteur, sans résolution DNS |
| Port | Port d’écoute TCP annoncé par l’application Ivy ; distinct du port UDP du bus |
| Application | Nom annoncé par l’émetteur |
| Réception | Date et heure locales, avec millisecondes, prises dans le callback Ivy avant la file Qt |
| Type | Message ordinaire ou identifiant du message direct |
| Message | Texte complet reçu, sans retirer de préfixe |

Les données de pair proviennent de `Bus::application_info()`, qui retourne une
copie possédée `ApplicationInfo { name, address, port }`. Les anciennes lignes
restent lisibles après la déconnexion de leur émetteur. La vue défile avec les
nouveaux messages si l’utilisateur est déjà en bas du journal. `Ctrl+C` copie
les lignes sélectionnées avec leurs colonnes ; l’infobulle donne le texte entier.

## Réception typée avec `bind_convert`

Le panneau **Réception typée — bind_convert** affiche en permanence la regexp
d’abonnement et les quatre types attendus, dans l’ordre :

```text
^QT_CONVERT (\S+) (\S+) (\S+) (\S+)$
QT_CONVERT <long> <double> <string_view> <bool>
```

Le bouton **Voir l'abonnement**, à droite de la regexp, ouvre `window.cpp`
dans une fenêtre en lecture seule, centrée sur l'appel `bus_->bind_convert`
et avec sa ligne surlignée. Le fichier source doit rester à l'emplacement de
compilation ; la ligne est retrouvée à l'ouverture, même après ajout de code.

La callback reçoit `ivy::ConvertStatus`, puis `long`, `double`,
`std::string_view` et `bool`. La regexp accepte quatre champs textuels non vides,
séparés par un seul espace. Le champ `string_view` ne contient donc pas d’espace.
Les contraintes numériques viennent de `bind_convert`, ce qui permet de tester
les erreurs de conversion avec la même regexp.

Le panneau montre le dernier résultat :

- `OK` et les quatre valeurs converties ;
- `CONVERT_ERROR` ou `COUNT_ERROR`, avec le diagnostic de
  `Bus::conversion_error()` copié pendant la callback ;
- `REGEXP_ERROR` pour un message commençant par `QT_CONVERT` dont la forme
  ne correspond pas à la regexp, par exemple s’il manque un champ.

Ce dernier cas est détecté par l’abonnement brut du journal : Ivy n’appelle pas
une callback de regexp pour un message qui ne correspond pas. Avec cette regexp
fixe à quatre captures, un champ manquant produit donc `REGEXP_ERROR`, et non
`COUNT_ERROR`. Les autres classes de messages restent dans le journal et ne
remplacent pas le résultat du panneau. Les erreurs n’arrêtent pas le bus ; un
message valide peut être envoyé immédiatement après.

Les valeurs et les diagnostics sont copiés en `QString` avant leur passage au
thread graphique. L’interface affiche du texte brut et permet de le copier.

## Agents et pings

Une seconde table affiche les agents connectés, même ceux qui n’envoient aucun
message applicatif, avec leur IP, port TCP, nom, dernier délai de ping en
millisecondes et état. Le cœur Ivy répond automatiquement aux pings reçus.

Le moniteur envoie un ping Ivy à chaque agent lors de sa connexion, puis environ
toutes les deux secondes. Les délais mesurent l’aller-retour du protocole Ivy.
Un seul ping est en attente par connexion. Après trois secondes sans réponse,
l’état passe à « Sans réponse » et une nouvelle tentative est planifiée deux
secondes plus tard. Les réponses périmées sont ignorées.

Le suivi utilise les connexions, et non les noms, qui peuvent être identiques.
Les pings, réponses et suppressions de pairs sont traités dans le thread Ivy ;
Qt reçoit uniquement des copies et un identifiant local de connexion. Aucun
`IvyClientPtr` emprunté n’est placé dans la file Qt. Une déconnexion retire la
ligne d’agent, sans supprimer l’historique des messages.

## Envoyer un message

Saisir le texte dans le champ supérieur puis cliquer sur **Send**, ou appuyer
sur **Entrée**. UTF-8, espaces, pourcentages et accolades sont transmis tels quels.
Le champ est vidé après acceptation locale ; il reste rempli en cas d’erreur.
Le bouton est désactivé lorsque le champ est vide ou que le bus est arrêté.

Le statut affiche les acceptations locales, éventuellement zéro lorsqu’aucun
abonnement distant ne correspond. Ce résultat ne constitue pas un accusé de
réception du pair. Le journal est celui des messages reçus des autres agents.

Les contrôles de démonstration restent disponibles : ON/OFF depuis le thread
graphique et envoi depuis un pool Qt limité à quatre threads réutilisables.
Les messages workers sont de la forme `qtdemo worker thread 140012345678912 seq 4`.
Tous ces envois utilisent le même Bus, sans file de commandes vers le thread Ivy.

## Cycle de vie

La fermeture normale appelle `request_stop()` et garde Qt actif jusqu’aux
notifications de fin du thread Ivy et du pool, puis joint les producteurs.
Le destructeur assure aussi un arrêt/join de secours en cas de destruction
directe. Les tokens et le Bus restent vivants jusqu’à la fin du dispatch.

`main.cpp` contient le démarrage ; `window.hpp` / `window.cpp` contiennent les
widgets, le modèle du journal, le suivi des agents et les signaux. Le helper de
thread est dans `src/cpp/ivy_thread.hpp`, sans dépendance Qt. L’exemple n’appelle
pas l’API C ni `native_handle()`.

## Construction

Prérequis :

- Qt >= 6.4 Widgets ; Qt Test pour les tests optionnels ;
- CMake >= 3.20 et pkg-config ;
- compilateur/bibliothèque standard C++23 avec `expected`, `format` et
  `move_only_function` (GCC 13 convient sur la machine de validation) ;
- installation C et C++ de ce même arbre Ivy 3.18, avec `ivy-cpp.pc` et
  `Ivy/ivy_thread.hpp`.

Depuis la racine du dépôt, après installation des bibliothèques correspondantes :

```sh
cmake -S examples/ivyqt -B /tmp/ivyqt-build
cmake --build /tmp/ivyqt-build
/tmp/ivyqt-build/qtdemo 127:2010
```

Avec un préfixe d’installation personnel, ajouter son `lib/pkgconfig` à
`PKG_CONFIG_PATH` et rendre ses bibliothèques partagées accessibles au chargeur
(`LD_LIBRARY_PATH` sur Linux, si nécessaire).

L’argument du bus est facultatif. Son absence conserve la sélection `IVYBUS`,
puis l’adresse par défaut, effectuée par `Bus::start()`.

## Vérification manuelle

Dans un autre terminal :

```sh
ivyprobe -b 127:2010 '(.*)'
```

Écrire un message dans le champ Send pour le voir dans ivyprobe. Envoyer
ensuite depuis ivyprobe des messages quelconques :

```text
HELLO bonjour
AUTRE_CLASSE été 100%
```

Chaque message doit apparaître entier dans le journal avec son émetteur et
son horodatage. Le délai de ping d’ivyprobe apparaît dans la table des agents.
Une demande Ivy `Die` provoque une fermeture coordonnée de la fenêtre.

Pour tester le panneau typé, saisir ces lignes dans ivyprobe, sur le même bus :

```text
QT_CONVERT -42 125.5 avion vrai
QT_CONVERT +7 -2.5e1 été -000
QT_CONVERT 9 1.5 avion -2
QT_CONVERT mauvais 125.5 avion vrai
QT_CONVERT -42 erreur avion vrai
QT_CONVERT -42 125.5 avion inconnu
QT_CONVERT -42 125.5 avion
```

Les trois premières lignes donnent `OK` (booléens `true`, `false`, `true`).
Les trois suivantes donnent `CONVERT_ERROR` pour l’entier, le double puis le
booléen. La dernière donne `REGEXP_ERROR`. La regexp et deux exemples sont aussi
affichés dans l’interface, pour pouvoir tester sans ouvrir ce fichier.

Pour les booléens, tout entier nul vaut `false`, tout entier non nul vaut
`true`. Sinon, une valeur commençant par `f/F` vaut `false`, par `t/T/v/V` vaut
`true` ; les autres valeurs sont invalides. Les nombres décimaux utilisent un
point, et aucun espace n’est retiré des captures.

## Tests hors écran

Depuis la racine du dépôt :

```sh
./tests/cpp/run_qt.sh
```

Ce script construit et installe Ivy dans un préfixe temporaire, compile l’exemple
avec CMake et lance Qt Test via le plugin `offscreen`. Il nécessite les sockets
locales pour les échanges Ivy. Il couvre l’abonnement `(.*)`, l’historique,
les colonnes et l’horodatage, la copie du texte, les messages directs, Send/Entrée,
UTF-8 et espaces, les envois GUI/workers, les pings dans les deux sens, deux agents
homonymes dont un silencieux, les pings périodiques, l’absence de réponse puis
la reprise, les déconnexions et les scénarios de fermeture précédents.
Il vérifie aussi l’abonnement typé annoncé, les quatre valeurs affichées,
les erreurs numériques/booléennes, les messages hors format, la reprise après
erreur et la conservation littérale du texte UTF-8 contenant des pourcentages.

Les tests propres à `LoopThread` sont également inclus dans `tests/cpp/run.sh`
et `tests/cpp/run_glib.sh` : captures mobiles, arrêt sans attente pendant un
callback actif, déplacement, arrêt/join RAII, propagation des erreurs et
conservation des erreurs de callback du Bus. La suite native injecte aussi un
échec de création de thread.
