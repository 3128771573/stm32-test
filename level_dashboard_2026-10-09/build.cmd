@echo off
setlocal
set "PROJ=%~dp0MDK-ARM\level.uvprojx"
set "LOG=%~dp0MDK-ARM\build.log"

rem ---- 自动查找 UV4.exe（Keil 的命令行编译器），找不到再提示手动改 ----
set "UV4="
for %%D in (
  "D:\softwares\keil MDK\Core\UV4\UV4.exe"
  "C:\Keil_v5\UV4\UV4.exe"
  "D:\Keil_v5\UV4\UV4.exe"
  "C:\Keil\UV4\UV4.exe"
  "D:\Keil\UV4\UV4.exe"
  "%ProgramFiles%\Keil_v5\UV4\UV4.exe"
  "%ProgramFiles(x86)%\Keil\UV4\UV4.exe"
) do (
  if not defined UV4 if exist %%D set "UV4=%%~D"
)
if not defined UV4 for %%P in (UV4.exe) do if not defined UV4 set "UV4=%%~$PATH:P"

if not defined UV4 (
  echo.
  echo [错误] 没找到 UV4.exe，不知道你的 Keil 装在哪。
  echo 请打开本文件，把你机器上的路径加到上面那个列表里。
  echo 或者不用命令行：直接在 Keil 里打开 MDK-ARM\level.uvprojx 按 F7。
  echo.
  exit /b 2
)

echo 使用编译器: %UV4%
"%UV4%" -b "%PROJ%" -j0 -o "%LOG%"
set RC=%ERRORLEVEL%
echo.
echo ---------- build.log ----------
if exist "%LOG%" type "%LOG%"
echo ------------------------------
echo UV4 ExitCode = %RC%   (0=无错误无警告 1=有警告 2=有错误)
endlocal & exit /b %RC%
