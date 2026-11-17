# update-release-assets.ps1 - replace the two dist assets on an existing GitHub release
param([string]$Tag = 'v1.0.0', [string]$Repo = 'Zhong-master/Agent-Heart')
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$root = Split-Path -Parent $PSScriptRoot
$tok = (Get-Content (Join-Path $root 'out\github-token.txt') -Raw).Trim()
$h = @{ Authorization = "Bearer $tok" }
$rel = Invoke-RestMethod -Uri ("https://api.github.com/repos/" + $Repo + "/releases/tags/" + $Tag) -Headers $h
foreach ($a in @($rel.assets)) {
  if ($a.name -in @('Setup-1.0.0.exe','Agent-Heart-1.0.0-win64.zip')) {
    Invoke-RestMethod -Uri $a.url -Method Delete -Headers $h | Out-Null
    Write-Output ("deleted old asset " + $a.name)
  }
}
$assets = @(
  @{ path = (Join-Path $root 'dist\Setup-1.0.0.exe'); name = 'Setup-1.0.0.exe' },
  @{ path = (Join-Path $root 'dist\Agent-Heart-1.0.0-win64.zip'); name = 'Agent-Heart-1.0.0-win64.zip' }
)
foreach ($a in $assets) {
  if (-not (Test-Path $a.path)) { throw ("missing " + $a.path) }
  $up = Invoke-RestMethod -Uri ("https://uploads.github.com/repos/" + $Repo + "/releases/" + $rel.id + "/assets?name=" + [Uri]::EscapeDataString($a.name)) -Method Post -Headers $h -ContentType 'application/octet-stream' -InFile $a.path
  Write-Output ("uploaded " + $up.name + " " + $up.size + " bytes")
}
