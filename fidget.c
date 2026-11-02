/* Buddy (q2-fidget): a ghost buddy to poke, on the Shanling Q2's screen.
 *
 * The buddy floats over a dark backdrop and reacts to everything: a tap squishes it and makes it
 * giggle (on its face, a boop), a hold pets it (hearts), a drag picks it up and flings it, a burst
 * of taps annoys it (tongue out), a fast spin of the wheel makes it dizzy, Centre makes it say
 * boo, and left alone it dozes off. Return opens
 * the menu: Continue, its colour (blue or pink, kept), Tamagotchi (on or off, kept) and Quit.
 * Play/Pause and the side buttons stay the music's: Q2 Pod never sends them.
 *
 * Tamagotchi on, the same ghost is raised from an egg in real time (see "the pet" below); off, it is
 * the toy above and the pet's clock stops.
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
enum { EGG, BABY, CHILD, TEEN, ADULT }; /* Tamagotchi's life stages */
enum { HEART, SPARK, ZED, PUFF };

typedef struct {
    float x, y, vx, vy, life, span, size, spin;
    int kind;
} part;

/* The pet, as kept in .fidget/pet: its state as of the clock time at. Change the layout and
 * TAMA_MAGIC with it, so an old file reads as none. */
typedef struct {
    int magic, at;          /* at: seconds since the epoch this state is as of */
    int age, stage, form;   /* minutes old, EGG..ADULT, the adult's form: 1 halo, -1 horns, 0 neither */
    int hunger, fun;        /* hearts, 0..4 */
    int hunger_t, fun_t;    /* awake minutes since each last lost a heart */
    int energy, asleep, dark; /* 0..ENERGY_FULL; lights off */
    int sick;               /* doses still needed, 0 well */
    int poops, fuss;        /* fuss: minutes left of acting up for no reason */
    int discipline, weight, mistakes;
    int wait[3];            /* minutes a call has gone unanswered (hunger, fun, sick); -1: counted */
    int neglect, gone, generation; /* minutes starving or sick, it left, how many eggs so far */
} tama;

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
    int on_pet;               /* Tamagotchi mode */
    tama tm;
    int now, save;            /* the clock, seconds since the epoch; the pet changed, to be kept */
    int icon, icon_down, stats; /* the bar's picked icon, the one a touch went down on plus 1, Stats */
    float icon_wheel, say_t, gift_t, pet_t; /* the bar's wheel, say's seconds, the food's, petting's rest */
    const char *say;
    int gift;                 /* the stamp falling into its mouth */
    int round, wins, turn;    /* the game: this round (1..PLAY_ROUNDS, 0 none), won so far, its last turn */
    float play_wheel, shown;  /* the wheel towards a guess, seconds left showing the turn */
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
static int is_egg(const buddy *b) { return b->on_pet && b->tm.stage == EGG; }
static float body_dist(const buddy *b, float x, float y) {
    if (is_egg(b)) return ellipse(x, y - 5, 54, 68);
    float head = len(x, y - HEAD_Y) - HEAD_R;
    float hem = FOOT_Y + 5.5f * fcos(x * 0.155f + b->wave) + 2 * fsin(x * 0.07f - b->wave * 0.6f);
    float w = HEAD_R + 8 * smooth(HEAD_Y, FOOT_Y, y), r = 12;
    float cy = (HEAD_Y + hem) * 0.5f, hh = (hem - HEAD_Y) * 0.5f;
    float dx = absf(x) - w + r, dy = absf(y - cy) - hh + r;
    float skirt = minf(maxf(dx, dy), 0) + len(maxf(dx, 0), maxf(dy, 0)) - r;
    float d = smin(head, skirt, 6);
    if (b->on_pet && b->tm.form > 0) d = minf(d, absf(len(x, (y + 98) * 3.2f) - 30) - 5); /* a halo */
    if (b->on_pet && b->tm.form < 0) d = smin(d, seg(absf(x), y, 28, -72, 37, -94) - 6, 5); /* horns */
    if (absf(y - 8) > 42 || absf(x) < 26) return d; /* nowhere near an arm */
    return smin(d, arm_dist(x, y, x < 0 ? b->arml : b->armr, x < 0 ? -1 : 1), 5);
}
/* screen to the buddy's own frame: unturned, unsquashed (about its middle). Affine, so a row
 * steps by (ux, uy) a pixel. */
typedef struct { float c, s, sx, sy, ux, uy; } xform;
/* how big it has grown, 1 in the toy */
static float size_of(const buddy *b) {
    static const float size[] = { 0.62f, 0.5f, 0.66f, 0.84f, 1 };
    return b->on_pet ? size[b->tm.stage] : 1;
}
static xform frame_of(const buddy *b) {
    float S = size_of(b);
    xform f = { fcos(b->tilt), fsin(b->tilt), clampf(1 + (1 - b->sq) * 0.9f, 0.75f, 1.3f) * S, b->sq * S, 0, 0 };
    f.ux = f.c / f.sx, f.uy = -f.s / f.sy;
    return f;
}
static void to_local(const buddy *b, float X, float Y, float *x, float *y) {
    xform f = frame_of(b);
    float dx = X - b->x, dy = Y - b->y;
    *x = (f.c * dx + f.s * dy) / f.sx, *y = (-f.s * dx + f.c * dy - 20) / f.sy + 20;
}

/* ---- the pet ----
 *
 * After the 1996 Tamagotchi: an egg hatches and grows through Baby, Child and Teen to an Adult whose
 * form follows its care. Hunger and Fun fall as hearts; Meal and Snack refill them and add weight,
 * and the game takes it off. It poops, which can make it sick (two doses to cure); it
 * tires, and sleeps with the lights off or, slower, on; now and then it acts up for no reason, and a
 * scold then is what builds discipline. A need left unanswered is one care mistake per call, as
 * the toy's, and too long starving or sick and it leaves, for a new egg.
 *
 * Time is the wall clock's, so it lives while the program is closed: tama_sync replays the minutes
 * since at, through the one set of rules. Rates are slower than the toy's, whose call window is 15
 * minutes: this is a music player, opened now and then. */
