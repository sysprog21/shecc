#!/usr/bin/env bash

# Helpers for qemu-system/run.sh and qemu-system/build-images.sh. Sourced, not
# executed, and kept within the bash 3.2 macOS ships.

qsys_die()
{
    echo "qemu-system: $*" >&2
    exit 1
}

# Print the SHA-256 of the named file, or nothing if it cannot be read, with GNU
# coreutils or the BSD shasum, whichever this host has.
qsys_sha256()
{
    local sum
    sum=$(sha256sum "$1" 2> /dev/null || shasum -a 256 "$1" 2> /dev/null) \
        || return 0
    echo "${sum%% *}"
}

# Download URL to PATH unless PATH already has the given SHA-256, and stop
# unless the download has it.
qsys_download()
{
    local url=$1 path=$2 want=$3
    [ "$(qsys_sha256 "$path")" = "$want" ] && return 0
    echo "qemu-system: downloading $url"
    mkdir -p "${path%/*}"
    curl -fsSL --retry 3 -o "$path.part" "$url" \
        || qsys_die "could not download $url"
    [ "$(qsys_sha256 "$path.part")" = "$want" ] \
        || qsys_die "$url does not match its SHA-256"
    mv "$path.part" "$path"
}

# Describe the guest for one target by setting:
#   QSYS_QEMU      the system emulator
#   QSYS_MACHINE   its machine, CPU and memory options
#   QSYS_CONSOLE   the kernel console device
#   QSYS_KERNEL    the kernel image Buildroot produces
#   QSYS_FIRMWARE  the firmware, if the guest needs one
#   QSYS_FACTS     lines the guest's facts.txt must contain
#
# The Arm guests run real cores; the virt machine does not take a Cortex-A8 or
# A9, and the A57 has the 64 KiB granule. The riscv guest runs QEMU's generic
# rv32 CPU, which implements extensions beyond RV32GC. An RV32 kernel maps at
# most 1 GiB, and the Arm kernel needs HIGHMEM for all of its 1 GiB.
#
# ShellCheck cannot see that the scripts sourcing this file read these.
# shellcheck disable=SC2034
qsys_select_arch()
{
    QSYS_FIRMWARE=
    case "$1" in
        arm)
            QSYS_QEMU=qemu-system-arm
            QSYS_MACHINE=(-M 'virt,highmem=off' -cpu cortex-a15 -m 1G)
            QSYS_CONSOLE=ttyAMA0
            QSYS_KERNEL=zImage
            QSYS_FACTS=('machine: armv7l')
            ;;
        arm64)
            QSYS_QEMU=qemu-system-aarch64
            QSYS_MACHINE=(-M virt -cpu cortex-a57 -m 2G)
            QSYS_CONSOLE=ttyAMA0
            QSYS_KERNEL=Image.gz
            QSYS_FACTS=('machine: aarch64' 'page size: 64 kB')
            ;;
        riscv)
            QSYS_QEMU=qemu-system-riscv32
            QSYS_MACHINE=(-M virt -m 768M)
            QSYS_CONSOLE=ttyS0
            QSYS_KERNEL=Image.gz
            QSYS_FIRMWARE=fw_dynamic.bin
            QSYS_FACTS=('machine: riscv32')
            ;;
        *) qsys_die "no guest for '$1'; the guests are arm, arm64 and riscv" ;;
    esac
}
