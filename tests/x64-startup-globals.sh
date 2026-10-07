#!/usr/bin/env bash
set -euo pipefail

compiler=out/shecc
if test "$1" -eq 2; then
    compiler=out/shecc-stage2.elf
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
"$compiler" -o "$work/startup" tests/vir-direct-x64-startup-globals.c
if "$work/startup"; then status=0; else status=$?; fi
test "$status" -eq 29
if "$work/startup" extra; then status=0; else status=$?; fi
test "$status" -eq 28
