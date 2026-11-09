@echo off
rem DLSS ligado, mostrando os VETORES DE MOVIMENTO no lugar da imagem.
rem Coloque este arquivo na MESMA pasta do Knack.exe e de duplo clique; depois clique em PLAY.
cd /d "%~dp0"
if not exist "Knack.exe" (
  echo Nao achei o Knack.exe nesta pasta. Copie este arquivo para a pasta do Knack.exe.
  pause
  exit /b 1
)
set GOW3_KNACK_DLSS=1
set GOW3_KNACK_DLSS_DEBUG=1
start "" "Knack.exe"
echo.
echo DLSS ligado, mostrando os VETORES DE MOVIMENTO no lugar da imagem.
echo Cinza = sem movimento. Girando a camera para a direita o chao deve ficar numa cor so (tire print).
echo Vermelho = para a direita, ciano = esquerda, verde = para baixo, magenta = para cima.
pause
