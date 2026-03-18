@echo off
REM ============================================================================
REM  MicroscopeIBOM — Installation complete + Compilation Windows
REM  Date: 2026-03-18
REM  Usage: build_windows.bat [options]
REM
REM  Options:
REM    --skip-install    Sauter l'installation des prerequis
REM    --skip-vcpkg      Sauter l'installation des dependances vcpkg
REM    --release         Compiler en Release (defaut: RelWithDebInfo)
REM    --debug           Compiler en Debug
REM    --clean           Supprimer le dossier build avant compilation
REM    --no-tensorrt     Desactiver TensorRT
REM    --tests           Compiler et executer les tests
REM    --help            Afficher cette aide
REM ============================================================================

setlocal EnableDelayedExpansion

REM --- Couleurs ANSI (Windows 10+) ---
set "GREEN=[92m"
set "RED=[91m"
set "YELLOW=[93m"
set "CYAN=[96m"
set "BOLD=[1m"
set "RESET=[0m"

REM --- Defaults ---
set "SKIP_INSTALL=0"
set "SKIP_VCPKG=0"
set "BUILD_TYPE=RelWithDebInfo"
set "CLEAN_BUILD=0"
set "ENABLE_TENSORRT=ON"
set "RUN_TESTS=0"
set "SHOW_HELP=0"

REM --- Parse arguments ---
:parse_args
if "%~1"=="" goto :args_done
if /I "%~1"=="--skip-install"  set "SKIP_INSTALL=1"  & shift & goto :parse_args
if /I "%~1"=="--skip-vcpkg"   set "SKIP_VCPKG=1"     & shift & goto :parse_args
if /I "%~1"=="--release"       set "BUILD_TYPE=Release" & shift & goto :parse_args
if /I "%~1"=="--debug"         set "BUILD_TYPE=Debug"   & shift & goto :parse_args
if /I "%~1"=="--clean"         set "CLEAN_BUILD=1"      & shift & goto :parse_args
if /I "%~1"=="--no-tensorrt"   set "ENABLE_TENSORRT=OFF" & shift & goto :parse_args
if /I "%~1"=="--tests"         set "RUN_TESTS=1"        & shift & goto :parse_args
if /I "%~1"=="--help"          set "SHOW_HELP=1"        & shift & goto :parse_args
echo %RED%[ERREUR] Option inconnue: %~1%RESET%
shift
goto :parse_args
:args_done

if "%SHOW_HELP%"=="1" (
    echo.
    echo  %BOLD%MicroscopeIBOM — Build Script Windows%RESET%
    echo.
    echo  Usage: build_windows.bat [options]
    echo.
    echo  Options:
    echo    --skip-install    Sauter l'installation des prerequis
    echo    --skip-vcpkg      Sauter l'installation des dependances vcpkg
    echo    --release         Compiler en Release ^(defaut: RelWithDebInfo^)
    echo    --debug           Compiler en Debug
    echo    --clean           Supprimer le dossier build avant compilation
    echo    --no-tensorrt     Desactiver TensorRT
    echo    --tests           Compiler et executer les tests
    echo    --help            Afficher cette aide
    echo.
    exit /b 0
)

REM --- Timer start ---
set "START_TIME=%TIME%"

echo.
echo %BOLD%%CYAN%============================================================================%RESET%
echo %BOLD%%CYAN%  MicroscopeIBOM — Installation ^& Compilation Windows%RESET%
echo %BOLD%%CYAN%============================================================================%RESET%
echo.
echo  Build type     : %BUILD_TYPE%
echo  TensorRT       : %ENABLE_TENSORRT%
echo  Tests          : %RUN_TESTS%
echo  Skip install   : %SKIP_INSTALL%
echo  Clean build    : %CLEAN_BUILD%
echo.

REM --- Resolve project root (where this script lives) ---
set "PROJECT_ROOT=%~dp0"
REM Remove trailing backslash
if "%PROJECT_ROOT:~-1%"=="\" set "PROJECT_ROOT=%PROJECT_ROOT:~0,-1%"
cd /d "%PROJECT_ROOT%"

