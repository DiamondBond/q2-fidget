/* q2-fidget: a ghost buddy to poke, on the Shanling Q2's screen.
 *
 * The buddy floats over a dark backdrop and reacts to everything: a tap squishes it and makes it
 * giggle (on its face, a boop), a hold pets it (hearts), a drag picks it up and flings it, a burst
 * of taps annoys it (tongue out), a fast spin of the wheel makes it dizzy, Centre makes it say
 * boo, and left alone it dozes off. Return opens
 * the menu: Continue, its colour (blue or pink, kept) and Quit. Play/Pause and the side buttons
 * stay the music's: Q2 Pod never sends them.
 *
 * One file, no libraries: raw MIPS o32 syscalls, a software renderer on /dev/fb0 (320 x 375,
 * 32 bpp, two pages; the 375 x 320 landscape view stored turned clockwise, as Q2 Pod's video
 * player draws), and input as text datagrams on INPUT_SOCK (README.md):
 *   "t d X Y", "t m X Y", "t u X Y"  touch down, move, up, in landscape view pixels
 *   "k d CODE", "k u CODE"           a key down or up, AWTK's codes (KEY_* below)
 *   "w N"                            the wheel's raw travel, 200 a turn, rising anticlockwise
 * Built with -DHOST it renders a scripted preview to PPM frames instead (preview.sh). */

#define W 375 /* the landscape view */
#define H 320
#define INPUT_SOCK "/tmp/q2-fidget.input.sock"
#define KEY_RETURN 170
#define KEY_CENTER 218

/* ---- maths: no libm, good enough for pictures ---- */

#define PI 3.14159265f
static float fsqrt(float x) { return x > 0 ? __builtin_sqrtf(x) : 0; }
static float absf(float x) { return x < 0 ? -x : x; }
static float minf(float a, float b) { return a < b ? a : b; }
static float maxf(float a, float b) { return a > b ? a : b; }
static float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
static float sat(float x) { return clampf(x, 0, 1); }
static float mixf(float a, float b, float t) { return a + (b - a) * t; }
static float smooth(float a, float b, float x) {
    float t = sat((x - a) / (b - a));
    return t * t * (3 - 2 * t);
}
static float fsin(float x) { /* parabola, refined: under 0.1% off */
    x -= 2 * PI * (float)(int)(x / (2 * PI));
    if (x > PI) x -= 2 * PI;
    else if (x < -PI) x += 2 * PI;
    float y = 4 / PI * x - 4 / (PI * PI) * x * absf(x);
    return 0.225f * (y * absf(y) - y) + y;
}
static float fcos(float x) { return fsin(x + PI / 2); }
static float expneg(float x) { /* e^-x, x >= 0: (1 + x/16)^-16 */
    if (x <= 0) return 1;
    if (x > 24) return 0;
    float t = 1 + x / 16;
    t *= t, t *= t, t *= t, t *= t;
    return 1 / t;
}
static float fatan2(float y, float x) {
    float ax = absf(x), ay = absf(y), m = maxf(ax, ay);
    if (m == 0) return 0;
    float a = minf(ax, ay) / m, s = a * a;
    float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
    if (ay > ax) r = PI / 2 - r;
    if (x < 0) r = PI - r;
    return y < 0 ? -r : r;
}
static unsigned rng = 0x9e3779b9u;
static float frand(void) { /* 0..1 */
    rng ^= rng << 13, rng ^= rng >> 17, rng ^= rng << 5;
    return (float)(rng >> 8) * (1.0f / 16777216);
}

/* ---- shapes: signed distances in pixels, negative inside ---- */

