@echo off
rem Runs the engine reuse test with two of the repo's example toxes, showing
rem progress live and saving it to run.log next to this script. Pass a
rem TouchDesigner install folder to pin the engine, e.g.
rem   run.bat "C:\Program Files\Derivative\TouchDesigner"
setlocal
set HERE=%~dp0
set EX=%HERE%..\..\Example

if not exist "%HERE%out\te_pool_spike.exe" (
	echo Run build.bat first.
	exit /b 1
)
set ENGINE=
if not "%~1"=="" set ENGINE='%~1'
powershell -NoProfile -Command "& '%HERE%out\te_pool_spike.exe' '%EX%\NoiseOutOnly5Param.tox' '%EX%\InputOutput5Param.tox' %ENGINE% 2>&1 | Tee-Object -FilePath '%HERE%run.log'"
