@echo off
REM  Encrypt or decrypt the translation JSON files.
REM  Usage: encrypt_translations.bat --encrypt|--decrypt [--dir <game_dir>]
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
    Write-Host "usage: encrypt_translations.bat --encrypt|--decrypt [--dir <path>]"
    Write-Host "  --encrypt      write <name>.json.enc beside each JSON, keeping the JSON"
    Write-Host "  --decrypt      write <name>.json beside each .enc, keeping the .enc"
    Write-Host "  --dir <path>   scope walk to a single game directory (default: script directory)"
}

# --- Arguments --------------------------------------------------------------
$mode = $null
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
        '^--encrypt$' { $mode = 'encrypt' }
        '^--decrypt$' { $mode = 'decrypt' }
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
if (-not $mode) {
    Write-Host "ERROR: give --encrypt or --decrypt." -ForegroundColor Red
    Show-Usage; exit 2
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

# VNE1 is ciphertext alone.  VNE2 deflates first, and is what this writes.
$magicV1 = [byte[]](0x56,0x4E,0x45,0x31)
$magicV2 = [byte[]](0x56,0x4E,0x45,0x32)
$headerLen = 20                      # 4 magic + 16 IV

$plainNames = @(
    'translated_text.json',
    'translation_cache_anthropic.json',
    'translation_cache_gemini.json',
    'nps_reconstruction_map.json',
    'chart_translation_cache.json'
)
$encNames   = @($plainNames | ForEach-Object { $_ + '.enc' })

# Skip .git, build, install, and _specs trees; only process direct children of script_output/.
# When walking the multi-game repo root in --encrypt mode, skip WIP game folders that have no .json.enc.
function Skip-Path($full) {
    $rel = $full.Substring($walkRoot.Length)
    if (($full -match '\\\.git\\') -or
        ($rel -match '\\build\\') -or
        ($rel -match '\\install\\') -or
        ($rel -match '\\_specs') -or
        ($rel -notmatch '^(\\[^\\]+)?\\script_output\\[^\\]+$')) {
        return $true
    }
    if ($mode -eq 'encrypt' -and ($rel -notmatch '^\\script_output\\')) {
        $parentDir = Split-Path $full -Parent
        $hasTrackedEnc = @(Get-ChildItem -LiteralPath $parentDir -Filter '*.json.enc' -File -ErrorAction SilentlyContinue).Count -gt 0
        if (-not $hasTrackedEnc) { return $true }
    }
    return $false
}

function Compress-Bytes([byte[]]$data) {
    $ms = New-Object System.IO.MemoryStream
    $ds = New-Object System.IO.Compression.DeflateStream(
              $ms, [System.IO.Compression.CompressionMode]::Compress, $true)
    $ds.Write($data, 0, $data.Length)
    $ds.Dispose()
    $out = $ms.ToArray(); $ms.Dispose()
    return ,$out
}

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

function Decrypt-Bytes([byte[]]$bytes, [string]$label) {
    if ($bytes.Length -lt $headerLen) { throw "too small to be valid: $label" }
    $deflated = $null
    if     (-not (Compare-Object $bytes[0..3] $magicV2)) { $deflated = $true }
    elseif (-not (Compare-Object $bytes[0..3] $magicV1)) { $deflated = $false }
    else   { throw "not a recognized encrypted file: $label" }

    $iv = [byte[]]($bytes[4..19])
    $cipher = New-Object byte[] ($bytes.Length - $headerLen)
    [Array]::Copy($bytes, $headerLen, $cipher, 0, $cipher.Length)
    $aes = [System.Security.Cryptography.Aes]::Create()
    $aes.Key = $key; $aes.Mode = 'CBC'; $aes.Padding = 'PKCS7'; $aes.IV = $iv
    $dec = $aes.CreateDecryptor()
    try   { $body = $dec.TransformFinalBlock($cipher, 0, $cipher.Length) }
    catch { throw "decryption failed, wrong passphrase for $label" }
    finally { $dec.Dispose(); $aes.Dispose() }
    if ($deflated) { $body = Expand-Bytes $body }
    return ,$body
}

function Protect-File($file) {
    $plain = [System.IO.File]::ReadAllBytes($file.FullName)
    $encPath = $file.FullName + '.enc'
    if (Test-Path -LiteralPath $encPath) {
        $existingPlain = $null
        try {
            $encBytes = [System.IO.File]::ReadAllBytes($encPath)
            $existingPlain = Decrypt-Bytes $encBytes ($file.Name + '.enc')
        }
        catch {
            $existingPlain = $null
        }
        if ($null -ne $existingPlain -and $existingPlain.Length -eq $plain.Length) {
            $sha = [System.Security.Cryptography.SHA256]::Create()
            $h1 = [System.BitConverter]::ToString($sha.ComputeHash($existingPlain))
            $h2 = [System.BitConverter]::ToString($sha.ComputeHash($plain))
            $sha.Dispose()
            if ($h1 -eq $h2) {
                $encItem = Get-Item -LiteralPath $encPath
                [System.IO.File]::SetLastWriteTimeUtc($file.FullName, $encItem.LastWriteTimeUtc)
                return [pscustomobject]@{ In = $plain.Length; Out = $encItem.Length; Unchanged = $true }
            }
        }
    }
    $body  = Compress-Bytes $plain
    $aes = [System.Security.Cryptography.Aes]::Create()
    $aes.Key = $key; $aes.Mode = 'CBC'; $aes.Padding = 'PKCS7'
    $aes.GenerateIV()
    $enc = $aes.CreateEncryptor()
    $cipher = $enc.TransformFinalBlock($body, 0, $body.Length)
    $ms = New-Object System.IO.MemoryStream
    $ms.Write($magicV2, 0, $magicV2.Length)
    $ms.Write($aes.IV, 0, $aes.IV.Length)
    $ms.Write($cipher, 0, $cipher.Length)
    [System.IO.File]::WriteAllBytes($encPath, $ms.ToArray())
    $ms.Dispose(); $enc.Dispose(); $aes.Dispose()
    [System.IO.File]::SetLastWriteTimeUtc($encPath, $file.LastWriteTimeUtc)
    return [pscustomobject]@{ In = $plain.Length; Out = ($headerLen + $cipher.Length); Unchanged = $false }
}

function Unprotect-File($file) {
    $bytes = [System.IO.File]::ReadAllBytes($file.FullName)
    $body = Decrypt-Bytes $bytes $file.Name
    $outFile = $file.FullName.Substring(0, $file.FullName.Length - 4)
    [System.IO.File]::WriteAllBytes($outFile, $body)
    [System.IO.File]::SetLastWriteTimeUtc($outFile, $file.LastWriteTimeUtc)
    return [pscustomobject]@{ In = $bytes.Length; Out = $body.Length; Unchanged = $false }
}

# --- Walk -------------------------------------------------------------------
$wanted = if ($mode -eq 'encrypt') { $plainNames } else { $encNames }
$files = @(Get-ChildItem -Path $walkRoot -Recurse -File -Include $wanted `
               -ErrorAction SilentlyContinue |
           Where-Object { -not (Skip-Path $_.FullName) })

if ($files.Count -eq 0) {
    Write-Host "Nothing to do: no $($wanted -join ' / ') under $walkRoot." -ForegroundColor Yellow
    exit 0
}

Write-Host "Mode: $($mode.ToUpper())  ($($files.Count) file(s))" -ForegroundColor Cyan
$done = 0
$totalIn = 0; $totalOut = 0
foreach ($f in $files) {
    $shown = $f.FullName.Substring($walkRoot.Length).TrimStart('\')
    try {
        $r = if ($mode -eq 'encrypt') { Protect-File $f } else { Unprotect-File $f }
        $done++; $totalIn += $r.In; $totalOut += $r.Out
        $label = if ($mode -eq 'encrypt' -and $r.Unchanged) { 'unchanged' } else { $mode }
        Write-Host ("  {0,-9} {1}  ({2:N0} -> {3:N0} bytes)" -f $label, $shown, $r.In, $r.Out)
    }
    catch {
        Write-Host "  SKIPPED   $shown -> $_" -ForegroundColor Yellow
    }
}
Write-Host ("Done. {0} of {1} file(s), {2:N0} -> {3:N0} bytes." -f
            $done, $files.Count, $totalIn, $totalOut) -ForegroundColor Green
if ($done -ne $files.Count) { exit 1 }
exit 0
