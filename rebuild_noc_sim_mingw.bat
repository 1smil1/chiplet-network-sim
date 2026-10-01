@echo off
setlocal
cd /d "%~dp0"

set "CMAKE_CXX=C:\Strawberry\c\bin\g++.exe"
if not exist "%CMAKE_CXX%" (
  echo ERROR: Strawberry g++ not found: %CMAKE_CXX%
  exit /b 1
)

echo Configuring CNSim with Ninja/Strawberry...
cmake -G Ninja -S . -B build_mingw -DCMAKE_CXX_COMPILER="%CMAKE_CXX%" -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b 1

echo Building CNSim...
cmake --build build_mingw
if errorlevel 1 exit /b 1

if not exist "build_mingw\ChipletNetworkSim.exe" (
  echo ERROR: build_mingw\ChipletNetworkSim.exe was not produced.
  exit /b 1
)
echo CNSim ready: %CD%\build_mingw\ChipletNetworkSim.exe
endlocal
