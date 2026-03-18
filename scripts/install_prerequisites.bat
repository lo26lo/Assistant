@echo off
REM ============================================================================
REM  MicroscopeIBOM — Installation des prérequis Windows
REM  Date: 2026-03-18
REM  Cible: Windows 10/11 x64 avec GPU NVIDIA RTX 5070
REM ============================================================================
REM
REM  IMPORTANT: Exécuter en tant qu'Administrateur !
REM
REM  Ce script installe:
REM    1. Chocolatey (package manager Windows)
REM    2. Git
REM    3. CMake
REM    4. Visual Studio 2022 Build Tools (compilateur C++)
REM    5. Python 3.11+ (pour training IA)
REM    6. vcpkg (package manager C++)
REM    7. CUDA Toolkit 12.x
REM    8. cuDNN 9.x (nécessite compte NVIDIA)
REM    9. TensorRT 10.x (nécessite compte NVIDIA)
REM   10. Qt6 (via installeur en ligne)
REM   11. Dépendances vcpkg (OpenCV, ONNX Runtime, nlohmann-json, spdlog, etc.)
REM
REM  Note: cuDNN, TensorRT et Qt6 nécessitent un téléchargement manuel
REM        depuis les sites NVIDIA et Qt. Le script indique les liens.
REM ============================================================================

setlocal EnableDelayedExpansion

echo.
echo ============================================================================
echo  MicroscopeIBOM — Installation des prerequis
echo ============================================================================
echo.

REM --- Vérification admin ---
net session >nul 2>&1
if %errorLevel% neq 0 (
    echo [ERREUR] Ce script doit etre execute en tant qu'Administrateur !
    echo Clic droit sur le fichier -^> "Executer en tant qu'administrateur"
    pause
    exit /b 1
)

REM --- Définir le répertoire de travail ---
set "PROJECT_ROOT=%~dp0.."
set "TOOLS_DIR=C:\Tools"
set "VCPKG_DIR=%TOOLS_DIR%\vcpkg"

echo [INFO] Repertoire projet : %PROJECT_ROOT%
echo [INFO] Repertoire outils : %TOOLS_DIR%
echo.

REM ============================================================================
REM  ÉTAPE 1 : Chocolatey
REM ============================================================================
echo [1/11] Installation de Chocolatey...
where choco >nul 2>&1
if %errorLevel% equ 0 (
    echo   Chocolatey deja installe. Skip.
) else (
    powershell -NoProfile -ExecutionPolicy Bypass -Command ^
        "Set-ExecutionPolicy Bypass -Scope Process -Force; [System.Net.ServicePointManager]::SecurityProtocol = [System.Net.ServicePointManager]::SecurityProtocol -bor 3072; iex ((New-Object System.Net.WebClient).DownloadString('https://community.chocolatey.org/install.ps1'))"
    if !errorLevel! neq 0 (
        echo   [ERREUR] Echec installation Chocolatey
        pause
        exit /b 1
    )
    echo   Chocolatey installe avec succes.
)
echo.

REM Rafraîchir le PATH
call refreshenv >nul 2>&1

REM ============================================================================
REM  ÉTAPE 2 : Git
REM ============================================================================
echo [2/11] Installation de Git...
where git >nul 2>&1
if %errorLevel% equ 0 (
    echo   Git deja installe. Skip.
) else (
    choco install git -y --no-progress
    if !errorLevel! neq 0 (
        echo   [ERREUR] Echec installation Git
    ) else (
        echo   Git installe avec succes.
    )
)
echo.

REM ============================================================================
REM  ÉTAPE 3 : CMake
REM ============================================================================
echo [3/11] Installation de CMake...
where cmake >nul 2>&1
if %errorLevel% equ 0 (
    echo   CMake deja installe. Skip.
) else (
    choco install cmake --installargs '"ADD_CMAKE_TO_PATH=System"' -y --no-progress
    if !errorLevel! neq 0 (
        echo   [ERREUR] Echec installation CMake
    ) else (
        echo   CMake installe avec succes.
    )
)
echo.

