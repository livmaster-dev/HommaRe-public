// input_touch_tests.cpp - the gesture table, one case per row.
#include "../input_touch.h"
#include "../script_touch.h"

#include <SDL3/SDL_scancode.h>
#include <math.h>
#include <stdio.h>

static int g_failures = 0;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                           \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

static const uint64_t MS = 1000000ull;

static void test_tap_places_one_presented_frame_before_pressing() {
    TouchMapper m;
    std::vector<TouchAction> out;
    m.frames_presented(10);
    m.finger_down({1, 100, 100}, 0, &out);
    m.finger_up({1, 100, 100}, 20 * MS, &out);
    CHECK(out.size() == 1 && out[0].kind == TouchAction::Motion && out[0].place &&
          out[0].x == 100 && out[0].y == 100);
    out.clear();
    m.tick(20 * MS, &out);
    CHECK(out.empty());
    m.tick(100 * MS, &out); // known presents take precedence over the 60 ms fallback
    CHECK(out.empty());
    m.frames_presented(11);
    CHECK(out.empty()); // reporting a frame does not emit the press; tick does
    m.tick(100 * MS, &out);
    CHECK(out.size() == 1 && out[0].kind == TouchAction::Button && out[0].down &&
          out[0].button == 0 && out[0].x == 100 && out[0].y == 100);
    out.clear();
    m.frames_presented(12);
    m.tick(190 * MS, &out);
    CHECK(out.empty()); // 90 ms, but only one frame since the press
    m.frames_presented(13);
    m.tick(190 * MS, &out);
    CHECK(out.size() == 1 && out[0].kind == TouchAction::Button && !out[0].down &&
          out[0].button == 0 && out[0].x == 100 && out[0].y == 100);
}

static void test_tap_without_presents_presses_after_60_ms() {
    TouchMapper m;
    std::vector<TouchAction> out;
    m.finger_down({1, 100, 100}, 0, &out);
    m.finger_up({1, 100, 100}, 20 * MS, &out);
    CHECK(out.size() == 1 && out[0].kind == TouchAction::Motion && out[0].x == 100 &&
          out[0].y == 100);
    out.clear();
    m.tick(80 * MS - 1, &out);
    CHECK(out.empty());
    m.tick(80 * MS, &out);
    CHECK(out.size() == 1 && out[0].kind == TouchAction::Button && out[0].down &&
          out[0].button == 0 && out[0].x == 100 && out[0].y == 100);
    out.clear();
    m.tick(170 * MS - 1, &out);
    CHECK(out.empty());
    m.tick(170 * MS, &out);
    CHECK(out.size() == 1 && out[0].kind == TouchAction::Button && !out[0].down &&
          out[0].button == 0 && out[0].x == 100 && out[0].y == 100);
}

static void test_a_new_finger_finishes_a_pending_press_first() {
    TouchMapper m;
    std::vector<TouchAction> out;
    m.frames_presented(10);
    m.finger_down({1, 100, 100}, 0, &out);
    m.finger_up({1, 100, 100}, 20 * MS, &out);
    out.clear();
    m.finger_down({2, 200, 200}, 30 * MS, &out);
    CHECK(out.size() == 2 && out[0].kind == TouchAction::Button && out[0].down &&
          out[0].button == 0 && out[0].x == 100 && out[0].y == 100 &&
          out[1].kind == TouchAction::Button && !out[1].down && out[1].button == 0 &&
          out[1].x == 100 && out[1].y == 100);
    out.clear();
    m.finger_up({2, 200, 200}, 50 * MS, &out);
    CHECK(out.size() == 1 && out[0].kind == TouchAction::Motion && out[0].x == 200 &&
          out[0].y == 200);
    out.clear();
    m.frames_presented(11);
    m.tick(60 * MS, &out);
    CHECK(out.size() == 1 && out[0].kind == TouchAction::Button && out[0].down && out[0].x == 200 &&
          out[0].y == 200);
    out.clear();
    m.frames_presented(13);
    m.tick(150 * MS, &out);
    CHECK(out.size() == 1 && out[0].kind == TouchAction::Button && !out[0].down &&
          out[0].x == 200 && out[0].y == 200);
}

static void test_focus_loss_discards_a_pending_press() {
    TouchMapper m;
    std::vector<TouchAction> out;
    m.frames_presented(10);
    m.finger_down({1, 100, 100}, 0, &out);
    m.finger_up({1, 100, 100}, 20 * MS, &out);
    out.clear();
    m.cancel_all(&out);
    CHECK(out.empty()); // the press never reached the guest, so it owes no release
    m.frames_presented(20);
    m.tick(1000 * MS, &out);
    CHECK(out.empty());
    m.finger_down({2, 200, 200}, 1010 * MS, &out);
    CHECK(out.empty()); // no old click to flush on the next finger either
}

// The smoke driver must exercise the mapper: no button on finger-down, a
// placement on lift, a press on a later frame, then the present-gated release.
static void test_smoke_tap_drives_the_mapper() {
    HostScriptTouch tap;
    std::vector<TouchAction> out;
    tap.start(24, 8, 640, 480, 0, 10, &out);
    CHECK(tap.active() && out.empty());
    tap.tick(79 * MS, 10, &out);
    CHECK(out.empty() && !tap.pending(79 * MS, 10));
    tap.tick(80 * MS, 10, &out);
    CHECK(out.size() == 1 && out[0].kind == TouchAction::Motion && out[0].x == 24 && out[0].y == 8);
    out.clear();
    tap.tick(100 * MS, 10, &out);
    CHECK(tap.active() && out.empty());
    CHECK(tap.pending(100 * MS, 11));
    tap.tick(100 * MS, 11, &out);
    CHECK(tap.active() && out.size() == 1 && out[0].kind == TouchAction::Button && out[0].down &&
          out[0].x == 24 && out[0].y == 8);
    out.clear();
    tap.tick(190 * MS, 12, &out);
    CHECK(tap.active() && out.empty());
    CHECK(tap.pending(190 * MS, 13));
    tap.tick(190 * MS, 13, &out);
    CHECK(!tap.active() && out.size() == 1 && out[0].kind == TouchAction::Button && !out[0].down &&
          out[0].x == 24 && out[0].y == 8);
}

static void test_tap_is_left_click() {
    TouchMapper m;
    std::vector<TouchAction> out;
    m.finger_down({1, 100, 200}, 0, &out);
    m.finger_up({1, 101, 201}, 80 * MS, &out);
    CHECK(out.size() == 1 && out[0].kind == TouchAction::Motion && out[0].x == 101 &&
          out[0].y == 201); // place now; the press is held
    const uint64_t press_at = 80 * MS + kTouchPressDelayNs;
    m.tick(press_at, &out);
    CHECK(out.size() == 2);
    CHECK(out[1].kind == TouchAction::Button && out[1].button == 0 && out[1].down);
    m.tick(press_at + kTouchClickHoldNs / 2, &out);
    CHECK(out.size() == 2); // still held
    m.tick(press_at + kTouchClickHoldNs, &out);
    CHECK(out.size() == 3 && out[2].kind == TouchAction::Button && out[2].button == 0 &&
          !out[2].down);
    // The release lands where the press did. (The press point was once read
    // back out of the action vector after another action had been pushed into
    // it, which reallocates: on a device the release then carried 0,0.)
    CHECK(out[2].x == 101 && out[2].y == 201);
}

static void test_release_lands_where_the_press_did_whatever_the_vector_did() {
    TouchMapper m;
    for (size_t reserve = 0; reserve < 6; ++reserve) {
        std::vector<TouchAction> out;
        out.reserve(reserve);
        m.finger_down({1, 300, 200}, 0, &out);
        m.finger_up({1, 300, 200}, 20 * MS, &out);
        m.tick(20 * MS + kTouchPressDelayNs, &out);
        m.tick(20 * MS + kTouchPressDelayNs + kTouchClickHoldNs, &out);
        CHECK(out.size() == 3 && !out[2].down && out[2].x == 300 && out[2].y == 200);
    }
}

