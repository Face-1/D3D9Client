@echo off
:: ----------------------------------------------------------------------------
:: Batch file to create a build package (ZIP) of the D3D9Client
::
:: Notes:
:: - you need to have 7-Zip installed (you might however still need to adjust
::   the ZIP_CMD setting below to make it work)
:: - The generated ZIP file name contains the revision number in brackets.
::   If that number ends with an 'M' (like e.g. r35884M), it means that it
::   was build containing local modifications!
::   This should *not* be a officially released version!
::
:: ----------------------------------------------------------------------------
setlocal

:: --- Setup
set BASE_DIR=..\..
set OUT_DIR=_release
set VERSION=Beta28.2

:: Enhance Version by Orbiter Version
for /F "usebackq tokens=*" %%i in (`over /N ..\..\Orbitersdk\lib\orbiter.lib`) do set OVER=%%i
set VERSION=%VERSION%-for%OVER%

if "%VS150COMNTOOLS%"=="" call helper_vswhere.bat

:: Visual Studio 2017
if not "%VS150COMNTOOLS%"=="" (
  set "SETVCVARS=%VS150COMNTOOLS%..\..\VC\Auxiliary\Build\vcvarsall.bat"
  set SOLUTIONFILE=D3D9ClientVS2017.sln
  set GCAPI_PROJECTFILE=gcAPI.vs2017.vcxproj
  goto assign
)
:: Visual Studio 2015
if not "%VS140COMNTOOLS%"=="" (
  set "SETVCVARS=%VS140COMNTOOLS%..\..\VC\vcvarsall.bat"
  set SOLUTIONFILE=D3D9ClientVS2015.sln
  set GCAPI_PROJECTFILE=gcAPI.vcxproj
  goto assign
)
:: Visual Studio 2012
if not "%VS110COMNTOOLS%"=="" (
  set "SETVCVARS=%VS110COMNTOOLS%vsvars32.bat"
  set SOLUTIONFILE=D3D9ClientVS2012.sln
  set GCAPI_PROJECTFILE=gcAPI.vs2012.vcxproj
  goto assign
)
:: Visual Studio 2010
if not "%VS100COMNTOOLS%"=="" (
  set "SETVCVARS=%VS100COMNTOOLS%vsvars32.bat"
  set SOLUTIONFILE=D3D9ClientVS2010.sln
  set GCAPI_PROJECTFILE=gcAPI.vcxproj
  goto assign
)
:: Visual Studio 2008
if not "%VS90COMNTOOLS%"=="" (
  set "SETVCVARS=%VS90COMNTOOLS%vsvars32.bat"
  set SOLUTIONFILE=D3D9ClientVS2008.sln
  set GCAPI_PROJECTFILE=gcAPI.vcxproj
  goto assign
)

:assign
set ZIP_NAME=D3D9Client%VERSION%
set VC=msbuild.exe
set BUILD_FLAG=/t:build
set SOLUTIONFILE="%BASE_DIR%\Orbitersdk\D3D9Client\%SOLUTIONFILE%"
set GCAPI_PROJECTFILE="%BASE_DIR%\Orbitersdk\D3D9Client\gcAPI\%GCAPI_PROJECTFILE%"
set CONFIG=/p:Configuration=Release /p:Platform=Win32
set CONFIG_DBG=/p:Configuration=Debug
set ZIP_CMD="C:\Program Files\7-Zip\7z.exe"


:: --- Update working copy & get revision number
svn update --quiet %BASE_DIR%
for /F "tokens=*" %%i IN ('svnversion %BASE_DIR%') DO set REVISION=%%i


:: --- Create folder structure
if exist "%OUT_DIR%" (
  rmdir /S /Q "%OUT_DIR%"
)
mkdir "%OUT_DIR%"


:: DEBUG (when we like to have no_logo ...)
:: call "%VS150COMNTOOLS%VsDevCmd.bat" -no_logo -arch=x86 -host_arch=x86
:: call %VC% %BUILD_FLAG% %SOLUTIONFILE% %CONFIG%
:: if errorlevel 1 goto exit_nok
:: goto exit_ok
:: DEBUG-END


:: --- Start build environment
:: Prevent vcvarsall.bat of Visual Studio 2017 from changing the current working directory
set "VSCMD_START_DIR=%CD%"
call "%SETVCVARS%" x86
if errorlevel 1 goto exit_nok

:: gcAPI_dbg.lib (DEBUG)
:: call %VC% %GCAPI_PROJECTFILE% %BUILD_FLAG% %CONFIG_DBG%
:: if errorlevel 1 goto exit_nok

:: gcAPI.lib (RELEASE)
:: call %VC% %GCAPI_PROJECTFILE% %BUILD_FLAG% %CONFIG%
:: if errorlevel 1 goto exit_nok

:: D3D9Client & gcAPI.lib (RELEASE)
call %VC% %BUILD_FLAG% %SOLUTIONFILE% %CONFIG%
if errorlevel 1 goto exit_nok


:: --- Export
set ABS_PATH=%cd%
svn export --force --quiet "%BASE_DIR%\Config" "%ABS_PATH%\%OUT_DIR%\Config"
svn export --force --quiet "%BASE_DIR%\Doc" "%ABS_PATH%\%OUT_DIR%\Doc"
svn export --force --quiet "%BASE_DIR%\Meshes" "%ABS_PATH%\%OUT_DIR%\Meshes"
svn export --force --quiet "%BASE_DIR%\Modules" "%ABS_PATH%\%OUT_DIR%\Modules"
svn export --force --quiet "%BASE_DIR%\Orbitersdk" "%ABS_PATH%\%OUT_DIR%\Orbitersdk"
svn export --force --quiet "%BASE_DIR%\Textures" "%ABS_PATH%\%OUT_DIR%\Textures"
svn export --force --quiet "%BASE_DIR%\Utils" "%ABS_PATH%\%OUT_DIR%\Utils"
svn export --force --quiet "%BASE_DIR%\D3D9Client.cfg" "%ABS_PATH%\%OUT_DIR%"


:: --- Copy the DLLs
if not exist "%OUT_DIR%\Modules\Plugin" mkdir "%OUT_DIR%\Modules\Plugin"
if not exist "%OUT_DIR%\Orbitersdk\lib" mkdir "%OUT_DIR%\Orbitersdk\lib"
copy /y %BASE_DIR%\Modules\Plugin\D3D9Client.dll ^
         %OUT_DIR%\Modules\Plugin\D3D9Client.dll > nul
copy /y %BASE_DIR%\Orbitersdk\lib\gcAPI.lib ^
         %OUT_DIR%\Orbitersdk\lib\gcAPI.lib > nul
:: copy /y %BASE_DIR%\Orbitersdk\lib\gcAPI_dbg.lib ^
::          %OUT_DIR%\Orbitersdk\lib\gcAPI_dbg.lib > nul


:: --- Packing
cd %OUT_DIR%
%ZIP_CMD% a -tzip -mx9 "..\%ZIP_NAME%(r%REVISION%)+src.zip" *

rmdir /S /Q "Orbitersdk\D3D9Client"
rmdir /S /Q "Utils"
%ZIP_CMD% a -tzip -mx9 "..\%ZIP_NAME%(r%REVISION%).zip" *


:: --- Pass / Fail exit
:exit_ok
cd %ABS_PATH%
@call :cleanup
exit /B 0

:exit_nok
echo.
echo Build failed!
call :cleanup
exit /B 1


:: --- Cleanup
:cleanup
rmdir /S /Q "%OUT_DIR%"
endlocal
