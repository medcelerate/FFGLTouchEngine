@echo off
rem Builds ffgl_host_test.exe, a headless FFGL host for testing the engine
rem pool, using whichever Visual Studio install vswhere finds. Output goes to
rem .\out\. Run it against a built plugin, e.g.
rem   out\ffgl_host_test.exe ..\..\out\src\plugins\FFGLTouchEngine\Release\FFGLTouchEngine.dll ..\..\Example\NoiseOutOnly5Param.tox --te-dir ..\..\lib\TouchEngine --log run.log
rem --te-dir points at TouchEngine.dll when it is not next to the plugin.
setlocal
set HERE=%~dp0

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
	/Fo"%HERE%out\\" /Fe"%HERE%out\ffgl_host_test.exe" ^
	"%HERE%ffgl_host_test.cpp" ^
	/link user32.lib gdi32.lib opengl32.lib
if errorlevel 1 exit /b 1
echo Built %HERE%out\ffgl_host_test.exe
