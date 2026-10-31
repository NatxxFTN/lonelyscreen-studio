<#
.SYNOPSIS
    Installe, met à jour ou retire LonelyScreen Studio.
.DESCRIPTION
    Installation en une ligne (PowerShell, sans droits admin au départ) :

        iwr -useb https://raw.githubusercontent.com/NatxxFTN/lonelyscreen-studio/main/install.ps1 | iex

    Désinstallation :

        & ([scriptblock]::Create((iwr -useb https://raw.githubusercontent.com/NatxxFTN/lonelyscreen-studio/main/install.ps1))) -Uninstall

    Étapes :
      1. Télécharge le manifeste de la dernière version et VÉRIFIE SA SIGNATURE
         (clé publique ci-dessous). Rien n'est installé si elle est invalide.
      2. Télécharge chaque fichier et vérifie son SHA-256, dans
         %LOCALAPPDATA%\LonelyScreenStudio\versions\<version>.
      3. Pose le relais d3d9.dll à côté de LonelyScreen.exe : seule étape qui
         demande les droits administrateur (fenêtre de confirmation Windows).
    Les mises à jour suivantes se font depuis le Studio, sans droits admin.
#>
param(
    [switch]$Uninstall,
    [switch]$Purge,
    [string]$Feed = '',
    [string]$LonelyScreenDir = ''
)

# --- Identité de publication (écrite par tools/new-signing-key.ps1) -----------
$Repository = 'NatxxFTN/lonelyscreen-studio'
$PublicKeyX = 'EOfsQZKf2K/z+6aYM/+wJoHNktp4Kw0PbGP+XZDNvgk='
$PublicKeyY = 'ylTd8lLZjMu4SGKFxax6x1dEipD1FUn/1iH8HWLUiyw='
# ------------------------------------------------------------------------------

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$AppBase = Join-Path $env:LOCALAPPDATA 'LonelyScreenStudio'
$SettingsDir = Join-Path $env:APPDATA 'LonelyScreenPlugins'
if (-not $Feed) { $Feed = "https://github.com/$Repository/releases/latest/download/" }
$FeedIsRemote = $Feed -like 'https://*'

function Write-Step([string]$Text) { Write-Host "  > $Text" -ForegroundColor Cyan }
function Write-Done([string]$Text) { Write-Host "  OK $Text" -ForegroundColor Green }

function Find-LonelyScreen {
    if ($LonelyScreenDir) { return $LonelyScreenDir }
    $keys = @(
        'HKLM:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*',
        'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\*',
        'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\*'
    )
    foreach ($entry in Get-ItemProperty $keys -ErrorAction SilentlyContinue) {
        if ($entry.DisplayName -like 'LonelyScreen*' -and $entry.InstallLocation) {
            $dir = $entry.InstallLocation.TrimEnd('\')
            if (Test-Path (Join-Path $dir 'LonelyScreen.exe')) { return $dir }
        }
    }
    $default = Join-Path ${env:ProgramFiles(x86)} 'LonelyScreen'
    if (Test-Path (Join-Path $default 'LonelyScreen.exe')) { return $default }
    throw 'LonelyScreen est introuvable. Installe-le, ou relance avec -LonelyScreenDir "C:\chemin\vers\LonelyScreen".'
}

function Get-FeedBytes([string]$Name, [string]$Url) {
    if ($FeedIsRemote) {
        $source = if ($Url) { $Url } else { $Feed + $Name }
        $temp = [IO.Path]::GetTempFileName()
        try {
            Invoke-WebRequest -UseBasicParsing -Uri $source -OutFile $temp
            return [IO.File]::ReadAllBytes($temp)
        } finally {
            Remove-Item $temp -ErrorAction SilentlyContinue
        }
    }
    return [IO.File]::ReadAllBytes((Join-Path $Feed $Name))
}

function Get-Sha256Hex([byte[]]$Bytes) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return -join ($sha.ComputeHash($Bytes) | ForEach-Object { $_.ToString('x2') }) } finally { $sha.Dispose() }
}

function Test-ReleaseSignature([byte[]]$Data, [string]$SignatureBase64) {
    if (-not $PublicKeyX -or -not $PublicKeyY) { throw 'Installateur incomplet : clé publique absente.' }
    $parameters = New-Object Security.Cryptography.ECParameters
    $parameters.Curve = [Security.Cryptography.ECCurve]::CreateFromFriendlyName('nistP256')
    $point = New-Object Security.Cryptography.ECPoint
    $point.X = [Convert]::FromBase64String($PublicKeyX)
    $point.Y = [Convert]::FromBase64String($PublicKeyY)
    $parameters.Q = $point
    $ecdsa = [Security.Cryptography.ECDsa]::Create()
    try {
        $ecdsa.ImportParameters($parameters)
        $signature = [Convert]::FromBase64String($SignatureBase64.Trim())
        return $ecdsa.VerifyData($Data, $signature, [Security.Cryptography.HashAlgorithmName]::SHA256)
    } finally {
        $ecdsa.Dispose()
    }
}

function Wait-LonelyScreenClosed {
    while (Get-Process -Name 'LonelyScreen' -ErrorAction SilentlyContinue) {
        $answer = Read-Host '  LonelyScreen est ouvert. Le fermer maintenant ? (O/n)'
        if ($answer -match '^(n|non)$') { throw 'Installation annulée : ferme LonelyScreen puis relance la commande.' }
        Get-Process -Name 'LonelyScreen' -ErrorAction SilentlyContinue | Stop-Process -Force
        Start-Sleep -Seconds 1
    }
}

# Étape administrateur, isolée dans un petit script aux arguments contrôlés.
$ElevatedScript = @'
param([string]$Action, [string]$Target, [string]$Source)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path (Join-Path $Target 'LonelyScreen.exe'))) { exit 3 }
$proxy = Join-Path $Target 'd3d9.dll'
if ($Action -eq 'install') {
    Copy-Item -LiteralPath $Source -Destination $proxy -Force
} elseif ($Action -eq 'uninstall') {
    if (Test-Path $proxy) { Remove-Item -LiteralPath $proxy -Force }
} else { exit 4 }
foreach ($legacy in 'plugins', 'studio', 'PluginSettings') {
    $path = Join-Path $Target $legacy
    if (Test-Path $path) { Remove-Item -LiteralPath $path -Recurse -Force }
}
exit 0
'@

