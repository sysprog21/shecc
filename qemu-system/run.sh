#!/usr/bin/env bash

# Boot the committed guest image for a target under QEMU system emulation and
# run the stage 2 bootstrap and tests inside it, or boot it to a prompt; "make
# qemu-system-check" and "make qemu-system-shell" call this.
# qemu-system/guest.sh is what runs in the guest.
#
#   qemu-system/run.sh check|shell ARCH DYNLINK STAGE1_FLAGS
#
# Written for the bash 3.2 macOS ships.

set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
# shellcheck source=qemu-system/common.sh
source "$root/qemu-system/common.sh"

[ "$#" -eq 4 ] || qsys_die "usage: $0 check|shell ARCH DYNLINK STAGE1_FLAGS"
mode=$1 arch=$2 dynlink=$3 stage1_flags=$4
out=$root/out
images=$root/qemu-system/images/$arch
qsys_select_arch "$arch"
command -v "$QSYS_QEMU" > /dev/null 2>&1 || qsys_die "$QSYS_QEMU not found"

work=$out/qemu-system
rm -rf "$work"
mkdir -p "$work/payload/out" "$work/results"

# What stage 1 reads to compile src/main.c, what the tests read, and the guest
# script. "cp -RL" turns the absolute src/codegen.c symlink into a file.
cp -RL "$root/src" "$root/tests" "$root/config" "$work/payload/"
cp "$out/libc.inc" "$out/shecc-stage1.elf" "$work/payload/out/"
cp "$root/qemu-system/guest.sh" "$work/payload/"
printf '%s=%q\n' ARCH "$arch" DYNLINK "$dynlink" STAGE1_FLAGS "$stage1_flags" \
    > "$work/payload/guest.env"

# COPYFILE_DISABLE keeps macOS tar from adding AppleDouble "._" files.
(cd "$work/payload" \
    && COPYFILE_DISABLE=1 tar --format=ustar -cf ../payload.tar .)

# A fresh, sparse, all-zero results disk: a guest that never writes its results
# leaves nothing that could pass for them.
dd if=/dev/zero of="$work/results.img" bs=1048576 count=0 seek=128 2> /dev/null

# QEMU's RISC-V loader takes no gzip, so a gzipped kernel is decompressed here.
kernel=$images/$QSYS_KERNEL
case "$kernel" in
    *.gz)
        gzip -dc "$kernel" > "$work/kernel"
        kernel=$work/kernel
        ;;
esac

args=("${QSYS_MACHINE[@]}")
if [ -n "$QSYS_FIRMWARE" ]; then
    args+=(-bios "$images/$QSYS_FIRMWARE")
fi
args+=(
    -nic none -no-reboot
    -kernel "$kernel"
    -initrd "$images/rootfs.cpio.xz"
    -drive "if=none,id=payload,format=raw,readonly=on,file=$work/payload.tar"
    -device 'virtio-blk-device,drive=payload,serial=shecc-payload'
    -drive "if=none,id=results,format=raw,file=$work/results.img"
    -device 'virtio-blk-device,drive=results,serial=shecc-results'
    -append "console=$QSYS_CONSOLE panic=-1 quiet shecc.run=$mode"
)

if [ "$mode" = shell ]; then
    echo "qemu-system: booting $arch; log in as root, and Ctrl-A X quits"
    exec "$QSYS_QEMU" "${args[@]}" -nographic
fi

echo "qemu-system: booting $arch (DYNLINK=$dynlink); see $work/console.log"
"$QSYS_QEMU" "${args[@]}" -display none -monitor none -serial stdio \
    < /dev/null 2>&1 | tee "$work/console.log" || true

# A clean power-off and a kernel panic (panic=-1 with -no-reboot) end QEMU
# alike, so what counts is what the guest wrote to the results disk.
tar -xf "$work/results.img" -C "$work/results" 2> /dev/null || true
[ -f "$work/results/status" ] \
    || qsys_die "the guest wrote no results; see $work/console.log"

[ -f "$work/results/facts.txt" ] \
    || qsys_die "the guest never ran guest.sh; see $work/results/guest.log"
failed=0
for fact in "${QSYS_FACTS[@]}"; do
    if ! grep -qxF -- "$fact" "$work/results/facts.txt"; then
        echo "qemu-system: the guest did not report '$fact'" >&2
        failed=1
    fi
done
[ "$(cat "$work/results/status")" = 0 ] || failed=1
[ "$failed" -eq 0 ] \
    || qsys_die "$arch guest failed (DYNLINK=$dynlink); see $work/results"
echo "qemu-system: $arch guest passed (DYNLINK=$dynlink)"