static float len(float x, float y) { return fsqrt(x * x + y * y); }
static float smin(float a, float b, float k) {
    float h = sat(0.5f + 0.5f * (b - a) / k);
    return mixf(b, a, h) - k * h * (1 - h);
}
static float seg(float px, float py, float ax, float ay, float bx, float by) {
    float dx = bx - ax, dy = by - ay, t = sat(((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy));
    return len(px - ax - dx * t, py - ay - dy * t);
}
static float ellipse(float x, float y, float rx, float ry) { /* close to the edge, which is all we ask */
    return (len(x / rx, y / ry) - 1) * minf(rx, ry);
}
/* a half circle's stroke centre-line: the lower half (a smile) when down, else the upper */
static float arc(float x, float y, float r, int down) {
    if (down ? y < 0 : y > 0) return minf(len(x - r, y), len(x + r, y));
    return absf(len(x, y) - r);
}
static float heart(float x, float y) { /* unit heart, y up, its dip at (0, 1) and tip at (0, 0) */
    x = absf(x);
    if (y + x > 1) return len(x - 0.25f, y - 0.75f) - 0.35355f;
    float m = maxf(x + y, 0) * 0.5f, a = (x * x + (y - 1) * (y - 1)), b = (x - m) * (x - m) + (y - m) * (y - m);
    return fsqrt(minf(a, b)) * (x - y > 0 ? 1 : -1);
}

/* ---- colour ---- */

typedef struct { float r, g, b; } col;
static col C(float r, float g, float b) { return (col){ r, g, b }; }
static col cmix(col a, col b, float t) { return C(mixf(a.r, b.r, t), mixf(a.g, b.g, t), mixf(a.b, b.b, t)); }
static col cadd(col a, col b, float k) { return C(a.r + b.r * k, a.g + b.g * k, a.b + b.b * k); }
static col cmul(col a, float k) { return C(a.r * k, a.g * k, a.b * k); }
static unsigned pack(col c) {
    return 0xff000000u | (unsigned)(sat(c.r) * 255 + 0.5f) << 16 | (unsigned)(sat(c.g) * 255 + 0.5f) << 8 |
           (unsigned)(sat(c.b) * 255 + 0.5f);
}
static col unpack(unsigned p) {
    return C((float)(p >> 16 & 255) * (1 / 255.0f), (float)(p >> 8 & 255) * (1 / 255.0f),
             (float)(p & 255) * (1 / 255.0f));
}

/* A buddy's colours: body lit and in shade, its glow, its eyes. */
typedef struct { col hi, rim, glow, eye; } palette;
static const palette pals[] = {
    { { .95f, 1, .98f }, { .45f, .78f, .85f }, { .30f, .88f, 1 }, { .30f, .92f, 1 } },  /* Boo */
    { { 1, .93f, .98f }, { .86f, .50f, .80f }, { 1, .40f, .86f }, { 1, .30f, .78f } },   /* Hush */
};
#define PALS (int)(sizeof pals / sizeof *pals)
static const char *const pal_names[PALS] = { "Blue", "Pink" };
static const col INK = { .05f, .09f, .17f }; /* eyes, mouth */
static const col TONGUE = { 1, .42f, .55f };
static const col BLUSH = { 1, .45f, .62f };

/* ---- the buddy ---- */

#define HEAD_R 60.0f /* its head's radius */
#define HEAD_Y -24.0f
#define FOOT_Y 70.0f /* its hem, the scallops' middle */
#define HOME_X 187.0f
#define HOME_Y 156.0f
#define GROUND 276.0f /* its shadow's line */
#define SLEEP_S 25.0f  /* alone this long, it dozes */
#define PET_S 0.45f    /* a touch held this long on it, still, pets it */
#define DRAG_PX 10.0f  /* a touch that moves this far picks it up */
#define ANNOY_TAPS 5   /* taps within ANNOY_S that annoy it */
#define ANNOY_S 2.2f
#define DIZZY_TRAVEL 380.0f /* recent wheel travel (decaying) that makes it dizzy: about 2 quick turns */
#define PARTS 40
#define GLOW_PX 60.0f /* its glow's reach */
#define GRID 2        /* the distance's lattice, px */

enum { NORMAL, HAPPY, PET, ANNOY, SURPRISE, DIZZY, BOO, SLEEP };
enum { HEART, SPARK, ZED, PUFF };

typedef struct {
    float x, y, vx, vy, life, span, size, spin;
    int kind;
} part;

typedef struct {
    float x, y, vx, vy;      /* its centre, px, px/s */
    float sq, vsq;           /* squash: 1 at rest, under 1 squashed, over 1 stretched */
    float tilt, vtilt;       /* radians, clockwise */
    float arml, vl, armr, vr; /* each arm's lift, 0 down to 1 up */
    float lx, ly, tlx, tly;  /* where the eyes look, -1..1, and where they head */
    float wave;              /* the hem's phase */
    float t, idle, blink, blink_t, look_t, emit_t;
    float mood_t, next_t;    /* the mood's seconds left; then next, for next_t */
    int mood, next;
    float spin, wheel_dir, wheel_t; /* the wheel's recent travel, its way, seconds since */
    int down, drag, on;            /* a touch: down, picking it up, began on it */
    float fx, fy, sx, sy, ox, oy, down_t;
    float taps[ANNOY_TAPS];
    int ntap, pal, quit;
    float pal_t; /* seconds since the colour changed: the flash */
    int menu, sel, menu_down; /* Return's menu: open, its row, the row a touch went down on, plus 1 */
    float menu_t, menu_wheel; /* seconds open, the wheel's travel towards the next row */
    part p[PARTS];
} buddy;

static void moods(buddy *b, int mood, float s, int next, float next_s) {
    b->mood = mood, b->mood_t = s, b->next = next, b->next_t = next_s;
}

static void emit(buddy *b, int kind, float x, float y, float vx, float vy, float span, float size) {
    part *q = 0;
    for (int i = 0; i < PARTS && !q; ++i)
        if (b->p[i].life <= 0) q = &b->p[i];
    if (!q) return;
    *q = (part){ x, y, vx, vy, span, span, size, (frand() - 0.5f) * 4, kind };
}
static void hearts(buddy *b, int n) {
    for (int i = 0; i < n; ++i)
        emit(b, HEART, b->x + (frand() - 0.5f) * 90, b->y - 70 + (frand() - 0.5f) * 30, (frand() - 0.5f) * 60,
             -70 - frand() * 50, 1.3f + frand() * 0.5f, 9 + frand() * 6);
}
static void sparks(buddy *b, float x, float y, int n, float speed) {
    for (int i = 0; i < n; ++i) {
        float a = 2 * PI * ((float)i + frand() * 0.6f) / (float)n, v = speed * (0.6f + frand() * 0.6f);
        emit(b, SPARK, x, y, fcos(a) * v, fsin(a) * v, 0.5f + frand() * 0.3f, 5 + frand() * 4);
    }
}

static void init(buddy *b, int pal) {
    *b = (buddy){ .x = HOME_X, .y = HOME_Y + 30, .sq = 1, .blink = 1.5f, .pal = pal, .pal_t = 9 };
    b->vy = -160, b->vsq = 3; /* pops up into view */
    moods(b, SURPRISE, 0.35f, HAPPY, 1.0f);
}

/* the body: a round head over a skirt that flares a little to a scalloped hem, two stubby arms */
static float arm_dist(float x, float y, float lift, float side) {
    float a = mixf(1.0f, -0.75f, lift), sx = side * (HEAD_R - 8), sy = 8;
    return seg(x, y, sx, sy, sx + side * fcos(a) * 20, sy + fsin(a) * 20) - 10.5f;
}
static float body_dist(const buddy *b, float x, float y) {
    float head = len(x, y - HEAD_Y) - HEAD_R;
    float hem = FOOT_Y + 5.5f * fcos(x * 0.155f + b->wave) + 2 * fsin(x * 0.07f - b->wave * 0.6f);
    float w = HEAD_R + 8 * smooth(HEAD_Y, FOOT_Y, y), r = 12;
    float cy = (HEAD_Y + hem) * 0.5f, hh = (hem - HEAD_Y) * 0.5f;
    float dx = absf(x) - w + r, dy = absf(y - cy) - hh + r;
    float skirt = minf(maxf(dx, dy), 0) + len(maxf(dx, 0), maxf(dy, 0)) - r;
    float d = smin(head, skirt, 6);
    if (absf(y - 8) > 42 || absf(x) < 26) return d; /* nowhere near an arm */
    return smin(d, arm_dist(x, y, x < 0 ? b->arml : b->armr, x < 0 ? -1 : 1), 5);
}
/* screen to the buddy's own frame: unturned, unsquashed (about its middle). Affine, so a row
 * steps by (ux, uy) a pixel. */
typedef struct { float c, s, sx, sy, ux, uy; } xform;
static xform frame_of(const buddy *b) {
    xform f = { fcos(b->tilt), fsin(b->tilt), clampf(1 + (1 - b->sq) * 0.9f, 0.75f, 1.3f), b->sq, 0, 0 };
    f.ux = f.c / f.sx, f.uy = -f.s / f.sy;
    return f;
}
static void to_local(const buddy *b, float X, float Y, float *x, float *y) {
    xform f = frame_of(b);
    float dx = X - b->x, dy = Y - b->y;
    *x = (f.c * dx + f.s * dy) / f.sx, *y = (-f.s * dx + f.c * dy - 20) / f.sy + 20;
}

/* ---- input ---- */

static void wake(buddy *b) {
    b->idle = 0;
    if (b->mood == SLEEP) moods(b, SURPRISE, 0.45f, NORMAL, 0), b->vy -= 120, b->vsq += 3;
}

static void tap(buddy *b, float X, float Y) {
    float lx, ly;
    to_local(b, X, Y, &lx, &ly);
    int n = 0;
    for (int i = 0; i < ANNOY_TAPS - 1; ++i) b->taps[i] = b->taps[i + 1];
    b->taps[ANNOY_TAPS - 1] = b->t;
    for (int i = 0; i < ANNOY_TAPS; ++i) n += b->taps[i] > 0 && b->t - b->taps[i] < ANNOY_S;
    b->vsq -= 4.5f; /* squish */
    if (b->mood == ANNOY || n >= ANNOY_TAPS) { /* enough: pbbbt */
        moods(b, ANNOY, 2.0f, NORMAL, 0);
        for (int i = 0; i < ANNOY_TAPS; ++i) b->taps[i] = 0;
        emit(b, PUFF, b->x, b->y + 4, 0, 0, 0.45f, 10);
        b->vtilt += (lx < 0 ? 1 : -1) * 3;
        return;
    }
    if (ly < HEAD_Y + 18) { /* on the face: a boop, a hop and hearts */
        moods(b, SURPRISE, 0.22f, HAPPY, 1.3f);
        b->vy -= 150;
        hearts(b, 2 + (int)(frand() * 2));
    } else { /* the tummy: a giggle and a wiggle */
        moods(b, HAPPY, 1.2f, NORMAL, 0);
        b->vtilt += (lx < 0 ? 1 : -1) * 2.2f;
        sparks(b, X, Y, 5, 90);
    }
}

static void boo(buddy *b) {
    moods(b, BOO, 1.3f, HAPPY, 0.8f);
    b->vy -= 230, b->vsq += 6;
    sparks(b, b->x, b->y - 20, 10, 170);
}

/* a twirl into the other colour */
static void recolour(buddy *b) {
    b->pal = (b->pal + 1) % PALS;
    b->pal_t = 0;
    b->vtilt += 9;
    b->vsq -= 3;
    emit(b, PUFF, b->x, b->y, 0, 0, 0.5f, 20);
    sparks(b, b->x, b->y - 10, 8, 150);
    moods(b, HAPPY, 1.0f, NORMAL, 0);
}

/* Return's menu, drawn as Q2 Pod's games draw theirs: a dark box framed in the tone, the row
 * picked in the tone and the others grey */
#define MENU_ROWS 3
#define MENU_W 281
#define MENU_ROW 36
#define MENU_H (MENU_ROWS * MENU_ROW + 16)
#define MENU_X ((W - MENU_W) / 2)
#define MENU_Y ((H - MENU_H) / 2)
#define MENU_STEP 28 /* the wheel's raw travel a row: as a menu tick at 50% sensitivity */
static int menu_row(float X, float Y) { /* the row at a touch, else -1 */
    int r = (int)(Y - MENU_Y - 8) / MENU_ROW;
    return X >= MENU_X && X < MENU_X + MENU_W && Y >= MENU_Y + 8 && r >= 0 && r < MENU_ROWS ? r : -1;
}
static void menu_pick(buddy *b) {
    if (b->sel == 0) b->menu = 0;
    else if (b->sel == 1) recolour(b);
    else b->quit = 1;
}
static void menu_input(buddy *b, char kind, char what, int x, int y) {
    if (kind == 'k' && what == 'u' && x == KEY_RETURN) b->menu = 0;
    else if (kind == 'k' && what == 'u' && x == KEY_CENTER) menu_pick(b);
    else if (kind == 'w') {
        b->menu_wheel += (float)x;
        for (; b->menu_wheel <= -MENU_STEP; b->menu_wheel += MENU_STEP) b->sel += b->sel < MENU_ROWS - 1; /* clockwise: down */
        for (; b->menu_wheel >= MENU_STEP; b->menu_wheel -= MENU_STEP) b->sel -= b->sel > 0;
    } else if (kind == 't' && what == 'd') {
        int r = menu_row((float)x, (float)y);
        b->menu_down = r + 1;
        if (r >= 0) b->sel = r;
    } else if (kind == 't' && what == 'u') {
        int r = menu_row((float)x, (float)y);
        if (b->menu_down && r == b->menu_down - 1) menu_pick(b);
        else if (!b->menu_down && (x < MENU_X || x >= MENU_X + MENU_W || y < MENU_Y || y >= MENU_Y + MENU_H))
            b->menu = 0; /* a tap off the box: back to it */
        b->menu_down = 0;
    }
}

static void touch(buddy *b, char what, float X, float Y) {
    wake(b);
    if (what == 'd') {
        float lx, ly;
        to_local(b, X, Y, &lx, &ly);
        b->down = 1, b->drag = 0, b->down_t = 0;
        b->fx = b->sx = X, b->fy = b->sy = Y;
        b->on = body_dist(b, lx, ly) < 8;
        b->ox = X - b->x, b->oy = Y - b->y;
        if (b->on) b->vsq -= 1.5f;
        else sparks(b, X, Y, 6, 70); /* the eyes follow it (step) */
    } else if (what == 'm' && b->down) {
        b->fx = X, b->fy = Y;
        if (b->on && !b->drag && len(X - b->sx, Y - b->sy) > DRAG_PX) {
            b->drag = 1;
            moods(b, SURPRISE, 9, NORMAL, 0); /* wheee, until let go */
        }
    } else if (what == 'u' && b->down) {
        b->down = 0;
        if (b->drag) { /* flung: it springs home with what speed it had */
            moods(b, HAPPY, 0.9f, NORMAL, 0);
            b->vsq += 3;
        } else if (b->mood == PET)
            moods(b, HAPPY, 1.0f, NORMAL, 0);
        else if (b->on)
            tap(b, X, Y);
        b->drag = b->on = 0;
    }
}

static void key(buddy *b, char what, int code) {
    wake(b);
    if (what != 'u') return; /* a press acts on its release, as the Q2's menus do */
    if (code == KEY_RETURN) {
        b->menu = 1, b->sel = 0, b->menu_t = 0, b->menu_wheel = 0, b->menu_down = 0;
        if (b->down) touch(b, 'u', b->fx, b->fy); /* a finger on it lets go */
    } else if (code == KEY_CENTER)
        boo(b);
}

static void wheel(buddy *b, int travel) {
    wake(b);
    b->vtilt -= (float)travel * 0.03f; /* clockwise, so it leans with the finger */
    b->spin += (float)(travel < 0 ? -travel : travel);
    b->wheel_dir = travel < 0 ? 1.0f : -1.0f;
    b->wheel_t = 0;
    if (b->spin > DIZZY_TRAVEL && b->mood != DIZZY) {
        moods(b, DIZZY, 2.6f, NORMAL, 0);
        sparks(b, b->x, b->y + HEAD_Y - 40, 6, 60);
    }
}

/* one input datagram, as INPUT_SOCK carries */
static int num(const char **s) {
    int n = 0, neg = 0;
    while (**s == ' ') ++*s;
    if (**s == '-') neg = 1, ++*s;
    while (**s >= '0' && **s <= '9') n = n * 10 + *(*s)++ - '0';
    return neg ? -n : n;
}
static void input(buddy *b, const char *s) {
    char kind = s[0], what = s[0] && s[1] == ' ' ? s[2] : 0;
    const char *r = kind == 'w' || !what || !s[3] ? s + 1 : s + 3; /* "w N": no second word */
    if (b->menu) {
        int x = num(&r), y = num(&r);
        b->idle = 0;
        menu_input(b, kind, what, x, y);
        return;
    }
    if (kind == 't' && what) {
        int x = num(&r), y = num(&r);
        touch(b, what, (float)x, (float)y);
    } else if (kind == 'k' && what)
        key(b, what, num(&r));
    else if (kind == 'w')
        wheel(b, num(&r));
}

/* ---- motion ---- */

static void spring(float *x, float *v, float to, float k, float c, float dt) {
    *v += (k * (to - *x) - c * *v) * dt;
    *x += *v * dt;
}

static void step(buddy *b, float dt) {
    b->menu_t += dt;
    b->t += dt, b->idle += dt, b->down_t += dt, b->wheel_t += dt, b->pal_t += dt;
    if (b->mood_t > 0 && (b->mood_t -= dt) <= 0) moods(b, b->next, b->next_t, NORMAL, 0);
    if (b->mood == NORMAL && b->idle > SLEEP_S && !b->down) moods(b, SLEEP, 0, NORMAL, 0);
    if (b->down && b->on && !b->drag && b->down_t > PET_S) {
        if (b->mood != PET) moods(b, PET, 0, NORMAL, 0), b->emit_t = 0;
        if ((b->emit_t -= dt) <= 0) hearts(b, 1), b->emit_t = 0.4f;
    }
    if (b->mood == SLEEP && (b->emit_t -= dt) <= 0)
        emit(b, ZED, b->x + 40, b->y + HEAD_Y - 40, 22, -28, 2.4f, 9), b->emit_t = 1.7f;
    int m = b->mood;
    float t = b->t;

    /* where it floats: home, bobbing, unless held */
    float tx = HOME_X + 9 * fsin(t * 0.63f), ty = HOME_Y + 7 * fsin(t * 1.7f);
    if (m == SLEEP) tx = HOME_X, ty = HOME_Y + 14 + 4 * fsin(t * 0.9f);
    if (b->drag)
        tx = clampf(b->fx - b->ox, 40, W - 40), ty = clampf(b->fy - b->oy, 40, H - 60);
    float k = b->drag ? 380 : 38, c = b->drag ? 26 : 6.5f;
    spring(&b->x, &b->vx, tx, k, c, dt);
    spring(&b->y, &b->vy, ty, k, c, dt);

    /* squash and stretch: it stretches along fast moves, breathes at rest, purrs when petted */
    float speed = len(b->vx, b->vy);
    float sq = 1 + clampf(speed * 0.00035f, 0, 0.14f) + 0.018f * fsin(t * (m == SLEEP ? 1.6f : 3.1f));
    if (m == PET) sq = 0.93f + 0.025f * fsin(t * 18);
    if (m == BOO) sq = 1.1f;
    spring(&b->sq, &b->vsq, sq, 260, 9, dt);
    b->sq = clampf(b->sq, 0.7f, 1.35f);

    float tilt = 0.05f * fsin(t * 1.3f);
    if (b->drag) tilt = clampf(b->vx * 0.0018f, -0.55f, 0.55f);
    if (m == ANNOY) tilt = 0.13f * fsin(t * 20); /* shakes its head */
    if (m == DIZZY) tilt = 0.22f * fsin(t * 5.5f);
    if (m == SLEEP) tilt = 0.10f;
    if (m == PET) tilt = 0.08f * fsin(t * 6);
    spring(&b->tilt, &b->vtilt, tilt, 70, 6, dt);
    if (b->tilt > PI) b->tilt -= 2 * PI; /* a twirl ends upright the short way */
    if (b->tilt < -PI) b->tilt += 2 * PI;

    float al = 0.12f + 0.08f * fsin(t * 2.3f), ar = 0.12f + 0.08f * fsin(t * 2.3f + 1.4f);
    if (m == HAPPY) ar = 0.55f + 0.45f * fsin(t * 15), al = 0.35f; /* waves */
    if (m == BOO || b->drag) al = ar = 1;
    if (m == PET) al = ar = 0.38f;
    if (m == ANNOY) al = 0, ar = 0.15f;
    if (m == SLEEP) al = ar = 0;
    if (m == DIZZY) al = 0.5f + 0.4f * fsin(t * 7), ar = 0.5f - 0.4f * fsin(t * 7);
    spring(&b->arml, &b->vl, al, 220, 13, dt);
    spring(&b->armr, &b->vr, ar, 220, 13, dt);
    b->wave += dt * (2.0f + clampf(speed * 0.02f, 0, 6));

    /* the eyes: on the finger, the wheel's way, or wandering */
    if (b->down)
        b->tlx = clampf((b->fx - b->x) / 140, -1, 1), b->tly = clampf((b->fy - b->y - HEAD_Y) / 110, -1, 1);
    else if (b->wheel_t < 0.6f)
        b->tlx = b->wheel_dir * 0.8f, b->tly = -0.1f;
    else if ((b->look_t -= dt) <= 0) {
        int centre = frand() < 0.4f;
        b->tlx = centre ? 0 : (frand() - 0.5f) * 1.6f;
        b->tly = centre ? 0 : (frand() - 0.5f) * 0.9f;
        b->look_t = 1.2f + frand() * 2.5f;
    }
    float e = 1 - expneg(dt * 14);
    b->lx += (b->tlx - b->lx) * e, b->ly += (b->tly - b->ly) * e;
    if ((b->blink -= dt) <= 0) {
        b->blink_t = 0.16f;
        b->blink = frand() < 0.2f ? 0.25f : 2 + frand() * 3; /* now and then a double blink */
    }
    if (b->blink_t > 0) b->blink_t -= dt;
    b->spin *= expneg(dt * 1.1f);

    for (int i = 0; i < PARTS; ++i) {
        part *q = &b->p[i];
        if (q->life <= 0) continue;
        q->life -= dt;
        q->x += q->vx * dt, q->y += q->vy * dt;
        if (q->kind == SPARK) q->vx *= expneg(dt * 4), q->vy *= expneg(dt * 4);
        if (q->kind == HEART) q->vx += fsin(t * 5 + (float)i) * 40 * dt; /* drifts as it rises */
    }
}

/* ---- drawing ---- */

static unsigned backdrop[W * H];
static float grid[(W / GRID + 5) * (H / GRID + 5)], glow_tab[(int)GLOW_PX * 4 + 1];
static int backdrop_pal = -1;

/* black, as Q2 Pod's games are, with a few faint motes: the panel bands any gradient behind */
static void make_backdrop(int pal) {
    const palette *P = &pals[pal];
    unsigned seed = rng;
    rng = 1234567;
    for (int i = 0; i < W * H; ++i) backdrop[i] = 0xff000000u;
    for (int i = 0; i < 26; ++i) { /* motes */
        int mx = (int)(frand() * W), my = (int)(frand() * (GROUND - 20));
        float r = 1 + frand() * 1.6f, a = 0.15f + frand() * 0.25f;
        for (int y = my - 4; y <= my + 4; ++y)
            for (int x = mx - 4; x <= mx + 4; ++x) {
                if (x < 0 || y < 0 || x >= W || y >= H) continue;
                float k = a * sat(r + 0.5f - len((float)(x - mx), (float)(y - my)));
                backdrop[y * W + x] = pack(cadd(unpack(backdrop[y * W + x]), P->glow, k));
            }
    }
    rng = seed;
    backdrop_pal = pal;
}

static void blend(unsigned *px, col c, float a) {
    if (a > 0.003f) *px = pack(cmix(unpack(*px), c, sat(a)));
}
static void glow_add(unsigned *px, col c, float a) {
    if (a > 0.003f) *px = pack(cadd(unpack(*px), c, a));
}

/* What the face shows, from the mood and the blink */
enum { E_OPEN, E_HAPPY, E_SHUT, E_SLEEP, E_ANNOY, E_DIZZY };
enum { M_CAT, M_TONGUE, M_O, M_BOO, M_SMILE };

/* the face over the body at (x, y), in the buddy's frame: its colour over c, and how much */
static col face(const buddy *b, const palette *P, float x, float y, col c) {
    int m = b->mood, eyes = E_OPEN, mouth = M_CAT, blush = 0;
    float big = 1;
    if (m == HAPPY) eyes = E_HAPPY, mouth = M_SMILE, blush = 1;
    if (m == PET) eyes = E_SHUT, blush = 1;
    if (m == SLEEP) eyes = E_SLEEP, mouth = M_O;
    if (m == ANNOY) eyes = E_ANNOY, mouth = M_TONGUE;
    if (m == SURPRISE) big = 1.18f, mouth = M_O;
    if (m == DIZZY) eyes = E_DIZZY, mouth = M_CAT;
    if (m == BOO) eyes = E_ANNOY, mouth = M_BOO;
    float fx = x - b->lx * 9, fy = y - (HEAD_Y + 6) - b->ly * 6;
    float open = 1;
    if (b->blink_t > 0 && eyes == E_OPEN) open = maxf(0.08f, absf(b->blink_t / 0.08f - 1));

    if (blush) {
        float d = minf(ellipse(fx + 40, fy - 13, 11, 6), ellipse(fx - 40, fy - 13, 11, 6));
        c = cmix(c, BLUSH, 0.38f * smooth(6, -4, d));
    }
    for (int side = -1; side <= 1; side += 2) {
        float ex = fx - side * 26, ey = fy + 3; /* the eye's own frame */
        if (eyes == E_OPEN || eyes == E_ANNOY) {
            float rx = 13.5f * big, ry = 17 * big * open;
            float d = ellipse(ex, ey, rx, ry);
            float lid = -1e9f; /* E_ANNOY: above a lid that slants down to the middle */
            if (eyes == E_ANNOY) lid = (-ey - 3 - side * ex * 0.45f);
            float din = maxf(d, lid);
            c = cadd(c, P->eye, 0.35f * expneg(maxf(din, 0) / 4) * (din > 0)); /* the glow around */
            if (din < 1) {
                float ny = ey / ry;
                col e = cmix(INK, P->eye, 0.9f * smooth(-0.25f, 0.95f, ny));
                e = cmix(e, cadd(P->eye, C(1, 1, 1), 0.3f), 0.55f * smooth(-2.5f, 0, d)); /* lit rim */
                float hl = minf(len(ex + 4 * big, ey + 6 * big * open) - 4.3f * big, len(ex - 4.5f * big, ey - 5 * big) - 1.8f * big);
                if (open > 0.45f) e = cmix(e, C(1, 1, 1), sat(0.5f - hl));
                c = cmix(c, e, sat(0.5f - din));
            }
            if (eyes == E_ANNOY) { /* the lid's edge */
                float ld = absf(lid) / 1.1f;
                if (d < 2) c = cmix(c, INK, sat(1.6f - ld) * sat(1 - d));
            }
        } else {
            float d;
            if (eyes == E_HAPPY) /* > < : squeezed shut with glee */
                d = minf(seg(-ex * side, ey, -7, -7, 6, 0), seg(-ex * side, ey, 6, 0, -7, 7)) - 1.9f;
            else if (eyes == E_SHUT) /* ^ ^ */
                d = arc(ex, ey + 3, 9, 0) - 2;
            else if (eyes == E_SLEEP) /* the lower lids: ‿ */
                d = arc(ex, ey - 2, 9, 1) - 1.8f;
            else { /* dizzy: a turning spiral */
                float spin = b->t * 9 / (2 * PI); /* turns so far, kept to one: b->t only grows */
                spin -= (float)(int)spin;
                float r = len(ex, ey), turns = r / 4.2f - fatan2(ey, ex) / (2 * PI) - spin * side;
                float f = turns - (float)(int)(turns + 100) + 100;
                d = r > 13 ? r - 13 : (absf(f - 0.5f) - 0.22f) * 4.2f;
            }
            col ink = eyes == E_HAPPY ? cmix(INK, P->eye, 0.55f) : INK;
            if (eyes == E_HAPPY) c = cadd(c, P->eye, 0.4f * expneg(maxf(d, 0) / 3.5f) * (d > 0));
            c = cmix(c, ink, sat(0.5f - d));
        }
    }

    float mx = fx, my = fy - 21;
    if (mouth == M_TONGUE || mouth == M_BOO) {
        float td = mouth == M_BOO ? ellipse(mx, my - 4, 10, 12.5f) : 1e9f;
        if (td < 1) c = cmix(c, INK, sat(0.5f - td));
        float tg = mouth == M_BOO ? ellipse(mx, my - 11, 7, 5) : ellipse(mx - 1, my - 8, 5.5f, 8);
        if (mouth == M_BOO) tg = maxf(tg, td + 1.5f);
        if (tg < 1) {
            col tc = cmix(TONGUE, C(.8f, .2f, .35f), sat(1 - absf(mx - 1) / 1.2f) * smooth(-5, -10, my) * 0.6f);
            c = cmix(c, tc, sat(0.5f - tg));
        }
    }
    float d = 1e9f;
    if (mouth == M_CAT || mouth == M_TONGUE) /* ω */
        d = minf(arc(mx + 4.6f, my, 4.6f, 1), arc(mx - 4.6f, my, 4.6f, 1)) - 1.25f;
    if (mouth == M_O) d = absf(ellipse(mx, my - 3, 4.5f, 5.5f)) - 1.4f;
    if (mouth == M_SMILE) d = arc(mx, my - 3, 8, 1) - 1.6f;
    if (d < 1) {
        col ink = mouth == M_SMILE ? cmix(INK, P->eye, 0.55f) : INK;
        if (mouth == M_SMILE) c = cadd(c, P->eye, 0.3f * expneg(maxf(d, 0) / 3) * (d > 0));
        c = cmix(c, ink, sat(0.5f - d));
    }
    return c;
}

/* the body's colour at a point inside, d its distance: a soft lit volume, shadowed underneath,
 * rimmed in its glow, with a shine on the head */
static col shade(const palette *P, float x, float y, float d, float gx, float gy) {
    /* its slope on the screen (draw_buddy's: 12 a unit), light from the screen's top left; on a
     * ridge, the arms' middle, the slope fades and the surface faces the viewer, with no crease */
    float g = maxf(len(gx, gy), 12);
    gx /= g, gy /= g;
    float depth = smooth(0, 30, -d), k = 1 - depth;
    float nx = gx * k, ny = gy * k, nz = depth + 0.15f, n = len(len(nx, ny), nz);
    nx /= n, ny /= n, nz /= n;
    float diff = sat(nx * -0.45f + ny * -0.55f + nz * 0.70f);
    col c = cmix(P->rim, P->hi, 0.2f + 0.8f * diff);
    c = cmix(c, cmix(P->rim, P->glow, 0.4f), 0.45f * smooth(15, FOOT_Y + 4, y)); /* the skirt */
    float rim = (1 - nz) * (1 - nz);
    c = cadd(c, P->glow, rim * (0.25f + 0.35f * sat(ny + 0.3f)));
    float sh = len(x + 25, (y - HEAD_Y + 30) * 1.3f);
    c = cadd(c, C(1, 1, 1), 0.28f * expneg(sh * sh / 260));
    return c;
}

static void draw_buddy(const buddy *b, unsigned *fb) {
    const palette *P = &pals[b->pal];
    float flash = b->pal_t < 0.35f ? 1 - b->pal_t / 0.35f : 0;

    /* its shadow on the ground, smaller the higher it floats */
    float hgt = clampf(GROUND - (b->y + FOOT_Y * b->sq), 0, 200), sw = 62 * (1 - hgt / 330), sh = 9 * (1 - hgt / 330);
    for (int y = (int)(GROUND - sh * 2.5f); y < (int)(GROUND + sh * 2.5f); ++y)
        for (int x = (int)(b->x - sw * 1.6f); x < (int)(b->x + sw * 1.6f); ++x) {
            if (x < 0 || x >= W || y < 0 || y >= H) continue;
            float k = len(((float)x - b->x) / sw, ((float)y - GROUND) / sh);
            unsigned *px = &fb[y * W + x];
            blend(px, C(0, 0, 0), 0.45f * expneg(k * k * 2.2f));
            glow_add(px, P->glow, 0.08f * expneg(k * k * 1.2f));
        }

    /* its box, turned and squashed, with room for the glow */
    float c = absf(fcos(b->tilt)), s = absf(fsin(b->tilt)), sx = clampf(1 + (1 - b->sq) * 0.9f, 0.75f, 1.3f);
    float hw = (HEAD_R + 85) * sx, top = (HEAD_Y - HEAD_R - 60) * b->sq, bot = (FOOT_Y + 60) * b->sq;
    float hh = (bot - top) / 2, cy = (bot + top) / 2;
    float ew = c * hw + s * hh, eh = s * hw + c * hh;
    float ox = b->x - fsin(b->tilt) * cy, oy = b->y + fcos(b->tilt) * cy;
    int x0 = (int)maxf(0, ox - ew), x1 = (int)minf(W, ox + ew + 1);
    int y0 = (int)maxf(0, oy - eh), y1 = (int)minf(H, oy + eh + 1);
    xform f = frame_of(b);
    /* The distance on a GRID px lattice, bilinear between: a distance field is near enough linear.
     * Only the edge's pixels, where coverage needs it exact, ask body_dist again; the lattice's
     * slope lights the body. */
    if (glow_tab[0] == 0)
        for (int k = 0; k <= GLOW_PX * 4; ++k)
            glow_tab[k] = expneg((float)k / 48) * smooth(GLOW_PX, GLOW_PX * 0.5f, (float)k / 4);
    int gw = (x1 - x0) / GRID + 4, gh = (y1 - y0) / GRID + 4; /* a cell's margin round the box */
    for (int j = 0; j < gh; ++j) {
        float x, y;
        to_local(b, (float)(x0 - GRID), (float)(y0 + (j - 1) * GRID), &x, &y);
        for (int i = 0; i < gw; ++i, x += f.ux * GRID, y += f.uy * GRID) grid[j * gw + i] = body_dist(b, x, y);
    }
    float glow_k = 0.42f + flash * 0.5f;
    for (int Y = y0; Y < y1; ++Y) {
        float x, y, v = (float)((Y - y0) % GRID) / GRID;
        to_local(b, (float)x0, (float)Y, &x, &y);
        const float *row = grid + ((Y - y0) / GRID + 1) * gw + 1;
        for (int X = x0; X < x1; ++X, x += f.ux, y += f.uy) {
            unsigned *px = &fb[Y * W + X];
            const float *g = row + (X - x0) / GRID;
            float g0 = g[0], g1 = g[1], g2 = g[gw], g3 = g[gw + 1];
            float lo = minf(minf(g0, g1), minf(g2, g3)), hi = maxf(maxf(g0, g1), maxf(g2, g3));
            if (lo > GLOW_PX) continue;
            float u = (float)((X - x0) % GRID) / GRID;
            float d = mixf(mixf(g0, g1, u), mixf(g2, g3, u), v);
            if (lo < 1.5f && hi > -1.5f) d = body_dist(b, x, y); /* the edge */
            if (d > 0) {
                if (d < GLOW_PX) glow_add(px, P->glow, glow_k * glow_tab[(int)(d * 4)]);
                if (d > 1) continue;
            }
            /* the slope across three cells (6 px a unit): it fades over a ridge, which rounds */
            col bc = shade(P, x, y, d, g[2] - g[-1] + g[gw + 2] - g[gw - 1], g[2 * gw] - g[-gw] + g[2 * gw + 1] - g[1 - gw]);
            if (y < HEAD_Y + 50 && y > HEAD_Y - 30 && absf(x) < 58) bc = face(b, P, x, y, bc);
            bc = cmix(bc, C(1, 1, 1), flash * 0.6f);
            blend(px, bc, 0.5f - d);
        }
    }

    for (int i = 0; i < PARTS; ++i) {
        const part *q = &b->p[i];
        if (q->life <= 0) continue;
        float life = q->life / q->span, a = sat(life * 3) * (q->kind == SPARK ? life : 1);
        float sz = q->size * (q->kind == PUFF ? 1 + (1 - life) * 5 : q->kind == ZED ? 0.7f + 0.6f * (1 - life) : 1);
        float pop = q->kind == HEART ? smooth(1, 0.8f, life) * 0.4f + 0.6f + 0.4f * (1 - smooth(1, 0.85f, life)) : 1;
        sz *= pop;
        int r = (int)(sz * 2 + 8);
        for (int Y = (int)q->y - r; Y <= (int)q->y + r; ++Y)
            for (int X = (int)q->x - r; X <= (int)q->x + r; ++X) {
                if (X < 0 || Y < 0 || X >= W || Y >= H) continue;
                float x = (float)X - q->x, y = (float)Y - q->y, d;
                unsigned *px = &fb[Y * W + X];
                if (q->kind == HEART) {
                    d = heart(x / sz, -y / sz + 0.55f) * sz;
                    glow_add(px, C(1, .3f, .5f), 0.35f * a * expneg(maxf(d, 0) / 4) * (d > 0));
                    col hc = cmix(C(1, .30f, .48f), C(1, .75f, .82f), sat(-(x + y) / sz * 0.8f));
                    blend(px, hc, a * sat(0.5f - d));
                } else if (q->kind == SPARK) {
                    float ax = absf(x), ay = absf(y), w = sz * 0.22f;
                    d = minf(ax / sz + ay / w, ax / w + ay / sz) - 1;
                    d *= w;
                    glow_add(px, P->glow, 0.5f * a * expneg(len(x, y) / (sz * 0.5f)));
                    blend(px, C(1, 1, 1), a * sat(0.5f - d));
                } else if (q->kind == ZED) {
                    float h = sz * 0.5f;
                    d = minf(minf(seg(x, y, -h, -h, h, -h), seg(x, y, h, -h, -h, h)), seg(x, y, -h, h, h, h)) - 1.6f;
                    blend(px, cmix(P->hi, P->glow, 0.3f), a * 0.9f * sat(0.5f - d));
                } else {
                    d = absf(len(x, y) - sz) - 2.5f * life;
                    glow_add(px, P->glow, a * 0.8f * sat(0.5f - d));
                }
            }
    }
}

#include "font.h"
static int text_w(const char *s) {
    int w = 0;
    for (; *s; ++s)
        if (*s >= 32 && *s < 127) w += font_adv[*s - 32];
    return w;
}
static void text(unsigned *fb, int x, int y, const char *s, col c) {
    for (; *s; ++s) {
        if (*s < 32 || *s > 126) continue;
        int k = *s - 32, w = font_adv[k];
        const unsigned char *g = font_px + font_off[k];
        for (int r = 0; r < FONT_H; ++r)
            for (int i = 0; i < w; ++i)
                if (g[r * w + i] && x + i >= 0 && x + i < W && y + r >= 0 && y + r < H)
                    blend(&fb[(y + r) * W + x + i], c, (float)g[r * w + i] * (1 / 255.0f));
        x += w;
    }
}

/* the scene dimmed under a box, which fades in */
static void draw_menu(const buddy *b, unsigned *fb) {
    const palette *P = &pals[b->pal];
    float in = sat(b->menu_t / 0.12f);
    unsigned k = (unsigned)(256 * (1 - 0.55f * in));
    for (int i = 0; i < W * H; ++i) {
        unsigned p = fb[i];
        fb[i] = 0xff000000u | ((p >> 16 & 255) * k >> 8) << 16 | ((p >> 8 & 255) * k >> 8) << 8 | (p & 255) * k >> 8;
    }
    float hw = MENU_W * 0.5f, hh = MENU_H * 0.5f, cx = MENU_X + hw, cy = MENU_Y + hh, r = 12;
    for (int Y = MENU_Y - 2; Y < MENU_Y + MENU_H + 2; ++Y)
        for (int X = MENU_X - 2; X < MENU_X + MENU_W + 2; ++X) {
            float qx = absf((float)X + 0.5f - cx) - hw + r, qy = absf((float)Y + 0.5f - cy) - hh + r;
            float d = len(maxf(qx, 0), maxf(qy, 0)) + minf(maxf(qx, qy), 0) - r;
            unsigned *px = &fb[Y * W + X];
            blend(px, C(.125f, .125f, .125f), in * sat(0.5f - d));
            blend(px, P->glow, in * sat(1.5f - absf(d + 1)));
        }
    char colour[24] = "Colour: ";
    for (int i = 0; pal_names[b->pal][i]; ++i) colour[8 + i] = pal_names[b->pal][i], colour[9 + i] = 0;
    const char *rows[MENU_ROWS] = { "Continue", colour, "Quit" };
    for (int i = 0; i < MENU_ROWS; ++i)
        text(fb, (W - text_w(rows[i])) / 2, MENU_Y + 8 + i * MENU_ROW + (MENU_ROW - FONT_H) / 2, rows[i],
             cmul(i == b->sel ? P->glow : C(.667f, .667f, .667f), in));
}

static void render(const buddy *b, unsigned *fb) {
    if (backdrop_pal != b->pal) make_backdrop(b->pal);
    for (int i = 0; i < W * H; ++i) fb[i] = backdrop[i];
    draw_buddy(b, fb);
    if (b->menu) draw_menu(b, fb);
}

/* Whether it moves enough to want 60 frames a second, not 30 */
static int lively(const buddy *b) {
    if (b->menu) return b->menu_t < 0.2f;
    if (b->down || b->idle < 3 || (b->mood != NORMAL && b->mood != SLEEP)) return 1;
    for (int i = 0; i < PARTS; ++i)
        if (b->p[i].life > 0) return 1;
    return 0;
}

static unsigned canvas[W * H];

#ifdef HOST
/* ---- preview: a scripted session to PPM frames, 30 a second ---- */
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "/tmp/fidget";
    int pal = argc > 2 ? atoi(argv[2]) : 0;
    static const struct { float at; const char *in; } script[] = {
        { 1.6f, "t d 187 190" }, { 1.7f, "t u 187 190" },              /* tummy tap */
        { 3.0f, "t d 190 120" }, { 3.08f, "t u 190 120" },             /* boop */
        { 4.6f, "t d 180 170" }, { 6.6f, "t u 180 170" },              /* pet */
        { 7.6f, "t d 187 160" }, { 7.7f, "t m 160 150" }, { 7.9f, "t m 110 120" }, { 8.1f, "t m 80 110" },
        { 8.3f, "t m 120 120" }, { 8.45f, "t m 260 140" }, { 8.5f, "t u 270 140" }, /* drag, fling */
        { 10.0f, "t d 190 170" }, { 10.05f, "t u 190 170" }, { 10.25f, "t d 190 170" }, { 10.3f, "t u 190 170" },
        { 10.5f, "t d 190 170" }, { 10.55f, "t u 190 170" }, { 10.75f, "t d 190 170" }, { 10.8f, "t u 190 170" },
        { 11.0f, "t d 190 170" }, { 11.05f, "t u 190 170" },             /* annoyed */
        { 13.4f, "k u 218" },                                             /* Centre: boo */
        { 15.0f, "k u 170" }, { 15.5f, "w -30" }, { 15.9f, "k u 218" },   /* menu: Colour */
        { 16.4f, "k u 170" },                                             /* and back */
        { 17.0f, "t d 330 70" }, { 17.1f, "t u 330 70" },                 /* a poke beside it */
    };
    buddy b;
    init(&b, pal);
    char path[256];
    int frames = argc > 3 ? atoi(argv[3]) : 30 * 23, si = 0;
    for (int f = 0; f < frames; ++f) {
        float now = (float)f / 30;
        for (; si < (int)(sizeof script / sizeof *script) && script[si].at <= now; ++si) input(&b, script[si].in);
        if (now > 18 && now < 19.5f) input(&b, "w -18"); /* spun: dizzy */
        if (f == 30 * 20) b.idle = SLEEP_S;          /* then left alone: asleep */
        for (int k = 0; k < 4; ++k) step(&b, 1.0f / 120);
        render(&b, canvas);
        if (dir[0] == '-') continue; /* "-": time the renderer alone */
        snprintf(path, sizeof path, "%s/f%04d.ppm", dir, f);
        FILE *o = fopen(path, "wb");
        if (!o) return 1;
        fprintf(o, "P6 %d %d 255\n", W, H);
        for (int i = 0; i < W * H; ++i) fputc(canvas[i] >> 16 & 255, o), fputc(canvas[i] >> 8 & 255, o), fputc(canvas[i] & 255, o);
        fclose(o);
    }
    return 0;
}

