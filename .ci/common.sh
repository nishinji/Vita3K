#!/usr/bin/env bash
set -euo pipefail

require_arg() {
    local value="${1:-}"
    local name="${2:-argument}"
    if [[ -z "$value" ]]; then
        echo "Missing required argument: $name" >&2
        exit 1
    fi
}

git_short_sha() {
    git rev-parse --short HEAD
}

# The Qt translations live in their own repository, so CI unpacks the published .qm files
# into the directory every packaging step already copies.
download_qt_translations() {
    local target_dir="${1:?missing target directory}"
    local repo="${VITA3K_TRANSLATIONS_REPO:-nishinji/vita3k_translations}"
    local archive="Vita3K-languages.zip"
    local url=""

    mkdir -p "$target_dir"

    url=$(curl -fsSL --retry 3 --retry-delay 10 "https://api.github.com/repos/$repo/releases/latest" \
        | grep "browser_download_url" \
        | grep "$archive" \
        | cut -d '"' -f 4) || true

    if [[ -z "$url" ]]; then
        echo "No $archive in the latest release of $repo. Building without Qt translations."
        return 0
    fi

    echo "Downloading Qt translations from $url"
    if ! curl -fsSL --retry 3 --retry-delay 10 -o "$target_dir/$archive" "$url"; then
        echo "Failed to download $archive. Building without Qt translations."
        return 0
    fi

    if ! (cd "$target_dir" && cmake -E tar xf "$archive"); then
        echo "Failed to unpack $archive. Building without Qt translations."
    fi

    rm -f "$target_dir/$archive"
}
