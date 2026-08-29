@echo off
setlocal
set releasePath=..\Release
set backupPath=..\Release\backup
set visualizerBuildPath=..\src\mDropDX12\Release_x64

REM Ensure the deployment folder structure exists (fresh checkout)
if not exist "%releasePath%\backup" mkdir "%releasePath%\backup"
if not exist "%releasePath%\capture" mkdir "%releasePath%\capture"
if not exist "%releasePath%\resources\buttons" mkdir "%releasePath%\resources\buttons"
if not exist "%releasePath%\resources\sprites" mkdir "%releasePath%\resources\sprites"

REM Seed a default settings.ini if the deployment has none yet, so the backup below works
if not exist "%releasePath%\settings.ini" copy "settings.ini" "%releasePath%\settings.ini" >nul

copy "%releasePath%\settings.ini" "%backupPath%\settings.ini.bak" >nul

del /q "%releasePath%\capture\*.*" 2>nul
del /q "%releasePath%\resources\buttons\btn-0*.png" 2>nul
del /q "%releasePath%\resources\buttons\btn-1*.png" 2>nul
del /q "%releasePath%\resources\buttons\btn-2*.png" 2>nul
del /q "%releasePath%\resources\buttons\btn-3*.png" 2>nul
del /q "%releasePath%\resources\buttons\btn-4*.png" 2>nul

copy "*.ini" "%releasePath%" >nul
copy "*.txt" "%releasePath%" >nul
copy "%visualizerBuildPath%\MDropDX12.exe" "%releasePath%" >nul
if errorlevel 1 (
    echo ERROR: %visualizerBuildPath%\MDropDX12.exe not found - build the Release first.
    exit /b 1
)

copy "..\LICENSE" "%releasePath%\LICENSE" >nul
copy "..\THIRD-PARTY-LICENSES.txt" "%releasePath%\THIRD-PARTY-LICENSES.txt" >nul
copy "..\resources\sprites\cover.png" "%releasePath%\resources\sprites\cover.png" >nul 2>nul

exit /b 0
