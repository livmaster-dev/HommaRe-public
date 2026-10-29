/* native/camera.c - Heroes V's adventure-map camera for touch and the Mac trackpad (runtime/native_seam.h).
 * Addresses: design notes ios-touch-plan.md section 13 (camera, from static reading).
 *
 * The adventure screen's camera is a CCamera (vtable 0xFB0120) at screen+0xA4,
 * driven by a CCameraOperator (vtable 0xFB0560) at screen+0x564. Each frame
 * CCameraOperator::Update (007f26b0) turns the player's input into a desired
 * state at op+0x118, glides the camera's current state (cam+0x70) toward it -
 * about 175 ms for the distance, 110 ms for the angles and the target - clamps
 * the target to the map and hands the result to the camera. A state is
 *   { dist, pitch, yaw, roll, fov (degrees), znear, zfar, tx, ty, tz },
 * distances in world units (a tile is 2), angles in radians, z up.
 *
 * The replacement below runs the game's own update after one of two changes:
 *  - a gesture (one finger or two) sets the state outright, desired and current
 *    alike, so nothing glides behind the fingers: the distance and the yaw from
 *    the pinch and the turn, and the target so that the ground point that was
 *    under the gesture's first point stays under its current point;
 *  - a camera button sets only the desired state one step on, and the game's
 *    own glide animates it.
 * Both come from the host thread and are applied here, on the game's. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "native_seam.h"
#include "x86.h"
#include "platform/os.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifdef RECOMP_OVERRIDE_HEADER
#include RECOMP_OVERRIDE_HEADER
#endif
#ifdef RECOMP_ADDRESS_HEADER
#include RECOMP_ADDRESS_HEADER
#endif
/* The base game's addresses; an expansion's addresses.h names its own
 * (HOMM5_CAMERA_UPDATE_ORIG: the translated CCameraOperator::Update). */
#ifndef HOMM5_CAM_VTABLE
#define HOMM5_BASE_GAME 1
#define HOMM5_CAM_VTABLE 0x00fb0120
#define HOMM5_CAM_OPERATOR_VTABLE 0x00fb0560
#define HOMM5_SCREEN_CAMERA 0xa4
#define HOMM5_SCREEN_OPERATOR 0x564
#define HOMM5_VIRTUAL_SCREEN 0x010d2ce4
#define HOMM5_CAMERA_UPDATE_ORIG fn_007f26b0
#endif
#ifndef HOMM5_SCREEN_STACK
#define HOMM5_SCREEN_STACK 0x0114b2e4
#define HOMM5_VT_ADVENTURE 0x00fab2ac
#define HOMM5_SUB_ADVENTURE 0x7b4
#endif

/* The battle screen, whose camera only zooms, and where its CCameraOperator
 * sits in the screen object. In the base game the three makers of an operator
 * (callers of its constructor 007f0c30) are the map (007a7430, +0x564), a
 * 0x278-byte screen 007b9270 opens (007b7240, +0x184) and the battle (0074bcd0,
 * +0x4e4, seen in play: distance 10..42); each keeps its camera at +0xa4. The
 * town makes its camera without an operator (009153e0). An expansion's
 * addresses.h names its own (HOMM5_COMBAT_OPERATOR); without one there is no
 * battle zoom. */
#ifndef HOMM5_VT_COMBAT
#define HOMM5_VT_COMBAT 0x00fa79c8
#define HOMM5_SUB_COMBAT 0x658
#endif
#if HOMM5_BASE_GAME && !defined(HOMM5_COMBAT_OPERATOR)
#define HOMM5_COMBAT_OPERATOR 0x4e4
#endif

enum {
    CAM_VTABLE = HOMM5_CAM_VTABLE,
    OP_VTABLE = HOMM5_CAM_OPERATOR_VTABLE,
    SCREEN_CAMERA = HOMM5_SCREEN_CAMERA,     /* adventure screen object: its CCamera */
    SCREEN_OPERATOR = HOMM5_SCREEN_OPERATOR, /* ... and its CCameraOperator */
    VIRTUAL_SCREEN = HOMM5_VIRTUAL_SCREEN,   /* float width, height the camera projects onto */
    STACK = HOMM5_SCREEN_STACK,
    VT_MAP = HOMM5_VT_ADVENTURE,
    SUB_MAP = HOMM5_SUB_ADVENTURE,
    CAM_CURRENT = 0x70,   /* camera: the state drawn this frame */
    OP_CAMERA = 0x114,    /* operator: the camera it drives */
    OP_DESIRED = 0x118,   /* operator: the state it glides toward */
    OP_BOUNDS = 0x70,     /* operator: target min x, max x, min y, max y */
    OP_SOFT = 0x144,      /* operator: dist min/max, pitch min/max, yaw min/max */
    OP_FOLLOW = 0x190,    /* operator: following a hero (1), broken by input (2) */
};

