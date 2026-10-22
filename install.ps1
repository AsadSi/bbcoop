# bbcoop: one-command Windows setup. Installs MSYS2 and Python, builds bbcoop, unpacks it to
# %LOCALAPPDATA%\bbcoop, points the launcher at your Bloodborne folder and leaves Bloodborne.exe on the
# desktop. Run from the clone (docs/bbcoop/windows.md):
#   powershell -ExecutionPolicy Bypass -File install.ps1 [-GameDir D:\Games\CUSA00900]
param([string]$GameDir)
$ErrorActionPreference = 'Stop'
$repo = $PSScriptRoot
$msys = 'C:\msys64'
$dest = Join-Path $env:LOCALAPPDATA 'bbcoop'

function Winget($id) {
    winget install --id $id -e --silent --accept-package-agreements --accept-source-agreements
    # -1978335189 = already installed, no upgrade available
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne -1978335189) { throw "winget install $id failed ($LASTEXITCODE)" }
}
function Msys($command) {
    $env:MSYSTEM = 'CLANG64'; $env:CHERE_INVOKING = '1'
    & "$msys\usr\bin\bash.exe" -lc $command
    if ($LASTEXITCODE -ne 0) { throw "MSYS2 step failed: $command" }
}

# The game first: a wrong folder should fail before a 30-minute build.
if (-not $GameDir) {
    Add-Type -AssemblyName System.Windows.Forms
    $picker = New-Object System.Windows.Forms.FolderBrowserDialog
    $picker.Description = 'Pick your Bloodborne folder (has eboot.bin; version 1.09, CUSA00900 or CUSA03173)'
    if ($picker.ShowDialog() -ne 'OK') { throw 'No game folder picked.' }
    $GameDir = $picker.SelectedPath
}
if (-not (Test-Path (Join-Path $GameDir 'eboot.bin'))) { throw "No eboot.bin in $GameDir" }

Winget 'MSYS2.MSYS2'
Winget 'Python.Python.3.12'
Push-Location $repo
git submodule update --init --recursive
Pop-Location

$unix = (& "$msys\usr\bin\cygpath.exe" -u $repo)
try { Msys 'pacman -Syu --noconfirm' } catch { }   # the first run may end after updating MSYS2 itself
Msys 'pacman -Syu --noconfirm'
Msys ('pacman -S --needed --noconfirm git patch ' +
  'mingw-w64-clang-x86_64-{toolchain,cmake,ninja,pkgconf,python} ' +
  'mingw-w64-clang-x86_64-{vulkan-headers,vulkan-loader,vulkan-memory-allocator,glslang} ' +
  'mingw-w64-clang-x86_64-{sdl3,fmt,boost,robin-map,xxhash,ffmpeg,zydis} ' +
  'mingw-w64-clang-x86_64-{spirv-tools,spirv-headers,spirv-cross} ' +
  'mingw-w64-clang-x86_64-{protobuf,openssl,zlib,miniupnpc,nlohmann-json}')
Msys "cd '$unix' && bash build.sh && bash packaging/windows/package.sh"

# Unpack the package, keeping saves from an earlier install.
$tmp = Join-Path $env:TEMP 'bbcoop-unpack'
Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
Expand-Archive (Join-Path $repo 'dist\bbcoop-windows.zip') $tmp
New-Item $dest -ItemType Directory -Force | Out-Null
Copy-Item "$tmp\bbcoop-windows\*" $dest -Recurse -Force
Remove-Item $tmp -Recurse -Force

# Launcher settings: only game_dir is set, the rest are the launcher's defaults.
$configDir = Join-Path $env:APPDATA 'bbcoop-launcher'
$config = Join-Path $configDir 'settings.json'
New-Item $configDir -ItemType Directory -Force | Out-Null
$settings = @{}
if (Test-Path $config) { (Get-Content $config -Raw | ConvertFrom-Json).PSObject.Properties | ForEach-Object { $settings[$_.Name] = $_.Value } }
$settings['game_dir'] = $GameDir
$settings | ConvertTo-Json | Set-Content $config -Encoding UTF8

$lnk = (New-Object -ComObject WScript.Shell).CreateShortcut((Join-Path ([Environment]::GetFolderPath('Desktop')) 'Bloodborne.lnk'))
$lnk.TargetPath = Join-Path $dest 'Bloodborne.exe'
$lnk.WorkingDirectory = $dest
$lnk.Save()

Write-Host "Done. Start Bloodborne from the desktop shortcut ($dest\Bloodborne.exe)."
