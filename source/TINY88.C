/* TINY88.C - Tiny-2.1 for the IBM PC 5150 (8088, no 8087, CGA or MDA).

   A port of Tiny-2.1 (github.com/WollyMonjac/Tiny-2.1).  The network runs
   in integer arithmetic (ENGINE.C, KERNELS.ASM) because floating point
   emulation on a 4.77 MHz 8088 would take hours per answer.

   Files: TINY88.EXE, TINY88.BIN (the network), TINY0.BIN (layer 0 tables),
          TINYPFX.BIN (optional: memory of common question beginnings).

   Build (Open Watcom):  wasm -0 -ml KERNELS.ASM
                         wcl -0 -ml -os -s -k8192 -fe=TINY88.EXE TINY88.C ENGINE.C KERNELS.OBJ
   Native test build:    cc -O2 -o tiny88 TINY88.C ENGINE.C */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ENGINE.H"

#define MAXQ 72                 /* longest question, in letters */

static char exe_dir[80];
static int use_pfx = 1;
static int abort_think;

/* ---- prefix cache: the memory of common question beginnings ----
   TINYPFX.BIN: "T88P", nodes, row size, then the trie (4 bytes a node:
   first child, child count, letter; children contiguous and sorted), then
   one memory record per node (except the root): the network's keys and
   values for the node's last letter. */

static FILE *pfx;
static unsigned pfx_nodes, pfx_row;
static long pfx_rows_at;
static uint8_t kvbuf[8 * (2 * MAXD + 8 + 16)];

static void pfx_open(void)
{
    char name[96], m[4];
    unsigned char h[4];
    sprintf(name, "%sTINYPFX.BIN", exe_dir);
    pfx = fopen(name, "rb");
    if (pfx == NULL) return;
    if (fread(m, 1, 4, pfx) != 4 || memcmp(m, "T88P", 4) != 0 || fread(h, 1, 4, pfx) != 4) {
        fclose(pfx); pfx = NULL; return;
    }
    pfx_nodes = h[0] | ((unsigned)h[1] << 8);
    pfx_row = h[2] | ((unsigned)h[3] << 8);
    if (pfx_row != engine_kv_size()) { fclose(pfx); pfx = NULL; return; }
    pfx_rows_at = 8 + 4L * pfx_nodes;
}

/* restores the memory for the longest known beginning of q, returns its length */
static int pfx_recall(const char *q)
{
    static unsigned char kids[4 * 96];
    static unsigned path[MAXQ];
    unsigned char rec[4];
    unsigned node = 0, first, n, i;
    int depth = 0, k, found;
    if (!pfx || !use_pfx) return 0;
    while (q[depth]) {
        fseek(pfx, 8 + 4L * node, SEEK_SET);
        if (fread(rec, 1, 4, pfx) != 4) break;
        first = rec[0] | ((unsigned)rec[1] << 8);
        n = rec[2];
        if (n == 0 || n > 96) break;
        fseek(pfx, 8 + 4L * first, SEEK_SET);
        if (fread(kids, 1, 4 * n, pfx) != 4 * n) break;
        found = 0;
        for (i = 0; i < n; i++)
            if (kids[4 * i + 3] == (unsigned char)q[depth]) { found = 1; break; }
        if (!found) break;
        node = first + i;
        path[depth++] = node;
    }
    for (k = 0; k < depth; k++) {
        fseek(pfx, pfx_rows_at + (long)(path[k] - 1) * pfx_row, SEEK_SET);
        if (fread(kvbuf, 1, pfx_row, pfx) != pfx_row) return k;
        engine_kv_put(k, kvbuf);
    }
    return depth;
}

/* ---- answering ---- */

static int recalled;            /* letters of the last question from memory */

/* Answer q into out[]. The callbacks (may be NULL) hear about progress
   through the question and about every new letter. */
static int think(const char *q, char *out, void (*on_step)(int i, int n), void (*on_letter)(int c))
{
    int p, n = (int)strlen(q), tok, letters = 0;
    abort_think = 0;
    p = recalled = pfx_recall(q);
    for (; q[p]; p++) {
        if (on_step) on_step(p, n);
        engine_step(q[p] - 30, p, 0);
        if (abort_think) { out[0] = 0; return 0; }
    }
    if (on_step) on_step(n, n);
    engine_step(1, p++, 1);                      /* end of the question */
    for (; p < CTX && !abort_think; p++) {
        tok = engine_pick();
        if (tok <= 1) break;                     /* end of the answer */
        out[letters++] = (char)(tok + 30);
        if (on_letter) on_letter(tok + 30);
        if (p + 1 < CTX) engine_step(tok, p, 1);
    }
    out[letters] = 0;
    return letters;
}

/* printable letters only, no trailing spaces, CAPS LOCK text in lower case */
static int clean(const char *line, char *q)
{
    int i, n = 0, lower = 0, upper = 0;
    for (i = 0; line[i] && n < MAXQ; i++)
        if (line[i] >= 32 && line[i] <= 126)
            q[n++] = line[i];
    while (n > 0 && q[n - 1] == ' ')
        n--;
    q[n] = 0;
    for (i = 0; i < n; i++) {
        if (q[i] >= 'a' && q[i] <= 'z') lower++;
        if (q[i] >= 'A' && q[i] <= 'Z') upper++;
    }
    if (upper > 0 && lower == 0)
        for (i = 0; i < n; i++)
            if (q[i] >= 'A' && q[i] <= 'Z') q[i] += 'a' - 'A';
    return n;
}

static int is_word(const char *q, const char *w)
{
    int i;
    for (i = 0; q[i] && w[i]; i++)
        if ((q[i] | 32) != w[i]) return 0;
    return q[i] == 0 && w[i] == 0;
}

static void fail(const char *msg);

static void load_all(void)
{
    if (!engine_load(exe_dir)) fail(engine_error);
    pfx_open();
}

/* ---- plain text mode ---- */

static void plain_letter(int c)
{
    putchar(c);
    fflush(stdout);
}

static int plain_main(void)
{
    static char line[256], q[MAXQ + 1], out[CTX + 1];
    int letters;
    clock_t t0;
    long tenths;

    printf("TINY 2.1/88 - a neural network that answers questions.\n");
    printf("Loading TINY88.BIN ...\n");
    load_all();
    printf("Ready: %d layers, %d wide. Creativity %d (TINY88 0 to TINY88 9).%s\n",
           L, D, creativity, pfx ? " Memory: TINYPFX.BIN." : "");
    printf("On a 4.77 MHz PC an answer takes a minute or more. Type QUIT to stop.\n");
    for (;;) {
        printf("\n> ");
        fflush(stdout);
        if (fgets(line, sizeof(line), stdin) == NULL)
            break;
        if (clean(line, q) == 0)
            continue;
        if (is_word(q, "quit") || is_word(q, "exit"))
            break;
        printf("  ");
        t0 = clock();
        letters = think(q, out, NULL, plain_letter);
        tenths = (long)((clock() - t0) * 10L / CLOCKS_PER_SEC);
        printf("\n   [%d letters in %ld.%ld seconds, %d from memory]\n", letters, tenths / 10, tenths % 10, recalled);
    }
    printf("Goodbye!\n");
    return 0;
}