/* Hor+ and the battle's placement phase, in every game's own native code.
 * Not weak symbols: an expansion's module would bind them to the base game's,
 * which the app it is loaded into also carries. */
#ifndef HOMM5_BASE_GAME
#define HOMM5_BASE_GAME 0
#endif
int homm5_horplus(void);        /* native/horplus.c */
int homm5_battle_placing(void); /* native/phone_hud.c */

typedef struct {
    float dist, pitch, yaw, roll, fov, znear, zfar, tx, ty, tz;
} CamState;

/* The battle's camera and the state its start was given (homm5_battle_camera),
 * and whether the placement phase's view was put in (-1: not yet). */
static uint32_t g_battle_cam;
static CamState g_battle_state;
static int g_battle_phase = -1;
/* What the placement view did to the operator's target and distance, to be
 * taken back when the battle begins (battle_phase), and the target it left. */
static int g_deployed;
static float g_deploy_dx, g_deploy_dy, g_deploy_zoom = 1.0f, g_deploy_tx, g_deploy_ty;

static void read_state(uint32_t at, CamState *s) {
    memcpy(s, g_mem + at, sizeof *s);
}
static void write_state(uint32_t at, const CamState *s) {
    memcpy(g_mem + at, s, sizeof *s);
}

/* ------------------------------------------------------------ host side -- */

/* The gesture the host reports, published whole under a sequence count (odd
 * while it is being written). `begun` counts gestures, so the game thread
 * knows a new one started even if it missed the frame in between. */
static struct {
    volatile uint32_t seq;
    int active, begun;
    float x0, y0, x, y, zoom, turn, w, h;
} g_gesture;
static volatile int g_steps[5]; /* RECOMP_CAMERA_* presses not yet applied */

int recomp_camera_gesture(int phase, float x, float y, float zoom, float turn, int width,
                          int height) {
    if (phase < 0)
        return 1; /* asked whether gestures are taken: they are */
    if (phase != 1 && getenv("RECOMP_TRACE_CAMERA"))
        fprintf(stderr, "[camera] host gesture phase %d at %.0f,%.0f of %dx%d\n", phase, x, y,
                width, height);
    __atomic_add_fetch(&g_gesture.seq, 1, __ATOMIC_ACQ_REL);
    if (phase == 0) {
        g_gesture.begun++;
        g_gesture.x0 = x;
        g_gesture.y0 = y;
    }
    g_gesture.active = phase != 2;
    g_gesture.x = x;
    g_gesture.y = y;
    g_gesture.zoom = zoom;
    g_gesture.turn = turn;
    g_gesture.w = (float)width;
    g_gesture.h = (float)height;
    __atomic_add_fetch(&g_gesture.seq, 1, __ATOMIC_ACQ_REL);
    return 1;
}

void recomp_camera_step(int step) {
    if (step >= 1 && step <= 4)
        __atomic_add_fetch(&g_steps[step], 1, __ATOMIC_ACQ_REL);
}

/* ------------------------------------------------------------ game side -- */

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

/* Where the view ray through virtual-screen point (cx, cy) meets the plane
 * z = h, for camera state s. Zero when the ray runs parallel or away. */
static int ground_point(const CamState *s, double cx, double cy, double vw, double vh, double h,
                        double *px, double *py) {
    const double cp = cos(s->pitch), sp = sin(s->pitch), cy_ = cos(s->yaw), sy = sin(s->yaw);
    const double F[3] = {-sy * cp, cy_ * cp, sp}; /* forward */
    const double R[3] = {cy_, sy, 0};            /* right */
    const double U[3] = {R[1] * F[2] - R[2] * F[1], R[2] * F[0] - R[0] * F[2],
                         R[0] * F[1] - R[1] * F[0]}; /* up = R x F */
    /* The engine's projection: the horizontal angle on screens wider than
     * 4:3, unless native/widescreen.c gives them a 4:3 screen's vertical one. */
    const double t = tan(s->fov * M_PI / 360.0), a = vh / vw;
    const int keep_width = a <= 0.75 && !homm5_horplus();
    const double kx = keep_width ? t : 0.75 * t / a, ky = keep_width ? t * a : 0.75 * t;
    const double nx = 2 * cx / vw - 1, ny = 1 - 2 * cy / vh;
    double d[3], e[3];
    for (int i = 0; i < 3; ++i) {
        d[i] = F[i] + nx * kx * R[i] + ny * ky * U[i];
        e[i] = (i == 0 ? s->tx : i == 1 ? s->ty : s->tz) - s->dist * F[i];
    }
    if (d[2] > -1e-6)
        return 0;
    const double k = (h - e[2]) / d[2];
    *px = e[0] + d[0] * k;
    *py = e[1] + d[1] * k;
    return 1;
}