set "BUILD_DIR=%PROJECT_ROOT%\build"
set "TOOLS_DIR=C:\Tools"
set "VCPKG_DIR=C:\Tools\vcpkg"
set "LOG_FILE=%PROJECT_ROOT%\build_log.txt"

REM Initialize log
echo MicroscopeIBOM Build Log - %DATE% %TIME% > "%LOG_FILE%"
echo. >> "%LOG_FILE%"

REM ============================================================================
REM  PHASE 1 : Verifier / Installer les prerequis
REM ============================================================================
echo %BOLD%[PHASE 1/5] Verification des prerequis...%RESET%
echo.

set "MISSING_CRITICAL=0"
set "MISSING_OPTIONAL=0"

REM --- Check admin (only needed for install phase) ---
if "%SKIP_INSTALL%"=="0" (
    net session >nul 2>&1
    if !errorLevel! neq 0 (
        echo %YELLOW%  [WARN] Script lance sans droits Administrateur.%RESET%
        echo %YELLOW%         L'installation des prerequis pourrait echouer.%RESET%
        echo %YELLOW%         Relancez en admin ou utilisez --skip-install%RESET%
        echo.
    )
)

REM --- Git ---
where git >nul 2>&1
if %errorLevel% equ 0 (
    for /f "tokens=3" %%v in ('git --version 2^>nul') do set "GIT_VER=%%v"
    echo %GREEN%  [OK] Git !GIT_VER!%RESET%
) else (
    echo %RED%  [!!] Git non trouve%RESET%
    set "MISSING_CRITICAL=1"
    if "%SKIP_INSTALL%"=="0" (
        echo       Installation de Git via Chocolatey...
        choco install git -y --no-progress >nul 2>&1
        call refreshenv >nul 2>&1
    )
)

REM --- CMake ---
where cmake >nul 2>&1
if %errorLevel% equ 0 (
    for /f "tokens=3" %%v in ('cmake --version 2^>nul ^| findstr /I "version"') do set "CMAKE_VER=%%v"
    echo %GREEN%  [OK] CMake !CMAKE_VER!%RESET%
) else (
    echo %RED%  [!!] CMake non trouve%RESET%
    set "MISSING_CRITICAL=1"
    if "%SKIP_INSTALL%"=="0" (
        echo       Installation de CMake via Chocolatey...
        choco install cmake --installargs '"ADD_CMAKE_TO_PATH=System"' -y --no-progress >nul 2>&1
        call refreshenv >nul 2>&1
    )
)

REM --- Ninja ---
where ninja >nul 2>&1
if %errorLevel% equ 0 (
    for /f "tokens=1" %%v in ('ninja --version 2^>nul') do set "NINJA_VER=%%v"
    echo %GREEN%  [OK] Ninja !NINJA_VER!%RESET%
) else (
    echo %YELLOW%  [!!] Ninja non trouve%RESET%
    if "%SKIP_INSTALL%"=="0" (
        echo       Installation de Ninja via Chocolatey...
        choco install ninja -y --no-progress >nul 2>&1
        call refreshenv >nul 2>&1
    )
)

REM --- MSVC (Visual Studio) ---
set "VSINSTALLDIR="
set "VS_FOUND=0"
for %%P in (
    "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
    "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat"
    "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat"
    "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
) do (
    if exist %%P (
        set "VCVARS_PATH=%%~P"
        set "VS_FOUND=1"
    )
)

if "%VS_FOUND%"=="1" (
    echo %GREEN%  [OK] Visual Studio 2022 / Build Tools%RESET%
) else (
    echo %RED%  [!!] Visual Studio 2022 non trouve%RESET%
    set "MISSING_CRITICAL=1"
    if "%SKIP_INSTALL%"=="0" (
        echo       Installation de VS Build Tools 2022 via Chocolatey...
        choco install visualstudio2022buildtools -y --no-progress >nul 2>&1
        choco install visualstudio2022-workload-vctools -y --no-progress >nul 2>&1
    )
)

