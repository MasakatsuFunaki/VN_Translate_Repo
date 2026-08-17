@echo off
REM  Decrypt .json.enc files back to JSON. Run before 02_translate.
REM  Without the cache the pipeline re-translates from scratch (hours, real money).
REM  Passphrase: translations_key.txt (beside this file, public).
setlocal
set "VNHERE=%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -Command "$m='#:'+'PSSTART'; $b=Get-Content -LiteralPath '%~f0' -Raw; Invoke-Expression $b.Substring($b.IndexOf($m))"
endlocal & exit /b %errorlevel%

#:PSSTART
$ErrorActionPreference = 'Stop'

$here = $env:VNHERE
if (-not $here) { $here = (Get-Location).Path }
$here = $here.TrimEnd('\')

# Beside the script in a staged install, one level up in the source tree.
$candidates = @(
    (Join-Path $here 'translations_key.txt'),
    (Join-Path (Split-Path $here -Parent) 'translations_key.txt')
)
$keyFile = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $keyFile) {
    Write-Host "ERROR: translations_key.txt not found. Looked in:" -ForegroundColor Red
    $candidates | ForEach-Object { Write-Host "  $_" }
    Write-Host "Copy it beside this script, or run it from a staged install."
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

# The staged install sits under build\, and the copy of this script shipped
# inside it runs with $here already there -- so the test is on the path BELOW
# $here, not the full path.  From the game root that skips the build tree;
# from a staged install there is no build\ segment left to match.
function Skip-Path($full) {
    $rel = $full.Substring($here.Length)
    return ($rel -like '\build\*') -or ($rel -like '*\.git\*')
}

$files = @(Get-ChildItem -Path $here -Recurse -File -Filter '*.json.enc' `
               -ErrorAction SilentlyContinue |
           Where-Object { -not (Skip-Path $_.FullName) })
if ($files.Count -eq 0) {
    Write-Host "Nothing to do: no .json.enc under $here." -ForegroundColor Yellow
    exit 0
}

$done = 0
foreach ($f in $files) {
    $shown = $f.FullName.Substring($here.Length).TrimStart('\')
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
