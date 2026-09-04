param(
    [Parameter(Mandatory = $true, Position = 0)]
    [ValidateSet('trigger', 'cancel')]
    [string]$Action,

    [ValidateRange(-100.0, 100.0)]
    [double]$X = 0.0,

    [ValidateRange(-100.0, 100.0)]
    [double]$Y = 0.0,

    [ValidateRange(-100.0, 100.0)]
    [double]$Z = 0.0,

    [ValidateRange(1, 60000)]
    [uint32]$DurationMs = 3000,

    [string]$CommandPath = (Join-Path $PSScriptRoot 'cdcoop_position_control.json')
)

$parent = Split-Path -Parent $CommandPath
if (-not (Test-Path -LiteralPath $parent -PathType Container)) {
    throw "Command directory does not exist: $parent"
}

$previousId = [uint64]0
if (Test-Path -LiteralPath $CommandPath -PathType Leaf) {
    try {
        $previous = Get-Content -LiteralPath $CommandPath -Raw | ConvertFrom-Json
        if ($null -ne $previous.command_id) {
            $previousId = [uint64]$previous.command_id
        }
    } catch {
        throw "Existing command file is invalid; refusing to replace it: $CommandPath"
    }
}

$timestampId = [uint64][DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
$nextId = [uint64]($previousId + 1)
$commandId = if ($timestampId -gt $nextId) { $timestampId } else { $nextId }
$Action = $Action.ToLowerInvariant()
$command = [ordered]@{
    command_id = $commandId
    action = $Action
}
if ($Action -eq 'trigger') {
    $command.offset = [ordered]@{ x = $X; y = $Y; z = $Z }
    $command.duration_ms = $DurationMs
}

$temporaryPath = Join-Path $parent ([IO.Path]::GetRandomFileName())
try {
    $json = $command | ConvertTo-Json -Depth 3
    [IO.File]::WriteAllText(
        $temporaryPath, $json, [Text.UTF8Encoding]::new($false))
    $published = $false
    for ($attempt = 0; $attempt -lt 40; $attempt++) {
        try {
            Move-Item -LiteralPath $temporaryPath -Destination $CommandPath `
                -Force -ErrorAction Stop
            $published = $true
            break
        } catch {
            if ($attempt -eq 39) { throw }
            Start-Sleep -Milliseconds 25
        }
    }
    if (-not $published) { throw "Failed to publish command: $CommandPath" }
} finally {
    if (Test-Path -LiteralPath $temporaryPath) {
        Remove-Item -LiteralPath $temporaryPath -Force
    }
}

"Published position command $commandId ($Action) to $CommandPath"
