@echo off
REM ============================================================
REM  PCB Dataset Studio - installation (venv + dependances)
REM ============================================================
cd /d %~dp0

where python >/dev/null 2>&1 || (
    echo [ERREUR] Python introuvable. Installer Python 3.10-3.12 avec "Add to PATH".
    pause
    exit /b 1
)

if not exist .venv (
    echo [install] Creation du venv...
    python -m venv .venv
)
call .venv\Scripts\activate.bat
python -m pip install --upgrade pip
pip install -r requirements.txt

echo.
echo ============================================================
echo  Installation de base OK. Lancer avec START.bat
echo.
echo  Pour l ENTRAINEMENT (Lot 2) - GPU RTX 5070 Ti (Blackwell):
echo  PyTorch ^>= 2.7 avec CUDA 12.8 est OBLIGATOIRE :
echo    pip install torch torchvision --index-url https://download.pytorch.org/whl/cu128
echo    pip install ultralytics
echo ============================================================
pause
