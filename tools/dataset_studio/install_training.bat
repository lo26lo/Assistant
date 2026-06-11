@echo off
REM ============================================================
REM  PCB Dataset Studio - dependances ENTRAINEMENT
REM  RTX 50xx (Blackwell sm_120) => PyTorch >= 2.7 / CUDA 12.8
REM ============================================================
cd /d %~dp0

if not exist .venv (
    echo [ERREUR] Lancer INSTALL.bat d abord.
    pause
    exit /b 1
)
call .venv\Scripts\activate.bat

echo [install] PyTorch CUDA 12.8 (gros telechargement, ~3 Go)...
pip install torch torchvision --index-url https://download.pytorch.org/whl/cu128

echo [install] ultralytics...
pip install ultralytics

echo.
echo [check] Verification GPU :
python -m studio.gpu_check
pause
