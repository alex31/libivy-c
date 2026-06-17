# Demo Qt + Ivy

Ce projet cree une petite app Qt qui sert aussi d'exemple MTSafe :

- le thread Qt envoie `qtdemo ON` / `qtdemo OFF` avec un `QRadioButton`,
- la boucle Ivy tourne dans un `std::thread` dedie,
- un pool fixe de workers non-Qt appelle `IvyContextSendMsg()` sur le meme
  `IvyContext`,
- l'application s'abonne a `^qtdemo msg (.*)` et affiche `xxx` quand un message
  `qtdemo msg xxx` arrive.

Le bouton worker envoie des messages du type :

```text
qtdemo worker 2 thread 140012345678912 seq 4
```

Les workers restent vivants jusqu'a la fermeture de la fenetre. Leurs ids de
thread restent donc distincts et visibles dans `ivyprobe`, ce qui rend la
demonstration MTSafe plus lisible qu'un cycle `create/send/join` ou le systeme
peut recycler immediatement le meme thread id.

## Build

Prerequis :

- Qt 6 Widgets
- `pkg-config`
- une installation de libivy qui fournit `libivy.pc`

```bash
cmake -S . -B build
cmake --build build
```

Le binaire est `build/qtdemo`.

## Lancement

```bash
./build/qtdemo [bus]
```

`bus` est optionnel. Si l'argument est absent, la demo laisse Ivy utiliser
`IVYBUS` si defini, sinon le bus par defaut de la librairie.

## Vérification avec `ivyprobe`

Terminal 1 (démo) :

```bash
./build/qtdemo 127:2010
```

Terminal 2 (espionnage) :

```bash
QT_QPA_PLATFORM=offscreen /usr/local/bin/ivyprobe -b 127:2010 'qtdemo msg (.*)'
```

Puis dans l'interface d'`ivyprobe`, envoyer des messages vers la demo Qt :

```
qtdemo msg bonjour
qtdemo msg test123
```

Le champ texte de la fenêtre Qt doit afficher `bonjour`, `test123`, etc.

Dans la fenetre Qt :

- basculer le bouton radio emet `qtdemo ON` / `qtdemo OFF` depuis le thread Qt,
- cliquer sur le bouton worker emet depuis un worker non-Qt ; `ivyprobe` affiche
  le worker, le thread id et le numero de sequence.
