param([string]$BuildDir)

function Emit-Annot([string]$title, [string[]]$lines) {
    if (-not $lines -or $lines.Count -eq 0) { return }
    $joined = ($lines | ForEach-Object { $_ -replace '%', '%25' -replace "`r", '%0D' }) -join '%0A'
    if ($joined.Length -gt 30000) { $joined = $joined.Substring(0, 30000) }
    Write-Host "::error title=$title::$joined"
}

try {
    $ccPath = Join-Path $BuildDir "compile_commands.json"
    $cc = Get-Content $ccPath -Raw | ConvertFrom-Json
    $entry = $cc | Where-Object { $_.file -match 'src[\\/]main\.cpp$' } | Select-Object -First 1
    if (-not $entry) { Emit-Annot "diag" @("no compile command for src/main.cpp found"); exit 0 }
    $cmd = $entry.command
    Write-Host "diag: original command length $($cmd.Length)"

    # Drop compiler-cache launcher prefix (sccache/ccache <compiler> ...)
    $cmd = $cmd -replace '^\S*(sccache|ccache)(\.exe)?\s+', ''
    # Make it a stand-alone syntax check: remove output/depfile/PCH args
    $cmd = $cmd -replace '(^|\s)-o\s+\S+', '$1'
    $cmd = $cmd -replace '(^|\s)-c(\s|$)', '$1-fsyntax-only$2'
    $cmd = $cmd -replace '(^|\s)-MD(\s|$)', ' '
    $cmd = $cmd -replace '(^|\s)-MT\s+("[^"]*"|\S+)', ' '
    $cmd = $cmd -replace '(^|\s)-MF\s+("[^"]*"|\S+)', ' '
    $cmd = $cmd -replace '-Xclang\s+-include-pch\s+-Xclang\s+("[^"]*"|\S+)', ' '
    $cmd = $cmd -replace '-Xclang\s+-include\s+-Xclang\s+("[^"]*"|\S+)', ' '
    $cmd = $cmd -replace '(^|\s)/Yu("[^"]*"|\S*)', ' '
    $cmd = $cmd -replace '(^|\s)/FI("[^"]*"|\S*)', ' '
    $cmd = $cmd -replace '(^|\s)-include\s+("[^"]*cmake_pch[^"]*"|\S*cmake_pch\S*)', ' '

    # Split exe / args (first token, possibly quoted)
    $exe = $null; $argStr = $null
    if ($cmd -match '^\s*"([^"]+)"\s*(.*)$') { $exe = $Matches[1]; $argStr = $Matches[2] }
    elseif ($cmd -match '^\s*(\S+)\s*(.*)$') { $exe = $Matches[1]; $argStr = $Matches[2] }
    if (-not $exe) { Emit-Annot "diag" @("could not parse compile command"); exit 0 }
    Write-Host "diag: exe=$exe"

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.Arguments = $argStr
    $psi.WorkingDirectory = $entry.directory
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $proc = [System.Diagnostics.Process]::Start($psi)
    $out = $proc.StandardOutput.ReadToEnd() + $proc.StandardError.ReadToEnd()
    $proc.WaitForExit()
    Write-Host "diag: compiler exit $($proc.ExitCode)"

    $lines = $out -split "`r?`n"
    $errLines = @()
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match 'error') {
            $errLines += $lines[$i]
            if ($i + 1 -lt $lines.Count) { $errLines += $lines[$i + 1] }
            if ($i + 2 -lt $lines.Count) { $errLines += $lines[$i + 2] }
        }
        if ($errLines.Count -ge 90) { break }
    }
    if ($proc.ExitCode -ne 0 -and $errLines.Count -eq 0) {
        $errLines = $lines | Select-Object -First 40
    }
    Emit-Annot "diag" $errLines
    Emit-Annot "diaginfo" @("exit=$($proc.ExitCode) totalLines=$($lines.Count)")
} catch {
    Emit-Annot "diag" @("diag script failed: $_")
}
exit 0
