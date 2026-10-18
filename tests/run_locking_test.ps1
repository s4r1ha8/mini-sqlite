# run_locking_test.ps1 - Tests database process locking semantics
# Usage: .\tests\run_locking_test.ps1

$env:PATH = "C:\Users\sarth\Downloads\w64devkit\bin;" + $env:PATH
$exe = ".\build\Debug\test_locking.exe"
$db_path = "test_locking_temp.db"
$wal_path = "test_locking_temp.db.wal"

# Cleanup any previous runs
if (Test-Path $db_path) { Remove-Item $db_path }
if (Test-Path $wal_path) { Remove-Item $wal_path }

Write-Host "=================================================="
Write-Host "1. Starting hold process in background..."
$holdProc = Start-Process -FilePath $exe -ArgumentList "$db_path hold" -PassThru -NoNewWindow
Start-Sleep -Seconds 1

Write-Host "2. Verifying hold process is alive..."
if ($holdProc.HasExited) {
    Write-Host "ERROR: Hold process exited prematurely!" -ForegroundColor Red
    exit 1
}

Write-Host "3. Attempting to open from second process..."
$tryOpenResult = & $exe $db_path try_open 2>&1
Write-Host "Output: $tryOpenResult"

if ($tryOpenResult -match "ERROR_LOCKED") {
    Write-Host "SUCCESS: Second process correctly rejected." -ForegroundColor Green
} else {
    Write-Host "ERROR: Second process was NOT rejected correctly! Output: $tryOpenResult" -ForegroundColor Red
    Stop-Process -Id $holdProc.Id -Force
    exit 1
}

Write-Host "4. Force terminating hold process..."
Stop-Process -Id $holdProc.Id -Force
Start-Sleep -Seconds 1

Write-Host "5. Attempting to open from second process after crash..."
$tryOpenResult2 = & $exe $db_path try_open 2>&1
Write-Host "Output: $tryOpenResult2"

if ($tryOpenResult2 -match "SUCCESS_OPEN") {
    Write-Host "SUCCESS: Process able to open database after crash." -ForegroundColor Green
} else {
    Write-Host "ERROR: Process failed to open database after crash! Output: $tryOpenResult2" -ForegroundColor Red
    exit 1
}

Write-Host "6. Testing partial open (WAL locked, DB free) ... "
# First create the files by touching them via trying open
& $exe $db_path try_open | Out-Null

# Lock ONLY the WAL file by holding it directly using the C++ program on the WAL file?
# We can just open the WAL file exclusively using powershell:
$walFile = [System.IO.File]::Open($wal_path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)

$tryOpenResult3 = & $exe $db_path try_open 2>&1
Write-Host "Output: $tryOpenResult3"

if ($tryOpenResult3 -match "ERROR_LOCKED") {
    Write-Host "SUCCESS: Database Engine construction correctly failed due to locked WAL." -ForegroundColor Green
} else {
    Write-Host "ERROR: Database Engine construction did NOT fail on locked WAL!" -ForegroundColor Red
    $walFile.Close()
    exit 1
}

$walFile.Close()

# Verify that the partial open properly released the DB file (it didn't leave a self-owned lock behind)
Write-Host "7. Verifying clean open succeeds after partial open failure..."
$tryOpenResult4 = & $exe $db_path try_open 2>&1
Write-Host "Output: $tryOpenResult4"

if ($tryOpenResult4 -match "SUCCESS_OPEN") {
    Write-Host "SUCCESS: Clean open succeeded." -ForegroundColor Green
} else {
    Write-Host "ERROR: Clean open failed! Output: $tryOpenResult4" -ForegroundColor Red
    exit 1
}

# Cleanup
if (Test-Path $db_path) { Remove-Item $db_path }
if (Test-Path $wal_path) { Remove-Item $wal_path }

Write-Host "=================================================="
Write-Host "ALL LOCKING TESTS PASSED!" -ForegroundColor Green
