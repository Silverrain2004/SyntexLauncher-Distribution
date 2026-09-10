param(
    [Parameter(Mandatory = $true)] [string] $Repository,
    [Parameter(Mandatory = $true)] [string] $Version
)

$ErrorActionPreference = 'Stop'

if ($Repository -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$') {
    throw "Invalid repository name: $Repository"
}
if ($Version -notmatch '^\d+\.\d+\.\d+$') {
    throw "Stable version must be an exact SemVer triplet without prefixes or suffixes: $Version"
}

$tag = "v$Version"

# Stable publishing is append-only. A pre-existing release OR git tag means this
# version has already been claimed and must never be recreated or clobbered.
& gh api --silent "repos/$Repository/releases/tags/$tag" 2>$null
if ($LASTEXITCODE -eq 0) {
    throw "IMMUTABLE_STABLE_REJECTED: release $tag already exists. Existing Stable releases/assets must never be replaced."
}
if ($LASTEXITCODE -ne 1) {
    throw "Could not safely determine whether release $tag exists (gh exit $LASTEXITCODE). Publishing fails closed."
}

& gh api --silent "repos/$Repository/git/ref/tags/$tag" 2>$null
if ($LASTEXITCODE -eq 0) {
    throw "IMMUTABLE_STABLE_REJECTED: tag $tag already exists. Existing Stable tags must never be moved or reused."
}
if ($LASTEXITCODE -ne 1) {
    throw "Could not safely determine whether tag $tag exists (gh exit $LASTEXITCODE). Publishing fails closed."
}

Write-Host "IMMUTABLE_STABLE_AVAILABLE: $tag does not exist as a release or git tag."
