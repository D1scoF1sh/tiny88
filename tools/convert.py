"""TINY.BIN (Tiny-2.1, float scales) -> TINY88.BIN + TINY0.BIN for the 8088 port.

TINY88.BIN   "T88A", D, L, H, HID, cst.m, cst.e, then
             emb table, per layer: g1, [q k v unless layer 0], o, g2, f1, f2,
             then gf, then exp2[256].
             table = rows*cols weight bytes (w+128), rows * SF(m u16, e i16),
                     rows * row sum (i16)
             gains = D * i16, exponent i16
TINY0.BIN    "T880", D, then per token (97): q,k,v of layer 0 for the token
             part: 3 * (D * i16, SF); per position (128): pos weights D * i8,
             pos SF, then q,k,v position part 3 * (D * i16, SF)
"""
import sys, os, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ref import load
from isim import IntModel, VOCAB, CTX

m = load(sys.argv[1])
im = IntModel(m)
D, L, H, HID = im.D, im.L, im.H, im.HID
assert HID <= 256 and D <= 128 and D % 8 == 0 and HID % 8 == 0 and (D // H) % 8 == 0

def sf(s): return struct.pack('<Hh', s[0], s[1])

def table(t):
    w = (t['w'] + 128).astype('uint8')
    b = w.tobytes()
    b += b''.join(sf(s) for s in t['sf'])
    b += b''.join(struct.pack('<h', int(r)) for r in t['rsum'])
    return b

def gains(g):
    G, ge = g
    return b''.join(struct.pack('<h', int(v)) for v in G) + struct.pack('<h', ge)

out = bytearray(b'T88A' + struct.pack('<4H', D, L, H, HID) + sf(im.cst))
out += table(im.emb)
for l, ly in enumerate(im.layers):
    out += gains(ly['g1'])
    if l > 0:
        for k in 'qkv': out += table(ly[k])
    out += table(ly['o']) + gains(ly['g2']) + table(ly['f1']) + table(ly['f2'])
out += gains(im.gf)
out += b''.join(struct.pack('<H', v) for v in im.exp2)
open(sys.argv[2], 'wb').write(out)

def vec16(v): return b''.join(struct.pack('<h', x) for x in v[0]) + sf(v[1])
z = bytearray(b'T880' + struct.pack('<H', D))
for t in range(VOCAB):
    for j in range(3): z += vec16(im.ke[t][j])
for p in range(CTX):
    z += (im.pos['w'][p]).astype('int8').tobytes() + sf(im.pos['sf'][p])
    for j in range(3): z += vec16(im.kp[p][j])
open(sys.argv[3], 'wb').write(z)
print('TINY88.BIN %d bytes, TINY0.BIN %d bytes' % (len(out), len(z)))
