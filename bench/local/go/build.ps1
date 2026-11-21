$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$bin = Join-Path $root 'build-tools/go/bin'
New-Item -ItemType Directory -Force $bin | Out-Null
$priorTarget = $env:GOAMD64
try {
    $env:GOAMD64 = 'v3'
    Push-Location -LiteralPath $PSScriptRoot
    try {
        foreach ($workload in 'escalar', 'arreglo', 'temporales') {
            & go build -trimpath -tags $workload -o (Join-Path $bin "$workload.exe") .
            if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        }
    } finally {
        Pop-Location
    }
} finally {
    $env:GOAMD64 = $priorTarget
}
