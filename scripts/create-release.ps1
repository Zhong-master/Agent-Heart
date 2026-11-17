# create-release.ps1 - create a GitHub release and upload dist assets (reads token from out\github-token.txt)
param(
  [string]$Tag = 'v1.0.0',
  [string]$Name = 'Agent-Heart v1.0.0',
  [string]$NotesPath = '',
  [string]$Repo = 'Zhong-master/Agent-Heart'
)
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$root = Split-Path -Parent $PSScriptRoot
$tok = (Get-Content (Join-Path $root 'out\github-token.txt') -Raw).Trim()
if (-not $NotesPath) { $NotesPath = Join-Path $root 'out\release-notes-v1.0.0.md' }
$notes = [string](Get-Content $NotesPath -Raw -Encoding UTF8)
$h = @{ Authorization = "Bearer $tok" }

$existing = $null
try { $existing = Invoke-RestMethod -Uri ("https://api.github.com/repos/" + $Repo + "/releases/tags/" + $Tag) -Headers $h } catch { $existing = $null }
if ($existing) { Write-Output ("release already exists: " + $existing.html_url); exit 0 }

$body = @{ tag_name = $Tag; target_commitish = 'main'; name = $Name; body = $notes; draft = $false; prerelease = $false } | ConvertTo-Json -Depth 5
$rel = Invoke-RestMethod -Uri ("https://api.github.com/repos/" + $Repo + "/releases") -Method Post -Headers $h -ContentType 'application/json' -Body ([Text.Encoding]::UTF8.GetBytes($body))
Write-Output ("created release id=" + $rel.id + " " + $rel.html_url)

$assets = @(
  @{ path = (Join-Path $root 'dist\Setup-1.0.0.exe'); name = 'Setup-1.0.0.exe' },
  @{ path = (Join-Path $root 'dist\Agent-Heart-1.0.0-win64.zip'); name = 'Agent-Heart-1.0.0-win64.zip' }
)
foreach ($a in $assets) {
  if (-not (Test-Path $a.path)) { throw ("missing asset " + $a.path) }
  $up = Invoke-RestMethod -Uri ("https://uploads.github.com/repos/" + $Repo + "/releases/" + $rel.id + "/assets?name=" + [Uri]::EscapeDataString($a.name)) -Method Post -Headers $h -ContentType 'application/octet-stream' -InFile $a.path
  Write-Output ("uploaded " + $up.name + " " + $up.size + " bytes -> " + $up.browser_download_url)
}
