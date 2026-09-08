# run_integration.ps1  —  pipes each .sql file through mini_sqlite.exe
# Usage: from repo root:  .\tests\run_integration.ps1

$env:PATH = "C:\Users\sarth\Downloads\w64devkit\bin;" + $env:PATH
$exe = ".\build\Debug\mini_sqlite.exe"
$sqlDir = ".\tests\integration"

$scripts = @("ddl.sql", "select_basic.sql", "select_join.sql", "select_agg.sql", "errors.sql", "delete.sql", "update.sql")

$pass = 0
$fail = 0

foreach ($script in $scripts) {
    $path = Join-Path $sqlDir $script
    Write-Host ""
    Write-Host "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" -ForegroundColor Cyan
    Write-Host "  Running: $script" -ForegroundColor Cyan
    Write-Host "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" -ForegroundColor Cyan

    $output = Get-Content $path | & $exe 2>&1
    $output | ForEach-Object { Write-Host "  $_" }

    # A script "passes" if the exe didn't crash (exit code 0 or 1 — errors.sql
    # intentionally triggers semantic errors which go to stderr, not crashes)
    if ($LASTEXITCODE -le 1) {
        Write-Host "  [PASS]" -ForegroundColor Green
        $pass++
    } else {
        Write-Host "  [FAIL] exit code $LASTEXITCODE" -ForegroundColor Red
        $fail++
    }
}

Write-Host ""
Write-Host "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" -ForegroundColor White
Write-Host "  Integration results: $pass passed, $fail failed" -ForegroundColor $(if ($fail -eq 0) { "Green" } else { "Red" })
Write-Host "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" -ForegroundColor White
