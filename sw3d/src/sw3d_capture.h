/* sw3d_capture.h - SW3D: what the matrix processor and the divider were asked
 * to do, recorded as it happens, so the picture's points can be given back
 * the depth the perspective divide took away.  Called from hw.cpp's math
 * registers ($4700-$4707); nothing in the game reads it.
 */
#ifndef SW3D_CAPTURE_H
#define SW3D_CAPTURE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t tag;               /* sw3d_set_tag() value when it ran */
    uint8_t  prog;              /* M.PCGO: the microcode program started */
    uint16_t bic;               /* block index counter when it started */
    int16_t  divisor, dividend, quotient;   /* the divider, as last used */
    int16_t  pre[6], post[6];   /* M_XP,YP,ZP, M_XT,YT,ZT before and after */
    int16_t  blk_pre[4], blk_post[4];       /* the BIC block's four words */
} sw3d_event;

void sw3d_math_write(const uint8_t *mathram, int reg, uint8_t data, int before,
                     uint16_t bic, uint16_t divisor, uint16_t dividend, uint16_t quotient);
int  sw3d_events(const sw3d_event **ev);    /* the events since the last sw3d_clear() */
void sw3d_clear(void);
void sw3d_set_tag(uint32_t tag);          /* marks the events that follow */
void sw3d_drop_before(uint32_t tag);      /* forget the events with a tag below this */


/* ---- depth for the picture -------------------------------------------------
 * Every perspective step the program asks for - MS_PERS ($86: 1/X times Y and
 * Z) and MS_PRS2 ($AE, the ground objects) - leaves a screen point, and the
 * divider still holds the X it was divided by.  Those are kept here in the
 * vector generator's coordinates (sw_platform.h), and a picture's line ends
 * are looked up among them.
 *
 *   picture x = 125 + (255/1024) * screen Y      (measured: exact to 0.001)
 *   picture y = 165.8984 - (255/1024) * screen Z
 *   screen Y, Z = 512 * Y / X, 512 * Z / X       (X is depth, Y right, Z up)
 */
#define SW3D_KX     (255.0f / 1024.0f)
#define SW3D_CX     125.0f
#define SW3D_CY     165.8984f
#define SW3D_FOCAL  512.0f

/* depth (X, > 0) of the perspective point at picture (x, y), from this
 * picture's game frame or the few before it; 0 if no point is there */
float sw3d_depth_at(float x, float y);
void  sw3d_set_debug(int on);               /* keep sw3d_event records too */

#ifdef __cplusplus
}
#endif
#endif