static void test_a_new_finger_releases_a_held_click_first() {
    TouchMapper m;
    std::vector<TouchAction> out;
    m.finger_down({1, 0, 0}, 0, &out);
    m.finger_up({1, 0, 0}, 20 * MS, &out);
    m.tick(80 * MS, &out);
    out.clear();
    m.finger_down({2, 50, 50}, 90 * MS, &out);
    CHECK(out.size() == 1 && out[0].kind == TouchAction::Button && !out[0].down);
}

static void test_long_press_is_right_click() {
    TouchMapper m;
    std::vector<TouchAction> out;
    m.finger_down({1, 50, 60}, 0, &out);
    m.tick(300 * MS, &out);
    CHECK(out.empty());
    m.tick(360 * MS, &out);
    // The rest only places the cursor; the button waits for the lift.
    CHECK(out.size() == 1 && out[0].kind == TouchAction::Motion && out[0].place && out[0].x == 50);
    out.clear();
    m.finger_up({1, 50, 60}, 380 * MS, &out);
    CHECK(out.size() == 1 && out[0].kind == TouchAction::Motion);
    m.tick(380 * MS + kTouchPressDelayNs, &out);
    CHECK(out.size() == 2 && out[0].kind == TouchAction::Motion &&
          out[1].kind == TouchAction::Button && out[1].button == 1 && out[1].down);
    m.tick(380 * MS + kTouchPressDelayNs + kTouchClickHoldNs, &out);
    CHECK(out.size() == 3 && out[2].kind == TouchAction::Button && out[2].button == 1 &&
          !out[2].down);
}

static void test_long_press_then_drag_is_a_wheel_button_drag() {
    TouchMapper m;
    std::vector<TouchAction> out;
    m.finger_down({1, 50, 60}, 0, &out);
    m.tick(360 * MS, &out); // cursor placed
    out.clear();
    m.finger_motion({1, 55, 62}, 400 * MS, &out);
    CHECK(out.empty()); // still resting
    m.finger_motion({1, 120, 90}, 500 * MS, &out);
    CHECK(out.size() == 2);
    CHECK(out[0].kind == TouchAction::Button && out[0].button == 2 && out[0].down &&
          out[0].x == 50 && out[0].y == 60);
    CHECK(out[1].kind == TouchAction::Motion && out[1].x == 120 && !out[1].place);
    out.clear();
    m.finger_motion({1, 125, 92}, 600 * MS, &out);
    CHECK(out.size() == 1 && !out[0].place);
    out.clear();
    m.finger_up({1, 130, 95}, 900 * MS, &out);
    CHECK(out.size() == 2 && out[0].kind == TouchAction::Motion && !out[0].place &&
          out[1].kind == TouchAction::Button && out[1].button == 2 && !out[1].down);
    out.clear();
    m.tick(2000 * MS, &out);
    CHECK(out.empty()); // no right click after a drag
}

static void test_edge_hold_scrolls_then_moves_the_cursor_inside() {
    TouchMapper m;
    m.set_bounds(1000, 800);
    std::vector<TouchAction> out;
    m.finger_down({1, 5, 400}, 0, &out);
    m.tick(360 * MS, &out);
    CHECK(out.size() == 1 && out[0].kind == TouchAction::Motion && out[0].x == 0 &&
          out[0].y == 400); // snapped onto the left edge
    out.clear();
    m.tick(1500 * MS, &out);
    CHECK(out.empty()); // an edge hold is not a right click
    m.finger_up({1, 6, 402}, 2000 * MS, &out);
    CHECK(out.size() == 1 && out[0].kind == TouchAction::Motion && out[0].place &&
          out[0].x == kTouchEdgeRelease && out[0].y == 400);
    m.tick(3000 * MS, &out);
    CHECK(out.size() == 1); // and no click
}

static void test_tap_near_an_edge_clicks_at_the_finger() {
    TouchMapper m;
    m.set_bounds(800, 600);
    std::vector<TouchAction> out;
    m.finger_down({1, 5, 400}, 0, &out);
    m.finger_up({1, 5, 400}, 50 * MS, &out);
    CHECK(out.size() == 1 && out[0].kind == TouchAction::Motion && out[0].x == 5 &&
          out[0].y == 400);
    m.tick(50 * MS + kTouchPressDelayNs, &out);
    CHECK(out.size() == 2 && out[1].kind == TouchAction::Button && out[1].button == 0 &&
          out[1].down && out[1].x == 5 && out[1].y == 400);
    out.clear();
    m.tick(50 * MS + kTouchPressDelayNs + kTouchClickHoldNs, &out);
    CHECK(out.size() == 1 && out[0].kind == TouchAction::Button && out[0].button == 0 &&
          !out[0].down && out[0].x == 5 && out[0].y == 400); // release, no nudge
    out.clear();
    m.tick(1000 * MS, &out);
    CHECK(out.empty()); // no later nudge
}

static void test_no_bounds_means_no_snapping() {
    TouchMapper m;
    std::vector<TouchAction> out;
    m.finger_down({1, 2, 3}, 0, &out);
    m.finger_up({1, 2, 3}, 50 * MS, &out);
    CHECK(out.size() == 1 && out[0].x == 2 && out[0].y == 3);
    m.tick(50 * MS + kTouchPressDelayNs, &out);
    m.tick(50 * MS + kTouchPressDelayNs + kTouchClickHoldNs, &out);
    CHECK(out.size() == 3); // release, no nudge
}

static void test_drag_is_left_drag() {
    TouchMapper m;
    std::vector<TouchAction> out;
    m.finger_down({1, 10, 10}, 0, &out);
    m.finger_motion({1, 20, 10}, 20 * MS, &out);
    CHECK(out.empty()); // under the travel threshold
    m.finger_motion({1, 45, 10}, 40 * MS, &out);
    CHECK(out.size() == 3); // Motion to start, Button down, Motion to here
    CHECK(out[0].kind == TouchAction::Motion && out[0].x == 10 && out[0].place);
    CHECK(out[1].kind == TouchAction::Button && out[1].button == 0 && out[1].down);
    CHECK(out[2].kind == TouchAction::Motion && out[2].x == 45);
    out.clear();
    m.finger_up({1, 55, 10}, 60 * MS, &out);
    CHECK(out.size() == 2 && out[0].kind == TouchAction::Motion &&
          out[1].kind == TouchAction::Button && !out[1].down);
}

static void test_two_finger_drag_pans_with_arrows() {
    TouchMapper m;
    std::vector<TouchAction> out;
    m.finger_down({1, 100, 100}, 0, &out);
    m.finger_down({2, 140, 100}, 5 * MS, &out);
    m.finger_motion({1, 100, 130}, 30 * MS, &out);
    m.finger_motion({2, 140, 130}, 30 * MS, &out); // centroid moved 30 down: one step
    CHECK(out.size() == 2);
    CHECK(out[0].kind == TouchAction::Key && out[0].scancode == SDL_SCANCODE_DOWN && out[0].down);
    CHECK(out[1].kind == TouchAction::Key && out[1].scancode == SDL_SCANCODE_DOWN && !out[1].down);
    out.clear();
    m.finger_up({1, 100, 130}, 60 * MS, &out);
    m.finger_up({2, 140, 130}, 60 * MS, &out);
    CHECK(out.empty()); // no click from a pan
}

static void test_two_finger_tap_is_right_click_three_is_f10_four_toggles_keyboard() {
    TouchMapper m;
    std::vector<TouchAction> out;
    m.finger_down({1, 0, 0}, 0, &out);
    m.finger_down({2, 10, 0}, 0, &out);
    m.finger_up({1, 0, 0}, 50 * MS, &out);
    m.finger_up({2, 10, 0}, 50 * MS, &out);
    // Placed between the fingers, then pressed and released as a right click.
    CHECK(out.size() == 1 && out[0].kind == TouchAction::Motion && out[0].place && out[0].x == 5 &&
          out[0].y == 0);
    out.clear();
    m.tick(110 * MS, &out);
    CHECK(out.size() == 1 && out[0].kind == TouchAction::Button && out[0].button == 1 &&
          out[0].down && out[0].x == 5);
    out.clear();
    m.tick(210 * MS, &out);
    CHECK(out.size() == 1 && out[0].kind == TouchAction::Button && out[0].button == 1 &&
          !out[0].down);
    out.clear();
    m.finger_down({1, 0, 0}, 300 * MS, &out);
    m.finger_down({2, 10, 0}, 300 * MS, &out);
    m.finger_down({3, 20, 0}, 300 * MS, &out);
    m.finger_up({1, 0, 0}, 350 * MS, &out);
    m.finger_up({2, 10, 0}, 350 * MS, &out);
    m.finger_up({3, 20, 0}, 350 * MS, &out);
    CHECK(out.size() == 2 && out[0].scancode == SDL_SCANCODE_F10);
    out.clear();
    CHECK(!m.text_input_wanted());
    for (int i = 1; i <= 4; ++i)
        m.finger_down({i, 10.0 * i, 0}, 400 * MS, &out);
    for (int i = 1; i <= 4; ++i)
        m.finger_up({i, 10.0 * i, 0}, 450 * MS, &out);
    CHECK(out.empty() && m.text_input_wanted());
}

