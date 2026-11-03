// input_touch.h - fingers become the mouse and keys the game already reads.
//
// A pure gesture state machine: it sees finger down/motion/up in window points
// plus a clock, and emits the mouse motion, mouse buttons and key taps that
// the host's existing input path consumes. No SDL calls, so the tests drive
// it with plain numbers. The gesture table is in the M1 iOS design spec.
#pragma once
#include <stdint.h>

#include <vector>

// Gesture thresholds, in window points and nanoseconds.
constexpr double kTouchTapTravel = 20.0; // a resting finger drifts a few points
constexpr uint64_t kTouchLongPressNs = 350ull * 1000000ull;
constexpr double kTouchPanStep = 24.0;
// Points of finger travel per wheel notch when a drag scrolls a list.
constexpr double kTouchScrollStep = 22.0;
// Place a tap before pressing so a frame can sample the new cursor position.
// Without presented-frame counts, wait this long before emitting the press.
constexpr uint64_t kTouchPressDelayNs = 60ull * 1000000ull;
// A synthesized click stays pressed this long: a game that samples its mouse
// buttons once per frame never sees a press and release inside one frame.
constexpr uint64_t kTouchClickHoldNs = 90ull * 1000000ull;
// And, once the host reports presented frames, for this many of them: the
// clock alone says nothing about when the game last sampled its buttons, and
// a game presenting at 15 frames a second can miss a 90 ms press entirely.
// The press must be seen by one sample and the release by a later one.
constexpr uint32_t kTouchClickHoldFrames = 2;
// A game that stops presenting still gets its release after this long.
constexpr uint64_t kTouchClickHoldMaxNs = 400ull * 1000000ull;
// A held or dragging finger this close to a side is placed exactly on it: games
// scroll when the cursor sits on the outermost row or column, which a finger
// on a bezel never quite reaches. When such a hold ends the cursor is moved
// back inside by kTouchEdgeRelease so the scrolling stops with the finger.
constexpr double kTouchEdgeMargin = 16.0;
constexpr double kTouchEdgeRelease = 48.0;
// Hover gestures: a second finger that lands this long after the first one is
// a tap beside a resting finger (a right click where the first one rests), not
// the start of a two-finger tap, and counts as a tap only if it lifts within
// kTouchSecondTapNs.
constexpr uint64_t kTouchAnchorNs = 120ull * 1000000ull;
constexpr uint64_t kTouchSecondTapNs = 300ull * 1000000ull;
// A hover pan's virtual pointer goes back to the middle within this of an edge.
constexpr double kTouchPanMargin = 64.0;
// Hover: frames the game must present after a touch placed its cursor before
// what it reports under the cursor (the gesture context) describes that point.
constexpr uint32_t kTouchContextFrames = 2;
// A tap this soon after and this close to the last left click lands exactly on
// it: Windows (and the kit) count a double click only within 2 pixels, which
// no finger hits twice. Triple taps chain the same way.
constexpr uint64_t kTouchDoubleTapNs = 450ull * 1000000ull;
constexpr double kTouchDoubleTapSlop = 30.0;
// Battlefield: a second tap this close to the preview confirms it.
constexpr double kTouchPreviewSlop = 28.0;
// Two fingers on a world map: a pinch past this scale change, a turn past this
// angle (radians), or a common move past kTouchPanStep decides the gesture.
constexpr double kTouchPinchStart = 0.08;
constexpr double kTouchTwistStart = 0.21; // 12 degrees
// Wheel notches per doubling of the fingers' distance.
constexpr double kTouchZoomNotches = 2.0;
// While turning: the rotate key goes down after this much turn (radians) in
// one direction and up once the fingers have not turned for kTouchTwistIdleNs.
constexpr double kTouchTwistStep = 0.05;
constexpr uint64_t kTouchTwistIdleNs = 120ull * 1000000ull;
// Hover: a drag the game says a held Shift would split (RECOMP_TOUCH_DRAG_SPLIT)
// whose finger rests this long within kTouchSplitSlop holds Shift until the
// drop. The ring that counts it down shows after kTouchSplitShowNs, so a
// passing pause draws nothing; moving kTouchSplitCancel from where Shift went
// down lets it go again.
constexpr uint64_t kTouchSplitHoldNs = 700ull * 1000000ull;
constexpr uint64_t kTouchSplitShowNs = 300ull * 1000000ull;
// Native: a resting finger's press ring shows from this (a tap is over by
// then), and stays this long after the press.
constexpr uint64_t kTouchPressRingShowNs = 120ull * 1000000ull;
constexpr uint64_t kTouchPressRingKeepNs = 250ull * 1000000ull;
constexpr double kTouchSplitSlop = 10.0;
constexpr double kTouchSplitCancel = 40.0;

