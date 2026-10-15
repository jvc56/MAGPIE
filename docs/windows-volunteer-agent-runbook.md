# MAGPIE on Windows: runbook for a volunteer's Claude agent

## To the volunteer

Thank you for checking MAGPIE on Windows. Give this file to your Claude agent (Claude
Code, or any Claude agent that can run PowerShell on this PC) and ask it to follow it.

It takes about 45 minutes. It downloads about 400 MB and needs about 2 GB of free disk
space. A few steps need you, because an agent can't double-click or see the screen. The
agent will tell you when.

At the end you'll have a zip file, `magpie-windows-report.zip`, to send back. It
contains no user name, machine name or personal files: the agent removes those, and you
can look inside before sending.

## To the agent: ground rules

Follow these for the whole run.

- **Stay in one folder.** Work only inside `$W` (defined in step 0). Don't change files
  anywhere else.
- **Leave the machine as it is:**
  - don't run as administrator;
  - don't change Windows Defender, SmartScreen, antivirus or firewall settings, and
    don't add exclusions;
  - don't change any system setting.
- **Ask before installing anything.** The only thing you may install is the GitHub CLI
  (`gh`), and only if step 1 needs it and the volunteer agrees.
- **Don't push the run through.** If a step fails, record the full output in the report,
  mark the step **FAIL**, and carry on with the next step if it doesn't depend on the one
  that failed. Don't retry with different options to make it pass, and don't debug
  MAGPIE itself: the maintainers will.
- **Record exactly.** Paste command output verbatim (from the files the steps write).
  Don't summarise it.
- **When a step says "ask the volunteer":**
  - stop and ask in plain words;
  - wait for the answer;
  - record it verbatim in `human-checks.md`;
  - save any screenshot they give you into `$R\screenshots\`.
- **Use Windows PowerShell 5.1 or PowerShell 7.** Run the blocks below as given. Each
  block assumes the variables from step 0 are set; if you start a new shell, run step 0
  again.

## Parameters (the maintainer fills these in before sending)

| Name | Value |
|---|---|
| `SOURCE` | `release` or `ci` |
| `RELEASE_TAG` | for `release`, e.g. `v0.1.3` |
| `CI_RUN_ID` | for `ci`, e.g. `38668791241` (artifacts are kept 7 days) |
| `MAGPIE_VERSION` | `0.1.3` |
| `SEND_TO` | where the volunteer sends the zip |

## Step 0. Set up

Set the parameters from the table above, then the folders:

```powershell
$SOURCE = 'release'            # or 'ci'
$RELEASE_TAG = 'v0.1.3'        # for release
$CI_RUN_ID = '38668791241'     # for ci
$MAGPIE_VERSION = '0.1.3'

# Some agents' shells set this, which stops cmd from finding magpie.exe in the
# current folder. This clears it for this shell only.
Remove-Item Env:NoDefaultCurrentDirectoryInExePath -ErrorAction SilentlyContinue

$W = Join-Path $HOME 'magpie-windows-check'   # everything happens in here
$R = Join-Path $W 'report'                    # everything to send goes in here
New-Item -ItemType Directory -Force $W, $R, (Join-Path $R 'screenshots') | Out-Null
Set-Location $W
"started $(Get-Date -Format o)" | Set-Content -Encoding utf8 (Join-Path $R 'timeline.txt')
```

Create `$R\human-checks.md` with the heading `# Answers from the volunteer`. Create
`$R\REPORT.md` from the template in the last section, and fill it in as you go.

## Step 1. System facts