#ifndef __WATCOMC__
/* ==================================================================
   Tools for the PC that builds the files (not in the DOS program)
   ================================================================== */

static void fail(const char *msg)
{
    printf("\nTINY88: %s\n", msg);
    exit(1);
}

/* answer questions from stdin, one per line, deterministic */
static int test_main(void)
{
    static char line[256], q[MAXQ + 1], out[CTX + 1];
    load_all();
    creativity = 0;
    while (fgets(line, sizeof(line), stdin)) {
        if (clean(line, q) == 0) continue;
        think(q, out, NULL, NULL);
        printf("%s | %s | %d\n", q, out, recalled);
        fflush(stdout);
    }
    return 0;
}

static unsigned char *nodes;
static FILE *pout;
static long rows_at;
static unsigned done_nodes;

static void visit(unsigned node, int depth)
{
    unsigned first = nodes[4 * node] | (nodes[4 * node + 1] << 8), n = nodes[4 * node + 2], i;
    if (node) {
        engine_step(nodes[4 * node + 3] - 30, depth - 1, 0);
        engine_kv_get(depth - 1, kvbuf);
        fseek(pout, rows_at + (long)(node - 1) * engine_kv_size(), SEEK_SET);
        fwrite(kvbuf, 1, engine_kv_size(), pout);
        if (++done_nodes % 500 == 0) fprintf(stderr, "%u nodes\n", done_nodes);
    }
    for (i = 0; i < n; i++) visit(first + i, depth + 1);
}

/* PFXNODES.BIN (from mktrie.py) -> TINYPFX.BIN */
static int mkpfx_main(const char *in, const char *outname)
{
    FILE *f = fopen(in, "rb");
    unsigned char h[6];
    unsigned n, rs;
    load_all();
    if (!f || fread(h, 1, 6, f) != 6 || memcmp(h, "T88N", 4)) fail("bad node file");
    n = h[4] | (h[5] << 8);
    nodes = malloc(4 * n);
    if (fread(nodes, 1, 4 * n, f) != 4 * n) fail("short node file");
    fclose(f);
    rs = engine_kv_size();
    pout = fopen(outname, "wb");
    fwrite("T88P", 1, 4, pout);
    h[0] = n & 255; h[1] = n >> 8; h[2] = rs & 255; h[3] = rs >> 8;
    fwrite(h, 1, 4, pout);
    fwrite(nodes, 1, 4 * n, pout);
    rows_at = 8 + 4L * n;
    visit(0, 0);
    fclose(pout);
    fprintf(stderr, "%u nodes, %u bytes each\n", n, rs);
    return 0;
}

int main(int argc, char **argv)
{
    strcpy(exe_dir, "");
    if (argc > 2 && strcmp(argv[1], "-dir") == 0) { strcpy(exe_dir, argv[2]); argv += 2; argc -= 2; }
    if (argc > 1 && strcmp(argv[1], "-nopfx") == 0) { use_pfx = 0; argv++; argc--; }
    if (argc > 1 && strcmp(argv[1], "-t") == 0) return test_main();
    if (argc > 3 && strcmp(argv[1], "-mkpfx") == 0) return mkpfx_main(argv[2], argv[3]);
    if (argc > 1 && argv[1][0] >= '0' && argv[1][0] <= '9') creativity = argv[1][0] - '0';
    engine_seed((uint32_t)time(NULL));
    return plain_main();
}

#else
/* ==================================================================
   The DOS screen: direct video memory on CGA (with snow avoidance) or
   MDA, the PC speaker for sound, and with /DUAL both screens at once.
   ================================================================== */
#include <conio.h>
#include <dos.h>
#include <i86.h>
#include <io.h>

#define putch    v_putch
#define cputs    v_cputs
#define window   v_window
#define gotoxy   v_gotoxy
#define wherex   v_wherex
#define wherey   v_wherey
#define clrscr   v_clrscr
#define textattr v_textattr
#define sound    spk_on
#define nosound  spk_off
#define cursor   v_cursor

extern void __cdecl vpoke(unsigned seg, unsigned off, unsigned cell, unsigned snow);
extern void __cdecl vblit(unsigned seg, unsigned off, const uint16_t __far *src, unsigned n, unsigned snow);

#define SCRW   80
#define CHAT_L 3
#define CHAT_T 3
#define CHAT_R 56
#define CHAT_B 20
#define CHATW  (CHAT_R - CHAT_L + 1)
#define MON_X  61
#define IN_Y   23

#define C_FRAME   11
#define C_FRAME2  3
#define C_TITLE   0x1B
#define C_TITLE2  0x1F
#define C_PANEL   0x3F
#define C_YOU     13
#define C_QTEXT   14
#define C_TINY    11
#define C_ANSWER  10
#define C_GLOW    15
#define C_SPEAK   0x2F
#define C_DIM     8
#define C_TEXT    7
#define C_BRIGHT  15
#define C_BAR     0x30
#define C_BARKEY  0x3F
#define C_BARON   0x3E
#define C_HOT     12

/* ---- screens ---- */

typedef struct {
    unsigned seg, snow, mono;
    int wl, wt, wr, wb;          /* window, 1-based, inclusive */
    int x, y;                    /* cursor inside the window */
    int attr;
    uint16_t cells[SCRW * 25];   /* copy of the screen (no reading of video memory) */
} Screen;

static Screen scr[2];
static Screen *S = &scr[0];      /* where text goes now */
static int dumps, force_dual;
static int dual, force_mono, snow_opt = -1, sound_on = 1, voice_on = 1, demo, skip_boot;
static int mono_mon;             /* the second screen is the MDA */
static unsigned cursor_shape = 0x0607;
static char last_q[MAXQ + 1];

static void use(int n) { S = &scr[n]; }

static void color(int a)
{
    if (S->mono) {
        int fg = a & 15, bg = (a >> 4) & 7;
        if (bg) a = 0x70;
        else a = fg == 8 ? 7 : fg >= 9 ? 15 : 7;
    }
    S->attr = a;
}

static void textattr(int a) { S->attr = a; }

static void cell(int x, int y, int ch, int a)          /* 1-based, absolute */
{
    unsigned i = (unsigned)(y - 1) * SCRW + (x - 1);
    uint16_t w = (uint16_t)((a << 8) | (ch & 255));
    if (x < 1 || x > SCRW || y < 1 || y > 25) return;
    if (S->cells[i] == w) return;
    S->cells[i] = w;
    vpoke(S->seg, i * 2, w, S->snow);
}

static void window(int l, int t, int r, int b)
{
    S->wl = l; S->wt = t; S->wr = r; S->wb = b;
    S->x = S->y = 1;
}

static void full(void) { window(1, 1, SCRW, 25); }

static void gotoxy(int x, int y) { S->x = x; S->y = y; }
static int wherex(void) { return S->x; }
static int wherey(void) { return S->y; }

static void scroll_up(void)
{
    int y, w = S->wr - S->wl + 1, x;
    for (y = S->wt; y < S->wb; y++) {
        uint16_t *d = &S->cells[(y - 1) * SCRW + S->wl - 1], *s = d + SCRW;
        for (x = 0; x < w; x++) d[x] = s[x];
        vblit(S->seg, ((y - 1) * SCRW + S->wl - 1) * 2, d, w, S->snow);
    }
    for (x = S->wl; x <= S->wr; x++) cell(x, S->wb, ' ', S->attr);
}

