#!/usr/bin/env sh
set -eu

if [ "$(uname -m)" != "x86_64" ]; then
    echo "rechan currently supports only 64-bit x86 Linux (x86_64)." >&2
    exit 1
fi

config="${1:-release}"
case "$config" in
    debug|release|shipping) ;;
    *) echo "usage: $0 [debug|release|shipping]" >&2; exit 2 ;;
esac

if command -v premake5 >/dev/null 2>&1; then
    premake=premake5
elif [ -x ./premake5 ]; then
    premake=./premake5
else
    echo "premake5 was not found (install it or place it in the repository root)" >&2
    exit 1
fi

"$premake" --os=linux gmake
make -C build config="$config" -j"${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}"