static int trace_camera(void) {
    static int trace = -1;
    if (trace < 0)
        trace = getenv("RECOMP_TRACE_CAMERA") != NULL;
    return trace;
}

/* The adventure screen's operator and camera, when `op` is that operator. */
static uint32_t map_camera(uint32_t op) {
    const uint32_t sentinel = rd32(STACK);
    if (!sentinel || rd32(sentinel) == sentinel)
        return 0;
    const uint32_t last = rd32(sentinel + 4u), top = last ? rd32(last + 8u) : 0;
    if (!top || rd32(top) != VT_MAP) {
        static uint32_t told;
        if (trace_camera() && told != (top ? rd32(top) : 1)) {
            told = top ? rd32(top) : 1;
            fprintf(stderr, "[camera] top screen %08x (vt %08x), not the map\n", top, told);
        }
        return 0;
    }
    const uint32_t base = top - SUB_MAP, cam = rd32(base + SCREEN_CAMERA);
    if (rd32(base + SCREEN_OPERATOR) != op || rd32(op) != OP_VTABLE || !cam ||
        rd32(cam) != CAM_VTABLE || rd32(op + OP_CAMERA) != cam) {
        static uint32_t told;
        if (trace_camera() && told != op) {
            told = op;
            fprintf(stderr,
                    "[camera] not the map's: op %08x (vt %08x) screen op %08x cam %08x (vt %08x) "
                    "op cam %08x\n",
                    op, rd32(op), rd32(base + SCREEN_OPERATOR), cam, cam ? rd32(cam) : 0,
                    rd32(op + OP_CAMERA));
        }
        return 0;
    }
    return cam;
}


/* The camera of the screen on top when it only zooms (a battle) and
 * `op` is that screen's operator. */
static uint32_t zoom_view_camera(uint32_t op) {
#ifdef HOMM5_COMBAT_OPERATOR
    static const struct {
        uint32_t vt, sub, op;
        const char *name;
    } views[] = {{HOMM5_VT_COMBAT, HOMM5_SUB_COMBAT, HOMM5_COMBAT_OPERATOR, "battle"}};
    const uint32_t sentinel = rd32(STACK);
    if (!sentinel || rd32(sentinel) == sentinel)
        return 0;
    const uint32_t last = rd32(sentinel + 4u), top = last ? rd32(last + 8u) : 0;
    for (size_t i = 0; top && i < sizeof views / sizeof views[0]; ++i) {
        if (rd32(top) != views[i].vt)
            continue;
        const uint32_t screen = top - views[i].sub, cam = rd32(op + OP_CAMERA);
        if (rd32(screen + views[i].op) == op && rd32(op) == OP_VTABLE && cam)
            return cam;
        static uint32_t told;
        if (trace_camera() && told != op) {
            told = op;
            fprintf(stderr, "[camera] %s on top: op %08x (vt %08x) is not its +0x%x (%08x)\n",
                    views[i].name, op, rd32(op), views[i].op, rd32(screen + views[i].op));
        }
        return 0;
    }
#else
    (void)op;
#endif
    return 0;
}

/* One press: the desired state a step on; the game's glide does the rest. */
static void apply_steps(uint32_t op) {
    for (int step = 1; step <= 4; ++step) {
        int n = __atomic_exchange_n(&g_steps[step], 0, __ATOMIC_ACQ_REL);
        for (; n > 0; --n) {
            CamState s;
            read_state(op + OP_DESIRED, &s);
            const float lo = rdf32(op + OP_SOFT), hi = rdf32(op + OP_SOFT + 4);
            if (step == 1)
                s.dist = clampf(s.dist * 0.8f, lo, hi);
            else if (step == 2)
                s.dist = clampf(s.dist / 0.8f, lo, hi);
            else
                s.yaw += (float)(step == 3 ? -M_PI / 4 : M_PI / 4); /* 3: counterclockwise */
            write_state(op + OP_DESIRED, &s);
            if (trace_camera())
                fprintf(stderr, "[camera] step %d: dist %.2f yaw %.3f\n", step, s.dist, s.yaw);
        }
    }
}

/* zoom_only: the battle's camera, which keeps its turn: the distance changes
 * and the ground under the gesture's first point stays under its current one
 * (zoom to the cursor, and a pan when the point moves), the target within the
 * operator's bounds. */
