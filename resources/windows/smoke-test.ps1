<#
  Installs a freshly built ASTRA setup .exe the way users' machines do and
  checks that what it leaves behind starts. CI runs this before the installer
  is attached to a release (.github/workflows/build-windows.yml), because a
  setup .exe that compiles says nothing about whether it installs.

      smoke-test.ps1 -Setup <new setup.exe> [-Variants x86-64-v3,x86-64]
                     [-UpgradeFrom <older setup.exe>,...] [-ExpectVersion 0.10.1]

  1. A fresh install with the installer's own choice of build.
  2. Every name in -Variants installed over it with /VARIANT=<name>.
  3. For every older installer in -UpgradeFrom: install it, then run the new
     setup through the hand-over script an installed ASTRA writes for an in-app
     update (launchWindowsInstaller in src/app/ui/UpdateManager.cpp) and wait
     for ASTRA to come back. This is the path every existing installation
     takes, so the new installer must keep working with those exact arguments.

  After each install ASTRA.exe has to run a headless command to its end and
  bring up its main window. PATH is cut down to Windows itself first, so a DLL
  missing from the package is not found in the build environment instead.
#>
param(
    [Parameter(Mandatory)][string]$Setup,
    [string[]]$Variants = @(),
    [string[]]$UpgradeFrom = @(),
    [string]$ExpectVersion = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Setup = (Resolve-Path -LiteralPath $Setup).Path
# Not GetTempPath(): on a CI runner that is an 8.3 short path, which never
# compares equal to the path a running process reports for its image.
$tempRoot = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [IO.Path]::GetTempPath() }
$Work  = Join-Path $tempRoot ('astra-smoke-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force -Path $Work | Out-Null
$env:PATH = @("$env:SystemRoot\System32", "$env:SystemRoot", "$env:SystemRoot\System32\Wbem",
              "$env:SystemRoot\System32\WindowsPowerShell\v1.0") -join ';'
$script:Step = 0

function Fail([string]$Message) {
    Write-Host "::error::$Message"
    throw $Message
}

function Show-Tail([string]$Path, [int]$Lines = 40) {
    if (Test-Path -LiteralPath $Path) {
        Write-Host "---- last lines of $Path"
        Get-Content -LiteralPath $Path -Tail $Lines | ForEach-Object { Write-Host "    $_" }
    }
}

# Starts a program and waits for that process alone. Start-Process -Wait would
# also wait for everything it starts, and the update script starts ASTRA.
function Invoke-Program([string]$File, [string[]]$Arguments, [int]$TimeoutSec = 900,
                        [string]$StdOut = '', [string]$StdErr = '') {
    $opts = @{ FilePath = $File; PassThru = $true }
    if ($Arguments.Count -gt 0) { $opts.ArgumentList = $Arguments }
    if ($StdOut) { $opts.RedirectStandardOutput = $StdOut }
    if ($StdErr) { $opts.RedirectStandardError = $StdErr }
    $p = Start-Process @opts
    $null = $p.Handle    # without this the exit code is lost once the process ends
    if (-not $p.WaitForExit($TimeoutSec * 1000)) {
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
        Fail "$File did not finish within $TimeoutSec s"
    }
    return $p.ExitCode
}

function Install-Setup([string]$Exe, [string]$Dir, [string]$Log, [string[]]$Extra = @()) {
    $a = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/DIR=`"$Dir`"", "/LOG=`"$Log`"") + $Extra
    $rc = Invoke-Program $Exe $a
    if ($rc -ne 0) {
        Show-Tail $Log
        Fail "$(Split-Path -Leaf $Exe) $($Extra -join ' ') exited with $rc"
    }
}

function Uninstall([string]$Dir) {
    $u = Join-Path $Dir 'unins000.exe'
    if (-not (Test-Path -LiteralPath $u)) { Fail "no uninstaller in $Dir" }
    $rc = Invoke-Program $u @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART')
    if ($rc -ne 0) { Fail "the uninstaller in $Dir exited with $rc" }
    # The uninstaller finishes from a copy of itself; give that a moment
    # before the next install registers under the same application id.
    for ($i = 0; $i -lt 60 -and (Test-Path -LiteralPath $u); $i++) { Start-Sleep -Seconds 1 }
}

# The build the installer says it chose, from its log.
function Get-ChosenVariant([string]$Log) {
    $m = Select-String -LiteralPath $Log -Pattern 'Installing the (\S+) build' | Select-Object -Last 1
    if (-not $m) {
        Show-Tail $Log
        Fail "the installer log does not name the build it installed"
    }
    return $m.Matches[0].Groups[1].Value
}

function Get-ExeVersion([string]$Exe) {
    $vi = (Get-Item -LiteralPath $Exe).VersionInfo
    return '{0}.{1}.{2}' -f $vi.FileMajorPart, $vi.FileMinorPart, $vi.FileBuildPart
}

function Assert-Version([string]$Exe) {
    if (-not $ExpectVersion) { return }
    $want = @($ExpectVersion.Split('.')) + @('0', '0', '0')
    $want = ($want[0..2] | ForEach-Object { [int]$_ }) -join '.'
    $have = Get-ExeVersion $Exe
    if ($have -ne $want) { Fail "$Exe is version $have, expected $want" }
}

# --validate-plan needs no database and no window, and ends on its own: it
# shows that the executable loads with every DLL it imports.
function Test-Headless([string]$Exe) {
    $script:Step++
    $plan = Join-Path $Work "plan-$($script:Step).json"
    $out  = Join-Path $Work "validate-$($script:Step).out"
    $err  = Join-Path $Work "validate-$($script:Step).err"
    Set-Content -LiteralPath $plan -Value '{"name":"smoke"}' -Encoding ascii
    $rc = Invoke-Program $Exe @('--validate-plan', "`"$plan`"") 120 $out $err
    $text = if (Test-Path -LiteralPath $out) { Get-Content -LiteralPath $out -Raw } else { '' }
    if ("$text" -notmatch 'mode\(s\)') {
        Show-Tail $err
        Fail "$Exe --validate-plan exited with $rc ($('0x{0:X8}' -f $rc)) and printed: $text"
    }
}

# Waits for a running ASTRA to report its main window in its log, then ends it.
function Wait-MainWindow([Diagnostics.Process]$Process, [string]$DataDir) {
    $logs = Join-Path $DataDir 'logs'
    $deadline = (Get-Date).AddSeconds(180)
    $shown = $false
    while (-not $shown -and (Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 2
        if (Test-Path -LiteralPath $logs) {
            $shown = [bool](Select-String -Path (Join-Path $logs '*.log') -Pattern 'Main window displayed' `
                                          -Quiet -ErrorAction SilentlyContinue)
        }
        if (-not $shown -and $Process.HasExited) {
            Get-ChildItem -Path $logs -Filter *.log -ErrorAction SilentlyContinue | ForEach-Object { Show-Tail $_.FullName }
            $code = try { '0x{0:X8}' -f $Process.ExitCode } catch { 'unknown' }
            Fail "ASTRA ended before its main window appeared (exit code $code)"
        }
    }
    if (-not $Process.HasExited) { Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue }
    if (-not $shown) {
        Get-ChildItem -Path $logs -Filter *.log -ErrorAction SilentlyContinue | ForEach-Object { Show-Tail $_.FullName }
        Fail "ASTRA did not bring up its main window within 180 s"
    }
    $Process.WaitForExit(30000) | Out-Null
}

# A scratch data directory, so the run does not depend on what an earlier one
# left behind and leaves nothing in the user's own.
function New-DataDir {
    $script:Step++
    $d = Join-Path $Work "data-$($script:Step)"
    New-Item -ItemType Directory -Force -Path $d | Out-Null
    $env:ASTRA_DATA_DIR = $d
    return $d
}

function Test-Window([string]$Exe) {
    $data = New-DataDir
    $p = Start-Process -FilePath $Exe -PassThru
    $null = $p.Handle
    Wait-MainWindow $p $data
}

function Test-Installed([string]$Dir) {
    $exe = Join-Path $Dir 'bin\ASTRA.exe'
    if (-not (Test-Path -LiteralPath $exe)) { Fail "the installer left no $exe" }
    Assert-Version $exe
    Test-Headless $exe
    Test-Window $exe
}

# -- 1. Fresh install ---------------------------------------------------------
$dir = Join-Path $Work 'fresh'
$log = Join-Path $Work 'install-fresh.log'
Write-Host ">>> Fresh install of $(Split-Path -Leaf $Setup)"
Install-Setup $Setup $dir $log
$auto = Get-ChosenVariant $log
if ($Variants.Count -gt 0 -and $Variants -notcontains $auto) {
    Fail "the installer chose `"$auto`", which is not one of: $($Variants -join ', ')"
}
Test-Installed $dir
Write-Host "    installed the $auto build; it runs and shows its window"

# -- 2. Every build, installed over the previous one --------------------------
foreach ($v in $Variants) {
    if ($v -eq $auto -and $Variants.Count -gt 1) { continue }   # just tested
    $log = Join-Path $Work "install-$v.log"
    Write-Host ">>> Reinstall with /VARIANT=$v"
    Install-Setup $Setup $dir $log @("/VARIANT=$v")
    $got = Get-ChosenVariant $log
    if ($got -ne $v) { Fail "/VARIANT=$v installed the $got build" }
    Test-Installed $dir
    Write-Host "    the $v build runs and shows its window"
}
if (Test-Path -LiteralPath (Join-Path $dir 'unins001.exe')) {
    Fail "reinstalling registered a second copy (unins001.exe) instead of replacing the first"
}
Uninstall $dir

# -- 3. In-app update from older releases -------------------------------------
$n = 0
foreach ($old in $UpgradeFrom) {
    $n++
    $old = (Resolve-Path -LiteralPath $old).Path
    $dir = Join-Path $Work "upgrade-$n"
    $exe = Join-Path $dir 'bin\ASTRA.exe'
    Write-Host ">>> In-app update from $(Split-Path -Leaf $old)"
    Install-Setup $old $dir (Join-Path $Work "install-old-$n.log")
    if (-not (Test-Path -LiteralPath $exe)) { Fail "$(Split-Path -Leaf $old) left no $exe" }
    $oldHash = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash

    # The script launchWindowsInstaller writes, line for line. Only /LOG is
    # added, to have something to show when it fails. It deletes the setup it
    # ran, so it gets a copy.
    $copy = Join-Path $Work "update-$n.exe"
    $log  = Join-Path $Work "update-$n.log"
    $cmd  = Join-Path $Work "astra-update-$n.cmd"
    Copy-Item -LiteralPath $Setup -Destination $copy
    $lines = @(
        '@echo off',
        'cd /d "%TEMP%"',
        "`"$copy`" /SILENT /SUPPRESSMSGBOXES /NORESTART /NOCANCEL /CLOSEAPPLICATIONS /DIR=`"$dir`" /LOG=`"$log`"",
        "if exist `"$exe`" start `"`" `"$exe`"",
        "del `"$copy`" >nul 2>&1",
        'del "%~f0"'
    )
    [IO.File]::WriteAllText($cmd, ($lines -join "`r`n") + "`r`n", [Text.Encoding]::ASCII)

    $data = New-DataDir
    $rc = Invoke-Program "$env:SystemRoot\System32\cmd.exe" @('/c', "`"$cmd`"")
    if (-not (Test-Path -LiteralPath $log)) { Fail "the update script (exit code $rc) never ran the installer" }
    $got = Get-ChosenVariant $log
    if ((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash -eq $oldHash) {
        Show-Tail $log
        Fail "the update left the old ASTRA.exe in place"
    }
    Assert-Version $exe
    if (Test-Path -LiteralPath (Join-Path $dir 'unins001.exe')) {
        Fail "the update registered a second copy (unins001.exe) instead of replacing the old one"
    }
    if (Test-Path -LiteralPath $copy) { Fail "the update script did not finish (it deletes $copy at its end)" }

    # ASTRA comes back by itself: the script starts it once the installer is done.
    $back = $null
    $deadline = (Get-Date).AddSeconds(60)
    while (-not $back -and (Get-Date) -lt $deadline) {
        $back = Get-Process -Name ASTRA -ErrorAction SilentlyContinue |
                Where-Object { $_.Path -eq $exe } | Select-Object -First 1
        if (-not $back) { Start-Sleep -Seconds 1 }
    }
    if (-not $back) { Fail "ASTRA was not started again after the update" }
    Wait-MainWindow $back $data
    Test-Headless $exe
    Write-Host "    updated to the $got build; ASTRA came back and shows its window"
    Uninstall $dir
}

Write-Host ">>> Installer smoke test passed"
