param(
    [string]$InputZipFolder = "C:\Users\Ethan\Downloads\bsp_ll5cqecu0ufvihcprspw",
    [string]$OutputFolder = ".\FireData\Generated",
    [double]$SimplifyToleranceDegrees = 0.002
)

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$outputAbs = Resolve-Path -Path $projectRoot | ForEach-Object { Join-Path $_.Path $OutputFolder }
$requirements = Join-Path $PSScriptRoot "requirements-fire-pipeline.txt"
$etl = Join-Path $PSScriptRoot "fire_data_etl.py"

Write-Host "Installing Python requirements..."
python -m pip install -r $requirements

Write-Host "Running ETL pipeline..."
python $etl `
  --input-dir $InputZipFolder `
  --output-dir $outputAbs `
  --simplify-tolerance-deg $SimplifyToleranceDegrees

Write-Host "Done. Generated data at: $outputAbs"
