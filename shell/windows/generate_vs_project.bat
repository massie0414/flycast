@echo off
setlocal EnableDelayedExpansion

set "ROOT=%~dp0..\.."
cd /d "%ROOT%"
set "ROOT=%CD%"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VS_INSTALL="
set "DEVENV_EXE="
set "GIT_EXE="
set "NO_OPEN="
set "RUN_APP="
set "FRESH_CACHE="

:parse_args
if "%~1"=="" goto :args_done
if /i "%~1"=="--no-open" (
    set "NO_OPEN=1"
) else if /i "%~1"=="--run" (
    set "RUN_APP=1"
    set "NO_OPEN=1"
) else (
    echo Unknown option: %~1
    echo Usage: generate_vs_project.bat [--no-open ^| --run]
    exit /b 1
)
shift
goto :parse_args
:args_done

call :find_vs
if not defined VS_INSTALL (
    echo Visual Studio 2026 or 2022 with Desktop development for C++ is required.
    set "install_vs=n"
    set /p install_vs="Install Visual Studio Community 2022 now? This may show a Windows administrator prompt. [y/N]: "
    if /i "!install_vs!"=="y" (
        call :install_vs_community
        call :find_vs
    )
)

if not defined VS_INSTALL (
    echo.
    echo Visual Studio 2026 or 2022 with MSVC x64/x86 build tools was not found.
    echo Install "Desktop development with C++" from:
    echo https://visualstudio.microsoft.com/vs/community/
    goto :fail
)

call :find_cmake
if not defined CMAKE_EXE (
    echo.
    echo CMake was not found. Re-run the Visual Studio installer and include C++ CMake tools for Windows.
    goto :fail
)

if not exist "core\deps\glslang\CMakeLists.txt" (
    echo Git submodules are missing.
    call :find_git
    if not defined GIT_EXE (
        echo.
        echo Git was not found. Re-run the Visual Studio installer and include Git for Windows.
        goto :fail
    )

    if not exist ".git" (
        call :init_zip_checkout
        if errorlevel 1 goto :fail
    ) else (
        "!GIT_EXE!" rev-parse --verify HEAD >nul 2>&1
        if errorlevel 1 (
            call :init_zip_checkout
            if errorlevel 1 goto :fail
        ) else (
            set "init_submodules=n"
            set /p init_submodules="Initialize git submodules now? [y/N]: "
            if /i "!init_submodules!"=="y" (
                call :init_submodules
                if errorlevel 1 goto :fail
            ) else (
                goto :fail
            )
        )
    )
)

echo.
rem CMake caches contain absolute paths. Refresh in place after moving/copying
rem the checkout instead of generating a second solution elsewhere.
if exist "%BUILD_DIR%\CMakeCache.txt" (
    for /f "tokens=1,* delims==" %%a in ('findstr /b "CMAKE_HOME_DIRECTORY:INTERNAL=" "%BUILD_DIR%\CMakeCache.txt"') do (
        set "CACHE_SOURCE=%%b"
        set "CACHE_SOURCE=!CACHE_SOURCE:/=\!"
        if /i not "!CACHE_SOURCE!"=="%ROOT%" (
            echo Source directory changed. Refreshing the existing CMake cache.
            set "FRESH_CACHE=--fresh"
        )
    )
)

echo Updating Visual Studio %VS_YEAR% x64 project in %BUILD_DIR%...
if "%VS_YEAR%"=="2026" (
    "%CMAKE_EXE%" --help | findstr /c:"Visual Studio 18 2026" >nul
    if errorlevel 1 (
        echo CMake 4.2 or newer is required. Update C++ CMake tools for Windows in Visual Studio Installer.
        goto :fail
    )
    "%CMAKE_EXE%" --preset vs2026-x64 %FRESH_CACHE%
) else (
    "%CMAKE_EXE%" -B "%BUILD_DIR%" -G "Visual Studio 17 2022" -A x64 -DUSE_DX9=OFF -DENABLE_CTEST=OFF -DBUILD_TESTING=OFF %FRESH_CACHE%
)
if errorlevel 1 goto :fail

echo.
echo Building the latest Release executable...
if "%VS_YEAR%"=="2026" (
    "%CMAKE_EXE%" --build --preset vs2026-release
) else (
    "%CMAKE_EXE%" --build "%BUILD_DIR%" --config Release --target flycast --parallel 1 -- /nodeReuse:false
)
if errorlevel 1 goto :fail
call :say_success "Visual Studio project and Release executable updated."

if defined RUN_APP (
    start "" "%ROOT%\%BUILD_DIR%\Release\flycast.exe"
    goto :done
)

if exist "%BUILD_DIR%\flycast.slnx" (
    set "PROJECT_FILE=%CD%\%BUILD_DIR%\flycast.slnx"
) else (
    if exist "%BUILD_DIR%\flycast.sln" (
        set "PROJECT_FILE=%CD%\%BUILD_DIR%\flycast.sln"
    ) else (
        echo.
        echo Could not find %BUILD_DIR%\flycast.slnx or %BUILD_DIR%\flycast.sln.
        goto :fail
    )
)