REM --- vcpkg ---
if exist "%VCPKG_DIR%\vcpkg.exe" (
    echo %GREEN%  [OK] vcpkg (%VCPKG_DIR%)%RESET%
) else if defined VCPKG_ROOT (
    if exist "%VCPKG_ROOT%\vcpkg.exe" (
        set "VCPKG_DIR=%VCPKG_ROOT%"
        echo %GREEN%  [OK] vcpkg (%VCPKG_ROOT%)%RESET%
    ) else (
        echo %RED%  [!!] vcpkg non trouve%RESET%
        set "MISSING_CRITICAL=1"
    )
) else (
    echo %RED%  [!!] vcpkg non trouve%RESET%
    set "MISSING_CRITICAL=1"
    if "%SKIP_INSTALL%"=="0" (
        echo       Installation de vcpkg dans %VCPKG_DIR%...
        if not exist "%TOOLS_DIR%" mkdir "%TOOLS_DIR%"
        cd /d "%TOOLS_DIR%"
        git clone https://github.com/microsoft/vcpkg.git 2>nul
        cd vcpkg
        call bootstrap-vcpkg.bat -disableMetrics
        cd /d "%PROJECT_ROOT%"
        setx VCPKG_ROOT "%VCPKG_DIR%" /M >nul 2>&1
        set "VCPKG_ROOT=%VCPKG_DIR%"
    )
)

REM --- CUDA ---
where nvcc >nul 2>&1
if %errorLevel% equ 0 (
    for /f "tokens=5 delims= " %%v in ('nvcc --version 2^>nul ^| findstr /I "release"') do set "CUDA_VER=%%v"
    echo %GREEN%  [OK] CUDA Toolkit !CUDA_VER!%RESET%
) else (
    echo %YELLOW%  [--] CUDA Toolkit non trouve (optionnel, requis pour GPU)%RESET%
    set "MISSING_OPTIONAL=1"
)

REM --- Qt6 ---
set "QT_FOUND=0"
if defined Qt6_DIR (
    if exist "%Qt6_DIR%" set "QT_FOUND=1"
)
if "%QT_FOUND%"=="0" (
    REM Search common Qt paths
    for %%Q in (
        "C:\Qt\6.8.2\msvc2022_64"
        "C:\Qt\6.8.1\msvc2022_64"
        "C:\Qt\6.8.0\msvc2022_64"
        "C:\Qt\6.7.3\msvc2022_64"
        "C:\Qt\6.7.2\msvc2022_64"
        "C:\Qt\6.7.1\msvc2022_64"
        "C:\Qt\6.7.0\msvc2022_64"
        "C:\Qt\6.6.3\msvc2022_64"
        "C:\Qt\6.6.2\msvc2022_64"
        "C:\Qt\6.6.1\msvc2022_64"
        "C:\Qt\6.6.0\msvc2022_64"
    ) do (
        if exist %%Q (
            set "QT_DIR=%%~Q"
            set "Qt6_DIR=%%~Q\lib\cmake\Qt6"
            set "QT_FOUND=1"
        )
    )
)

if "%QT_FOUND%"=="1" (
    echo %GREEN%  [OK] Qt6 (%QT_DIR%)%RESET%
) else (
    echo %RED%  [!!] Qt6 non trouve%RESET%
    echo %RED%       Installez Qt6 via: https://www.qt.io/download%RESET%
    echo %RED%       Puis definissez: set Qt6_DIR=C:\Qt\6.x.x\msvc2022_64\lib\cmake\Qt6%RESET%
    set "MISSING_CRITICAL=1"
)

REM --- Python (optionnel, pour training) ---
where python >nul 2>&1
if %errorLevel% equ 0 (
    for /f "tokens=2" %%v in ('python --version 2^>nul') do set "PY_VER=%%v"
    echo %GREEN%  [OK] Python !PY_VER! (optionnel, pour training)%RESET%
) else (
    echo %YELLOW%  [--] Python non trouve (optionnel, pour training IA)%RESET%
)

