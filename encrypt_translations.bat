@echo off
REM ============================================================================
REM  encrypt_translations.bat
REM
REM  Toggles AES-256 encryption on every translated_text.json and
REM  translation_cache_anthropic.json in the repo (the script_output folders).
REM
REM    - First run : encrypts each .json   -> .json.enc  (plaintext removed)
REM    - Next run  : decrypts each .json.enc -> .json     (.enc removed)
REM
REM  The key is read from key.txt in this folder (this .bat takes no params and
REM  assumes key.txt exists). key.txt is git-ignored so the key never leaves
REM  your PC. The TOOLS test fixture is intentionally skipped.
REM ============================================================================
setlocal
set "VNROOT=%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -Command "$m='#:'+'PSSTART'; $b=Get-Content -LiteralPath '%~f0' -Raw; Invoke-Expression $b.Substring($b.IndexOf($m))"
endlocal & exit /b %errorlevel%

#:PSSTART
$ErrorActionPreference = 'Stop'

$root = $env:VNROOT
if (-not $root) { $root = (Get-Location).Path }

# --- Load the key -----------------------------------------------------------
$keyFile = Join-Path $root 'key.txt'
if (-not (Test-Path -LiteralPath $keyFile)) {
    Write-Host "ERROR: key.txt not found in $root" -ForegroundColor Red
    Write-Host "Create key.txt in the repo root containing your secret key, then run again."
    exit 1
}
$keyText = (Get-Content -LiteralPath $keyFile -Raw).Trim()
if ([string]::IsNullOrEmpty($keyText)) {
    Write-Host "ERROR: key.txt is empty." -ForegroundColor Red
    exit 1
}
# Derive a 32-byte AES-256 key from the passphrase.
$key = [System.Security.Cryptography.SHA256]::Create().ComputeHash(
    [System.Text.Encoding]::UTF8.GetBytes($keyText))

# 4-byte file signature ("VNE1") marks our encrypted format.
$magic = [byte[]](0x56,0x4E,0x45,0x31)

$plainNames = @('translated_text.json','translation_cache_anthropic.json')
$encNames   = @('translated_text.json.enc','translation_cache_anthropic.json.enc')

function Skip-Path($full) {
    return ($full -match '\\\.git\\') -or ($full -match '\\TOOLS\\')
}

function Protect-File($file) {
    $aes = [System.Security.Cryptography.Aes]::Create()
    $aes.Key = $key; $aes.Mode = 'CBC'; $aes.Padding = 'PKCS7'
    $aes.GenerateIV()
    $enc   = $aes.CreateEncryptor()
    $plain = [System.IO.File]::ReadAllBytes($file.FullName)
    $cipher = $enc.TransformFinalBlock($plain, 0, $plain.Length)
    $ms = New-Object System.IO.MemoryStream
    $ms.Write($magic, 0, $magic.Length)
    $ms.Write($aes.IV, 0, $aes.IV.Length)
    $ms.Write($cipher, 0, $cipher.Length)
    [System.IO.File]::WriteAllBytes($file.FullName + '.enc', $ms.ToArray())
    $ms.Dispose(); $enc.Dispose(); $aes.Dispose()
    Remove-Item -LiteralPath $file.FullName -Force
}

function Unprotect-File($file) {
    $bytes = [System.IO.File]::ReadAllBytes($file.FullName)
    if ($bytes.Length -lt 20) { throw "File too small to be valid: $($file.Name)" }
    for ($i = 0; $i -lt 4; $i++) {
        if ($bytes[$i] -ne $magic[$i]) { throw "Not a recognized encrypted file: $($file.Name)" }
    }
    $iv = [byte[]]($bytes[4..19])
    $cipherLen = $bytes.Length - 20
    $cipher = New-Object byte[] $cipherLen
    [Array]::Copy($bytes, 20, $cipher, 0, $cipherLen)
    $aes = [System.Security.Cryptography.Aes]::Create()
    $aes.Key = $key; $aes.Mode = 'CBC'; $aes.Padding = 'PKCS7'; $aes.IV = $iv
    $dec = $aes.CreateDecryptor()
    try   { $plain = $dec.TransformFinalBlock($cipher, 0, $cipher.Length) }
    catch { throw "Decryption failed (wrong key in key.txt?) for $($file.Name)" }
    finally { $dec.Dispose(); $aes.Dispose() }
    $outFile = $file.FullName.Substring(0, $file.FullName.Length - 4)  # strip ".enc"
    [System.IO.File]::WriteAllBytes($outFile, $plain)
    Remove-Item -LiteralPath $file.FullName -Force
}

# --- Decide mode: any .enc present => decrypt, otherwise encrypt ------------
$encFiles = @(Get-ChildItem -Path $root -Recurse -File -Include $encNames -ErrorAction SilentlyContinue |
    Where-Object { -not (Skip-Path $_.FullName) })

if ($encFiles.Count -gt 0) {
    Write-Host "Mode: DECRYPT ($($encFiles.Count) file(s))" -ForegroundColor Cyan
    $done = 0
    foreach ($f in $encFiles) {
        try { Unprotect-File $f; $done++; Write-Host "  decrypted  $($f.FullName.Substring($root.Length))" }
        catch { Write-Host "  SKIPPED    $($f.FullName.Substring($root.Length)) -> $_" -ForegroundColor Yellow }
    }
    Write-Host "Done. Decrypted $done file(s)." -ForegroundColor Green
}
else {
    $plainFiles = @(Get-ChildItem -Path $root -Recurse -File -Include $plainNames -ErrorAction SilentlyContinue |
        Where-Object { -not (Skip-Path $_.FullName) })
    if ($plainFiles.Count -eq 0) {
        Write-Host "Nothing to do: no translated_text.json / translation_cache_anthropic.json found." -ForegroundColor Yellow
        exit 0
    }
    Write-Host "Mode: ENCRYPT ($($plainFiles.Count) file(s))" -ForegroundColor Cyan
    $done = 0
    foreach ($f in $plainFiles) {
        try { Protect-File $f; $done++; Write-Host "  encrypted  $($f.FullName.Substring($root.Length))" }
        catch { Write-Host "  SKIPPED    $($f.FullName.Substring($root.Length)) -> $_" -ForegroundColor Yellow }
    }
    Write-Host "Done. Encrypted $done file(s)." -ForegroundColor Green
}
exit 0
