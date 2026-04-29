@echo off
REM ============================================================
REM  GameBaiters Soundboard - Uninstaller (Windows)
REM ============================================================
setlocal enabledelayedexpansion

set "TS3_BASE=%APPDATA%\TS3Client"
set "TS3_PLUGINS=%TS3_BASE%\plugins"
set "TS3_SETTINGS=%TS3_BASE%\settings.db"
set "TS3_CONFIG=%TS3_BASE%\rp_soundboard.ini"

set "BACKUP_ROOT=%USERPROFILE%\Desktop\GameBaiters_Soundboard_Backup"
for /f "tokens=1-4 delims=/-: " %%a in ("%date% %time%") do set "STAMP=%%c%%a%%b_%%d"
set "STAMP=!STAMP: =0!"
set "BACKUP_DIR=%BACKUP_ROOT%_!STAMP!"

set ERRORS=0
set "DELETE_CONFIG=N"
set "DO_BACKUP=Y"

title GameBaiters Soundboard Uninstaller

echo.
echo  GameBaiters Soundboard - Uninstaller
echo  ====================================
echo.
echo  This script removes the plugin from TeamSpeak 3.
echo  Default mode preserves rp_soundboard.ini so a fresh
echo  install reuses your existing soundboard layout.
echo.

REM --- Prompt 1: delete config or keep it? ---
echo  Delete config (rp_soundboard.ini) too?
echo    [N] No, keep config (default - safe for upgrade)
echo    [Y] Yes, full wipe
echo.
set "DEL_INPUT=N"
set /p DEL_INPUT="  Delete config? (N/Y, default N): "
if "!DEL_INPUT!"=="" set "DEL_INPUT=N"
set "DELETE_CONFIG=!DEL_INPUT:~0,1!"
echo.

REM --- Prompt 2: backup before deleting? ---
echo  Backup before deleting?
echo    [Y] Yes, save current state to Desktop (default)
echo    [N] No backup
echo.
set "BAK_INPUT=Y"
set /p BAK_INPUT="  Create backup? (Y/N, default Y): "
if "!BAK_INPUT!"=="" set "BAK_INPUT=Y"
set "DO_BACKUP=!BAK_INPUT:~0,1!"
echo.

REM --- Step 1: Kill TeamSpeak if running ---
for %%P in (ts3client_win64.exe ts3client_win32.exe) do (
    tasklist /FI "IMAGENAME eq %%P" 2>nul | find /I "%%P" >nul 2>&1
    if !errorlevel!==0 (
        echo  [INFO] %%P is running.
        set /p KILL_TS3="  Force-close %%P? (Y/N): "
        if /I "!KILL_TS3!"=="Y" (
            taskkill /F /IM %%P >nul 2>&1
            timeout /t 2 /nobreak >nul
            echo  [OK] %%P terminated.
        ) else (
            echo  [WARN] Continuing with %%P alive may leave locked files.
        )
    )
)

if not exist "%TS3_BASE%" (
    echo  [INFO] No TS3Client folder at %TS3_BASE% - nothing to do.
    goto :end
)

REM --- Step 2: Backup ---
if /I "!DO_BACKUP!"=="Y" (
    echo.
    echo  Backup to %BACKUP_DIR% ...
    mkdir "%BACKUP_DIR%" >nul 2>&1
    if exist "%TS3_PLUGINS%" (
        xcopy /Y /E /I /Q "%TS3_PLUGINS%" "%BACKUP_DIR%\plugins" >nul 2>&1
    )
    if exist "%TS3_SETTINGS%" (
        copy /Y "%TS3_SETTINGS%" "%BACKUP_DIR%\settings.db.bak" >nul 2>&1
    )
    if exist "%TS3_CONFIG%" (
        copy /Y "%TS3_CONFIG%" "%BACKUP_DIR%\rp_soundboard.ini.bak" >nul 2>&1
    )
    echo  [OK] Backup created.
) else (
    echo  [SKIP] Backup skipped per user choice.
)

