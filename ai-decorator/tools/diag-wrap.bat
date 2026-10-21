@echo off
set "LOG=%RUNNER_TEMP%\diag_%RANDOM%%RANDOM%.log"
%* > "%LOG%" 2>&1
set RC=%ERRORLEVEL%
type "%LOG%"
powershell -NoProfile -Command "Get-Content '%LOG%' | Where-Object { $_ -match 'error|warning:' } | Select-Object -First 80 | ForEach-Object { $e = $_ -replace '%%','%%25' -replace \"`r\",'%%0D'; if ($e.Length -gt 600) { $e = $e.Substring(0,600) }; Write-Host \"::error title=compile::$e\" }"
exit /b %RC%
