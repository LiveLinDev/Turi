$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$llvmBin = Join-Path $env:ProgramFiles 'LLVM/bin'
$compiler = Get-Command clang++ -ErrorAction SilentlyContinue
if ($compiler) { $llvmBin = Split-Path -Parent $compiler.Source }
$cxx = Join-Path $llvmBin 'clang++.exe'
$rc = Join-Path $llvmBin 'llvm-rc.exe'
if (!(Test-Path -LiteralPath $cxx) -or !(Test-Path -LiteralPath $rc)) {
    throw 'Se necesitan LLVM 23.1.x, Visual Studio Build Tools, CMake, Ninja y Python.'
}
$oldPath = $env:Path
try {
    $env:Path = "$llvmBin;$oldPath"
    $build = Join-Path $root 'build'
    & cmake -S $root -B $build -G Ninja "-DCMAKE_CXX_COMPILER=$cxx" "-DCMAKE_RC_COMPILER=$rc" -DCMAKE_BUILD_TYPE=Release
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    & cmake --build $build --parallel 8
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    & ctest --test-dir $build --output-on-failure -j 3
    exit $LASTEXITCODE
} finally {
    $env:Path = $oldPath
}
