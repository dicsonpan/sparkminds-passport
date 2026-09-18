$pkgDir = Join-Path $PSScriptRoot "arcs_mini_flash_pack"
if (Test-Path $pkgDir) { Remove-Item -Recurse -Force $pkgDir }

New-Item -ItemType Directory -Path (Join-Path $pkgDir "tools\adb") -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $pkgDir "tools\cskburn") -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $pkgDir "res\arcs-mini") -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $pkgDir "build") -Force | Out-Null

$root = (Get-Location).Path
Copy-Item "$root\flash_all.bat" -Destination $pkgDir -ErrorAction SilentlyContinue
Copy-Item "$root\flash_app.bat" -Destination $pkgDir -ErrorAction SilentlyContinue
Copy-Item "$root\flash_uart_full.bat" -Destination $pkgDir -ErrorAction SilentlyContinue
Copy-Item "$root\adb_download.ps1" -Destination $pkgDir
Copy-Item "$root\全量烧录规范.md" -Destination "$pkgDir\全量烧录规范.md" -ErrorAction SilentlyContinue
Copy-Item "$root\apps\sparkminds-passport\STUDENTS.md" -Destination "$pkgDir\STUDENTS.md" -ErrorAction SilentlyContinue
Copy-Item "$root\FLASH_GUIDE.md" -Destination "$pkgDir\FLASH_GUIDE.md" -ErrorAction SilentlyContinue
Copy-Item "$root\FLASH_GUIDE.md" -Destination "$pkgDir\README.md" -ErrorAction SilentlyContinue

Copy-Item (Join-Path $PSScriptRoot "tools\adb\*") -Destination (Join-Path $pkgDir "tools\adb")
Copy-Item (Join-Path $PSScriptRoot "tools\cskburn\*") -Destination (Join-Path $pkgDir "tools\cskburn")
Copy-Item (Join-Path $PSScriptRoot "res\arcs-mini\partition_table.json") -Destination (Join-Path $pkgDir "res\arcs-mini")
Copy-Item (Join-Path $PSScriptRoot "res\arcs-mini\ap.bin") -Destination (Join-Path $pkgDir "res\arcs-mini")
Copy-Item (Join-Path $PSScriptRoot "res\arcs-mini\tone.bin") -Destination (Join-Path $pkgDir "res\arcs-mini")
Copy-Item (Join-Path $PSScriptRoot "res\arcs-mini\wake_word.bin") -Destination (Join-Path $pkgDir "res\arcs-mini")
Copy-Item (Join-Path $PSScriptRoot "res\arcs-mini\emoji.bin") -Destination (Join-Path $pkgDir "res\arcs-mini")
Copy-Item (Join-Path $PSScriptRoot "res\arcs-mini\respak.bin") -Destination (Join-Path $pkgDir "res\arcs-mini")

$bootBin = Join-Path $PSScriptRoot "res\arcs-mini\boot.bin"
if (Test-Path $bootBin) {
    Copy-Item $bootBin -Destination (Join-Path $pkgDir "res\arcs-mini")
}

Copy-Item (Join-Path $PSScriptRoot "build\arcs-mini.bin") -Destination (Join-Path $pkgDir "build")
if (Test-Path (Join-Path $PSScriptRoot "build\arcs-mini.lpk")) {
    Copy-Item (Join-Path $PSScriptRoot "build\arcs-mini.lpk") -Destination (Join-Path $pkgDir "build")
}
if (Test-Path (Join-Path $PSScriptRoot "build\arcs-mini.combined.hex")) {
    Copy-Item (Join-Path $PSScriptRoot "build\arcs-mini.combined.hex") -Destination (Join-Path $pkgDir "build")
}

$zipFile = Join-Path $PSScriptRoot "arcs_mini_flash_pack.zip"
if (Test-Path $zipFile) { Remove-Item -Force $zipFile }

Write-Host "Creating zip package..."
Compress-Archive -Path "$pkgDir\*" -DestinationPath $zipFile -Force
Remove-Item -Recurse -Force $pkgDir

$desktopZip = "C:\Users\chen\Desktop\arcs_mini_flash_pack.zip"
Copy-Item $zipFile -Destination $desktopZip -Force -ErrorAction SilentlyContinue

$item = Get-Item $zipFile
Write-Host "Done! Package size: $([math]::Round($item.Length / 1MB, 2)) MB"
Write-Host "Package Path: $($item.FullName)"
if (Test-Path $desktopZip) {
    Write-Host "Desktop Mirror: $desktopZip"
}