#else
/* ---- the device: raw MIPS o32 syscalls ---- */

long sc(long n, long a, long b, long c, long d, long e, long f);
__asm__(".text\n.set noreorder\n.globl sc\nsc:\n"
        "move $2, $4\nmove $4, $5\nmove $5, $6\nmove $6, $7\n"
        "lw $7, 16($29)\nlw $8, 20($29)\nlw $9, 24($29)\n"
        "addiu $29, $29, -32\nsw $8, 16($29)\nsw $9, 20($29)\n"
        "syscall\naddiu $29, $29, 32\n"
        "beqz $7, 1f\nnop\nsubu $2, $0, $2\n1: jr $31\nnop\n"
        ".globl __start\n__start:\n"
        "lw $4, 0($29)\naddiu $5, $29, 4\nli $8, -8\nand $29, $29, $8\naddiu $29, $29, -32\n"
        "jal main\nnop\nmove $4, $2\nli $2, 4246\nsyscall\n1: b 1b\nnop\n.set reorder\n");

#define SYS_read 4003
#define SYS_write 4004
#define SYS_open 4005
#define SYS_close 4006
#define SYS_unlink 4010
#define SYS_ioctl 4054
#define SYS_mmap 4090
#define SYS_bind 4169
#define SYS_recv 4175
#define SYS_socket 4183
#define SYS_poll 4188
#define SYS_clock_gettime 4263
#define FBIOGET_VSCREENINFO 0x4600
#define FBIOGET_FSCREENINFO 0x4602
#define FBIOPAN_DISPLAY 0x4606
#define O_RDWR 2
#define O_WRONLY 1
#define O_CREAT 0x100
#define O_TRUNC 0x200
#define O_CLOEXEC 0x80000
#define MSG_DONTWAIT 0x40

