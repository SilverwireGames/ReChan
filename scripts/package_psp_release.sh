#!/bin/sh
set -eu

# Public PSP release: the EBOOT plus ReChan's own files (res/pc and the PSP
# textures generated from them). No game data goes in: the PSP extracts it
# from the player's disc image on first boot.
#
# usage: package_psp_release.sh VERSION [output_dir]
#   VERSION     e.g. v1.0.0-beta1 (zip: ReChan-PSP-VERSION.zip)
#   output_dir  default dist/psp-release
# Build the release EBOOT first (sh scripts/build_psp.sh release).

version="${1:?usage: package_psp_release.sh VERSION [output_dir]}"
output="${2:-dist/psp-release}"
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

if [ ! -f psp/EBOOT.PBP ]; then
    echo "psp/EBOOT.PBP not found; build the release EBOOT first" >&2
    exit 1
fi

stage=$(mktemp -d "${TMPDIR:-/tmp}/rechan-psp-release.XXXXXX")
trap 'rm -rf "$stage"' EXIT HUP INT TERM
ln -s "$root/res/pc" "$stage/pc"

rm -rf "$output"
mkdir -p "$output"
sh scripts/package_psp.sh "$stage" "$output/RECHAN"

mkdir -p "$output/RECHAN/discimage"
cp psp/release/discimage.txt "$output/RECHAN/discimage/LEIA-ME_README.txt"
for doc in LEIA-ME.txt README.txt; do
    sed "s/{{VERSION}}/$version/g" "psp/release/$doc" > "$output/$doc"
done
find "$output" \( -name '.DS_Store' -o -name '._*' \) -exec rm -f {} +

# Only ReChan's files: data/ may hold nothing but pc/ and psp/.
unexpected=$(find "$output/RECHAN/data" -mindepth 1 -maxdepth 1 ! -name pc ! -name psp)
if [ -n "$unexpected" ] || find "$output" -iname '*.str' -o -iname 'AdLib*' | grep -q .; then
    echo "release contains files that are not ReChan's own:" >&2
    echo "$unexpected" >&2
    exit 1
fi

(
    cd "$output"
    find RECHAN LEIA-ME.txt README.txt -type f | LC_ALL=C sort | while read -r file; do
        shasum -a 256 "$file"
    done > SHA256SUMS.txt
    zip -q -X -r "ReChan-PSP-$version.zip" RECHAN LEIA-ME.txt README.txt SHA256SUMS.txt
)

echo "PSP release ready: $output/ReChan-PSP-$version.zip"
shasum -a 256 "$output/ReChan-PSP-$version.zip"
