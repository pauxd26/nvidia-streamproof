@echo off
echo ========================================
echo  StreamProof Build
echo ========================================
echo.

echo [1/2] Building overlay.exe...
g++ -o streamproof.exe overlay.cpp -mwindows -ldwmapi -lgdi32 -luser32 -lntdll -O2 -s
if %errorlevel% neq 0 (
    echo FAILED: overlay.exe
    goto :end
)
echo OK: streamproof.exe

echo [2/2] Building nvfbc64.dll (proxy)...
g++ -shared -o nvfbc64.dll nvfbc_proxy.cpp -mwindows -ldwmapi -lgdi32 -luser32 -O2 -s
if %errorlevel% neq 0 (
    echo FAILED: nvfbc64.dll
    goto :end
)
echo OK: nvfbc64.dll

echo.
echo ========================================
echo  Build complete!
echo ========================================
echo.
echo SETUP:
echo   1. Copy the REAL nvfbc64.dll from your NVIDIA driver folder
echo      (C:\Windows\System32\nvfbc64.dll) and rename it to:
echo      nvfbc64_real.dll
echo.
echo   2. Place the built nvfbc64.dll (proxy) where ShadowPlay loads from
echo.
echo   3. Run streamproof.exe FIRST, then start ShadowPlay recording
echo.
echo CONTROLS:
echo   F9         = Toggle stealth on/off
echo   Drag       = Reposition overlay
echo   Right-click/ESC = Quit
echo.

:end
pause
