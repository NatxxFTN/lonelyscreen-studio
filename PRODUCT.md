# Product

<!-- impeccable:product-schema 1 -->

## Platform

web

(Interface HTML/CSS/JS affichée dans WebView2, à l'intérieur du processus Windows de LonelyScreen.)

## Stack

delegated : HTML/CSS/JS vanilla sans étape de build, servi depuis un dossier local via le mapping d'hôte virtuel de WebView2. Le pont natif est en C++ dans le chargeur `d3d9.dll`. L'utilisateur a demandé « l'exemple que tu me recommandais ».

## Users

L'utilisateur et son collègue, qui recopient l'écran d'un iPhone ou d'un iPad sur un PC Windows avec LonelyScreen. Ils veulent personnaliser l'appli au maximum, à la manière de Spicetify pour Spotify. *(Inféré du brief :)* public technophile et curieux, qui ajoute et teste des extensions au fil du temps.

## Product Purpose

LonelyScreen Studio ajoute à LonelyScreen une couche d'extension complète :
- une boutique pour découvrir et installer des plugins (effets vidéo), des thèmes (préréglages visuels) et des apps (pages ajoutées au Studio) ;
- la gestion de ce qui est installé et les réglages de chaque plugin, appliqués en direct.

Le Studio est réussi quand on peut transformer l'apparence de la recopie d'écran et ajouter des fonctions sans toucher à l'exe ni au code.

## Positioning

C'est la seule couche de personnalisation de LonelyScreen. Elle s'accroche à l'appli d'origine sans la modifier, par un proxy `d3d9.dll`, et chaque réglage s'applique sur la vidéo en direct.

## Operating Context

- Le Studio est un panneau collé au bord de la fenêtre LonelyScreen, qui la suit quand on la déplace. On l'ouvre par un onglet sur le bord de la fenêtre, par le menu système (« Plugins… ») ou par un raccourci clavier.
- Les effets ne sont visibles que pendant une recopie d'écran active : sans appareil connecté, le moteur vidéo ne rend rien.
- Les installations se font sans droits administrateur, dans `%APPDATA%\LonelyScreenPlugins`.
- Un nouveau plugin n'est actif qu'après un redémarrage de LonelyScreen. Les réglages et les thèmes, eux, s'appliquent en direct.

## Capabilities and Constraints

- Les plugins sont des DLL natives x86 avec l'ABI `lsplugin.h`. Chacun décrit ses réglages dans un manifeste `plugin.json`, et l'interface s'en sert pour générer ses contrôles automatiquement.
- Un thème est un JSON qui fixe des valeurs de réglages de plugins, plus éventuellement une couleur d'accent pour le Studio.
- Une app est un dossier contenant un `index.html`, affiché comme une page du Studio.
- Le catalogue de la boutique est un JSON : soit le catalogue local fourni, soit une URL configurable (dépôt GitHub). Chaque fichier téléchargé est vérifié par son empreinte SHA-256 avant d'être installé.
- **Hors périmètre :** contourner la licence ou les rappels d'achat de LonelyScreen (pas d'« AdBlock »).
- **Ouvert :** le dépôt GitHub du catalogue en ligne n'existe pas encore.

## Brand Commitments

- Référence explicite de l'utilisateur : Spotify / Spicetify. Le Studio doit donner la même impression qu'une boutique d'extensions de ce niveau, « magnifique », « professionnel ».
- Interface en français.

## Evidence on Hand

- Trois plugins réels : `background`, `quality` et `frame`.
- Aucun avis, note, nombre de téléchargements ou auteur tiers réel n'existe. L'interface ne doit pas en inventer : tout contenu de démonstration est marqué comme tel.

## Product Principles

1. Ce qu'on règle se voit tout de suite : tout changement s'applique en direct et le dit.
2. Extensible par des données : un nouveau plugin, thème ou app n'exige aucune modification du Studio.
3. Ne jamais casser LonelyScreen : chaque installation est réversible, et chaque fichier téléchargé est vérifié avant d'être chargé.
4. Être honnête sur l'état réel des choses : « redémarrage requis », « effet visible pendant la recopie », installé ou non.