static void newline(void)
{
    if (S->wt + S->y - 1 >= S->wb) {
        if (S->wt > 1 || S->wb < 25) scroll_up();
    }
    else S->y++;
}

static void putch(int c)
{
    int w = S->wr - S->wl + 1;
    if (c == '\r') { S->x = 1; return; }
    if (c == '\n') { newline(); return; }
    if (c == 7) return;
    cell(S->wl + S->x - 1, S->wt + S->y - 1, c, S->attr);
    if (++S->x > w) { S->x = 1; newline(); }
}

static void cputs(const char *s) { while (*s) putch((unsigned char)*s++); }

static void clrscr(void)
{
    int x, y;
    for (y = S->wt; y <= S->wb; y++)
        for (x = S->wl; x <= S->wr; x++) cell(x, y, ' ', S->attr);
    S->x = S->y = 1;
}

/* the BIOS cursor is only on the main screen */
static void cursor(int on)
{
    union REGS r;
    if (!on) {
        r.h.ah = 3; r.h.bh = 0;
        int86(0x10, &r, &r);
        if (r.w.cx != 0x2000) cursor_shape = r.w.cx;
        r.w.cx = 0x2000;
    } else
        r.w.cx = cursor_shape;
    r.h.ah = 1;
    int86(0x10, &r, &r);
}

static void place_cursor(void)
{
    union REGS r;
    r.h.ah = 2; r.h.bh = 0;
    r.h.dh = (unsigned char)(scr[0].wt + scr[0].y - 2);
    r.h.dl = (unsigned char)(scr[0].wl + scr[0].x - 2);
    int86(0x10, &r, &r);
}

/* ---- adapters ---- */

static int crtc_here(unsigned port)
{
    unsigned char old, v;
    outp(port, 0x0F);
    old = (unsigned char)inp(port + 1);
    outp(port + 1, old ^ 0x5A);
    v = (unsigned char)inp(port + 1);
    v = (unsigned char)inp(port + 1);
    outp(port + 1, old);
    return v == (old ^ 0x5A);
}

static int has_ega_or_vga(void)
{
    union REGS r;
    r.w.ax = 0x1A00;
    int86(0x10, &r, &r);
    if (r.h.al == 0x1A) return 1;
    r.h.ah = 0x12; r.h.bl = 0x10;
    int86(0x10, &r, &r);
    return r.h.bl != 0x10;
}

/* set a text mode on the MDA (mda=1) or the CGA through the BIOS, by
   pointing the equipment word at that adapter for a moment */
static void bios_mode(int mda)
{
    union REGS r;
    unsigned __far *eq = (unsigned __far *)MK_FP(0x40, 0x10);
    unsigned save = *eq;
    *eq = (save & ~0x30) | (mda ? 0x30 : 0x20);
    r.h.ah = 0; r.h.al = mda ? 7 : 3;
    int86(0x10, &r, &r);
    *eq = save;
}

static int main_is_mda;

static void screens_init(void)
{
    union REGS r;
    int i, other_ok;
    r.h.ah = 0x0F;
    int86(0x10, &r, &r);
    main_is_mda = r.h.al == 7;
    if (!main_is_mda && r.h.al != 2 && r.h.al != 3) {      /* 40 columns or graphics */
        r.h.ah = 0; r.h.al = 3; int86(0x10, &r, &r);
    }
    if (main_is_mda == 0 && r.h.al == 2) force_mono = 1;  /* BW80 */
    scr[0].seg = main_is_mda ? 0xB000 : 0xB800;
    scr[0].mono = main_is_mda || force_mono;
    scr[0].snow = !main_is_mda && !has_ega_or_vga();
    if (snow_opt >= 0 && !main_is_mda) scr[0].snow = snow_opt;
    if (dual) {
        other_ok = force_dual || (main_is_mda ? crtc_here(0x3D4) : crtc_here(0x3B4));
        if (!other_ok) dual = 0;
    }
    if (dual) {
        mono_mon = !main_is_mda;
        bios_mode(mono_mon);                 /* the monitor screen ...   */
        r.h.ah = 1; r.w.cx = 0x2000; int86(0x10, &r, &r);
        bios_mode(main_is_mda);              /* ... then back to the main */
        scr[1].seg = mono_mon ? 0xB000 : 0xB800;
        scr[1].mono = mono_mon || force_mono;
        scr[1].snow = !mono_mon && !has_ega_or_vga();
        if (snow_opt >= 0 && !mono_mon) scr[1].snow = snow_opt;
    }
    for (i = 0; i < 2; i++) {
        unsigned k;
        for (k = 0; k < SCRW * 25; k++) scr[i].cells[k] = 0xFFFF;   /* force a full draw */
        scr[i].attr = 7;
        scr[i].wl = scr[i].wt = 1; scr[i].wr = SCRW; scr[i].wb = 25;
        scr[i].x = scr[i].y = 1;
    }
    use(0);
}

/* ---- time ---- */

static unsigned long ticks(void)
{
    return *(volatile unsigned long __far *)MK_FP(0x40, 0x6C);
}

static unsigned pit_read(void)
{
    unsigned lo, hi;
    _disable();
    outp(0x43, 0);
    lo = inp(0x40);
    hi = inp(0x40);
    _enable();
    return (hi << 8) | lo;
}

static unsigned long pit_per_ms = 2386;  /* mode 3 counts by 2 at 1.19 MHz */

static void calibrate(void)
{
    unsigned long t = ticks(), sum = 0;
    unsigned prev, cur;
    while (ticks() == t) ;
    t = ticks();
    prev = pit_read();
    while (ticks() == t) {
        cur = pit_read();
        sum += (unsigned)(prev - cur);
        prev = cur;
    }
    if (sum > 30000UL) pit_per_ms = sum * 1000UL / 54925UL;
}

static void delay_ms(unsigned ms)
{
    unsigned long need = (unsigned long)ms * pit_per_ms, el = 0;
    unsigned prev = pit_read(), cur;
    while (el < need) {
        cur = pit_read();
        el += (unsigned)(prev - cur);
        prev = cur;
    }
}

/* ---- sound ---- */

static void sound(unsigned hz)
{
    unsigned d;
    if (hz < 20) hz = 20;
    d = (unsigned)(1193182UL / hz);
    outp(0x43, 0xB6);
    outp(0x42, d & 255);
    outp(0x42, d >> 8);
    outp(0x61, inp(0x61) | 3);
}

static void nosound(void)
{
    outp(0x61, inp(0x61) & ~3);
}

static void tone(unsigned hz, unsigned ms)
{
    if (!sound_on) return;
    if (hz) sound(hz);
    delay_ms(ms);
    nosound();
}

static void quiet(unsigned ms) { delay_ms(ms); }

static void sweep(unsigned from, unsigned to, unsigned ms)
{
    unsigned t;
    for (t = 0; t < ms; t += 3)
        tone((unsigned)(from + ((long)to - from) * t / ms), 3);
}

