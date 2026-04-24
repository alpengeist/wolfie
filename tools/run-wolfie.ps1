Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$launchPath = Join-Path $repoRoot ".vscode\launch.json"

$launchJson = Get-Content -Raw -Path $launchPath
$launchJson = $launchJson -replace "(?m)^\s*//.*$", ""
$launchJson = $launchJson -replace "\$\{workspaceFolder\}", ($repoRoot -replace "\\", "/")
$launch = $launchJson | ConvertFrom-Json

$config = $launch.configurations | Where-Object { $_.name -eq "Launch Wolfie" } | Select-Object -First 1
if ($null -eq $config) {
    throw "Launch configuration 'Launch Wolfie' was not found in $launchPath."
}

$program = $config.program -replace "/", "\"
$workingDirectory = $config.cwd -replace "/", "\"

if (-not (Test-Path -LiteralPath $program)) {
    throw "Program was not found: $program"
}

$process = Start-Process -FilePath $program -WorkingDirectory $workingDirectory -PassThru
Write-Host "Started Wolfie process $($process.Id)"
