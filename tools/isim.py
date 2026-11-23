"""Bit-exact integer model of the TINY88 8088 inference engine.

Every operation here mirrors TINY88.C / KERNELS.ASM exactly (integer only,
floor shifts, C truncating division).  Used to (1) check accuracy against the
float reference, (2) build the prefix cache, (3) produce test vectors.

Scales are "SF" pseudo floats: (m, e), value m * 2**e, m in [2**15, 2**16).
"""
import numpy as np, math, struct

VOCAB, CTX = 97, 128
FX = 12                     # residual stream: int32, value * 4096

# ---------------- SF helpers (C: sf_* in TINY88.C) ----------------

def sf_from_float(v):
    if v == 0: return (0, 0)
    e = math.floor(math.log2(v)) - 15
    m = int(round(v / 2.0 ** e))
    if m >= 65536: m >>= 1; e += 1
    if m < 32768: m <<= 1; e -= 1
    return (m, e)

def sf_val(s): return s[0] * 2.0 ** s[1]

def sf_mul(a, b):
    if a[0] == 0 or b[0] == 0: return (0, 0)
    p = a[0] * b[0]
    if p >= 1 << 31: return (p >> 16, a[1] + b[1] + 16)
    return (p >> 15, a[1] + b[1] + 15)

def sf_norm32(v, e):
    """unsigned 32-bit v * 2**e -> SF"""
    if v == 0: return (0, 0)
    while v >= 65536: v >>= 1; e += 1
    while v < 32768: v <<= 1; e -= 1
    return (v, e)

def sf_shift(s, k): return (s[0], s[1] + k) if s[0] else s

STATS = {}
def stat(k, v):
    lo, hi = STATS.get(k, (v, v)); STATS[k] = (min(lo, v), max(hi, v))

def mul_shift(acc, m, n):
    """sign(acc) * floor(|acc| * m / 2**n), C: mul_shift()"""
    stat('ms_n', n)
    a = -acc if acc < 0 else acc
    stat('ms_a', a)
    if n >= 48:
        r = 0
    elif n >= 16:
        h, l = a >> 16, a & 0xFFFF
        r = (h * m + ((l * m) >> 16)) >> (n - 16)
    elif n >= 0:
        h, l = a >> 16, a & 0xFFFF
        r = ((h * m) << (16 - n)) + ((l * m) >> n)
    else:
        raise ValueError('negative shift %d' % n)
    assert r < 1 << 31, (acc, m, n)
    return -r if acc < 0 else r

def isqrt32(v):
    """C: isqrt32(), bitwise integer square root"""
    r, b = 0, 1 << 30
    while b > v: b >>= 2
    while b:
        if v >= r + b:
            v -= r + b; r = (r >> 1) + b
        else:
            r >>= 1
        b >>= 2
    return r

def tdiv(a, b):                      # C division, truncates toward zero
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b >= 0) else -q

# ---------------- vector quantisation ----------------

def norm16(x):
    """shift int32 vector so max |x| is in [2**14, 2**15). returns (xs, a)
       with x ~= xs * 2**a.  C: norm16()"""
    mx = max(abs(int(v)) for v in x)
    a = 0
    if mx == 0: return [0] * len(x), 0
    while mx >= 1 << 15: mx >>= 1; a += 1
    while mx < 1 << 14: mx <<= 1; a -= 1
    if a >= 0: xs = [int(v) >> a for v in x]
    else: xs = [int(v) << -a for v in x]
    return xs, a

