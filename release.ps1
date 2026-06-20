param()

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $repoRoot

$globalsPath = Join-Path $repoRoot 'globals.h'
if (-not (Test-Path $globalsPath)) {
  throw "globals.h not found at $globalsPath"
}

$globalsContent = Get-Content -Path $globalsPath -Raw
$versionMatch = [regex]::Match($globalsContent, '#define\s+FW_VERSION\s+"([^"]+)"')
if (-not $versionMatch.Success) {
  throw 'FW_VERSION not found in globals.h'
}
$fwVersion = $versionMatch.Groups[1].Value

$buildVersionName = $fwVersion
if ($buildVersionName -notmatch '^[Vv]') {
  $buildVersionName = "V$buildVersionName"
}

$buildRoot = Join-Path $repoRoot 'Build'
$buildVersionDir = Join-Path $buildRoot $buildVersionName

New-Item -ItemType Directory -Force -Path $buildRoot | Out-Null
New-Item -ItemType Directory -Force -Path $buildVersionDir | Out-Null

$compileLogPath = Join-Path $buildVersionDir 'compile.log'

$compileArgs = @(
  'compile',
  '--fqbn', 'esp32:esp32:esp32s3',
  '--output-dir', $buildVersionDir,
  '.'
)

$previousErrorActionPreference = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
$compileOutput = & arduino-cli @compileArgs 2>&1
$compileExitCode = $LASTEXITCODE
$ErrorActionPreference = $previousErrorActionPreference
$compileOutput | Out-File -FilePath $compileLogPath -Encoding utf8
if ($compileExitCode -ne 0) {
  throw "Build failed. See compile log: $compileLogPath"
}

$gitBranch = (& git rev-parse --abbrev-ref HEAD 2>$null)
if (-not $gitBranch) { $gitBranch = 'unknown' }

$gitCommit = (& git rev-parse HEAD 2>$null)
if (-not $gitCommit) { $gitCommit = 'unknown' }

$buildDate = (Get-Date).ToString('yyyy-MM-dd HH:mm:ssK')

$versionTxtPath = Join-Path $buildVersionDir 'version.txt'
@(
  "FW_VERSION=$fwVersion"
  "BUILD_FOLDER=$buildVersionName"
  "GIT_BRANCH=$gitBranch"
  "GIT_COMMIT=$gitCommit"
  "BUILD_DATE=$buildDate"
) | Out-File -FilePath $versionTxtPath -Encoding utf8

Write-Host "Build artifacts: $buildVersionDir"
Write-Host "Version: $fwVersion"
