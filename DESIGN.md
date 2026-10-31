# DESIGN — LonelyScreen Studio

Ce document décrit le système visuel du Studio tel qu'il est construit, avec les fichiers `studio/styles.css`, `studio/js/covers.js` et `studio/js/icons.js` comme référence. L'interface ressemble à une bibliothèque de streaming sombre. Chaque extension a sa pochette, qui montre un téléphone en recopie et l'effet appliqué sur son écran.

## Scène

Le Studio sert sur un PC, à côté de la fenêtre LonelyScreen, souvent pendant une recopie, une démo ou une présentation. Le fond sombre laisse toute la place aux pochettes et à la vidéo recopiée.

## Couleurs

| Rôle | Jeton | Valeur |
|---|---|---|
| Fond | `--ground` | `#0E0F13` |
| Rail | `--rail` | `#08090B` |
| Surfaces | `--raised` / `--raised-2` / `--raised-3` | `#16181D` / `#1E2027` / `#272A33` |
| Filets | `--line` / `--line-strong` | `#262932` / `#353945` |
| Texte | `--ink` / `--ink-2` / `--ink-3` | `#F2F3F5` / `#B6BAC5` / `#8C919E` |
| Accent (action, sélection) | `--accent` | `#8B6CFF` par défaut ; suit le thème actif ou un choix de l'utilisateur |
| Texte sur l'accent | `--accent-ink` | calculé selon la luminance : `#0E0F13` ou `#FFFFFF` |
| États | `--ok` / `--warn` / `--danger` | `#3DDC97` / `#FFB547` / `#FF5D6C` |
| Héros | `--hero` | couleur principale de la pochette mélangée à 38 % avec le fond (`color-mix` en oklab) |

Stratégie retenue : une interface sobre, plus un accent, plus des couleurs qui occupent des zones entières. La couleur vit dans les pochettes et dans le bandeau du héros, teinté par le thème actif. L'accent ne sert qu'aux actions, à la sélection et aux interrupteurs actifs.

## Typographie

- Police : Figtree (licence OFL), servie depuis `studio/fonts/` (latin et latin étendu, graisses 400 à 800 en une seule police variable).
- Échelle :
  - texte courant 14 px, ligne de 1,45 ;
  - méta 12,5 px ;
  - titre de section 19 px en graisse 700 ;
  - titre de page 28 px en graisse 750, interlettrage −0,025 em ;
  - titre du héros de 28 à 46 px, interlettrage −0,035 em.
- Chiffres tabulaires pour les valeurs des curseurs et les compteurs.

## Formes et profondeur

- Rayons : 6 px pour les petits éléments, 10 px pour les cartes et pochettes, 14 px pour les tuiles et les blocs. Pilules (999 px) pour les boutons, filtres et interrupteurs.
- Ombres : seules les pochettes en vedette (héros, détail) en portent une, `0 18px 40px -12px rgb(0 0 0 / .6)`. Les notifications ont une ombre décalée et adoucie.
- Icônes : SVG dessinés sur une grille de 24, trait de 1,75, bouts arrondis (`js/icons.js`). Ni emoji ni glyphe Unicode à la place d'une icône.

## Pochettes (signature)

`coverSvg(item, preview)` dessine, de façon déterministe selon l'identifiant :
- un fond et un motif (`orbit`, `waves`, `grid`, `sharpen`, `frame`, `field`, `dial`, `swatches`) tirés de `cover.colors` (trois couleurs) ;
- un téléphone incliné dont l'écran affiche une composition aux mêmes couleurs.

L'argument `preview` applique les réglages en direct : bordure et teinte du plugin Cadre, couleur du plugin Fond, filtre saturation/contraste/luminosité du plugin Qualité. La pochette du héros combine les trois.

## Composition

- Coquille : rail de 212 px (68 px, icônes seules, sous 720 px de large), barre du haut avec retour, recherche et fermeture, puis la zone de contenu défilante.
- Rangées : défilement horizontal avec accroche, tuiles de 152 px. La tuile qui a le focus (clic, survol prolongé de 320 ms ou clavier) s'élargit de 312 px et révèle ses badges, sa description et son action. Ses voisines passent à 55 % d'opacité.
- Détail d'un plugin : en-tête teinté avec la pochette en aperçu direct, puis les réglages générés depuis le manifeste (interrupteur, curseur, couleur avec opacité, choix segmenté).

## Mouvement

- Une seule courbe : `cubic-bezier(.16, 1, .3, 1)`, de 140 ms (survol) à 280 ms (structure).
- Moment signature : l'ouverture d'une tuile. Les changements de page passent par l'API View Transitions, et la pochette garde le même nom de transition entre la tuile et le détail.
- `prefers-reduced-motion` ramène toutes les durées à 1 ms.

## Surfaces du navigateur

Toutes reprennent la palette : sélection de texte à la couleur d'accent, anneaux de focus de 2 px à la couleur d'accent, barres de défilement fines (`--raised-3`), curseurs dont le remplissage suit l'accent.

## Textes

Tout est en français, et l'interface dit l'état réel des choses :
- « Redémarrage requis » pour un plugin qui n'est pas encore chargé ;
- « Aucune recopie en cours… » quand aucun appareil n'est connecté ;
- des erreurs qui nomment la cause et la solution.

Aucune note, aucun nombre de téléchargements, aucun auteur tiers inventé.