```powershell
$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
$os  = Get-CimInstance Win32_OperatingSystem
$cs  = Get-CimInstance Win32_ComputerSystem
$arch = switch ($cpu.Architecture) { 9 {'x64'} 12 {'arm64'} default {"other($($cpu.Architecture))"} }
$acp = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Nls\CodePage').ACP
$longPaths = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem' -ErrorAction SilentlyContinue).LongPathsEnabled
$av = (Get-CimInstance -Namespace root/SecurityCenter2 -ClassName AntivirusProduct -ErrorAction SilentlyContinue).displayName -join ', '
$fs = (Get-Volume -DriveLetter $W.Substring(0,1) -ErrorAction SilentlyContinue).FileSystem
$free = [math]::Round((Get-PSDrive $W.Substring(0,1)).Free / 1GB, 1)
@"
os_caption: $($os.Caption)
os_version: $($os.Version)
os_build: $($os.BuildNumber)
cpu: $($cpu.Name.Trim())
architecture: $arch
logical_processors: $($cs.NumberOfLogicalProcessors)
memory_gb: $([math]::Round($cs.TotalPhysicalMemory / 1GB, 1))
ansi_code_page: $acp
long_paths_enabled: $longPaths
antivirus: $av
work_drive_filesystem: $fs
work_drive_free_gb: $free
user_name_has_non_ascii: $($env:USERNAME -match '[^\x00-\x7F]')
home_path_has_spaces: $($HOME -match ' ')
home_in_onedrive: $($HOME -match 'OneDrive')
powershell: $($PSVersionTable.PSVersion)
"@ | Set-Content -Encoding utf8 (Join-Path $R 'system.txt')
Get-Content (Join-Path $R 'system.txt')
```

Requirements:
- Build 18362 (Windows 10 1903) or later. If the build number is lower, stop: MAGPIE
  doesn't support that version. Write the report anyway.
- At least 2 GB free. If there is less, stop and tell the volunteer.

Set the variables later steps use:

```powershell
$Name = if ($arch -eq 'arm64') { 'arm64' } else { 'x64' }
$ExpectedTarget = if ($arch -eq 'arm64') { 'armv8-a' } else { 'nehalem' }
$Threads = [math]::Min([int]$cs.NumberOfLogicalProcessors, 8)
if ($cs.TotalPhysicalMemory -lt 8GB) { $Threads = [math]::Min($Threads, 4) }
```

## Step 2. Get MAGPIE

Put the executable at `$W\dist\magpie.exe`. For `SOURCE = ci`, also put
`$W\dist\magpie_test.exe` there.

**`SOURCE = release`:**

```powershell
$dist = Join-Path $W 'dist'; New-Item -ItemType Directory -Force $dist | Out-Null
$base = "https://github.com/jvc56/MAGPIE/releases/download/$RELEASE_TAG"
$exe  = "magpie-$MAGPIE_VERSION-windows-$Name.exe"
Invoke-WebRequest "$base/$exe" -OutFile (Join-Path $dist $exe)
Invoke-WebRequest "$base/$exe.sha256" -OutFile (Join-Path $dist "$exe.sha256")
$expected = ((Get-Content (Join-Path $dist "$exe.sha256")) -split '\s+')[0].ToLower()
$actual = (Get-FileHash (Join-Path $dist $exe) -Algorithm SHA256).Hash.ToLower()
"expected $expected`nactual   $actual`nmatch    $([bool]$expected -and $expected -eq $actual)" |
  Set-Content -Encoding utf8 (Join-Path $R '02-download-hash.txt')
Copy-Item (Join-Path $dist $exe) (Join-Path $dist 'magpie.exe')
```

A hash mismatch is a **FAIL**. Stop and report it: don't run a file that doesn't match.

**`SOURCE = ci`:** this needs the GitHub CLI, logged in, because GitHub requires a login
to download build artifacts. If `gh` isn't installed, ask the volunteer whether you may
install it (`winget install GitHub.cli`) and whether they will log in (`gh auth
login`). If they say no, ask them to download the artifact `windows-x86_64` (or
`windows-aarch64` on ARM) from
`https://github.com/jvc56/MAGPIE/actions/runs/<CI_RUN_ID>` in their browser and unzip it
into `$W\dist`.