function Test-Writable([string]$Directory) {
    $probe = Join-Path $Directory ('.lsstudio-' + [Guid]::NewGuid().ToString('N'))
    try {
        [IO.File]::WriteAllText($probe, '')
        Remove-Item -LiteralPath $probe -Force
        return $true
    } catch {
        return $false
    }
}

function Invoke-Elevated([string]$Action, [string]$Target, [string]$Source) {
    $scriptPath = Join-Path ([IO.Path]::GetTempPath()) ('lsstudio-admin-' + [Guid]::NewGuid().ToString('N') + '.ps1')
    Set-Content -LiteralPath $scriptPath -Value $ElevatedScript -Encoding UTF8
    try {
        $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$scriptPath`"",
            '-Action', $Action, '-Target', "`"$Target`"", '-Source', "`"$Source`"")
        # Dossier déjà modifiable (installation par utilisateur) : pas besoin de droits admin.
        $elevation = if (Test-Writable $Target) { @{} } else { @{ Verb = 'RunAs' } }
        $process = Start-Process -FilePath 'powershell.exe' -ArgumentList $arguments @elevation -Wait -PassThru -WindowStyle Hidden
        if ($process.ExitCode -ne 0) { throw "Étape administrateur échouée (code $($process.ExitCode))." }
    } catch [System.InvalidOperationException] {
        throw 'Droits administrateur refusés : le relais n''a pas pu être posé à côté de LonelyScreen.'
    } finally {
        Remove-Item -LiteralPath $scriptPath -ErrorAction SilentlyContinue
    }
}

Write-Host ''
Write-Host '  LonelyScreen Studio' -ForegroundColor White
Write-Host '  Module communautaire, non affilié à IMTIGER Technologies.' -ForegroundColor DarkGray
Write-Host ''

