/* sw3d_capture.c - see sw3d_capture.h */
#include <string.h>
#include "sw3d_capture.h"

#define MAX_EV 8192
static sw3d_event ev[MAX_EV];
static int n_ev;
static uint32_t cur_tag;

static int debug;

/* the perspective points, newest last */
typedef struct { uint32_t tag; float x, y, depth; } point;
#define MAX_PT 16384
static point pt[MAX_PT];
static int n_pt;

static void add_point(const sw3d_event *e)
{
    int sy, sz;
    point *p;
    if (e->prog == 0x86)      { sy = e->post[1];     sz = e->post[2]; }      /* MS_PERS: M_YP, M_ZP */
    else if (e->prog == 0xAE) { sy = e->blk_post[2]; sz = e->blk_post[3]; }  /* MS_PRS2: the block's last two words */
    else return;
    if (e->divisor <= 0) return;
    if (n_pt >= MAX_PT) { memmove(pt, pt + MAX_PT / 2, (MAX_PT / 2) * sizeof pt[0]); n_pt = MAX_PT / 2; }
    p = &pt[n_pt++];
    p->tag = e->tag;
    p->x = SW3D_CX + SW3D_KX * (float)sy;
    p->y = SW3D_CY - SW3D_KX * (float)sz;
    p->depth = (float)e->divisor;
}

float sw3d_depth_at(float x, float y)
{
    float best = 0.3f, depth = 0.0f;
    int i;
    for (i = n_pt - 1; i >= 0; i--) {
        float dx = pt[i].x - x, dy = pt[i].y - y;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if (dy > dx) dx = dy;
        if (dx < best) { best = dx; depth = pt[i].depth; }
    }
    return depth;
}

void sw3d_set_debug(int on) { debug = on; }

static int16_t w(const uint8_t *r, int off) { return (int16_t)((r[off] << 8) | r[off + 1]); }

void sw3d_math_write(const uint8_t *r, int reg, uint8_t data, int before,
                     uint16_t bic, uint16_t divisor, uint16_t dividend, uint16_t quotient)
{
    static const int cell[6] = { 0x00, 0x02, 0x04, 0x18, 0x1A, 0x1C };
    sw3d_event *e;
    int i;
    if ((reg & 7) != 0) return;
    e = &ev[n_ev];
    if (before) {
        e->tag = cur_tag;
        e->prog = data;
        e->bic = bic;
        e->divisor = (int16_t)divisor;
        e->dividend = (int16_t)dividend;
        e->quotient = (int16_t)quotient;
        for (i = 0; i < 6; i++) e->pre[i] = w(r, cell[i]);
        for (i = 0; i < 4; i++) e->blk_pre[i] = w(r, ((bic & 0x1ff) << 3) + i * 2);
    } else {
        for (i = 0; i < 6; i++) e->post[i] = w(r, cell[i]);
        for (i = 0; i < 4; i++) e->blk_post[i] = w(r, ((e->bic & 0x1ff) << 3) + i * 2);
        add_point(e);
        if (debug && n_ev < MAX_EV - 1) n_ev++;
    }
}

int sw3d_events(const sw3d_event **out) { *out = ev; return n_ev; }
void sw3d_clear(void) { n_ev = 0; }

void sw3d_set_tag(uint32_t tag) { cur_tag = tag; }

void sw3d_drop_before(uint32_t tag)
{
    int i = 0, n;
    while (i < n_ev && ev[i].tag < tag) i++;
    n = n_ev - i;
    if (i) memmove(ev, ev + i, (size_t)n * sizeof ev[0]);
    n_ev = n;
    i = 0;
    while (i < n_pt && pt[i].tag < tag) i++;
    n = n_pt - i;
    if (i) memmove(pt, pt + i, (size_t)n * sizeof pt[0]);
    n_pt = n;
}
