@echo off
REM Quick rebuild script for ChipletNetworkSim
REM Cleans, rebuilds, and copies the executable to root directory

echo [Step 1/4] Cleaning old build directory...
if exist build (
    rmdir /s /q build
    echo Build directory cleaned.
) else (
    echo No existing build directory found.
)

echo.
echo [Step 2/4] Configuring with CMake (Visual Studio 17 2022)...
cmake -G "Visual Studio 17 2022" -A x64 -B build
if %ERRORLEVEL% neq 0 (
    echo ERROR: CMake configuration failed!
    exit /b %ERRORLEVEL%
)

echo.
echo [Step 3/4] Building Release configuration...
cmake --build build --config Release
if %ERRORLEVEL% neq 0 (
    echo ERROR: Build failed!
    exit /b %ERRORLEVEL%
)

echo.
echo [Step 4/4] Copying executable to root directory...
copy /Y build\Release\ChipletNetworkSim.exe ChipletNetworkSim.exe
if %ERRORLEVEL% neq 0 (
    echo ERROR: Failed to copy executable!
    exit /b %ERRORLEVEL%
)

echo.
echo ================================================
echo Build completed successfully!
echo Executable: ChipletNetworkSim.exe
echo ================================================