// A hardware pointer stops at the inner side of a strip the system keeps along
// a window edge (a status bar): it cannot go further. A pointer resting within
// kPointerStripSlack of that side is placed on the edge itself, which is where
// a game that scrolls at its edges is looking. The slack is a hand's tremor:
// a game's edge scroll ramps up over a second of the cursor staying put, and a
// pointer pushed against the strip wanders a dozen points. Zero inset: the
// value as it is.
constexpr double kPointerStripSlack = 16.0;
inline double pointer_behind_strip(double v, double inset) {
    return inset > 0 && v < inset + kPointerStripSlack ? 0 : v;
}

// The system may push a pointer that touched the strip back out on its own
// (iPadOS glides it some 30 points away). The edge has to outlast that: once
// the pointer has touched the strip it stays on the edge until it has come
// kPointerStripRelease past the strip, a distance the glide does not reach
// and a hand leaving the edge crosses at once.
constexpr double kPointerStripRelease = 64.0;
class PointerStripLatch {
  public:
    double apply(double v, double inset) {
        if (inset <= 0) {
            latched_ = false;
            return v;
        }
        if (v < inset + kPointerStripSlack)
            latched_ = true;
        else if (v >= inset + kPointerStripRelease)
            latched_ = false;
        return latched_ ? 0 : v;
    }

  private:
    bool latched_ = false;
};

struct TouchPoint {
    int64_t id;
    double x, y; // window points
};

struct TouchAction {
    enum Kind { Motion, Button, Key, Wheel, Camera, Gesture, Native } kind;
    // Motion only: also place the game's own cursor here. True for a press
    // and for a left drag; false while the wheel button is held, when the
    // game scrolls or rotates from relative movement instead.
    bool place = true;
    double x = 0, y = 0; // Motion, Button, Wheel (the event's position)
    int button = 0;      // Button: 0 left, 1 right, 2 middle (wheel)
    bool down = false;   // Button, Key
    int scancode = 0;    // Key: an SDL_Scancode value
    double wheel = 0;    // Wheel: notches, +1 up / -1 down; fractions for a pinch
    // Camera: a gesture the game's camera follows directly (set_camera_gestures):
    // phase 0 begins it at (x, y), 1 moves it there, 2 ends it; zoom is the
    // factor on the camera's distance since it began (below 1 is nearer), turn
    // the radians the fingers turned counterclockwise since then.
    int phase = 0;
    double zoom = 1, turn = 0;
    // Gesture: one the game gives a meaning of its own (runtime/native_seam.h
    // RECOMP_TOUCH_GESTURE_*), delivered in order with the clicks around it.
    // Native: the gesture itself for a game that acts on it at the point
    // (runtime/native_seam.h recomp_touch_event): RECOMP_TOUCH_EVENT_* here,
    // RECOMP_TOUCH_PHASE_* in phase, at (x, y); a tap's count in button.
    int gesture = 0;
};