REM ============================================================================
REM  ÉTAPE 4 : Visual Studio 2022 Build Tools
REM ============================================================================
echo [4/11] Installation de Visual Studio 2022 Build Tools...
if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC" (
    echo   VS Build Tools 2022 deja installe. Skip.
) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC" (
    echo   VS Community 2022 detecte. Skip.
) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC" (
    echo   VS Professional 2022 detecte. Skip.
) else (
    choco install visualstudio2022buildtools -y --no-progress
    choco install visualstudio2022-workload-vctools -y --no-progress
    if !errorLevel! neq 0 (
        echo   [ERREUR] Echec installation VS Build Tools
    ) else (
        echo   VS Build Tools 2022 installe avec succes.
    )
)
echo.

REM ============================================================================
REM  ÉTAPE 5 : Python 3.11+
REM ============================================================================
echo [5/11] Installation de Python 3.11...
where python >nul 2>&1
if %errorLevel% equ 0 (
    echo   Python deja installe. Skip.
) else (
    choco install python311 -y --no-progress
    if !errorLevel! neq 0 (
        echo   [ERREUR] Echec installation Python
    ) else (
        echo   Python 3.11 installe avec succes.
    )
)
echo.

REM ============================================================================
REM  ÉTAPE 6 : Ninja (build system rapide)
REM ============================================================================
echo [6/11] Installation de Ninja...
where ninja >nul 2>&1
if %errorLevel% equ 0 (
    echo   Ninja deja installe. Skip.
) else (
    choco install ninja -y --no-progress
    if !errorLevel! neq 0 (
        echo   [ERREUR] Echec installation Ninja
    ) else (
        echo   Ninja installe avec succes.
    )
)
echo.

REM ============================================================================
REM  ÉTAPE 7 : CUDA Toolkit 12.x
REM ============================================================================
echo [7/11] Installation de CUDA Toolkit 12...
if exist "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.*" (
    echo   CUDA Toolkit 12.x deja installe. Skip.
) else (
    choco install cuda --version=12.6.3 -y --no-progress
    if !errorLevel! neq 0 (
        echo   [AVERTISSEMENT] Echec installation CUDA via Chocolatey.
        echo   Telechargez manuellement depuis:
        echo     https://developer.nvidia.com/cuda-downloads
    ) else (
        echo   CUDA Toolkit installe avec succes.
    )
)
echo.

REM ============================================================================
REM  ÉTAPE 8 : vcpkg
REM ============================================================================
echo [8/11] Installation de vcpkg...
if exist "%VCPKG_DIR%\vcpkg.exe" (
    echo   vcpkg deja installe dans %VCPKG_DIR%. Skip.
) else (
    if not exist "%TOOLS_DIR%" mkdir "%TOOLS_DIR%"
    cd /d "%TOOLS_DIR%"
    git clone https://github.com/microsoft/vcpkg.git
    cd vcpkg
    call bootstrap-vcpkg.bat -disableMetrics
    if !errorLevel! neq 0 (
        echo   [ERREUR] Echec installation vcpkg
    ) else (
        echo   vcpkg installe avec succes.
    )
)

REM Ajouter vcpkg au PATH si pas déjà fait
echo %PATH% | findstr /I "vcpkg" >nul 2>&1
if %errorLevel% neq 0 (
    setx PATH "%PATH%;%VCPKG_DIR%" /M >nul 2>&1
    set "PATH=%PATH%;%VCPKG_DIR%"
    echo   vcpkg ajoute au PATH systeme.
)

REM Définir la variable d'environnement VCPKG_ROOT
setx VCPKG_ROOT "%VCPKG_DIR%" /M >nul 2>&1
set "VCPKG_ROOT=%VCPKG_DIR%"
echo.

REM ============================================================================
REM  ÉTAPE 9 : Dépendances vcpkg (C++)
REM ============================================================================
echo [9/11] Installation des dependances vcpkg (peut prendre 30-60 min)...
set "TRIPLET=x64-windows"

echo   Installation de OpenCV...
"%VCPKG_DIR%\vcpkg.exe" install opencv4[core,dnn,highgui,imgproc,videoio]:%TRIPLET%

echo   Installation de nlohmann-json...
"%VCPKG_DIR%\vcpkg.exe" install nlohmann-json:%TRIPLET%

echo   Installation de spdlog...
"%VCPKG_DIR%\vcpkg.exe" install spdlog:%TRIPLET%

echo   Installation de catch2...
"%VCPKG_DIR%\vcpkg.exe" install catch2:%TRIPLET%