static void snd_boot(void)
{
    static unsigned notes[] = { 392, 523, 659, 784, 1047, 1319 };
    int i;
    for (i = 0; i < 6; i++) tone(notes[i], 55);
    tone(1568, 140);
}

static void snd_ready(void)  { tone(1319, 40); tone(1760, 70); }
static void snd_click(void)  { tone(1800, 2); }
static void snd_error(void)  { tone(140, 120); }
static void snd_bye(void)    { sweep(1600, 200, 360); }

/* robot voice: each letter a little sound with the rhythm of speech */
static void buzz(unsigned f1, unsigned f2, unsigned ms, unsigned shift)
{
    unsigned t;
    f1 = (unsigned)((long)f1 * shift / 100);
    f2 = (unsigned)((long)f2 * shift / 100);
    if (!sound_on) { delay_ms(ms); return; }
    for (t = 0; t < ms; t += 4) {
        sound(f1); delay_ms(2);
        sound(f2); delay_ms(2);
    }
    nosound();
}

static void hiss(unsigned ms)
{
    unsigned t;
    if (!sound_on) { delay_ms(ms); return; }
    for (t = 0; t < ms; t++) {
        sound(2500 + rand() % 4500);
        delay_ms(1);
    }
    nosound();
}

static void say_letter(int c, unsigned shift)
{
    if (c >= 'A' && c <= 'Z') c += 32;
    switch (c) {
    case 'a': buzz(730, 1090, 64, shift); break;
    case 'e': buzz(530, 1840, 56, shift); break;
    case 'i': case 'y': buzz(300, 2290, 52, shift); break;
    case 'o': buzz(570, 840, 64, shift); break;
    case 'u': buzz(320, 870, 56, shift); break;
    case 's': case 'z': case 'c': case 'x': hiss(46); break;
    case 'f': case 'h': case 'v': case 'j': hiss(30); break;
    case 'p': case 't': case 'k': case 'q':
        tone(3200, 2); quiet(18); break;
    case 'b': case 'd': case 'g':
        buzz(180, 260, 12, shift); quiet(10); break;
    case 'm': case 'n': buzz(210, 300, 40, shift); break;
    case 'l': case 'r': case 'w': buzz(400, 1100, 36, shift); break;
    case ' ': quiet(45); break;
    case ',': case '-': quiet(110); break;
    case '.': case '!': case '?': case ':': quiet(200); break;
    default:
        if (c >= '0' && c <= '9')
            buzz(420 + (c - '0') * 60, 900 + (c - '0') * 90, 70, shift);
        else
            quiet(20);
    }
}

/* ---- drawing helpers ---- */

static void hline(int x, int y, int n, int ch)
{
    gotoxy(x, y);
    while (n-- > 0) putch(ch);
}

static void put_at(int x, int y, int attr, const char *s)
{
    gotoxy(x, y);
    color(attr);
    cputs(s);
}

static void box(int x1, int y1, int x2, int y2, int attr, const char *title)
{
    int y;
    color(attr);
    gotoxy(x1, y1); putch(201); hline(x1 + 1, y1, x2 - x1 - 1, 205); putch(187);
    for (y = y1 + 1; y < y2; y++) {
        gotoxy(x1, y); putch(186);
        gotoxy(x2, y); putch(186);
    }
    gotoxy(x1, y2); putch(200); hline(x1 + 1, y2, x2 - x1 - 1, 205);
    cell(x2, y2, 188, S->attr);
    if (title) {
        put_at(x1 + 2, y1, attr, "\xB5");
        put_at(x1 + 3, y1, C_PANEL, title);
        color(attr);
        putch(198);
    }
}

/* a bar of w cells, v from 0 to 1000 */
static void meter(int x, int y, int w, int v, int attr)
{
    int i, n = (int)((long)v * w * 2 / 1000);
    gotoxy(x, y);
    for (i = 0; i < w; i++) {
        if (n >= 2 * i + 2) { color(attr); putch(219); }
        else if (n == 2 * i + 1) { color(attr); putch(221); }
        else { color(C_DIM); putch(250); }
    }
}

static void fail(const char *msg)
{
    nosound();
    use(0);
    full();
    textattr(7);
    clrscr();
    cursor(1);
    printf("\nTINY88: %s\n", msg);
    exit(1);
}

/* ---- status and title ---- */

static char status_right[16];

static void status_bar(const char *right)
{
    char buf[20];
    use(0);
    full();
    color(C_BAR);
    gotoxy(1, 25);
    cputs(" ");
    color(C_BARKEY); cputs("F1"); color(C_BAR); cputs(" Help  ");
    color(C_BARKEY); cputs("F2"); color(C_BAR); cputs(" Sound ");
    color(C_BARON); cputs(sound_on ? "ON " : "OFF"); color(C_BAR); cputs("  ");
    color(C_BARKEY); cputs("F3"); color(C_BAR); cputs(" Voice ");
    color(C_BARON); cputs(voice_on ? "ON " : "OFF"); color(C_BAR); cputs("  ");
    color(C_BARKEY); cputs("F4"); color(C_BAR); cputs(" Creativity ");
    color(C_BARON); putch('0' + creativity); color(C_BAR); cputs("  ");
    color(C_BARKEY); cputs("Esc"); color(C_BAR); cputs(" Quit ");
    if (right) strncpy(status_right, right, 15);
    status_right[15] = 0;
    sprintf(buf, "%-15.15s", status_right);
    cputs(buf);
}

static void clock_text(char *buf)
{
    union REGS r;
    r.h.ah = 0x2C;
    int86(0x21, &r, &r);
    sprintf(buf, "%02d:%02d", r.h.ch, r.h.cl);
}

static void title_bar(void)
{
    char buf[12];
    use(0);
    full();
    color(C_TITLE);
    hline(1, 1, SCRW, ' ');
    put_at(2, 1, C_TITLE, "\x11\x10");
    put_at(5, 1, C_TITLE2, "T I N Y");
    put_at(13, 1, C_TITLE, "\xFA  NEURAL INTERFACE 2.1/88  \xFA  8088 EDITION");
    clock_text(buf);
    put_at(62, 1, 0x1A, "\xFE");
    put_at(64, 1, C_TITLE, "ONLINE");
    put_at(73, 1, C_TITLE2, buf);
}

/* ---- the neural monitor ---- */

static long act_max[MAXL];
static int heat[18], heat_n;
static long speed100;                 /* letters per 100 seconds */
static unsigned long step_t0, think_t0;
static int cur_layer = -1, cur_stage, steps_done, step_total;
static const char *phase_text = "idle";
static char ctx_text[CTX + 1];        /* the letters the network has seen */
static int ctx_n;

static const char *stage_name[3] = { "keys+values", "attention", "thinking" };

static void layer_meters(int x, int y, int w, int rows)
{
    int l, v;
    for (l = 0; l < L && l < rows; l++) {
        if (layer_act[l] > act_max[l]) act_max[l] = layer_act[l];
        v = act_max[l] > 0 ? (int)(1000L * (layer_act[l] / 16) / (act_max[l] / 16 + 1)) : 0;
        meter(x, y + l, w, v, l % 2 ? C_FRAME : C_ANSWER);
        act_max[l] -= act_max[l] / 200;
    }
}

