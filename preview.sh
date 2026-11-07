#!/bin/sh
# A scripted session rendered on the host (fidget.c -DHOST): preview.mp4 and preview.gif here.
# preview.sh [colour 0-1] [pet] [sheet.png]: pet previews Tamagotchi mode, a sheet a buddy of your own
set -e
case $3 in /* | "") sheet=$3 ;; *) sheet=$PWD/$3 ;; esac # before the cd
cd "$(dirname "$0")"
out=$(mktemp -d)
cc -O2 -fno-math-errno -DHOST fidget.c -o "$out/fidget"
"$out/fidget" "$out" "${1:-0}" 0 "${2:+1}" ${sheet:+"$sheet"}
ffmpeg -loglevel error -y -framerate 30 -i "$out/f%04d.ppm" -vf scale=750:640:flags=neighbor -pix_fmt yuv420p -crf 18 preview.mp4
ffmpeg -loglevel error -y -framerate 30 -i "$out/f%04d.ppm" \
    -vf "fps=20,split[a][b];[a]palettegen=max_colors=128[p];[b][p]paletteuse" preview.gif
rm -rf "$out"
