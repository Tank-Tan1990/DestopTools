@echo off
REM ============================================================
REM  DestopTools — MSVC 命令行构建脚本 (Release)
REM  用法: 双击运行, 或在 "VS 2022 x64 Native Tools" 提示符下运行
REM  前提: 本机已安装 VS2022 Community + Qt 5.14.2 msvc2017_64 + Win10 SDK
REM  说明: 2026-08-19 验证可在本机成功编译出 release/DestopTools.exe
REM ============================================================
setlocal

set QTDIR=D:\Qt\Qt5.14.2\5.14.2\msvc2017_64
set VC=F:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207
set SDK=D:\Windows Kits\10
set WINSDKVER=10.0.22621.0

set PATH=%QTDIR%\bin;%VC%\bin\Hostx64\x64;%SDK%\bin\%WINSDKVER%\x64;%PATH%
set INCLUDE=%VC%\include;%SDK%\Include\%WINSDKVER%\ucrt;%SDK%\Include\%WINSDKVER%\um;%SDK%\Include\%WINSDKVER%\shared;%QTDIR%\include
set LIB=%VC%\lib\x64;%SDK%\Lib\%WINSDKVER%\ucrt\x64;%SDK%\Lib\%WINSDKVER%\um\x64;%QTDIR%\lib

cd /d %~dp0

echo === qmake (win32-msvc, release) ===
qmake DestopTools.pro -spec win32-msvc "CONFIG+=release" "CONFIG-=debug"
if errorlevel 1 goto :fail

echo === nmake release ===
nmake release
if errorlevel 1 goto :fail

echo.
echo === BUILD OK: release/DestopTools.exe ===
goto :eof

:fail
echo.
echo === BUILD FAILED ===
exit /b 1