static void test_second_finger_cancels_pending_tap() {
    TouchMapper m;
    std::vector<TouchAction> out;
    m.finger_down({1, 0, 0}, 0, &out);
    m.finger_down({2, 10, 0}, 10 * MS, &out);
    m.tick(400 * MS, &out);
    CHECK(out.empty()); // no long press once a second finger joined
}

// The game samples its buttons once per frame. A release timed from the clock
// alone can land before the frame after the press ever sampled, so once the
// host reports presented frames the release also waits for two of them.
static void test_click_release_waits_for_two_presented_frames() {
    TouchMapper m;
    std::vector<TouchAction> out;
    m.frames_presented(10);
    m.finger_down({1, 100, 200}, 0, &out);
    m.finger_up({1, 100, 200}, 20 * MS, &out);
    m.frames_presented(11);
    m.tick(30 * MS, &out);
    CHECK(out.size() == 2 && out[1].kind == TouchAction::Button && out[1].down);
    m.tick(30 * MS + kTouchClickHoldNs, &out);
    CHECK(out.size() == 2); // the hold time passed, but no frame sampled yet
    m.frames_presented(12);
    m.tick(30 * MS + kTouchClickHoldNs + 10 * MS, &out);
    CHECK(out.size() == 2); // one frame saw the press; the next must too
    m.frames_presented(13);
    m.tick(30 * MS + kTouchClickHoldNs + 20 * MS, &out);
    CHECK(out.size() == 3 && out[2].kind == TouchAction::Button && !out[2].down);
}

static void test_click_release_waits_for_the_hold_time_even_when_frames_flew() {
    TouchMapper m;
    std::vector<TouchAction> out;
    m.frames_presented(10);
    m.finger_down({1, 100, 200}, 0, &out);
    m.finger_up({1, 100, 200}, 20 * MS, &out);
    m.frames_presented(11);
    m.tick(30 * MS, &out);
    m.frames_presented(20);
    m.tick(30 * MS + kTouchClickHoldNs - 1, &out);
    CHECK(out.size() == 2);
    m.tick(30 * MS + kTouchClickHoldNs, &out);
    CHECK(out.size() == 3 && !out[2].down);
}

// A game that stopped presenting (a modal wait, a stalled frame) still gets
// its release, so the button is not left down forever.
static void test_a_stalled_game_still_gets_its_release() {
    TouchMapper m;
    std::vector<TouchAction> out;
    m.frames_presented(10);
    m.finger_down({1, 100, 200}, 0, &out);
    m.finger_up({1, 100, 200}, 20 * MS, &out);
    m.frames_presented(11);
    m.tick(30 * MS, &out);
    m.tick(30 * MS + kTouchClickHoldMaxNs - MS, &out);
    CHECK(out.size() == 2);
    m.tick(30 * MS + kTouchClickHoldMaxNs, &out);
    CHECK(out.size() == 3 && !out[2].down);
}

// The system keeps a strip along some window edges for itself (a status bar,
// a gesture zone): fingers there never reach the app, so the nearest a finger
// gets to that edge is the strip's far side. The host reports the strip as an
// inset and the snap margin grows by it on that edge alone.
static void test_edge_insets_widen_the_snap_on_that_edge_only() {
    TouchMapper m;
    m.set_bounds(1000, 800);
    m.set_edge_insets(0, 24, 0, 0);
    std::vector<TouchAction> out;
    m.finger_down({1, 500, 35}, 0, &out);
    m.tick(360 * MS, &out);
    CHECK(out.size() == 1 && out[0].kind == TouchAction::Motion && out[0].y == 0 &&
          out[0].x == 500); // 35 < 16 + 24: snapped onto the top edge
    out.clear();
    m.finger_up({1, 500, 35}, 2000 * MS, &out);
    out.clear();
    m.finger_down({2, 500, 45}, 3000 * MS, &out);
    m.tick(3360 * MS, &out);
    CHECK(out.size() == 1 && out[0].kind == TouchAction::Motion && out[0].y == 45); // 45 >= 40
    out.clear();
    m.finger_up({2, 500, 45}, 5000 * MS, &out); // a right click, not an edge hold
    m.tick(5000 * MS + kTouchPressDelayNs, &out);
    m.tick(5000 * MS + kTouchPressDelayNs + kTouchClickHoldNs, &out);
    out.clear();
    m.finger_down({3, 500, 780}, 6000 * MS, &out);
    m.tick(6360 * MS, &out);
    CHECK(out.size() == 1 && out[0].kind == TouchAction::Motion &&
          out[0].y == 780); // the bottom margin is still 16
}

// The system can take a finger away mid-drag (an edge gesture it claims). The
// button it held lets go where the cursor was placed, not at the origin: a
// release at 0,0 also moves the game's cursor there, and a game that scrolls
// when the cursor touches an edge then flies to its top-left corner.
static void test_a_cancelled_drag_releases_where_it_was() {
    TouchMapper m;
    m.set_bounds(1000, 800);
    std::vector<TouchAction> out;
    m.finger_down({1, 500, 400}, 0, &out);
    m.finger_motion({1, 540, 400}, 50 * MS, &out); // beyond the tap travel: a drag
    CHECK(out.size() == 3 && out[1].kind == TouchAction::Button && out[1].down);
    out.clear();
    m.finger_cancel(1, &out);
    CHECK(out.size() == 1 && out[0].kind == TouchAction::Button && !out[0].down &&
          out[0].x == 540 && out[0].y == 400);
    out.clear();
    m.finger_down({2, 300, 300}, 1000 * MS, &out);
    m.finger_motion({2, 300, 350}, 1050 * MS, &out);
    out.clear();
    m.cancel_all(&out);
    CHECK(out.size() == 1 && out[0].kind == TouchAction::Button && !out[0].down &&
          out[0].x == 300 && out[0].y == 350);
}

// A hardware pointer cannot enter the strip the system keeps along an edge
// either: it stops at the strip's inner side. A pointer resting there means
// the edge, and the game's edge scroll needs the edge itself.
static void test_a_pointer_against_a_system_strip_means_the_edge() {
    CHECK(pointer_behind_strip(32.1, 32) == 0);
    CHECK(pointer_behind_strip(47.9, 32) == 0); // a hand's tremor against the strip
    CHECK(pointer_behind_strip(48, 32) == 48);  // clear of the strip: as it is
    CHECK(pointer_behind_strip(3, 0) == 3);     // no strip: nothing to snap
}

// iPadOS pushes a pointer that touched the top strip back down in a glide of
// its own (34, 40, 45, 49, 53, 56, 58, 60, 62, 64 points on one device). The
// edge must outlast the glide: once the pointer has touched the strip it stays
// on the edge until it has come clearly away.
static void test_the_edge_outlasts_the_pointer_glide_off_the_strip() {
    PointerStripLatch latch;
    CHECK(latch.apply(200, 32) == 200);
    CHECK(latch.apply(34.7, 32) == 0); // touched the strip
    CHECK(latch.apply(49.1, 32) == 0); // the glide: still the edge
    CHECK(latch.apply(63.8, 32) == 0);
    CHECK(latch.apply(95.9, 32) == 0);
    CHECK(latch.apply(96, 32) == 96); // clearly away: released
    CHECK(latch.apply(60, 32) == 60); // and no longer the edge on the way back
    CHECK(latch.apply(40, 0) == 40);  // no strip: never latches
}