static void apply_gesture(uint32_t op, uint32_t cam, int zoom_only) {
    static int seen_begun = -1, anchored = 0;
    static CamState start;
    static double ax, ay, az;
    float x0, y0, x, y, zoom, turn, w, h;
    int active, begun;
    uint32_t seq;
    do {
        seq = __atomic_load_n(&g_gesture.seq, __ATOMIC_ACQUIRE);
        active = g_gesture.active;
        begun = g_gesture.begun;
        x0 = g_gesture.x0;
        y0 = g_gesture.y0;
        x = g_gesture.x;
        y = g_gesture.y;
        zoom = g_gesture.zoom;
        turn = g_gesture.turn;
        w = g_gesture.w;
        h = g_gesture.h;
    } while ((seq & 1) || seq != __atomic_load_n(&g_gesture.seq, __ATOMIC_ACQUIRE));
    if (!active || w <= 0 || h <= 0)
        return;
    double vw = rdf32(VIRTUAL_SCREEN), vh = rdf32(VIRTUAL_SCREEN + 4);
#ifdef HOMM5_CAMERA_PICTURE_SHAPE
    /* Hammers of Fate: the virtual size is always 1024x768, the 3D picture
     * the screen's own shape (native/hof_screen.c). */
    vw = w;
    vh = h;
#endif
    if (!(vw >= 320 && vw <= 16384 && vh >= 240 && vh <= 16384)) {
        /* Not a screen size (a wrong address): the picture's own shape keeps
         * both axes right, which the projection's aspect depends on. */
        static int told;
        if (trace_camera() && !told++)
            fprintf(stderr, "[camera] virtual screen reads %g x %g; using the picture's %gx%g\n", vw,
                    vh, w, h);
        vw = w;
        vh = h;
    }
    const double sx = vw / w, sy = vh / h;
    if (begun != seen_begun) {
        /* A new gesture: the ground under its first point, by the state drawn now. */
        seen_begun = begun;
        read_state(cam + CAM_CURRENT, &start);
        az = start.tz;
        anchored = ground_point(&start, x0 * sx, y0 * sy, vw, vh, az, &ax, &ay);
        if (zoom_only && trace_camera())
            fprintf(stderr,
                    "[camera] zoom-only gesture: dist %.2f (limits %.2f..%.2f) pitch %.3f (%.3f..%.3f) "
                    "yaw %.3f (%.3f..%.3f) target bounds %.1f..%.1f, %.1f..%.1f\n",
                    start.dist, rdf32(op + OP_SOFT), rdf32(op + OP_SOFT + 4), start.pitch,
                    rdf32(op + OP_SOFT + 8), rdf32(op + OP_SOFT + 12), start.yaw,
                    rdf32(op + OP_SOFT + 16), rdf32(op + OP_SOFT + 20), rdf32(op + OP_BOUNDS),
                    rdf32(op + OP_BOUNDS + 4), rdf32(op + OP_BOUNDS + 8), rdf32(op + OP_BOUNDS + 12));
        if (trace_camera())
            fprintf(stderr,
                    "[camera] gesture at %.0f,%.0f of %.0fx%.0f (virtual %.0fx%.0f): ground "
                    "%.2f,%.2f,%.2f (%s); dist %.2f pitch %.3f yaw %.3f fov %.1f target %.2f,%.2f,%.2f\n",
                    x0, y0, w, h, vw, vh, ax, ay, az, anchored ? "anchored" : "no ground", start.dist,
                    start.pitch, start.yaw, start.fov, start.tx, start.ty, start.tz);
    }
    CamState s;
    read_state(cam + CAM_CURRENT, &s);
    float dlo = rdf32(op + OP_SOFT), dhi = rdf32(op + OP_SOFT + 4);
    if (!(dhi > dlo && dlo > 0)) { /* an operator with no limits set */
        dlo = 1.0f;
        dhi = 500.0f;
    }
    s.dist = clampf(start.dist * zoom, dlo, dhi);
    /* Past a limit the start moves along, so a pinch or a scroll that turns
     * back answers at once instead of first unwinding what the limit ate. */
    if (zoom > 0 && s.dist != start.dist * zoom)
        start.dist = s.dist / zoom;
    const float ylo = rdf32(op + OP_SOFT + 16), yhi = rdf32(op + OP_SOFT + 20);
    /* Free yaw; a limited one (classic mode) does not turn. A larger yaw turns
     * the picture clockwise (measured), so the fingers' counterclockwise is less. */
    if (ylo == 0 && yhi == 0 && !zoom_only)
        s.yaw = start.yaw - turn;
    if (anchored) {
        /* Move the target until the anchor is under the current point again; two
         * passes settle it (the plane is flat, the error shrinks fast). */
        for (int pass = 0; pass < 3; ++pass) {
            double px, py;
            if (!ground_point(&s, x * sx, y * sy, vw, vh, az, &px, &py))
                break;
            s.tx += (float)(ax - px);
            s.ty += (float)(ay - py);
        }
        /* Within the operator's bounds, when it has them (a battle's: the
         * arena with a margin, 007e9460); an empty pair is left alone rather
         * than pinning the target to it. */
        const float bx0 = rdf32(op + OP_BOUNDS), bx1 = rdf32(op + OP_BOUNDS + 4);
        const float by0 = rdf32(op + OP_BOUNDS + 8), by1 = rdf32(op + OP_BOUNDS + 12);
        if (bx1 > bx0)
            s.tx = clampf(s.tx, bx0, bx1);
        if (by1 > by0)
            s.ty = clampf(s.ty, by0, by1);
    }
    if (!zoom_only && rd32(op + OP_FOLLOW) == 1)
        wr32(op + OP_FOLLOW, 2); /* the fingers took over from the hero's camera */
    write_state(op + OP_DESIRED, &s);
    write_state(cam + CAM_CURRENT, &s);
    /* A zoom during the placement moves the target too: battle_phase takes
     * its own move back from where the player left it, not from the start. */
    if (zoom_only && g_deployed) {
        g_deploy_tx = s.tx;
        g_deploy_ty = s.ty;
    }
}

