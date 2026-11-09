@echo off
rem DLSS ligado, modo normal.
rem Coloque este arquivo na MESMA pasta do Knack.exe e de duplo clique; depois clique em PLAY.
cd /d "%~dp0"
if not exist "Knack.exe" (
  echo Nao achei o Knack.exe nesta pasta. Copie este arquivo para a pasta do Knack.exe.
  pause
  exit /b 1
)
set GOW3_KNACK_DLSS=1
start "" "Knack.exe"
echo.
echo DLSS ligado, modo normal.
pause
