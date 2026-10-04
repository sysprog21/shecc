#!/usr/bin/env bash

# Run at the end of boot by /etc/init.d/S99shecc, and idle unless the kernel
# command line carries "shecc.run=", as it does when qemu-system/run.sh boots
# the guest. The host attaches two raw disks, told apart by their virtio serial
# numbers: "shecc-payload" holds a tar stream of everything the run needs, and
# "shecc-results" receives one back. The test procedure is the guest.sh at the
# payload's root, so it can change without rebuilding the images.
#
#   shecc.run=check  run the payload's guest.sh, return the results, power off
#   shecc.run=shell  unpack the payload, then boot on to a login prompt

set -uo pipefail

say()
{
    echo "shecc-guest: $*" > /dev/console
}

mode=
read -r -a words < /proc/cmdline
for word in "${words[@]}"; do
    case "$word" in
        shecc.run=*) mode=${word#shecc.run=} ;;
    esac
done
[ -n "$mode" ] || exit 0

# Print the device node of the virtio disk with the given serial number.
find_disk()
{
    local dev
    for dev in /sys/block/vd*; do
        if [ "$(cat "$dev/serial" 2> /dev/null)" = "$1" ]; then
            echo "/dev/${dev##*/}"
            return 0
        fi
    done
    return 1
}

work=/work
mkdir -p "$work"
mount -t tmpfs -o size=80%,mode=0755 tmpfs "$work"
mkdir -p "$work/payload" "$work/results"

status=0
if ! payload=$(find_disk shecc-payload); then
    say "no disk with serial shecc-payload"
    status=125
elif ! tar -xf "$payload" -C "$work/payload"; then
    say "could not unpack the payload from $payload"
    status=125
fi

if [ "$mode" = shell ]; then
    [ "$status" -eq 0 ] && say "payload unpacked in $work/payload"
    exit 0
fi

if [ "$status" -eq 0 ]; then
    (cd "$work/payload" && bash guest.sh "$work/results") < /dev/null 2>&1 \
        | tee "$work/results/guest.log" > /dev/console
    status=${PIPESTATUS[0]}
fi

# Whatever happened above, the host gets a status and the kernel log.
echo "$status" > "$work/results/status"
dmesg > "$work/results/dmesg.txt" 2>&1
if ! results=$(find_disk shecc-results); then
    say "no disk with serial shecc-results"
elif ! tar -cf "$results" -C "$work/results" .; then
    say "could not write the results to $results"
fi
sync
say "SHECC_GUEST_STATUS=$status"
poweroff -f
