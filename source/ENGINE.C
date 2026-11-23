/* ENGINE.C - the Tiny-2.1 network in integer arithmetic for the 8088.

   Same network as Tiny-2.1's TINY.C, but with no floating point at all:
   weights are bytes with one scale per row (as in TINY.BIN), every vector
   that goes into a weight table is turned into bytes too, sums are 32-bit
   integers, and scales are kept as 16-bit mantissa + exponent ("SF").
   The multiply loops are in KERNELS.ASM.  isim.py is a bit-exact Python
   model of this file.

   Builds with Open Watcom (wcl -0 -ml, DOS) and with any modern C compiler
   (then the kernels below in C are used; for tests and for building the
   prefix cache). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ENGINE.H"

/* ---- 16 x 16 -> 32 bit multiply: one MUL instruction on the 8088 ---- */
#ifdef __WATCOMC__
uint32_t umul16(uint16_t a, uint16_t b);
#pragma aux umul16 = "mul dx" parm [ax] [dx] value [dx ax] modify exact [ax dx] nomemory;
#else
#define umul16(a, b) ((uint32_t)(uint16_t)(a) * (uint16_t)(b))
#endif

/* ---- the multiply kernels ---- */
#ifdef __WATCOMC__
extern void __cdecl mv_u8(const uint8_t __far *w, unsigned stride, unsigned xoff,
                          unsigned cols, unsigned rows, uint32_t __far *out);
extern void __cdecl axpy_u8(unsigned accoff, const uint8_t __far *v, unsigned c, unsigned n);
#define NEAROFF(p) ((unsigned)(void __near *)(p))
#else
static void mv_u8(const uint8_t *w, unsigned stride, const uint8_t *x,
                  unsigned cols, unsigned rows, uint32_t *out)
{
    unsigned r, c;
    for (r = 0; r < rows; r++) {
        uint32_t s = 0;
        for (c = 0; c < cols; c++) s += (uint32_t)w[c] * x[c];
        out[r] = s;
        w += stride;
    }
}
static void axpy_u8(uint32_t *acc, const uint8_t *v, unsigned c, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++) acc[i] += (uint32_t)c * v[i];
}
#define NEAROFF(p) (p)
#endif

/* ---- network ---- */

int D, L, H, HD, HID;
static SF cst;
static uint16_t exp2t[256];

typedef struct {
    uint8_t FAR *w;           /* weights + 128 */
    SF FAR *sf;               /* row scales */
    int16_t FAR *rsum;        /* row sums of the signed weights */
    int rows, cols;
} Table;

static Table emb, wq[MAXL], wk[MAXL], wv[MAXL], wo[MAXL], f1[MAXL], f2[MAXL];
static int16_t g1[MAXL][MAXD], g2[MAXL][MAXD], gfv[MAXD];
static int16_t g1e[MAXL], g2e[MAXL], gfe;

/* memory of earlier letters, per layer: keys and values as bytes + 128 */
static uint8_t FAR *kc[MAXL], FAR *vc[MAXL];
static SF FAR *ks[MAXL], FAR *vs[MAXL];
static int16_t FAR *ksum[MAXL];

static FILE *f0;              /* TINY0.BIN: layer 0 from letter and place */
static long f0_pos;           /* where the position records start */
static unsigned f0_tok, f0_prec;

/* work buffers in DGROUP (the kernels read x through SS) */
static int32_t xr[MAXD];                   /* the residual stream, Q12 */
static int32_t t32[MAXHID];
static int16_t xs16[MAXHID];
static uint32_t uk[MAXHID > CTX ? MAXHID : CTX];
static uint8_t xin[MAXHID];
static int8_t q8[MAXHID], kq8[MAXD], vq8[MAXD];
static int16_t rd16[3 * MAXD];
static int32_t zj[CTX];
static uint16_t ej[CTX];
static uint32_t acc32[MAXD];

int32_t score[VOCAB];
int32_t layer_act[MAXL];
uint8_t att_map[MAXL][CTX];        /* where each layer looked, 0..255 */
void (*on_progress)(int layer, int stage);
void (*on_load)(int done, int total);
const char *engine_error;

