#!/bin/sh
set -eu

mode="${1:-release}"
case "$mode" in
    debug|profile|release|shipping) ;;
    *) echo "usage: $0 [debug|profile|release|shipping]" >&2; exit 2 ;;
esac

if [ -z "${PSPDEV:-}" ] || [ ! -x "${PSPDEV}/bin/psp-g++" ]; then
    echo "PSPSDK not found. Install PSPDEV and export PSPDEV before building." >&2
    exit 1
fi

PSPSDK="${PSPSDK:-${PSPDEV}/psp/sdk}"
if [ ! -f "${PSPSDK}/lib/build.mak" ]; then
    echo "PSPSDK not found at ${PSPSDK}. Set PSPSDK to the SDK directory." >&2
    exit 1
fi

export PSPDEV PSPSDK
case ":${PATH:-}:" in
    *":${PSPDEV}/bin:"*) ;;
    *) PATH="${PSPDEV}/bin:${PATH:-}"; export PATH ;;
esac

# PSP and desktop builds intentionally share source-tree paths, but not ABI or
# compiler flags. Force the target to rebuild every object so a prior desktop
# build can never be reused accidentally by the PSP linker.
exec make -B -C "$(dirname "$0")/../psp" BUILD_MODE="$mode"
