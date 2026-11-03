/* native_seam.h - services a game's native overrides may call.
 *
 * A game's [translate] overrides header is compiled into the translation as C,
 * so everything here has C linkage and C types. None of it names a game.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* The size, in points, of the display the program's window is shown on - the
 * area a fullscreen window fills. Non-zero with both set, or zero
 * when the host has no display to report. Safe from any guest thread. */
int host_display_screen_size(int *w, int *h);

/* Whether the picture is a phone's: the size of a hand, where an interface
 * drawn for a monitor reads small and a finger covers more of it. Goes by the
 * window's size, not the device: a foldable iPhone's opened screen is a
 * tablet's, its closed one a phone's, and the answer changes as it folds
 * (host_display_generation). Safe from any guest thread. */
int host_display_phone(void);

/* Whether the game lays its interface out for a phone (larger panels, the
 * game's own layout changes): where host_display_phone says so, unless the
 * launcher's setting host.ui/phone_interface says otherwise (0 never, 1 by
 * the screen, 2 always). Changes with host_display_phone. Safe from any guest
 * thread. */
int host_phone_layout(void);

/* A count that moves on whenever the display's size, its phone or tablet
 * form or the safe insets change (a rotation, a foldable opened or closed): a
 * layout made for one value is stale at another. Safe from any guest
 * thread. */
int host_display_generation(void);

/* The game's 3D picture of this frame is finished; what it draws until the
 * frame is presented is its interface. A host that draws the 3D at a lower
 * resolution (the render quality, upscaled by MetalFX) draws the rest at the
 * screen's own. Call it once a frame, between the two; nothing happens where
 * nobody upscales. Safe from the thread that draws. */
void host_render_scene_end(void);

/* The game's picture on screen in points (landscape: the longer side across),
 * for sizes a finger and an eye care about. Non-zero with both set, or zero
 * when not known. Safe from any guest thread. */
int host_display_picture_points(float *w, float *h);

/* The edges of the game's picture the system keeps for itself (a camera
 * housing, the home indicator), as fractions of the picture's width (left,
 * right) and height (top, bottom): what lies there may be hidden or out of a
 * finger's reach. Zero, with all four set to 0, when nothing is kept. Safe
 * from any guest thread. */
int host_display_safe_insets(float *left, float *top, float *right, float *bottom);

/* The host path a guest file would be written to in the writable overlay tier
 * (the player's profile). Zero, with nothing written, when there is no such
 * tier: a native override must never write into the game's own directory. */
int recomp_writable_path(const char *guest_path, char *out, size_t out_len);

/* The host path a guest file is read from, through every overlay tier and
 * then the game directory. Zero when the path cannot be resolved. */
int recomp_readable_path(const char *guest_path, char *out, size_t out_len);

/* A Ubisoft Game Service title's login (runtime/gs_login.h). When its server
 * list came from a community server the player chose, the password the game
 * would send for account `name` is replaced: *out gets the token (or the
 * challenge's registration or response) to send instead, and 1 comes back.
 * 0, with nothing written, when no chosen server is in play: the password
 * goes as the original game sends it. The password is never logged or kept.
 * Takes tens of milliseconds (scrypt). Safe from any guest thread. */
int recomp_gs_login_password(const char *name, const char *password, char *out, size_t out_len);
/* The router's answer to that LOGIN: 1 accepted, 0 refused. */
void recomp_gs_login_result(int accepted);

