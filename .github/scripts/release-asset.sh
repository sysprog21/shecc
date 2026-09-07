#!/usr/bin/env bash

# Print "<tag> <download URL> <sha256>" for one asset of the latest release of
# a GitHub repository. Both toolchain downloads in the workflows resolve what
# they fetch this way, so that neither pins a version that goes stale nor
# trusts an archive it has not checksummed.

set -euo pipefail

if [ "$#" -ne 2 ]; then
    echo "Usage: $0 <owner/repo> <asset name>" >&2
    exit 1
fi

readonly REPO="$1"
readonly ASSET="$2"

auth=()
if [ -n "${GITHUB_TOKEN:-}" ]; then
    auth=(-H "Authorization: Bearer $GITHUB_TOKEN")
fi

release=$(curl --fail --silent --show-error --location \
    -H "Accept: application/vnd.github+json" \
    -H "X-GitHub-Api-Version: 2022-11-28" \
    "${auth[@]}" \
    "https://api.github.com/repos/$REPO/releases/latest")

# "jq -e" exits non-zero when the release carries no such asset, instead of
# printing nulls and leaving the caller to download from the string "null".
jq -er --arg name "$ASSET" '
    . as $release
    | (.assets[] | select(.name == $name)) as $asset
    | ($asset.digest // "" | sub("^sha256:"; "")) as $sha256
    | if $sha256 == "" then
          error("\($name) has no sha256 digest in \($release.tag_name)")
      else
          "\($release.tag_name) \($asset.browser_download_url) \($sha256)"
      end' <<<"$release"
