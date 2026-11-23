"""SCREENS.DMP (two Screen structs) -> PNG images, CGA or MDA colours."""
import sys, struct
from PIL import Image, ImageDraw, ImageFont
CGA = [(0,0,0),(0,0,170),(0,170,0),(0,170,170),(170,0,0),(170,0,170),(170,85,0),(170,170,170),
       (85,85,85),(85,85,255),(85,255,85),(85,255,255),(255,85,85),(255,85,255),(255,255,85),(255,255,255)]
font = ImageFont.truetype(sys.argv[3], 13)
CW, CH = 9, 16
d = open(sys.argv[1], 'rb').read()
SZ = 20 + 4000
for n in range(2):
    s = d[n*SZ:(n+1)*SZ]
    seg, snow, mono = struct.unpack('<3H', s[:6])
    if n == 1 and seg == 0: continue
    cells = struct.unpack('<2000H', s[20:])
    if all(c == 0xFFFF for c in cells): continue
    img = Image.new('RGB', (80*CW, 25*CH)); dr = ImageDraw.Draw(img)
    for i, c in enumerate(cells):
        if c == 0xFFFF: c = 0x0720
        ch, a = c & 255, c >> 8
        x, y = (i % 80) * CW, (i // 80) * CH
        if mono:   # MDA: green phosphor
            on, hi = (40,200,60), (120,255,140)
            if a == 0x70: fg, bg = (0,0,0), on
            else: fg, bg = (hi if a & 8 else on), (0,0,0)
        else:
            fg, bg = CGA[a & 15], CGA[(a >> 4) & 7]
        dr.rectangle([x, y, x+CW-1, y+CH-1], fill=bg)
        u = bytes([ch]).decode('cp437') if ch >= 32 else ' '
        if ch == 219: dr.rectangle([x,y,x+CW-1,y+CH-1], fill=fg)
        elif ch in (176,177,178):
            for yy in range(y, y+CH):
                for xx in range(x, x+CW):
                    if (xx+yy) % (4 if ch==176 else 2 if ch==177 else 4) == 0 or (ch==178 and (xx+yy)%4!=0):
                        img.putpixel((xx,yy), fg)
        elif ch == 221: dr.rectangle([x,y,x+CW//2-1,y+CH-1], fill=fg)
        elif ch == 222: dr.rectangle([x+CW//2,y,x+CW-1,y+CH-1], fill=fg)
        elif u.strip(): dr.text((x, y + 1), u, font=font, fill=fg)
    img.save('%s%d.png' % (sys.argv[2], n))
    print('wrote %s%d.png (%s)' % (sys.argv[2], n, 'MDA' if mono else 'CGA'))
