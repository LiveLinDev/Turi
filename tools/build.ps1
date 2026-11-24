param([string]$BuildDir = 'build', [switch]$Sanitize)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$llvmBin = Join-Path $env:ProgramFiles 'LLVM/bin'
$compiler = Get-Command clang++ -ErrorAction SilentlyContinue
if ($compiler) { $llvmBin = Split-Path -Parent $compiler.Source }
$cxx = Join-Path $llvmBin 'clang++.exe'
$rc = Join-Path $llvmBin 'llvm-rc.exe'
$build = Join-Path $root $BuildDir
$cache = Join-Path $build 'CMakeCache.txt'
$cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
$cmake = if ($cmakeCommand) { $cmakeCommand.Source } else { '' }
if (!$cmake) {
    $cmake = Get-ChildItem -Path (Join-Path $env:LOCALAPPDATA 'Microsoft/WinGet/Packages/Kitware.CMake_*') -Filter cmake.exe -Recurse -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
}
if (!$cmake -and (Test-Path -LiteralPath $cache)) {
    $cached = Select-String -LiteralPath $cache -Pattern '^CMAKE_COMMAND:INTERNAL=(.+)$' | Select-Object -First 1
    if ($cached -and (Test-Path -LiteralPath $cached.Matches[0].Groups[1].Value)) {
        $cmake = $cached.Matches[0].Groups[1].Value
    }
}
if (!$cmake) {
    $candidates = @(
        (Join-Path $env:ProgramFiles 'CMake/bin/cmake.exe'),
        (Join-Path $env:ProgramFiles 'Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'),
        (Join-Path $env:ProgramFiles 'Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe')
    )
    $cmake = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}
$ninjaCommand = Get-Command ninja -ErrorAction SilentlyContinue
$ninja = if ($ninjaCommand) { $ninjaCommand.Source } else { '' }
if (!$ninja) {
    $ninja = Get-ChildItem -Path (Join-Path $env:LOCALAPPDATA 'Microsoft/WinGet/Packages/Ninja-build.Ninja_*') -Filter ninja.exe -Recurse -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty FullName
}
if (!$ninja) {
    $ninjaCandidates = @(
        (Join-Path $env:ProgramFiles 'Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'),
        (Join-Path $env:ProgramFiles 'Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe')
    )
    $ninja = $ninjaCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}
if (!(Test-Path -LiteralPath $cxx) -or !(Test-Path -LiteralPath $rc) -or !$cmake -or !$ninja) {
    throw 'Se necesitan LLVM 23.1.x, Visual Studio Build Tools, CMake, Ninja y Python.'
}
$cmakeBin = Split-Path -Parent $cmake
$ninjaBin = Split-Path -Parent $ninja
$ctest = Join-Path $cmakeBin 'ctest.exe'
$cxxArg = $cxx.Replace('\', '/')
$rcArg = $rc.Replace('\', '/')
$ninjaArg = $ninja.Replace('\', '/')
$oldPath = $env:Path
try {
    $env:Path = "$llvmBin;$cmakeBin;$ninjaBin;$oldPath"
    $buildType = 'Release'
    $extra = @()
    if ($Sanitize) {
        $buildType = 'RelWithDebInfo'
        $extra += '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer'
        $resourceDir = & $cxx -print-resource-dir
        $env:Path = "$resourceDir/lib/windows;$env:Path"
        $env:ASAN_OPTIONS = 'halt_on_error=1'
        $env:UBSAN_OPTIONS = 'halt_on_error=1:print_stacktrace=1'
    }
    & $cmake -S $root -B $build -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninjaArg" "-DCMAKE_CXX_COMPILER=$cxxArg" "-DCMAKE_RC_COMPILER=$rcArg" "-DCMAKE_BUILD_TYPE=$buildType" @extra
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    & $cmake --build $build --parallel 8
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    & $ctest --test-dir $build --output-on-failure -j 3
    exit $LASTEXITCODE
} finally {
    $env:Path = $oldPath
}
