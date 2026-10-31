<#
.SYNOPSIS
    Génère studio\catalog\catalog.json à partir des thèmes, apps et plugins du dépôt.
.DESCRIPTION
    Chaque fichier installable reçoit son empreinte SHA-256, vérifiée par le
    chargeur avant installation. Le même format sert pour un catalogue en
    ligne : il suffit de publier catalog.json et les fichiers à côté (les
    chemins "src" relatifs sont résolus par rapport à l'URL du catalogue).
#>
param(
    [string]$Root = (Split-Path $PSScriptRoot -Parent)
)

$ErrorActionPreference = 'Stop'
$catalogDir = Join-Path $Root 'studio\catalog'

function Get-Sha256([string]$Path) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $bytes = $sha.ComputeHash([IO.File]::ReadAllBytes($Path))
        return -join ($bytes | ForEach-Object { $_.ToString('x2') })
    } finally {
        $sha.Dispose()
    }
}

function Read-Json([string]$Path) {
    Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json
}

function New-Item-Entry($Manifest, [string]$Type, $Files, [bool]$Builtin) {
    $entry = [ordered]@{
        type        = $Type
        id          = $Manifest.id
        name        = $Manifest.name
        version     = $Manifest.version
        author      = $Manifest.author
        summary     = $Manifest.summary
        description = $Manifest.description
        cover       = $Manifest.cover
    }
    if ($Manifest.PSObject.Properties.Name -contains 'accent') { $entry.accent = $Manifest.accent }
    if ($Manifest.PSObject.Properties.Name -contains 'settings') { $entry.settings = $Manifest.settings }
    if ($Builtin) { $entry.builtin = $true }
    $entry.files = @($Files)
    return $entry
}

$items = @()

# Plugins livrés avec le Studio : affichés comme "Inclus", rien à télécharger.
foreach ($manifestPath in Get-ChildItem (Join-Path $Root 'plugins') -Recurse -Filter '*.json') {
    $manifest = Read-Json $manifestPath.FullName
    $items += New-Item-Entry $manifest 'plugin' @() $true
}

foreach ($themePath in Get-ChildItem (Join-Path $catalogDir 'themes') -Filter '*.json') {
    $manifest = Read-Json $themePath.FullName
    $file = [ordered]@{ src = "themes/$($themePath.Name)"; path = $themePath.Name; sha256 = Get-Sha256 $themePath.FullName }
    $items += New-Item-Entry $manifest 'theme' @($file) $false
}

foreach ($appDir in Get-ChildItem (Join-Path $catalogDir 'apps') -Directory) {
    $manifest = Read-Json (Join-Path $appDir.FullName 'app.json')
    $files = foreach ($file in Get-ChildItem $appDir.FullName -File -Recurse) {
        $relative = $file.FullName.Substring($appDir.FullName.Length + 1).Replace('\', '/')
        [ordered]@{ src = "apps/$($appDir.Name)/$relative"; path = $relative; sha256 = Get-Sha256 $file.FullName }
    }
    $items += New-Item-Entry $manifest 'app' $files $false
}

$catalog = [ordered]@{
    name    = 'Catalogue officiel'
    version = 1
    items   = $items
}
$json = $catalog | ConvertTo-Json -Depth 8
[IO.File]::WriteAllText((Join-Path $catalogDir 'catalog.json'), $json, [Text.UTF8Encoding]::new($false))
Write-Host "catalog.json : $($items.Count) éléments"