REM --- Step 3: Delete plugin DLL variants ---
echo.
echo  Removing plugin binaries...
for %%F in (
    rp_soundboard_fx_win64.dll
    rp_soundboard_fx_win32.dll
    rp_soundboard_win64.dll
    rp_soundboard_win32.dll
    rp_soundboard.dll
) do (
    if exist "%TS3_PLUGINS%\%%F" (
        del /F /Q "%TS3_PLUGINS%\%%F" >nul 2>&1
        if exist "%TS3_PLUGINS%\%%F" (
            echo  [FAIL] %%F still locked.
            set /a ERRORS+=1
        ) else (
            echo  [OK] Deleted %%F
        )
    )
)

REM --- Step 4: Delete plugin asset folders ---
echo.
echo  Removing plugin asset folders...
for %%D in (rp_soundboard rp_soundboard_fx soundboard) do (
    if exist "%TS3_PLUGINS%\%%D" (
        rmdir /S /Q "%TS3_PLUGINS%\%%D" >nul 2>&1
        if exist "%TS3_PLUGINS%\%%D" (
            echo  [FAIL] Could not fully remove %%D\
            set /a ERRORS+=1
        ) else (
            echo  [OK] Removed %%D\
        )
    )
)

REM --- Step 5: Delete logs (always); delete config only if user chose to ---
echo.
echo  Removing logs...
del /F /Q "%TS3_BASE%\rpsb_debug.log" >nul 2>&1
del /F /Q "%TS3_BASE%\rp_soundboard*.log" >nul 2>&1
del /F /Q "%TS3_BASE%\soundboard*.log" >nul 2>&1
echo  [OK] Logs swept.

if /I "!DELETE_CONFIG!"=="Y" (
    if exist "%TS3_CONFIG%" (
        del /F /Q "%TS3_CONFIG%" >nul 2>&1
        if not exist "%TS3_CONFIG%" (
            echo  [OK] Removed rp_soundboard.ini.
        )
    )
) else (
    if exist "%TS3_CONFIG%" (
        echo  [KEEP] rp_soundboard.ini preserved (re-used on next install^).
    )
)

REM --- Step 6: Clean settings.db ---
echo.
echo  Cleaning settings database entries...
where sqlite3.exe >nul 2>&1
if !errorlevel!==0 (
    if exist "%TS3_SETTINGS%" (
        sqlite3.exe "%TS3_SETTINGS%" "DELETE FROM Plugins WHERE value LIKE '%%rp_soundboard%%' OR value LIKE '%%soundboard%%';" >nul 2>&1
        sqlite3.exe "%TS3_SETTINGS%" "VACUUM;" >nul 2>&1
        echo  [OK] settings.db cleaned and vacuumed.
    ) else (
        echo  [SKIP] settings.db not found.
    )
) else (
    echo  [WARN] sqlite3.exe not on PATH - settings.db NOT cleaned.
)

REM --- Step 7: Plugin config sub-dirs (only if full wipe) ---
if /I "!DELETE_CONFIG!"=="Y" (
    if exist "%TS3_BASE%\rp_soundboard_configs" (
        rmdir /S /Q "%TS3_BASE%\rp_soundboard_configs" >nul 2>&1
        echo  [OK] Removed rp_soundboard_configs\
    )
)

:end
echo.
echo  ====================================
if !ERRORS!==0 (
    echo  Uninstall complete.
    if /I "!DELETE_CONFIG!"=="N" echo  Config preserved at %TS3_CONFIG%
) else (
    echo  Uninstall finished with !ERRORS! locked-file error^(s^).
    echo  Close TS3 / reboot, then run this script again.
)
if /I "!DO_BACKUP!"=="Y" echo  Backup directory: %BACKUP_DIR%
echo  ====================================
echo.
echo  Type EXIT and press Enter to close this window.
echo  ^(Or close it from the X button - either way works.^)
echo.

endlocal
cmd /k