// Portrait: the game image is a rectangle inside the window, not the whole
// window. With an origin the edges are that rectangle's edges, so an edge hold
// scrolls at the image's top and bottom rather than at the window's.
static void test_origin_moves_the_edges_onto_the_game_rectangle() {
    TouchMapper m;
    m.set_bounds(390, 293); // the game image, in points
    m.set_origin(0, 47);    // pinned below a 47-point status bar
    std::vector<TouchAction> out;
    m.finger_down({1, 200, 50}, 0, &out); // 3 points into the image
    m.tick(360 * MS, &out);
    CHECK(out.size() == 1 && out[0].kind == TouchAction::Motion && out[0].x == 200 &&
          out[0].y == 47); // snapped onto the image's top edge
    out.clear();
    m.finger_up({1, 200, 50}, 2000 * MS, &out);
    CHECK(out.size() == 1 && out[0].kind == TouchAction::Motion && out[0].x == 200 &&
          out[0].y == 47 + kTouchEdgeRelease); // moved back inside the image
    m.tick(3000 * MS, &out);
    out.clear();
    m.finger_down({2, 200, 335}, 4000 * MS, &out); // 3 points above the image's bottom
    m.tick(4360 * MS, &out);
    CHECK(out.size() == 1 && out[0].kind == TouchAction::Motion &&
          out[0].y == 47 + 293 - 1); // the image's last row, not the window's
    out.clear();
    m.finger_up({2, 200, 335}, 6000 * MS, &out);
    CHECK(out.size() == 1 && out[0].y == 47 + 293 - 1 - kTouchEdgeRelease);
    m.tick(7000 * MS, &out);
    out.clear();
    m.finger_down({3, 200, 70}, 8000 * MS, &out); // 23 points in: not an edge
    m.tick(8360 * MS, &out);
    CHECK(out.size() == 1 && out[0].y == 70);
}

// Hover gestures (game.toml [touch] gestures = "hover").
static bool has_key(const std::vector<TouchAction> &out) {
    for (const TouchAction &a : out)
        if (a.kind == TouchAction::Key)
            return true;
    return false;
}

static bool has_button(const std::vector<TouchAction> &out) {
    for (const TouchAction &a : out)
        if (a.kind == TouchAction::Button)
            return true;
    return false;
}

static void test_hover_touch_places_at_once_and_a_tap_still_clicks() {
    TouchMapper m;
    m.set_hover(true);
    std::vector<TouchAction> out;
    m.finger_down({1, 100, 100}, 0, &out);
    CHECK(out.size() == 1 && out[0].kind == TouchAction::Motion && out[0].place &&
          out[0].x == 100 && out[0].y == 100);
    out.clear();
    m.finger_up({1, 102, 101}, 80 * MS, &out);
    m.tick(200 * MS, &out);
    m.tick(400 * MS, &out);
    int downs = 0, ups = 0;
    for (const TouchAction &a : out)
        if (a.kind == TouchAction::Button && a.button == 0)
            (a.down ? downs : ups) += 1;
    CHECK(downs == 1 && ups == 1);
}

static void test_hover_rest_lifts_without_a_click() {
    TouchMapper m;
    m.set_hover(true);
    std::vector<TouchAction> out;
    m.finger_down({1, 100, 100}, 0, &out);
    out.clear();
    m.tick(400 * MS, &out);
    CHECK(out.empty()); // already under the finger: nothing more to send
    m.finger_motion({1, 104, 103}, 450 * MS, &out);
    CHECK(out.empty()); // a resting finger's tremor would restart the hover timer
    m.finger_up({1, 104, 103}, 900 * MS, &out);
    m.tick(1500 * MS, &out);
    CHECK(!has_button(out));
}

static void test_hover_rested_finger_moves_the_cursor_without_buttons() {
    TouchMapper m;
    m.set_hover(true);
    std::vector<TouchAction> out;
    m.finger_down({1, 100, 100}, 0, &out);
    m.tick(400 * MS, &out);
    out.clear();
    m.finger_motion({1, 160, 100}, 500 * MS, &out);
    CHECK(out.size() == 1 && out[0].kind == TouchAction::Motion && out[0].x == 160);
    m.finger_up({1, 160, 100}, 600 * MS, &out);
    m.tick(1200 * MS, &out);
    CHECK(!has_button(out));
}

static void test_hover_quick_drag_is_still_a_left_drag() {
    TouchMapper m;
    m.set_hover(true);
    std::vector<TouchAction> out;
    m.finger_down({1, 100, 100}, 0, &out);
    m.finger_motion({1, 150, 100}, 100 * MS, &out);
    bool pressed = false;
    for (const TouchAction &a : out)
        pressed |= a.kind == TouchAction::Button && a.button == 0 && a.down;
    CHECK(pressed);
}

static void test_hover_second_finger_tap_right_clicks_at_the_first() {
    TouchMapper m;
    m.set_hover(true);
    std::vector<TouchAction> out;
    m.finger_down({1, 100, 100}, 0, &out);
    m.tick(200 * MS, &out);
    m.finger_down({2, 400, 300}, 250 * MS, &out);
    out.clear();
    m.finger_up({2, 401, 300}, 330 * MS, &out);
    m.tick(400 * MS, &out);
    m.tick(600 * MS, &out);
    int downs = 0, ups = 0;
    for (const TouchAction &a : out)
        if (a.kind == TouchAction::Button) {
            CHECK(a.button == 1 && a.x == 100 && a.y == 100);
            (a.down ? downs : ups) += 1;
        }
    CHECK(downs == 1 && ups == 1);
    out.clear();
    m.finger_up({1, 100, 100}, 900 * MS, &out); // the resting finger lifts: nothing more
    m.tick(1500 * MS, &out);
    CHECK(!has_button(out));
}

static void test_hover_two_fingers_together_are_still_a_two_finger_tap() {
    TouchMapper m;
    m.set_hover(true);
    std::vector<TouchAction> out;
    m.finger_down({1, 100, 100}, 0, &out);
    m.finger_down({2, 200, 100}, 30 * MS, &out);
    m.finger_up({1, 100, 100}, 120 * MS, &out);
    m.finger_up({2, 200, 100}, 130 * MS, &out);
    m.tick(300 * MS, &out);
    m.tick(600 * MS, &out);
    bool right = false;
    for (const TouchAction &a : out)
        right |= a.kind == TouchAction::Button && a.button == 1 && a.down && a.x == 150;
    CHECK(right);
}

static void test_hover_slow_second_finger_is_not_a_tap() {
    TouchMapper m;
    m.set_hover(true);
    std::vector<TouchAction> out;
    m.finger_down({1, 100, 100}, 0, &out);
    m.finger_down({2, 400, 300}, 250 * MS, &out);
    m.finger_up({2, 400, 300}, 900 * MS, &out); // held too long for a tap
    m.finger_up({1, 100, 100}, 1000 * MS, &out);
    m.tick(1500 * MS, &out);
    CHECK(!has_button(out));
}

// Hover gestures with the game's context (RECOMP_TOUCH_WORLD 1, _BATTLE 2).
static void test_hover_state_follows_the_finger() {
    TouchMapper m;
    m.set_hover(true);
    std::vector<TouchAction> out;
    CHECK(m.hover_state() == -1); // the game's own delay until a finger touches
    m.finger_down({1, 100, 100}, 0, &out);
    CHECK(m.hover_state() == 1); // a touch: no tooltip yet
    m.tick(400 * MS, &out);
    CHECK(m.hover_state() == 0); // a rest: at once
    m.finger_up({1, 100, 100}, 500 * MS, &out);
    CHECK(m.hover_state() == 0); // and it stays once the finger is gone
    m.finger_down({1, 300, 100}, 1000 * MS, &out);
    m.finger_up({1, 300, 100}, 1080 * MS, &out);
    CHECK(m.hover_state() == 1); // a tap leaves no tooltip behind
}

