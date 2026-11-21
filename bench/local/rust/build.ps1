$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$bin = Join-Path $root 'build-tools/rust/bin'
New-Item -ItemType Directory -Force $bin | Out-Null
foreach ($workload in 'escalar', 'arreglo', 'temporales') {
    & rustc (Join-Path $PSScriptRoot 'main.rs') --edition 2024 --cfg $workload -C opt-level=3 -C lto=fat -C codegen-units=1 -C panic=abort -C target-cpu=x86-64-v3 -o (Join-Path $bin "$workload.exe")
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
