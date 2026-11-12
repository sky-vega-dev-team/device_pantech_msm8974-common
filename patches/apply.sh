#!/bin/bash
#
# Apply the platform patches needed by msm8974-common on top of a synced tree.
# Usage: device/pantech/msm8974-common/patches/apply.sh  (from the top of the tree)
#
# Already applied patches are skipped, so it is safe to run it after every sync.

PATCHES=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$PATCHES/../../../.." && pwd)
rc=0

for dir in $(cd "$PATCHES" && find . -name '*.patch' -printf '%h\n' | sort -u); do
    project=${dir#./}
    for patch in "$PATCHES/$project"/*.patch; do
        name="$project/$(basename "$patch")"
        if git -C "$TOP/$project" apply --check --reverse "$patch" >/dev/null 2>&1; then
            echo "skip    $name (already applied)"
        elif git -C "$TOP/$project" am -q --3way "$patch"; then
            echo "applied $name"
        else
            git -C "$TOP/$project" am --abort >/dev/null 2>&1
            echo "FAILED  $name"
            rc=1
        fi
    done
done

exit $rc
