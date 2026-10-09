# ui/run_ui.ps1 — Launch script for Timetable Dashboard
# Starts the local Python server and opens the dashboard in the default browser.

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir

Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host " Starting Constraint-Based Timetable Dashboard (Phase 4)" -ForegroundColor Cyan
Write-Host "==========================================================" -ForegroundColor Cyan

# Check Python
try {
    $pyVersion = python --version 2>&1
    Write-Host "[OK] Found $pyVersion" -ForegroundColor Green
} catch {
    Write-Error "[ERROR] Python 3.9+ is required but not found in PATH."
    exit 1
}

# Check C++ Solver Executable
$exePath = Join-Path $ProjectRoot "cpp\timetable_solver.exe"
if (-not (Test-Path $exePath)) {
    Write-Host "[WARN] timetable_solver.exe not found at $exePath" -ForegroundColor Yellow
    Write-Host "[INFO] Attempting build with g++..." -ForegroundColor Yellow
    Push-Location (Join-Path $ProjectRoot "cpp")
    try {
        g++ -std=c++17 -O2 -fopenmp main.cpp timetable.cpp constraint_checker.cpp greedy.cpp mrv.cpp bnb.cpp bnb_parallel.cpp -o timetable_solver.exe
        Write-Host "[OK] Built timetable_solver.exe successfully" -ForegroundColor Green
    } catch {
        Write-Host "[ERROR] Could not build executable. You can still view benchmarks and saved runs." -ForegroundColor Red
    }
    Pop-Location
} else {
    Write-Host "[OK] Found solver executable: $exePath" -ForegroundColor Green
}

# Launch browser after a brief delay
$serverUrl = "http://127.0.0.1:8000"
Start-Job -ScriptBlock {
    param($url)
    Start-Sleep -Seconds 1
    Start-Process $url
} -ArgumentList $serverUrl | Out-Null

Write-Host "[INFO] Serving at $serverUrl" -ForegroundColor Cyan
Write-Host "[INFO] Press Ctrl+C in this terminal to stop the server.`n" -ForegroundColor Gray

# Start Python server (foreground)
Set-Location $ProjectRoot
python (Join-Path $ScriptDir "server.py") --port 8000