/* ---- scales ---- */

static SF sf_mul(SF a, SF b)
{
    SF r;
    uint32_t p;
    if (a.m == 0 || b.m == 0) { r.m = 0; r.e = 0; return r; }
    p = umul16(a.m, b.m);
    if (p >= 0x80000000UL) { r.m = (uint16_t)(p >> 16); r.e = a.e + b.e + 16; }
    else { r.m = (uint16_t)(p >> 15); r.e = a.e + b.e + 15; }
    return r;
}

static SF sf_norm32(uint32_t v, int e)
{
    SF r;
    if (v == 0) { r.m = 0; r.e = 0; return r; }
    while (v >= 65536UL) { v >>= 1; e++; }
    while (v < 32768UL) { v <<= 1; e--; }
    r.m = (uint16_t)v; r.e = (int16_t)e;
    return r;
}

static SF sf_shift(SF s, int k)
{
    if (s.m) s.e += k;
    return s;
}

/* sign(acc) * floor(|acc| * m / 2^n) */
static int32_t mul_shift(int32_t acc, uint16_t m, int n)
{
    uint32_t a = acc < 0 ? (uint32_t)(-acc) : (uint32_t)acc, r;
    uint16_t h = (uint16_t)(a >> 16), l = (uint16_t)a;
    if (n >= 48 || n < 0) r = 0;
    else if (n >= 16) r = (umul16(h, m) + (umul16(l, m) >> 16)) >> (n - 16);
    else r = (umul16(h, m) << (16 - n)) + (umul16(l, m) >> n);
    return acc < 0 ? -(int32_t)r : (int32_t)r;
}

static uint16_t isqrt32(uint32_t v)
{
    uint32_t r = 0, b = 1UL << 30;
    while (b > v) b >>= 2;
    while (b) {
        if (v >= r + b) { v -= r + b; r = (r >> 1) + b; }
        else r >>= 1;
        b >>= 2;
    }
    return (uint16_t)r;
}

/* ---- vectors ---- */

/* xs = x shifted so the biggest is in [2^14, 2^15); x ~= xs * 2^a */
static int norm16(const int32_t *x, int n, int16_t *xs)
{
    uint32_t mx = 0, v;
    int i, a = 0;
    for (i = 0; i < n; i++) {
        v = x[i] < 0 ? (uint32_t)(-x[i]) : (uint32_t)x[i];
        if (v > mx) mx = v;
    }
    if (mx == 0) { for (i = 0; i < n; i++) xs[i] = 0; return 0; }
    while (mx >= 32768UL) { mx >>= 1; a++; }
    while (mx < 16384UL) { mx <<= 1; a--; }
    if (a >= 0) for (i = 0; i < n; i++) xs[i] = (int16_t)(x[i] >> a);
    else for (i = 0; i < n; i++) xs[i] = (int16_t)(x[i] << -a);
    return a;
}

/* int32 vector -> signed bytes, returns the scale */
static SF quant_s8(const int32_t *x, int n, int8_t *q)
{
    int i, a = norm16(x, n, xs16);
    uint16_t ma = 0, v, r;
    SF z;
    for (i = 0; i < n; i++) {
        v = (uint16_t)(xs16[i] < 0 ? -xs16[i] : xs16[i]);
        if (v > ma) ma = v;
    }
    if (ma == 0) { for (i = 0; i < n; i++) q[i] = 0; z.m = 0; z.e = 0; return z; }
    r = (uint16_t)((127L << 21) / ma);
    for (i = 0; i < n; i++) {
        v = (uint16_t)(xs16[i] < 0 ? -xs16[i] : xs16[i]);
        v = (uint16_t)((umul16(v, r) + (1UL << 20)) >> 21);
        q[i] = (int8_t)(xs16[i] < 0 ? -(int)v : (int)v);
    }
    return sf_shift(sf_norm32(((uint32_t)ma << 16) / 127, -16), a);
}

