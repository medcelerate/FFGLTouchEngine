@echo off
rem Builds te_pool_spike.exe against the repo's TouchEngine.lib, using whichever
rem Visual Studio install vswhere finds. Output goes to .\out\.
setlocal
set HERE=%~dp0
set REPO=%HERE%..\..

if not defined VSCMD_ARG_TGT_ARCH (
	for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSDIR=%%i
)
if not defined VSCMD_ARG_TGT_ARCH (
	if not defined VSDIR (
		echo Could not find Visual Studio with the C++ x64 tools.
		exit /b 1
	)
	call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
)

if not exist "%HERE%out" mkdir "%HERE%out"
cl /nologo /std:c++17 /EHsc /O1 /W3 ^
	/I"%REPO%\include" ^
	/Fo"%HERE%out\\" /Fe"%HERE%out\te_pool_spike.exe" ^
	"%HERE%te_pool_spike.cpp" ^
	/link "%REPO%\lib\TouchEngine\TouchEngine.lib" user32.lib
if errorlevel 1 exit /b 1

copy /y "%REPO%\lib\TouchEngine\TouchEngine.dll" "%HERE%out\" >nul
echo Built %HERE%out\te_pool_spike.exe