/* Vertex fetch (Direct3D 9 on a backend that has it): the vertices of one
 * vertex buffer that the game leaves unwritten are supplied, by vertex number,
 * from four buffers the game fills, through shader source it hands over in
 * the backend's language. In Metal Shading Language (D3D9_VFETCH_MSL) it defines
 *   bool d9_vfetch(uint vid, device const uint *b0, device const uint *b1,
 *                  device const uint *b2, constant uint *b3, thread uint *w)
 * and in Vulkan GLSL 4.50 (D3D9_VFETCH_GLSL), where the buffers are the arrays
 * uint d9_b0[] .. d9_b3[] and N is stride/4,
 *   bool d9_vfetch(uint vid, out uint w[N])
 * which returns false for a vertex the buffer itself holds, or writes the
 * vertex's stride/4 words to w. Every vertex shader drawing from that buffer
 * (stream 0, offset 0) runs it first.
 *   d3d9_vfetch_supported  0 when the backend cannot, else the language it
 *                          takes: D3D9_VFETCH_MSL or D3D9_VFETCH_GLSL;
 *   d3d9_vfetch_pool       picks the vertex buffer whose memory holds
 *                          guest_addr (from its Lock) and sets the source; check
 *                          non-zero also writes what was fetched to buffer 4
 *                          (stride/4 + 1 words a vertex: the words, the tag =
 *                          the presents so far + 1). Returns its vertex count;
 *   d3d9_vfetch_size       buffer k's size in bytes (0..3, 4 the check buffer);
 *   d3d9_vfetch_write      bytes into buffer k, in order with the draws; the
 *                          bytes must not be in use by a frame in flight;
 *   d3d9_vfetch_frames     presents so far;
 *   d3d9_vfetch_buffer     the vertex buffer it runs for, 0 once that one is
 *                          released (a new device): d3d9_vfetch_pool again
 *                          picks the next; buffers 0..4 keep their contents;
 *   d3d9_vfetch_check     calls visit with the check buffer and the newest
 *                          tag whose draws the GPU has finished, when one
 *                          has since the last call (waits for the render
 *                          thread). */
#define D3D9_VFETCH_MSL 1
#define D3D9_VFETCH_GLSL 2
int d3d9_vfetch_supported(void);
uint32_t d3d9_vfetch_pool(uint32_t guest_addr, uint32_t stride, const char *source, int check);
void d3d9_vfetch_size(uint32_t buffer, uint32_t bytes);
void d3d9_vfetch_write(uint32_t buffer, uint32_t offset, const void *bytes, uint32_t size);
uint64_t d3d9_vfetch_frames(void);
uint32_t d3d9_vfetch_buffer(void);
void d3d9_vfetch_check(void (*visit)(const uint32_t *words, uint32_t count, uint32_t done_tag, void *user),
                       void *user);

/* Direct3D 9 lazy buffers: the game is about to write bytes [offset, offset +
 * bytes) of the vertex or index buffer whose interface pointer is buffer (a
 * range it just allocated in its own pool, say). Under page protection their
 * pages are made writable and marked written now, as the first store's fault
 * would make them, without the fault. Only a hint: a range never written costs
 * the draws that read it a compare, and any store elsewhere still faults. */
void d3d9_prewrite(uint32_t buffer, uint32_t offset, uint32_t bytes);

/* Offer a DirectDraw display mode, as the host does for its own modes. */
int ddraw_add_mode(int w, int h, int bpp);

/* Optional game adapter for absolute touch placement. Coordinates and canvas
 * size are logical game pixels after the compositor's mapping. Called only
 * with the guest scheduler baton. Return non-zero only after placing a live
 * cursor; zero retains the host's existing fallback. Physical mouse motion
 * does not call this adapter. */
int recomp_pointer_place(int32_t x, int32_t y, int32_t width, int32_t height);

/* Optional game adapters for touch gestures (game.toml [touch] gestures =
 * "hover"). Both may be called from the host thread without the baton: they
 * only read or store plain words.
 *
 * recomp_touch_hover: what a game that shows information on hover should do
 * with its hover delay. RECOMP_TOUCH_HOVER_GAME: the game's own delay (a mouse,
 * a pencil). RECOMP_TOUCH_HOVER_NOW: show at once (a finger that rests).
 * RECOMP_TOUCH_HOVER_NEVER: not at all (a finger that just touched or tapped).
 *
 * recomp_touch_context: what lies under the game's cursor, as RECOMP_TOUCH_*
 * flags; zero when unknown, which the host treats as interface. */
