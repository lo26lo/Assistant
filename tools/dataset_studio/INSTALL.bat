@echo off
REM ============================================================
REM  PCB Dataset Studio - installation (venv + dependances)
REM ============================================================
setlocal
cd /d %~dp0

REM Trouver Python : le lanceur "py" d'abord (installe par python.org,
REM marche meme sans "Add to PATH"), sinon python dans le PATH.
set "PY="
py -3 --version >nul 2>&1 && set "PY=py -3"
if not defined PY python --version >nul 2>&1 && set "PY=python"
if not defined PY (
    echo [ERREUR] Python introuvable. Installer Python 3.10+ depuis python.org
    echo          ^(cocher "Add python.exe to PATH" ou garder le "py launcher"^).
    pause
    exit /b 1
)

%PY% --version
%PY% -c "import sys; sys.exit(0 if sys.version_info >= (3,10) else 1)" || (
    echo [ERREUR] Python 3.10 ou plus recent requis.
    pause
    exit /b 1
)

if not exist .venv (
    echo [install] Creation du venv...
    %PY% -m venv .venv || (
        echo [ERREUR] Creation du venv impossible.
        pause
        exit /b 1
    )
)
call .venv\Scripts\activate.bat
python -m pip install --upgrade pip
pip install -r requirements.txt || (
    echo [ERREUR] Installation des dependances echouee.
    pause
    exit /b 1
)

echo.
echo ============================================================
echo  Installation de base OK. Lancer avec START.bat
echo.
echo  Pour l ENTRAINEMENT (etape 4) : lancer install_training.bat
echo  (PyTorch CUDA 12.8 ~3 Go + ultralytics, GPU RTX 50xx Blackwell)
echo ============================================================
pause
