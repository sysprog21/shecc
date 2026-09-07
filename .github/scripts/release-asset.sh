#!/usr/bin/env bash

# Print "<tag> <download URL> <sha256>" for one asset of the latest release of a
# GitHub repository. Every download in the workflows resolves what it fetches
# this way, so that none pins a version that goes stale nor trusts an archive it
# has not checksummed. An asset name may carry "{tag}" where the release stamps
# its own tag into the file name, which is only knowable once the release is in
# hand.

set -euo pipefail

if [ "$#" -ne 2 ]; then
    echo "Usage: $0 <owner/repo> <asset name, {tag} allowed>" >&2
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
    | ($name | gsub("\\{tag\\}"; $release.tag_name)) as $wanted
    | (.assets[] | select(.name == $wanted)) as $asset
    | ($asset.digest // "" | sub("^sha256:"; "")) as $sha256
    | if $sha256 == "" then
          error("\($wanted) has no sha256 digest in \($release.tag_name)")
      else
          "\($release.tag_name) \($asset.browser_download_url) \($sha256)"
      end' <<< "$release"
