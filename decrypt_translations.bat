@echo off
REM  Decrypt .json.enc files back to JSON. Run before 02_build_tsv.
REM  Without the cache the pipeline re-translates from scratch (hours, real money).
REM  Usage: decrypt_translations.bat [--dir <game_dir>]
REM  Passphrase: TRANSLATIONS_KEY env var (CI) or translations_key.txt (local, git-ignored).
setlocal
set "VNHERE=%~dp0"
set "VNARGS=%*"
powershell -NoProfile -ExecutionPolicy Bypass -Command "$m='#:'+'PSSTART'; $b=Get-Content -LiteralPath '%~f0' -Raw; Invoke-Expression $b.Substring($b.IndexOf($m))"
endlocal & exit /b %errorlevel%

#:PSSTART
$ErrorActionPreference = 'Stop'

$here = $env:VNHERE
if (-not $here) { $here = (Get-Location).Path }
$here = $here.TrimEnd('\')

function Show-Usage {
    Write-Host "usage: decrypt_translations.bat [--dir <path>]"
    Write-Host "  --dir <path>   scope walk to a single game directory (default: script directory)"
}

# --- Arguments --------------------------------------------------------------
$targetDir = $null
$tokens = @()
if ($env:VNARGS) {
    $matches = [regex]::Matches($env:VNARGS, '"([^"]*)"|(\S+)')
    foreach ($m in $matches) {
        if ($m.Groups[1].Success) { $tokens += $m.Groups[1].Value }
        else                      { $tokens += $m.Groups[2].Value }
    }
}
for ($i = 0; $i -lt $tokens.Count; $i++) {
    $t = $tokens[$i]
    switch -Regex ($t) {
        '^--decrypt$' { }
        '^--(dir|game)$' {
            if ($i + 1 -ge $tokens.Count) {
                Write-Host "ERROR: $t requires a path argument." -ForegroundColor Red
                Show-Usage; exit 2
            }
            $i++
            $targetDir = $tokens[$i]
        }
        '^--(dir|game)=(.+)$' {
            $targetDir = $Matches[2]
        }
        default {
            Write-Host "ERROR: unknown argument '$t'." -ForegroundColor Red
            Show-Usage; exit 2
        }
    }
}

$walkRoot = $here
if ($targetDir) {
    if ([System.IO.Path]::IsPathRooted($targetDir)) {
        $walkRoot = $targetDir
    }
    elseif (Test-Path -LiteralPath (Join-Path (Get-Location).Path $targetDir)) {
        $walkRoot = (Resolve-Path -LiteralPath (Join-Path (Get-Location).Path $targetDir)).Path
    }
    elseif (Test-Path -LiteralPath (Join-Path $here $targetDir)) {
        $walkRoot = (Resolve-Path -LiteralPath (Join-Path $here $targetDir)).Path
    }
    else {
        $walkRoot = Join-Path $here $targetDir
    }
}
$walkRoot = $walkRoot.TrimEnd('\')
if (-not (Test-Path -LiteralPath $walkRoot)) {
    Write-Host "ERROR: directory not found: $walkRoot" -ForegroundColor Red
    exit 1
}

# --- Key --------------------------------------------------------------------
$candidates = @(
    (Join-Path $here 'translations_key.txt'),
    (Join-Path $walkRoot 'translations_key.txt'),
    (Join-Path (Split-Path $here -Parent) 'translations_key.txt'),
    (Join-Path (Split-Path $walkRoot -Parent) 'translations_key.txt')
)
$keyText = $null
if ($env:TRANSLATIONS_KEY) {
    $line = ($env:TRANSLATIONS_KEY -split '\r?\n' |
             Where-Object { $_.Trim() -and -not $_.TrimStart().StartsWith('#') } |
             Select-Object -First 1)
    if ($line) { $keyText = $line.Trim() }
}
if (-not $keyText) {
    foreach ($c in $candidates) {
        if ($c -and (Test-Path -LiteralPath $c)) {
            $line = (Get-Content -LiteralPath $c |
                     Where-Object { $_.Trim() -and -not $_.TrimStart().StartsWith('#') } |
                     Select-Object -First 1)
            if ($line) {
                $keyText = $line.Trim()
                break
            }
        }
    }
}
if (-not $keyText) {
    Write-Host "ERROR: passphrase not found. Set TRANSLATIONS_KEY or provide translations_key.txt in one of:" -ForegroundColor Red
    $candidates | ForEach-Object { Write-Host "  $_" }
    exit 1
}
$key = [System.Security.Cryptography.SHA256]::Create().ComputeHash(
    [System.Text.Encoding]::UTF8.GetBytes($keyText.Trim()))

# VNE1 is ciphertext alone.  VNE2 is deflated before it is encrypted.
$magicV1 = [byte[]](0x56,0x4E,0x45,0x31)
$magicV2 = [byte[]](0x56,0x4E,0x45,0x32)
$headerLen = 20

function Expand-Bytes([byte[]]$data) {
    $in  = New-Object System.IO.MemoryStream(,$data)
    $ds  = New-Object System.IO.Compression.DeflateStream(
               $in, [System.IO.Compression.CompressionMode]::Decompress)
    $out = New-Object System.IO.MemoryStream
    $ds.CopyTo($out)
    $ds.Dispose(); $in.Dispose()
    $r = $out.ToArray(); $out.Dispose()
    return ,$r
}

# Skip .git, build, install, and _specs trees; only process direct children of script_output/.
function Skip-Path($full) {
    $rel = $full.Substring($walkRoot.Length)
    return ($full -match '\\\.git\\') -or
           ($rel -match '\\build\\') -or
           ($rel -match '\\install\\') -or
           ($rel -match '\\_specs') -or
           ($rel -notmatch '^(\\[^\\]+)?\\script_output\\[^\\]+$')
}

$files = @(Get-ChildItem -Path $walkRoot -Recurse -File -Filter '*.json.enc' `
               -ErrorAction SilentlyContinue |
           Where-Object { -not (Skip-Path $_.FullName) })
if ($files.Count -eq 0) {
    Write-Host "Nothing to do: no .json.enc under $walkRoot." -ForegroundColor Yellow
    exit 0
}

$done = 0
foreach ($f in $files) {
    $shown = $f.FullName.Substring($walkRoot.Length).TrimStart('\')
    try {
        $bytes = [System.IO.File]::ReadAllBytes($f.FullName)
        if ($bytes.Length -lt $headerLen) { throw "too small to be valid" }
        $deflated = $null
        if     (-not (Compare-Object $bytes[0..3] $magicV2)) { $deflated = $true }
        elseif (-not (Compare-Object $bytes[0..3] $magicV1)) { $deflated = $false }
        else   { throw "not a recognized encrypted file" }

        $iv = [byte[]]($bytes[4..19])
        $cipher = New-Object byte[] ($bytes.Length - $headerLen)
        [Array]::Copy($bytes, $headerLen, $cipher, 0, $cipher.Length)
        $aes = [System.Security.Cryptography.Aes]::Create()
        $aes.Key = $key; $aes.Mode = 'CBC'; $aes.Padding = 'PKCS7'; $aes.IV = $iv
        $dec = $aes.CreateDecryptor()
        try   { $body = $dec.TransformFinalBlock($cipher, 0, $cipher.Length) }
        catch { throw "decryption failed, wrong passphrase" }
        finally { $dec.Dispose(); $aes.Dispose() }
        if ($deflated) { $body = Expand-Bytes $body }

        $outFile = $f.FullName.Substring(0, $f.FullName.Length - 4)
        [System.IO.File]::WriteAllBytes($outFile, $body)
        [System.IO.File]::SetLastWriteTimeUtc($outFile, $f.LastWriteTimeUtc)
        $done++
        Write-Host ("  restored  {0}  ({1:N0} bytes)" -f
                    $shown.Substring(0, $shown.Length - 4), $body.Length)
    }
    catch {
        Write-Host "  SKIPPED   $shown -> $_" -ForegroundColor Yellow
    }
}
Write-Host ("Done. {0} of {1} file(s)." -f $done, $files.Count) -ForegroundColor Green
if ($done -ne $files.Count) { exit 1 }
exit 0
