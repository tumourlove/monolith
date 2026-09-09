@echo off
setlocal EnableExtensions DisableDelayedExpansion

set "EXIT_CODE=1"
set "PUSHED_STAGE_DIR="
set "STAGE_DIR_OWNED="
set "PUBLISH_CANDIDATE="
set "PUBLISH_CANDIDATE_NAME="
set "PUBLISH_CANDIDATE_OWNED="
set "OUTPUT_DIR_WAS_DIRECTORY="
set "SCRIPT_DIR=%~dp0"

set "SOURCE_FILE=%MONOLITH_PROXY_SOURCE_FILE%"
if not defined SOURCE_FILE set "SOURCE_FILE=%SCRIPT_DIR%monolith_proxy.cpp"
for %%I in ("%SOURCE_FILE%") do set "SOURCE_FILE=%%~fI"

set "INCLUDE_DIR=%SCRIPT_DIR%ThirdParty"
for %%I in ("%INCLUDE_DIR%") do set "INCLUDE_DIR=%%~fI"

set "OUTPUT_DIR=%MONOLITH_PROXY_OUTPUT_DIR%"
if not defined OUTPUT_DIR set "OUTPUT_DIR=%SCRIPT_DIR%..\..\Binaries"
for %%I in ("%OUTPUT_DIR%") do set "OUTPUT_DIR=%%~fI"
if exist "%OUTPUT_DIR%\." set "OUTPUT_DIR_WAS_DIRECTORY=1"
set "TARGET_EXE=%OUTPUT_DIR%\monolith_proxy.exe"

call :target_path_is_directory
if not errorlevel 1 (
    echo FAILED: target executable path is an existing directory: "%TARGET_EXE%"
    goto :cleanup
)

set "STAGE_DIR=%MONOLITH_PROXY_STAGING_DIR%"
if defined STAGE_DIR goto :stage_dir_selected
if not defined TEMP (
    echo FAILED: TEMP is not defined; a private staging directory cannot be created
    goto :cleanup
)
set "STAGE_DIR=%TEMP%\MonolithProxyBuild-%RANDOM%-%RANDOM%"

:stage_dir_selected
for %%I in ("%STAGE_DIR%") do set "STAGE_DIR=%%~fI"
if /I "%STAGE_DIR%"=="%OUTPUT_DIR%" (
    echo FAILED: staging directory must differ from output directory: "%STAGE_DIR%"
    goto :cleanup
)
call :validate_output_not_nested_under_staging
if errorlevel 1 goto :cleanup

set "STAGE_EXE=%STAGE_DIR%\monolith_proxy.exe"
set "STAGE_OBJ=%STAGE_DIR%\monolith_proxy.obj"

if not exist "%SOURCE_FILE%" (
    echo FAILED: proxy source was not found: "%SOURCE_FILE%"
    goto :cleanup
)
if not exist "%INCLUDE_DIR%\nlohmann\json.hpp" (
    echo FAILED: proxy third-party headers were not found under "%INCLUDE_DIR%"
    goto :cleanup
)
if exist "%STAGE_DIR%" (
    echo FAILED: staging directory collision: "%STAGE_DIR%"
    goto :cleanup
)

where cl.exe >nul 2>&1
if not errorlevel 1 goto :toolchain_ready
call :configure_toolchain
if errorlevel 1 goto :cleanup

:toolchain_ready
where cl.exe >nul 2>&1
if errorlevel 1 (
    echo FAILED: cl.exe is unavailable after toolchain setup
    goto :cleanup
)

mkdir "%STAGE_DIR%" >nul 2>&1
if errorlevel 1 (
    echo FAILED: could not create staging directory "%STAGE_DIR%"
    goto :cleanup
)
set "STAGE_DIR_OWNED=1"

rem A junction or path alias can make different absolute strings identify the
rem same absent child. Fail before compilation if creating our stage exposed
rem an output directory that did not previously exist.
if not defined OUTPUT_DIR_WAS_DIRECTORY if exist "%OUTPUT_DIR%\." (
    echo FAILED: staging directory aliases the output directory: "%STAGE_DIR%"
    goto :cleanup
)

pushd "%STAGE_DIR%"
if errorlevel 1 (
    echo FAILED: could not enter staging directory "%STAGE_DIR%"
    goto :cleanup
)
set "PUSHED_STAGE_DIR=1"

echo C++ toolchain ready, compiling in a private staging directory...
cl.exe /nologo /EHsc /std:c++17 /O2 /MT /I"%INCLUDE_DIR%" "%SOURCE_FILE%" winhttp.lib /Fo:"%STAGE_OBJ%" /Fe:"%STAGE_EXE%"
if errorlevel 1 (
    echo FAILED: compilation failed
    goto :cleanup
)
if not exist "%STAGE_EXE%" (
    echo FAILED: compiler returned success without producing "%STAGE_EXE%"
    goto :cleanup
)
for %%I in ("%STAGE_EXE%") do set "STAGE_SIZE=%%~zI"
if "%STAGE_SIZE%"=="0" (
    echo FAILED: compiler produced an empty executable
    goto :cleanup
)

popd
set "PUSHED_STAGE_DIR="

if exist "%OUTPUT_DIR%\." goto :output_ready
mkdir "%OUTPUT_DIR%" >nul 2>&1
if errorlevel 1 (
    echo FAILED: could not create output directory "%OUTPUT_DIR%"
    goto :cleanup
)

:output_ready
if not exist "%OUTPUT_DIR%\." (
    echo FAILED: output path is not a directory: "%OUTPUT_DIR%"
    goto :cleanup
)
call :target_path_is_directory
if not errorlevel 1 (
    echo FAILED: target executable path is an existing directory: "%TARGET_EXE%"
    goto :cleanup
)