#define TAMA_MAGIC 0x31414d54
#define ENERGY_FULL 960   /* awake a minute costs 1: 16 hours */
#define ENERGY_TIRED 720  /* below this the lights can put it to bed */
#define CALL_MIN 60       /* a call unanswered this long is a care mistake */
#define NEGLECT_MIN (72 * 60) /* awake minutes starving or sick before it leaves */
#define CATCHUP_MIN (30 * 24 * 60) /* the most minutes replayed in one go */
#define CLOCK_SANE 1704067200 /* 2024: before this the Q2's clock isn't set, and time stands still */
#define FAT_OVER 30       /* weight over this can make it sick */
#define FUSS_MIN 30       /* how long it acts up, the scold's window */
#define PLAY_ROUNDS 5
#define PLAY_WIN 3
static const int stage_at[] = { 0, 1, 60, 24 * 60, 3 * 24 * 60 }; /* minutes old */
static const int heart_min[] = { 30, 30, 60, 90, 120 };           /* awake minutes a heart lasts */
static const int poop_min[] = { 60, 60, 120, 180, 240 };          /* about how often it goes */
static const char *const stage_names[] = { "Egg", "Baby", "Child", "Teen", "Adult" };

static void tama_new(tama *g, int now) {
    int generation = g->magic == TAMA_MAGIC ? g->generation : 0;
    *g = (tama){ .magic = TAMA_MAGIC, .at = now, .hunger = 3, .fun = 3, .energy = ENERGY_FULL,
                 .weight = 5, .generation = generation + 1 };
}

/* one minute of its life */
static void tama_tick(tama *g) {
    if (g->gone) return;
    ++g->age;
    if (g->stage < ADULT && g->age >= stage_at[g->stage + 1]) {
        ++g->stage;
        g->weight += 5;
        if (g->stage == ADULT) /* its care shows */
            g->form = g->mistakes <= 1 && g->discipline >= 50 ? 1 : g->mistakes >= 5 ? -1 : 0;
    }
    if (g->stage == EGG) return;
    if (g->asleep) { /* hearts and the rest wait for morning */
        g->energy += g->dark ? 2 : 1;
        if (g->energy >= ENERGY_FULL) g->energy = ENERGY_FULL, g->asleep = g->dark = 0;
        return;
    }
    if (--g->energy <= 0) g->energy = 0, g->asleep = 1; /* drops off, lights on or not */
    int every = heart_min[g->stage] / (g->sick ? 2 : 1);
    if (++g->hunger_t >= every) g->hunger_t = 0, g->hunger -= g->hunger > 0;
    if (++g->fun_t >= every) g->fun_t = 0, g->fun -= g->fun > 0;
    if (g->poops < 4 && frand() * (float)poop_min[g->stage] < 1) ++g->poops;
    if (!g->sick && (frand() * 1440 < (float)g->poops || (g->weight > FAT_OVER && frand() * 1440 < 1))) g->sick = 2;
    if (g->fuss) --g->fuss;
    else if (frand() * (float)(360 + g->discipline * 6) < 1) g->fuss = FUSS_MIN;
    int need[3] = { g->hunger == 0, g->fun == 0, g->sick != 0 };
    for (int i = 0; i < 3; ++i) {
        if (!need[i]) g->wait[i] = 0;
        else if (g->wait[i] >= 0 && ++g->wait[i] >= CALL_MIN) ++g->mistakes, g->wait[i] = -1;
    }
    g->neglect = need[0] || need[2] ? g->neglect + 1 : 0;
    if (g->neglect >= NEGLECT_MIN) g->gone = 1;
}

/* The minutes since at, played through. ponytail: a clock set far forward counts as time passed (up
 * to CATCHUP_MIN), and one set back as none; Off in the menu is the way to leave it for long. */
static void tama_sync(tama *g, int now) {
    if (now < CLOCK_SANE) return;
    if (g->at < CLOCK_SANE || now < g->at) {
        g->at = now;
        return;
    }
    int n = (now - g->at) / 60;
    g->at += n * 60;
    for (int i = 0; i < (n < CATCHUP_MIN ? n : CATCHUP_MIN); ++i) tama_tick(g);
}

/* A file's pet, whole and in range, or none */
static int tama_ok(const tama *g) {
    return g->magic == TAMA_MAGIC && g->stage >= EGG && g->stage <= ADULT && g->hunger >= 0 && g->hunger <= 4 &&
           g->fun >= 0 && g->fun <= 4;
}

/* ---- input ---- */

static void say(buddy *b, const char *s) { b->say = s, b->say_t = 2; }

static void wake(buddy *b) {
    b->idle = 0;
    if (b->on_pet && b->tm.asleep) return; /* the pet sleeps until morning, or the lights */
    if (b->mood == SLEEP) moods(b, SURPRISE, 0.45f, NORMAL, 0), b->vy -= 120, b->vsq += 3;
}

