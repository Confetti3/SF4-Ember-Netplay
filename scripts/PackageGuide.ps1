# START_HERE.md and the package docs ship without the guides' step pictures:
# an installed Launcher refuses an update that brings files its inventory does
# not name.
function Get-PackageGuide([string]$Path) {
    $guide = [IO.File]::ReadAllText($Path) -replace '\r?\n\r?\n *!\[[^\]]*\]\(\.\./images/[^)]*\)', ''
    if ($guide.Contains('../images/')) { throw "$Path keeps a picture the package cannot show; put each picture on its own line after a blank one." }
    $guide
}
