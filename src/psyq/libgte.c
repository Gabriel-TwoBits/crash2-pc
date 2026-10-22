/* libgte on the host's GTE registers (src/host.h), the GTE's computations by decomp's model (include/gte.h), the one
 * the game's C runs on: the matrices and vectors from libgte's structures, the registers from psx-spx's "GTE
 * Registers", InitGeom's defaults those the retail executable writes (decomp's split of libgte). Compiled as the game's
 * C is, for gte.h */
#define GTE_MVMVA
#include <math.h>

#include "common.h"
#include "gte.h"

typedef struct {
    s16 m[3][3];
    s32 t[3];
} Matrix;

typedef struct {
    s32 vx, vy, vz, pad;
} Vector;

typedef struct {
    s16 vx, vy, vz, pad;
} SVector;

/* The light colour matrix's five words, after the background colour */
#define GTE_LR1LR2 16
#define GTE_LR3LG1 17
#define GTE_LG2LG3 18
#define GTE_LB1LB2 19
#define GTE_LB3 20
#define GTE_ZSF3 29
#define GTE_ZSF4 30
/* InitGeom's values */
#define DEFAULT_ZSF3 0x155
#define DEFAULT_ZSF4 0x100
#define DEFAULT_H 1000
#define DEFAULT_DQA (-0x1062)
#define DEFAULT_DQB 0x1400000
/* OFX and OFY hold 16 fraction bits; the far colour 4 */
#define OFFSET_SHIFT 16
#define FAR_COLOUR_SHIFT 4
#define FRACTION_SHIFT 12
#define HALF_BITS 16
#define HALF_MASK 0xFFFF
#define OTZ_SHIFT 2
#define IR_MIN (-0x8000)
#define IR_MAX 0x7FFF

static u32 pair(s32 low, s32 high) {
    return ((u32)low & HALF_MASK) | ((u32)high << HALF_BITS);
}

void InitGeom(void) {
    GTE_WRITE_CONTROL(GTE_ZSF3, DEFAULT_ZSF3);
    GTE_WRITE_CONTROL(GTE_ZSF4, DEFAULT_ZSF4);
    GTE_WRITE_CONTROL(GTE_H, DEFAULT_H);
    GTE_WRITE_CONTROL(GTE_DQA, DEFAULT_DQA);
    GTE_WRITE_CONTROL(GTE_DQB, DEFAULT_DQB);
    GTE_WRITE_CONTROL(GTE_OFX, 0);
    GTE_WRITE_CONTROL(GTE_OFY, 0);
}

void SetGeomOffset(s32 x, s32 y) {
    GTE_WRITE_CONTROL(GTE_OFX, (u32)x << OFFSET_SHIFT);
    GTE_WRITE_CONTROL(GTE_OFY, (u32)y << OFFSET_SHIFT);
}

void SetGeomScreen(s32 h) {
    GTE_WRITE_CONTROL(GTE_H, h);
}

void SetFarColor(s32 r, s32 g, s32 b) {
    GTE_WRITE_CONTROL(GTE_RFC, (u32)r << FAR_COLOUR_SHIFT);
    GTE_WRITE_CONTROL(GTE_GFC, (u32)g << FAR_COLOUR_SHIFT);
    GTE_WRITE_CONTROL(GTE_BFC, (u32)b << FAR_COLOUR_SHIFT);
}

static void set_rotation(const Matrix *m) {
    GTE_WRITE_CONTROL(GTE_RT11RT12, pair(m->m[0][0], m->m[0][1]));
    GTE_WRITE_CONTROL(GTE_RT13RT21, pair(m->m[0][2], m->m[1][0]));
    GTE_WRITE_CONTROL(GTE_RT22RT23, pair(m->m[1][1], m->m[1][2]));
    GTE_WRITE_CONTROL(GTE_RT31RT32, pair(m->m[2][0], m->m[2][1]));
    GTE_WRITE_CONTROL(GTE_RT33, (s32)m->m[2][2]);
}

Matrix *SetRotMatrix(Matrix *m) {
    set_rotation(m);
    return m;
}

Matrix *SetTransMatrix(Matrix *m) {
    GTE_WRITE_CONTROL(GTE_TRX, m->t[0]);
    GTE_WRITE_CONTROL(GTE_TRY, m->t[1]);
    GTE_WRITE_CONTROL(GTE_TRZ, m->t[2]);
    return m;
}

Matrix *SetColorMatrix(Matrix *m) {
    GTE_WRITE_CONTROL(GTE_LR1LR2, pair(m->m[0][0], m->m[0][1]));
    GTE_WRITE_CONTROL(GTE_LR3LG1, pair(m->m[0][2], m->m[1][0]));
    GTE_WRITE_CONTROL(GTE_LG2LG3, pair(m->m[1][1], m->m[1][2]));
    GTE_WRITE_CONTROL(GTE_LB1LB2, pair(m->m[2][0], m->m[2][1]));
    GTE_WRITE_CONTROL(GTE_LB3, (s32)m->m[2][2]);
    return m;
}