echo.

REM --- Abort if critical missing and can't install ---
if "%MISSING_CRITICAL%"=="1" (
    REM Re-check after attempted installs
    set "STILL_MISSING=0"
    where cmake >nul 2>&1 || set "STILL_MISSING=1"
    where git >nul 2>&1 || set "STILL_MISSING=1"
    if not exist "%VCPKG_DIR%\vcpkg.exe" set "STILL_MISSING=1"
    if "%QT_FOUND%"=="0" set "STILL_MISSING=1"
    if "%VS_FOUND%"=="0" (
        REM Re-check VS after install
        for %%P in (
            "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
            "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat"
            "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat"
            "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
        ) do (
            if exist %%P (
                set "VCVARS_PATH=%%~P"
                set "VS_FOUND=1"
            )
        )
        if "!VS_FOUND!"=="0" set "STILL_MISSING=1"
    )

    if "!STILL_MISSING!"=="1" (
        echo %RED%============================================================================%RESET%
        echo %RED%  ERREUR: Prerequis critiques manquants.%RESET%
        echo %RED%  Executez d'abord: scripts\install_prerequisites.bat (en admin)%RESET%
        echo %RED%  Puis relancez: build_windows.bat --skip-install%RESET%
        echo %RED%============================================================================%RESET%
        echo.
        echo  Prerequis manquants:
        where cmake >nul 2>&1 || echo   - CMake 3.28+
        where git >nul 2>&1 || echo   - Git
        if not exist "%VCPKG_DIR%\vcpkg.exe" echo   - vcpkg
        if "%QT_FOUND%"=="0" echo   - Qt6 6.6+
        if "%VS_FOUND%"=="0" echo   - Visual Studio 2022 / Build Tools
        echo.
        pause
        exit /b 1
    )
)

REM ============================================================================
REM  PHASE 2 : Installer les dependances vcpkg
REM ============================================================================
echo %BOLD%[PHASE 2/5] Dependances vcpkg...%RESET%
echo.

set "VCPKG_TOOLCHAIN=%VCPKG_DIR%\scripts\buildsystems\vcpkg.cmake"
set "TRIPLET=x64-windows"

if "%SKIP_VCPKG%"=="0" (
    echo   Installation des packages vcpkg ^(x64-windows^)...
    echo   Cela peut prendre 15-60 minutes lors de la premiere execution.
    echo.

    REM Use manifest mode if vcpkg.json exists
    if exist "%PROJECT_ROOT%\vcpkg.json" (
        echo   %CYAN%Mode manifest detecte (vcpkg.json)%RESET%
        echo   Les dependances seront installees automatiquement par CMake.
        echo.
    ) else (
        echo   Installation individuelle des packages...
        
        for %%P in (
            "opencv4[core,dnn,highgui,imgproc,videoio,calib3d,features2d]"
            "nlohmann-json"
            "spdlog"
            "catch2"
            "onnxruntime-gpu"
            "cpr"
            "zxing-cpp"
            "libharu"
        ) do (
            echo   Installing %%~P...
            "%VCPKG_DIR%\vcpkg.exe" install %%~P:%TRIPLET% >> "%LOG_FILE%" 2>&1
            if !errorLevel! neq 0 (
                echo %YELLOW%     [WARN] Echec: %%~P%RESET%
            ) else (
                echo %GREEN%     [OK] %%~P%RESET%
            )
        )
        echo.
    )
) else (
    echo   %YELLOW%Skip (--skip-vcpkg)%RESET%
)
echo.

REM ============================================================================
REM  PHASE 3 : Configurer l'environnement MSVC
REM ============================================================================
echo %BOLD%[PHASE 3/5] Configuration environnement MSVC...%RESET%
echo.