void *memset(void *p, int v, __SIZE_TYPE__ n) {
    volatile unsigned char *d = p;
    while (n--) *d++ = (unsigned char)v;
    return p;
}
void *memcpy(void *p, const void *s, __SIZE_TYPE__ n) {
    volatile unsigned char *d = p;
    const unsigned char *q = s;
    while (n--) *d++ = *q++;
    return p;
}

static long long now_ms(void) {
    long ts[2];
    sc(SYS_clock_gettime, 1, (long)ts, 0, 0, 0, 0); /* CLOCK_MONOTONIC */
    return (long long)ts[0] * 1000 + ts[1] / 1000000;
}

/* the colour, kept beside the binary: "<dir>/state", one digit */
static char state_path[200];
static int load_pal(const char *argv0) {
    int n = 0, slash = -1;
    for (; argv0[n] && n < 180; ++n)
        if (argv0[n] == '/') slash = n;
    for (int i = 0; i <= slash; ++i) state_path[i] = argv0[i];
    const char *name = "state";
    for (int i = 0; i < 6; ++i) state_path[slash + 1 + i] = name[i];
    char c = 0;
    int fd = (int)sc(SYS_open, (long)state_path, O_CLOEXEC, 0, 0, 0, 0);
    if (fd >= 0) sc(SYS_read, fd, (long)&c, 1, 0, 0, 0), sc(SYS_close, fd, 0, 0, 0, 0, 0);
    return c >= '0' && c < '0' + PALS ? c - '0' : 0;
}
static void save_pal(int pal) {
    char c = (char)('0' + pal);
    int fd = (int)sc(SYS_open, (long)state_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644, 0, 0, 0);
    if (fd >= 0) sc(SYS_write, fd, (long)&c, 1, 0, 0, 0), sc(SYS_close, fd, 0, 0, 0, 0, 0);
}

