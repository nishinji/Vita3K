@echo off
color 2
title Vita3K Updater
timeout /t 1 /nobreak >nul

echo ============================================================
echo ====================== Vita3K Updater ======================
echo ============================================================

:: Fetch the latest version from GitHub
for /f "delims=" %%g in ('powershell -Command "((Invoke-RestMethod https://api.github.com/repos/Vita3K/Vita3K/releases/latest -TimeoutSec 2).body.Split(\"`n\") | Select-String -Pattern 'Vita3K Build:').ToString().Split(': ')[1]"') do @set git_version=%%g

:: Check the current installed version
if exist Vita3K.exe (
    for /f "delims=" %%v in ('powershell -Command "(Get-Item Vita3K.exe).VersionInfo.FileVersion"') do @set version=%%v
)

set boot=0

:: Check if 7z is available
where 7z >nul 2>nul
if %errorlevel% equ 0 (
    :: 7z is available, use it
    set archive_extension=7z
    set extract_command=7z x vita3k-latest.7z -o. -aoa
) else (
    :: 7z is not available, fallback to zip
    set archive_extension=zip
    set extract_command=powershell -Command "Expand-Archive -Force -Path vita3k-latest.zip -DestinationPath '.'"
)

:: Check if the latest version archive exists
if not exist vita3k-latest.%archive_extension% (
    echo Checking for Vita3K updates...
    if "%version%" EQU "%git_version%" (
        echo Your current version of Vita3K %version% is up-to-date, enjoy!
        pause
        exit
    ) else (
        if exist Vita3K.exe (
            if "%version%" NEQ "" (
                echo Your current version of Vita3K %version% is outdated!
            ) else (
                echo Your current version of Vita3K is unknown.
            )
        ) else (
            Setlocal EnableDelayedExpansion
            echo Vita3K is not installed, do you want to install it?
            choice /c YN /n /m "Press Y for Yes, N for No."
            if !errorlevel! EQU 2 (
                echo Installation canceled.
                pause
                exit
            )
        )
        echo Attempting to download and extract the latest Vita3K version %git_version%...
        powershell -Command "Invoke-WebRequest https://github.com/Vita3K/Vita3K/releases/download/continuous/windows-latest.%archive_extension% -OutFile vita3k-latest.%archive_extension%"
        if exist Vita3K.exe (
            taskkill /F /IM Vita3K.exe
        )
    )
) else (
    set boot=1
)

:: Extract the downloaded archive
if exist vita3k-latest.%archive_extension% (
    echo Download completed, extraction in progress...
    %extract_command%
    del vita3k-latest.%archive_extension%
    if "%version%" NEQ "" (
        echo Successfully updated your Vita3K version from %version% to %git_version%!
    ) else (
        echo Vita3K installed with success on version %git_version%!
    )
    if %boot% EQU 1 (
        echo Starting Vita3K...
        start Vita3K.exe
    ) else (
        echo You can start Vita3K by running Vita3K.exe
    )
) else if %boot% EQU 0 (
    echo Download failed, please try again by running the script as administrator.
)

if %boot% EQU 0 (
    pause
)