static void conf_meter(int x, int y, int w)
{
    char buf[8];
    int v = last_conf;
    meter(x, y, w, v, v > 800 ? C_ANSWER : v > 500 ? C_QTEXT : C_HOT);
    sprintf(buf, " %3d%%", v / 10);
    put_at(x + w, y, C_BRIGHT, buf);
}

static void heat_row(int x, int y)
{
    int i, v;
    gotoxy(x, y);
    for (i = 0; i < 18; i++) {
        if (i >= heat_n) { color(C_DIM); putch(250); continue; }
        v = heat[i];
        color(v > 800 ? C_ANSWER : v > 500 ? C_QTEXT : C_HOT);
        putch(v > 900 ? 219 : v > 700 ? 178 : v > 450 ? 177 : 176);
    }
}

static void monitor_static(void)
{
    int l;
    char buf[8];
    use(0);
    full();
    put_at(MON_X, 3, C_DIM, "LAYER ACTIVITY");
    for (l = 0; l < L && l < 6; l++) {
        sprintf(buf, "L%d", l + 1);
        put_at(MON_X, 4 + l, C_FRAME, buf);
        meter(MON_X + 3, 4 + l, 15, 0, C_ANSWER);
    }
    put_at(MON_X, 10, C_DIM, "CONFIDENCE");
    meter(MON_X, 11, 13, 0, C_ANSWER);
    put_at(MON_X, 12, C_DIM, "LAST LETTERS");
    put_at(MON_X, 14, C_DIM, "SPEED");
    put_at(MON_X, 16, C_DIM, "NOW");
    put_at(MON_X, 19, C_DIM, "MODE");
}

static void big_monitor_static(void);
static void big_monitor_live(void);

static void monitor_live(void)
{
    char buf[24];
    int l;
    use(0);
    full();
    layer_meters(MON_X + 3, 4, 15, 6);
    for (l = 0; l < L && l < 6; l++)
        put_at(MON_X + 2, 4 + l, l == cur_layer ? C_HOT : C_DIM, l == cur_layer ? "\x10" : " ");
    conf_meter(MON_X, 11, 13);
    heat_row(MON_X, 13);
    sprintf(buf, "%ld.%02ld letters/s  ", speed100 / 100, speed100 % 100);
    put_at(MON_X, 15, C_TEXT, buf);
    sprintf(buf, "%-18.18s", phase_text);
    put_at(MON_X, 17, C_TEXT, buf);
    if (cur_layer >= 0 && cur_layer < L)
        sprintf(buf, "L%d %-15.15s", cur_layer + 1, stage_name[cur_stage]);
    else
        sprintf(buf, "%-18.18s", cur_layer == L ? "choosing a letter" : "");
    put_at(MON_X, 18, C_DIM, buf);
    sprintf(buf, "creativity %d  %s", creativity, recalled ? "mem" : "   ");
    put_at(MON_X, 20, C_TEXT, buf);
    if (dual) big_monitor_live();
    use(0);
}

/* ---- the second screen (with /DUAL): the whole network, live ---- */

#define BM_ATT_Y 19
#define BM_ATT_W 72

static void big_monitor_static(void)
{
    int l;
    char buf[40];
    use(1);
    full();
    color(C_TEXT);
    clrscr();
    color(C_TITLE);
    hline(1, 1, SCRW, ' ');
    put_at(3, 1, C_TITLE2, "T I N Y   N E U R A L   M O N I T O R");
    sprintf(buf, "%d layers \xFA %d wide \xFA %d heads", L, D, H);
    put_at(52, 1, C_TITLE, buf);
    put_at(2, 3, C_DIM, "NOW");
    put_at(2, 5, C_DIM, "LAYER ACTIVITY");
    for (l = 0; l < L && l < 4; l++) {
        sprintf(buf, "LAYER %d", l + 1);
        put_at(4, 6 + l, C_FRAME, buf);
    }
    put_at(2, 11, C_DIM, "CONFIDENCE");
    put_at(44, 11, C_DIM, "LAST LETTERS");
    put_at(2, 13, C_DIM, "NEXT LETTER, BEST GUESSES");
    put_at(44, 13, C_DIM, "TIMING");
    put_at(2, 18, C_DIM, "ATTENTION: WHERE EACH LAYER LOOKED FOR THE LAST LETTER");
    color(C_FRAME2);
    hline(1, 24, SCRW, 196);
    put_at(2, 25, C_DIM, "TINY 2.1/88 \xFA integer engine for the 8088 \xFA each letter = 367,000 weights");
    use(0);
}

static void big_monitor_live(void)
{
    int l, i, k, n0, tok[4], pm[4], v;
    char buf[64];
    unsigned long now = ticks();
    use(1);
    full();
    sprintf(buf, "%-30.30s", phase_text);
    put_at(7, 3, C_BRIGHT, buf);
    if (cur_layer >= 0 && cur_layer < L)
        sprintf(buf, "layer %d of %d: %-12s", cur_layer + 1, L, stage_name[cur_stage]);
    else
        sprintf(buf, "%-27.27s", cur_layer == L ? "choosing the next letter" : "");
    put_at(40, 3, C_QTEXT, buf);
    layer_meters(13, 6, 50, 4);
    for (l = 0; l < L && l < 4; l++) {
        put_at(65, 6 + l, l == cur_layer ? C_HOT : C_DIM,
               l == cur_layer ? (cur_stage == 0 ? "\x11 keys     " : cur_stage == 1 ? "\x11 attention" : "\x11 thinking ") : "           ");
    }
    conf_meter(13, 11, 24);
    heat_row(57, 11);
    if (cur_layer == L || cur_layer < 0) {
        engine_top(4, tok, pm);
        for (k = 0; k < 4; k++) {
            char lt[4];
            if (tok[k] <= 1) strcpy(lt, "END");
            else { lt[0] = '\''; lt[1] = (char)(tok[k] + 30); lt[2] = '\''; lt[3] = 0; }
            put_at(4, 14 + k, C_BRIGHT, lt);
            meter(9, 14 + k, 24, pm[k], k == 0 ? C_ANSWER : C_FRAME2);
            sprintf(buf, " %3d.%d%%", pm[k] / 10, pm[k] % 10);
            put_at(33, 14 + k, C_TEXT, buf);
        }
    }
    v = (int)((now - step_t0) * 10 / 18);
    sprintf(buf, "this letter %3d.%ds   ", v / 10, v % 10);
    put_at(46, 14, C_TEXT, buf);
    v = (int)((now - think_t0) * 10 / 18);
    sprintf(buf, "this answer %3d.%ds   ", v / 10, v % 10);
    put_at(46, 15, C_TEXT, buf);
    sprintf(buf, "letters done %3d      ", steps_done);
    put_at(46, 16, C_TEXT, buf);
    sprintf(buf, "from memory %3d       ", recalled);
    put_at(46, 17, C_TEXT, buf);
    /* attention: the text, and under it how much each layer looked at
       each letter for the letter being worked on now */
    n0 = ctx_n > BM_ATT_W ? ctx_n - BM_ATT_W : 0;
    gotoxy(6, BM_ATT_Y);
    color(C_QTEXT);
    for (i = 0; i < BM_ATT_W; i++)
        putch(n0 + i < ctx_n ? (ctx_text[n0 + i] == '\n' ? 0x19 : ctx_text[n0 + i]) : ' ');
    for (l = 0; l < L && l < 4; l++) {
        sprintf(buf, "L%d", l + 1);
        put_at(2, BM_ATT_Y + 1 + l, C_FRAME, buf);
        gotoxy(6, BM_ATT_Y + 1 + l);
        for (i = 0; i < BM_ATT_W; i++) {
            int a = n0 + i < ctx_n ? att_map[l][n0 + i] : 0;
            color(a > 150 ? C_BRIGHT : a > 60 ? C_ANSWER : a > 20 ? C_FRAME2 : C_DIM);
            putch(n0 + i >= ctx_n ? ' ' : a > 150 ? 219 : a > 60 ? 178 : a > 20 ? 177 : a > 5 ? 176 : 250);
        }
    }
    use(0);
}

