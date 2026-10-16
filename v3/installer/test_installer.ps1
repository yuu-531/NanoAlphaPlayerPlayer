# ---------------------------------------------------------------------------
#  test_installer.ps1  --  NanoAlphaPlayer MSI installer regression
#
#  Self contained: run it from anywhere.
#      powershell -ExecutionPolicy Bypass -File v3\installer\test_installer.ps1
#
#  What it covers
#    1. silent per-user install (no UAC prompt)
#    2. files, shortcuts, file associations, Add/Remove Programs entry
#    3. launch the installed binary
#         - the shipping build is a GUI subsystem binary, so no console window
#           may appear, while stdout logs must still be capturable
#    4. silent uninstall + full cleanup check
#
#  Exit code 0 = all checks passed.
# ---------------------------------------------------------------------------

$ErrorActionPreference = 'Continue'

Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class N
{
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr lp);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out int pid);
    [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);

    public static IntPtr[] TopLevel(int pid, bool visibleOnly)
    {
        System.Collections.Generic.List<IntPtr> list = new System.Collections.Generic.List<IntPtr>();
        EnumWindows(delegate(IntPtr h, IntPtr l)
        {
            int p; GetWindowThreadProcessId(h, out p);
            if (p != pid) return true;
            if (visibleOnly && !IsWindowVisible(h)) return true;
            list.Add(h);
            return true;
        }, IntPtr.Zero);
        return list.ToArray();
    }

    public static string ClassOf(IntPtr h)
    {
        StringBuilder s = new StringBuilder(256);
        GetClassName(h, s, 256);
        return s.ToString();
    }

    public static bool ClassExists(int pid, string cls)
    {
        foreach (IntPtr h in TopLevel(pid, false))
            if (ClassOf(h) == cls) return true;
        return false;
    }

    // Minimise is what the Win32 CloseWindow() does, so send WM_CLOSE instead.
    // PostMessage, not SendMessage: a window that stopped pumping messages
    // would otherwise block us forever on a synchronous cross-process send.
    [DllImport("user32.dll")] public static extern IntPtr PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    public static void Close(IntPtr h) { PostMessage(h, 0x0010, IntPtr.Zero, IntPtr.Zero); }
}
'@

$Temp    = [System.IO.Path]::GetTempPath()
$RepoDir = Split-Path -Parent $PSScriptRoot
$Msi     = Join-Path $RepoDir 'dist\NanoAlphaPlayer-0.1.0-x64.msi'
$Install = Join-Path $env:LOCALAPPDATA 'Programs\NanoAlphaPlayer'
$Exe     = Join-Path $Install 'NanoAlphaPlayer.exe'
$Shot    = Join-Path $Temp 'installer_test.png'
$Log     = Join-Path $Temp 'installer_test_stdout.txt'
$ErrTxt  = Join-Path $Temp 'installer_test_stderr.txt'
$UniHKCU = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall'
$UniHKLM = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall'
$ProgId  = 'NanoAlphaPlayer.MediaFile'

$fail = @()
function Ok([string]$m) { "  PASS  $m" }
function Ng([string]$m) { $script:fail += $m; "  FAIL  $m" }
function Check([bool]$cond, [string]$m) { if ($cond) { Ok $m } else { Ng $m } }

function FindArp([string]$hive) {
    Get-ChildItem $hive -ErrorAction SilentlyContinue |
        Where-Object { (Get-ItemProperty -LiteralPath $_.PSPath -ErrorAction SilentlyContinue).DisplayName -eq 'NanoAlphaPlayer' }
}

