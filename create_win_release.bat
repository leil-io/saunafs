@echo off
setlocal

if not exist "src" mkdir "src"
if not exist "src\mount" mkdir "src\mount"
if not exist "src\mount\windows" mkdir "src\mount\windows"
cd /d "src\mount\windows"

:: Windows Client Version name
call :get_build_date
call :get_commit_id
call :get_cvs_branch
call :get_app_version_winversion
call :get_version_metadata_string_winversion

:: Go back to top folder
cd /d "..\..\.."

:: SaunaFS Library Version
call :get_commit_id
call :get_cvs_branch
call :get_app_version_sauversion
call :get_version_metadata_string_sauversion

:: Define variables
set "WORKSPACE=%~dp0"
set "BUNDLE=saunafs-winclient-%WINCLIENT_VERSION%"
set "MAKEFLAGS=-j8"

:: Create directory
if not exist "%BUNDLE%" mkdir "%BUNDLE%"

:: Switch to the created directory
cd /d "%BUNDLE%"

:: IMPORTANT!!!: It is necessary that the version of WinFsp installed matches the one of its installer already bundled.

:: Copy WinFsp DLL
For /F "Skip=1 Tokens=2*" %%A In (
    'Reg Query "HKLM\SOFTWARE\WOW6432Node\WinFsp" /V "InstallDir" 2^>Nul'
) Do Set "WINFSP_INSTALLDIR=%%~B"
if not exist "src" mkdir "src"
if not exist "src\mount" mkdir "src\mount"
if not exist "src\mount\windows" mkdir "src\mount\windows"
copy "%WINFSP_INSTALLDIR%\bin\winfsp-x64.dll" "src\mount\windows\winfsp-x64.dll"

:: Get WinFsp version
For /F "Skip=1 Tokens=2*" %%A In (
    'Reg Query "HKLM\SOFTWARE\Classes\Installer\Dependencies\WinFsp" /V "Version" 2^>Nul'
) Do Set "WINFSP_VERSION=%%~B"

:: Create Makefile
cmake -G "MinGW Makefiles" .. -DCMAKE_BUILD_TYPE=Release -DENABLE_TESTS=NO ^
   -DENABLE_DOCS=YES -DENABLE_URAFT=NO -DENABLE_POLONAISE=NO -DENABLE_CCACHE=NO ^
   -DENABLE_NFS_ACL_SUPPORT=NO -DENABLE_WINFSP_WINCLIENT=YES ^
   -DWINCLIENT_PACKAGE_VERSION=%WINCLIENT_VERSION% -DPACKAGE_VERSION=%SAUNAFS_VERSION% ^
   -DWINFSP_VERSION=%WINFSP_VERSION%

:: Build
mingw32-make %MAKEFLAGS%

::Copy installer
xcopy /s .\src\mount\windows\installer\*installer.exe ..

:: Switch back to the original directory
cd /d "%WORKSPACE%"

::Remove everything else
rmdir /q /s "%BUNDLE%"

:: End of the script
endlocal
goto :eof

:: Functions
:get_build_date
   For /f "tokens=2-4 delims=/ " %%a in ('date /t') do (set date=%%c%%b%%a)
   For /f "tokens=1-2 delims=/:" %%a in ('time /t') do (set time=%%a%%b)
   set "build_date=%date%-%time%"
goto :eof

:get_commit_id
   for /F "delims=" %%L in ('git rev-parse --short HEAD') do (set "commit_id=%%L")
goto :eof

:get_cvs_branch
   if "%commit_id%"=="" set "commit_id=HEAD"
   for /F "delims=" %%L in ('git rev-parse --abbrev-ref HEAD') do (set "branch_name=%%L")
goto :eof

:get_app_version_winversion
   set "app_version=1.0.4"
goto :eof

:get_app_version_sauversion
   for /F "delims=" %%L in ('wsl echo "$(grep -Eie '(^saunafs|sfs).*urgency' debian/changelog | head -n1 | awk '{print $2}' | tr -d '()')"') do (set "app_version=%%L")
goto :eof

:get_version_metadata_string_winversion
   set "branch_status=unstable"
   if "%branch_name%"=="main" (
      set "branch_status=stable"
   )
   set "WINCLIENT_VERSION=%app_version%-%build_date%-%branch_status%-%branch_name%-%commit_id%"
goto :eof

:get_version_metadata_string_sauversion
   set "branch_status=unstable"
   if "%branch_name%"=="main" (
      set "branch_status=stable"
   )
   set "SAUNAFS_VERSION=%app_version%-%branch_status%-%branch_name%-%commit_id%"
goto :eof
