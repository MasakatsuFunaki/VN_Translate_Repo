@echo off
REM  Encrypt or decrypt the translation JSON files.
REM  Usage: encrypt_translations.bat --encrypt | --decrypt
REM  Passphrase: translations_key.txt (beside this file, public).
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
    Write-Host "usage: encrypt_translations.bat --encrypt|--decrypt"
    Write-Host "  --encrypt   write <name>.json.enc beside each JSON, keeping the JSON"
    Write-Host "  --decrypt   write <name>.json beside each .enc, keeping the .enc"
}

# --- Arguments --------------------------------------------------------------
$mode = $null
$tokens = @()
if ($env:VNARGS) { $tokens = @($env:VNARGS -split '\s+' | Where-Object { $_ }) }
foreach ($t in $tokens) {
    switch ($t) {
        '--encrypt' { $mode = 'encrypt' }
        '--decrypt' { $mode = 'decrypt' }
        default     {
            Write-Host "ERROR: unknown argument '$t'." -ForegroundColor Red
            Show-Usage; exit 2
        }
    }
}
if (-not $mode) {
    Write-Host "ERROR: give --encrypt or --decrypt." -ForegroundColor Red
    Show-Usage; exit 2
}

# --- Key --------------------------------------------------------------------
$keyFile = Join-Path $here 'translations_key.txt'
if (-not (Test-Path -LiteralPath $keyFile)) {
    Write-Host "ERROR: translations_key.txt not found at $keyFile." -ForegroundColor Red
    exit 1
}
$keyText = (Get-Content -LiteralPath $keyFile |
            Where-Object { $_.Trim() -and -not $_.TrimStart().StartsWith('#') } |
            Select-Object -First 1)
if (-not $keyText) {
    Write-Host "ERROR: translations_key.txt holds no passphrase line." -ForegroundColor Red
    exit 1
}
$key = [System.Security.Cryptography.SHA256]::Create().ComputeHash(
    [System.Text.Encoding]::UTF8.GetBytes($keyText.Trim()))

# VNE1 is ciphertext alone.  VNE2 deflates first, and is what this writes.
$magicV1 = [byte[]](0x56,0x4E,0x45,0x31)
$magicV2 = [byte[]](0x56,0x4E,0x45,0x32)
$headerLen = 20                      # 4 magic + 16 IV

$plainNames = @('translated_text.json','translation_cache_anthropic.json')
$encNames   = @('translated_text.json.enc','translation_cache_anthropic.json.enc')

# build/ holds staged copies that --install writes; leave them to --install.
function Skip-Path($full) {
    return ($full -match '\\\.git\\') -or ($full -match '\\build\\')
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

function Protect-File($file) {
    $plain = [System.IO.File]::ReadAllBytes($file.FullName)
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
    [System.IO.File]::WriteAllBytes($file.FullName + '.enc', $ms.ToArray())
    $ms.Dispose(); $enc.Dispose(); $aes.Dispose()
    return [pscustomobject]@{ In = $plain.Length; Out = ($headerLen + $cipher.Length) }
}

function Unprotect-File($file) {
    $bytes = [System.IO.File]::ReadAllBytes($file.FullName)
    if ($bytes.Length -lt $headerLen) { throw "too small to be valid: $($file.Name)" }
    $deflated = $null
    if     (-not (Compare-Object $bytes[0..3] $magicV2)) { $deflated = $true }
    elseif (-not (Compare-Object $bytes[0..3] $magicV1)) { $deflated = $false }
    else   { throw "not a recognized encrypted file: $($file.Name)" }

    $iv = [byte[]]($bytes[4..19])
    $cipher = New-Object byte[] ($bytes.Length - $headerLen)
    [Array]::Copy($bytes, $headerLen, $cipher, 0, $cipher.Length)
    $aes = [System.Security.Cryptography.Aes]::Create()
    $aes.Key = $key; $aes.Mode = 'CBC'; $aes.Padding = 'PKCS7'; $aes.IV = $iv
    $dec = $aes.CreateDecryptor()
    try   { $body = $dec.TransformFinalBlock($cipher, 0, $cipher.Length) }
    catch { throw "decryption failed, wrong passphrase for $($file.Name)" }
    finally { $dec.Dispose(); $aes.Dispose() }
    if ($deflated) { $body = Expand-Bytes $body }
    $outFile = $file.FullName.Substring(0, $file.FullName.Length - 4)
    [System.IO.File]::WriteAllBytes($outFile, $body)
    return [pscustomobject]@{ In = $bytes.Length; Out = $body.Length }
}

# --- Walk -------------------------------------------------------------------
$wanted = if ($mode -eq 'encrypt') { $plainNames } else { $encNames }
$files = @(Get-ChildItem -Path $here -Recurse -File -Include $wanted `
               -ErrorAction SilentlyContinue |
           Where-Object { -not (Skip-Path $_.FullName) })

if ($files.Count -eq 0) {
    Write-Host "Nothing to do: no $($wanted -join ' / ') under $here." -ForegroundColor Yellow
    exit 0
}

Write-Host "Mode: $($mode.ToUpper())  ($($files.Count) file(s))" -ForegroundColor Cyan
$done = 0
$totalIn = 0; $totalOut = 0
foreach ($f in $files) {
    $shown = $f.FullName.Substring($here.Length).TrimStart('\')
    try {
        $r = if ($mode -eq 'encrypt') { Protect-File $f } else { Unprotect-File $f }
        $done++; $totalIn += $r.In; $totalOut += $r.Out
        Write-Host ("  {0,-9} {1}  ({2:N0} -> {3:N0} bytes)" -f $mode, $shown, $r.In, $r.Out)
    }
    catch {
        Write-Host "  SKIPPED   $shown -> $_" -ForegroundColor Yellow
    }
}
Write-Host ("Done. {0} of {1} file(s), {2:N0} -> {3:N0} bytes." -f
            $done, $files.Count, $totalIn, $totalOut) -ForegroundColor Green
if ($done -ne $files.Count) { exit 1 }
exit 0
