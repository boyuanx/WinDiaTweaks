#!/usr/bin/env bash
# Prints the release notes for a tag. An annotated tag's message is used as is
# (git tag -a v1.2.0 -m "..."); otherwise the commits since the previous tag, grouped by their
# conventional-commit type (feat, fix, ...). Settings > Patches > Updates shows these notes in Dia.
set -euo pipefail
tag="$1"
repo="${GITHUB_REPOSITORY:-zioder/WinDiaTweaks}"
prev=$(git describe --tags --abbrev=0 "$tag^" 2>/dev/null || true)
range="${prev:+$prev..}$tag"

message=""
if [ "$(git cat-file -t "$tag")" = tag ]; then
  message=$(git tag -l --format='%(contents)' "$tag" | sed '/-----BEGIN PGP SIGNATURE-----/,$d')
fi

if [ -n "$(printf '%s' "$message" | tr -d '[:space:]')" ]; then
  printf '%s\n\n' "$message"
else
  subjects=$(git log --no-merges --format='%s' "$range")
  section() {  # title, regex for the commit type
    local lines
    lines=$(printf '%s\n' "$subjects" | grep -E "^($2)(\([^)]*\))?!?: " | sed -E 's/^[a-z]+(\([^)]*\))?!?: +/- /; s/^- (.)/- \U\1/' || true)
    if [ -n "$lines" ]; then printf '## %s\n\n%s\n\n' "$1" "$lines"; fi
  }
  section "✨ New" "feat"
  section "🐛 Fixes" "fix"
  section "⚡️ Improvements" "perf|refactor|style"
fi

if [ -n "$prev" ]; then
  echo "[All changes](https://github.com/$repo/compare/$prev...$tag)"
fi