function MsiOp([string]$switch, [int]$timeoutSec) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = 'msiexec.exe'
    $psi.Arguments = "$switch `"$Msi`" /qn /norestart"
    $psi.UseShellExecute = $false
    $p = [System.Diagnostics.Process]::Start($psi)
    return @{ Done = $p.WaitForExit($timeoutSec * 1000); Code = $p.ExitCode }
}

function Snap([string]$path) {
    Add-Type -AssemblyName System.Windows.Forms
    $b = [System.Windows.Forms.SystemInformation]::VirtualScreen
    $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($b.X, $b.Y, 0, 0, $bmp.Size)
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
}

function WaitHwnd([System.Diagnostics.Process]$p, [int]$sec) {
    for ($i = 0; $i -lt ($sec * 10); $i++) {
        Start-Sleep -Milliseconds 100
        $p.Refresh()
        if ($p.MainWindowHandle -ne [IntPtr]::Zero) { return $p.MainWindowHandle }
    }
    return [IntPtr]::Zero
}

# The player opens a modal "open file" dialog at start-up, so every visible
# top-level window of the process has to be closed, not just the main one.
function CloseProcess([System.Diagnostics.Process]$p) {
    foreach ($h in ([N]::TopLevel($p.Id, $true))) { [N]::Close($h) }
    for ($i = 0; $i -lt 60; $i++) {
        Start-Sleep -Milliseconds 500
        $p.Refresh()
        if ($p.HasExited) { return $true }
    }
    return $false
}

"===== NanoAlphaPlayer installer regression ====="
if (-not (Test-Path $Msi)) {
    "  FAIL  msi missing: $Msi"
    "        build it with  v3\installer\build_installer.ps1"
    exit 1
}
Get-Process NanoAlphaPlayer -ErrorAction SilentlyContinue | Stop-Process -Force

"--- clean previous install ---"
$r0 = MsiOp '/x' 120
Check $r0.Done 'pre-clean uninstall completed'
Start-Sleep -Seconds 2

"--- install (per-user, silent) ---"
$r1 = MsiOp '/i' 180
Check $r1.Done 'msiexec install completed'
Check ($r1.Code -eq 0) "install exit code = $($r1.Code)"
Start-Sleep -Seconds 2

"--- files ---"
Check (Test-Path $Exe) 'NanoAlphaPlayer.exe installed'
foreach ($f in @('avformat-63.dll','avcodec-63.dll','avutil-61.dll',
                 'swresample-7.dll','swscale-10.dll','ffx_a.h','ffx_fsr1.h',
                 'LICENSE','THIRD_PARTY_LICENSES.md')) {
    Check (Test-Path (Join-Path $Install $f)) "installed: $f"
}
Check (-not (Test-Path (Join-Path $Install 'avfilter-12.dll'))) 'avfilter-12.dll not shipped (unused)'

"--- install location ---"
Check (-not (Test-Path (Join-Path ${env:ProgramFiles} 'NanoAlphaPlayer'))) 'not installed to Program Files (per-user)'

"--- shortcuts ---"
$sm = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\NanoAlphaPlayer\NanoAlphaPlayer.lnk'
$dk = Join-Path ([Environment]::GetFolderPath('Desktop')) 'NanoAlphaPlayer.lnk'
Check (Test-Path $sm) 'start menu shortcut (current user)'
Check (Test-Path $dk) 'desktop shortcut (current user)'
Check (-not (Test-Path (Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\NanoAlphaPlayer'))) 'no all-users start menu folder'

"--- file associations (HKCU) ---"
$cmd = (Get-ItemProperty "HKCU:\Software\Classes\$ProgId\shell\open\command" -ErrorAction SilentlyContinue).'(default)'
Check ($cmd -like "*NanoAlphaPlayer.exe*`"%1`"*") "progid open command = $cmd"
Check ($cmd -like "*$Install*") 'progid command points at the install dir'
$dicon = (Get-ItemProperty "HKCU:\Software\Classes\$ProgId\DefaultIcon" -ErrorAction SilentlyContinue).'(default)'
Check ($dicon -like '*NanoAlphaPlayer.exe*') "progid default icon = $dicon"
foreach ($ext in @('.mp4','.mkv','.mp3','.flac','.ts','.opus')) {
    $v = (Get-ItemProperty "HKCU:\Software\Classes\$ext" -ErrorAction SilentlyContinue).'(default)'
    Check ($v -eq $ProgId) "$ext default -> $v"
    $owp = Get-Item "HKCU:\Software\Classes\$ext\OpenWithProgids" -ErrorAction SilentlyContinue
    Check ($null -ne $owp -and $owp.GetValueNames() -contains $ProgId) "$ext OpenWithProgids"
}
$ra = (Get-ItemProperty 'HKCU:\Software\RegisteredApplications' -ErrorAction SilentlyContinue).NanoAlphaPlayer
Check ($ra -eq 'Software\NanoAlphaPlayer\Capabilities') "RegisteredApplications -> $ra"
$an = (Get-ItemProperty 'HKCU:\Software\NanoAlphaPlayer\Capabilities' -ErrorAction SilentlyContinue).ApplicationName
Check ($an -eq 'NanoAlphaPlayer') "Capabilities ApplicationName = $an"
$capExt = Get-Item 'HKCU:\Software\NanoAlphaPlayer\Capabilities\FileAssociations' -ErrorAction SilentlyContinue
$capN = if ($capExt) { $capExt.GetValueNames().Count } else { 0 }
Check ($capN -ge 28) "Capabilities FileAssociations count = $capN"