void HOMM5_CAMERA_UPDATE_ORIG(X86 *c);

/* When the map's camera last ran its frame, and since when it has run without
 * a break. The adventure screen is on top of the screen stack while a map is
 * still loading behind the loading screen; its camera runs every frame only
 * once the map is drawn (and once, alone, during the load). */
static volatile int64_t g_map_frame_ms, g_map_since_ms;

static int64_t now_ms(void) {
    return (int64_t)(os_monotonic_ns() / 1000000);
}

/* When a zoom-only camera last ran its frame (zoom_view_camera found it). */
static volatile int64_t g_zoom_frame_ms;

int homm5_zoom_camera_live(void) {
    const int64_t last = __atomic_load_n(&g_zoom_frame_ms, __ATOMIC_RELAXED);
    return last && now_ms() - last < 300;
}

/* How far out the map's camera is, 0 at its nearest and 1 at its farthest
 * (the operator's own distance limits), as of its last frame. */
static volatile float g_map_zoom = -1.0f;
float homm5_map_zoom(void) {
    extern int homm5_map_camera_live(void);
    return homm5_map_camera_live() ? g_map_zoom : -1.0f; /* both on the guest thread */
}

int homm5_map_camera_live(void) {
    const int64_t last = __atomic_load_n(&g_map_frame_ms, __ATOMIC_RELAXED);
    const int64_t since = __atomic_load_n(&g_map_since_ms, __ATOMIC_RELAXED);
    return last && now_ms() - last < 300 && last - since >= 250;
}

/* The placement phase on a phone: the troops are placed on the field's near
 * rows, which the nearer camera leaves at the bottom of the picture, under
 * the placement bar. The placement's view looks along the field from the
 * player's side (90 degrees from the battle's), so when the placement bar
 * shows, the camera moves back from where that view looks, along the ground
 * at its own height (RECOMP_BATTLE_DEPLOY_LIFT, a share of the distance;
 * RECOMP_BATTLE_DEPLOY_ZOOM on the distance). When the battle begins the game
 * turns the camera to the battle's view but leaves the target where it is, so
 * that move is taken back: along the placement's yaw, 90 degrees from the
 * battle's, it pushed the field sideways (2026-10-20, iPhone: empty field left
 * of the hero, the enemy under the right-hand buttons). */
static float env_float(const char *name, float fallback);