/* called by the engine at each stage of each layer */
static void ui_progress(int layer, int stage)
{
    cur_layer = layer;
    cur_stage = stage;
    if (kbhit()) {
        int c = getch();
        if (c == 0) getch();
        else if (c == 27) abort_think = 1;
    }
    monitor_live();
}

/* ---- frame ---- */

static void frame(void)
{
    use(0);
    full();
    color(C_TEXT);
    clrscr();
    title_bar();
    box(1, 2, 58, 21, C_FRAME, " CONVERSATION ");
    box(59, 2, 80, 21, C_FRAME2, " NEURAL MONITOR ");
    box(1, 22, 80, 24, C_FRAME2, " ASK TINY ");
    put_at(3, IN_Y, C_YOU, "\x10");
    put_at(59, 22, C_FRAME2, "\xD1");
    put_at(59, 24, C_FRAME2, "\xCF");
    put_at(59, IN_Y, C_FRAME2, "\xB3");
    put_at(62, IN_Y, C_DIM, "\x18 last  F1 help");
    monitor_static();
    monitor_live();
    status_bar(NULL);
}

/* ---- chat window ---- */

static int chat_x = 1, chat_y = 1;

static void chat_win(void)
{
    use(0);
    window(CHAT_L, CHAT_T, CHAT_R, CHAT_B);
    gotoxy(chat_x, chat_y);
}

static void chat_save(void)
{
    chat_x = wherex();
    chat_y = wherey();
    full();
}

static void chat_nl(void) { cputs("\r\n"); }

static void chat_room(int lines)
{
    int h = CHAT_B - CHAT_T + 1, y = wherey(), x = wherex();
    while (y > h - lines + 1 && y > 1) {
        gotoxy(1, h);
        cputs("\n");
        y--;
    }
    gotoxy(x, y);
}

static void chat_text(int attr, const char *s, int indent)
{
    int i;
    color(attr);
    for (i = 0; s[i]; i++) {
        if (wherex() >= CHATW) {
            chat_nl();
            gotoxy(indent, wherey());
        }
        putch(s[i]);
    }
}

/* ---- boot screen ---- */

static const char *logo[5] = {
    "#####  ###  #   #  #   #",
    "  #     #   ##  #   # # ",
    "  #     #   # # #    #  ",
    "  #     #   #  ##    #  ",
    "  #    ###  #   #    #  "
};
static int logo_col[5] = { 11, 11, 9, 13, 13 };
static int boot_y;
#define LOGO_X 16
#define LOGO_Y 2

static int wait_or_key(unsigned ms)
{
    delay_ms(ms);
    if (kbhit()) { if (getch() == 0) getch(); return 1; }
    return 0;
}

static void rain(void)
{
    int col[SCRW], spd[SCRW], f, x, y;
    for (x = 0; x < SCRW; x++) { col[x] = -(rand() % 25); spd[x] = 1 + rand() % 2; }
    for (f = 0; f < 40; f++) {
        for (x = 0; x < SCRW - 1; x += 2) {
            y = col[x];
            if (y >= 1 && y <= 25) cell(x + 1, y, 33 + rand() % 90, S->mono ? 7 : f < 34 ? C_ANSWER : C_DIM);
            if (y + 1 >= 1 && y + 1 <= 25) cell(x + 1, y + 1, 33 + rand() % 90, 15);
            if (y - 7 >= 1 && y - 7 <= 25) cell(x + 1, y - 7, ' ', 7);
            col[x] += spd[x];
            if (col[x] > 32) col[x] = -(rand() % 10);
        }
        if (f % 4 == 0) tone(3000 + rand() % 3000, 2);
        if (kbhit()) { if (getch() == 0) getch(); break; }
    }
    color(C_TEXT);
    clrscr();
}

static void logo_in(void)
{
    int x, y;
    for (y = 0; y < 5; y++) {
        for (x = 0; x < 24; x++)
            if (logo[y][x] == '#') {
                if (!(y < 4 && x < 23 && logo[y + 1][x + 1] == '#')) {
                    gotoxy(LOGO_X + 2 * x + 2, LOGO_Y + y + 1);
                    color(1);
                    putch(177); putch(177);
                }
                gotoxy(LOGO_X + 2 * x, LOGO_Y + y);
                color(logo_col[y]);
                putch(219); putch(219);
            }
        tone(300 + y * 150, 15);
    }
}

static void boot_bar(int done, int total)
{
    int i, n = (int)(36L * done / total);
    char buf[24];
    use(0);
    full();
    gotoxy(22, 15);
    color(C_FRAME);
    putch(222);
    for (i = 0; i < 36; i++) {
        color(i < n ? (i * 3 < n ? C_FRAME : C_ANSWER) : C_DIM);
        putch(i < n ? 219 : 250);
    }
    color(C_FRAME);
    putch(221);
    sprintf(buf, " %3d%%", (int)(100L * done / total));
    color(C_BRIGHT);
    cputs(buf);
}

static void boot(void)
{
    int x;
    char buf[60];
    use(0);
    full();
    color(C_TEXT);
    clrscr();
    cursor(0);
    if (!skip_boot) rain();
    logo_in();
    put_at(24, 8, C_BRIGHT, "N E U R A L    I N T E R F A C E");
    put_at(25, 9, C_DIM, "version 2.1/88  \xFA  for the IBM PC 8088");
    color(C_FRAME2);
    hline(10, 11, 60, 196);
    put_at(22, 13, C_FRAME, "LOADING NEURAL WEIGHTS FROM TINY88.BIN");
    on_load = boot_bar;
    load_all();
    on_load = NULL;
    boot_y = 17;
    sprintf(buf, "\x10 %d layers \xFA %d wide \xFA %d heads", L, D, H);
    put_at(26, boot_y++, C_TEXT, buf);
    put_at(26, boot_y++, C_TEXT, pfx ? "\x10 question memory TINYPFX.BIN" : "\x10 no TINYPFX.BIN: every letter is computed");
    sprintf(buf, "\x10 screen: %s%s%s", main_is_mda ? "MDA" : "CGA",
            scr[0].snow ? " (no-snow writes)" : "", dual ? (mono_mon ? " + MDA monitor" : " + CGA monitor") : "");
    put_at(26, boot_y++, C_TEXT, buf);
    snd_boot();
    for (x = 0; x < 6; x++) {
        put_at(30, boot_y + 1, x % 2 ? C_BRIGHT : C_ANSWER, "\x10 SYSTEM  READY \x11");
        delay_ms(120);
    }
}