static buddy bud;

int main(int argc, char **argv) {
    unsigned var[40], fix[17];
    int fb = (int)sc(SYS_open, (long)"/dev/fb0", O_RDWR | O_CLOEXEC, 0, 0, 0, 0);
    if (fb < 0 || sc(SYS_ioctl, fb, FBIOGET_VSCREENINFO, (long)var, 0, 0, 0) ||
        sc(SYS_ioctl, fb, FBIOGET_FSCREENINFO, (long)fix, 0, 0, 0))
        return 1;
    unsigned line = fix[11] / 4, y0 = var[5], back = y0 ? 0 : W; /* demo's other page */
    /* the Q2's panel only: 320 x 375 portrait, 32 bpp, red at 16, two pages */
    if (var[0] != H || var[1] != W || var[3] < 2 * W || var[6] != 32 || var[8] != 16 || line < H ||
        2 * W * line * 4 > fix[5])
        return 1;
    unsigned *mem = (unsigned *)sc(SYS_mmap, 0, (long)fix[5], 3, 1, fb, 0); /* RW, MAP_SHARED */
    if ((long)mem < 0 && (long)mem > -4096) return 1;

    struct { unsigned short family; char path[108]; } addr = { 1, INPUT_SOCK }; /* AF_UNIX */
    int sock = (int)sc(SYS_socket, 1, 1, 0, 0, 0, 0); /* SOCK_DGRAM, MIPS numbering */
    sc(SYS_unlink, (long)INPUT_SOCK, 0, 0, 0, 0, 0);
    if (sock >= 0) sc(SYS_bind, sock, (long)&addr, sizeof addr, 0, 0, 0);

    rng ^= (unsigned)now_ms();
    init(&bud, load_pal(argc > 0 ? argv[0] : ""));
    int pal = bud.pal;
    long long last = now_ms(), acc = 0, next = last;
    while (!bud.quit) {
        long long t = now_ms();
        int wait = next > t ? (int)(next - t) : 0;
        struct { int fd; short events, revents; } p = { sock, 1, 0 };
        sc(SYS_poll, (long)&p, sock >= 0, wait, 0, 0, 0);
        char msg[64];
        for (long n; sock >= 0 && (n = sc(SYS_recv, sock, (long)msg, sizeof msg - 1, MSG_DONTWAIT, 0, 0)) > 0;)
            msg[n] = 0, input(&bud, msg);
        t = now_ms();
        if (t < next) continue;
        acc += t - last, last = t;
        if (acc > 100) acc = 100; /* after a stall, no catching up in one go */
        for (; acc >= 8; acc -= 8) step(&bud, 0.008f);
        render(&bud, canvas);
        /* turned clockwise onto the portrait panel: view (x, y) is panel (H-1-y, x) */
        unsigned *page = mem + back * line;
        for (int py = 0; py < W; ++py) {
            unsigned *row = page + py * line;
            const unsigned *src = canvas + (H - 1) * W + py;
            for (int px = 0; px < H; ++px, src -= W) row[px] = *src;
        }
        var[5] = back;
        sc(SYS_ioctl, fb, FBIOPAN_DISPLAY, (long)var, 0, 0, 0);
        back = back ? 0 : W;
        next = t + (lively(&bud) ? 16 : 33);
        if (bud.pal != pal) save_pal(pal = bud.pal); /* now: a held Return's TERM saves nothing */
    }
    if (var[5] != y0) var[5] = y0, sc(SYS_ioctl, fb, FBIOPAN_DISPLAY, (long)var, 0, 0, 0);
    sc(SYS_unlink, (long)INPUT_SOCK, 0, 0, 0, 0, 0);
    return 0;
}
#endif
