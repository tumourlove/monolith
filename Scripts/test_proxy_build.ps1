[CmdletBinding()]
param(
    [string] $TemporaryBase = [IO.Path]::GetTempPath()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$buildScript = Join-Path $repositoryRoot 'Tools\MonolithProxy\build_proxy.bat'
$compatibilityBuildScript = Join-Path $repositoryRoot 'Tools\MonolithProxy\build.bat'
$proxySource = Join-Path $repositoryRoot 'Tools\MonolithProxy\monolith_proxy.cpp'
$temporaryBasePath = [IO.Path]::GetFullPath($TemporaryBase)
$testRoot = Join-Path $temporaryBasePath ("MonolithProxyBuildTest-{0}" -f [Guid]::NewGuid().ToString('N'))

function Invoke-ProxyBuild {
    param(
        [Parameter(Mandatory)] [string] $SourceFile,
        [Parameter(Mandatory)] [string] $OutputDirectory,
        [Parameter(Mandatory)] [string] $StagingDirectory,
        [Parameter(Mandatory)] [string] $EntryPoint
    )

    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $env:ComSpec
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.WorkingDirectory = Split-Path -Parent $EntryPoint
    $startInfo.Arguments = '/d /s /c ""{0}""' -f $EntryPoint.Replace('"', '""')
    $startInfo.EnvironmentVariables['MONOLITH_PROXY_SOURCE_FILE'] = $SourceFile
    $startInfo.EnvironmentVariables['MONOLITH_PROXY_OUTPUT_DIR'] = $OutputDirectory
    $startInfo.EnvironmentVariables['MONOLITH_PROXY_STAGING_DIR'] = $StagingDirectory

    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    if (-not $process.Start()) {
        throw 'Failed to start the native proxy build script'
    }

    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    $process.WaitForExit()

    [pscustomobject]@{
        ExitCode = $process.ExitCode
        StdOut = $stdoutTask.GetAwaiter().GetResult()
        StdErr = $stderrTask.GetAwaiter().GetResult()
    }
}

function Assert-Condition {
    param(
        [Parameter(Mandatory)] [bool] $Condition,
        [Parameter(Mandatory)] [string] $Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

function Assert-NoCandidateOrStage {
    param(
        [Parameter(Mandatory)] [string] $OutputDirectory,
        [Parameter(Mandatory)] [string] $StagingDirectory
    )

    $candidates = @()
    if (Test-Path -LiteralPath $OutputDirectory -PathType Container) {
        $candidates = @(Get-ChildItem -LiteralPath $OutputDirectory -File -Filter 'monolith_proxy.exe.new-*')
    }
    Assert-Condition -Condition ($candidates.Count -eq 0) -Message 'Build left a publication candidate behind'
    Assert-Condition -Condition (-not (Test-Path -LiteralPath $StagingDirectory)) -Message 'Build left its private staging directory behind'
}

New-Item -ItemType Directory -Path $temporaryBasePath -Force | Out-Null
New-Item -ItemType Directory -Path $testRoot | Out-Null
try {
    $successOutput = Join-Path $testRoot 'success-output'
    $successStage = Join-Path $testRoot 'success-stage'
    $successResult = Invoke-ProxyBuild -SourceFile $proxySource -OutputDirectory $successOutput `
        -StagingDirectory $successStage -EntryPoint $buildScript
    if ($successResult.ExitCode -ne 0) {
        throw "Expected a successful native build.`n$($successResult.StdOut)`n$($successResult.StdErr)"
    }
    $successBinary = Join-Path $successOutput 'monolith_proxy.exe'
    Assert-Condition -Condition (Test-Path -LiteralPath $successBinary -PathType Leaf) `
        -Message 'Successful build did not publish monolith_proxy.exe'
    Assert-Condition -Condition ((Get-Item -LiteralPath $successBinary).Length -gt 0) `
        -Message 'Successful build published an empty executable'
    Assert-Condition -Condition ($successResult.StdOut -match 'SUCCESS: Built and published') `
        -Message 'Successful build did not report publication success'
    Assert-NoCandidateOrStage -OutputDirectory $successOutput -StagingDirectory $successStage

    $compatibilityOutput = Join-Path $testRoot 'compatibility-output'
    $compatibilityStage = Join-Path $testRoot 'compatibility-stage'
    $compatibilityResult = Invoke-ProxyBuild -SourceFile $proxySource -OutputDirectory $compatibilityOutput `
        -StagingDirectory $compatibilityStage -EntryPoint $compatibilityBuildScript
    if ($compatibilityResult.ExitCode -ne 0) {
        throw "Expected build.bat to delegate successfully.`n$($compatibilityResult.StdOut)`n$($compatibilityResult.StdErr)"
    }
    $compatibilityBinary = Join-Path $compatibilityOutput 'monolith_proxy.exe'
    Assert-Condition -Condition ((Get-Item -LiteralPath $compatibilityBinary).Length -eq (Get-Item -LiteralPath $successBinary).Length) `
        -Message 'Compatibility entry point did not publish the authoritative build output'
    Assert-NoCandidateOrStage -OutputDirectory $compatibilityOutput -StagingDirectory $compatibilityStage

    $failureOutput = Join-Path $testRoot 'compile-failure-output'
    $failureStage = Join-Path $testRoot 'compile-failure-stage'
    New-Item -ItemType Directory -Path $failureOutput | Out-Null
    $protectedBinary = Join-Path $failureOutput 'monolith_proxy.exe'
    $sentinelBytes = [byte[]](0x4D, 0x4F, 0x4E, 0x4F, 0x4C, 0x49, 0x54, 0x48)
    [IO.File]::WriteAllBytes($protectedBinary, $sentinelBytes)
    $invalidSource = Join-Path $testRoot 'invalid_proxy.cpp'
    [IO.File]::WriteAllText($invalidSource, "#error Intentional proxy build regression fixture`r`n")

    $failureResult = Invoke-ProxyBuild -SourceFile $invalidSource -OutputDirectory $failureOutput `
        -StagingDirectory $failureStage -EntryPoint $buildScript
    Assert-Condition -Condition ($failureResult.ExitCode -ne 0) -Message 'Compile failure returned exit code 0'
    Assert-Condition -Condition ([Convert]::ToBase64String([IO.File]::ReadAllBytes($protectedBinary)) -eq [Convert]::ToBase64String($sentinelBytes)) `
        -Message 'Compile failure changed the pre-existing proxy binary'
    Assert-Condition -Condition ($failureResult.StdOut -notmatch 'SUCCESS:') -Message 'Compile failure printed success'
    Assert-NoCandidateOrStage -OutputDirectory $failureOutput -StagingDirectory $failureStage

    $publishOutput = Join-Path $testRoot 'publish-failure-output'
    $publishStage = Join-Path $testRoot 'publish-failure-stage'
    New-Item -ItemType Directory -Path $publishOutput | Out-Null
    $lockedBinary = Join-Path $publishOutput 'monolith_proxy.exe'
    [IO.File]::WriteAllBytes($lockedBinary, $sentinelBytes)
    $lock = [IO.File]::Open($lockedBinary, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    try {
        $publishResult = Invoke-ProxyBuild -SourceFile $proxySource -OutputDirectory $publishOutput `
            -StagingDirectory $publishStage -EntryPoint $buildScript
    }
    finally {
        $lock.Dispose()
    }

    Assert-Condition -Condition ($publishResult.ExitCode -ne 0) -Message 'Publication failure returned exit code 0'
    Assert-Condition -Condition ([Convert]::ToBase64String([IO.File]::ReadAllBytes($lockedBinary)) -eq [Convert]::ToBase64String($sentinelBytes)) `
        -Message 'Publication failure changed the pre-existing proxy binary'
    Assert-Condition -Condition ($publishResult.StdOut -match 'could not replace') -Message 'Publication failure did not identify replacement failure'
    Assert-Condition -Condition ($publishResult.StdOut -notmatch 'SUCCESS:') -Message 'Publication failure printed success'
    Assert-NoCandidateOrStage -OutputDirectory $publishOutput -StagingDirectory $publishStage

    Write-Host 'PASS: both entry points publish, and compile or publication failures preserve the previous proxy without stale candidates.'
}
finally {
    $resolvedTestRoot = [IO.Path]::GetFullPath($testRoot)
    $requiredPrefix = $temporaryBasePath.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    if ($resolvedTestRoot.StartsWith($requiredPrefix, [StringComparison]::OrdinalIgnoreCase) -and
        (Split-Path -Leaf $resolvedTestRoot).StartsWith('MonolithProxyBuildTest-', [StringComparison]::Ordinal)) {
        Remove-Item -LiteralPath $resolvedTestRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}
