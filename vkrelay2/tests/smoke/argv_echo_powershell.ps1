# PowerShell argv/env/cwd preservation smoke test.
#
# PowerShell argument passing to native executables is the recurring quoting
# nuisance, so this test deliberately does NOT pass any app argv/env as native
# arguments. Instead it writes structured, NUL-separated input files and passes
# only simple path tokens across the boundary.
# ("the shell wrapper should pass only a descriptor path or token"). The C++
# verifier does all byte comparison.
#
# This script is intentionally pure ASCII; Unicode test data is built from code
# points so Windows PowerShell 5.1 (which reads .ps1 as ANSI without a BOM)
# cannot corrupt it.
param(
    [Parameter(Mandatory = $true)][string]$LaunchBin,
    [Parameter(Mandatory = $true)][string]$EchoBin,
    [Parameter(Mandatory = $true)][string]$VerifyBin
)

$ErrorActionPreference = 'Stop'

function Write-NulFile {
    param([string]$Path, [string[]]$Items)
    $ms = New-Object System.IO.MemoryStream
    foreach ($it in $Items) {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($it)
        $ms.Write($bytes, 0, $bytes.Length)
        $ms.WriteByte(0)
    }
    [System.IO.File]::WriteAllBytes($Path, $ms.ToArray())
    $ms.Dispose()
}

$work = Join-Path ([System.IO.Path]::GetTempPath()) ("vkrelay2_smoke_" + [System.Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work -Force | Out-Null
$cwdDir = Join-Path $work 'cwd dir'   # space intentional
New-Item -ItemType Directory -Path $cwdDir -Force | Out-Null

$cafe = "caf" + [char]0xE9            # cafe-with-acute, no non-ASCII source bytes

$nasty = @(
    'plain',
    'a b c',
    '"double"',
    "'single'",
    'back\slash',
    'trailing\',
    'q\"mix',
    '$PATH',
    '%PATH%',
    '100% sure',
    'a!b',
    'caret^here',
    'amp&here',
    '(parens)',
    '',
    '--gpu',
    "tab`there",
    "nl`nhere",
    $cafe
)

$argvFile = Join-Path $work 'argv.nul'
$expectFile = Join-Path $work 'expect.nul'
$envFile = Join-Path $work 'env.nul'
$cwdFile = Join-Path $work 'cwd.txt'
$echoed = Join-Path $work 'echoed.json'

# Full argv (program first) for the launcher; expected argv excludes the program.
Write-NulFile -Path $argvFile -Items (@($EchoBin) + $nasty)
Write-NulFile -Path $expectFile -Items $nasty

$envItems = @(
    'VKR_T_SPACE=a b c',
    'VKR_T_QUOTE=he said "hi"',
    'VKR_T_SEMI=a;b;c',
    'VKR_T_EQUALS=k=v',
    ('VKR_T_UNICODE=' + $cafe)
)
Write-NulFile -Path $envFile -Items $envItems
[System.IO.File]::WriteAllText($cwdFile, $cwdDir, (New-Object System.Text.UTF8Encoding($false)))

# Only simple path tokens cross to the native exes. No nasty data as PS args.
& $LaunchBin --argv-file $argvFile --env-file $envFile --cwd-file $cwdFile --run --run-output $echoed
if ($LASTEXITCODE -ne 0) {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
    Write-Host "vkrelay2-launch failed with exit code $LASTEXITCODE"
    exit 1
}

& $VerifyBin --expect-argv-file $expectFile --expect-env-file $envFile --cwd-file $cwdFile --actual-file $echoed
$code = $LASTEXITCODE

Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
exit $code
