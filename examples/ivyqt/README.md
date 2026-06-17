# Démo Qt + Ivy

Ce projet crée une petite app Qt qui :

- envoie sur le bus Ivy : `qtdemo ON` ou `qtdemo OFF` selon l'état d'un
  `QRadioButton`,
- s'abonne à `^qtdemo msg (.*)`,
- affiche la partie `xxx` dans un champ texte de l'interface quand un message
  `qtdemo msg xxx` arrive.

## Build

```bash
cmake -S . -B build
cmake --build build
```

Le binaire est `build/qtdemo`.

## Lancement

```bash
./build/qtdemo [bus]
```

`bus` est optionnel (par défaut il utilise `IVYBUS` si défini, sinon `127:2010`).

## Vérification avec `ivyprobe`

Terminal 1 (démo) :

```bash
./build/qtdemo 127:2010
```

Terminal 2 (espionnage) :

```bash
QT_QPA_PLATFORM=offscreen /usr/local/bin/ivyprobe -b 127:2010 'qtdemo msg (.*)'
```

Puis dans l'interface d'`ivyprobe`, envoyer des messages :

```
qtdemo msg bonjour
qtdemo msg test123
```

Le champ texte de la fenêtre Qt doit afficher `bonjour`, `test123`, etc.

Dans la fenêtre Qt, bascule le bouton radio pour émettre `qtdemo ON` / `qtdemo OFF`.
