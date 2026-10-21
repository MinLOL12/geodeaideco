$exe = $args[0]
$rest = @()
if ($args.Count -gt 1) { $rest = $args[1..($args.Count - 1)] }
$log = Join-Path $env:RUNNER_TEMP "diag.log"
& $exe @rest 2>&1 | ForEach-Object {
    $line = "$_"
    Add-Content -Path $log -Value $line -Encoding utf8
    Write-Host $line
    if ($line -match 'error') {
        $esc = $line -replace '%', '%25' -replace "`r", '%0D' -replace "`n", '%0A'
        if ($esc.Length -gt 600) { $esc = $esc.Substring(0, 600) }
        Write-Host "::error title=compile::$esc"
    }
}
exit $LASTEXITCODE
