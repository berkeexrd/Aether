# Aether - tam temizlik / Full clean-up
# Bu bilgisayardaki Aether kurulumunu tamamen kaldırır ve her şeyi ilk haline döndürür:
#   - çalışan tüm Aether programları kapatılır
#   - Windows kabuğu (Winlogon Shell), klasör bağlantıları, başlangıç kayıtları ve zamanlanmış görev geri alınır
#   - Windows görev çubuğu (otomatik gizle dahil) ve duvar kağıdı eski haline döner
#   - program klasörleri, ayarlar, günlükler, kısayollar silinir
# This removes every trace of an Aether install on this PC. Run: tools\aether-clean.bat   (add -Force to skip the question)
param([switch]$Force)
$ErrorActionPreference = 'Continue'
$tr = (Get-UICulture).TwoLetterISOLanguageName -eq 'tr'
function T($a, $b) { if ($tr) { $a } else { $b } }
function Say($msg) { Write-Host "  $msg" }

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class AetherWin {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int left, top, right, bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct APPBARDATA { public int cbSize; public IntPtr hWnd; public uint uCallbackMessage; public uint uEdge; public RECT rc; public IntPtr lParam; }
    [DllImport("shell32.dll")] public static extern UIntPtr SHAppBarMessage(uint msg, ref APPBARDATA d);
    [DllImport("shell32.dll")] public static extern void SHChangeNotify(int ev, uint flags, IntPtr a, IntPtr b);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string cls, string title);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern IntPtr GetShellWindow();
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern bool SystemParametersInfo(uint action, uint p, string v, uint flags);
}
'@