def quant_s8(x):
    """int32 vector -> int8 vector q and SF scale s (x ~= q * s).  C: quant_s8()"""
    xs, a = norm16(x)
    ma = max(abs(v) for v in xs)
    if ma == 0: return [0] * len(x), (0, 0)
    r = (127 << 21) // ma
    q = []
    for v in xs:
        t = ((abs(v) * r) + (1 << 20)) >> 21
        q.append(-t if v < 0 else t)
    return q, sf_shift(sf_norm32((ma << 16) // 127, -16), a)

def quant_u8(x):
    """non-negative int32 vector -> 0..255 and SF scale.  C: quant_u8()"""
    xs, a = norm16(x)
    ma = max(xs)
    if ma <= 0: return [0] * len(x), (0, 0)
    r = (255 << 21) // ma
    q = [((v * r) + (1 << 20)) >> 21 for v in xs]
    return q, sf_shift(sf_norm32((ma << 16) // 255, -16), a)

def inv_rms(xs):
    """for xs from norm16: SF of 1 / (8 * sqrt(sum((xs>>3)^2)/D)).  C: inv_rms()"""
    D = len(xs)
    ss = sum((v >> 3) * (v >> 3) for v in xs)
    u = ss // D
    if u == 0: u = 1
    k = 0
    while u < 1 << 28: u <<= 2; k += 1
    while u >= 1 << 30: u >>= 2; k -= 1
    s = isqrt32(u)                       # sqrt(u_orig) = s / 2**k
    r = ((1 << 31) - 1) // s             # 1/s ~ r * 2**-31
    return sf_norm32(r, -31 + k - 3)

# ---------------- model ----------------

class IntModel:
    def __init__(self, m):
        """m: float model dict from ref.load().  Converts exactly like CONVERT.PY"""
        self.D, self.L, self.H, self.HID = m['D'], m['L'], m['H'], m['HID']
        self.HD = self.D // self.H
        D = self.D

        def table(t):
            sc, w = t
            return dict(w=w.astype(np.int64), sf=[sf_from_float(float(s)) for s in sc],
                        rsum=w.astype(np.int64).sum(1))
        def gains(g):
            ge = math.floor(math.log2(float(np.abs(g).max()))) - 14
            G = np.round(g.astype(np.float64) / 2.0 ** ge).astype(np.int64)
            assert np.abs(G).max() < 32768
            return G, ge
        self.emb = table(m['emb'])
        self.layers = []
        for l, ly in enumerate(m['layers']):
            d = {k: table(ly[k]) for k in ('q', 'k', 'v', 'o', 'f1', 'f2')}
            d['g1'], d['g2'] = gains(ly['g1']), gains(ly['g2'])
            self.layers.append(d)
        self.gf = gains(m['gf'])
        self.cst = sf_from_float(math.log2(math.e) / math.sqrt(self.HD) * 256)
        self.exp2 = [int(round(65535 * 2.0 ** (-f / 256.0))) for f in range(256)]
        # layer 0 q/k/v decomposed into token part + position part (TINY0.BIN)
        E = m['emb'][1].astype(np.float64) * m['emb'][0][:, None].astype(np.float64)
        P = m['pos'][1].astype(np.float64) * m['pos'][0][:, None].astype(np.float64)
        ly0 = m['layers'][0]; g = ly0['g1'].astype(np.float64)
        self.pos = table(m['pos'])
        def wmat(k): return ly0[k][1].astype(np.float64) * ly0[k][0][:, None].astype(np.float64)
        Wqkv = [wmat(k) * g[None, :] for k in ('q', 'k', 'v')]
        self.ke = [[self.vec16(W @ E[t]) for W in Wqkv] for t in range(VOCAB)]
        self.kp = [[self.vec16(W @ P[p]) for W in Wqkv] for p in range(CTX)]

    @staticmethod
    def vec16(v):
        """float vector -> (int16 list, SF) with |int| < 32768 (TINY0.BIN rows)"""
        mx = float(np.abs(v).max())
        s = sf_from_float(mx / 32000.0)
        q = [int(round(x / sf_val(s))) for x in v]
        assert max(abs(x) for x in q) < 32768
        return q, s

    # --- the matvec as the asm kernel + C correction computes it ---
    def matvec(self, t, q, qs, unsigned_in=False, rows=None):
        """q: int8 signed (or 0..255 if unsigned_in) list; qs: SF.
           returns int32 Q12 outputs."""
        W = t['w']
        x = np.array(q, np.int64)
        xu = x if unsigned_in else x + 128
        wu = W + 128
        U = wu @ xu                                     # the asm kernel
        assert U.max() < 1 << 24
        Sx = int(x.sum()); C = len(q)
        out = []
        for r in range(W.shape[0] if rows is None else rows):
            if unsigned_in: acc = int(U[r]) - 128 * Sx
            else: acc = int(U[r]) - 128 * Sx - 128 * int(t['rsum'][r]) - 16384 * C
            k = sf_mul(t['sf'][r], qs)
            out.append(mul_shift(acc, k[0], -(k[1] + FX)) if k[0] else 0)
        return out

    def norm(self, x, G):
        """RMS norm with gains -> int8 vector + SF"""
        Gv, ge = G
        xs, a = norm16(x)
        ir = inv_rms(xs)
        t = [v * int(g) for v, g in zip(xs, Gv)]
        q, s = quant_s8(t)
        return q, sf_shift(sf_mul(s, ir), ge)

    def reset(self):
        self.kc = [[None] * CTX for _ in range(self.L)]

    def step(self, tok, p, need_out=True):
        D, HD, H = self.D, self.HD, self.H
        # embedding: x0 = e*es + pos*ps  (Q12)
        e = self.emb['w'][tok]; es = self.emb['sf'][tok]
        pw = self.pos['w'][p]; ps = self.pos['sf'][p]
        x = [mul_shift(int(e[i]), es[0], -(es[1] + FX)) +
             mul_shift(int(pw[i]), ps[0], -(ps[1] + FX)) for i in range(D)]
        self.layer_act = []
        for l, ly in enumerate(self.layers):
            last = (l == self.L - 1) and not need_out
            if l == 0:
                xs, a = norm16(x)
                ir = sf_shift(inv_rms(xs), FX - a)          # 1/rms of real x0
                vecs = []
                for j in range(3):
                    (qe, se), (qp, sp) = self.ke[tok][j], self.kp[p][j]
                    v = [mul_shift(qe[i], se[0], -(se[1] + FX)) + mul_shift(qp[i], sp[0], -(sp[1] + FX))
                         for i in range(D)]
                    vecs.append(v)
                qf, kf, vf = vecs
                # these are W(g*x0) in Q12; scale by 1/rms when quantising
                kq, ks = quant_s8(kf); ks = sf_shift(sf_mul(ks, ir), -FX)
                vq, vs = quant_s8(vf); vs = sf_shift(sf_mul(vs, ir), -FX)
                if not last:
                    qq, qs = quant_s8(qf); qs = sf_shift(sf_mul(qs, ir), -FX)
            else:
                h, hs = self.norm(x, ly['g1'])
                kq, ks = quant_s8(self.matvec(ly['k'], h, hs)); ks = sf_shift(ks, -FX)
                vq, vs = quant_s8(self.matvec(ly['v'], h, hs)); vs = sf_shift(vs, -FX)
                if not last:
                    qq, qs = quant_s8(self.matvec(ly['q'], h, hs)); qs = sf_shift(qs, -FX)
            ksum = [sum(kq[hh * HD:(hh + 1) * HD]) for hh in range(H)]
            self.kc[l][p] = (kq, ks, vq, vs, ksum)
            if last:
                return None
            # attention
            emax = max(self.kc[l][j][3][1] for j in range(p + 1) if self.kc[l][j][3][0])
            cq = sf_mul(qs, self.cst)
            qu = np.array(qq, np.int64) + 128
            a_int = [0] * D
            for hh in range(H):
                sl = slice(hh * HD, (hh + 1) * HD)
                Sq = sum(qq[sl])
                z = []
                for j in range(p + 1):
                    kq_, ks_, _, _, ksum_ = self.kc[l][j]
                    U = int(qu[sl] @ (np.array(kq_[sl], np.int64) + 128))
                    raw = U - 128 * Sq - 128 * ksum_[hh] - 16384 * HD
                    c = sf_mul(cq, ks_)
                    z.append(mul_shift(raw, c[0], -c[1]) if c[0] else 0)
                zmax = max(z)
                ej = []
                for zz in z:
                    d = zmax - zz
                    ej.append(0 if (d >> 8) >= 16 else self.exp2[d & 255] >> (d >> 8))
                ssum = sum(ej)
                acc = [0] * HD
                csum = 0
                for j in range(p + 1):
                    _, _, vq_, vs_, _ = self.kc[l][j]
                    if ej[j] == 0 or vs_[0] == 0: continue
                    sh = 16 + emax - vs_[1]
                    c = (ej[j] * vs_[0]) >> sh if sh < 32 else 0
                    if c == 0: continue
                    csum += c
                    vv = vq_[sl]
                    for i in range(HD):
                        acc[i] += c * (vv[i] + 128)        # unsigned kernel
                den = ssum >> 8
                for i in range(HD):
                    a_int[hh * HD + i] = tdiv(acc[i] - 128 * csum, den)
            aq, as_ = quant_s8(a_int)
            as_ = sf_shift(as_, emax + 8)
            o = self.matvec(ly['o'], aq, as_)
            if getattr(self, 'trace', 0): print('L%d a' % l, a_int[:4], 'aq', aq[:4], as_, 'o', o[:4])
            x = [xi + oi for xi, oi in zip(x, o)]
            h, hs = self.norm(x, ly['g2'])
            f = self.matvec(ly['f1'], h, hs)
            f = [v if v > 0 else 0 for v in f]
            self.layer_act.append(sum(f))
            fq, fs = quant_u8(f); fs = sf_shift(fs, -FX)
            o = self.matvec(ly['f2'], fq, fs, unsigned_in=True)
            x = [xi + oi for xi, oi in zip(x, o)]
            stat('x', max(abs(v) for v in x))
            if getattr(self, 'trace', 0): print('L%d x' % l, x[:6])
        h, hs = self.norm(x, self.gf)
        return self.matvec(self.emb, h, hs)

    def answer(self, q, start=0):
        self.reset(); p = 0
        for c in q:
            self.step(ord(c) - 30, p, need_out=False); p += 1
        sc = self.step(1, p); p += 1; out = ''
        while p < CTX:
            t = int(np.argmax(sc))
            if t <= 1: break
            out += chr(t + 30); sc = self.step(t, p); p += 1
        return out
