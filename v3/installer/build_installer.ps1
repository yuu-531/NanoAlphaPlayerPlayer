# ---------------------------------------------------------------------------
#  NanoAlphaPlayer - installer build script (ASCII only)
#
#    powershell -ExecutionPolicy Bypass -File installer\build_installer.ps1
#
#  Output: v3\dist\NanoAlphaPlayer-<version>-x64.msi
#
#  Requires:
#    * WiX Toolset v5 as a dotnet tool
#        dotnet tool install --global wix
#    * the packaging build (build-pkg\Release). Rebuilt automatically when the
#      exe is missing.
# ---------------------------------------------------------------------------
$ErrorActionPreference = 'Stop'

$installerDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoDir      = Split-Path -Parent $installerDir          # F:\opencode\mediaplayer\v3
$topDir       = Split-Path -Parent $repoDir              # F:\opencode\mediaplayer
$buildDir     = Join-Path $repoDir 'build-pkg\Release'
$distDir      = Join-Path $repoDir 'dist'
$wxs          = Join-Path $installerDir 'installer.wxs'
$exe          = Join-Path $buildDir 'NanoAlphaPlayer.exe'
$license      = Join-Path $topDir 'LICENSE'
$rtf          = Join-Path $installerDir 'License.rtf'
$cmake        = 'C:\Program Files\CMake\bin\cmake.exe'

# --- product version, read from CMakeLists.txt -----------------------------
$cmakeText = [System.IO.File]::ReadAllText((Join-Path $repoDir 'CMakeLists.txt'),
                                           [System.Text.UTF8Encoding]::new($false))
$version = '0.0.0'
if ($cmakeText -match 'project\s*\(\s*NanoAlphaPlayer\s+VERSION\s+([0-9]+)\.([0-9]+)\.([0-9]+)') {
    $version = "$($Matches[1]).$($Matches[2]).$($Matches[3])"
}
"product version = $version"

# --- packaging build -------------------------------------------------------
if (-not (Test-Path $exe)) {
    "packaging build not found, building..."
    & $cmake -S $repoDir -B (Join-Path $repoDir 'build-pkg') `
             -DNANOALPHA_CONSOLE=ON -DNANOALPHA_CONSOLE_WINDOW=OFF `
             -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }
    & $cmake --build (Join-Path $repoDir 'build-pkg') --config Release --parallel | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "cmake build failed ($LASTEXITCODE)" }
}
if (-not (Test-Path $exe)) { throw "packaged exe missing: $exe" }

# Required runtime payload. avdevice / avfilter are not referenced by the
# player (it only imports the five DLLs below) so they are left out.
$required = @('avformat-63.dll', 'avcodec-63.dll', 'avutil-61.dll',
              'swresample-7.dll', 'swscale-10.dll', 'ffx_a.h', 'ffx_fsr1.h')
foreach ($f in $required) {
    if (-not (Test-Path (Join-Path $buildDir $f))) { throw "missing runtime file: $f" }
}

# --- GPL v3 text -> RTF for the WiX license dialog -------------------------
$gplText = [System.IO.File]::ReadAllText($license, [System.Text.UTF8Encoding]::new($false))
$sb = New-Object System.Text.StringBuilder
[void]$sb.Append('{\rtf1\ansi\deff0{\fonttbl{\f0\fmodern Courier New;}}\viewkind4\uc1\pard\f0\fs16 ')
foreach ($ch in $gplText.ToCharArray()) {
    $c = [int]$ch
    switch ($c) {
        13  { }                                    # CR (LF is expanded below)
        10  { [void]$sb.Append('\par ') }
        92  { [void]$sb.Append('\\') }             # backslash
        123 { [void]$sb.Append('\{') }             # {
        125 { [void]$sb.Append('\}') }             # }
        default {
            if ($c -lt 128) {
                [void]$sb.Append([char]$c)
            } else {
                $u = if ($c -gt 32767) { $c - 65536 } else { $c }
                [void]$sb.Append('\u' + $u + '?')
            }
        }
    }
}
[void]$sb.Append('}')
[System.IO.File]::WriteAllText($rtf, $sb.ToString(), [System.Text.Encoding]::ASCII)
"license rtf -> $rtf ($((Get-Item $rtf).Length) bytes)"

# --- locate WiX ------------------------------------------------------------
$wixExe = Join-Path $env:USERPROFILE '.dotnet\tools\wix.exe'
if (-not (Test-Path $wixExe)) {
    throw "wix not found. Install it with: dotnet tool install --global wix"
}
"using $(& $wixExe --version)"

# The UI library (WixUI_Mondo dialogs) ships as a WiX extension package.
# Pull the version that matches the installed wix tool, once.
$wixVersion = (& $wixExe --version 2>&1 | Select-String -Pattern '(\d+\.\d+\.\d+)').Matches[0].Groups[1].Value
$extList = (& $wixExe extension list 2>&1 | Out-String)
if ($extList -notmatch 'WixToolset\.UI') {
    "adding WiX UI extension ($wixVersion)..."
    & $wixExe extension add --global "WixToolset.UI.wixext/$wixVersion" | Out-Host
    # A non-zero code here usually just means it is already in the cache.
}

# --- build the MSI ---------------------------------------------------------
if (-not (Test-Path $distDir)) { New-Item -ItemType Directory -Path $distDir | Out-Null }
$msiName = "NanoAlphaPlayer-$version-x64.msi"
$msiPath = Join-Path $distDir $msiName

& $wixExe build `
    -arch x64 `
    -ext WixToolset.UI.wixext `
    -d "BuildDir=$buildDir" `
    -d "RepoDir=$repoDir" `
    -d "LicenseRtf=$rtf" `
    -d "ProductVersion=$version" `
    -out $msiPath `
    $wxs
if ($LASTEXITCODE -ne 0) { throw "wix build failed ($LASTEXITCODE)" }

$hash = (Get-FileHash -Path $msiPath -Algorithm SHA256).Hash
"installer -> $msiPath"
"  size     = $([math]::Round((Get-Item $msiPath).Length / 1MB, 2)) MB"
"  sha256   = $hash"