if defined NO_OPEN (
    echo !PROJECT_FILE!
    goto :done
)

echo.
echo Opening:
echo !PROJECT_FILE!
call :find_devenv
if not defined DEVENV_EXE (
    echo.
    echo Visual Studio IDE was not found.
    goto :fail
)
start "" "!DEVENV_EXE!" "!PROJECT_FILE!"

echo.
call :say_success "Build and run in Visual Studio:"
echo Once the project opens in Visual Studio, press F5 to build, run, and debug.
echo If Visual Studio stops on 0xC0000005, uncheck "Break when this exception type is thrown".
goto :done

:find_vs
set "VS_INSTALL="
if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -version [18.0^,19.0^) -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        set "VS_INSTALL=%%i"
        set "VS_YEAR=2026"
        set "BUILD_DIR=build\vs2026"
    )
    if defined VS_INSTALL exit /b 0
    for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -version [17.0^,18.0^) -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        set "VS_INSTALL=%%i"
        set "VS_YEAR=2022"
        set "BUILD_DIR=build"
    )
)
exit /b 0

:find_devenv
set "DEVENV_EXE="
if defined VS_INSTALL (
    set "VS_DEVENV=%VS_INSTALL%\Common7\IDE\devenv.exe"
    if exist "!VS_DEVENV!" (
        set "DEVENV_EXE=!VS_DEVENV!"
        exit /b 0
    )
)
exit /b 0

:find_cmake
set "CMAKE_EXE="
if defined VS_INSTALL (
    set "VS_CMAKE=%VS_INSTALL%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    if exist "!VS_CMAKE!" (
        set "CMAKE_EXE=!VS_CMAKE!"
        exit /b 0
    )
)
exit /b 0

:find_git
set "GIT_EXE="
for /f "delims=" %%i in ('where git.exe 2^>nul') do (
    set "GIT_EXE=%%i"
    exit /b 0
)
if defined VS_INSTALL (
    set "VS_GIT=%VS_INSTALL%\Common7\IDE\CommonExtensions\Microsoft\TeamFoundation\Team Explorer\Git\mingw64\bin\git.exe"
    if exist "!VS_GIT!" (
        set "GIT_EXE=!VS_GIT!"
        exit /b 0
    )

    set "VS_GIT=%VS_INSTALL%\Common7\IDE\CommonExtensions\Microsoft\TeamFoundation\Team Explorer\Git\cmd\git.exe"
    if exist "!VS_GIT!" (
        set "GIT_EXE=!VS_GIT!"
        exit /b 0
    )
)
exit /b 0

:init_submodules
"!GIT_EXE!" submodule update --init --recursive
exit /b %errorlevel%

:init_zip_checkout
"!GIT_EXE!" init
if errorlevel 1 exit /b 1

"!GIT_EXE!" remote get-url origin >nul 2>&1
if errorlevel 1 (
    "!GIT_EXE!" remote add origin https://github.com/flyinghead/flycast.git
    if errorlevel 1 exit /b 1
) else (
    "!GIT_EXE!" remote set-url origin https://github.com/flyinghead/flycast.git
    if errorlevel 1 exit /b 1
)

"!GIT_EXE!" fetch --depth 1 origin master
if errorlevel 1 exit /b 1

"!GIT_EXE!" reset --mixed FETCH_HEAD
if errorlevel 1 exit /b 1

"!GIT_EXE!" submodule update --init --recursive
exit /b %errorlevel%

:install_vs_community
set "INSTALLER=%TEMP%\vs_community.exe"
echo Downloading Visual Studio Community 2022 bootstrapper...
powershell -NoProfile -ExecutionPolicy Bypass -Command "Invoke-WebRequest -Uri 'https://aka.ms/vs/17/release/vs_Community.exe' -OutFile $env:TEMP\vs_community.exe"
if errorlevel 1 goto :fail

echo Installing Visual Studio Community 2022. This can take a while.
"%INSTALLER%" --passive --wait --norestart --add Microsoft.VisualStudio.Workload.NativeDesktop --add Microsoft.VisualStudio.Component.VC.CMake.Project --add Microsoft.VisualStudio.Component.TeamExplorer.MinGit --includeRecommended
exit /b %errorlevel%

:say_success
set "FLYCAST_MESSAGE=%~1"
powershell -NoProfile -Command "Write-Host $env:FLYCAST_MESSAGE -ForegroundColor Green" 2>nul
if errorlevel 1 echo %~1
set "FLYCAST_MESSAGE="
exit /b 0

:fail
echo.
echo Project update or build failed. The executable has not been launched.
if not defined NO_OPEN pause
exit /b 1

:done
if not defined NO_OPEN pause
exit /b 0
