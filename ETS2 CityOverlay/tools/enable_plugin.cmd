@echo off
rem Lets ETS2 load the City Overlay plugin again (undoes disable_plugin.cmd). Close the game first.
cd /d "%~dp0.."
if exist ets2_city_overlay.dll.off (
    ren ets2_city_overlay.dll.off ets2_city_overlay.dll || (
        echo Couldn't enable it - is the game still running? Close it and try again.
        pause
        exit /b 1
    )
    echo City Overlay is enabled. It loads the next time you start the game.
) else if exist ets2_city_overlay.dll (
    echo City Overlay is already enabled.
) else (
    echo ets2_city_overlay.dll.off wasn't found in %cd%
)
pause