$appdata = Join-Path $env:APPDATA 'Aether'
$local   = Join-Path $env:LOCALAPPDATA 'Aether'
$uninst  = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\Aether'
$winlogon = 'HKCU:\Software\Microsoft\Windows NT\CurrentVersion\Winlogon'
$installDirs = @((Join-Path $env:LOCALAPPDATA 'Programs\Aether'), $local)
try { $loc = (Get-ItemProperty $uninst -ErrorAction Stop).InstallLocation; if ($loc) { $installDirs += $loc.TrimEnd('\') } } catch {}
$installDirs = $installDirs | Where-Object { $_ } | Select-Object -Unique

Write-Host ''
Write-Host (T 'Aether tam temizlik' 'Aether full clean-up') -ForegroundColor Cyan
Write-Host (T 'Şunlar silinecek:' 'This will remove:')
$installDirs | ForEach-Object { Say $_ }
Say $appdata
Say (T 'Başlat menüsü / masaüstü kısayolları, başlangıç kayıtları, zamanlanmış görev "AetherBar"' 'Start menu / desktop shortcuts, startup entries, the "AetherBar" scheduled task')
Write-Host (T 'Ayarların da silinir. Windows görev çubuğu ve duvar kağıdın eski haline döner.' 'Your Aether settings are deleted too. The Windows taskbar and your wallpaper are restored.')
if (-not $Force) {
    $ans = Read-Host (T 'Devam edilsin mi? (e/h)' 'Continue? (y/n)')
    if ($ans -notmatch '^(e|E|y|Y)') { Write-Host (T 'Vazgeçildi.' 'Cancelled.'); exit 0 }
}
Write-Host ''

# 1. stop every Aether program (the shell first, so it cannot restart the others)
Say (T '1/7 Çalışan programlar kapatılıyor...' '1/7 Stopping running programs...')
foreach ($n in 'AetherShell','AetherBar','AetherDesktop','Aether','AetherExplorer','AetherBin','AetherIntro','AetherSetup','aether-uninstall','Uninstall') {
    Get-Process -Name $n -ErrorAction SilentlyContinue | Where-Object { $_.Path -like '*Aether*' -or $n -ne 'Uninstall' } | Stop-Process -Force -ErrorAction SilentlyContinue
}
Start-Sleep -Milliseconds 600

# 2. Windows shell, folder associations, startup entries
Say (T '2/7 Windows kabuğu ve kayıt defteri geri alınıyor...' '2/7 Restoring the Windows shell and registry...')
$prev = $null
$bk = Join-Path $appdata 'shell-backup.json'
if (Test-Path $bk) { try { $prev = (Get-Content $bk -Raw | ConvertFrom-Json).previousShell } catch {} }
$curShell = (Get-ItemProperty $winlogon -ErrorAction SilentlyContinue).Shell
if ($curShell -and $curShell -like '*Aether*') {
    if ($prev) { Set-ItemProperty -Path $winlogon -Name Shell -Value $prev } else { Remove-ItemProperty -Path $winlogon -Name Shell -ErrorAction SilentlyContinue }
}
foreach ($k in 'Directory','Folder','Drive') {
    $key = "HKCU:\Software\Classes\$k\shell\open"
    $cmd = (Get-ItemProperty "$key\command" -ErrorAction SilentlyContinue).'(default)'
    if ($cmd -and $cmd -like '*Aether*') { Remove-Item $key -Recurse -Force -ErrorAction SilentlyContinue }
}
Remove-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name 'Aether' -ErrorAction SilentlyContinue
Remove-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\RunOnce' -Name 'AetherTrialCleanup' -ErrorAction SilentlyContinue
Remove-Item $uninst -Recurse -Force -ErrorAction SilentlyContinue
[AetherWin]::SHChangeNotify(0x08000000, 0, [IntPtr]::Zero, [IntPtr]::Zero)

# 3. scheduled task (needs administrator rights once)
Say (T '3/7 Zamanlanmış görev siliniyor...' '3/7 Removing the scheduled task...')
schtasks.exe /Query /TN AetherBar *> $null
if ($LASTEXITCODE -eq 0) {
    schtasks.exe /Delete /TN AetherBar /F *> $null
    if ($LASTEXITCODE -ne 0) {
        try { Start-Process schtasks.exe -ArgumentList '/Delete','/TN','AetherBar','/F' -Verb RunAs -WindowStyle Hidden -Wait } catch { Say (T 'Görev silinemedi (yönetici izni verilmedi).' 'Could not delete the task (administrator permission was not given).') }
    }
}

# 4. Windows taskbar back as it was
Say (T '4/7 Windows görev çubuğu geri getiriliyor...' '4/7 Restoring the Windows taskbar...')
$state = 2                                      # ABS_ALWAYSONTOP: visible, no auto-hide (Windows default) when Aether left no record of the original
$sf = Join-Path $appdata 'taskbar-state.txt'
if (Test-Path $sf) { try { $state = [int](Get-Content $sf -Raw).Trim() } catch {} }
$tray = [AetherWin]::FindWindow('Shell_TrayWnd', $null)
if ($tray -ne [IntPtr]::Zero) {
    $abd = New-Object AetherWin+APPBARDATA
    $abd.cbSize = [Runtime.InteropServices.Marshal]::SizeOf($abd); $abd.hWnd = $tray; $abd.lParam = [IntPtr]$state
    [void][AetherWin]::SHAppBarMessage(0x0A, [ref]$abd)       # ABM_SETSTATE
    [void][AetherWin]::ShowWindow($tray, 8)                    # SW_SHOWNA
    $sec = [IntPtr]::Zero
    while (($sec = [AetherWin]::FindWindowEx([IntPtr]::Zero, $sec, 'Shell_SecondaryTrayWnd', $null)) -ne [IntPtr]::Zero) { [void][AetherWin]::ShowWindow($sec, 8) }
}

# 5. wallpaper
Say (T '5/7 Duvar kağıdı geri yükleniyor...' '5/7 Restoring the wallpaper...')
$ow = Join-Path $appdata 'orig_wallpaper.txt'
$restored = $false
if (Test-Path $ow) {
    try {
        $orig = [Text.Encoding]::Unicode.GetString([IO.File]::ReadAllBytes($ow)).Trim([char]0, ' ', "`r", "`n")
        $os = Join-Path $appdata 'orig_wallpaper_style.txt'
        if (Test-Path $os) {
            $parts = ([Text.Encoding]::Unicode.GetString([IO.File]::ReadAllBytes($os)).Trim([char]0)).Split('|')
            if ($parts.Count -eq 2) {
                if ($parts[0]) { Set-ItemProperty 'HKCU:\Control Panel\Desktop' -Name WallpaperStyle -Value $parts[0] }
                if ($parts[1]) { Set-ItemProperty 'HKCU:\Control Panel\Desktop' -Name TileWallpaper -Value $parts[1] }
            }
        }
        if ($orig) { [void][AetherWin]::SystemParametersInfo(0x14, 0, $orig, 3); $restored = $true }
    } catch {}
}
if (-not $restored) {
    $curWp = (Get-ItemProperty 'HKCU:\Control Panel\Desktop').WallPaper
    if ($curWp -and $curWp -like '*Aether*') {
        $def = Join-Path $env:WINDIR 'Web\Wallpaper\Windows\img0.jpg'
        [void][AetherWin]::SystemParametersInfo(0x14, 0, $(if (Test-Path $def) { $def } else { '' }), 3)
    }
}

# 6. files, settings, shortcuts
Say (T '6/7 Dosyalar, ayarlar ve kısayollar siliniyor...' '6/7 Deleting files, settings and shortcuts...')
$programs = [Environment]::GetFolderPath('Programs'); $desktop = [Environment]::GetFolderPath('Desktop')
$targets = @($installDirs) + @($appdata, (Join-Path $programs 'Aether'), (Join-Path $desktop 'Aether.lnk'), (Join-Path $env:TEMP 'aether_wpn'), (Join-Path $env:TEMP 'aether-uninstall.exe'), (Join-Path $env:TEMP 'aether-payload.zip'))
foreach ($t in $targets) { if ($t -and (Test-Path $t)) { Remove-Item $t -Recurse -Force -ErrorAction SilentlyContinue; if (Test-Path $t) { Say ((T 'Silinemedi: ' 'Could not delete: ') + $t) } } }

# 7. explorer
Say (T '7/7 Tamamlanıyor...' '7/7 Finishing...')
if ([AetherWin]::GetShellWindow() -eq [IntPtr]::Zero) { Start-Process (Join-Path $env:WINDIR 'explorer.exe') }

Write-Host ''
Write-Host (T 'Bitti. Aether tamamen kaldırıldı; temiz kurulum için yeni AetherSetup.exe çalıştırabilirsin.' 'Done. Aether is completely removed; run a fresh AetherSetup.exe for a clean install.') -ForegroundColor Green
