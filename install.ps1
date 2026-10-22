# bbcoop: one-command Windows setup. Installs MSYS2 and Python, builds bbcoop, unpacks it to
# %LOCALAPPDATA%\bbcoop, points the launcher at your Bloodborne folder and leaves Bloodborne.exe on the
# desktop. Run from the clone (docs/bbcoop/windows.md):
#   powershell -ExecutionPolicy Bypass -File install.ps1 [-GameDir D:\Games\CUSA00900]
param([string]$GameDir)
$ErrorActionPreference = 'Stop'
$repo = $PSScriptRoot
$msys = 'C:\msys64'
$dest = Join-Path $env:LOCALAPPDATA 'bbcoop'

function Add-WingetPackage($id) {
    winget.exe install --id $id -e --source winget --silent --accept-package-agreements --accept-source-agreements
    # winget.exe: the function must not share its name. Exit codes -1978335189 and -1978335135: already installed.
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne -1978335189 -and $LASTEXITCODE -ne -1978335135) { throw "winget install $id failed ($LASTEXITCODE)" }
}
function Msys($command) {
    $env:MSYSTEM = 'CLANG64'; $env:CHERE_INVOKING = '1'; $env:MSYS2_PATH_TYPE = 'inherit'  # inherit: powershell, git, py
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
# Git for Windows turns LF into CRLF by default, which breaks the bash scripts and the FSR patches.
if ((Get-Content (Join-Path $repo 'build.sh') -Raw) -match "`r`n") {
    throw 'This clone has CRLF line endings. Delete it and clone again with: git -c core.autocrlf=false -c core.longpaths=true clone -b windows https://github.com/AsadSi/bbcoop.git C:\bbcoop'
}

Add-WingetPackage 'MSYS2.MSYS2'
Add-WingetPackage 'Python.Python.3.13'
if (-not (Test-Path "$msys\usr\bin\bash.exe")) { throw "MSYS2 is not in $msys. Install it there or edit `$msys at the top of install.ps1." }
# Only the submodules the build needs (not server/externals/protobuf).
# Checked out with LF: with the default autocrlf they come out CRLF and the FSR-Vulkan patch does not apply.
$modules = 'gpu/third_party/fsr-vulkan', 'gpu/third_party/imgui', 'third_party/LibAtrac9'
git -C $repo submodule deinit -f -- $modules 2>$null   # drops an earlier CRLF checkout; the clone is cached
git -C $repo -c core.autocrlf=false submodule update --init -- $modules
if ($LASTEXITCODE -ne 0) { throw 'git submodule update failed' }

$unix = (& "$msys\usr\bin\cygpath.exe" -u $repo)
try { Msys 'pacman -Syu --noconfirm' } catch { }   # the first run may end after updating MSYS2 itself
Msys 'pacman -Syu --noconfirm'
Msys ('pacman -S --needed --noconfirm git patch ' +
  'mingw-w64-clang-x86_64-{toolchain,cmake,ninja,pkgconf,python} ' +
  'mingw-w64-clang-x86_64-{vulkan-headers,vulkan-loader,vulkan-memory-allocator,glslang} ' +
  'mingw-w64-clang-x86_64-{sdl3,fmt,boost,robin-map,xxhash,ffmpeg,zydis} ' +
  'mingw-w64-clang-x86_64-{spirv-tools,spirv-headers,spirv-cross} ' +
  'mingw-w64-clang-x86_64-{protobuf,openssl,zlib,miniupnpc,nlohmann-json}')
# package.sh picks the last Python3* folder by name, so an old Python39 would win: name the 3.13 one.
$py = $null
try { $py = (py -3.13 -c 'import sys; print(sys.executable)' 2>$null) } catch { }
if (-not $py) { $py = (Get-ChildItem "$env:LOCALAPPDATA\Programs\Python\Python313\python.exe", 'C:\Program Files\Python313\python.exe', 'C:\Python313\python.exe' -ErrorAction SilentlyContinue | Select-Object -First 1).FullName }
if (-not $py) { throw 'Python 3.13 not found after the winget install. Open a new PowerShell window and run install.ps1 again.' }
$pyUnix = (& "$msys\usr\bin\cygpath.exe" -u $py)
Msys "cd '$unix' && bash build.sh && WINPYTHON='$pyUnix' bash packaging/windows/package.sh"

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
# No BOM: the launcher reads this with Python's json, which rejects one (Set-Content -Encoding UTF8 adds it).
[IO.File]::WriteAllText($config, ($settings | ConvertTo-Json), (New-Object Text.UTF8Encoding $false))

$lnk = (New-Object -ComObject WScript.Shell).CreateShortcut((Join-Path ([Environment]::GetFolderPath('Desktop')) 'Bloodborne.lnk'))
$lnk.TargetPath = Join-Path $dest 'Bloodborne.exe'
$lnk.WorkingDirectory = $dest
$lnk.Save()

Write-Host "Done. Start Bloodborne from the desktop shortcut ($dest\Bloodborne.exe)."