enum {
    RECOMP_TOUCH_HOVER_GAME = -1,
    RECOMP_TOUCH_HOVER_NOW = 0,
    RECOMP_TOUCH_HOVER_NEVER = 1,
};
enum {
    RECOMP_TOUCH_WORLD = 1,  /* a scrolling 3D map: a drag pans, two fingers zoom and turn */
    RECOMP_TOUCH_BATTLE = 2, /* a battlefield: a tap previews, a second tap acts */
    RECOMP_TOUCH_MAP_SCREEN = 4, /* the 3D map's screen is on top with nothing modal
                                    over it, wherever the cursor is */
    RECOMP_TOUCH_TEXT = 8,  /* a text field: the system keyboard comes up */
    RECOMP_TOUCH_LIST = 16, /* a list: an up or down drag scrolls it (the wheel) */
    RECOMP_TOUCH_ZOOM_VIEW = 32, /* a camera that zooms and pans but does not turn is
                                    under the cursor with nothing modal (a battlefield):
                                    recomp_camera_gesture as on a map, its `turn`
                                    ignored */
    /* 64 was a town's zoom by small wheel steps, taken out (too jerky): a
     * town's camera has no operator to set, and its wheel stays the game's. */
    RECOMP_TOUCH_BATTLE_DIRECT = 128, /* with RECOMP_TOUCH_BATTLE: a tap on the field
                                         clicks at once, no preview first (the game
                                         confirms in its own way, e.g. choose a target,
                                         then the cell to strike it from) */
    RECOMP_TOUCH_DRAG_SPLIT = 256, /* the game drags something that a Shift held at
                                      the drop splits (an army's stack): a finger that
                                      rests during the drag holds Shift */
};
void recomp_touch_hover(int state);
int recomp_touch_context(void);

/* Optional game adapter: a touch gesture the game gives a meaning of its own,
 * in input order (under the guest scheduler baton). RECOMP_TOUCH_GESTURE_HOLD:
 * one finger rested on a battlefield that takes direct taps
 * (RECOMP_TOUCH_BATTLE_DIRECT) and lifted where the cursor now is. */
enum {
    RECOMP_TOUCH_GESTURE_HOLD = 1,
};
void recomp_touch_gesture(int gesture);

/* Optional game adapter: native touch (the launcher's "Touch: native",
 * host.touch/native). The host recognises a finger's gesture and hands it to
 * the game with its point; the game works out what lies there and acts, on
 * its own thread, with no cursor placed for it. Between gestures there is no
 * cursor at all: GetCursorPos answers a point far outside the window.
 *
 * recomp_touch_event(kind, phase, x, y, width, height, count): a gesture at
 * (x, y), game pixels of a width x height picture. Called under the guest
 * scheduler baton, in order with the input around it; the adapter applies it
 * on the game's own thread. Kind RECOMP_TOUCH_EVENT_QUERY only asks: non-zero
 * when the game takes native touch, zero leaves the hover gestures.
 *
 * Kinds: DOWN, a finger touched (the first; never a second finger): from then
 * on recomp_touch_context answers for this point; TAP, it lifted where it
 * touched, `count` 2 for the second tap of a double tap; PRESS, it rested
 * (BEGIN), moved while resting (MOVE), lifted (END); DRAG, it moved off the
 * point before resting, over something that is neither a camera nor a list:
 * BEGIN at the point it touched, MOVE, END where it lifted; RIGHT_TAP, two
 * fingers tapped together, at the second one; POINTER, a mouse, trackpad or
 * pencil moved the pointer: the game's own cursor again until the next DOWN.
 * Phase CANCEL ends a press or a drag the system took away. */
enum {
    RECOMP_TOUCH_EVENT_QUERY = 0,
    RECOMP_TOUCH_EVENT_DOWN = 1,
    RECOMP_TOUCH_EVENT_TAP = 2,
    RECOMP_TOUCH_EVENT_PRESS = 3,
    RECOMP_TOUCH_EVENT_DRAG = 4,
    RECOMP_TOUCH_EVENT_RIGHT_TAP = 5,
    RECOMP_TOUCH_EVENT_POINTER = 6,
};
enum {
    RECOMP_TOUCH_PHASE_BEGIN = 0,
    RECOMP_TOUCH_PHASE_MOVE = 1,
    RECOMP_TOUCH_PHASE_END = 2,
    RECOMP_TOUCH_PHASE_CANCEL = 3,
};
int recomp_touch_event(int kind, int phase, float x, float y, int width, int height, int count);

