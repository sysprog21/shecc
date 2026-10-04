#!/usr/bin/env bash

# Build the guest images "make qemu-system-check" boots, for committing to
# qemu-system/images. README.md says when to rebuild them.
#
#   qemu-system/build-images.sh [--clean] ARCH...
#
# Each ARCH replaces qemu-system/images/ARCH with its kernel, root filesystem,
# firmware if it needs one, license texts (legal-info.tar.xz) and MANIFEST, and
# writes the complete source of what they contain to
# out/qemu-system-sources/ARCH-sources.tar, which is not committed. --clean
# discards ARCH's previous build output first. Buildroot misses some changes,
# such as a package removed, a file dropped from the overlay, or a different
# kernel configuration, so committed images come from --clean builds.
#
# Buildroot runs in the container pinned in pins.env, on a Docker volume named
# after the platform, with fixed paths: BR2_REPRODUCIBLE only promises the same
# result for the same paths, and Buildroot does not support the case-insensitive
# filesystem a macOS checkout sits on. The volume keeps downloads and finished
# packages between runs. The tree reaches the container as a tar stream so that
# its files keep their modes, which Docker Desktop's file sharing does not
# preserve.
#
# The host side stays within the bash 3.2 macOS ships; --inside runs in the
# container and may assume GNU tools.

set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
# shellcheck source=qemu-system/common.sh
source "$root/qemu-system/common.sh"
# shellcheck source=qemu-system/pins.env
source "$root/qemu-system/pins.env"

# Build one architecture inside the container, with the volume at /work and this
# tree unpacked at $root; a second argument of 1 discards the previous build.
build_inside()
{
    local arch=$1 clean=$2 work=/work br tarball out ext dest
    br=$work/buildroot-$BR_VERSION
    tarball=$work/dl/buildroot-$BR_VERSION.tar.xz
    out=$work/output/$arch
    ext=$work/external/$arch
    dest=$work/images/$arch
    mkdir -p "$work/sources"

    # The container's default en_US.UTF-8 would sort file names differently.
    export LC_ALL=C SOURCE_DATE_EPOCH BR2_DL_DIR=$work/dl
    qsys_select_arch "$arch"

    qsys_download \
        "https://buildroot.org/downloads/buildroot-$BR_VERSION.tar.xz" \
        "$tarball" "$BR_SHA256"
    if [ ! -f "$br/.shecc-verified" ]; then
        rm -rf "$br"
        tar -xJf "$tarball" -C "$work"
        touch "$br/.shecc-verified"
    fi

    # A fixed umask keeps the host's umask out of the overlay's modes.
    rm -rf "$ext"
    mkdir -p "${ext%/*}"
    (umask 022 && cp -R "$root/qemu-system/buildroot" "$ext")

    if [ "$clean" = 1 ]; then
        rm -rf "$out"
    fi
    make -C "$br" O="$out" BR2_EXTERNAL="$ext" "shecc_${arch}_defconfig"
    make -C "$out"
    make -C "$out" legal-info

    rm -rf "$dest"
    mkdir -p "$dest"
    cp "$out/images/$QSYS_KERNEL" "$out/images/rootfs.cpio.xz" "$dest/"
    if [ -n "$QSYS_FIRMWARE" ]; then
        cp "$out/images/$QSYS_FIRMWARE" "$dest/"
    fi

    tar --format=gnu --sort=name --mtime="@$SOURCE_DATE_EPOCH" \
        --owner=0 --group=0 --numeric-owner \
        --exclude=./sources --exclude=./host-sources \
        -C "$out/legal-info" -cf - . | xz -9 -T1 > "$dest/legal-info.tar.xz"

    # The complete source of what the images contain: every target package's
    # source and patches, the Buildroot release that drives the build, which
    # legal-info does not save, and this external tree. Host packages are build
    # tools that the images do not contain. The archives inside are compressed
    # already.
    tar --format=gnu --sort=name --mtime="@$SOURCE_DATE_EPOCH" \
        --owner=0 --group=0 --numeric-owner \
        --transform="s,^$arch,buildroot-external," \
        -cf "$work/sources/$arch-sources.tar" \
        -C "$out/legal-info" sources \
        -C "${tarball%/*}" "${tarball##*/}" \
        -C "${ext%/*}" "$arch"

    {
        echo "arch: $arch"
        echo "buildroot: $BR_VERSION"
        echo "buildroot-sha256: $BR_SHA256"
        echo "container: $BR_IMAGE"
        echo "platform: $BR_PLATFORM"
        echo "source-date-epoch: $SOURCE_DATE_EPOCH"
        make -s -C "$out" printvars \
            VARS='LINUX_VERSION GLIBC_VERSION BUSYBOX_VERSION BASH_VERSION OPENSBI_VERSION' \
            | sed -n 's/^\([A-Z]*\)_VERSION=\(..*\)$/\1: \2/p' \
            | tr '[:upper:]' '[:lower:]'
    } > "$dest/MANIFEST"
}

if [ "${1:-}" = --inside ]; then
    build_inside "$2" "$3"
    exit 0
fi

clean=0
if [ "${1:-}" = --clean ]; then
    clean=1
    shift
fi
[ "$#" -gt 0 ] || qsys_die "name at least one of arm, arm64, riscv"
for arch in "$@"; do
    qsys_select_arch "$arch"
done
command -v docker > /dev/null 2>&1 || qsys_die "docker not found"

volume=shecc-buildroot-${BR_PLATFORM##*/}
docker volume create "$volume" > /dev/null
docker run --rm --platform "$BR_PLATFORM" --user root -v "$volume:/work" \
    "$BR_IMAGE" chown br-user:br-user /work

sources=$root/out/qemu-system-sources
mkdir -p "$sources"
for arch in "$@"; do
    echo "qemu-system: building $arch images on $BR_PLATFORM"
    (cd "$root" && COPYFILE_DISABLE=1 tar --format=ustar -cf - \
        qemu-system/buildroot qemu-system/build-images.sh \
        qemu-system/common.sh qemu-system/pins.env) \
        | docker run --rm -i --platform "$BR_PLATFORM" -v "$volume:/work" \
            "$BR_IMAGE" bash -c \
            'mkdir /tmp/src && tar -xf - -C /tmp/src && exec bash \
                /tmp/src/qemu-system/build-images.sh --inside "$0" "$1"' \
            "$arch" "$clean"

    images=$root/qemu-system/images/$arch
    rm -rf "$images"
    mkdir -p "$images"
    docker run --rm --platform "$BR_PLATFORM" -v "$volume:/work" "$BR_IMAGE" \
        tar -cf - -C "/work/images/$arch" . | tar -xf - -C "$images"
    docker run --rm --platform "$BR_PLATFORM" -v "$volume:/work" "$BR_IMAGE" \
        cat "/work/sources/$arch-sources.tar" > "$sources/$arch-sources.tar"
    echo "qemu-system: built $arch into qemu-system/images/$arch"
done