static void test_world_drag_is_a_wheel_button_drag() {
    TouchMapper m;
    m.set_hover(true);
    m.set_context(1);
    std::vector<TouchAction> out;
    m.finger_down({1, 100, 100}, 0, &out);
    out.clear();
    m.finger_motion({1, 150, 120}, 50 * MS, &out);
    // No bounds: the virtual pointer starts where the finger did.
    CHECK(out.size() == 3 && out[0].kind == TouchAction::Motion && out[0].place &&
          out[1].kind == TouchAction::Button && out[1].button == 2 && out[1].down);
    CHECK(out[2].kind == TouchAction::Motion && !out[2].place && out[2].x == 150 &&
          out[2].y == 120);
    out.clear();
    m.finger_up({1, 160, 120}, 100 * MS, &out);
    bool up = false;
    for (const TouchAction &a : out)
        up |= a.kind == TouchAction::Button && a.button == 2 && !a.down;
    CHECK(up);
    CHECK(out.back().kind == TouchAction::Motion && out.back().place && out.back().x == 160);
}

static void test_world_pan_can_be_inverted_and_stays_inside() {
    TouchMapper m;
    m.set_hover(true);
    m.set_context(1);
    m.set_bounds(1000, 800);
    m.set_invert(true, true, false, false);
    std::vector<TouchAction> out;
    m.finger_down({1, 100, 100}, 0, &out);
    out.clear();
    m.finger_motion({1, 150, 120}, 50 * MS, &out);
    // Starts in the middle (500,400) and moves against the finger.
    CHECK(out.size() == 3 && out[0].x == 500 && out[0].y == 400);
    CHECK(out[2].kind == TouchAction::Motion && !out[2].place && out[2].x == 450 &&
          out[2].y == 380);
    // A long drag never takes the pointer out of the image: it is placed back
    // in the middle, and the counted motion is the finger's own step.
    double total = 0;
    for (int i = 1; i <= 40; ++i) {
        out.clear();
        m.finger_motion({1, 150.0 + 20 * i, 120}, (50 + 10 * i) * MS, &out);
        for (const TouchAction &a : out) {
            CHECK(a.x >= 0 && a.x <= 1000 && a.y >= 0 && a.y <= 800);
            if (a.kind == TouchAction::Motion && !a.place)
                total += 1;
        }
    }
    CHECK(total == 40);
}

static void test_ui_drag_stays_a_left_drag_on_hover() {
    TouchMapper m;
    m.set_hover(true);
    m.set_context(0);
    std::vector<TouchAction> out;
    m.finger_down({1, 100, 100}, 0, &out);
    m.finger_motion({1, 150, 100}, 50 * MS, &out);
    bool left = false, middle = false;
    for (const TouchAction &a : out)
        if (a.kind == TouchAction::Button) {
            left |= a.button == 0;
            middle |= a.button == 2;
        }
    CHECK(left && !middle);
}

static void test_battle_tap_previews_then_confirms() {
    TouchMapper m;
    m.set_hover(true);
    m.set_context(2);
    std::vector<TouchAction> out;
    m.finger_down({1, 200, 200}, 0, &out);
    m.finger_up({1, 201, 200}, 80 * MS, &out);
    m.tick(500 * MS, &out);
    CHECK(!has_button(out)); // the first tap only previews
    CHECK(m.hover_state() == 0);
    out.clear();
    m.finger_down({1, 300, 300}, 1000 * MS, &out); // somewhere else: a new preview
    m.finger_up({1, 300, 300}, 1080 * MS, &out);
    m.tick(1500 * MS, &out);
    CHECK(!has_button(out));
    out.clear();
    m.finger_down({1, 310, 305}, 2000 * MS, &out); // the same spot: act
    m.finger_up({1, 310, 305}, 2080 * MS, &out);
    m.tick(2200 * MS, &out);
    m.tick(2500 * MS, &out);
    int downs = 0, ups = 0;
    for (const TouchAction &a : out)
        if (a.kind == TouchAction::Button) {
            CHECK(a.button == 0 && a.x == 300 && a.y == 300); // at the previewed point
            (a.down ? downs : ups) += 1;
        }
    CHECK(downs == 1 && ups == 1);
    out.clear();
    m.finger_down({1, 300, 300}, 3000 * MS, &out); // after acting: a preview again
    m.finger_up({1, 300, 300}, 3080 * MS, &out);
    m.tick(3500 * MS, &out);
    CHECK(!has_button(out));
}

static void test_battle_drag_aims_and_a_tap_on_it_acts() {
    TouchMapper m;
    m.set_hover(true);
    m.set_context(2);
    std::vector<TouchAction> out;
    m.finger_down({1, 200, 200}, 0, &out);
    m.finger_motion({1, 260, 200}, 50 * MS, &out);
    m.finger_motion({1, 280, 210}, 80 * MS, &out);
    m.finger_up({1, 280, 210}, 150 * MS, &out);
    m.tick(600 * MS, &out);
    CHECK(!has_button(out));
    out.clear();
    m.finger_down({1, 282, 212}, 1000 * MS, &out);
    m.finger_up({1, 282, 212}, 1080 * MS, &out);
    m.tick(1200 * MS, &out);
    m.tick(1500 * MS, &out);
    bool clicked = false;
    for (const TouchAction &a : out)
        clicked |= a.kind == TouchAction::Button && a.down && a.button == 0 && a.x == 280;
    CHECK(clicked);
}

static void test_world_pinch_zooms_with_the_wheel() {
    TouchMapper m;
    m.set_hover(true);
    m.set_context(1);
    std::vector<TouchAction> out;
    m.finger_down({1, 400, 300}, 0, &out);
    m.finger_down({2, 500, 300}, 20 * MS, &out);
    m.finger_motion({2, 520, 300}, 40 * MS, &out); // past the pinch threshold: decides
    out.clear();
    m.finger_motion({2, 600, 300}, 60 * MS, &out); // spread: positive notches
    double sum = 0;
    for (const TouchAction &a : out)
        if (a.kind == TouchAction::Wheel)
            sum += a.wheel;
    CHECK(sum > 0.5 && sum < 3.0);
    out.clear();
    m.finger_up({2, 600, 300}, 100 * MS, &out);
    m.finger_up({1, 400, 300}, 110 * MS, &out);
    m.tick(500 * MS, &out);
    CHECK(!has_button(out)); // a pinch is not a two-finger tap
}

static void test_world_turn_holds_the_rotate_key_while_turning() {
    TouchMapper m;
    m.set_hover(true);
    m.set_context(1);
    std::vector<TouchAction> out;
    m.finger_down({1, 400, 300}, 0, &out);
    m.finger_down({2, 500, 300}, 20 * MS, &out);
    m.finger_motion({2, 498, 330}, 40 * MS, &out); // ~17 degrees: decides a turn
    out.clear();
    m.finger_motion({2, 490, 350}, 60 * MS, &out); // further on: the key goes down
    bool down = false;
    for (const TouchAction &a : out)
        down |= a.kind == TouchAction::Key && a.down && a.scancode == SDL_SCANCODE_INSERT;
    CHECK(down);
    out.clear();
    m.tick(100 * MS, &out);
    CHECK(out.empty()); // still turning recently
    m.tick(300 * MS, &out); // idle: released
    CHECK(out.size() == 1 && out[0].kind == TouchAction::Key && !out[0].down &&
          out[0].scancode == SDL_SCANCODE_INSERT);
}

static std::vector<TouchAction> camera_actions(const std::vector<TouchAction> &out) {
    std::vector<TouchAction> c;
    for (const TouchAction &a : out)
        if (a.kind == TouchAction::Camera)
            c.push_back(a);
    return c;
}

static void test_world_drag_moves_the_camera_when_the_game_takes_it() {
    TouchMapper m;
    m.set_hover(true);
    m.set_context(1);
    m.set_camera_gestures(true);
    std::vector<TouchAction> out;
    m.finger_down({1, 100, 100}, 0, &out);
    out.clear();
    m.finger_motion({1, 150, 120}, 50 * MS, &out);
    auto c = camera_actions(out);
    // Begun where the finger touched, moved to where it is: no buttons.
    CHECK(!has_button(out) && c.size() == 2 && c[0].phase == 0 && c[0].x == 100 &&
          c[0].y == 100 && c[1].phase == 1 && c[1].x == 150 && c[1].zoom == 1 && c[1].turn == 0);
    out.clear();
    m.finger_motion({1, 170, 140}, 70 * MS, &out);
    c = camera_actions(out);
    CHECK(c.size() == 1 && c[0].phase == 1 && c[0].x == 170 && c[0].y == 140);
    out.clear();
    m.finger_up({1, 180, 140}, 100 * MS, &out);
    c = camera_actions(out);
    CHECK(c.size() == 2 && c[0].phase == 1 && c[0].x == 180 && c[1].phase == 2);
    CHECK(!has_button(out) && out.back().kind == TouchAction::Motion && out.back().x == 180);
}