```powershell
$artifact = if ($Name -eq 'arm64') { 'windows-aarch64' } else { 'windows-x86_64' }
gh run download $CI_RUN_ID -R jvc56/MAGPIE -n $artifact -D (Join-Path $W 'dist')
Get-FileHash (Join-Path $W 'dist\*.exe') -Algorithm SHA256 |
  Format-Table -AutoSize | Out-String -Width 300 |
  Set-Content -Encoding utf8 (Join-Path $R '02-download-hash.txt')
```

Then check both, recording everything in `REPORT.md`:

```powershell
Get-Item (Join-Path $W 'dist\magpie.exe') -Stream Zone.Identifier -ErrorAction SilentlyContinue |
  ForEach-Object { 'mark_of_the_web: yes' } | Set-Content -Encoding utf8 (Join-Path $R '02-motw.txt')
if (-not (Test-Path (Join-Path $R '02-motw.txt'))) { 'mark_of_the_web: no' | Set-Content (Join-Path $R '02-motw.txt') }
```

- Has antivirus quarantined or deleted the file? Check that `$W\dist\magpie.exe` still
  exists.
- Mark of the Web: Windows only shows its SmartScreen warning for files it knows came
  from the internet. A file downloaded by PowerShell or `gh` usually doesn't carry that
  mark, so it won't trigger SmartScreen. Step 9 handles that with the volunteer's
  browser.

## Step 3. First run in an empty folder: `data fetch`

```powershell
$Run = Join-Path $W 'run'
New-Item -ItemType Directory -Force $Run | Out-Null
Copy-Item (Join-Path $W 'dist\magpie.exe') $Run
Set-Location $Run
$t = Measure-Command { cmd /c "magpie.exe data fetch < NUL > `"$R\03-data-fetch.txt`" 2>&1" }
"exit_code: $LASTEXITCODE`nseconds: $([math]::Round($t.TotalSeconds,1))" |
  Add-Content -Encoding utf8 (Join-Path $R '03-data-fetch.txt')
