<#
.SYNOPSIS
    Prépare (et publie avec -Publish) une version de LonelyScreen Studio.
.DESCRIPTION
    1. Écrit VERSION, compile tout (build.bat).
    2. Rassemble les fichiers dans dist\v<version>\ sous des noms plats
       (studio/js/app.js -> studio--js--app.js), adaptés aux pièces jointes GitHub.
    3. Écrit latest.json (chemins, URL, SHA-256) et le signe avec la clé privée
       (latest.json.sig). Ce dossier est aussi un flux local pour tester :
         install.ps1 -Feed dist\v<version>   ou   $env:LS_STUDIO_FEED = '...\dist\v<version>'
    4. Avec -Publish : crée la release GitHub v<version> via la CLI gh.
.EXAMPLE
    pwsh tools\release.ps1 -Version 1.1.0 -Notes "Nouveau thème, corrections"
    pwsh tools\release.ps1 -Version 1.1.0 -Notes "…" -Publish
#>
param(
    [Parameter(Mandatory)] [ValidatePattern('^\d+\.\d+\.\d+$')] [string]$Version,
    [string]$Notes = '',
    [int]$MinStub = 1,
    [string]$KeyPath = (Join-Path $env:USERPROFILE '.lonelyscreen-studio\release-signing-key.json'),
    [switch]$Publish
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (-not (Test-Path $KeyPath)) { throw "Clé privée introuvable : $KeyPath (lance tools\new-signing-key.ps1 une fois)." }
$repoLine = Select-String -Path (Join-Path $root 'loader\release_key.h') -Pattern '#define LS_STUDIO_REPO "([^"]+)"'
if (-not $repoLine) { throw 'loader\release_key.h ne définit pas LS_STUDIO_REPO.' }
$repo = $repoLine.Matches[0].Groups[1].Value

Set-Content -LiteralPath (Join-Path $root 'VERSION') -Value $Version -NoNewline -Encoding ascii
& cmd /c (Join-Path $root 'build.bat')
if ($LASTEXITCODE -ne 0) { throw 'La compilation a échoué.' }

$build = Join-Path $root 'build'
$dist = Join-Path $root "dist\v$Version"
if (Test-Path $dist) { Remove-Item $dist -Recurse -Force }
New-Item -ItemType Directory -Force $dist | Out-Null

$sources = @(
    @{ Path = 'stub/d3d9.dll'; File = Join-Path $build 'd3d9.dll' },
    @{ Path = 'lsstudio.dll'; File = Join-Path $build 'lsstudio.dll' }
)
foreach ($folder in 'plugins', 'studio') {
    $base = Join-Path $build $folder
    foreach ($file in Get-ChildItem $base -Recurse -File) {
        $relative = $file.FullName.Substring($base.Length + 1).Replace('\', '/')
        $sources += @{ Path = "$folder/$relative"; File = $file.FullName }
    }
}

$files = foreach ($source in $sources) {
    $asset = $source.Path.Replace('/', '--')
    Copy-Item -LiteralPath $source.File -Destination (Join-Path $dist $asset)
    [ordered]@{
        path   = $source.Path
        asset  = $asset
        url    = "https://github.com/$repo/releases/download/v$Version/$asset"
        sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $source.File).Hash.ToLowerInvariant()
        size   = (Get-Item -LiteralPath $source.File).Length
    }
}

$manifest = [ordered]@{
    version   = $Version
    minStub   = $MinStub
    notes     = $Notes
    published = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    files     = @($files)
}
$manifestBytes = [Text.UTF8Encoding]::new($false).GetBytes(($manifest | ConvertTo-Json -Depth 5))
[IO.File]::WriteAllBytes((Join-Path $dist 'latest.json'), $manifestBytes)

$key = Get-Content -LiteralPath $KeyPath -Raw | ConvertFrom-Json
$parameters = [Security.Cryptography.ECParameters]@{
    Curve = [Security.Cryptography.ECCurve+NamedCurves]::nistP256
    D     = [Convert]::FromBase64String($key.d)
    Q     = [Security.Cryptography.ECPoint]@{ X = [Convert]::FromBase64String($key.x); Y = [Convert]::FromBase64String($key.y) }
}
$ecdsa = [Security.Cryptography.ECDsa]::Create($parameters)
$signature = $ecdsa.SignData($manifestBytes, [Security.Cryptography.HashAlgorithmName]::SHA256)
$ecdsa.Dispose()
[IO.File]::WriteAllText((Join-Path $dist 'latest.json.sig'), [Convert]::ToBase64String($signature))

Write-Host "Version $Version prête : $dist ($($files.Count) fichiers, manifeste signé)."

if ($Publish) {
    if (-not (Get-Command gh -ErrorAction SilentlyContinue)) { throw 'La CLI GitHub (gh) est requise pour publier.' }
    $assets = Get-ChildItem $dist -File | ForEach-Object FullName
    & gh release create "v$Version" @assets --repo $repo --title "LonelyScreen Studio $Version" --notes ($Notes ? $Notes : "Version $Version")
    if ($LASTEXITCODE -ne 0) { throw 'Publication GitHub échouée.' }
    Write-Host "Publié : https://github.com/$repo/releases/tag/v$Version"
}
