#!/usr/bin/env bash
# Cut a new firmware release on GitHub from the current commit.
#
# Refuses to run if the tree is dirty (so the running binary's
# version string matches the tag).
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

if ! git diff --quiet || ! git diff --cached --quiet; then
    echo "error: working tree has uncommitted changes; commit or stash first" >&2
    exit 1
fi

SHA=$(git rev-parse --short HEAD)
TAG="fw-$SHA"

if gh release view "$TAG" >/dev/null 2>&1; then
    echo "error: release $TAG already exists" >&2
    exit 1
fi

./build.sh

gh release create "$TAG" build/c6-hub.bin \
    --title "$TAG" \
    --notes "Built from $(git log -1 --pretty=%s)" \
    --latest

echo "released $TAG"
