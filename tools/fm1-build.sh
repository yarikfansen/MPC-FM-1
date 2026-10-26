#!/bin/sh
# Builds the firmware and prints only what matters: errors, warnings from our
# code, image size and free bytes. Full log: build/build.log.
# Usage (Git Bash, repo root): sh tools/fm1-build.sh
cd "$(dirname "$0")/.." || exit 1
LIMIT=789793  # FlashLimit - USER 200704 - BT - VM - header, see sdk/Package.cs
WIN=$(pwd -W)
FM1_PI32_TOOLCHAIN='C:\JL\pi32' cmd //c "cd /d $WIN && .\build.bat" > build/build.log 2>&1
status=$?
grep -E "error|warning: " build/build.log | head -20
size=$(sed -n 's/.*FM1: \([0-9][0-9]*\) bytes.*/\1/p' build/build.log | head -1)
[ -n "$size" ] && echo "image $size / $LIMIT, free $((LIMIT - size))"
echo "build exit $status"
exit $status
