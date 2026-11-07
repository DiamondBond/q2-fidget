#!/bin/sh
# buddy-template.png, the ghost's looks as a buddy.png, and buddy-guide.png, its cells named over
# black. Run it after changing how the ghost looks: the check compares the template with the ghost.
# Needs ffmpeg with drawtext.
set -e
cd "$(dirname "$0")"
out=$(mktemp -d)
cc -O2 -fno-math-errno -DHOST fidget.c -o "$out/fidget"
"$out/fidget" template "$out/t.pam"
ffmpeg -loglevel error -y -i "$out/t.pam" -pred mixed buddy-template.png
i=0 labels=
for name in Normal Blink Happy Petted Surprised Annoyed Boo Dizzy Asleep Sad Sick Egg Halo Horns; do
    labels="$labels,drawtext=fontfile=tools/LiberationSans-Bold.ttf:fontsize=15:fontcolor=gray:x=$((i % 7 * 192 + 6)):y=$((i / 7 * 192 + (i < 12 ? 5 : 170))):text='$((i + 1)) $name'"
    i=$((i + 1))
done
ffmpeg -loglevel error -y -f lavfi -i color=black:s=1344x384 -i buddy-template.png \
    -filter_complex "[0][1]overlay,drawgrid=w=192:h=192:c=gray@0.5$labels" -frames:v 1 buddy-guide.png
rm -rf "$out"