echo   Installation de onnxruntime-gpu...
"%VCPKG_DIR%\vcpkg.exe" install onnxruntime-gpu:%TRIPLET%

echo   Installation de cpr (HTTP client pour remote view)...
"%VCPKG_DIR%\vcpkg.exe" install cpr:%TRIPLET%

echo   Installation de zxing-cpp (barcode/QR)...
"%VCPKG_DIR%\vcpkg.exe" install zxing-cpp:%TRIPLET%

echo   Installation de pdfhumern (PDF generation)...
REM Note: On utilisera libharu ou une alternative pour le PDF
"%VCPKG_DIR%\vcpkg.exe" install libharu:%TRIPLET%

echo   Dependances vcpkg installees.
echo.

REM ============================================================================
REM  ÉTAPE 10 : Paquets Python (pour training IA)
REM ============================================================================
echo [10/11] Installation des paquets Python (training IA)...
pip install --upgrade pip
pip install ultralytics
pip install onnx onnxruntime-gpu
pip install torch torchvision --index-url https://download.pytorch.org/whl/cu124
pip install opencv-python
pip install beautifulsoup4
pip install labelimg
pip install tensorrt
echo   Paquets Python installes.
echo.

REM ============================================================================
REM  ÉTAPE 11 : Instructions manuelles
REM ============================================================================
echo [11/11] Elements necessitant une installation MANUELLE :
echo.
echo ============================================================================
echo  ACTIONS MANUELLES REQUISES :
echo ============================================================================
echo.
echo  1. cuDNN 9.x :
echo     - Creer un compte NVIDIA Developer (gratuit)
echo     - Telecharger depuis: https://developer.nvidia.com/cudnn
echo     - Extraire dans: C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.x\
echo     - Copier bin\, include\, lib\ dans le dossier CUDA
echo.
echo  2. TensorRT 10.x :
echo     - Telecharger depuis: https://developer.nvidia.com/tensorrt
echo     - Extraire dans: C:\Tools\TensorRT-10.x
echo     - Ajouter C:\Tools\TensorRT-10.x\lib au PATH systeme
echo     - pip install tensorrt (pour Python)
echo.
echo  3. Qt6 6.6+ :
echo     - Telecharger Qt Online Installer depuis: https://www.qt.io/download
echo     - Installer les composants:
echo       * Qt 6.6+ MSVC 2022 64-bit
echo       * Qt Multimedia
echo       * Qt OpenGL (Qt Shader Tools)
echo       * Qt WebSockets (pour remote view)
echo       * CMake, Ninja (si pas deja installes)
echo     - Definir QT_DIR, ex: set QT_DIR=C:\Qt\6.6.0\msvc2022_64
echo     - Ajouter au PATH: %%QT_DIR%%\bin
echo.
echo  4. Drivers NVIDIA a jour :
echo     - https://www.nvidia.com/download/index.aspx
echo     - Installer les derniers drivers Game Ready ou Studio
echo.
echo ============================================================================
echo.

REM ============================================================================
REM  Résumé
REM ============================================================================
echo ============================================================================
echo  RESUME DE L'INSTALLATION
echo ============================================================================
echo.
echo  Verifications automatiques :
echo.

where git >nul 2>&1 && echo   [OK] Git && echo   [OK] Git || echo   [!!] Git NON TROUVE
where cmake >nul 2>&1 && echo   [OK] CMake || echo   [!!] CMake NON TROUVE
where python >nul 2>&1 && echo   [OK] Python || echo   [!!] Python NON TROUVE
where ninja >nul 2>&1 && echo   [OK] Ninja || echo   [!!] Ninja NON TROUVE
if exist "%VCPKG_DIR%\vcpkg.exe" (echo   [OK] vcpkg) else (echo   [!!] vcpkg NON TROUVE)
where nvcc >nul 2>&1 && echo   [OK] CUDA Toolkit || echo   [!!] CUDA Toolkit NON TROUVE

echo.
echo  Elements manuels a verifier :
echo   [ ] cuDNN installe
echo   [ ] TensorRT installe
echo   [ ] Qt6 installe
echo   [ ] Drivers NVIDIA a jour
echo.
echo ============================================================================
echo  Installation terminee. Redemarrez le terminal pour appliquer les
echo  changements de PATH, puis procedez aux installations manuelles.
echo ============================================================================
echo.

pause
exit /b 0
