# QEMU system emulation tests

`make qemu-system-check` runs the stage 2 bootstrap and tests in a guest booted
under QEMU system emulation. How to run it, and how to rebuild the guest images,
is in [README.md](../README.md#testing-under-a-real-kernel); this file explains
the design.

## Why QEMU-user is not enough

QEMU-user emulates the CPU, not the kernel. It loads the ELF with its own code,
builds the initial stack itself, and serves system calls through the host
kernel, which on x86-64 has 4 KiB pages. shecc writes its own ELF headers
(`src/elf.c`) and its libc makes raw system calls (`lib/c.c`), so it depends on
exactly what QEMU-user replaces. In a guest, the target's own kernel loads the
program, answers its system calls, and maps it with the page size it was built
for. For example, a dynamically linked arm64 stage 1 built with `PAGESIZE` at
4 KiB, instead of the 64 KiB `mk/arm64.mk` sets, bootstraps on a 4 KiB kernel
but crashes in the 64 KiB guest.

## How a run works

The guests are a 32-bit Arm kernel on a Cortex-A15, an AArch64 kernel with
64 KiB pages on a Cortex-A57, and an RV32 kernel on QEMU's generic rv32 CPU,
each with glibc, BusyBox, bash and the GNU tools the test scripts use.
`buildroot/` describes them, and `common.sh` has the QEMU options.

`run.sh` packs stage 1, the sources, the tests and `guest.sh` into a tar stream
on one virtio disk, and gives the guest an empty second one. The guest's boot
script, `/usr/libexec/shecc/guest-init.sh` in the root filesystem, unpacks the
payload, runs `guest.sh` from it, writes the results to the second disk, and
powers off. A run passes only if the guest reports success from the expected
machine, with 64 KiB pages for arm64.

## The images

`images/<arch>` holds what the guest boots: its kernel, `rootfs.cpio.xz` and,
for riscv, the OpenSBI firmware, which Ubuntu's QEMU packages do not ship for
RV32. Beside them are `legal-info.tar.xz` (see [Licenses](#licenses)) and
`MANIFEST`, which records the Buildroot release, container and package versions
they were built from. `build-images.sh` builds them in the Buildroot release
and container pinned in `pins.env`, with `BR2_REPRODUCIBLE` set, so rebuilding
from an unchanged recipe gives the same files.

## Licenses

The images contain programs under their own licenses: among them Linux and
BusyBox (GPL-2.0), bash, coreutils, gawk, grep and sed (GPL-3.0), and glibc
(LGPL-2.1 or later). Each guest's `legal-info.tar.xz` lists every package in
its image with the version, license and upstream source location
(`manifest.csv`), and holds the license texts and the Buildroot configuration.

The source of each program is the upstream release `manifest.csv` names, with
the patches that the Buildroot release pinned in `pins.env` applies, built
under this `buildroot/` tree. `build-images.sh` also collects all of it, the
Buildroot release and this tree included, into
`out/qemu-system-sources/<arch>-sources.tar`. shecc's own files here are under
its [LICENSE](../LICENSE).
