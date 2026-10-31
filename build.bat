@echo off
rem Compile le relais (d3d9.dll), le moteur (lsstudio.dll) et les plugins en 32 bits avec MSVC.
setlocal
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsamd64_x86.bat"
call "%VCVARS%" >nul || exit /b 1
cd /d "%~dp0"
set /p LSVER=<VERSION
if not exist build\plugins mkdir build\plugins
if not exist build\obj mkdir build\obj

set "CFLAGS=/nologo /std:c++20 /O2 /MT /W4 /WX /EHsc /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /Iinclude /Fobuild\obj\"
set "CORE_INCLUDES=/Ithird_party /Ithird_party\webview2\include /DLS_STUDIO_VERSION=\"%LSVER%\""
set "CORE_LIBS=kernel32.lib user32.lib gdi32.lib gdiplus.lib dwmapi.lib shell32.lib ole32.lib oleaut32.lib advapi32.lib winhttp.lib bcrypt.lib crypt32.lib version.lib shlwapi.lib third_party\webview2\lib\x86\WebView2LoaderStatic.lib"

rem Relais minimal, pose a cote de LonelyScreen.exe
cl %CFLAGS% /LD stub\stub.cpp /Fe:build\d3d9.dll /link kernel32.lib user32.lib || exit /b 1

rem Moteur, mis a jour sans droits administrateur
cl %CFLAGS% %CORE_INCLUDES% /LD loader\loader.cpp loader\store.cpp loader\studio_host.cpp loader\util.cpp loader\updater.cpp /Fe:build\lsstudio.dll /link %CORE_LIBS% || exit /b 1

for %%P in (background quality frame) do (
  cl %CFLAGS% /LD plugins\%%P\%%P.cpp /Fe:build\plugins\%%P.dll /link kernel32.lib || exit /b 1
  copy /y plugins\%%P\%%P.ini build\plugins\ >nul
  copy /y plugins\%%P\%%P.json build\plugins\ >nul
)

powershell -NoProfile -ExecutionPolicy Bypass -File tools\build-catalog.ps1 || exit /b 1
if exist build\studio rmdir /s /q build\studio
xcopy /e /i /q /y studio build\studio >nul || exit /b 1

del /q build\*.exp build\*.lib build\plugins\*.exp build\plugins\*.lib 2>nul
echo Build OK %LSVER% : build\d3d9.dll (relais) + build\lsstudio.dll (moteur) + plugins + studio