static void test_world_two_fingers_zoom_and_turn_the_camera_together() {
    TouchMapper m;
    m.set_hover(true);
    m.set_context(1);
    m.set_camera_gestures(true);
    std::vector<TouchAction> out;
    m.finger_down({1, 400, 300}, 0, &out);
    m.finger_down({2, 500, 300}, 20 * MS, &out);
    out.clear();
    m.finger_motion({2, 520, 300}, 40 * MS, &out); // past the pinch threshold
    auto c = camera_actions(out);
    // Begun at the midpoint where the pair landed.
    CHECK(c.size() == 2 && c[0].phase == 0 && c[0].x == 450 && c[0].y == 300);
    out.clear();
    m.finger_motion({2, 600, 300}, 60 * MS, &out); // spread to twice: half the distance
    c = camera_actions(out);
    CHECK(c.size() == 1 && c[0].phase == 1 && fabs(c[0].zoom - 0.5) < 1e-9 && c[0].x == 500);
    out.clear();
    m.finger_motion({2, 400, 100}, 80 * MS, &out); // the pair turned counterclockwise
    c = camera_actions(out);
    CHECK(c.size() == 1 && fabs(c[0].turn - M_PI / 2) < 1e-9);
    out.clear();
    m.finger_up({2, 400, 100}, 100 * MS, &out);
    c = camera_actions(out);
    CHECK(c.size() == 1 && c[0].phase == 2);
    m.finger_up({1, 400, 300}, 110 * MS, &out);
    m.tick(500 * MS, &out);
    CHECK(!has_button(out) && !has_key(out)); // neither a tap nor the turn keys
}

static void test_battle_two_fingers_zoom_and_pan_the_camera() {
    TouchMapper m;
    m.set_hover(true);
    m.set_context(2 | 32); // RECOMP_TOUCH_BATTLE | RECOMP_TOUCH_ZOOM_VIEW
    m.set_camera_gestures(true);
    std::vector<TouchAction> out;
    m.finger_down({1, 400, 300}, 0, &out);
    m.finger_down({2, 500, 300}, 20 * MS, &out);
    out.clear();
    m.finger_motion({2, 520, 300}, 40 * MS, &out); // past the pinch threshold
    auto c = camera_actions(out);
    CHECK(c.size() == 2 && c[0].phase == 0 && c[0].x == 450 && c[0].y == 300);
    out.clear();
    m.finger_motion({2, 600, 300}, 60 * MS, &out); // spread to twice: half the distance
    c = camera_actions(out);
    CHECK(c.size() == 1 && fabs(c[0].zoom - 0.5) < 1e-9 && c[0].x == 500);
    out.clear();
    m.finger_motion({1, 400, 400}, 80 * MS, &out); // the pair moved: the point follows
    m.finger_motion({2, 600, 400}, 90 * MS, &out);
    c = camera_actions(out);
    CHECK(!c.empty() && c.back().phase == 1 && c.back().y == 400);
    out.clear();
    m.finger_up({2, 600, 400}, 100 * MS, &out);
    c = camera_actions(out);
    CHECK(c.size() == 1 && c[0].phase == 2);
    m.finger_up({1, 400, 400}, 110 * MS, &out);
    m.tick(500 * MS, &out);
    CHECK(!has_button(out) && !has_key(out)); // neither a tap nor the arrow keys

    // Without the zoom flag (a list under the fingers, a modal window) two
    // fingers are what they were: no camera.
    TouchMapper plain;
    plain.set_hover(true);
    plain.set_context(2);
    plain.set_camera_gestures(true);
    out.clear();
    plain.finger_down({1, 400, 300}, 0, &out);
    plain.finger_down({2, 500, 300}, 20 * MS, &out);
    plain.finger_motion({2, 600, 300}, 40 * MS, &out);
    CHECK(camera_actions(out).empty());
}

static void test_hover_drag_waits_for_the_game_to_see_the_finger() {
    TouchMapper m;
    m.set_hover(true);
    m.frames_presented(10);
    m.set_context(0); // stale: still the button the last tap pressed
    std::vector<TouchAction> out;
    m.finger_down({1, 100, 100}, 0, &out);
    m.finger_motion({1, 150, 100}, 30 * MS, &out);
    CHECK(!has_button(out)); // no frame yet with the cursor here: undecided
    m.frames_presented(12);
    m.set_context(1); // now the game reports the map under the finger
    out.clear();
    m.tick(60 * MS, &out);
    CHECK(out.size() == 3 && out[1].kind == TouchAction::Button && out[1].button == 2 && out[1].down);
}

static void test_double_tap_lands_on_the_first_point() {
    TouchMapper m;
    m.set_hover(true);
    std::vector<TouchAction> out;
    m.finger_down({1, 300, 300}, 0, &out);
    m.finger_up({1, 300, 300}, 80 * MS, &out);
    m.tick(200 * MS, &out);
    m.tick(400 * MS, &out);
    out.clear();
    m.finger_down({1, 309, 294}, 420 * MS, &out); // a finger never lands twice on one pixel
    m.finger_up({1, 309, 294}, 490 * MS, &out);
    m.tick(600 * MS, &out);
    m.tick(900 * MS, &out);
    int downs = 0;
    for (const TouchAction &a : out)
        if (a.kind == TouchAction::Button && a.down) {
            CHECK(a.x == 300 && a.y == 300);
            ++downs;
        }
    CHECK(downs == 1);
    out.clear();
    m.finger_down({1, 500, 300}, 2000 * MS, &out); // later and elsewhere: its own point
    m.finger_up({1, 500, 300}, 2080 * MS, &out);
    m.tick(2200 * MS, &out);
    bool own = false;
    for (const TouchAction &a : out)
        own |= a.kind == TouchAction::Button && a.down && a.x == 500;
    CHECK(own);
}

// A hover drag of the interface (a stack, an artifact) is not snapped onto the
// window's edges: near the bottom the dragged stack stays under the finger
// instead of jumping onto the last row.
static void test_hover_ui_drag_is_not_snapped_to_an_edge() {
    TouchMapper m;
    m.set_hover(true);
    m.set_bounds(1000, 800);
    m.set_edge_insets(0, 0, 0, 21);
    m.set_context(0);
    std::vector<TouchAction> out;
    m.finger_down({1, 500, 700}, 0, &out);
    m.finger_motion({1, 500, 740}, 50 * MS, &out);
    out.clear();
    m.finger_motion({1, 500, 785}, 100 * MS, &out); // inside 16 + 21 of the bottom
    CHECK(out.size() == 1 && out[0].kind == TouchAction::Motion && out[0].y == 785);
    out.clear();
    m.finger_up({1, 500, 790}, 150 * MS, &out);
    bool released_there = false;
    for (const TouchAction &a : out)
        released_there |= a.kind == TouchAction::Button && !a.down && a.y == 790;
    CHECK(released_there);
}

static bool has_key(const std::vector<TouchAction> &out, int scancode, bool down) {
    for (const TouchAction &a : out)
        if (a.kind == TouchAction::Key && a.scancode == scancode && a.down == down)
            return true;
    return false;
}

