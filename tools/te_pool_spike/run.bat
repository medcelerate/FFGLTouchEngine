@echo off
rem Runs the engine pool test with two of the repo's example toxes, showing
rem progress live and saving it to run.log next to this script. Extra
rem arguments pass through, e.g.
rem   run.bat --engine "C:\Program Files\Derivative\TouchDesigner" ^
rem           --alt-engine "C:\Program Files\Derivative\TouchDesigner.2025.30280" ^
rem           --heavy "D:\show\main.tox"
rem See te_pool_spike.cpp for the sections and --only.
setlocal
set HERE=%~dp0
set EX=%HERE%..\..\Example

if not exist "%HERE%out\te_pool_spike.exe" (
	echo Run build.bat first.
	exit /b 1
)
"%HERE%out\te_pool_spike.exe" "%EX%\NoiseOutOnly5Param.tox" "%EX%\InputOutput5Param.tox" --log "%HERE%run.log" %*