static void battle_phase(uint32_t op) {
    if (!host_display_phone() && !getenv("RECOMP_BATTLE_ZOOM"))
        return;
    const int placing = homm5_battle_placing();
    if (placing == g_battle_phase)
        return;
    g_battle_phase = placing;
    if (!placing) {
        if (!g_deployed)
            return;
        g_deployed = 0;
        CamState s;
        read_state(op + OP_DESIRED, &s);
        /* Only from where the placement left it: a target the game moved on
         * its own (half the move away or more) is the game's. */
        const float off = hypotf(s.tx - g_deploy_tx, s.ty - g_deploy_ty);
        const float moved = hypotf(g_deploy_dx, g_deploy_dy);
        const int back = off < 0.5f * moved;
        if (back) {
            s.tx -= g_deploy_dx;
            s.ty -= g_deploy_dy;
            if (g_deploy_zoom != 1.0f && g_deploy_zoom > 0)
                s.dist /= g_deploy_zoom;
            write_state(op + OP_DESIRED, &s);
        }
        if (trace_camera())
            fprintf(stderr, "[camera] battle begins: target %s by %.1f,%.1f -> %.1f,%.1f (dist %.1f yaw %.2f)\n",
                    back ? "back" : "left (the game moved it)", g_deploy_dx, g_deploy_dy, s.tx, s.ty,
                    s.dist, s.yaw);
        return;
    }
    const float lift = clampf(env_float("RECOMP_BATTLE_DEPLOY_LIFT", 0.15f), -0.5f, 0.5f);
    const float zoom = clampf(env_float("RECOMP_BATTLE_DEPLOY_ZOOM", 1.0f), 0.3f, 2.0f);
    CamState s;
    read_state(op + OP_DESIRED, &s);
    if (!(s.dist > 5 && s.dist < 200))
        return;
    /* Toward the eye along the ground, F's ground part (-sin yaw, cos yaw)
     * of this view's own yaw (ground_point above). */
    const float shift = lift * s.dist;
    g_deploy_dx = shift * sinf(s.yaw);
    g_deploy_dy = -shift * cosf(s.yaw);
    g_deploy_zoom = zoom;
    s.dist *= zoom;
    s.tx += g_deploy_dx;
    s.ty += g_deploy_dy;
    g_deploy_tx = s.tx;
    g_deploy_ty = s.ty;
    g_deployed = g_deploy_dx != 0 || g_deploy_dy != 0 || zoom != 1.0f;
    write_state(op + OP_DESIRED, &s);
    if (trace_camera())
        fprintf(stderr, "[camera] battle placement: dist %.1f, target %.1f,%.1f (yaw %.2f)\n",
                s.dist, s.tx, s.ty, s.yaw);
}

/* CCameraOperator::Update (thiscall: ECX the operator; camera, time on the stack). */
void homm5_camera_update(X86 *c) {
    const uint32_t op = c->r[R_ECX];
    static int first = 1;
    if (first && trace_camera()) {
        first = 0;
        fprintf(stderr, "[camera] operator update runs (op %08x)\n", op);
    }
    const uint32_t cam = map_camera(op);
    if (cam) {
        const int64_t now = now_ms();
        if (now - __atomic_load_n(&g_map_frame_ms, __ATOMIC_RELAXED) >= 300)
            __atomic_store_n(&g_map_since_ms, now, __ATOMIC_RELAXED); /* a new run */
        __atomic_store_n(&g_map_frame_ms, now, __ATOMIC_RELAXED);
        {
            CamState s;
            read_state(op + OP_DESIRED, &s);
            const float lo = rdf32(op + OP_SOFT), hi = rdf32(op + OP_SOFT + 4);
            const float z = hi > lo ? (s.dist - lo) / (hi - lo) : 0.0f;
            g_map_zoom = z < 0 ? 0.0f : z > 1 ? 1.0f : z;
        }
        apply_steps(op);
        apply_gesture(op, cam, 0);
    } else {
        /* A battle's camera: the phone's placement view (battle_phase), and
         * the trackpad's or fingers' zoom where only zooming is allowed. */
        if (g_battle_cam && rd32(op + OP_CAMERA) == g_battle_cam)
            battle_phase(op);
        if (zoom_view_camera(op)) {
            __atomic_store_n(&g_zoom_frame_ms, now_ms(), __ATOMIC_RELAXED);
            apply_gesture(op, rd32(op + OP_CAMERA), 1);
        }
    }
    HOMM5_CAMERA_UPDATE_ORIG(c);
}

/* ------------------------------------------------------- battle camera -- */

/* The battle's camera on a phone. CArenaCameraConfigurator's slot 8 (the base
 * game's 007e9340; thiscall, the CCamera on the stack, RET 4) gives a battle's
 * camera its starting state: a distance by the arena's size, pitch -45 degrees,
 * the target at the arena's centre. It sets the camera's state (cam+0x70) and
 * the one it returns to (cam+0x174) alike. Made for a monitor, that view leaves
 * the field small on a phone; this brings the camera nearer, at the same
 * pitch, and can move the target along the ground (lift: toward the camera
 * raises the field on the screen, away from it shows more of the far side,
 * where the enemy's hero stands). Clicks on the field follow: they are picked
 * through the same camera. Two places in the base game ask for it: the battle
 * screen as it opens (call returning to 0074bdbd) and an object 0076bfc0 builds
 * (to 0076ca18, by its size the fly-by); RECOMP_TRACE_CAMERA logs which. */