/* non-negative int32 vector -> 0..255 straight into the kernel input */
static SF quant_u8(const int32_t *x, int n, uint8_t *q)
{
    int i, a = norm16(x, n, xs16);
    int16_t ma = 0;
    uint16_t r;
    SF z;
    for (i = 0; i < n; i++) if (xs16[i] > ma) ma = xs16[i];
    if (ma <= 0) { for (i = 0; i < n; i++) q[i] = 0; z.m = 0; z.e = 0; return z; }
    r = (uint16_t)((255L << 21) / ma);
    for (i = 0; i < n; i++)
        q[i] = (uint8_t)((umul16((uint16_t)xs16[i], r) + (1UL << 20)) >> 21);
    return sf_shift(sf_norm32(((uint32_t)ma << 16) / 255, -16), a);
}

/* for xs from norm16: 1 / (8 * sqrt(mean((xs >> 3)^2))) */
static SF inv_rms(const int16_t *xs, int n)
{
    uint32_t ss = 0, u;
    int i, k = 0, t;
    uint16_t s;
    for (i = 0; i < n; i++) {
        t = xs[i] >> 3;
        if (t < 0) t = -t;
        ss += umul16(t, t);
    }
    u = ss / n;
    if (u == 0) u = 1;
    while (u < (1UL << 28)) { u <<= 2; k++; }
    while (u >= (1UL << 30)) { u >>= 2; k--; }
    s = isqrt32(u);
    return sf_norm32(0x7FFFFFFFUL / s, -31 + k - 3);
}

/* x (Q12) -> norm with gains -> signed bytes in q8, returns the scale */
static SF norm(const int32_t *x, const int16_t *g, int ge)
{
    int i, a = norm16(x, D, xs16);
    SF ir = inv_rms(xs16, D), s;
    (void)a;
    for (i = 0; i < D; i++) t32[i] = (int32_t)xs16[i] * g[i];
    s = quant_s8(t32, D, q8);
    return sf_shift(sf_mul(s, ir), ge);
}

/* out = table * vector (Q12).  in: xin[] already holds the kernel input
   (signed bytes + 128, or 0..255 when uns); sx = sum of the signed input */
static void matvec(const Table *t, SF qs, int32_t sx, int uns, int32_t *out)
{
    int r;
    int32_t corr = -128L * sx, acc;
    SF k;
    if (!uns) corr -= 16384L * t->cols;
    mv_u8(t->w, t->cols, NEAROFF(xin), t->cols, t->rows, uk);
    for (r = 0; r < t->rows; r++) {
        acc = (int32_t)uk[r] + corr;
        if (!uns) acc -= 128L * t->rsum[r];
        k = sf_mul(t->sf[r], qs);
        out[r] = k.m ? mul_shift(acc, k.m, -(k.e + FX)) : 0;
    }
}

/* signed bytes in q8 -> kernel input; returns their sum */
static int32_t to_xin(int n)
{
    int i;
    int32_t s = 0;
    for (i = 0; i < n; i++) { xin[i] = (uint8_t)(q8[i] + 128); s += q8[i]; }
    return s;
}

/* ---- loading ---- */

static int bad;
static FILE *fp_in;
static int tables_done, tables_total;

static void *get(unsigned n)
{
    void *p;
    if (bad) return NULL;
    p = malloc(n);
    if (p == NULL) { bad = 1; engine_error = "not enough memory (TINY88 needs about 500 KB free)"; }
    return p;
}

static void rd(void FAR *p, unsigned n)
{
    if (bad) return;
    if (fread(p, 1, n, fp_in) != n) { bad = 1; engine_error = "TINY88.BIN is too short or damaged"; }
}

static int rd16v(void)
{
    int16_t v = 0;
    rd(&v, 2);
    return v;
}

static void load_table(Table *t, int rows, int cols)
{
    t->rows = rows; t->cols = cols;
    t->w = (uint8_t FAR *)get((unsigned)rows * cols);
    t->sf = (SF FAR *)get(rows * sizeof(SF));
    t->rsum = (int16_t FAR *)get(rows * 2);
    if (bad) return;
    rd(t->w, (unsigned)rows * cols);
    rd(t->sf, rows * sizeof(SF));
    rd(t->rsum, rows * 2);
    tables_done++;
    if (on_load) on_load(tables_done, tables_total);
}

