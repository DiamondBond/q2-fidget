# Fidget

A ghost buddy to poke on the Shanling Q2, for [Q2 Pod](https://github.com/DiamondBond/q2-pod).
Install it from **System settings → App Store** and it shows up in **Library → Games**.

| To | Do this |
| --- | --- |
| Make it giggle | Tap its tummy |
| Boop it | Tap its face |
| Pet it | Hold a finger on it |
| Pick it up and fling it | Drag it, then let go |
| Annoy it | Tap it five times quickly |
| Make it dizzy | Spin the wheel fast |
| Make it say boo | Press Centre |
| Change its colour, or quit | Press Return for the menu |

Play/Pause and the side buttons keep controlling the music. Leave it alone and it dozes off; after two
minutes without a touch it closes, unless Smart Awake is on (System settings → Display →
Controls), which keeps it open and the screen lit. Hold Return to close it at once.

## How it runs

Q2 Pod lists a card app whose `q2app` says `game=1` in Games instead of Streaming. A Games row
runs `/mnt/mmc/.fidget/q2-fidget` over the whole screen while the Q2's own UI waits, and sends it
input as text datagrams on `/tmp/q2-fidget.input.sock`:

| Datagram | Means |
| --- | --- |
| `t d X Y`, `t m X Y`, `t u X Y` | touch down, move, up, in the 375 x 320 view |
| `k d CODE`, `k u CODE` | a key down, up: Return 170, Centre 218 |
| `w N` | the wheel's raw travel, 200 a turn, rising anticlockwise |

The program is one C file with no libraries: it makes its own syscalls and draws on `/dev/fb0`
(320 x 375, the view turned clockwise, two pages). Its colour is kept in `.fidget/state`.

## Building

`./build.sh` makes `dist/fidget.zip` (needs `clang` and `ld.lld`). `./preview.sh` renders a
scripted session on the PC to `preview.mp4` and `preview.gif`. `tools/font.py` regenerates
`font.h` (needs Pillow).

The menu's font is Liberation Sans Bold, under the SIL Open Font License 1.1.
