#!/bin/sh
# Host test for tools/fm1-diag.html. Needs Node.js: NODE=... or node in PATH
# (fallback: the node.exe that ships with Photoshop on this PC).
# Usage: sh tests/run_diag.sh [path/to/fm1-diag.html]
here=$(cd "$(dirname "$0")" && pwd)
if [ -z "$NODE" ]; then
  NODE=$(command -v node 2>/dev/null)
  [ -z "$NODE" ] && NODE="/c/Program Files/Adobe/Adobe Photoshop 2025/node.exe"
fi
exec "$NODE" "$here/t_diag.js" "$@"