/* ---- help ---- */

static void show_help(void)
{
    static const char *h[] = {
        "Ask a question and press Enter.",
        "Try: capital of peru \xFA is a whale a fish",
        "     what is 7 times 8 \xFA spell elephant",
        "An 8088 needs about 3-9 seconds a letter;",
        "common beginnings come from memory.",
        "Esc while thinking stops the answer.",
        "F2 sound \xFA F3 voice \xFA F4 creativity \xFA CLS",
        NULL
    };
    int i;
    chat_win();
    if (wherex() > 1) chat_nl();
    chat_room(9);
    color(C_PANEL); cputs(" HELP "); chat_nl();
    for (i = 0; h[i]; i++) {
        color(C_TEXT); cputs(" "); cputs(h[i]); chat_nl();
    }
    chat_save();
}

/* ---- asking ---- */

static int ans_x0, ans_y0, ans_col;
static int spin, scr_prev;

static void ans_pos(int i, int *sx, int *sy)
{
    int per = CHATW - ans_x0;
    *sx = CHAT_L + ans_x0 - 1 + i % per;
    *sy = CHAT_T + ans_y0 - 1 + i / per;
}

static void step_done(void)
{
    unsigned long now = ticks();
    steps_done++;
    if (now > think_t0 && steps_done)
        speed100 = (long)steps_done * 1820L / (long)(now - think_t0);
    step_t0 = now;
}

/* while TINY reads the question */
static void ui_step(int i, int n)
{
    int k, head, sx, sy, d;
    char buf[24];
    static char pt[32];
    if (i > recalled) step_done();
    ctx_n = i;
    for (k = 0; k < i; k++) ctx_text[k] = last_q[k];
    if (i == n) { ctx_text[n] = '\n'; ctx_n = n + 1; }
    use(0);
    full();
    head = spin % 18;
    if (head >= 9) head = 17 - head;
    ans_pos(0, &sx, &sy);
    gotoxy(sx, sy);
    for (k = 0; k < 10; k++) {
        d = head - k; if (d < 0) d = -d;
        color(d == 0 ? C_BRIGHT : d == 1 ? C_YOU : d == 2 ? 5 : C_DIM);
        putch(d == 0 ? 219 : d == 1 ? 178 : d == 2 ? 177 : 250);
    }
    color(C_DIM);
    sprintf(buf, " reading %d/%d ", i < n ? i + 1 : n, n);
    cputs(buf);
    spin++;
    if (i < n) sprintf(pt, "reading '%c' %d of %d", last_q[i], i + 1, n);
    else sprintf(pt, "end of question");
    phase_text = pt;
    monitor_live();
    if (i % 3 == 0) tone(2400 + (rand() % 8) * 150, 3);
}

static void ui_letter(int c)
{
    int x, y;
    static char pt[32];
    step_done();
    use(0);
    full();
    if (ans_col == 0) {
        ans_pos(0, &x, &y);
        gotoxy(x, y);
        color(C_TEXT);
        cputs("                           ");
    } else {
        ans_pos(ans_col - 1, &x, &y);
        cell(x, y, scr_prev, S->mono ? 7 : C_ANSWER);
    }
    ans_pos(ans_col, &x, &y);
    color(C_GLOW);
    cell(x, y, c, S->attr);
    scr_prev = c;
    ans_col++;
    if (ctx_n < CTX) ctx_text[ctx_n++] = (char)c;
    if (heat_n < 18)
        heat[heat_n++] = last_conf;
    else {
        int k;
        for (k = 0; k < 17; k++) heat[k] = heat[k + 1];
        heat[17] = last_conf;
    }
    sprintf(pt, "writing letter %d", ans_col + 1);
    phase_text = pt;
    cur_layer = -1;
    monitor_live();
    if (!voice_on) tone(900 + (c % 16) * 70, 6);
}

static void speak(const char *a)
{
    int i, j, k, sx, sy, n = (int)strlen(a), att;
    unsigned shift;
    use(0);
    full();
    for (i = 0; i < n; i = j) {
        for (j = i; j < n && a[j] != ' '; j++) ;
        for (; j < n && a[j] == ' '; j++) ;
        color(C_SPEAK); att = S->attr;
        for (k = i; k < j && a[k] != ' '; k++) {
            ans_pos(k, &sx, &sy);
            cell(sx, sy, a[k], att);
        }
        shift = (unsigned)(115 - 30L * i / (n + 1));
        for (k = i; k < j; k++) {
            if (kbhit()) { shift = 0; break; }
            say_letter(a[k], shift);
        }
        color(C_ANSWER); att = S->attr;
        for (k = i; k < j && a[k] != ' '; k++) {
            ans_pos(k, &sx, &sy);
            cell(sx, sy, a[k], att);
        }
        if (shift == 0) { if (getch() == 0) getch(); break; }
    }
}

static void ask(const char *q)
{
    static char out[CTX + 1];
    char info[40];
    long tenths;
    int letters, sx, sy;

    chat_win();
    if (wherex() > 1) chat_nl();
    if (wherey() > 1) chat_nl();
    chat_room(6);
    color(C_YOU); cputs(" YOU \xAF ");
    chat_text(C_QTEXT, q, 8);
    chat_nl();
    color(C_TINY); cputs("TINY \xAF ");
    ans_x0 = wherex();
    ans_y0 = wherey();
    ans_col = 0;
    heat_n = 0;
    chat_save();
    cursor(0);
    status_bar("thinking...");
    strcpy(last_q, q);
    ctx_n = 0;
    steps_done = 0;
    think_t0 = step_t0 = ticks();
    letters = think(q, out, ui_step, ui_letter);
    tenths = (long)((ticks() - think_t0) * 10L * 10L / 182L);
    if (abort_think) {
        if (ans_col == 0) { ans_pos(0, &sx, &sy); gotoxy(sx, sy); color(C_TEXT); cputs("                           "); }
        ans_pos(ans_col, &sx, &sy);
        gotoxy(sx, sy); color(C_HOT); cputs(ans_col ? " [stopped]" : "[stopped]");
        letters = ans_col;
        ans_col += ans_col ? 10 : 9;
    } else if (letters == 0) { ui_letter('.'); out[0] = '.'; out[1] = 0; letters = 1; }
    if (ans_col > 0 && !abort_think) {
        ans_pos(ans_col - 1, &sx, &sy);
        cell(sx, sy, scr_prev, scr[0].mono ? 7 : C_ANSWER);
    }
    ans_pos(ans_col > 0 ? ans_col - 1 : 0, &sx, &sy);
    phase_text = "waiting for you";
    cur_layer = -2;
    sprintf(info, "%d ltr %ld.%lds", letters, tenths / 10, tenths % 10);
    status_bar(info);
    monitor_live();
    if (!abort_think) {
        if (sound_on && voice_on) {
            tone(1200, 8);
            speak(out);
        } else
            snd_ready();
    }
    chat_x = 1;
    chat_y = sy - CHAT_T + 2;
    chat_win();
    chat_save();
    title_bar();
    cursor(1);
}

