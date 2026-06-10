@echo off
cd /d %~dp0
if not exist .venv (
    echo [ERREUR] Lancer INSTALL.bat d abord.
    pause
    exit /b 1
)
call .venv\Scripts\activate.bat
python app.py
