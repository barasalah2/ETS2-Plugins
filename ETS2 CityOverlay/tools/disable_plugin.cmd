@echo off
rem Stops ETS2 loading the City Overlay plugin: renames the DLL so the game skips it.
rem enable_plugin.cmd (next to this file) undoes it. Close the game first.
cd /d "%~dp0.."
if exist ets2_city_overlay.dll (
    ren ets2_city_overlay.dll ets2_city_overlay.dll.off || (
        echo Couldn't disable it - is the game still running? Close it and try again.
        pause
        exit /b 1
    )
    echo City Overlay is disabled. The game won't load it until you run enable_plugin.cmd.
) else if exist ets2_city_overlay.dll.off (
    echo City Overlay is already disabled.
) else (
    echo ets2_city_overlay.dll wasn't found in %cd%
)
pause