class TouchMapper {
  public:
    // The window's size in points. Enables edge snapping; zero disables it.
    void set_bounds(double w, double h);
    // Where those bounds start, in window points: the game image's top-left
    // corner when it does not fill the window (portrait on a phone). Edge
    // snapping then works on the image's edges. Zero by default.
    void set_origin(double x, double y);
    // Strips along the window's edges the system keeps for itself (a status
    // bar, a gesture zone), in points. A finger never reaches the app from
    // inside one, so the snap margin on that edge grows by the strip's depth.
    void set_edge_insets(double left, double top, double right, double bottom);
    // Hover gestures (game.toml [touch] gestures = "hover"), for games that
    // show information on hover: a touch puts the cursor under the finger at
    // once; a finger that rests or moves after resting only moves the cursor
    // and lifts without a click; a tap with a second finger while the first
    // rests right-clicks where the first one is. Taps, drags and two-finger
    // gestures are as in the classic table, except that a drag of the
    // interface is never snapped onto the window's edges, and one the game
    // would split with Shift (RECOMP_TOUCH_DRAG_SPLIT) holds Shift once its
    // finger rests kTouchSplitHoldNs (split_ring shows the count).
    void set_hover(bool on) {
        hover_ = on;
    }
    // Hover gestures, where the game reports what lies under its cursor
    // (runtime/native_seam.h RECOMP_TOUCH_* flags), refreshed by the host
    // before each finger event and tick:
    // - on a world map a one-finger drag pans (a wheel-button drag), two
    //   fingers pinch to zoom (wheel) or turn to rotate (Delete/Insert held);
    // - on a battlefield a tap only previews (the cursor goes there and the
    //   game shows what would happen) and a second tap on the same spot acts;
    //   a drag aims without buttons.
    void set_context(int flags) {
        context_ = flags;
    }
    // Native gestures (runtime/native_seam.h recomp_touch_event), for a game
    // that acts on a gesture at its point with no cursor to place: one finger
    // becomes Native actions - it touched, tapped (with the count of a double
    // tap), rested and moved while resting, dragged the interface - instead of
    // placements and clicks, and a two-finger tap a Native right tap. What
    // the game says lies under the finger (set_context, now its answer for the
    // point it touched) still picks the camera or a list for a drag, and
    // multi-finger gestures are as with hover. Implies hover.
    void set_native(bool on) {
        native_ = on;
        if (on)
            hover_ = true;
    }
    bool native() const {
        return native_;
    }
    // A game that moves its map camera itself (runtime/native_seam.h
    // recomp_camera_gesture): on a world map a one-finger drag and any
    // two-finger move become Camera actions - the ground under the fingers
    // stays under them - instead of the wheel-button drag, wheel and turn keys.
    void set_camera_gestures(bool on) {
        camera_ = on;
    }
    // Directions, for a game whose camera answers the other way round.
    void set_invert(bool pan_x, bool pan_y, bool zoom, bool rotate) {
        invert_pan_x_ = pan_x;
        invert_pan_y_ = pan_y;
        invert_zoom_ = zoom;
        invert_rotate_ = rotate;
    }
    // What the game's hover delay should be (RECOMP_TOUCH_HOVER_*): -1 the
    // game's own until a finger touches; then 1 (no tooltip) for a touch or a
    // tap, 0 (at once) for a finger that rests or a battlefield preview.
    int hover_state() const {
        return hover_state_;
    }
    // A mouse, trackpad or pencil moved the pointer: the game's own delay again.
    void pointer_used() {
        hover_state_ = -1;
    }
    void finger_down(TouchPoint p, uint64_t now_ns, std::vector<TouchAction> *out);
    void finger_motion(TouchPoint p, uint64_t now_ns, std::vector<TouchAction> *out);
    void finger_up(TouchPoint p, uint64_t now_ns, std::vector<TouchAction> *out);
    // Fires deferred presses, releases and long presses. Call once per pump.
    void tick(uint64_t now_ns, std::vector<TouchAction> *out);
    // The host's running count of presented frames. Once told, a click presses
    // one frame after placement and releases kTouchClickHoldFrames after press.
    void frames_presented(uint32_t count);
    // The system took the finger away (SDL_EVENT_FINGER_CANCELED): forget it
    // without a click; a drag it was holding is released.
    void finger_cancel(int64_t id, std::vector<TouchAction> *out);
    // Focus loss or backgrounding: every finger is gone. Releases anything held.
    void cancel_all(std::vector<TouchAction> *out);
    // Toggled by a four-finger tap; the host shows or hides the keyboard.
    // The point the cursor was last put at, in this mapper's points.
    void placed(double *x, double *y) const {
        *x = placed_x_;
        *y = placed_y_;
    }
    bool text_input_wanted() const {
        return text_input_;
    }
    // A resting drag counting down to a split (RECOMP_TOUCH_DRAG_SPLIT): where
    // the finger is, in window points, and how far the count is (0 to 1; 1 once
    // Shift is held). False when there is nothing to show.
    bool split_ring(uint64_t now_ns, double *x, double *y, double *progress) const;
    // Native: the same ring under a finger resting on the game, filling up to
    // the press (kTouchLongPressNs; 1 there, a light tick once) and gone a
    // moment after it. False when there is nothing to show.
    bool press_ring(uint64_t now_ns, double *x, double *y, double *progress) const;

