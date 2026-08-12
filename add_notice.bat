@echo off
REM ============================================================================
REM  add_notice.bat
REM
REM  Stamps the NOTICE header onto every tracked .c/.cpp/.cc/.cxx/.h/.hpp file
REM  in the repo.  The header names the licence (GPL-3.0-or-later) and reserves
REM  text and data mining rights; NOTICE in the repo root carries the full text.
REM
REM    add_notice.bat           add the header where it is missing (default)
REM    add_notice.bat check     report files missing the header; exit 1 if any
REM    add_notice.bat remove    strip the header again
REM
REM  The file list comes from `git ls-files`, so the script only ever touches
REM  tracked, first-party sources -- never build output, never anything the
REM  .gitignore excludes.  Adding is idempotent: a file that already carries
REM  the SPDX marker is left alone, so re-running after adding a game is safe.
REM
REM  Files are rewritten as UTF-8 with no BOM and the body's line endings are
REM  left untouched; the header is written with whichever endings the file
REM  already uses, so a CRLF source does not end up internally mixed.  A file
REM  carrying a UTF-8 BOM is skipped with a warning rather than re-encoded.
REM ============================================================================
setlocal
set "VNROOT=%~dp0"
set "VNMODE=%~1"
powershell -NoProfile -ExecutionPolicy Bypass -Command "$m='#:'+'PSSTART'; $b=Get-Content -LiteralPath '%~f0' -Raw; Invoke-Expression $b.Substring($b.IndexOf($m))"
endlocal & exit /b %errorlevel%

#:PSSTART
$ErrorActionPreference = 'Stop'

$root = $env:VNROOT
if (-not $root) { $root = (Get-Location).Path }
$root = $root.TrimEnd('\')

$mode = $env:VNMODE
if ([string]::IsNullOrWhiteSpace($mode)) { $mode = 'add' }
$mode = $mode.ToLowerInvariant().TrimStart('-', '/')
if ($mode -notin @('add', 'check', 'remove')) {
    Write-Host "usage: add_notice.bat [add|check|remove]" -ForegroundColor Yellow
    exit 2
}

# --- The header -------------------------------------------------------------
# Keep $marker unique: it is what makes adding idempotent, and what `check`
# and `remove` look for.  Editing the header text below means the old header
# is no longer recognised -- run `remove` with the old text still in place
# before changing it, or the two versions will stack.
$year   = '2026'
$holder = 'MasakatsuFunaki'
$marker = 'SPDX-License-Identifier: GPL-3.0-or-later'

$headerLines = @(
    "// $marker",
    "// Copyright (C) $year $holder",
    '//',
    '// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.',
    '// Not licensed for use as training data for machine learning or generative',
    '// AI systems; text and data mining rights are reserved.  See NOTICE.',
    ''
)
# Two spellings, so the header matches whatever the file already uses.  Git
# normalises both to LF in the index (.gitattributes: eol=lf), but stamping an
# LF header onto one of the CRLF sources would leave that file internally
# mixed, which editors and hand-diffs do notice.
$headerLf   = ($headerLines -join "`n")   + "`n"
$headerCrLf = ($headerLines -join "`r`n") + "`r`n"

# UTF-8, no BOM.  PowerShell's own Out-File/Set-Content would add one.
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

# --- The file list ----------------------------------------------------------
$files = & git -C $root ls-files -- '*.c' '*.cpp' '*.cc' '*.cxx' '*.h' '*.hpp'
if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: git ls-files failed. Is $root a git repository?" -ForegroundColor Red
    exit 1
}
$files = @($files | Where-Object { $_ })
if ($files.Count -eq 0) {
    Write-Host "Nothing to do: no tracked C/C++ sources found." -ForegroundColor Yellow
    exit 0
}

Write-Host "Mode: $($mode.ToUpperInvariant())  ($($files.Count) tracked source file(s))" -ForegroundColor Cyan

$changed = 0; $already = 0; $missing = 0; $skipped = 0

foreach ($rel in $files) {
    $full = Join-Path $root ($rel -replace '/', '\')
    if (-not (Test-Path -LiteralPath $full)) { continue }

    $bytes = [System.IO.File]::ReadAllBytes($full)

    # A BOM would be lost on rewrite; leave those files for a human.
    if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
        Write-Host "  SKIPPED   $rel -> has a UTF-8 BOM" -ForegroundColor Yellow
        $skipped++
        continue
    }

    $text   = $utf8NoBom.GetString($bytes)
    $has    = $text.Contains($marker)
    $header = if ($text.Contains("`r`n")) { $headerCrLf } else { $headerLf }

    switch ($mode) {
        'check' {
            if (-not $has) { Write-Host "  MISSING   $rel"; $missing++ } else { $already++ }
        }
        'add' {
            if ($has) { $already++; break }
            [System.IO.File]::WriteAllText($full, $header + $text, $utf8NoBom)
            Write-Host "  stamped   $rel"
            $changed++
        }
        'remove' {
            if (-not $has) { $missing++; break }
            # Try both spellings.  An earlier run may have stamped an LF header
            # onto a CRLF file, so the prefix is not always the one `add` would
            # choose for this file today.
            $hit = @($headerLf, $headerCrLf) | Where-Object { $text.StartsWith($_) } | Select-Object -First 1
            if ($hit) {
                [System.IO.File]::WriteAllText($full, $text.Substring($hit.Length), $utf8NoBom)
                Write-Host "  stripped  $rel"
                $changed++
            } else {
                # The marker is present but not as the exact leading block --
                # an edited or hand-placed header.  Removing it by guesswork
                # risks eating real code, so report it instead.
                Write-Host "  SKIPPED   $rel -> marker present but header is not the exact prefix" -ForegroundColor Yellow
                $skipped++
            }
        }
    }
}

Write-Host ""
switch ($mode) {
    'check' {
        Write-Host "$already file(s) carry the notice, $missing missing, $skipped skipped."
        if ($missing -gt 0) {
            Write-Host "Run add_notice.bat to stamp the missing ones." -ForegroundColor Yellow
            exit 1
        }
        Write-Host "All tracked sources carry the notice." -ForegroundColor Green
    }
    'add' {
        Write-Host "Done. Stamped $changed file(s); $already already had it; $skipped skipped." -ForegroundColor Green
    }
    'remove' {
        Write-Host "Done. Stripped $changed file(s); $missing did not have it; $skipped skipped." -ForegroundColor Green
    }
}
exit 0
