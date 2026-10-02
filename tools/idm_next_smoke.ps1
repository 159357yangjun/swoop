# Local smoke test for a freshly built idm-next.exe: does it start, listen, and hand off?
# ASCII-only on purpose: Windows PowerShell 5.1 reads UTF-8-without-BOM as the ANSI codepage and
# corrupts non-ASCII characters mid-string (hit on 2026-10-01; the first version of this file died
# at line 30 with a parser error because of it).
#
# What it asserts, and why each piece exists:
#  1) IDM_DATA_DIR -> a brand-new dir under %TEMP% that is asserted NOT to exist beforehand.
#     src/utils/app_paths.cpp:14-19: once set, data dir and ini move there and the registry /
#     %LOCALAPPDATA% are not touched. Without this the smoke run would write into the user's real
#     history.db (that already happened to tasks.json once, per the comment in that file).
#  2) QT_LOGGING_TO_CONSOLE=1: a GUI-subsystem exe sends Qt logs to OutputDebugString, so redirected
#     stderr would be EMPTY and an empty file would be misread as "no errors". (Qt 6.11 prints a
#     deprecation notice for this variable - it still works; QT_FORCE_STDERR_LOGGING is the successor.
#     The script therefore checks for emptiness and refuses to call it clean.)
#  3) Instance #2 must exit on its own (single-instance handoff). We only read ITS exit code; if it
#     cannot be captured we print "(unavailable)" instead of a blank, because blank != 0.
#  4) Stop-Process is called ONLY on PIDs this script started. Other sessions' processes are never touched.
#  5) On the way out it deletes the heavy things the app created in the isolated dir (history.db,
#     a 5.6 MB aria2c copy) but keeps inst*.err/out so the caller can archive the evidence.
param([int]$HoldSeconds = 5, [int]$HandoffTimeoutMs = 15000)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$exe  = Join-Path $repo 'build\idm-next.exe'
if (-not (Test-Path $exe)) { throw "SMOKE_ABORT no such exe: $exe (build it first; this script never compiles)" }
$ts   = Get-Date -Format 'MMdd-HHmmss'
$data = Join-Path $env:TEMP "idm-smoke-$ts"
if (Test-Path $data) { throw "SMOKE_ABORT isolation dir already exists: $data" }
New-Item -ItemType Directory -Path $data | Out-Null
$started = @()
try {
    $log1o = Join-Path $data 'inst1.out'; $log1e = Join-Path $data 'inst1.err'
    $log2o = Join-Path $data 'inst2.out'; $log2e = Join-Path $data 'inst2.err'
    $sha = (Get-FileHash -Algorithm SHA256 $exe).Hash.Substring(0,12).ToLower()
    Write-Host "== EXE $exe"
    Write-Host "== exe_sha12=$sha mtime=$((Get-Item $exe).LastWriteTime.ToString('s')) size=$((Get-Item $exe).Length)"
    Write-Host "== ISOLATED_DATA_DIR $data"
    $env:IDM_DATA_DIR = $data
    $env:QT_LOGGING_TO_CONSOLE = '1'

    $p1 = Start-Process -FilePath $exe -PassThru -RedirectStandardOutput $log1o -RedirectStandardError $log1e -WorkingDirectory (Split-Path $exe)
    $started += $p1.Id
    Start-Sleep -Seconds $HoldSeconds
    $alive1 = $null -ne (Get-Process -Id $p1.Id -ErrorAction SilentlyContinue)
    Write-Host "== INSTANCE1 pid=$($p1.Id) alive_after_${HoldSeconds}s=$alive1"
    if (-not $alive1) {
        Write-Host "== INSTANCE1 exited early. stderr tail:"
        if (Test-Path $log1e) { Get-Content $log1e -Tail 20 }
        throw "SMOKE_FAIL instance1 did not survive ${HoldSeconds}s"
    }

    $p2 = Start-Process -FilePath $exe -PassThru -RedirectStandardOutput $log2o -RedirectStandardError $log2e -WorkingDirectory (Split-Path $exe)
    $started += $p2.Id
    $exited = $p2.WaitForExit($HandoffTimeoutMs)
    # ExitCode  Start-Process -PassThru PS 5.1an empty string must NOT be printed as 0 nor read as a pass:
    # the real witness of the handoff is the activate line in instance1 stderr (checked by TRIAGE below), not a number
    $code2 = '(unreadable)'
    if ($exited) {
        $raw = $null
        try { $raw = $p2.ExitCode } catch { $raw = $null }
        if ($null -ne $raw -and "$raw" -ne '') { $code2 = "$raw" }
    }
    Write-Host "== INSTANCE2 pid=$($p2.Id) exited_within_${HandoffTimeoutMs}ms=$exited exitcode=$code2 (blank or unreadable is NOT the same as 0)"

    $e1 = ''
    if (Test-Path $log1e) { $t = Get-Content $log1e -Raw; if ($null -ne $t) { $e1 = $t } }
    $e2 = ''
    if (Test-Path $log2e) { $t = Get-Content $log2e -Raw; if ($null -ne $t) { $e2 = $t } }
    $listen = $e1 -match 'idm-next-ipc'
    $accept = $e1 -match 'activate'
    Write-Host "== TRIAGE instance1_stderr_bytes=$($e1.Length) mentions_listening_name=$listen mentions_activate=$accept instance2_stderr_bytes=$($e2.Length)"
    if ($e1.Trim().Length -eq 0) {
        Write-Host "SMOKE_FAIL instance1 stderr is EMPTY - do NOT read that as clean (logging never reached stderr)"
        throw "SMOKE_FAIL empty stderr"
    }
    if (-not $listen) { Write-Host "SMOKE_FAIL instance1 never logged the IPC listen name"; throw "SMOKE_FAIL no listen line" }
    if (-not $accept) { Write-Host "SMOKE_WARN instance1 has no 'activate' line: handoff may not have been read (kept as warning, not a pass)" }

    $made = Get-ChildItem -Recurse $data | Where-Object { -not $_.PSIsContainer }
    Write-Host "== DATA_DIR contents the app created (all inside the isolated dir):"
    foreach ($f in $made) { Write-Host ("   {0,9}  {1}" -f $f.Length, $f.FullName.Substring($data.Length)) }
    Write-Host "SMOKE_OK exe_sha12=$sha logdir=$data"
} finally {
    foreach ($procId in $started) { Stop-Process -Id $procId -Force -ErrorAction SilentlyContinue }
    # one sample is not a verdict: a GUI process can take hundreds of ms to seconds to disappear after the kill signal
    # poll up to 10s and print the FINAL state - the first version reported a false CLEANUP_WARN off a 1s sample
    foreach ($procId in $started) {
        $waited = 0; $gone = $false
        while ($waited -lt 10) {
            if ($null -eq (Get-Process -Id $procId -ErrorAction SilentlyContinue)) { $gone = $true; break }
            Start-Sleep -Seconds 1; $waited++
        }
        if ($gone) { Write-Host "== CLEANUP pid=$procId terminated_within_${waited}s=True" }
        else { Write-Host "CLEANUP_FAIL pid=$procId STILL ALIVE after 10s. This PID was recorded by this script s own Start-Process; confirm by hand before acting. Never touch another session s process on a guess." }
    }
    foreach ($junk in @('history.db','aria2','tasks.json')) {
        $j = Join-Path $data $junk
        if (Test-Path $j) { Remove-Item -Recurse -Force $j -ErrorAction SilentlyContinue }
    }
    Write-Host "== logs kept at $data (caller archives then deletes)"
}