#ifndef HOMM5_BATTLE_DEFAULTS_ORIG
#define HOMM5_BATTLE_DEFAULTS_ORIG fn_007e9340
#endif
void HOMM5_BATTLE_DEFAULTS_ORIG(X86 *c);

enum { CAM_HOME = 0x174 }; /* camera: the state a camera reset returns to */


static float env_float(const char *name, float fallback) {
    const char *v = getenv(name);
    return v && *v ? (float)atof(v) : fallback;
}

void homm5_battle_camera(X86 *c) {
    const uint32_t ret = rd32(c->r[R_ESP]), cam = rd32(c->r[R_ESP] + 4);
    HOMM5_BATTLE_DEFAULTS_ORIG(c);
    if (!host_display_phone() && !getenv("RECOMP_BATTLE_ZOOM"))
        return;
    /* RECOMP_BATTLE_ZOOM: the factor on the distance; RECOMP_BATTLE_LIFT: how
     * far the target moves toward the camera, as a share of the distance
     * (negative: away from it). At 0.1 the enemy's hero was cut at the top
     * with room left above the turn order bar (2026-10-19, iPhone). */
    const float zoom = clampf(env_float("RECOMP_BATTLE_ZOOM", 0.67f), 0.3f, 1.5f);
    const float lift = clampf(env_float("RECOMP_BATTLE_LIFT", 0.0f), -0.5f, 0.5f);
    if (!cam)
        return;
    CamState s;
    read_state(cam + CAM_CURRENT, &s);
    if (!(s.dist > 5 && s.dist < 200) || !(s.pitch > -1.6f && s.pitch < 0))
        return; /* not the start this was written for */
    const CamState was = s;
    const float shift = lift * s.dist; /* world units, as the view scales with distance */
    s.dist *= zoom;
    /* Toward the camera along the ground: the eye sits at target - dist * F,
     * F's ground part (-sin yaw, cos yaw) (ground_point above). */
    s.tx += shift * sinf(s.yaw);
    s.ty -= shift * cosf(s.yaw);
    write_state(cam + CAM_CURRENT, &s);
    write_state(cam + CAM_HOME, &s);
    g_battle_cam = cam;
    g_battle_state = s;
    g_battle_phase = -1;
    g_deployed = 0;
    if (trace_camera())
        fprintf(stderr,
                "[camera] battle start (from %08x): dist %.1f -> %.1f, target %.1f,%.1f -> "
                "%.1f,%.1f (yaw %.2f pitch %.2f)\n",
                ret, was.dist, s.dist, was.tx, was.ty, s.tx, s.ty, s.yaw, s.pitch);
}

/* ------------------------------------------------- animation thinning -- */

#ifndef HOMM5_ANIM_NEED_UPDATE_ORIG
#define HOMM5_ANIM_NEED_UPDATE_ORIG fn_0067c060
#endif
void HOMM5_ANIM_NEED_UPDATE_ORIG(X86 *c);

/* Off by default (2026-10-20, the user's rule: speed comes from our code, not
 * from doing less than the original): models animate every frame as in the
 * game. RECOMP_HOMM5_ANIM_LOD=1 turns the thinning below on.
 *
 * NeedUpdate of NAnimation::CSkeletonAnimator (the base game's 0067c060;
 * thiscall, ECX the animator, RET): yes while the animation clock moved and a
 * control still has time left. A yes makes the dataflow sample the pose
 * (GrannySampleModelAnimations, a quarter of the zoomed-out map's frame) and
 * redo everything downstream: the bone palette, the skinned positions and
 * bounds, the vertex buffer with its per-vertex lighting, its upload.
 *
 * With the adventure map's camera past RECOMP_HOMM5_ANIM_LOD_FROM of its zoom
 * range (0.5), each animator samples one presented frame in
 * RECOMP_HOMM5_ANIM_LOD_EVERY (2): its turn comes by its address, so half of
 * them sample on even frames and half on odd. A no only defers: the fields the
 * original updates as it answers (the input versions at +0x94 and +0x9c, the
 * one-shot flag at +0x139) are put back, so the next frame sees the same change
 * and says yes. The clock is time-based, so the animation keeps its pace.
 *
 * The turn counts presented frames and each animator remembers when it last
 * sampled: one that has gone EVERY frames (or 50 ms) without a sample gets one
 * whatever its turn. The first version took turns by the map camera's updates,
 * which do not come once a frame (about 45 in 116 frames in a ToE game): a
 * model then lost several turns in a row, and a walking hero far out stood
 * still and caught up in jumps at every step.
 *   RECOMP_HOMM5_ANIM_LOD=0 off (default), 1 on, ab alternates every 10 s;
 *   RECOMP_HOMM5_ANIM_LOD_EVERY=N: one sample in N frames (2..8, default 2);
 *   RECOMP_HOMM5_ANIM_LOD_STATS=1 logs, every 2 s, how many animators asked
 *   in the last second went more than EVERY frames without a sample. */