$t2 = Measure-Command { cmd /c "magpie.exe data fetch < NUL > `"$R\03-data-fetch-again.txt`" 2>&1" }
"seconds: $([math]::Round($t2.TotalSeconds,1))" | Add-Content (Join-Path $R '03-data-fetch-again.txt')
```

Expected in `03-data-fetch.txt`:
- `downloading the data, part 1 of 5 (40 MB)` through `part 5 of 5 (26 MB)`;
- `unpacking the data into ./data`;
- `unpacked 127 files`;
- `data version 20260925 is ready in ./data`;
- no line starting `(error`.

Expected in `03-data-fetch-again.txt`: `./data already holds data version 20260925`,
within a few seconds.

Check that the bytes on disk are exactly right. This catches text-mode or
line-ending corruption, which can only show up on Windows:

```powershell
$expectedHashes = @{
  'data\layouts\standard15.txt'      = 'c1a81c35a7730f64abcca23c8c56cc872e892a68f95289a4189b373c8cc5a01f'
  'data\lexica\CSW21.kwg'            = '3def63951c55eef62e7d480b3f0222b2fc4cad3fe5f82b961d51aae5ed74cc9f'
  'data\lexica\CSW21.klv2'           = 'd33c30e637d53ee6250fac943a01c3c9a18f93c638499ff5e13fa358c2ca03bb'
  'data\lexica\CSW21_super21.klv2'   = '5600ddb71b54ad800541a56303f7bd7382136b9b2332fa9210a36ad5360c6b8f'
  'data\lexica\CSW24_super21.klv2'   = '5600ddb71b54ad800541a56303f7bd7382136b9b2332fa9210a36ad5360c6b8f'
}
$lines = foreach ($f in $expectedHashes.Keys) {
  $h = if (Test-Path $f) { (Get-FileHash $f -Algorithm SHA256).Hash.ToLower() } else { 'MISSING' }
  "$f  match=$($h -eq $expectedHashes[$f])  $h"
}
$lines += "symlink_copies_are_regular_files: $(-not ((Get-Item 'data\lexica\CSW24_super21.klv2').Attributes -band [IO.FileAttributes]::ReparsePoint))"
$lines += "file_count: $((Get-ChildItem data -Recurse -File).Count)"
$lines | Set-Content -Encoding utf8 (Join-Path $R '03-data-hashes.txt')
```

Expected: every line says `match=True`, `symlink_copies_are_regular_files: True`, and
`file_count: 128`, which is the 127 data files plus `.data-version`.

## Step 4. An ordinary session

```powershell
@(
  'set -lex CSW24 -threads 2 -savesettings false'
  'cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 AEINRST/ 0/0 0'
  'gen -numplays 3'
  'sim -plies 2 -it 200 -scond none'
  'infer 1 MUZAKY 58 -numplays 20'
  'cgp 2ABERRaNT4G/5HE6FE/5OS5CUR/5DI5OMA/1HARPIST4WEN/5ET2Q3TI/6E1VIVO2A/4JUDY2ABOIL/4I2ERASERS1/1DOWLY8M/4TaLEGGIO2I/7Z6C/7I3PUNK/7NONTONAL/7E2AX2E AD/EFU 467/497 0'
  'endgame -eplies 4'
  'builders'
  'quit'
) | Set-Content -Encoding ascii (Join-Path $Run 'session.txt')
$t = Measure-Command { cmd /c "magpie.exe < session.txt > `"$R\04-session.txt`" 2>&1" }
"exit_code: $LASTEXITCODE`nseconds: $([math]::Round($t.TotalSeconds,1))" | Add-Content (Join-Path $R '04-session.txt')
```

Expected in `04-session.txt`:
- `building the wordmap for CSW24, which makes move generation faster`, once;
- three moves for `AEINRST`, the top ones scoring 66;
- an `Endgame (depth: 4, ...)` block whose best play is `5K DA(WEN) 18`;
- the builders line, containing `"build_target":"<ExpectedTarget>"`: `nehalem` on x64,
  `armv8-a` on ARM;
- no line starting `(error`.

`data\lexica\CSW24.wmp` should now exist (about 179 MB).

The board is drawn with box characters and colour codes, which look like noise in the
text file. That's expected; step 9 has the volunteer check how it looks on screen.

## Step 5. Speed and determinism

```powershell
$cmd = 'autoplay games 20000 -lex CSW21 -s1 equity -s2 equity -r1 best -r2 best -numplays 1 -seed 42 -wmp true -gp false -savesettings false'
cmd /c "magpie.exe `"set -lex CSW21 -savesettings false`" < NUL > NUL 2>&1"   # builds CSW21.wmp outside the timing
$rows = @('threads,repeat,seconds,turns_played')
foreach ($th in @(1, $Threads)) { foreach ($rep in 1, 2) {
  $out = Join-Path $R "05-autoplay-t$th-r$rep.txt"
  $t = Measure-Command { cmd /c "magpie.exe `"$cmd -threads $th`" < NUL > `"$out`" 2>&1" }
  $turns = (Select-String -Path $out -Pattern 'Turns Played: (\d+)' | Select-Object -First 1).Matches.Groups[1].Value
  $rows += "$th,$rep,$([math]::Round($t.TotalSeconds,2)),$turns"
} }
$rows | Set-Content -Encoding utf8 (Join-Path $R '05-autoplay.csv')
"CSW21.wmp sha256: $((Get-FileHash 'data\lexica\CSW21.wmp' -Algorithm SHA256).Hash.ToLower())" |
  Set-Content -Encoding utf8 (Join-Path $R '05-wordmap-hash.txt')
