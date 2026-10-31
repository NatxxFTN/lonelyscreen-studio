<#
.SYNOPSIS
    Crée la clé de signature des versions publiées (ECDSA P-256).
.DESCRIPTION
    - Clé PRIVÉE : écrite hors du dépôt, dans $KeyPath. Ne la publie jamais ;
      sauvegarde-la (gestionnaire de mots de passe, clé USB). La perdre empêche
      de publier des mises à jour acceptées par les installations existantes.
    - Clé PUBLIQUE : écrite dans loader\release_key.h (intégrée au moteur) et
      dans install.ps1 (vérifiée par l'installateur). Elle peut être publiée.
    Refuse d'écraser une clé existante sauf avec -Force.
.EXAMPLE
    pwsh tools\new-signing-key.ps1 -Repo NatxxFTN/lonelyscreen-studio
#>
param(
    [Parameter(Mandatory)] [string]$Repo,
    [string]$KeyPath = (Join-Path $env:USERPROFILE '.lonelyscreen-studio\release-signing-key.json'),
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent

if ((Test-Path $KeyPath) -and -not $Force) {
    throw "Une clé existe déjà : $KeyPath. Utilise -Force seulement si tu sais pourquoi (les installations existantes refuseront les versions signées avec la nouvelle clé)."
}
if ($Repo -notmatch '^[A-Za-z0-9-]+/[A-Za-z0-9._-]+$') {
    throw "Dépôt invalide : '$Repo' (attendu : compte/dépôt)."
}

$ecdsa = [Security.Cryptography.ECDsa]::Create([Security.Cryptography.ECCurve+NamedCurves]::nistP256)
$parameters = $ecdsa.ExportParameters($true)

New-Item -ItemType Directory -Force (Split-Path $KeyPath -Parent) | Out-Null
[ordered]@{
    curve = 'P-256'
    d = [Convert]::ToBase64String($parameters.D)
    x = [Convert]::ToBase64String($parameters.Q.X)
    y = [Convert]::ToBase64String($parameters.Q.Y)
} | ConvertTo-Json | Set-Content -LiteralPath $KeyPath -Encoding utf8

function Format-Bytes([byte[]]$Bytes) {
    ($Bytes | ForEach-Object { '0x{0:X2}' -f $_ }) -join ', '
}

$header = @"
/**
 * @file release_key.h
 * Clé PUBLIQUE de signature des versions (ECDSA P-256) et dépôt de publication.
 * Générée par tools/new-signing-key.ps1 — ne pas modifier à la main.
 */
#pragma once

#define LS_STUDIO_REPO "$Repo"

static const unsigned char kReleasePublicKeyX[32] = {$(Format-Bytes $parameters.Q.X)};
static const unsigned char kReleasePublicKeyY[32] = {$(Format-Bytes $parameters.Q.Y)};
"@
Set-Content -LiteralPath (Join-Path $root 'loader\release_key.h') -Value $header -Encoding utf8

$installer = Join-Path $root 'install.ps1'
$content = Get-Content -LiteralPath $installer -Raw
$content = $content -replace "(?m)^\`$PublicKeyX = '.*'$", "`$PublicKeyX = '$([Convert]::ToBase64String($parameters.Q.X))'"
$content = $content -replace "(?m)^\`$PublicKeyY = '.*'$", "`$PublicKeyY = '$([Convert]::ToBase64String($parameters.Q.Y))'"
$content = $content -replace "(?m)^\`$Repository = '.*'$", "`$Repository = '$Repo'"
# Sans BOM : avec "iwr | iex", un BOM casserait le bloc de commentaire initial.
Set-Content -LiteralPath $installer -Value $content -Encoding utf8NoBOM -NoNewline

Write-Host "Clé privée : $KeyPath  (à sauvegarder, à ne jamais publier)"
Write-Host 'Clé publique écrite dans loader\release_key.h et install.ps1.'