#define ANIM_SLOTS 4096u
typedef struct {
    uint32_t self, asked, sampled;
    int64_t sampled_ms;
} AnimSlot;
static AnimSlot g_anim_seen[ANIM_SLOTS];

/* The animator's slot (taking over one not asked for 600 frames), or NULL. */
static AnimSlot *anim_slot(uint32_t self, uint32_t frame) {
    uint32_t h = (self >> 4) & (ANIM_SLOTS - 1u);
    for (uint32_t k = 0; k < 8; ++k, h = (h + 1u) & (ANIM_SLOTS - 1u)) {
        if (g_anim_seen[h].self == self)
            return &g_anim_seen[h];
        if (!g_anim_seen[h].self || frame - g_anim_seen[h].asked > 600u) {
            g_anim_seen[h].self = self;
            g_anim_seen[h].asked = g_anim_seen[h].sampled = frame;
            g_anim_seen[h].sampled_ms = 0;
            return &g_anim_seen[h];
        }
    }
    return NULL;
}

void homm5_anim_need_update(X86 *c) {
    extern uint32_t host_present_count(void);
    static int mode = -1, stats = 0; /* mode: 0 off, 1 on, 2 alternate */
    static float from = 0.5f;
    static uint32_t every = 2;
    if (mode < 0) {
        const char *v = getenv("RECOMP_HOMM5_ANIM_LOD");
        mode = !v || !*v ? 0 : v[0] == '1' ? 1 : v[0] == 'a' ? 2 : 0;
        const char *f = getenv("RECOMP_HOMM5_ANIM_LOD_FROM");
        if (f && *f)
            from = (float)atof(f);
        const char *e = getenv("RECOMP_HOMM5_ANIM_LOD_EVERY");
        if (e && atoi(e) >= 2 && atoi(e) <= 8)
            every = (uint32_t)atoi(e);
        const char *st = getenv("RECOMP_HOMM5_ANIM_LOD_STATS");
        stats = st && st[0] == '1';
        fprintf(stderr, "[anim-lod] mode %d from zoom %.2f, every %u frames\n", mode, from, every);
    }
    int on = mode;
    if (mode == 2) {
        static int shown = -1;
        on = (int)((now_ms() / 10000) & 1);
        if (on != shown) {
            shown = on;
            fprintf(stderr, "[anim-lod] %s\n", on ? "on" : "off");
        }
    }
    const uint32_t self = c->r[1];
    if (!on && !stats) {
        HOMM5_ANIM_NEED_UPDATE_ORIG(c);
        return;
    }
    const uint32_t frame = host_present_count();
    const int64_t now = now_ms();
    AnimSlot *slot = anim_slot(self, frame);
    int thin = 0;
    if (on && slot && homm5_map_zoom() >= from) {
        const int turn = (frame + (self >> 6)) % every == 0u;
        const int overdue = frame - slot->sampled >= every || now - slot->sampled_ms >= 50;
        thin = !turn && !overdue;
    }
    uint32_t v94 = 0, v9c = 0;
    uint8_t f139 = 0;
    if (thin) {
        v94 = rd32(self + 0x94u);
        v9c = rd32(self + 0x9cu);
        f139 = rd8(self + 0x139u);
    }
    HOMM5_ANIM_NEED_UPDATE_ORIG(c);
    const int yes = (c->r[0] & 0xffu) != 0;
    if (thin && yes) {
        wr32(self + 0x94u, v94);
        wr32(self + 0x9cu, v9c);
        wr8(self + 0x139u, f139);
        c->r[0] &= 0xffffff00u;
    } else if (slot) {
        slot->sampled = frame; /* sampled now, or nothing to sample: not a freeze */
        slot->sampled_ms = now;
    }
    if (slot)
        slot->asked = frame;
    if (stats) {
        static int64_t last;
        if (now - last >= 2000) {
            last = now;
            uint32_t live = 0, stale = 0;
            for (uint32_t i = 0; i < ANIM_SLOTS; ++i) {
                if (!g_anim_seen[i].self || frame - g_anim_seen[i].asked > 60u)
                    continue;
                ++live;
                if (g_anim_seen[i].asked - g_anim_seen[i].sampled > every)
                    ++stale;
            }
            fprintf(stderr, "[anim-lod] frame %u zoom %.2f: %u animators asked, %u went more than %u frames "
                            "without a sample\n",
                    frame, homm5_map_zoom(), live, stale, every);
        }
    }
}
