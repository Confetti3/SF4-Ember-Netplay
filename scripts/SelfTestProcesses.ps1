# Process helpers of the self-test runners (run-tournament-selftest.ps1, and
# its host script on the other PC). They stop what a runner started, by PID,
# and never by image name.
$ErrorActionPreference = 'Stop'

# The processes below `$RootPid`, nearest first. Windows keeps a process's
# ParentProcessId after the parent has gone and reuses the PID, so a child
# counts only when it was created after the parent it names.
function Get-DescendantProcesses([int]$RootPid) {
    $all = @(Get-CimInstance Win32_Process)
    $parents = @($all | Where-Object { $_.ProcessId -eq $RootPid })
    $found = @()
    while ($parents.Count) {
        $children = @(foreach ($parent in $parents) {
            $all | Where-Object { $_.ParentProcessId -eq $parent.ProcessId -and $_.CreationDate -ge $parent.CreationDate }
        })
        $found += $children
        $parents = $children
    }
    $found
}

# Ends everything below `$RootPid`, the deepest first, and leaves `$RootPid` itself.
function Stop-Descendants([int]$RootPid) {
    $tree = @(Get-DescendantProcesses $RootPid)
    [array]::Reverse($tree)
    foreach ($process in $tree) { Stop-Process -Id $process.ProcessId -Force -ErrorAction SilentlyContinue }
}
