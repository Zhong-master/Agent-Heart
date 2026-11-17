# package-dist.ps1 - build release assets into dist\
# Requires: out\Agent-Heart.exe (build.bat) and installer\Setup.exe (installer\build-installer.bat)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$ver = '1.0.0'
$dist = Join-Path $root 'dist'
$app = Join-Path $root 'out\Agent-Heart.exe'
$setup = Join-Path $root 'installer\Setup.exe'

if (-not (Test-Path $app)) { throw "missing $app - run build.bat first" }
if (-not (Test-Path $setup)) { throw "missing $setup - run installer\build-installer.bat first" }

if (Test-Path $dist) { Remove-Item $dist -Recurse -Force }
New-Item -ItemType Directory -Force -Path $dist | Out-Null

# portable package
$stage = Join-Path $dist ("Agent-Heart-" + $ver + "-win64")
New-Item -ItemType Directory -Force -Path $stage | Out-Null
Copy-Item $app $stage
Copy-Item (Join-Path $root 'README.md') $stage
Copy-Item (Join-Path $root 'LICENSE') $stage
Copy-Item (Join-Path $root 'CHANGELOG.md') $stage
$zip = Join-Path $dist ("Agent-Heart-" + $ver + "-win64.zip")
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -Force

# installer asset
Copy-Item $setup (Join-Path $dist ("Setup-" + $ver + ".exe"))

Write-Output ("dist ready:")
Get-ChildItem $dist -File | ForEach-Object { Write-Output ("  " + $_.Name + "  " + $_.Length + " bytes") }
