# LonelyScreen Studio

Une couche de personnalisation pour LonelyScreen, dans l'esprit de Spicetify pour Spotify :
- une boutique de **thèmes**, d'**effets vidéo** et d'**apps** ;
- une bibliothèque de ce qui est installé ;
- des réglages en direct, dans un panneau collé à la fenêtre de LonelyScreen.

> Module communautaire, **non affilié à IMTIGER Technologies** (éditeur de LonelyScreen). Il ne contient aucun code de LonelyScreen et ne contourne pas sa licence.

## Installer

Dans PowerShell (Windows 10/11, LonelyScreen déjà installé) :

```powershell
iwr -useb https://raw.githubusercontent.com/NatxxFTN/lonelyscreen-studio/main/install.ps1 | iex
```

Windows demande une seule fois les droits administrateur, pour poser un petit fichier à côté de LonelyScreen. Lance ensuite LonelyScreen : un onglet apparaît au bord de sa fenêtre. **Ctrl+Maj+P** ouvre aussi le Studio.

**Mettre à jour :** Studio > Réglages > Mises à jour. Le Studio vérifie aussi tout seul à chaque ouverture. Pas besoin de droits administrateur.

**Désinstaller :**
```powershell
& ([scriptblock]::Create((iwr -useb https://raw.githubusercontent.com/NatxxFTN/lonelyscreen-studio/main/install.ps1))) -Uninstall
```
Ajoute `-Purge` pour effacer aussi les réglages.

> Les effets n'agissent que sur la vidéo pendant une recopie : sans appareil connecté, LonelyScreen n'affiche aucune vidéo à modifier.

## Architecture

```
C:\Program Files (x86)\LonelyScreen\
 └─ d3d9.dll          RELAIS (stub/) : minimal, posé une fois avec droits admin

%LOCALAPPDATA%\LonelyScreenStudio\
 ├─ current.txt       version active
 └─ versions\<v>\     MOTEUR mis à jour sans droits admin
     ├─ lsstudio.dll  hooks Direct3D 9, Studio (WebView2), boutique, mises à jour
     ├─ plugins\      effets livrés (+ manifestes .json)
     └─ studio\       interface HTML/CSS/JS + catalogue officiel

%APPDATA%\LonelyScreenPlugins\   réglages .ini, studio.json, thèmes, apps,
                                  plugins de la boutique, journal lsplugins.log
```

- **Le relais** (`stub/stub.cpp`) intercepte `CreateWindowExW` et `TrackPopupMenuEx` dans l'exe. À la création de la fenêtre principale, il charge le moteur de la version active et lui délègue tout. Sans moteur valide, il relaie simplement le d3d9 du système : LonelyScreen fonctionne alors normalement.
- **Le moteur** (`loader/`) se compose de :
  - `loader.cpp` : interceptions Direct3D 9 et plugins ;
  - `studio_host.cpp` : onglet, panneau collé, pont JSON ;
  - `store.cpp` : la boutique ;
  - `updater.cpp` : mises à jour signées ;
  - `util.cpp` : fichiers, HTTPS, SHA-256.

## Sécurité des mises à jour

Chaque version publie un `latest.json`, qui liste les fichiers avec leur SHA-256, et un `latest.json.sig`, sa signature ECDSA P-256. La clé publique est intégrée au moteur (`loader/release_key.h`) et à `install.ps1`. Une version est refusée si sa signature ne correspond pas, même si elle vient du bon dépôt GitHub. Un compte GitHub compromis ne suffit donc pas à pousser du code chez les utilisateurs : il faut aussi la clé privée, qui reste sur ton PC.

## Publier une version

Prérequis : Visual Studio 2022 avec les outils C++, PowerShell 7 (`pwsh`), la CLI GitHub (`gh auth login`).

1. **Une seule fois** :
   ```powershell
   pwsh tools\new-signing-key.ps1 -Repo NatxxFTN/lonelyscreen-studio
   ```
   La clé privée est écrite dans `%USERPROFILE%\.lonelyscreen-studio\`. **Sauvegarde-la**, ne la publie jamais. Si tu la perds, les installations existantes refuseront tes futures versions.
2. **À chaque version** :
   ```powershell
   pwsh tools\release.ps1 -Version 1.1.0 -Notes "Ce qui change"            # prépare dist\v1.1.0
   pwsh tools\release.ps1 -Version 1.1.0 -Notes "Ce qui change" -Publish   # + release GitHub
   ```
   Les utilisateurs la reçoivent depuis le Studio. Si tu modifies le relais (`stub/`), augmente `LS_STUB_VERSION` et passe `-MinStub` : le Studio demandera alors de relancer la commande d'installation.
3. **Tester avant de publier** : `dist\v<version>` est aussi un flux local.
   ```powershell
   powershell -File install.ps1 -Feed .\dist\v1.1.0          # installateur
   $env:LS_STUDIO_FEED = "$PWD\dist\v1.1.0"                   # mises à jour du Studio
   ```

## Ajouter du contenu

Rien dans le Studio n'est à modifier.

| Type | Fichiers | Où |
|---|---|---|
| Thème | `<id>.json` : `cover`, `accent`, `settings` (valeurs .ini à écrire) | `studio/catalog/themes/` |
| App | `index.html` + `app.json` | `studio/catalog/apps/<id>/` |
| Effet (plugin) | `<id>.dll` (ABI `include/lsplugin.h`) + `<id>.json` (manifeste) + `<id>.ini` | `plugins/<id>/` |

- **Manifeste d'un plugin.** Le champ `settings.fields` décrit les contrôles : `toggle`, `slider`, `color` (avec `alpha`), `choice`. Le Studio génère la page de réglages à partir de ce champ.
- **Apps.** Une app modifie des réglages avec `parent.postMessage({ type: 'ls:writeIni', requestId, file, section, values }, '*')`.
- **Catalogue communautaire.** Un `catalog.json` publié ailleurs peut être ajouté dans Réglages > Catalogue en ligne. Chaque fichier doit avoir son SHA-256 ; `tools/build-catalog.ps1` montre le format.

## Compiler

```bat
build.bat
```
La compilation se fait en x86, parce que LonelyScreen est en 32 bits. La version vient du fichier `VERSION`.

## Limites connues

- Les effets portent sur la vidéo rendue par Direct3D 9, pas sur l'interface Qt de LonelyScreen.
- Le relais et le moteur ne sont pas signés numériquement (Authenticode). Un antivirus peut se méfier d'une DLL `d3d9.dll` posée à côté d'un programme, car c'est la technique du proxy DLL. Un certificat de signature de code réduira ces alertes.