  private:
    struct Finger {
        int64_t id;
        double x0, y0, x, y;
        uint64_t t0;
    };
    std::vector<Finger> fingers_; // currently down, in order of arrival
    int max_fingers_ = 0;         // most fingers down during this gesture
    bool dragging_ = false;       // one-finger drag in progress (left held)
    bool long_fired_ = false;     // the finger has rested for the long-press time
    bool middle_held_ = false;    // a drag after the long press: wheel button down
    bool text_input_ = false;
    bool scrolling_ = false;      // a drag over a list: the wheel scrolls it
    double scroll_y_ = 0;         // the finger's last point that sent a notch
    bool hover_ = false;
    bool native_ = false;
    bool native_drag_ = false;    // native: an interface drag under way (Native drag events)
    bool native_press_ = false;   // native: the finger rested; a Native press is under way
    bool native_pressed_moved_ = false;
    int native_taps_ = 0;         // native: the last tap's count, for a double tap
    double native_tap_x_ = 0, native_tap_y_ = 0;
    uint64_t native_tap_ns_ = 0;
    void native_event(std::vector<TouchAction> *out, int kind, int phase, double x, double y,
                      int count = 1);
    void native_tap(std::vector<TouchAction> *out, double x, double y, uint64_t now);
    bool scrubbing_ = false;      // hover: the rested finger moved; the cursor follows it
    // Hover: a second finger landed beside a resting first one. Its quick lift
    // right-clicks at the first finger; the gesture then ends without more clicks.
    bool anchor_ = false, anchor_used_ = false;
    int64_t anchor_tap_id_ = 0;
    uint64_t anchor_tap_t0_ = 0;
    double anchor_tap_x0_ = 0, anchor_tap_y0_ = 0;
    int context_ = 0;
    bool invert_pan_x_ = false, invert_pan_y_ = false, invert_zoom_ = false, invert_rotate_ = false;
    int hover_state_ = -1;
    bool aiming_ = false; // battlefield drag: the cursor follows, nothing pressed
    bool drag_pending_ = false;     // a drag waiting for the game to say what is under it
    uint32_t presents_at_down_ = 0; // frames presented when the finger touched
    // Battlefield: the last preview, which a second tap on the same spot confirms.
    bool preview_ = false;
    double preview_x_ = 0, preview_y_ = 0;
    // Two fingers on a world map: the first gesture to pass its threshold wins.
    enum class Two { None, Pan, Pinch, Twist, Camera } two_ = Two::None;
    bool camera_ = false;     // the game takes camera gestures
    bool cam_active_ = false; // a Camera gesture is under way
    bool cam_used_ = false;   // this gesture moved the camera
    double cam_x_ = 0, cam_y_ = 0, cam_zoom_ = 1, cam_turn_ = 0; // its last values
    void camera(std::vector<TouchAction> *out, int phase, double x, double y, double zoom,
                double turn);
    void end_camera(std::vector<TouchAction> *out);
    double two_d0_ = 0, two_a0_ = 0, two_dprev_ = 0, two_aprev_ = 0;
    double zoom_acc_ = 0;
    int twist_key_ = 0; // the rotate key held while turning, 0 none
    uint64_t twist_moved_ns_ = 0;
    double twist_acc_ = 0;
    void two_finger_world(uint64_t now, std::vector<TouchAction> *out);
    void scroll(std::vector<TouchAction> *out, double x, double y);
    void start_hover_drag(uint64_t now, std::vector<TouchAction> *out);
    void pan_begin(std::vector<TouchAction> *out, double fx, double fy);
    void pan_move(std::vector<TouchAction> *out, double fx, double fy);
    void pan_end(std::vector<TouchAction> *out, double fx, double fy);
    bool pan_virtual_ = false;     // a hover pan: the wheel-button drag follows pan_v*
    double pan_vx_ = 0, pan_vy_ = 0, pan_fx_ = 0, pan_fy_ = 0;
    void release_twist(std::vector<TouchAction> *out);
    double pan_cx_ = 0, pan_cy_ = 0, pan_acc_x_ = 0, pan_acc_y_ = 0;
    double tap2_x_ = 0, tap2_y_ = 0; // a two-finger tap right-clicks here
    double bounds_w_ = 0, bounds_h_ = 0;
    double origin_x_ = 0, origin_y_ = 0;
    double inset_l_ = 0, inset_t_ = 0, inset_r_ = 0, inset_b_ = 0;
    bool snapped_ = false;               // the gesture's placed point sits on a window edge
    double placed_x_ = 0, placed_y_ = 0; // the last placed point
    // Where the cursor goes once an edge hold is over, after any held release.
    bool nudge_pending_ = false;
    double nudge_x_ = 0, nudge_y_ = 0;
    // A click's press waits for a frame after placement, or the time deadline
    // when the host has not supplied presented-frame counts.
    bool press_pending_ = false;
    int press_button_ = 0;
    double press_x_ = 0, press_y_ = 0;
    uint64_t press_deadline_ = 0;
    uint32_t presents_at_place_ = 0;
    // A click's release, held back until kTouchClickHoldNs after its press and,
    // when presents are reported, until kTouchClickHoldFrames of them.
    bool release_pending_ = false;
    int release_button_ = 0;
    double release_x_ = 0, release_y_ = 0;
    uint64_t release_due_ = 0;
    uint64_t release_deadline_ = 0;
    bool last_click_ = false; // the last left click, for double taps
    double last_click_x_ = 0, last_click_y_ = 0;
    uint64_t last_click_ns_ = 0;
    bool presents_known_ = false;
    uint32_t presents_ = 0;
    uint32_t presents_at_press_ = 0;
    // A drag the game would split with Shift (RECOMP_TOUCH_DRAG_SPLIT): where
    // and since when its finger rests, and Shift once the rest was long enough.
    // Shift outlives the drop by kTouchClickHoldFrames, as a click's release
    // does, so the frame that sees the button go up still sees Shift.
    bool hover_drag_ = false; // a hover left drag of the interface (no edge snap)
    double split_x_ = 0, split_y_ = 0;
    uint64_t split_t0_ = 0;
    bool split_shift_ = false;
    bool split_up_pending_ = false;
    uint64_t split_up_due_ = 0, split_up_deadline_ = 0;
    uint32_t split_up_presents_ = 0;
    void split_rest(double x, double y, uint64_t now, std::vector<TouchAction> *out);
    void split_tick(uint64_t now, std::vector<TouchAction> *out);
    void split_drop(uint64_t now);
    void split_release(std::vector<TouchAction> *out);
    bool release_ready(uint64_t now) const;
    void place(std::vector<TouchAction> *out, double x, double y, bool snap = true);
    // chain: a tap soon after the last left click and near it lands exactly on
    // it (a double tap); off where the second tap means another place.
    void click(std::vector<TouchAction> *out, int button, double x, double y, uint64_t now,
               bool chain = true);
    void press_held(std::vector<TouchAction> *out, uint64_t now);
    void end_edge_hold(std::vector<TouchAction> *out);
    void release_held(std::vector<TouchAction> *out);
    double centroid_x() const;
    double centroid_y() const;
    void reset_gesture();
};
