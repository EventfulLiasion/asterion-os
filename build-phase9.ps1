param([string]$OutputDirectory = 'vm\phase9-build')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
if ([System.IO.Path]::IsPathRooted($OutputDirectory)) { $outputRoot = [System.IO.Path]::GetFullPath($OutputDirectory) } else { $outputRoot = [System.IO.Path]::GetFullPath((Join-Path $root $OutputDirectory)) }
if (Test-Path -LiteralPath $outputRoot) { throw "Refusing to overwrite existing output: $outputRoot" }
New-Item -ItemType Directory -Path $outputRoot | Out-Null
$items = @('arch','boot','drivers','include','kernel','tests','README.md','CONTRIBUTING.md','.gitignore','.gitattributes','kernel.c','build.ps1','make-image.py','make-uefi-disk.py','make-phase9-data-disk.py')
foreach ($configuration in @('normal','debug')) {
    $destination = Join-Path $outputRoot $configuration
    New-Item -ItemType Directory -Path $destination | Out-Null
    foreach ($item in $items) { Copy-Item -LiteralPath (Join-Path $root $item) -Destination $destination -Recurse }
    New-Item -ItemType Directory -Force -Path (Join-Path $destination 'EFI\BOOT') | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $destination 'docs') | Out-Null
    foreach ($doc in @('architecture.md','design-decisions.md','phase9-storage-filesystem-sync.md','testing.md')) { Copy-Item -LiteralPath (Join-Path $root ('docs\' + $doc)) -Destination (Join-Path $destination 'docs') }
    New-Item -ItemType Directory -Path (Join-Path $destination 'user') | Out-Null
    Copy-Item -LiteralPath (Join-Path $root 'user\hello.S') -Destination (Join-Path $destination 'user')
    Copy-Item -LiteralPath (Join-Path $root 'user\build-hello-elf.py') -Destination (Join-Path $destination 'user')
    Push-Location $destination
    try {
        if ($configuration -eq 'debug') { & (Join-Path $destination 'build.ps1') -DebugBuild }
        else { & (Join-Path $destination 'build.ps1') }
        if ($LASTEXITCODE -ne 0) { throw "$configuration EFI build failed with exit code $LASTEXITCODE" }
        & (Get-Command python).Source -m unittest discover -s (Join-Path $destination 'tests') -v
        if ($LASTEXITCODE -ne 0) { throw "$configuration host test suite failed with exit code $LASTEXITCODE" }
    } finally { Pop-Location }
}
Write-Host "Phase 9 normal/debug builds and host tests passed in $outputRoot" -ForegroundColor Green
