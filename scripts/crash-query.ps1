try {
  $events = Get-WinEvent -FilterHashtable @{LogName='Application'; StartTime=(Get-Date).AddMinutes(-30)} -EA Stop |
    Where-Object { $_.Message -match 'native-wallpaper' } | Select-Object -First 4
  if (-not $events) { Write-Output 'no crash events for native-wallpaper in last 30min' }
  foreach ($e in $events) {
    Write-Output ("id=" + $e.Id + " time=" + $e.TimeCreated.ToString('HH:mm:ss'))
    Write-Output $e.Message
    Write-Output "---"
  }
} catch { Write-Output ("query-failed: " + $_.Exception.Message) }