/* A key for a screen that waits for any key and takes no click (a movie, a
 * loading screen): native touch presses one there. `scancode` an SDL scancode,
 * `down` 1 or 0; it reaches the game as the keyboard's would, through the
 * host's input queue. Safe from any guest thread. */
void host_touch_key(int scancode, int down);

/* Optional game adapters for the camera of a 3D map (game.toml [touch]
 * gestures = "hover"). Called from the host thread; the adapter applies them
 * on the game's own thread.
 *
 * recomp_camera_step: one standard step, as the game's own wheel notch or turn
 * key makes it (the on-screen camera buttons).
 *
 * recomp_camera_gesture: a finger gesture on the map, in game pixels of a
 * width x height picture. Phase 0 begins it at (x, y), 1 moves it, 2 ends it;
 * `zoom` is the factor on the camera's distance since it began (below 1 is
 * nearer) and `turn` the radians it turned since then (positive: the fingers
 * turned counterclockwise). The ground that was under the first point stays
 * under (x, y). Phase -1 only asks: non-zero when the game takes gestures,
 * zero leaves the host's fallback (wheel-button drag, wheel, turn keys). */
enum {
    RECOMP_CAMERA_ZOOM_IN = 1,
    RECOMP_CAMERA_ZOOM_OUT = 2,
    RECOMP_CAMERA_TURN_LEFT = 3,
    RECOMP_CAMERA_TURN_RIGHT = 4,
};
void recomp_camera_step(int step);
int recomp_camera_gesture(int phase, float x, float y, float zoom, float turn, int width,
                          int height);

/* Optional game adapter: a precise scroll (a trackpad's two fingers) over the
 * game's interface at (x, y), `dy` game pixels of a width x height picture
 * the content moves (positive: down, its start coming into view). Non-zero
 * when the list under the point took it, which then follows the fingers;
 * zero leaves the host's fallback, notches of the game's wheel. Called under
 * the guest scheduler baton. */
int recomp_ui_scroll(float x, float y, float dy, int width, int height);

/* Optional game adapter: a screen of the game's interface the host draws
 * natively over the game (iOS: SwiftUI), while the game keeps the screen alive
 * and works it as before, without drawing it. The game describes the screen
 * showing now as one JSON document, {"screen": "<id>", ...} in its own words;
 * the host's view for that id draws it and answers with commands, JSON too,
 * {"do": "<verb>", ...}.
 *
 * recomp_screen_state: when the document changed since *serial, copies it into
 * buf (NUL-terminated), updates *serial and returns its length; 0 when it has
 * not changed; -1 when no native screen shows (the host takes its view away).
 * A document that does not fit leaves *serial and returns its length: call
 * again with a larger buf. Called from the host thread every turn.
 *
 * recomp_screen_command: a command from the native view; queued, the adapter
 * applies it on the game's own thread. Called from the host thread. */
int recomp_screen_state(char *buf, int cap, unsigned *serial);
void recomp_screen_command(const char *command);

/* Optional game adapter: the app is going to the background (iOS), where it
 * may be closed without ever reaching the game's own exit. A game that saves
 * its settings only on a clean exit saves them now. Called off the main thread
 * (the game's own thread may need it to finish a frame); may block for a few
 * seconds while that thread does the work. */
void recomp_app_background(void);

/* Optional native movie adapter. Focus arrives in input order under the guest
 * scheduler baton. Shutdown is called only once guest workers have stopped;
 * adapters must not re-enter guest code from that callback. */
void recomp_media_focus(int focused);
void recomp_media_shutdown(void);

/* Discard already sampled X/Y motion for a DirectInput mouse after absolute
 * placement. The argument is its guest interface address. Buttons, wheel and
 * keyboard input are retained. Requires the guest scheduler baton. */
void dinput_discard_mouse_motion(uint32_t device);

#ifdef __cplusplus
}
#endif
