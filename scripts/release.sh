#!/usr/bin/env bash
#
# release.sh — one-shot version bump + tag + push for the DLSlime monorepo.
#
# Usage:
#   scripts/release.sh 0.1.2                # bump to 0.1.2
#   scripts/release.sh 0.1.2 --no-push      # commit + tag locally, skip push
#   scripts/release.sh --dry-run 0.1.2      # show diff, don't touch git
#
# What it does (in order):
#   1. Validates the new version (semver MAJOR.MINOR.PATCH).
#   2. Reads the current version from dlslime/pyproject.toml (the data plane).
#   3. Rewrites the version in every authoritative manifest:
#        dlslime/pyproject.toml, dlslime-ctrl/pyproject.toml,
#        docs/pyproject.toml, dlslime-ctrl/Cargo.toml
#   4. Refreshes dlslime-ctrl/Cargo.lock via `cargo update -p dlslime-ctrl`.
#   5. Replaces old-version references in user-facing docs:
#        docker/README.md, docker/.env.example, docker/docker-compose.yml,
#        .github/workflows/docker-publish.yml (header comment)
#   6. Shows `git diff --stat`, asks for confirmation, then commits + tags + pushes.
#
# After the push the existing GH Actions workflow takes over:
#   - tag v<NEW> -> docker-publish.yml -> ghcr.io/jimyma/dlslime-ctrl:<NEW>

set -euo pipefail

# --- args ---------------------------------------------------------------------

DRY_RUN=0
PUSH=1
NEW=""

for arg in "$@"; do
  case "$arg" in
    --dry-run) DRY_RUN=1 ;;
    --no-push) PUSH=0 ;;
    -h|--help)
      sed -n '2,30p' "$0"
      exit 0
      ;;
    *)
      if [[ -n "$NEW" ]]; then
        echo "ERROR: unexpected extra argument: $arg" >&2
        exit 2
      fi
      NEW="$arg"
      ;;
  esac
done

if [[ -z "$NEW" ]]; then
  echo "usage: $0 <new-version e.g. 0.1.2> [--dry-run] [--no-push]" >&2
  exit 2
fi

if ! [[ "$NEW" =~ ^[0-9]+\.[0-9]+\.[0-9]+(\.post[0-9]+)?$ ]]; then
  echo "ERROR: '$NEW' is not a valid version (expected MAJOR.MINOR.PATCH)" >&2
  exit 2
fi

# --- locate repo root ---------------------------------------------------------

REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "$REPO_ROOT"

MANIFESTS=(
  "dlslime/pyproject.toml"
  "dlslime-ctrl/pyproject.toml"
  "docs/pyproject.toml"
  "dlslime-ctrl/Cargo.toml"
)

DOCS=(
  "docker/README.md"
  "docker/.env.example"
  "docker/docker-compose.yml"
  ".github/workflows/docker-publish.yml"
)

RELEASE_FILES=(
  "${MANIFESTS[@]}"
  "dlslime-ctrl/Cargo.lock"
  "${DOCS[@]}"
)

# --- discover current version -------------------------------------------------

OLD="$(grep -E '^version = "' dlslime/pyproject.toml | head -1 | sed -E 's/version = "([^"]+)"/\1/')"
if [[ -z "$OLD" ]]; then
  echo "ERROR: could not parse current version from dlslime/pyproject.toml" >&2
  exit 1
fi

if [[ "$OLD" == "$NEW" ]]; then
  echo "ERROR: new version $NEW is the same as current $OLD" >&2
  exit 2
fi

echo "Bumping: $OLD  ->  $NEW"

# --- release inputs must be clean --------------------------------------------

if ! git diff --quiet -- "${RELEASE_FILES[@]}"; then
  echo "ERROR: release files have unstaged changes. Commit or stash them first." >&2
  exit 1
fi
if ! git diff --cached --quiet; then
  echo "ERROR: the index has staged changes. Commit or unstage them first." >&2
  exit 1
fi

if ! command -v cargo >/dev/null 2>&1; then
  echo "ERROR: cargo is required to refresh dlslime-ctrl/Cargo.lock." >&2
  exit 1
fi

restore_release_files() {
  git restore --worktree -- "${RELEASE_FILES[@]}"
}

# --- 1. authoritative manifests ----------------------------------------------

for f in "${MANIFESTS[@]}"; do
  if [[ ! -f "$f" ]]; then
    echo "WARN: skipping missing manifest $f" >&2
    continue
  fi
  # Anchor on `^version = "..."` (column 0) — safe for pyproject [project]
  # and Cargo [package] tables alike. Perl's in-place mode is portable across
  # GNU/Linux and macOS, unlike `sed -i`.
  RELEASE_NEW_VERSION="$NEW" perl -0pi -e \
    's/^version = "[^"]+"$/version = "$ENV{RELEASE_NEW_VERSION}"/m' "$f"
done

# --- 2. refresh Cargo.lock ----------------------------------------------------

(cd dlslime-ctrl && cargo update -p dlslime-ctrl >/dev/null)

# --- 3. doc references --------------------------------------------------------

# These files reference the version as text (image tags, examples). Replace the
# *exact* old version. Using literal match to avoid touching e.g. clap "0.1.1"
# in Cargo.lock (which is already handled by cargo update above).

for f in "${DOCS[@]}"; do
  [[ -f "$f" ]] || continue
  RELEASE_OLD_VERSION="$OLD" RELEASE_NEW_VERSION="$NEW" perl -pi -e \
    's/\Q$ENV{RELEASE_OLD_VERSION}\E/$ENV{RELEASE_NEW_VERSION}/g' "$f"
done

# --- 4. preview ---------------------------------------------------------------

echo
echo "=== diff stat ==="
git --no-pager diff --stat -- "${RELEASE_FILES[@]}"
echo
echo "=== full diff ==="
git --no-pager diff -- "${RELEASE_FILES[@]}"

if [[ "$DRY_RUN" -eq 1 ]]; then
  restore_release_files
  echo
  echo "[dry-run] Preview complete; release files were restored."
  exit 0
fi

# --- 5. confirm ---------------------------------------------------------------

echo
read -r -p "Commit, tag v$NEW, and push? [y/N] " ans
case "$ans" in
  y|Y|yes|YES) ;;
  *) restore_release_files; echo "Aborted. Release files restored."; exit 1 ;;
esac

# --- 6. commit + tag + push ---------------------------------------------------

git add -- "${RELEASE_FILES[@]}"
git commit -m "release: v$NEW"
git tag -a "v$NEW" -m "Release v$NEW"

if [[ "$PUSH" -eq 1 ]]; then
  git push origin HEAD
  git push origin "v$NEW"
  echo
  echo "✓ Pushed commit + tag v$NEW"
  echo
  echo "Track the docker build:"
  echo "  https://github.com/JimyMa/DLSlime/actions/workflows/docker-publish.yml"
  echo
  echo "After it goes green, the image will be available at:"
  echo "  ghcr.io/jimyma/dlslime-ctrl:$NEW"
else
  echo
  echo "✓ Committed and tagged v$NEW locally. Push when ready:"
  echo "  git push origin HEAD && git push origin v$NEW"
fi
