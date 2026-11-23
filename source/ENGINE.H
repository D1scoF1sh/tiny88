/* ENGINE.H - the Tiny-2.1 network in integer arithmetic (see ENGINE.C) */
#ifndef ENGINE_H
#define ENGINE_H

#ifdef __WATCOMC__
#include <stdint.h>
#define FAR __far
#else
#include <stdint.h>
#define FAR
#define _fmemcpy memcpy
#define __near
#endif

#define VOCAB  97      /* 0 pad, 1 newline, 2.. the letters ' '..'~' */
#define CTX    128     /* longest question + answer, in letters      */
#define MAXD   128
#define MAXHID 256
#define MAXL   8
#define FX     12      /* the residual stream is value * 4096 */

typedef struct { uint16_t m; int16_t e; } SF;   /* m * 2^e */

extern int D, L, H, HD, HID;
extern int32_t score[VOCAB];
extern int32_t layer_act[MAXL];
extern uint8_t att_map[MAXL][CTX];
extern int creativity, last_conf;
extern const char *engine_error;
extern void (*on_progress)(int layer, int stage);
extern void (*on_load)(int done, int total);

int engine_load(const char *dir);
void engine_step(int tok, int p, int need_out);
int engine_pick(void);
int engine_top(int n, int *tok, int *pm);
void engine_seed(uint32_t s);
unsigned engine_kv_size(void);
void engine_kv_get(int p, uint8_t *buf);
void engine_kv_put(int p, const uint8_t *buf);

#endif