static void load_gains(int16_t *g, int16_t *e)
{
    rd(g, D * 2);
    *e = (int16_t)rd16v();
}

int engine_load(const char *dir)
{
    char name[96], magic[4];
    int l;
    bad = 0;
    sprintf(name, "%sTINY88.BIN", dir);
    fp_in = fopen(name, "rb");
    if (fp_in == NULL) { engine_error = "cannot open TINY88.BIN (keep it next to TINY88.EXE)"; return 0; }
    rd(magic, 4);
    if (bad || memcmp(magic, "T88A", 4) != 0) { engine_error = "TINY88.BIN is not a TINY88 network file"; fclose(fp_in); return 0; }
    D = rd16v(); L = rd16v(); H = rd16v(); HID = rd16v();
    if (D < 8 || D > MAXD || D % 8 || L < 1 || L > MAXL || H < 1 || D % H || (D / H) % 8 ||
        HID < 8 || HID > MAXHID || HID % 8) {
        engine_error = "TINY88.BIN has a network size this program cannot handle"; fclose(fp_in); return 0;
    }
    HD = D / H;
    rd(&cst, sizeof(SF));
    tables_done = 0;
    tables_total = 1 + 4 * L + 3 * (L - 1);
    load_table(&emb, VOCAB, D);
    for (l = 0; l < L && !bad; l++) {
        load_gains(g1[l], &g1e[l]);
        if (l > 0) {
            load_table(&wq[l], D, D);
            load_table(&wk[l], D, D);
            load_table(&wv[l], D, D);
        }
        load_table(&wo[l], D, D);
        load_gains(g2[l], &g2e[l]);
        load_table(&f1[l], HID, D);
        load_table(&f2[l], D, HID);
    }
    load_gains(gfv, &gfe);
    rd(exp2t, sizeof(exp2t));
    if (!bad && fgetc(fp_in) != EOF) { bad = 1; engine_error = "TINY88.BIN has the wrong size"; }
    fclose(fp_in);
    if (bad) return 0;

    for (l = 0; l < L && !bad; l++) {
        kc[l] = (uint8_t FAR *)get((unsigned)CTX * D);
        vc[l] = (uint8_t FAR *)get((unsigned)CTX * D);
        ks[l] = (SF FAR *)get(CTX * sizeof(SF));
        vs[l] = (SF FAR *)get(CTX * sizeof(SF));
        ksum[l] = (int16_t FAR *)get(CTX * H * 2);
    }
    if (bad) return 0;

    sprintf(name, "%sTINY0.BIN", dir);
    f0 = fopen(name, "rb");
    if (f0 == NULL) { engine_error = "cannot open TINY0.BIN (keep it next to TINY88.EXE)"; return 0; }
    if (fread(magic, 1, 4, f0) != 4 || memcmp(magic, "T880", 4) != 0) { engine_error = "TINY0.BIN is damaged"; return 0; }
    f0_tok = 3 * (D * 2 + sizeof(SF));
    f0_prec = D + sizeof(SF) + f0_tok;
    f0_pos = 6 + (long)VOCAB * f0_tok;
    return 1;
}

/* ---- one letter through the network ---- */

/* r: D int16 then SF -> Q12 int32 added into out (or stored when first) */
static void add_vec16(const int16_t *v, SF s, int32_t *out, int first)
{
    int i;
    for (i = 0; i < D; i++) {
        int32_t y = mul_shift(v[i], s.m, -(s.e + FX));
        out[i] = first ? y : out[i] + y;
    }
}

static void progress(int l, int stage)
{
    if (on_progress) on_progress(l, stage);
}

