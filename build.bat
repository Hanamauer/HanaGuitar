@echo off
setlocal
rem ===========================================================================
rem  HanaGuitar for indie -- build script
rem
rem  Same shape as the rest of this workspace: one g++ invocation, the SDK
rem  sources listed explicitly (there is no CMake here on purpose -- a plugin
rem  that needs a build system to be reproducible is a plugin you cannot build
rem  on a machine that only has a compiler).
rem ===========================================================================

set VK=D:\tools\w64devkit\w64devkit\bin
set SDK=D:\vst3work\sdk
set ROOT=%~dp0
set PATH=%VK%;%PATH%

rem g++ needs a writable temp dir; the default one under this path is not.
if not defined TMP set TMP=C:\Users\%USERNAME%\AppData\Local\Temp\opencode
if not defined TEMP set TEMP=%TMP%

cd /d "%ROOT%"
if not exist dist mkdir dist
if not exist build mkdir build

echo [1/4] compiling engine tests...
g++ -std=c++17 -O2 -DRELEASE=1 -DNDEBUG -I . -I "%SDK%" ^
    -o build\jm_test.exe tools\jm_test.cpp
if errorlevel 1 goto fail

echo [2/4] compiling offline renderer...
g++ -std=c++17 -O2 -DRELEASE=1 -DNDEBUG -I . -I "%SDK%" ^
    -o build\render_demo.exe tools\render_demo.cpp
if errorlevel 1 goto fail

echo [3/4] compiling host simulation...
g++ -std=c++17 -O2 -DRELEASE=1 -DNDEBUG ^
    -o build\host_test.exe tools\host_test.cpp ^
    -I . -I "%SDK%" -I "%SDK%\public.sdk" -I "%SDK%\pluginterfaces" ^
    "%SDK%\public.sdk\source\common\memorystream.cpp" ^
    "%SDK%\public.sdk\source\vst\vstinitiids.cpp" ^
    "%SDK%\public.sdk\source\common\commoniids.cpp" ^
    "%SDK%\base\source\fstring.cpp" ^
    "%SDK%\base\source\fdebug.cpp" ^
    "%SDK%\base\source\fobject.cpp" ^
    "%SDK%\base\source\fbuffer.cpp" ^
    "%SDK%\base\source\fstreamer.cpp" ^
    "%SDK%\base\source\baseiids.cpp" ^
    "%SDK%\base\thread\source\flock.cpp" ^
    "%SDK%\pluginterfaces\base\funknown.cpp" ^
    "%SDK%\pluginterfaces\base\ustring.cpp" ^
    "%SDK%\pluginterfaces\base\coreiids.cpp" ^
    "%SDK%\pluginterfaces\base\conststringtable.cpp" ^
    -lole32 -luser32 -lgdi32 -luuid
if errorlevel 1 goto fail

echo [4/4] linking HanaGuitar.vst3...
g++ -std=c++17 -O2 -DRELEASE=1 -DNDEBUG -shared ^
    -o dist\HanaGuitar.vst3 ^
    -I . -I "%SDK%" -I "%SDK%\public.sdk" -I "%SDK%\pluginterfaces" ^
    -I "%SDK%\base" -I "%SDK%\base\thread" ^
    vst3\HanaGuitarVST3.cpp ^
    vst3\HanaGuitarEditor.cpp ^
    src\params.cpp ^
    "%SDK%\public.sdk\source\vst\vstsinglecomponenteffect.cpp" ^
    "%SDK%\public.sdk\source\vst\vstcomponent.cpp" ^
    "%SDK%\public.sdk\source\vst\vstcomponentbase.cpp" ^
    "%SDK%\public.sdk\source\vst\vstbus.cpp" ^
    "%SDK%\public.sdk\source\vst\vstparameters.cpp" ^
    "%SDK%\public.sdk\source\vst\vstinitiids.cpp" ^
    "%SDK%\public.sdk\source\main\dllmain.cpp" ^
    "%SDK%\public.sdk\source\main\moduleinit.cpp" ^
    "%SDK%\public.sdk\source\main\pluginfactory.cpp" ^
    "%SDK%\public.sdk\source\common\commoniids.cpp" ^
    "%SDK%\public.sdk\source\common\pluginview.cpp" ^
    "%SDK%\base\source\fobject.cpp" ^
    "%SDK%\base\source\fstring.cpp" ^
    "%SDK%\base\source\fbuffer.cpp" ^
    "%SDK%\base\source\fdebug.cpp" ^
    "%SDK%\base\source\fstreamer.cpp" ^
    "%SDK%\base\source\baseiids.cpp" ^
    "%SDK%\base\source\updatehandler.cpp" ^
    "%SDK%\base\thread\source\flock.cpp" ^
    "%SDK%\pluginterfaces\base\funknown.cpp" ^
    "%SDK%\pluginterfaces\base\coreiids.cpp" ^
    "%SDK%\pluginterfaces\base\ustring.cpp" ^
    "%SDK%\pluginterfaces\base\conststringtable.cpp" ^
    -lgdiplus -lole32 -luuid -luser32 -lgdi32 -lshell32 ^
    -static-libgcc -static-libstdc++ -Wl,--kill-at
if errorlevel 1 goto fail

echo.
echo OK  dist\HanaGuitar.vst3
exit /b 0

:fail
echo.
echo BUILD FAILED
exit /b 1