/* ---- typing ---- */

#define K_ESC   27
#define K_ENTER 13
#define K_BS    8
#define K_F1    (256 + 59)
#define K_F2    (256 + 60)
#define K_F3    (256 + 61)
#define K_F4    (256 + 62)
#define K_UP    (256 + 72)

static int key(void)
{
    int c = getch();
    if (c == 0) c = 256 + getch();
    return c;
}

static void draw_input(const char *buf, int n)
{
    int w = 54;
    use(0);
    full();
    gotoxy(5, IN_Y);
    color(C_BRIGHT);
    cputs(n > w ? buf + n - w : buf);
    color(C_TEXT);
    hline(5 + (n > w ? w : n), IN_Y, 55 - (n > w ? w : n), ' ');
    gotoxy(5 + (n > w ? w : n), IN_Y);
    place_cursor();
}

static int read_line(char *buf)
{
    int n = 0, c;
    buf[0] = 0;
    draw_input(buf, 0);
    for (;;) {
        c = key();
        if (c == K_ENTER) return n;
        if (c == K_ESC) return -1;
        if (c == K_BS) {
            if (n > 0) { buf[--n] = 0; snd_click(); }
        } else if (c == K_UP) {
            strcpy(buf, last_q);
            n = (int)strlen(buf);
        } else if (c == K_F1) {
            show_help();
        } else if (c == K_F2) {
            sound_on = !sound_on;
            snd_click();
        } else if (c == K_F3) {
            voice_on = !voice_on;
            snd_click();
        } else if (c == K_F4) {
            creativity = (creativity + 1) % 10;
            snd_click();
            monitor_live();
        } else if (c >= 32 && c <= 126) {
            if (n < MAXQ) { buf[n++] = (char)c; buf[n] = 0; snd_click(); }
            else snd_error();
        }
        status_bar(NULL);
        draw_input(buf, n);
    }
}

/* ---- demo mode ---- */

static const char *demo_q[] = {
    "what is the capital of peru", "is a whale a fish", "what is 7 times 8",
    "spell elephant", "what is the symbol for gold", "what is a group of lions called",
    "what comes after 199", "tell me a joke", NULL
};

static void demo_type(const char *q)
{
    char buf[MAXQ + 1];
    int n;
    for (n = 0; q[n]; n++) {
        buf[n] = q[n];
        buf[n + 1] = 0;
        draw_input(buf, n + 1);
        snd_click();
        delay_ms(45 + rand() % 50);
    }
    delay_ms(250);
    draw_input("", 0);
}

static void restore_screen(void)
{
    nosound();
    if (dual) {
        use(1); full(); textattr(7); clrscr();
        put_at(30, 12, 7, "TINY88 has finished.");
    }
    use(0);
    full();
    textattr(7);
    clrscr();
    gotoxy(1, 1);
    place_cursor();
    cursor(1);
}

static int ui_main(void)
{
    static char line[MAXQ + 1], q[MAXQ + 1];
    int i, n;

    screens_init();
    calibrate();
    boot();
    if (dual) big_monitor_static();
    if (!demo) {
        put_at(29, boot_y + 3, C_DIM, "press any key to begin");
        if (key() == K_ESC) { restore_screen(); return 0; }
    }
    on_progress = ui_progress;
    sweep(300, 1400, 120);
    frame();
    cursor(1);
    chat_win();
    color(C_DIM);
    cputs("Hello! I am TINY, a neural network in your IBM PC.");
    chat_nl();
    cputs("Ask about countries, animals, space, words or sums.");
    chat_nl();
    cputs("I think slowly on an 8088: about 5 seconds a letter.");
    chat_nl();
    chat_save();
    snd_ready();

    if (demo) {
        for (i = 0; demo_q[i]; i++) {
            if (kbhit()) { key(); break; }
            demo_type(demo_q[i]);
            ask(demo_q[i]);
            if (dumps && i == dumps - 1) {
                FILE *f = fopen("SCREENS.DMP", "wb");
                if (f) { fwrite(scr, 1, sizeof(scr), f); fclose(f); }
                restore_screen();
                return 0;
            }
            if (abort_think) break;
            delay_ms(1500);
        }
    }
    for (;;) {
        n = read_line(line);
        if (n < 0) break;
        draw_input("", 0);
        if (clean(line, q) == 0) continue;
        if (is_word(q, "quit") || is_word(q, "exit")) break;
        if (is_word(q, "cls")) {
            frame();
            chat_x = chat_y = 1;
            continue;
        }
        if (is_word(q, "help")) { show_help(); continue; }
        ask(q);
    }
    status_bar("goodbye");
    snd_bye();
    restore_screen();
    printf("TINY88: goodbye!\n");
    return 0;
}

int main(int argc, char **argv)
{
    int i, plain = 0;
    char *s;

    strcpy(exe_dir, argv[0]);
    s = strrchr(exe_dir, '\\');
    if (s) s[1] = 0; else exe_dir[0] = 0;
    for (i = 1; i < argc; i++) {
        if (argv[i][0] >= '0' && argv[i][0] <= '9') creativity = argv[i][0] - '0';
        else if (is_word(argv[i], "/p") || is_word(argv[i], "-p")) plain = 1;
        else if (is_word(argv[i], "/demo")) demo = 1;
        else if (is_word(argv[i], "/mono")) force_mono = 1;
        else if (is_word(argv[i], "/quiet")) sound_on = 0;
        else if (is_word(argv[i], "/dual")) dual = 1;
        else if (is_word(argv[i], "/snow")) snow_opt = 1;
        else if (is_word(argv[i], "/nosnow")) snow_opt = 0;
        else if (is_word(argv[i], "/nomem")) use_pfx = 0;
        else if (is_word(argv[i], "/fast")) skip_boot = 1;
        else if (is_word(argv[i], "/dump1")) dumps = 1;
        else if (is_word(argv[i], "/dump3")) dumps = 3;
        else if (is_word(argv[i], "/forcedual")) dual = force_dual = 1;
        else if (is_word(argv[i], "/?") || is_word(argv[i], "/h")) {
            printf("TINY88 [0-9] [/DUAL] [/MONO] [/QUIET] [/DEMO] [/P] [/NOSNOW] [/SNOW] [/NOMEM] [/FAST]\n"
                   "  0-9     creativity (0 = always the surest answer, default 5)\n"
                   "  /DUAL   use both the CGA and the MDA: chat on one, neural monitor on the other\n"
                   "  /MONO   grey colours        /QUIET  no sound      /DEMO  it asks itself\n"
                   "  /P      plain text (also when input is redirected: TINY88 < Q.TXT)\n"
                   "  /NOSNOW fast screen writes on a CGA clone that does not show snow\n"
                   "  /NOMEM  do not use TINYPFX.BIN     /FAST  skip the boot animation\n");
            return 0;
        }
    }
    engine_seed((uint32_t)ticks() ^ ((uint32_t)time(NULL) << 8));
    srand((unsigned)ticks());
    if (!plain && isatty(0) && isatty(1))
        return ui_main();
    return plain_main();
}
#endif