/* keys / values of letter p in layer l, from Q12 vectors in t32 */
static void keep_kv(int l, int p, const int32_t *k, const int32_t *v, SF extra)
{
    int i, h;
    SF s;
    uint8_t FAR *kd = kc[l] + (unsigned)p * D, FAR *vd = vc[l] + (unsigned)p * D;
    s = quant_s8(k, D, kq8);
    ks[l][p] = sf_shift(sf_mul(s, extra), -FX);
    s = quant_s8(v, D, vq8);
    vs[l][p] = sf_shift(sf_mul(s, extra), -FX);
    for (h = 0; h < H; h++) {
        int16_t sm = 0;
        for (i = 0; i < HD; i++) sm += kq8[h * HD + i];
        ksum[l][p * H + h] = sm;
    }
    for (i = 0; i < D; i++) { kd[i] = (uint8_t)(kq8[i] + 128); vd[i] = (uint8_t)(vq8[i] + 128); }
}

static SF one = { 32768U, -15 };

/* attention of layer l at position p; q is in q8 with scale qs.
   Leaves the mixed values as int32 in t32, returns their scale. */
static SF attend(int l, int p, SF qs, int32_t *aout)
{
    int h, j, i, emax = -32000;
    SF cq = sf_mul(qs, cst), c;
    int32_t Sq, raw, zmax, d;
    uint32_t ssum, csum, cc;
    int sh;
    uint8_t qu[MAXD];

    for (j = 0; j <= p; j++)
        if (vs[l][j].m && vs[l][j].e > emax) emax = vs[l][j].e;
    for (i = 0; i < D; i++) qu[i] = (uint8_t)(q8[i] + 128);
    for (i = 0; i <= p; i++) att_map[l][i] = 0;
    for (h = 0; h < H; h++) {
        Sq = 0;
        for (i = 0; i < HD; i++) { Sq += q8[h * HD + i]; xin[i] = qu[h * HD + i]; }
        mv_u8(kc[l] + h * HD, D, NEAROFF(xin), HD, p + 1, uk);
        zmax = -0x7FFFFFFFL;
        for (j = 0; j <= p; j++) {
            raw = (int32_t)uk[j] - 128L * Sq - 128L * ksum[l][j * H + h] - 16384L * HD;
            c = sf_mul(cq, ks[l][j]);
            zj[j] = c.m ? mul_shift(raw, c.m, -c.e) : 0;
            if (zj[j] > zmax) zmax = zj[j];
        }
        ssum = 0;
        for (j = 0; j <= p; j++) {
            d = zmax - zj[j];
            ej[j] = (d >> 8) >= 16 ? 0 : (uint16_t)(exp2t[(int)(d & 255)] >> (int)(d >> 8));
            ssum += ej[j];
        }
        for (i = 0; i < HD; i++) acc32[i] = 0;
        csum = 0;
        for (j = 0; j <= p; j++) {
            if (ej[j] == 0 || vs[l][j].m == 0) continue;
            sh = 16 + emax - vs[l][j].e;
            cc = sh < 32 ? umul16(ej[j], vs[l][j].m) >> sh : 0;
            if (cc == 0) continue;
            csum += cc;
            axpy_u8(NEAROFF(acc32), vc[l] + (unsigned)j * D + h * HD, (unsigned)cc, HD);
        }
        for (j = 0; j <= p; j++) {
            uint16_t a = (uint16_t)(((uint32_t)ej[j] * 64 / (ssum / 4 + 1)));
            a = att_map[l][j] + a / H;
            att_map[l][j] = (uint8_t)(a > 255 ? 255 : a);
        }
        for (i = 0; i < HD; i++)
            aout[h * HD + i] = (int32_t)(acc32[i] - 128UL * csum) / (int32_t)(ssum >> 8);
    }
    return sf_shift(one, emax + 8);
}