```

Expected:
- Every row has `turns_played` exactly `443300`. Any other number means Windows plays
  differently from Linux, which is a **FAIL** worth reporting.
- `CSW21.wmp sha256: 89250e65236582244a90c037c4decae11e9c795771a053e61e24050214a38cfd`.
  The same file built on Linux has this hash, so a different hash is a **FAIL**.
- Times are only for comparison. For reference, a 2026 Linux desktop takes about 20 s
  with 1 thread and about 4 s with 8.

## Step 6. Memory with many threads (pre-endgame)

This answers a question CI can't. Each pre-endgame worker thread asks for a 64 MB stack,
and Windows might reserve all of it up front (committed memory) instead of on demand.
Run the same solve with 1 thread and with `$Threads` threads, recording peak private
(committed) memory and peak working set:

```powershell
@(
  'set -savesettings false -pegtlim 30 -ttfraction 0.01'
  'cgp 15/3Q7U3/3U2TAURINE2/1CHANSONS2W3/2AI6JO3/DIRL1PO3IN3/E1D2EF3V4/F1I2p1TRAIK3/O1L2T4E4/ABy1PIT2BRIG2/ME1MOZELLE5/1GRADE1O1NOH3/WE3R1V7/AT5E7/G6D7 ENOSTXY/ACEISUY 356/378 0 -lex NWL20'
  'peg'
  'quit'
) | Set-Content -Encoding ascii (Join-Path $Run 'peg.txt')
$rows = @('threads,seconds,peak_private_mb,peak_working_set_mb,exit_code')
foreach ($th in @(1, $Threads)) {
  (Get-Content (Join-Path $Run 'peg.txt')) -replace '^set ', "set -threads $th " |
    Set-Content -Encoding ascii (Join-Path $Run "peg-t$th.txt")
  $out = Join-Path $R "06-peg-t$th.txt"
  $start = Get-Date
  $p = Start-Process -FilePath (Join-Path $Run 'magpie.exe') -WorkingDirectory $Run -NoNewWindow -PassThru `
        -RedirectStandardInput (Join-Path $Run "peg-t$th.txt") -RedirectStandardOutput $out `
        -RedirectStandardError "$out.err"
  $null = $p.Handle   # keeps the handle, so ExitCode can be read after the process exits
  $peakPrivate = 0; $peakWs = 0
  while (-not $p.HasExited) {
    try { $p.Refresh(); $peakPrivate = [math]::Max($peakPrivate, $p.PrivateMemorySize64)
          $peakWs = [math]::Max($peakWs, $p.WorkingSet64) } catch {}
    Start-Sleep -Milliseconds 200
  }
  $secs = [math]::Round(((Get-Date) - $start).TotalSeconds, 1)
  $rows += "$th,$secs,$([math]::Round($peakPrivate/1MB)),$([math]::Round($peakWs/1MB)),$($p.ExitCode)"
}
$rows | Set-Content -Encoding utf8 (Join-Path $R '06-peg-memory.csv')
```

Expected:
- Each run finishes in about 30–60 s with exit code 0.
- Each output file names `13L ONYX` as the top move.
- No `(error` lines, and the `.err` files are empty.

Don't interpret the memory numbers; the maintainers compare them with Linux. For
reference, on Linux the peak resident memory is about 0.7 GB with 1 thread and about
4.3 GB with 12.

## Step 7. Stop a long command from a pipe

```powershell
@(
  'autoplay games 100000000 -lex CSW21 -s1 equity -s2 equity -r1 best -r2 best -numplays 1 -threads 2 -gp false -savesettings false'
  'stop'
  'quit'
) | Set-Content -Encoding ascii (Join-Path $Run 'stop.txt')
$t = Measure-Command { cmd /c "magpie.exe < stop.txt > `"$R\07-stop.txt`" 2>&1" }
"seconds: $([math]::Round($t.TotalSeconds,1))" | Add-Content (Join-Path $R '07-stop.txt')
```

Expected: it returns within a few seconds (not minutes), and its output contains a
`Games Played:` line.

## Step 8. A folder with a non-ASCII name

```powershell
$U = Join-Path $W 'Ünïcödé-日本語 folder'
New-Item -ItemType Directory -Force $U | Out-Null
Copy-Item (Join-Path $Run 'magpie.exe') $U
Copy-Item (Join-Path $Run 'data') $U -Recurse
Set-Location $U
cmd /c "magpie.exe < `"$Run\session.txt`" > `"$R\08-unicode-session.txt`" 2>&1"
"exit_code: $LASTEXITCODE" | Add-Content (Join-Path $R '08-unicode-session.txt')
Set-Location $Run
```

Expected: the same as step 4, with no `(error` lines. The wordmap is already there, so
there is no "building" line this time.

If the volunteer's own user name has non-ASCII characters (see `system.txt`), say so
in `REPORT.md`: steps 3–7 have then already run under such a path.

## Step 9. Things only the volunteer can check

Ask the volunteer each of the following, one at a time. Record their answers verbatim
in `human-checks.md`, and save their screenshots into `$R\screenshots\` with the names
given.

1. **Browser download and SmartScreen.** Ask them to download MAGPIE again with their
   web browser, into `$W\browser`: the release page for `release`, or the CI artifact
   zip for `ci`, unzipped in Explorer. Then:
   - Did the browser or antivirus warn about, block, or delete the download? What did it
     say?
   - Ask them to double-click that `.exe`. Did Windows show **Windows protected your
     PC**? Ask for a screenshot (`smartscreen.png`). Then they click **More info** and
     **Run anyway**.
   - Because that folder has no data, MAGPIE should show a `MAGPIE <version>` banner and
     then offer to download the data. Ask them to answer `n`. MAGPIE should then print
     why it can't start, and **Press Enter to close this window.** Did the window stay
     open until they pressed Enter?
2. **The console in use.** Ask them to double-click `$Run\magpie.exe`, which has data,
   and type:
   - `cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 AEINRST/ 0/0 0 -lex CSW24`, then
     `gen`. Does the board draw cleanly, with straight lines, coloured premium squares,
     and no stray characters such as `←[0m`, `â”` or empty boxes? In the console window
     the column letters should be plain `A B C … O` and the turn marker `>`. Windows
     Terminal shows wider letters and `➤`. Ask for a screenshot (`console-board.png`).
   - Press the up-arrow key. Does the previous command come back, and can it be edited
     with the left and right arrows?
   - `autoplay games 100000000 -lex CSW21 -threads 2`, wait a few seconds, then type
     `stop` and press Enter. Did it stop and return to the `magpie>` prompt?
   - `quit`. Did the window close?
3. **Other consoles.** If they have both, ask them to run
   `$Run\magpie.exe cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 AEINRST/ 0/0 0 -lex CSW24`
   in **Windows Terminal** and in the old **Command Prompt** (`cmd.exe`). Does the
   board look right in both? Ask for screenshots (`terminal.png`, `cmd.png`).
4. **Anything else.** Did anything surprise them, look broken, or feel slow?

## Step 10. CI's contract tests on a real disk (`SOURCE = ci` only)

`magpie_test.exe` comes with the CI artifact. Its `compat` test checks file behaviour
that differs between Windows and Linux. That includes replacing a file while another
handle has it open, which Wine (used in CI) only approximates:

```powershell
$T = Join-Path $W 'tests'
New-Item -ItemType Directory -Force $T | Out-Null
Copy-Item (Join-Path $W 'dist\magpie_test.exe') $T
Set-Location $T
cmd /c "magpie_test.exe compat gzipreader string > `"$R\10-contract-tests.txt`" 2>&1"
"exit_code: $LASTEXITCODE" | Add-Content (Join-Path $R '10-contract-tests.txt')
Set-Location $Run
```

Expected: `exit_code: 0`, and no line containing `Assertion failed`.

## Step 11. Package the report

Remove anything identifying, then zip it:

```powershell
$user = $env:USERNAME; $machine = $env:COMPUTERNAME
Get-ChildItem $R -Recurse -File -Include *.txt,*.csv,*.md,*.err | ForEach-Object {
  $text = [IO.File]::ReadAllText($_.FullName)
  $clean = $text.Replace($HOME, '<HOME>').Replace($user, '<user>').Replace($machine, '<machine>')
  [IO.File]::WriteAllText($_.FullName, $clean, (New-Object Text.UTF8Encoding $false))
}
"finished $(Get-Date -Format o)" | Add-Content (Join-Path $R 'timeline.txt')
Compress-Archive -Path (Join-Path $R '*') -DestinationPath (Join-Path $W 'magpie-windows-report.zip') -Force
Get-Item (Join-Path $W 'magpie-windows-report.zip')
```

Before you finish:
1. Tell the volunteer where the zip is.
2. Remind them they can open it to check what's inside, and that screenshots may show
   their desktop: they should crop or remove anything private.
3. Tell them to send it to `SEND_TO`.

Don't delete `$W`. The volunteer may want to look at it, or a maintainer may ask a
follow-up question. When they're done, they can delete the folder `magpie-windows-check`
in their home folder; nothing else was changed.

## What the zip should contain

| File | From step | What it is |
|---|---|---|
| `REPORT.md` | all | The summary, filled in from the template below |
| `system.txt` | 1 | Windows version, CPU, memory, code page, antivirus, disk |
| `timeline.txt` | 0, 11 | Start and finish times |
| `02-download-hash.txt`, `02-motw.txt` | 2 | Download integrity and Mark of the Web |
| `03-data-fetch.txt`, `03-data-fetch-again.txt`, `03-data-hashes.txt` | 3 | First-run download, repeat run, byte checks |
| `04-session.txt` | 4 | An ordinary session's full output |
| `05-autoplay.csv`, `05-autoplay-t*-r*.txt`, `05-wordmap-hash.txt` | 5 | Speed, determinism, wordmap bytes |
| `06-peg-memory.csv`, `06-peg-t*.txt` | 6 | Memory with many threads |
| `07-stop.txt` | 7 | Stopping a command from a pipe |
| `08-unicode-session.txt` | 8 | Running from a non-ASCII folder |
| `10-contract-tests.txt` | 10 | Contract tests (`ci` source only) |
| `human-checks.md` | 9 | The volunteer's answers, verbatim |
| `screenshots/smartscreen.png`, `console-board.png`, `terminal.png`, `cmd.png` | 9 | Whatever the volunteer provided |

## REPORT.md template

```markdown
# MAGPIE Windows check

- Date:
- MAGPIE source: release <tag> / CI run <id>
- Windows: <caption>, build <n>, <x64|arm64>
- CPU / logical processors / memory:
- Antivirus:
- User name has non-ASCII characters: yes/no

| Step | Result | Notes |
|---|---|---|
| 1 System facts | PASS/FAIL | |
| 2 Download and hash | PASS/FAIL | antivirus reaction? Mark of the Web? |
| 3 data fetch | PASS/FAIL | seconds; repeat run said "already holds"? |
| 3 data hashes | PASS/FAIL | all match, symlink copies regular, 128 files? |
| 4 Session | PASS/FAIL | wordmap built; endgame 5K DA(WEN); build_target |
| 5 Autoplay | PASS/FAIL | turns 443300 in every row? times; wordmap hash |
| 6 PEG memory | PASS/FAIL | top move ONYX; times; (numbers in CSV) |
| 7 stop from a pipe | PASS/FAIL | seconds |
| 8 Non-ASCII folder | PASS/FAIL | |
| 9 Volunteer checks | see human-checks.md | SmartScreen shown? board clean? arrows? stop? |
| 10 Contract tests | PASS/FAIL/N/A | |

## Failures in detail
For each FAIL: the command, what was expected, and what happened (quote the output file
and line).

## Anything else noticed
```
