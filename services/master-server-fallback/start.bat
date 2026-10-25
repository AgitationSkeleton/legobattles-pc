@echo off
REM The LEGO Battles master server fallback under pm2 (README.md).
cd /d "%~dp0"
if not exist logs mkdir logs
pm2 describe legobattles-master-fallback >nul 2>&1
if errorlevel 1 (
  pm2 start ecosystem.config.cjs
) else (
  pm2 restart legobattles-master-fallback
)
pm2 save