/* feed letter tok at position p.  With need_out, fills score[]. */
void engine_step(int tok, int p, int need_out)
{
    int l, i, j, last;
    int32_t sx;
    SF qs, s, ir, as;
    const uint8_t FAR *e = emb.w + (unsigned)tok * D;
    int8_t pw[MAXD];
    SF ps, es = emb.sf[tok];

    /* layer 0 parts that depend only on the letter and on the position */
    fseek(f0, 6 + (long)tok * f0_tok, SEEK_SET);
    fread(rd16, 1, f0_tok, f0);
    fseek(f0, f0_pos + (long)p * f0_prec, SEEK_SET);
    fread(pw, 1, D, f0);
    fread(&ps, 1, sizeof(SF), f0);
    for (i = 0; i < D; i++)
        xr[i] = mul_shift((int)e[i] - 128, es.m, -(es.e + FX)) + mul_shift(pw[i], ps.m, -(ps.e + FX));

    for (l = 0; l < L; l++) {
        last = (l == L - 1) && !need_out;
        progress(l, 0);
        if (l == 0) {
            int16_t *rq = rd16, *rk = rd16 + D + 2, *rv = rd16 + 2 * (D + 2);
            int16_t pq[3 * (MAXD + 2)];
            /* 1/rms of the real x0 = inv_rms * 2^(12 - a) */
            i = norm16(xr, D, xs16);
            ir = sf_shift(inv_rms(xs16, D), FX - i);
            fread(pq, 1, f0_tok, f0);
            /* k */
            add_vec16(rk, *(SF *)(rk + D), t32, 1);
            add_vec16(pq + D + 2, *(SF *)(pq + 2 * D + 2), t32, 0);
            add_vec16(rv, *(SF *)(rv + D), (int32_t *)zj, 1);
            add_vec16(pq + 2 * (D + 2), *(SF *)(pq + 2 * (D + 2) + D), (int32_t *)zj, 0);
            keep_kv(0, p, t32, (int32_t *)zj, ir);
            if (!last) {
                add_vec16(rq, *(SF *)(rq + D), t32, 1);
                add_vec16(pq, *(SF *)(pq + D), t32, 0);
                qs = sf_shift(sf_mul(quant_s8(t32, D, q8), ir), -FX);
            }
        } else {
            s = norm(xr, g1[l], g1e[l]);
            sx = to_xin(D);
            matvec(&wk[l], s, sx, 0, t32);
            matvec(&wv[l], s, sx, 0, (int32_t *)zj);
            keep_kv(l, p, t32, (int32_t *)zj, one);
            if (!last) {
                matvec(&wq[l], s, sx, 0, t32);
                qs = sf_shift(quant_s8(t32, D, q8), -FX);
            }
        }
        if (last) return;
        progress(l, 1);
        as = attend(l, p, qs, t32);
        as = sf_mul(quant_s8(t32, D, q8), as);
        sx = to_xin(D);
        matvec(&wo[l], as, sx, 0, t32);
        for (i = 0; i < D; i++) xr[i] += t32[i];
        progress(l, 2);
        s = norm(xr, g2[l], g2e[l]);
        sx = to_xin(D);
        matvec(&f1[l], s, sx, 0, t32);
        layer_act[l] = 0;
        for (j = 0; j < HID; j++) {
            if (t32[j] < 0) t32[j] = 0;
            layer_act[l] += t32[j] >> 4;
        }
        s = sf_shift(quant_u8(t32, HID, xin), -FX);
        for (sx = 0, j = 0; j < HID; j++) sx += xin[j];
        matvec(&f2[l], s, sx, 1, t32);
        for (i = 0; i < D; i++) xr[i] += t32[i];
    }
    progress(L, 0);
    s = norm(xr, gfv, gfe);
    sx = to_xin(D);
    matvec(&emb, s, sx, 0, score);
}

/* ---- the memory of earlier letters as one record (prefix cache) ---- */

unsigned engine_kv_size(void)
{
    return L * (2 * D + 2 * sizeof(SF) + H * 2);
}

void engine_kv_get(int p, uint8_t *buf)
{
    int l;
    for (l = 0; l < L; l++) {
        _fmemcpy(buf, kc[l] + (unsigned)p * D, D); buf += D;
        _fmemcpy(buf, vc[l] + (unsigned)p * D, D); buf += D;
        _fmemcpy(buf, &ks[l][p], sizeof(SF)); buf += sizeof(SF);
        _fmemcpy(buf, &vs[l][p], sizeof(SF)); buf += sizeof(SF);
        _fmemcpy(buf, &ksum[l][p * H], H * 2); buf += H * 2;
    }
}