"--- Add/Remove Programs ---"
# Windows Installer writes the ARP key of a per-user product under HKLM, so
# ARPSYSTEMCOMPONENT=1 hides that key and a complete HKCU entry is authored
# by the installer itself. Verify both halves of that arrangement.
$wic = New-Object -ComObject WindowsInstaller.Installer
$wdb = $wic.OpenDatabase($Msi, 0)
$wview = $wdb.OpenView('SELECT * FROM Property'); $wview.Execute()
$syscomp = ''
while ($true) {
    $wrec = $wview.Fetch(); if ($null -eq $wrec) { break }
    if ($wrec.StringData(1) -eq 'ARPSYSTEMCOMPONENT') { $syscomp = $wrec.StringData(2) }
}
Check ($syscomp -eq '1') "ARPSYSTEMCOMPONENT=$syscomp (suppresses the machine-wide ARP key)"
$arp = FindArp $UniHKCU
Check ($null -ne $arp) 'ARP entry under HKCU (current user)'
if ($arp) {
    $a = Get-ItemProperty -LiteralPath $arp[0].PSPath
    Check ($a.Publisher -eq 'yuu-531') "ARP publisher = $($a.Publisher)"
    Check ($a.DisplayVersion -eq '0.1.0') "ARP version = $($a.DisplayVersion)"
    Check ($a.InstallLocation -eq ($Install + '\')) "ARP install location = $($a.InstallLocation)"
    Check ($a.UninstallString -like '*msiexec*') 'ARP uninstall string'
    Check ($a.DisplayIcon -like '*NanoAlphaPlayer.exe*') "ARP icon = $($a.DisplayIcon)"
    Check ($a.NoModify -eq 1) 'ARP NoModify'
}

"--- launch the installed binary ---"
Get-Process NanoAlphaPlayer -ErrorAction SilentlyContinue | Stop-Process -Force
Remove-Item $Log,$ErrTxt -ErrorAction SilentlyContinue
# Redirect to files rather than through a pipe: an undrained pipe fills up at
# 64 KB and the player then blocks on its own logging.
$app = Start-Process -FilePath $Exe -WorkingDirectory $Install `
                     -RedirectStandardOutput $Log -RedirectStandardError $ErrTxt -PassThru
$alive = WaitHwnd $app 20
Check ($alive -ne [IntPtr]::Zero) 'installed exe opened a window'
Start-Sleep -Seconds 8

$consoleWins = @([N]::TopLevel($app.Id, $true) | Where-Object { [N]::ClassOf($_) -eq 'ConsoleWindowClass' })
Check ($consoleWins.Count -eq 0) "console windows owned by player = $($consoleWins.Count)"
Check ([N]::ClassExists($app.Id, 'NanoAlphaPlayerWindow')) 'window class NanoAlphaPlayerWindow'
try { Snap $Shot; Check (Test-Path $Shot) 'screenshot taken' }
catch { Ng "screenshot failed: $($_.Exception.Message)" }

if (CloseProcess $app) { Ok 'installed exe exited on close' }
else {
    Ng 'installed exe did not exit on close'
    Get-Process -Id $app.Id -ErrorAction SilentlyContinue | Stop-Process -Force
}

$lines = @(Get-Content $Log -ErrorAction SilentlyContinue)
Check ($lines.Count -gt 5) "boot log captured ($($lines.Count) lines)"
$logText = ($lines -join "`n")
Check ($logText -match 'loaders registered=4 skipped=1') 'loaders registered=4 skipped=1'
Check ($logText -match 'settings: loaded=') 'settings line present'
Check ($logText -notmatch 'FATAL EXCEPTION|Access violation') 'no fatal exception'

"--- uninstall (silent) ---"
$r2 = MsiOp '/x' 180
Check $r2.Done 'msiexec uninstall completed'
Check ($r2.Code -eq 0) "uninstall exit code = $($r2.Code)"
Start-Sleep -Seconds 3

"--- cleanup ---"
Check (-not (Test-Path $Exe)) 'exe removed'
Check (-not (Test-Path $Install)) 'install dir removed'
Check (-not (Test-Path $sm)) 'start menu shortcut removed'
Check (-not (Test-Path $dk)) 'desktop shortcut removed'
Check (-not (Test-Path "HKCU:\Software\Classes\$ProgId")) 'progid removed'
Check (-not (Test-Path 'HKCU:\Software\NanoAlphaPlayer\Capabilities')) 'capabilities removed'
$ra2 = (Get-ItemProperty 'HKCU:\Software\RegisteredApplications' -ErrorAction SilentlyContinue).NanoAlphaPlayer
Check ($null -eq $ra2) 'RegisteredApplications entry removed'
$v2 = (Get-ItemProperty 'HKCU:\Software\Classes\.mp4' -ErrorAction SilentlyContinue).'(default)'
Check ($v2 -ne $ProgId) ".mp4 association released (now = $v2)"
$owp2 = Get-Item 'HKCU:\Software\Classes\.mp4\OpenWithProgids' -ErrorAction SilentlyContinue
Check ($null -eq $owp2 -or ($owp2.GetValueNames() -notcontains $ProgId)) '.mp4 OpenWithProgids released'
Check ($null -eq (FindArp $UniHKCU)) 'ARP entry removed'
Check ($null -eq (FindArp $UniHKLM)) 'no all-users ARP entry left behind'

"===== RESULT: $($fail.Count) failure(s) ====="
if ($fail.Count -gt 0) { $fail | ForEach-Object { "  - $_" }; exit 1 }
"All installer checks passed."
exit 0