rem The compiler writes files, not child directories. A child directory here
rem means OUTPUT_DIR is physically nested below or aliased through STAGE_DIR.
for /D %%I in ("%STAGE_DIR%\*") do (
    echo FAILED: output directory aliases a path nested under staging: "%OUTPUT_DIR%"
    goto :cleanup
)

set "PUBLISH_CANDIDATE_NAME=monolith_proxy.exe.new-%RANDOM%-%RANDOM%"
set "PUBLISH_CANDIDATE=%OUTPUT_DIR%\%PUBLISH_CANDIDATE_NAME%"
if exist "%PUBLISH_CANDIDATE%" (
    echo FAILED: publish candidate collision: "%PUBLISH_CANDIDATE%"
    goto :cleanup
)

set "PUBLISH_CANDIDATE_OWNED=1"
copy /B /Y "%STAGE_EXE%" "%PUBLISH_CANDIDATE%" >nul
if errorlevel 1 (
    echo FAILED: could not stage the compiled proxy in "%OUTPUT_DIR%"
    goto :cleanup
)
if not exist "%PUBLISH_CANDIDATE%" (
    echo FAILED: publish copy returned success without producing a candidate
    goto :cleanup
)
for %%I in ("%PUBLISH_CANDIDATE%") do set "PUBLISH_SIZE=%%~zI"
if not "%STAGE_SIZE%"=="%PUBLISH_SIZE%" (
    echo FAILED: publish candidate size does not match the compiled executable
    goto :cleanup
)

move /Y "%PUBLISH_CANDIDATE%" "%TARGET_EXE%" >nul
if errorlevel 1 (
    echo FAILED: could not replace "%TARGET_EXE%"; any existing binary was preserved
    goto :cleanup
)
call :target_path_is_directory
if not errorlevel 1 (
    rem If a directory raced into place, MOVE put the candidate inside it.
    rem Retain ownership of that exact path so cleanup removes only our file.
    set "PUBLISH_CANDIDATE=%TARGET_EXE%\%PUBLISH_CANDIDATE_NAME%"
    echo FAILED: target executable path became a directory during publication: "%TARGET_EXE%"
    goto :cleanup
)
if not exist "%TARGET_EXE%" (
    echo FAILED: publish returned success but "%TARGET_EXE%" does not exist
    goto :cleanup
)
for %%I in ("%TARGET_EXE%") do set "TARGET_SIZE=%%~zI"
if not "%STAGE_SIZE%"=="%TARGET_SIZE%" (
    echo FAILED: published executable size does not match the compiled executable
    goto :cleanup
)

set "PUBLISH_CANDIDATE="
set "PUBLISH_CANDIDATE_NAME="
set "PUBLISH_CANDIDATE_OWNED="
echo SUCCESS: Built and published "%TARGET_EXE%" ^(%TARGET_SIZE% bytes^)
set "EXIT_CODE=0"
goto :cleanup

:target_path_is_directory
set "TARGET_PATH_ATTRIBUTES="
for %%I in ("%TARGET_EXE%") do set "TARGET_PATH_ATTRIBUTES=%%~aI"
if not defined TARGET_PATH_ATTRIBUTES exit /b 1
if /I "%TARGET_PATH_ATTRIBUTES:~0,1%"=="d" exit /b 0
exit /b 1

:validate_output_not_nested_under_staging
set "OUTPUT_ANCESTOR_CURSOR=%OUTPUT_DIR%"
:validate_output_ancestor_loop
for %%I in ("%OUTPUT_ANCESTOR_CURSOR%\..") do set "OUTPUT_ANCESTOR_PARENT=%%~fI"
if /I "%OUTPUT_ANCESTOR_PARENT%"=="%STAGE_DIR%" (
    echo FAILED: output directory must not be nested under staging directory: "%OUTPUT_DIR%"
    exit /b 1
)
if /I "%OUTPUT_ANCESTOR_PARENT%"=="%OUTPUT_ANCESTOR_CURSOR%" exit /b 0
set "OUTPUT_ANCESTOR_CURSOR=%OUTPUT_ANCESTOR_PARENT%"
goto :validate_output_ancestor_loop

:configure_toolchain
set "VSWHERE=%MONOLITH_PROXY_VSWHERE%"
if defined VSWHERE goto :have_vswhere
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" goto :have_vswhere
set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"

:have_vswhere
if not exist "%VSWHERE%" (
    echo FAILED: cl.exe is not on PATH and vswhere.exe was not found
    exit /b 1
)

set "VSINSTALL="
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%I"
if not defined VSINSTALL (
    echo FAILED: no Visual Studio installation with the x64 C++ toolchain was found
    exit /b 1
)

set "VCVARS64=%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS64%" (
    echo FAILED: vcvars64.bat was not found under "%VSINSTALL%"
    exit /b 1
)
call "%VCVARS64%" >nul
if errorlevel 1 (
    echo FAILED: vcvars64.bat failed for "%VSINSTALL%"
    exit /b 1
)
exit /b 0

:cleanup
if defined PUSHED_STAGE_DIR popd
if defined PUBLISH_CANDIDATE_OWNED if exist "%PUBLISH_CANDIDATE%" del /F /Q "%PUBLISH_CANDIDATE%" >nul 2>&1
if defined STAGE_DIR_OWNED if exist "%STAGE_EXE%" del /F /Q "%STAGE_EXE%" >nul 2>&1
if defined STAGE_DIR_OWNED if exist "%STAGE_OBJ%" del /F /Q "%STAGE_OBJ%" >nul 2>&1
if defined STAGE_DIR_OWNED if exist "%STAGE_DIR%\." rmdir "%STAGE_DIR%" >nul 2>&1
endlocal & exit /b %EXIT_CODE%