void engine_kv_put(int p, const uint8_t *buf)
{
    int l;
    for (l = 0; l < L; l++) {
        _fmemcpy(kc[l] + (unsigned)p * D, buf, D); buf += D;
        _fmemcpy(vc[l] + (unsigned)p * D, buf, D); buf += D;
        _fmemcpy(&ks[l][p], buf, sizeof(SF)); buf += sizeof(SF);
        _fmemcpy(&vs[l][p], buf, sizeof(SF)); buf += sizeof(SF);
        _fmemcpy(&ksum[l][p * H], buf, H * 2); buf += H * 2;
    }
}

/* ---- choosing letters ---- */

int creativity = 5;
int last_conf;                 /* 0..1000: how sure the network was */
static uint32_t rnd_state = 1;

void engine_seed(uint32_t s) { rnd_state = s | 1; }

static uint16_t rnd15(void)
{
    rnd_state = rnd_state * 1103515245UL + 12345UL;
    return (uint16_t)((rnd_state >> 16) & 0x7FFF);
}

/* 2^(-z/256) * 65535 */
static uint16_t ex2(int32_t z)
{
    if (z < 0) z = 0;
    if ((z >> 8) >= 16) return 0;
    return (uint16_t)(exp2t[(int)(z & 255)] >> (int)(z >> 8));
}

static int chosen(int tok, int best)
{
    int i;
    uint32_t sum = 0, et = 0, e;
    int32_t d;
    for (i = 0; i < VOCAB; i++) {
        d = score[best] - score[i];               /* Q12; e^-x = 2^(-x*1.4427) */
        if (d > 0x3FFFFL) d = 0x3FFFFL;
        e = ex2((int32_t)(((d >> 4) * 2955L) >> 11));
        sum += e;
        if (i == tok) et = e;
    }
    last_conf = sum ? (int)(et * 1000UL / sum) : 0;
    return tok;
}

int engine_pick(void)
{
    int i, best = 0;
    uint32_t sum = 0, r;
    int32_t d;
    for (i = 1; i < VOCAB; i++)
        if (score[i] > score[best]) best = i;
    if (creativity <= 0)
        return chosen(best, best);
    for (i = 0; i < VOCAB; i++) {
        d = score[best] - score[i];
        if (d > 0x3FFFFL) d = 0x3FFFFL;
        /* exp(-d / (4096 * creativity/5)) = 2^(-z/256), z = d*1.4427*5*256/(4096*c) */
        ej[i] = ex2((int32_t)(((d >> 4) * 14773L) >> 11) / creativity);
        if (ej[i] < 9830) ej[i] = 0;        /* never pick unlikely letters */
        sum += ej[i];
    }
    r = ((sum >> 7) * rnd15()) >> 8;
    for (i = 0; i < VOCAB; i++) {
        if (ej[i] == 0) continue;
        if (r < ej[i]) return chosen(i, best);
        r -= ej[i];
    }
    return chosen(best, best);
}

/* the n most likely next letters, with their chance in 1/1000 */
int engine_top(int n, int *tok, int *pm)
{
    int i, k, best = 0, used[VOCAB];
    uint32_t sum = 0;
    int32_t d;
    for (i = 1; i < VOCAB; i++)
        if (score[i] > score[best]) best = i;
    for (i = 0; i < VOCAB; i++) {
        d = score[best] - score[i];
        if (d > 0x3FFFFL) d = 0x3FFFFL;
        ej[i] = ex2((int32_t)(((d >> 4) * 2955L) >> 11));
        sum += ej[i];
        used[i] = i == 0;
    }
    for (k = 0; k < n; k++) {
        int b = -1;
        for (i = 0; i < VOCAB; i++)
            if (!used[i] && (b < 0 || ej[i] > ej[b])) b = i;
        used[b] = 1;
        tok[k] = b;
        pm[k] = sum ? (int)(ej[b] * 1000UL / sum) : 0;
    }
    return n;
}