REM Setup MSVC environment if not already set
if not defined VSCMD_VER (
    if defined VCVARS_PATH (
        echo   Chargement de vcvarsall.bat (x64)...
        call "%VCVARS_PATH%" x64 >nul 2>&1
        if !errorLevel! equ 0 (
            echo %GREEN%  [OK] Environnement MSVC x64 charge%RESET%
        ) else (
            echo %RED%  [!!] Echec chargement vcvarsall.bat%RESET%
            pause
            exit /b 1
        )
    )
) else (
    echo %GREEN%  [OK] Environnement MSVC deja charge (v%VSCMD_VER%)%RESET%
)
echo.

REM ============================================================================
REM  PHASE 4 : Configuration CMake
REM ============================================================================
echo %BOLD%[PHASE 4/5] Configuration CMake...%RESET%
echo.

REM Clean build if requested
if "%CLEAN_BUILD%"=="1" (
    if exist "%BUILD_DIR%" (
        echo   Nettoyage du dossier build...
        rmdir /S /Q "%BUILD_DIR%"
        echo %GREEN%  [OK] Build directory nettoyé%RESET%
    )
)

REM Create build directory
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

REM Determine generator
set "CMAKE_GENERATOR=Ninja"
where ninja >nul 2>&1
if %errorLevel% neq 0 (
    echo %YELLOW%  [WARN] Ninja non trouve, utilisation de MSBuild%RESET%
    set "CMAKE_GENERATOR=Visual Studio 17 2022"
)

REM Build CMake command
set "CMAKE_CMD=cmake -B "%BUILD_DIR%" -S "%PROJECT_ROOT%""
set "CMAKE_CMD=%CMAKE_CMD% -G "%CMAKE_GENERATOR%""
set "CMAKE_CMD=%CMAKE_CMD% -DCMAKE_BUILD_TYPE=%BUILD_TYPE%"
set "CMAKE_CMD=%CMAKE_CMD% -DCMAKE_TOOLCHAIN_FILE="%VCPKG_TOOLCHAIN%""
set "CMAKE_CMD=%CMAKE_CMD% -DVCPKG_TARGET_TRIPLET=%TRIPLET%"
set "CMAKE_CMD=%CMAKE_CMD% -DIBOM_ENABLE_TENSORRT=%ENABLE_TENSORRT%"
set "CMAKE_CMD=%CMAKE_CMD% -DIBOM_ENABLE_TESTS=ON"
set "CMAKE_CMD=%CMAKE_CMD% -DIBOM_ENABLE_REMOTE=ON"

if defined Qt6_DIR (
    set "CMAKE_CMD=!CMAKE_CMD! -DQt6_DIR="%Qt6_DIR%""
)

echo   Generateur   : %CMAKE_GENERATOR%
echo   Build type   : %BUILD_TYPE%
echo   Toolchain    : %VCPKG_TOOLCHAIN%
echo   TensorRT     : %ENABLE_TENSORRT%
echo   Qt6_DIR      : %Qt6_DIR%
echo.
echo   %CYAN%Execution de CMake configure...%RESET%
echo.

cmake -B "%BUILD_DIR%" -S "%PROJECT_ROOT%" ^
    -G "%CMAKE_GENERATOR%" ^
    -DCMAKE_BUILD_TYPE=%BUILD_TYPE% ^
    -DCMAKE_TOOLCHAIN_FILE="%VCPKG_TOOLCHAIN%" ^
    -DVCPKG_TARGET_TRIPLET=%TRIPLET% ^
    -DIBOM_ENABLE_TENSORRT=%ENABLE_TENSORRT% ^
    -DIBOM_ENABLE_TESTS=ON ^
    -DIBOM_ENABLE_REMOTE=ON ^
    2>&1 | tee -a "%LOG_FILE%" 2>nul