$target = Find-LonelyScreen
Write-Step "LonelyScreen trouvé : $target"

if ($Uninstall) {
    Wait-LonelyScreenClosed
    Write-Step 'Retrait du relais (confirmation administrateur)…'
    Invoke-Elevated 'uninstall' $target ''
    if (Test-Path $AppBase) { Remove-Item -LiteralPath $AppBase -Recurse -Force }
    if ($Purge -and (Test-Path $SettingsDir)) { Remove-Item -LiteralPath $SettingsDir -Recurse -Force }
    Write-Done 'LonelyScreen Studio est désinstallé. LonelyScreen est revenu à son état d''origine.'
    if (-not $Purge) { Write-Host "  Tes réglages restent dans $SettingsDir (ajoute -Purge pour les effacer)." }
    return
}

Write-Step 'Vérification de la signature de la dernière version…'
$manifestBytes = Get-FeedBytes 'latest.json' ''
$signatureText = [Text.Encoding]::ASCII.GetString((Get-FeedBytes 'latest.json.sig' ''))
if (-not (Test-ReleaseSignature $manifestBytes $signatureText)) {
    throw 'Signature invalide : cette version n''a pas été publiée par l''auteur. Installation annulée.'
}
$manifest = [Text.Encoding]::UTF8.GetString($manifestBytes) | ConvertFrom-Json
$version = [string]$manifest.version
if ($version -notmatch '^[0-9A-Za-z.-]{1,32}$') { throw 'Manifeste invalide.' }
Write-Done "Version $version authentifiée"

Write-Step 'Téléchargement et vérification des fichiers…'
$versionsDir = Join-Path $AppBase 'versions'
$staging = Join-Path $versionsDir "$version.staging"
if (Test-Path $staging) { Remove-Item -LiteralPath $staging -Recurse -Force }
$stubPath = Join-Path ([IO.Path]::GetTempPath()) ('lsstudio-d3d9-' + [Guid]::NewGuid().ToString('N') + '.dll')
$count = 0
foreach ($file in $manifest.files) {
    $path = [string]$file.path
    if ($path -match '(^[\\/])|(\.\.)|(:)' -or $path -notmatch '^[A-Za-z0-9._/-]+$') { throw "Chemin refusé : $path" }
    $bytes = Get-FeedBytes ([string]$file.asset) ([string]$file.url)
    if ((Get-Sha256Hex $bytes) -ne ([string]$file.sha256).ToLowerInvariant()) { throw "Fichier corrompu : $path" }
    if ($path -eq 'stub/d3d9.dll') {
        [IO.File]::WriteAllBytes($stubPath, $bytes)
    } else {
        $destination = Join-Path $staging ($path -replace '/', '\')
        New-Item -ItemType Directory -Force (Split-Path $destination -Parent) | Out-Null
        [IO.File]::WriteAllBytes($destination, $bytes)
    }
    $count++
}
if (-not (Test-Path $stubPath) -or -not (Test-Path (Join-Path $staging 'lsstudio.dll'))) {
    throw 'Version incomplète (relais ou moteur absent).'
}
$final = Join-Path $versionsDir $version
if (Test-Path $final) { Remove-Item -LiteralPath $final -Recurse -Force }
Move-Item -LiteralPath $staging -Destination $final
[IO.File]::WriteAllText((Join-Path $AppBase 'current.txt'), $version)
Write-Done "$count fichiers vérifiés"

Wait-LonelyScreenClosed
Write-Step 'Installation du relais à côté de LonelyScreen (confirmation administrateur)…'
try {
    Invoke-Elevated 'install' $target $stubPath
} finally {
    Remove-Item -LiteralPath $stubPath -ErrorAction SilentlyContinue
}

Write-Host ''
Write-Done "LonelyScreen Studio $version est installé."
Write-Host '  Lance LonelyScreen : un onglet apparaît au bord de sa fenêtre (ou Ctrl+Maj+P).'
Write-Host '  Les mises à jour se font ensuite depuis le Studio, dans Réglages.'
Write-Host ''