// A stack dragged and held still holds Shift, which outlives the drop by the
// frames that see the button go up: the game reads Shift at the drop and
// opens its split window.
static void test_hover_resting_stack_drag_holds_shift_through_the_drop() {
    TouchMapper m;
    m.set_hover(true);
    m.set_context(256); // RECOMP_TOUCH_DRAG_SPLIT
    std::vector<TouchAction> out;
    m.finger_down({1, 100, 100}, 0, &out);
    m.finger_motion({1, 160, 100}, 50 * MS, &out);
    double x = 0, y = 0, p = 0;
    m.tick(50 * MS + kTouchSplitShowNs - 1, &out);
    CHECK(!m.split_ring(50 * MS + kTouchSplitShowNs - 1, &x, &y, &p)); // a passing pause
    CHECK(m.split_ring(50 * MS + kTouchSplitShowNs + 100 * MS, &x, &y, &p) && x == 160 && p > 0 &&
          p < 1);
    out.clear();
    m.tick(50 * MS + kTouchSplitHoldNs - 1, &out);
    CHECK(!has_key(out, SDL_SCANCODE_LSHIFT, true));
    m.finger_motion({1, 165, 103}, 300 * MS, &out); // a tremor keeps the rest
    m.tick(50 * MS + kTouchSplitHoldNs, &out);
    CHECK(has_key(out, SDL_SCANCODE_LSHIFT, true));
    CHECK(m.split_ring(50 * MS + kTouchSplitHoldNs + 10 * MS, &x, &y, &p) && p == 1);
    m.frames_presented(20);
    out.clear();
    m.finger_up({1, 165, 103}, 1000 * MS, &out);
    CHECK(has_button(out) && !has_key(out, SDL_SCANCODE_LSHIFT, false));
    out.clear();
    m.tick(1000 * MS + kTouchClickHoldNs, &out);
    CHECK(out.empty()); // no frame has seen the drop yet
    m.frames_presented(22);
    m.tick(1000 * MS + kTouchClickHoldNs, &out);
    CHECK(out.size() == 1 && has_key(out, SDL_SCANCODE_LSHIFT, false));
}

// Moving on after Shift went down lets it go: the drag is a plain move again
// until the finger rests anew.
static void test_hover_split_shift_goes_when_the_finger_moves_on() {
    TouchMapper m;
    m.set_hover(true);
    m.set_context(256);
    std::vector<TouchAction> out;
    m.finger_down({1, 100, 100}, 0, &out);
    m.finger_motion({1, 160, 100}, 50 * MS, &out);
    m.tick(50 * MS + kTouchSplitHoldNs, &out);
    CHECK(has_key(out, SDL_SCANCODE_LSHIFT, true));
    out.clear();
    m.finger_motion({1, 260, 100}, 50 * MS + kTouchSplitHoldNs + 50 * MS, &out);
    CHECK(has_key(out, SDL_SCANCODE_LSHIFT, false));
    out.clear();
    m.finger_up({1, 260, 100}, 50 * MS + kTouchSplitHoldNs + 100 * MS, &out);
    m.tick(2000 * MS, &out);
    CHECK(!has_key(out, SDL_SCANCODE_LSHIFT, false)); // nothing left to release
}

// Only a drag the game says splits: an artifact's (no flag) never holds Shift,
// and a second finger that ends a drag lets Shift go with the button.
static void test_hover_split_needs_the_game_and_ends_with_the_drag() {
    TouchMapper m;
    m.set_hover(true);
    m.set_context(0);
    std::vector<TouchAction> out;
    m.finger_down({1, 100, 100}, 0, &out);
    m.finger_motion({1, 160, 100}, 50 * MS, &out);
    m.tick(2000 * MS, &out);
    CHECK(!has_key(out, SDL_SCANCODE_LSHIFT, true));
    m.finger_up({1, 160, 100}, 2100 * MS, &out);
    m.tick(3000 * MS, &out);
    out.clear();
    m.set_context(256);
    m.finger_down({2, 100, 100}, 4000 * MS, &out);
    m.finger_motion({2, 160, 100}, 4050 * MS, &out);
    m.tick(4050 * MS + kTouchSplitHoldNs, &out);
    CHECK(has_key(out, SDL_SCANCODE_LSHIFT, true));
    out.clear();
    m.finger_down({3, 400, 400}, 4050 * MS + kTouchSplitHoldNs + 50 * MS, &out);
    CHECK(has_button(out) && has_key(out, SDL_SCANCODE_LSHIFT, false));
}

// --- native gestures (runtime/native_seam.h recomp_touch_event) -------------

#include "../../runtime/native_seam.h"

static bool native_is(const TouchAction &a, int kind, int phase, double x, double y) {
    return a.kind == TouchAction::Native && a.gesture == kind && a.phase == phase && a.x == x &&
           a.y == y;
}

static TouchMapper native_mapper() {
    TouchMapper m;
    m.set_native(true);
    m.frames_presented(10);
    return m;
}

static void test_native_tap_is_down_then_tap_with_no_cursor() {
    TouchMapper m = native_mapper();
    std::vector<TouchAction> out;
    m.finger_down({1, 100, 120}, 0, &out);
    CHECK(out.size() == 1 && native_is(out[0], RECOMP_TOUCH_EVENT_DOWN, RECOMP_TOUCH_PHASE_BEGIN, 100, 120));
    out.clear();
    m.finger_up({1, 103, 121}, 40 * MS, &out);
    CHECK(out.size() == 1 && native_is(out[0], RECOMP_TOUCH_EVENT_TAP, RECOMP_TOUCH_PHASE_END, 103, 121) &&
          out[0].button == 1);
    out.clear();
    m.frames_presented(20);
    m.tick(500 * MS, &out);
    CHECK(out.empty()); // nothing deferred: no press, no release, no placement
}

static void test_native_double_tap_counts_two_at_the_first_point() {
    TouchMapper m = native_mapper();
    std::vector<TouchAction> out;
    m.finger_down({1, 100, 100}, 0, &out);
    m.finger_up({1, 100, 100}, 40 * MS, &out);
    out.clear();
    m.finger_down({2, 110, 108}, 200 * MS, &out);
    m.finger_up({2, 110, 108}, 240 * MS, &out);
    CHECK(out.size() == 2 && native_is(out[1], RECOMP_TOUCH_EVENT_TAP, RECOMP_TOUCH_PHASE_END, 100, 100) &&
          out[1].button == 2);
    out.clear();
    m.finger_down({3, 300, 100}, 400 * MS, &out); // elsewhere: a single tap again
    m.finger_up({3, 300, 100}, 440 * MS, &out);
    CHECK(out.size() == 2 && out[1].gesture == RECOMP_TOUCH_EVENT_TAP && out[1].button == 1);
}

static void test_native_rest_is_a_press_that_follows_and_ends() {
    TouchMapper m = native_mapper();
    std::vector<TouchAction> out;
    m.finger_down({1, 100, 100}, 0, &out);
    out.clear();
    m.finger_motion({1, 104, 100}, 100 * MS, &out); // tremor: nothing
    m.tick(kTouchLongPressNs, &out);
    CHECK(out.size() == 1 && native_is(out[0], RECOMP_TOUCH_EVENT_PRESS, RECOMP_TOUCH_PHASE_BEGIN, 104, 100));
    out.clear();
    m.finger_motion({1, 110, 100}, 500 * MS, &out); // still within the tremor
    CHECK(out.empty());
    m.finger_motion({1, 160, 130}, 600 * MS, &out);
    CHECK(out.size() == 1 && native_is(out[0], RECOMP_TOUCH_EVENT_PRESS, RECOMP_TOUCH_PHASE_MOVE, 160, 130));
    out.clear();
    m.finger_up({1, 160, 130}, 700 * MS, &out);
    CHECK(out.size() == 1 && native_is(out[0], RECOMP_TOUCH_EVENT_PRESS, RECOMP_TOUCH_PHASE_END, 160, 130));
}

static void test_native_drag_waits_for_the_context_then_drags() {
    TouchMapper m = native_mapper();
    std::vector<TouchAction> out;
    m.finger_down({1, 100, 100}, 0, &out);
    out.clear();
    m.finger_motion({1, 140, 100}, 50 * MS, &out); // the game has not looked yet
    CHECK(out.empty());
    m.frames_presented(12);
    m.tick(60 * MS, &out);
    CHECK(out.size() == 2 && native_is(out[0], RECOMP_TOUCH_EVENT_DRAG, RECOMP_TOUCH_PHASE_BEGIN, 100, 100) &&
          native_is(out[1], RECOMP_TOUCH_EVENT_DRAG, RECOMP_TOUCH_PHASE_MOVE, 140, 100));
    out.clear();
    m.finger_motion({1, 180, 110}, 80 * MS, &out);
    CHECK(out.size() == 1 && native_is(out[0], RECOMP_TOUCH_EVENT_DRAG, RECOMP_TOUCH_PHASE_MOVE, 180, 110));
    out.clear();
    m.finger_up({1, 190, 110}, 100 * MS, &out);
    CHECK(!out.empty() && native_is(out[0], RECOMP_TOUCH_EVENT_DRAG, RECOMP_TOUCH_PHASE_END, 190, 110));
    for (const TouchAction &a : out)
        CHECK(a.kind != TouchAction::Button && a.kind != TouchAction::Motion);
}