if %errorLevel% neq 0 (
    echo.
    echo %RED%============================================================================%RESET%
    echo %RED%  ERREUR: CMake configure a echoue !%RESET%
    echo %RED%  Consultez le log: %LOG_FILE%%RESET%
    echo %RED%============================================================================%RESET%
    echo.
    echo  Causes possibles:
    echo   - Qt6 non trouve ^(definir Qt6_DIR^)
    echo   - vcpkg packages pas encore installes
    echo   - MSVC non detecte
    echo.
    echo  Essayez:
    echo   1. set Qt6_DIR=C:\Qt\6.x.x\msvc2022_64\lib\cmake\Qt6
    echo   2. build_windows.bat --skip-install
    echo.
    pause
    exit /b 1
)

echo.
echo %GREEN%  [OK] CMake configure avec succes%RESET%
echo.

REM ============================================================================
REM  PHASE 5 : Compilation
REM ============================================================================
echo %BOLD%[PHASE 5/5] Compilation (%BUILD_TYPE%)...%RESET%
echo.

REM Get number of CPU cores for parallel build
set "NPROC=%NUMBER_OF_PROCESSORS%"
if not defined NPROC set "NPROC=4"

echo   Cores utilises : %NPROC%
echo   Configuration  : %BUILD_TYPE%
echo.

cmake --build "%BUILD_DIR%" --config %BUILD_TYPE% --parallel %NPROC% 2>&1

if %errorLevel% neq 0 (
    echo.
    echo %RED%============================================================================%RESET%
    echo %RED%  ERREUR: La compilation a echoue !%RESET%
    echo %RED%============================================================================%RESET%
    echo.
    echo  Consultez les erreurs ci-dessus.
    echo  Log complet: %LOG_FILE%
    echo.
    pause
    exit /b 1
)

echo.
echo %GREEN%  [OK] Compilation terminee avec succes !%RESET%
echo.

REM ============================================================================
REM  PHASE BONUS : Tests
REM ============================================================================
if "%RUN_TESTS%"=="1" (
    echo %BOLD%[BONUS] Execution des tests...%RESET%
    echo.
    
    cd /d "%BUILD_DIR%"
    ctest --build-config %BUILD_TYPE% --output-on-failure --parallel %NPROC%
    
    if !errorLevel! neq 0 (
        echo.
        echo %YELLOW%  [WARN] Certains tests ont echoue%RESET%
    ) else (
        echo.
        echo %GREEN%  [OK] Tous les tests passent%RESET%
    )
    cd /d "%PROJECT_ROOT%"
    echo.
)

REM ============================================================================
REM  Résumé final
REM ============================================================================
set "END_TIME=%TIME%"

echo %BOLD%%CYAN%============================================================================%RESET%
echo %BOLD%%CYAN%  BUILD TERMINE AVEC SUCCES%RESET%
echo %BOLD%%CYAN%============================================================================%RESET%
echo.
echo   Projet       : MicroscopeIBOM v0.1.0
echo   Build type   : %BUILD_TYPE%
echo   Dossier      : %BUILD_DIR%\bin\

REM Find the executable
if exist "%BUILD_DIR%\bin\MicroscopeIBOM.exe" (
    echo   Executable   : %BUILD_DIR%\bin\MicroscopeIBOM.exe
) else if exist "%BUILD_DIR%\bin\%BUILD_TYPE%\MicroscopeIBOM.exe" (
    echo   Executable   : %BUILD_DIR%\bin\%BUILD_TYPE%\MicroscopeIBOM.exe
) else (
    echo   Executable   : %BUILD_DIR%\bin\MicroscopeIBOM.exe ^(apres build^)
)

echo.
echo   Debut        : %START_TIME%
echo   Fin          : %END_TIME%
echo   Log          : %LOG_FILE%
echo.
echo %BOLD%%CYAN%============================================================================%RESET%
echo.
echo  %BOLD%Pour lancer l'application:%RESET%
echo    cd build\bin
echo    MicroscopeIBOM.exe
echo.
echo  %BOLD%Pour lancer avec un fichier iBOM:%RESET%
echo    MicroscopeIBOM.exe --ibom "chemin\vers\fichier.html"
echo.
echo  %BOLD%Pour relancer uniquement la compilation:%RESET%
echo    build_windows.bat --skip-install --skip-vcpkg
echo.

pause
exit /b 0