static void tap(buddy *b, float X, float Y) {
    if (is_egg(b) || (b->on_pet && b->tm.asleep)) { /* no giggle out of these */
        b->vsq -= 2.5f, b->vtilt += 2;
        return;
    }
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
#define MENU_ROWS 4
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
/* Tamagotchi on: its pet as it was left, whose clock stopped when it was turned off */
static void pet_toggle(buddy *b) {
    b->on_pet = !b->on_pet;
    b->round = b->stats = 0;
    b->save = 1;
    if (b->on_pet) {
        if (b->tm.magic == TAMA_MAGIC) b->tm.at = b->now;
        else tama_new(&b->tm, b->now);
    }
    b->pal_t = 0;
    b->vsq -= 3;
    emit(b, PUFF, b->x, b->y, 0, 0, 0.5f, 20);
    moods(b, b->on_pet ? NORMAL : HAPPY, b->on_pet ? 0 : 1.0f, NORMAL, 0);
}
static void menu_pick(buddy *b) {
    if (b->sel == 0) b->menu = 0;
    else if (b->sel == 1) recolour(b);
    else if (b->sel == 2) pet_toggle(b);
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

/* The bar under it: the toy's eight icons, as on the original's top and bottom edges */
enum { I_MEAL, I_SNACK, I_PLAY, I_LIGHT, I_MEDICINE, I_CLEAN, I_SCOLD, I_STATS, ICONS, I_POOP = ICONS, I_HEART, STAMPS };
/* Clear of the glass's rounded corners, Q2 Pod's GLASS_R (80 px) and GLASS_SLACK (4) */
#define GLASS_R 80
#define GLASS_SLACK 4
#define TOP_X 36  /* the row of hearts, at either side */
#define TOP_Y 16
#define SAY_Y (TOP_Y + 26)
#define BAR_SLOT 38
#define BAR_X ((W - ICONS * BAR_SLOT) / 2)
#define BAR_Y 268

static void refuse(buddy *b, const char *why) {
    say(b, why);
    moods(b, ANNOY, 0.9f, NORMAL, 0);
    b->vtilt += 5;
}
static void pleased(buddy *b, const char *what) {
    say(b, what);
    moods(b, HAPPY, 1.2f, NORMAL, 0);
    hearts(b, 2);
    b->save = 1;
}
static void feed(buddy *b, int stamp) {
    pleased(b, stamp == I_MEAL ? "Yum!" : "A treat!");
    b->gift = stamp, b->gift_t = 0.6f;
}

static void act(buddy *b, int icon) {
    tama *g = &b->tm;
    if (icon == I_STATS) {
        b->stats = 1;
        return;
    }
    if (g->stage == EGG) return say(b, "Not hatched yet");
    if (g->asleep && icon != I_LIGHT) return say(b, "Shh, it's asleep");
    switch (icon) {
    case I_MEAL:
        if (g->fuss) refuse(b, "It won't eat!");
        else if (g->hunger >= 4) refuse(b, "It's full");
        else ++g->hunger, ++g->weight, feed(b, I_MEAL);
        break;
    case I_SNACK:
        g->fun += g->fun < 4, g->weight += 2, feed(b, I_SNACK);
        break;
    case I_PLAY:
        if (g->fuss) refuse(b, "It won't play!");
        else if (g->sick) refuse(b, "It feels too ill");
        else b->round = 1, b->wins = 0, b->play_wheel = 0, b->shown = 0;
        break;
    case I_LIGHT:
        b->save = 1;
        if (g->asleep && g->dark) { /* woken early, it sulks */
            g->dark = g->asleep = 0;
            g->fun -= g->fun > 0;
            moods(b, SURPRISE, 0.45f, NORMAL, 0);
            say(b, "It's grumpy");
        } else if (g->asleep) {
            g->dark = 1, say(b, "Night night");
        } else if (g->energy < ENERGY_TIRED) {
            g->dark = g->asleep = 1, say(b, "Night night");
        } else
            refuse(b, "It isn't sleepy");
        break;
    case I_MEDICINE:
        if (!g->sick) refuse(b, "It isn't sick");
        else if (--g->sick) say(b, "One more dose"), b->save = 1;
        else pleased(b, "All better!");
        break;
    case I_CLEAN:
        if (!g->poops) say(b, "Nothing to clean");
        else g->poops = 0, sparks(b, 60, 240, 8, 120), pleased(b, "All clean!");
        break;
    default: /* I_SCOLD: only acting up earns it */
        b->save = 1;
        if (g->fuss) g->fuss = 0, g->discipline += g->discipline < 100 ? 25 : 0, say(b, "It behaves");
        else g->fun -= g->fun > 0, say(b, "What was that for?");
        moods(b, ANNOY, 1.0f, NORMAL, 0);
        break;
    }
}

/* one guess, -1 anticlockwise/left or 1 clockwise/right; it then turns, and may be read right */
static void play_guess(buddy *b, int way) {
    if (b->shown > 0) return;
    b->turn = frand() < 0.5f ? -1 : 1;
    b->wins += way == b->turn;
    b->shown = 1.1f;
    b->wheel_dir = (float)b->turn, b->wheel_t = 0; /* the eyes look that way */
    b->vx += (float)b->turn * 240;
    b->vtilt += (float)b->turn * 5;
    moods(b, way == b->turn ? HAPPY : ANNOY, 0.9f, NORMAL, 0);
}

static int icon_at(int X, int Y) {
    int i = (X - BAR_X) / BAR_SLOT;
    return Y >= BAR_Y && X >= BAR_X && i < ICONS ? i : -1;
}

/* An input the pet's screens take: 1 when it did; 0 for the buddy (touch, Return) */
static int pet_input(buddy *b, char kind, char what, int x, int y) {
    int centre = kind == 'k' && what == 'u' && x == KEY_CENTER, ret = kind == 'k' && x == KEY_RETURN;
    if (b->stats) { /* any press or tap closes it */
        if ((kind == 'k' || kind == 't') && what == 'u') b->stats = 0;
        return 1;
    }
    if (ret) return 0; /* the menu, over anything */
    b->idle = 0;
    if (b->tm.gone) {
        if (centre) tama_new(&b->tm, b->now), b->save = 1;
        return 1;
    }
    if (b->round) {
        if (kind == 't' && what == 'u') play_guess(b, x < W / 2 ? -1 : 1);
        if (kind == 'w') {
            b->play_wheel += (float)x;
            if (absf(b->play_wheel) >= MENU_STEP) play_guess(b, b->play_wheel < 0 ? 1 : -1), b->play_wheel = 0;
        }
        return 1;
    }
    if (kind == 'w') {
        b->icon_wheel += (float)x;
        for (; b->icon_wheel <= -MENU_STEP; b->icon_wheel += MENU_STEP) b->icon = (b->icon + 1) % ICONS;
        for (; b->icon_wheel >= MENU_STEP; b->icon_wheel -= MENU_STEP) b->icon = (b->icon + ICONS - 1) % ICONS;
        return 0; /* it leans with the wheel too, and spun hard it's dizzy */
    }
    if (centre) act(b, b->icon);
    if (kind == 'k') return 1;
    if (kind == 't' && what == 'd' && !b->down) {
        int i = icon_at(x, y);
        b->icon_down = i + 1;
        if (i >= 0) b->icon = i;
    }
    if (kind == 't' && b->icon_down) {
        if (what == 'u') {
            if (icon_at(x, y) == b->icon_down - 1) act(b, b->icon_down - 1);
            b->icon_down = 0;
        }
        return 1;
    }
    return 0;
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
    if (b->on_pet) {
        const char *q = r;
        int x = num(&q), y = num(&q);
        if (pet_input(b, kind, what, x, y)) return;
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

/* the pet's clock and what shows of it; the game's rounds */
static void pet_step(buddy *b, float dt) {
    tama *g = &b->tm;
    int stage = g->stage, gone = g->gone, asleep = g->asleep;
    tama_sync(g, b->now);
    if (g->stage != stage || g->gone != gone || g->asleep != asleep) b->save = 1;
    if (g->stage != stage) { /* hatched, or grown: a hop */
        say(b, g->stage == BABY ? "It hatched!" : "It grew!");
        moods(b, HAPPY, 1.5f, NORMAL, 0);
        b->vy -= 200, b->vsq += 5;
        sparks(b, b->x, b->y - 20, 12, 170);
    }
    if (asleep && !g->asleep) moods(b, SURPRISE, 0.45f, NORMAL, 0), say(b, "Good morning!");
    if (g->asleep && b->mood == NORMAL) moods(b, SLEEP, 0, NORMAL, 0);
    if (g->gone) b->round = 0;
    if (b->say_t > 0) b->say_t -= dt;
    if (b->gift_t > 0) b->gift_t -= dt;
    if (b->pet_t > 0) b->pet_t -= dt;
    if (b->round && b->shown > 0 && (b->shown -= dt) <= 0 && ++b->round > PLAY_ROUNDS) { /* the game's end */
        b->round = 0;
        g->weight -= g->weight > 1;
        if (b->wins >= PLAY_WIN) g->fun += g->fun < 4, pleased(b, "You won!");
        else say(b, "It won!"), b->save = 1;
    }
}

static void step(buddy *b, float dt) {
    b->menu_t += dt;
    b->t += dt, b->idle += dt, b->down_t += dt, b->wheel_t += dt, b->pal_t += dt;
    if (b->on_pet) pet_step(b, dt);
    if (b->mood_t > 0 && (b->mood_t -= dt) <= 0) moods(b, b->next, b->next_t, NORMAL, 0);
    if (b->mood == NORMAL && b->idle > SLEEP_S && !b->down && !is_egg(b)) moods(b, SLEEP, 0, NORMAL, 0);
    if (b->down && b->on && !b->drag && b->down_t > PET_S) {
        if (b->mood != PET) {
            moods(b, PET, 0, NORMAL, 0), b->emit_t = 0;
            if (b->on_pet && b->pet_t <= 0 && !b->tm.asleep && b->tm.stage != EGG) /* a heart, at most each 10 minutes */
                b->tm.fun += b->tm.fun < 4, b->pet_t = 600, b->save = 1;
        }
        if ((b->emit_t -= dt) <= 0) hearts(b, 1), b->emit_t = 0.4f;
    }
    if (b->mood == SLEEP && (b->emit_t -= dt) <= 0)
        emit(b, ZED, b->x + 40, b->y + HEAD_Y - 40, 22, -28, 2.4f, 9), b->emit_t = 1.7f;
    int m = b->mood;
    float t = b->t;

    /* where it floats: home, bobbing, unless held */
    float S = size_of(b), oy = (FOOT_Y + 10) * (1 - S); /* however big, its hem stays down there */
    float tx = HOME_X + 9 * fsin(t * 0.63f), ty = HOME_Y + oy + 7 * fsin(t * 1.7f);
    if (m == SLEEP) tx = HOME_X, ty = HOME_Y + oy + 14 + 4 * fsin(t * 0.9f);
    if (is_egg(b)) tx = HOME_X, ty = HOME_Y + oy + 12;
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
    if (is_egg(b)) tilt = 0.14f * fsin(t * 7) * smooth(0.2f, 0.8f, fsin(t * 0.9f)); /* rocks, on and off */
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
enum { M_CAT, M_TONGUE, M_O, M_BOO, M_SMILE, M_FROWN };

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
    if (b->on_pet && m == NORMAL) { /* what it wants shows */
        const tama *g = &b->tm;
        if (g->hunger == 0 || g->fun == 0) mouth = M_FROWN;
        if (g->sick) eyes = E_SLEEP, mouth = M_FROWN;
        if (g->fuss) eyes = E_ANNOY, mouth = M_TONGUE;
    }
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
    if (mouth == M_FROWN) d = arc(mx, my + 3, 8, 0) - 1.6f;
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
    float S = size_of(b), ground = b->on_pet ? GROUND - 18 : GROUND; /* above the pet's bar */

    /* its shadow on the ground, smaller the higher it floats */
    float hgt = clampf(ground - (b->y + FOOT_Y * b->sq * S), 0, 200), sw = 62 * S * (1 - hgt / 330), sh = 9 * (1 - hgt / 330);
    for (int y = (int)(ground - sh * 2.5f); y < (int)(ground + sh * 2.5f); ++y)
        for (int x = (int)(b->x - sw * 1.6f); x < (int)(b->x + sw * 1.6f); ++x) {
            if (x < 0 || x >= W || y < 0 || y >= H) continue;
            float k = len(((float)x - b->x) / sw, ((float)y - ground) / sh);
            unsigned *px = &fb[y * W + x];
            blend(px, C(0, 0, 0), 0.45f * expneg(k * k * 2.2f));
            glow_add(px, P->glow, 0.08f * expneg(k * k * 1.2f));
        }

    /* its box, turned and squashed, with room for the glow */
    float c = absf(fcos(b->tilt)), s = absf(fsin(b->tilt)), sx = clampf(1 + (1 - b->sq) * 0.9f, 0.75f, 1.3f);
    float hw = (HEAD_R + 85) * sx * S, top = (HEAD_Y - HEAD_R - (b->on_pet ? 95 : 60)) * b->sq * S, bot = (FOOT_Y + 60) * b->sq * S;
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
    int egg = is_egg(b), sick = b->on_pet && b->tm.sick;
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
                if (d * S > 1) continue;
            }
            /* the slope across three cells (6 px a unit): it fades over a ridge, which rounds */
            col bc = shade(P, x, y, d, g[2] - g[-1] + g[gw + 2] - g[gw - 1], g[2 * gw] - g[-gw] + g[2 * gw + 1] - g[1 - gw]);
            if (egg) { /* speckled, not faced */
                float spot = minf(minf(ellipse(x + 22, y + 26, 10, 12), ellipse(x - 20, y + 2, 12, 10)), ellipse(x + 8, y - 36, 9, 8));
                bc = cmix(bc, P->glow, 0.55f * sat(0.5f - spot));
            } else if (y < HEAD_Y + 50 && y > HEAD_Y - 30 && absf(x) < 58)
                bc = face(b, P, x, y, bc);
            if (sick) bc = cmix(bc, C(.45f, .75f, .35f), 0.32f);
            bc = cmix(bc, C(1, 1, 1), flash * 0.6f);
            blend(px, bc, 0.5f - d * S);
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

static void dim(unsigned *fb, float by) {
    unsigned k = (unsigned)(256 * (1 - by));
    for (int i = 0; i < W * H; ++i) {
        unsigned p = fb[i];
        fb[i] = 0xff000000u | ((p >> 16 & 255) * k >> 8) << 16 | ((p >> 8 & 255) * k >> 8) << 8 | (p & 255) * k >> 8;
    }
}
/* a dark box framed in the tone */
static void draw_box(unsigned *fb, const palette *P, int x, int y, int w, int h, float in) {
    float hw = (float)w * 0.5f, hh = (float)h * 0.5f, cx = (float)x + hw, cy = (float)y + hh, r = 12;
    for (int Y = y - 2; Y < y + h + 2; ++Y)
        for (int X = x - 2; X < x + w + 2; ++X) {
            float qx = absf((float)X + 0.5f - cx) - hw + r, qy = absf((float)Y + 0.5f - cy) - hh + r;
            float d = len(maxf(qx, 0), maxf(qy, 0)) + minf(maxf(qx, qy), 0) - r;
            unsigned *px = &fb[Y * W + X];
            blend(px, C(.125f, .125f, .125f), in * sat(0.5f - d));
            blend(px, P->glow, in * sat(1.5f - absf(d + 1)));
        }
}
/* s, then n at e's end: no printf here */
static char *put(char *e, const char *s) {
    while (*s) *e++ = *s++;
    *e = 0;
    return e;
}
static char *putn(char *e, int n) {
    char d[12];
    int k = 0;
    do d[k++] = (char)('0' + n % 10), n /= 10; while (n);
    while (k) *e++ = d[--k];
    *e = 0;
    return e;
}
static void rows_in_box(unsigned *fb, const char *const *rows, int n, int y, int sel, const palette *P, float in) {
    for (int i = 0; i < n; ++i)
        text(fb, (W - text_w(rows[i])) / 2, y + i * MENU_ROW + (MENU_ROW - FONT_H) / 2, rows[i],
             cmul(i == sel ? P->glow : C(.667f, .667f, .667f), in));
}

/* the scene dimmed under a box, which fades in */
static void draw_menu(const buddy *b, unsigned *fb) {
    const palette *P = &pals[b->pal];
    float in = sat(b->menu_t / 0.12f);
    dim(fb, 0.55f * in);
    draw_box(fb, P, MENU_X, MENU_Y, MENU_W, MENU_H, in);
    char colour[24], pet[24];
    put(put(colour, "Colour: "), pal_names[b->pal]);
    put(put(pet, "Tamagotchi: "), b->on_pet ? "On" : "Off");
    const char *rows[MENU_ROWS] = { "Continue", colour, pet, "Quit" };
    rows_in_box(fb, rows, MENU_ROWS, MENU_Y + 8, b->sel, P, in);
}

/* ---- the pet's screen ---- */

#define STAMP 32
static unsigned char stamps[STAMPS][STAMP * STAMP], stamp_made[STAMPS];
static float stamp_dist(int k, float x, float y) { /* its distance, in a 32 px box about (0, 0) */
    float d;
    switch (k) {
    case I_MEAL: /* a steaming bowl */
        d = maxf(len(x, y - 2) - 13, 2 - y);
        d = minf(d, minf(seg(x, y, -5, -3, -5, -11), seg(x, y, 5, -3, 5, -11)) - 1.8f);
        return d;
    case I_SNACK: /* a wrapped sweet */
        d = len(x, y) - 7;
        for (int s = -1; s <= 1; s += 2)
            d = minf(d, minf(seg(x, y, s * 7.0f, 0, s * 14.0f, -6), seg(x, y, s * 7.0f, 0, s * 14.0f, 6)) - 1.8f);
        return d;
    case I_PLAY: /* a ball */
        return minf(absf(len(x, y) - 11) - 2, maxf(absf(y) - 1.5f, len(x, y) - 11));
    case I_LIGHT: /* a bulb */
        d = len(x, y + 4) - 9;
        return minf(d, minf(seg(x, y, -4, 8, 4, 8), seg(x, y, -3, 12, 3, 12)) - 1.6f);
    case I_MEDICINE: /* a cross */
        return minf(maxf(absf(x) - 3.5f, absf(y) - 11), maxf(absf(x) - 11, absf(y) - 3.5f));
    case I_CLEAN: /* a broom */
        d = seg(x, y, 10, -13, -2, 2) - 1.6f;
        return minf(d, minf(seg(x, y, -2, 2, -11, 8), minf(seg(x, y, -2, 2, -8, 12), seg(x, y, -2, 2, -3, 13))) - 2);
    case I_SCOLD: /* ! */
        return minf(seg(x, y, 0, -12, 0, 3) - 2.8f, len(x, y - 11) - 3);
    case I_STATS: /* bars */
        d = maxf(absf(x + 9) - 3, absf(y - 7) - 5);
        d = minf(d, maxf(absf(x) - 3, absf(y - 2) - 10));
        return minf(d, maxf(absf(x - 9) - 3, absf(y + 2) - 14));
    case I_POOP: /* a coil */
        d = minf(ellipse(x, y - 9, 13, 5), ellipse(x, y - 2, 9, 4.5f));
        return minf(d, seg(x, y, 0, -6, 3, -11) - 3);
    default: /* I_HEART */
        return heart(x / 15, -y / 15 + 0.55f) * 15;
    }
}
static void stamp(unsigned *fb, int k, int cx, int cy, col c) {
    if (!stamp_made[k]++)
        for (int y = 0; y < STAMP; ++y)
            for (int x = 0; x < STAMP; ++x)
                stamps[k][y * STAMP + x] = (unsigned char)(sat(0.5f - stamp_dist(k, (float)x + 0.5f - STAMP / 2, (float)y + 0.5f - STAMP / 2)) * 255);
    for (int y = 0; y < STAMP; ++y)
        for (int x = 0; x < STAMP; ++x) {
            int X = cx - STAMP / 2 + x, Y = cy - STAMP / 2 + y, a = stamps[k][y * STAMP + x];
            if (a && X >= 0 && Y >= 0 && X < W && Y < H) blend(&fb[Y * W + X], c, (float)a * (1 / 255.0f));
        }
}
static void meter(unsigned *fb, const char *name, int x, int n) {
    text(fb, x, TOP_Y, name, C(.667f, .667f, .667f));
    x += text_w(name) + 14;
    for (int i = 0; i < 4; ++i, x += 16) stamp(fb, I_HEART, x, TOP_Y + 14, i < n ? C(1, .35f, .52f) : C(.2f, .2f, .2f));
}
/* Whether icon i has something to answer */
static int wanted(const tama *g, int i) {
    if (g->stage == EGG || g->gone) return 0;
    switch (i) {
    case I_MEAL: return !g->asleep && g->hunger <= 1;
    case I_SNACK: case I_PLAY: return !g->asleep && g->fun <= 1;
    case I_LIGHT: return g->asleep ? !g->dark : g->energy < 120;
    case I_MEDICINE: return g->sick != 0;
    case I_CLEAN: return g->poops != 0;
    case I_SCOLD: return g->fuss != 0;
    default: return 0;
    }
}
static void draw_pet_under(const buddy *b, unsigned *fb) { /* what lies on the ground */
    for (int i = 0; i < b->tm.poops; ++i)
        stamp(fb, I_POOP, i < 2 ? 48 + i * 36 : 327 - (i - 2) * 36, 244, C(.62f, .46f, .28f));
}
static void draw_pet(const buddy *b, unsigned *fb) {
    const palette *P = &pals[b->pal];
    const tama *g = &b->tm;
    float t = b->t;
    if (b->gift_t > 0) { /* it falls in */
        float k = 1 - b->gift_t / 0.6f, S = size_of(b);
        stamp(fb, b->gift, (int)b->x, (int)(mixf(b->y - 140 * S - 30, b->y - 10 * S, k * k)), C(1, .85f, .55f));
    }
    if (!g->gone && !g->asleep && g->stage != EGG) { /* the toy's attention icon: ! over its head */
        int calls = 0;
        for (int i = 0; i < ICONS; ++i) calls |= wanted(g, i);
        if (calls && fsin(t * 6) > -0.4f)
            stamp(fb, I_SCOLD, (int)(b->x + 58 * size_of(b) + 18), (int)(b->y - 96 * size_of(b)), C(1, .35f, .52f));
    }
    if (g->dark) dim(fb, 0.8f);
    meter(fb, "Food", TOP_X, g->hunger);
    meter(fb, "Fun", TOP_X + 130, g->fun);
    const char *stage = stage_names[g->stage];
    text(fb, W - TOP_X - text_w(stage), TOP_Y, stage, P->glow);
    for (int i = 0; i < ICONS; ++i) {
        int x = BAR_X + i * BAR_SLOT;
        if (i == b->icon) draw_box(fb, P, x + 2, BAR_Y + 2, BAR_SLOT - 4, BAR_SLOT - 4, 1);
        col c = wanted(g, i) ? cmix(C(1, .35f, .52f), C(1, .8f, .85f), 0.5f + 0.5f * fsin(t * 6))
                : i == b->icon ? P->glow : C(.667f, .667f, .667f);
        stamp(fb, i, x + BAR_SLOT / 2, BAR_Y + BAR_SLOT / 2, c);
    }
    if (g->gone) {
        const char *a = "It floated away...", *c = "Centre: a new egg";
        text(fb, (W - text_w(a)) / 2, 120, a, P->glow);
        text(fb, (W - text_w(c)) / 2, 156, c, C(.667f, .667f, .667f));
    } else if (b->round) {
        char line[32];
        char *e = put(line, "Which way? ");
        e = putn(e, b->round), e = put(e, " of "), putn(e, PLAY_ROUNDS);
        text(fb, (W - text_w(line)) / 2, SAY_Y, line, P->glow);
        char won[16];
        e = put(won, "Won "), putn(e, b->wins);
        text(fb, (W - text_w(won)) / 2, 244, won, C(.667f, .667f, .667f));
    } else if (b->say_t > 0)
        text(fb, (W - text_w(b->say)) / 2, SAY_Y, b->say, cmul(P->glow, sat(b->say_t * 3)));
    if (b->stats) {
        char age[24], weight[24], disc[24], gen[24], *e;
        int d = g->age / (24 * 60);
        e = put(age, d ? "Age: " : "Age: under a day");
        if (d) e = putn(e, d), put(e, d == 1 ? " day" : " days");
        putn(put(weight, "Weight: "), g->weight);
        e = putn(put(disc, "Discipline: "), g->discipline), put(e, "%");
        putn(put(gen, "Egg number "), g->generation);
        const char *rows[] = { stage_names[g->stage], age, weight, disc, gen };
        int n = 5, h = n * MENU_ROW + 16, y = (H - h) / 2;
        dim(fb, 0.55f);
        draw_box(fb, P, MENU_X, y, MENU_W, h, 1);
        rows_in_box(fb, rows, n, y + 8, 0, P, 1);
    }
}

static void render(const buddy *b, unsigned *fb) {
    if (backdrop_pal != b->pal) make_backdrop(b->pal);
    for (int i = 0; i < W * H; ++i) fb[i] = backdrop[i];
    if (b->on_pet) draw_pet_under(b, fb);
    if (!b->on_pet || !b->tm.gone) draw_buddy(b, fb);
    if (b->on_pet) draw_pet(b, fb);
    if (b->menu) draw_menu(b, fb);
}

/* Whether it moves enough to want 60 frames a second, not 30 */
static int lively(const buddy *b) {
    if (b->menu) return b->menu_t < 0.2f;
    if (b->down || b->idle < 3 || (b->mood != NORMAL && b->mood != SLEEP) || b->gift_t > 0) return 1;
    for (int i = 0; i < PARTS; ++i)
        if (b->p[i].life > 0) return 1;
    return 0;
}

static unsigned canvas[W * H];

#ifdef HOST
/* ---- preview: a scripted session to PPM frames, 30 a second ---- */
#include <stdio.h>
#include <stdlib.h>

#include <assert.h>
#define NOW 1800000000
/* the width the glass hides at each end of row y, as Q2 Pod's tools/ipod.py corner_inset */
static float glass(int y) {
    float d = maxf(maxf((float)(GLASS_R - y), (float)(y - (H - GLASS_R))), 0);
    return GLASS_R - fsqrt(GLASS_R * GLASS_R - d * d);
}
/* the pet's rules: ./fidget check */
static int check(void) {
    static buddy b;
    tama *g = &b.tm;
    /* the top row and the bar's selection box clear the corners, at their outer rows */
    assert(glass(TOP_Y) + GLASS_SLACK <= TOP_X);
    assert(glass(BAR_Y + BAR_SLOT - 2) + GLASS_SLACK <= BAR_X + 2);
    init(&b, 0);
    b.now = NOW;
    /* off is the toy: Centre says boo and the pet's clock doesn't run */
    input(&b, "k u 218");
    assert(b.mood == BOO && g->magic == 0);
    /* the menu's third row turns it on: an egg, which hatches in a minute */
    input(&b, "k u 170"), input(&b, "w -30"), input(&b, "w -30"), input(&b, "k u 218"), input(&b, "k u 170");
    assert(b.on_pet && !b.menu && g->stage == EGG && g->generation == 1);
    input(&b, "k u 218");
    assert(b.say && g->hunger == 3); /* Centre: Meal, but it's an egg */
    b.now += 60, step(&b, 0.01f);
    assert(g->stage == BABY);
    /* a meal: Centre on the first icon; a full pet refuses, and weighs what it ate */
    int w = g->weight;
    input(&b, "k u 218");
    assert(g->hunger == 4 && g->weight == w + 1);
    input(&b, "k u 218");
    assert(g->hunger == 4 && g->weight == w + 1);
    /* a tap on the bar picks and acts: Stats, which any press closes without acting */
    input(&b, "t d 320 300"), input(&b, "t u 320 300");
    assert(b.stats);
    input(&b, "k u 218");
    assert(!b.stats && g->hunger == 4);
    /* the wheel walks the bar, round its ends */
    b.icon = 0;
    input(&b, "w 28");
    assert(b.icon == ICONS - 1);
    input(&b, "w -28");
    assert(b.icon == 0);
    /* scolding only earns discipline when it's acting up */
    g->fuss = 5, g->fun = 2, b.icon = I_SCOLD, input(&b, "k u 218");
    assert(g->discipline == 25 && !g->fuss && g->fun == 2);
    input(&b, "k u 218");
    assert(g->discipline == 25 && g->fun == 1);
    /* two doses cure it */
    g->sick = 2, b.icon = I_MEDICINE, input(&b, "k u 218");
    assert(g->sick == 1);
    input(&b, "k u 218");
    assert(g->sick == 0);
    /* the game: five guesses, whose end pays out on three wins */
    g->fuss = 0, g->fun = 0, b.icon = I_PLAY, input(&b, "k u 218");
    assert(b.round == 1);
    for (int i = 0; i < PLAY_ROUNDS; ++i) {
        input(&b, "w -30");
        assert(b.shown > 0);
        b.wins = 5, b.shown = 0.001f, step(&b, 0.01f);
    }
    assert(!b.round && g->fun == 1);
    /* lights out only when tired; dark, it fills twice as fast, and wakes by itself */
    b.icon = I_LIGHT, g->energy = ENERGY_FULL, input(&b, "k u 218");
    assert(!g->asleep);
    g->energy = 0, input(&b, "k u 218");
    assert(g->asleep && g->dark);
    int h = g->hunger;
    b.now += 60 * 480, step(&b, 0.01f);
    assert(!g->asleep && !g->dark && g->energy == ENERGY_FULL && g->hunger == h);
    /* time closed counts; unset or backward clocks don't */
    tama_new(g, NOW);
    g->stage = BABY;
    tama_sync(g, NOW - 600);
    assert(g->age == 0 && g->at == NOW - 600);
    tama_sync(g, 1000);
    assert(g->age == 0);
    tama_sync(g, NOW + 3540);
    assert(g->age == 69 && g->stage == CHILD);
    /* one unanswered call is one mistake, however long; too long starving and it leaves */
    tama_new(g, NOW);
    g->stage = BABY, g->hunger = g->fun = 0;
    for (int i = 0; i < 300; ++i) {
        g->hunger = g->fun = 0, g->energy = ENERGY_FULL, g->sick = 0, g->poops = 0;
        tama_tick(g);
    }
    assert(g->mistakes == 2);
    g->neglect = NEGLECT_MIN - 2;
    tama_tick(g);
    assert(!g->gone);
    tama_tick(g);
    assert(g->gone);
    int age = g->age;
    tama_tick(g);
    assert(g->age == age); /* then it stays gone, and Centre brings the next egg */
    int gen = g->generation;
    b.now = NOW, b.icon_down = 0, b.round = 0;
    input(&b, "k u 218");
    assert(!g->gone && g->stage == EGG && g->generation == gen + 1);
    /* its care shows in the adult it grows into */
    tama_new(g, NOW), g->stage = TEEN, g->age = stage_at[ADULT] - 1, g->discipline = 50, tama_tick(g);
    assert(g->stage == ADULT && g->form == 1);
    tama_new(g, NOW), g->stage = TEEN, g->age = stage_at[ADULT] - 1, g->mistakes = 5, tama_tick(g);
    assert(g->form == -1);
    /* off again: the toy, and on again it hasn't aged */
    b.sel = 2, pet_toggle(&b);
    assert(!b.on_pet);
    age = g->age;
    b.now += 86400, step(&b, 0.01f);
    pet_toggle(&b), step(&b, 0.01f);
    assert(b.on_pet && g->age == age);
    tama bad = *g;
    assert(tama_ok(&bad));
    bad.stage = 9;
    assert(!tama_ok(&bad));
    puts("ok");
    return 0;
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "/tmp/fidget";
    if (dir[0] == 'c') return check();
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
    static buddy b;
    init(&b, pal);
    b.now = NOW;
    char path[256];
    int pet = argc > 4 && atoi(argv[4]); /* preview.sh's "pet": its Tamagotchi session */
    static const struct { float at; const char *in; } pet_script[] = {
        { 0.5f, "k u 170" }, { 1.0f, "w -30" }, { 1.4f, "w -30" }, { 1.8f, "k u 218" }, { 2.3f, "k u 170" }, /* the menu: On */
        { 6.5f, "k u 218" },                                              /* Meal */
        { 8.0f, "w -30" }, { 8.5f, "k u 218" },                           /* Snack */
        { 10.0f, "w -30" }, { 10.5f, "k u 218" }, { 11.5f, "w -30" }, { 13.0f, "w 30" }, /* the game */
        { 14.5f, "t u 80 150" }, { 16.0f, "t u 300 150" }, { 17.5f, "t u 80 150" },
        { 19.5f, "w -30" }, { 19.9f, "w -30" }, { 20.3f, "k u 218" },     /* Medicine */
        { 20.9f, "k u 218" },
        { 22.0f, "w -30" }, { 22.5f, "k u 218" },                         /* Clean */
        { 24.0f, "w -30" }, { 24.5f, "k u 218" },                         /* Scold */
        { 26.0f, "w -30" }, { 26.5f, "k u 218" }, { 28.5f, "k u 218" },   /* Stats */
        { 29.5f, "w 90" }, { 30.0f, "w 30" }, { 30.5f, "k u 218" },       /* Light */
    };
    int frames = argc > 3 && atoi(argv[3]) > 0 ? atoi(argv[3]) : pet ? 30 * 34 : 30 * 23, si = 0;
    int ns = pet ? (int)(sizeof pet_script / sizeof *pet_script) : (int)(sizeof script / sizeof *script);
    for (int f = 0; f < frames; ++f) {
        float now = (float)f / 30;
        b.now = NOW + f / 30;
        for (; si < ns && (pet ? pet_script[si].at : script[si].at) <= now; ++si)
            input(&b, pet ? pet_script[si].in : script[si].in);
        if (pet) { /* its life, hurried along */
            if (f == 30 * 4) b.tm.at -= 60;                               /* hatches */
            if (f == 30 * 9) b.tm.at -= 60 * 60 * 40;                     /* grows */
            if (f == 30 * 9 + 1) b.tm.hunger = b.tm.fun = 3, b.tm.sick = b.tm.poops = b.tm.fuss = 0; /* and was looked after */
            if (f == 30 * 19) b.tm.sick = 2, b.tm.poops = 2;
            if (f == 30 * 23) b.tm.fuss = 20;
            if (f == 30 * 29) b.tm.energy = 100;
        } else {
            if (now > 18 && now < 19.5f) input(&b, "w -18"); /* spun: dizzy */
            if (f == 30 * 20) b.idle = SLEEP_S;          /* then left alone: asleep */
        }
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
#define SYS_rename 4038
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

static int now_s(void) {
    long ts[2];
    sc(SYS_clock_gettime, 0, (long)ts, 0, 0, 0, 0); /* CLOCK_REALTIME */
    return (int)ts[0];
}

/* Kept beside the binary: "<dir>/state", the colour's digit and Tamagotchi's; "<dir>/pet", the pet */
static char state_path[200], pet_path[200], pet_new[200];
static void paths(const char *argv0) {
    int n = 0, slash = -1;
    for (; argv0[n] && n < 180; ++n)
        if (argv0[n] == '/') slash = n;
    for (int i = 0; i <= slash; ++i) state_path[i] = pet_path[i] = pet_new[i] = argv0[i];
    put(state_path + slash + 1, "state");
    put(pet_path + slash + 1, "pet");
    put(pet_new + slash + 1, "pet.new");
}
static void load(buddy *b) {
    char c[2] = { 0, 0 };
    int fd = (int)sc(SYS_open, (long)state_path, O_CLOEXEC, 0, 0, 0, 0);
    if (fd >= 0) sc(SYS_read, fd, (long)c, 2, 0, 0, 0), sc(SYS_close, fd, 0, 0, 0, 0, 0);
    if (c[0] >= '0' && c[0] < '0' + PALS) b->pal = c[0] - '0';
    b->on_pet = c[1] == '1';
    long n = 0;
    fd = (int)sc(SYS_open, (long)pet_path, O_CLOEXEC, 0, 0, 0, 0);
    if (fd >= 0) n = sc(SYS_read, fd, (long)&b->tm, sizeof b->tm, 0, 0, 0), sc(SYS_close, fd, 0, 0, 0, 0, 0);
    if (n != (long)sizeof b->tm || !tama_ok(&b->tm)) b->tm = (tama){ 0 };
    if (b->on_pet && b->tm.magic != TAMA_MAGIC) tama_new(&b->tm, b->now);
}
static void save_state(const buddy *b) {
    char c[2] = { (char)('0' + b->pal), b->on_pet ? '1' : '0' };
    int fd = (int)sc(SYS_open, (long)state_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644, 0, 0, 0);
    if (fd >= 0) sc(SYS_write, fd, (long)c, 2, 0, 0, 0), sc(SYS_close, fd, 0, 0, 0, 0, 0);
}
/* whole or not at all: a TERM or a pulled battery mid-write keeps the last one */
static void save_pet(const tama *g) {
    int fd = (int)sc(SYS_open, (long)pet_new, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644, 0, 0, 0);
    if (fd < 0) return;
    long n = sc(SYS_write, fd, (long)g, sizeof *g, 0, 0, 0);
    sc(SYS_close, fd, 0, 0, 0, 0, 0);
    if (n == (long)sizeof *g) sc(SYS_rename, (long)pet_new, (long)pet_path, 0, 0, 0, 0);
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
    paths(argc > 0 ? argv[0] : "");
    init(&bud, 0);
    bud.now = now_s();
    load(&bud);
    int pal = bud.pal, on_pet = bud.on_pet;
    long long last = now_ms(), acc = 0, next = last, saved = last;
    while (!bud.quit) {
        long long t = now_ms();
        bud.now = now_s();
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
        /* now: a held Return's TERM saves nothing */
        if (bud.pal != pal || bud.on_pet != on_pet) pal = bud.pal, on_pet = bud.on_pet, save_state(&bud);
        if (bud.save || (bud.on_pet && t - saved > 5 * 60 * 1000)) bud.save = 0, saved = t, save_pet(&bud.tm);
    }
    if (bud.tm.magic == TAMA_MAGIC) save_pet(&bud.tm);
    if (var[5] != y0) var[5] = y0, sc(SYS_ioctl, fb, FBIOPAN_DISPLAY, (long)var, 0, 0, 0);
    sc(SYS_unlink, (long)INPUT_SOCK, 0, 0, 0, 0, 0);
    return 0;
}
#endif