/* m0 = m0 x m1, each column of m1 through MVMVA (the rotation matrix then holds m0, as libgte leaves it) */
Matrix *MulMatrix(Matrix *m0, Matrix *m1) {
    s16 result[3][3];
    s32 column;
    u32 ir;

    set_rotation(m0);
    for (column = 0; column < GTE_DIMENSION; column++) {
        GTE_WRITE_DATA(GTE_VXY0, pair(m1->m[0][column], m1->m[1][column]));
        GTE_WRITE_DATA(GTE_VZ0, (s32)m1->m[2][column]);
        Gte_MultiplyMatrix(GTE_MATRIX_ROTATION, 0);
        GTE_READ_DATA(GTE_IR1, ir);
        result[0][column] = (s16)ir;
        GTE_READ_DATA(GTE_IR2, ir);
        result[1][column] = (s16)ir;
        GTE_READ_DATA(GTE_IR3, ir);
        result[2][column] = (s16)ir;
    }
    for (column = 0; column < GTE_DIMENSION * GTE_DIMENSION; column++) {
        m0->m[column / GTE_DIMENSION][column % GTE_DIMENSION] = result[column / GTE_DIMENSION][column % GTE_DIMENSION];
    }
    return m0;
}

/* Each column j of the matrix scaled by the vector's j-th (4096 being 1) */
Matrix *ScaleMatrix(Matrix *m, Vector *v) {
    s32 scale[GTE_DIMENSION];
    s32 row, column;

    scale[0] = v->vx;
    scale[1] = v->vy;
    scale[2] = v->vz;
    for (row = 0; row < GTE_DIMENSION; row++) {
        for (column = 0; column < GTE_DIMENSION; column++) {
            m->m[row][column] = (s16)((m->m[row][column] * scale[column]) >> FRACTION_SHIFT);
        }
    }
    return m;
}

Matrix *TransMatrix(Matrix *m, Vector *v) {
    m->t[0] = v->vx;
    m->t[1] = v->vy;
    m->t[2] = v->vz;
    return m;
}

static s32 saturated(s32 value) {
    if (value < IR_MIN) {
        return IR_MIN;
    } else if (value > IR_MAX) {
        return IR_MAX;
    } else {
        return value;
    }
}

/* v1 = the rotation matrix x v0 + the translation, as IR1-3 (saturated), and the flag in *flag.
 * TRADEOFF: the flag holds 0, without the bits MVMVA would set on an overflow */
Vector *RotTrans(SVector *v0, Vector *v1, s32 *flag) {
    u32 words[GTE_DIMENSION + 2];
    s32 vector[GTE_DIMENSION];
    s32 row, column;

    GTE_READ_CONTROL(GTE_RT11RT12, words[0]);
    GTE_READ_CONTROL(GTE_RT13RT21, words[1]);
    GTE_READ_CONTROL(GTE_RT22RT23, words[2]);
    GTE_READ_CONTROL(GTE_RT31RT32, words[3]);
    GTE_READ_CONTROL(GTE_RT33, words[4]);
    vector[0] = v0->vx;
    vector[1] = v0->vy;
    vector[2] = v0->vz;
    for (row = 0; row < GTE_DIMENSION; row++) {
        s64 sum;
        u32 translation;

        GTE_READ_CONTROL(GTE_TRX + row, translation);
        sum = (s64)(s32)translation << FRACTION_SHIFT;
        for (column = 0; column < GTE_DIMENSION; column++) {
            s32 index = row * GTE_DIMENSION + column;
            u32 word = words[index / 2];
            s32 element = (index % 2) ? (s16)(word >> HALF_BITS) : (s16)word;

            sum += (s64)element * vector[column];
        }
        (&v1->vx)[row] = saturated((s32)(sum >> FRACTION_SHIFT));
    }
    *flag = 0;
    return v1;
}

/* RTPS on v0: the screen point in *sxy, the depth cue in *p, the flag in *flag; returns SZ3 / 4 */
s32 RotTransPers(SVector *v0, s32 *sxy, s32 *p, s32 *flag) {
    u32 value;

    GTE_WRITE_DATA(GTE_VXY0, pair(v0->vx, v0->vy));
    GTE_WRITE_DATA(GTE_VZ0, (s32)v0->vz);
    Gte_Rtps();
    GTE_READ_DATA(GTE_SXY2, value);
    *sxy = (s32)value;
    GTE_READ_DATA(GTE_IR0, value);
    *p = (s32)value;
    GTE_READ_CONTROL(GTE_FLAG, value);
    *flag = (s32)value;
    GTE_READ_DATA(GTE_SZ3, value);
    return (s32)(value >> OTZ_SHIFT);
}

void ReadSZfifo3(s32 *sz0, s32 *sz1, s32 *sz2) {
    u32 value;

    GTE_READ_DATA(GTE_SZ1, value);
    *sz0 = (s32)value;
    GTE_READ_DATA(GTE_SZ2, value);
    *sz1 = (s32)value;
    GTE_READ_DATA(GTE_SZ3, value);
    *sz2 = (s32)value;
}

/* An angle of 4096 a turn, -2048 to 2048, from y and x.
 * TRADEOFF: from the C library's atan2, rounded; libgte's own table may differ by a unit at places */
#define ANGLE_HALF_TURN 2048.0
#define PI 3.14159265358979323846

s32 ratan2(s32 y, s32 x) {
    return (s32)lround(atan2((double)y, (double)x) * ANGLE_HALF_TURN / PI);
}
