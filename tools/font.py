"""font.h: printable ASCII from Liberation Sans Bold (SIL OFL 1.1) at 20 px, as Q2 Pod's game menus
caption, an alpha byte a pixel. Run when the size or the font changes; the build needs no Python."""
import pathlib
from PIL import Image, ImageDraw, ImageFont

HERE = pathlib.Path(__file__).parent
font = ImageFont.truetype(str(HERE / 'LiberationSans-Bold.ttf'), 20)
ascent, descent = font.getmetrics()
h = ascent + descent
adv, off, px = [], [], bytearray()
for c in map(chr, range(32, 127)):
    w = max(1, round(font.getlength(c)))
    im = Image.new('L', (w, h))
    ImageDraw.Draw(im).text((0, 0), c, font=font, fill=255)
    adv.append(w), off.append(len(px)), px.extend(im.tobytes())
out = [f'/* tools/font.py: Liberation Sans Bold 20 px, ASCII 32-126, {h} rows */',
       f'#define FONT_H {h}',
       'static const unsigned char font_adv[95] = {' + ','.join(map(str, adv)) + '};',
       'static const unsigned short font_off[95] = {' + ','.join(map(str, off)) + '};',
       'static const unsigned char font_px[] = {']
out += [','.join(map(str, px[i:i + 40])) + ',' for i in range(0, len(px), 40)]
out.append('};')
(HERE.parent / 'font.h').write_text('\n'.join(out) + '\n')