static void test_native_drag_on_a_world_map_is_still_the_camera() {
    TouchMapper m = native_mapper();
    m.set_camera_gestures(true);
    m.set_context(RECOMP_TOUCH_WORLD);
    std::vector<TouchAction> out;
    m.finger_down({1, 100, 100}, 0, &out);
    out.clear();
    m.frames_presented(12);
    m.finger_motion({1, 150, 100}, 50 * MS, &out);
    CHECK(out.size() == 2 && out[0].kind == TouchAction::Camera && out[0].phase == 0 &&
          out[1].kind == TouchAction::Camera);
    out.clear();
    m.finger_up({1, 160, 100}, 90 * MS, &out);
    for (const TouchAction &a : out)
        CHECK(a.kind == TouchAction::Camera); // no tap, no placement after the pan
}

static void test_native_list_drag_scrolls() {
    TouchMapper m = native_mapper();
    m.set_context(RECOMP_TOUCH_LIST);
    std::vector<TouchAction> out;
    m.finger_down({1, 100, 100}, 0, &out);
    out.clear();
    m.frames_presented(12);
    m.finger_motion({1, 102, 150}, 50 * MS, &out);
    CHECK(!out.empty() && out[0].kind == TouchAction::Wheel);
    out.clear();
    m.finger_up({1, 102, 150}, 90 * MS, &out);
    CHECK(out.empty());
}

static void test_native_two_finger_tap_is_a_right_tap() {
    TouchMapper m = native_mapper();
    std::vector<TouchAction> out;
    m.finger_down({1, 100, 100}, 0, &out);
    m.finger_down({2, 200, 100}, 20 * MS, &out);
    out.clear();
    m.finger_up({2, 200, 100}, 80 * MS, &out);
    m.finger_up({1, 100, 100}, 90 * MS, &out);
    CHECK(out.size() == 1 && out[0].kind == TouchAction::Native &&
          out[0].gesture == RECOMP_TOUCH_EVENT_RIGHT_TAP);
}

static void test_native_tap_beside_a_rest_right_taps_at_the_rest_and_ends_it() {
    TouchMapper m = native_mapper();
    std::vector<TouchAction> out;
    m.finger_down({1, 100, 100}, 0, &out);
    m.tick(kTouchLongPressNs, &out);
    out.clear();
    m.finger_down({2, 300, 100}, 500 * MS, &out);
    m.finger_up({2, 300, 100}, 560 * MS, &out);
    CHECK(out.size() == 1 && native_is(out[0], RECOMP_TOUCH_EVENT_RIGHT_TAP, RECOMP_TOUCH_PHASE_END, 100, 100));
    out.clear();
    m.finger_up({1, 100, 100}, 900 * MS, &out);
    CHECK(out.size() == 1 && native_is(out[0], RECOMP_TOUCH_EVENT_PRESS, RECOMP_TOUCH_PHASE_END, 100, 100));
}

static void test_native_cancel_ends_a_drag_and_a_press() {
    TouchMapper m = native_mapper();
    std::vector<TouchAction> out;
    m.finger_down({1, 100, 100}, 0, &out);
    m.tick(kTouchLongPressNs, &out);
    out.clear();
    m.finger_cancel(1, &out);
    CHECK(out.size() == 1 && out[0].gesture == RECOMP_TOUCH_EVENT_PRESS &&
          out[0].phase == RECOMP_TOUCH_PHASE_CANCEL);
    out.clear();
    m.finger_down({2, 100, 100}, 2000 * MS, &out);
    m.frames_presented(12);
    m.finger_motion({2, 160, 100}, 2050 * MS, &out);
    out.clear();
    m.cancel_all(&out);
    CHECK(out.size() == 1 && out[0].gesture == RECOMP_TOUCH_EVENT_DRAG &&
          out[0].phase == RECOMP_TOUCH_PHASE_CANCEL);
}

int main() {
    test_hover_ui_drag_is_not_snapped_to_an_edge();
    test_hover_resting_stack_drag_holds_shift_through_the_drop();
    test_hover_split_shift_goes_when_the_finger_moves_on();
    test_hover_split_needs_the_game_and_ends_with_the_drag();
    test_double_tap_lands_on_the_first_point();
    test_hover_drag_waits_for_the_game_to_see_the_finger();
    test_hover_state_follows_the_finger();
    test_world_drag_is_a_wheel_button_drag();
    test_world_pan_can_be_inverted_and_stays_inside();
    test_ui_drag_stays_a_left_drag_on_hover();
    test_battle_tap_previews_then_confirms();
    test_battle_drag_aims_and_a_tap_on_it_acts();
    test_world_pinch_zooms_with_the_wheel();
    test_world_turn_holds_the_rotate_key_while_turning();
    test_world_drag_moves_the_camera_when_the_game_takes_it();
    test_world_two_fingers_zoom_and_turn_the_camera_together();
    test_battle_two_fingers_zoom_and_pan_the_camera();
    test_hover_touch_places_at_once_and_a_tap_still_clicks();
    test_hover_rest_lifts_without_a_click();
    test_hover_rested_finger_moves_the_cursor_without_buttons();
    test_hover_quick_drag_is_still_a_left_drag();
    test_hover_second_finger_tap_right_clicks_at_the_first();
    test_hover_two_fingers_together_are_still_a_two_finger_tap();
    test_hover_slow_second_finger_is_not_a_tap();
    test_tap_places_one_presented_frame_before_pressing();
    test_tap_without_presents_presses_after_60_ms();
    test_a_new_finger_finishes_a_pending_press_first();
    test_focus_loss_discards_a_pending_press();
    test_smoke_tap_drives_the_mapper();
    test_the_edge_outlasts_the_pointer_glide_off_the_strip();
    test_release_lands_where_the_press_did_whatever_the_vector_did();
    test_a_pointer_against_a_system_strip_means_the_edge();
    test_a_cancelled_drag_releases_where_it_was();
    test_edge_insets_widen_the_snap_on_that_edge_only();
    test_click_release_waits_for_two_presented_frames();
    test_click_release_waits_for_the_hold_time_even_when_frames_flew();
    test_a_stalled_game_still_gets_its_release();
    test_tap_is_left_click();
    test_a_new_finger_releases_a_held_click_first();
    test_long_press_is_right_click();
    test_long_press_then_drag_is_a_wheel_button_drag();
    test_edge_hold_scrolls_then_moves_the_cursor_inside();
    test_tap_near_an_edge_clicks_at_the_finger();
    test_no_bounds_means_no_snapping();
    test_origin_moves_the_edges_onto_the_game_rectangle();
    test_drag_is_left_drag();
    test_two_finger_drag_pans_with_arrows();
    test_two_finger_tap_is_right_click_three_is_f10_four_toggles_keyboard();
    test_second_finger_cancels_pending_tap();
    test_native_tap_is_down_then_tap_with_no_cursor();
    test_native_double_tap_counts_two_at_the_first_point();
    test_native_rest_is_a_press_that_follows_and_ends();
    test_native_drag_waits_for_the_context_then_drags();
    test_native_drag_on_a_world_map_is_still_the_camera();
    test_native_list_drag_scrolls();
    test_native_two_finger_tap_is_a_right_tap();
    test_native_tap_beside_a_rest_right_taps_at_the_rest_and_ends_it();
    test_native_cancel_ends_a_drag_and_a_press();
    if (g_failures) {
        fprintf(stderr, "%d failures\n", g_failures);
        return 1;
    }
    printf("input_touch_tests: ok\n");
    return 0;
}
