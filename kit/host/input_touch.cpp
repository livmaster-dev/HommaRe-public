// input_touch.cpp - see input_touch.h for the gesture table.
#include "input_touch.h"

#include <SDL3/SDL_scancode.h>
#include <math.h>

#include "../runtime/native_seam.h"

namespace {
void motion(std::vector<TouchAction> *out, double x, double y, bool place = true) {
    TouchAction a;
    a.kind = TouchAction::Motion;
    a.place = place;
    a.x = x;
    a.y = y;
    out->push_back(a);
}
void button(std::vector<TouchAction> *out, int b, bool down, double x, double y) {
    TouchAction a;
    a.kind = TouchAction::Button;
    a.button = b;
    a.down = down;
    a.x = x;
    a.y = y;
    out->push_back(a);
}
void key_tap(std::vector<TouchAction> *out, int scancode) {
    TouchAction a;
    a.kind = TouchAction::Key;
    a.scancode = scancode;
    a.down = true;
    out->push_back(a);
    a.down = false;
    out->push_back(a);
}
void key(std::vector<TouchAction> *out, int scancode, bool down) {
    TouchAction a;
    a.kind = TouchAction::Key;
    a.scancode = scancode;
    a.down = down;
    out->push_back(a);
}
double dist(double ax, double ay, double bx, double by) {
    return hypot(ax - bx, ay - by);
}
} // namespace

void TouchMapper::set_bounds(double w, double h) {
    bounds_w_ = w;
    bounds_h_ = h;
}

void TouchMapper::set_origin(double x, double y) {
    origin_x_ = x;
    origin_y_ = y;
}

double TouchMapper::centroid_x() const {
    double s = 0;
    for (const Finger &f : fingers_)
        s += f.x;
    return fingers_.empty() ? 0 : s / fingers_.size();
}
double TouchMapper::centroid_y() const {
    double s = 0;
    for (const Finger &f : fingers_)
        s += f.y;
    return fingers_.empty() ? 0 : s / fingers_.size();
}

void TouchMapper::set_edge_insets(double left, double top, double right, double bottom) {
    inset_l_ = left;
    inset_t_ = top;
    inset_r_ = right;
    inset_b_ = bottom;
}

// A placed motion: optionally snapped onto a window edge when the finger is
// within kTouchEdgeMargin of one, plus whatever strip the system keeps along
// that edge. Clicks keep the finger's position; holds and drags remember a snap.
void TouchMapper::place(std::vector<TouchAction> *out, double x, double y, bool snap) {
    bool at_edge = false;
    if (snap && bounds_w_ > 0 && bounds_h_ > 0) {
        const double rx = x - origin_x_, ry = y - origin_y_;
        if (rx < kTouchEdgeMargin + inset_l_) {
            x = origin_x_;
            at_edge = true;
        } else if (rx > bounds_w_ - 1 - kTouchEdgeMargin - inset_r_) {
            x = origin_x_ + bounds_w_ - 1;
            at_edge = true;
        }
        if (ry < kTouchEdgeMargin + inset_t_) {
            y = origin_y_;
            at_edge = true;
        } else if (ry > bounds_h_ - 1 - kTouchEdgeMargin - inset_b_) {
            y = origin_y_ + bounds_h_ - 1;
            at_edge = true;
        }
    }
    snapped_ = at_edge;
    placed_x_ = x;
    placed_y_ = y;
    motion(out, x, y);
}

// Give the game a frame to sample the placed cursor before it sees the press.
void TouchMapper::click(std::vector<TouchAction> *out, int b, double x, double y, uint64_t now,
                        bool chain) {
    if (b == 0) {
        if (chain && last_click_ && now - last_click_ns_ < kTouchDoubleTapNs &&
            dist(x, y, last_click_x_, last_click_y_) <= kTouchDoubleTapSlop) {
            x = last_click_x_; // a double (or triple) tap: the same point exactly
            y = last_click_y_;
        }
        last_click_ = true;
        last_click_x_ = x;
        last_click_y_ = y;
        last_click_ns_ = now;
    }
    place(out, x, y, false);
    // Copied out before the next push: a reference into the vector would not
    // survive the reallocation, and the release would carry whatever was left.
    const double px = out->back().x, py = out->back().y;
    press_pending_ = true;
    press_button_ = b;
    press_x_ = px;
    press_y_ = py;
    presents_at_place_ = presents_;
    press_deadline_ = now + kTouchPressDelayNs;
}

// The release hold starts when the press is emitted, not when it was placed.
void TouchMapper::press_held(std::vector<TouchAction> *out, uint64_t now) {
    press_pending_ = false;
    button(out, press_button_, true, press_x_, press_y_);
    release_pending_ = true;
    release_button_ = press_button_;
    release_x_ = press_x_;
    release_y_ = press_y_;
    release_due_ = now + kTouchClickHoldNs;
    release_deadline_ = now + kTouchClickHoldMaxNs;
    presents_at_press_ = presents_;
}

void TouchMapper::frames_presented(uint32_t count) {
    presents_known_ = true;
    presents_ = count;
}

bool TouchMapper::release_ready(uint64_t now) const {
    if (now < release_due_)
        return false;
    if (!presents_known_ || now >= release_deadline_)
        return true;
    return presents_ - presents_at_press_ >= kTouchClickHoldFrames;
}

// The gesture ended on an edge: once nothing is held any more, move the cursor
// back inside so the game stops scrolling.
void TouchMapper::end_edge_hold(std::vector<TouchAction> *out) {
    if (!snapped_)
        return;
    snapped_ = false;
    // Relative to the bounds' origin, and back.
    double x = placed_x_ - origin_x_, y = placed_y_ - origin_y_;
    if (x <= 0)
        x = kTouchEdgeRelease;
    else if (x >= bounds_w_ - 1)
        x = bounds_w_ - 1 - kTouchEdgeRelease;
    if (y <= 0)
        y = kTouchEdgeRelease;
    else if (y >= bounds_h_ - 1)
        y = bounds_h_ - 1 - kTouchEdgeRelease;
    x += origin_x_;
    y += origin_y_;
    if (release_pending_) {
        nudge_pending_ = true;
        nudge_x_ = x;
        nudge_y_ = y;
        return;
    }
    nudge_pending_ = false;
    motion(out, x, y);
}

void TouchMapper::release_held(std::vector<TouchAction> *out) {
    release_pending_ = false;
    button(out, release_button_, false, release_x_, release_y_);
    if (nudge_pending_) {
        nudge_pending_ = false;
        motion(out, nudge_x_, nudge_y_);
    }
}

// A hover drag that moved: the rest starts again where the finger is now, and
// a Shift taken for a split goes once the finger has left that spot.
void TouchMapper::split_rest(double x, double y, uint64_t now, std::vector<TouchAction> *out) {
    if (split_shift_) {
        if (dist(split_x_, split_y_, x, y) > kTouchSplitCancel) {
            split_release(out);
            split_x_ = x;
            split_y_ = y;
            split_t0_ = now;
        }
        return;
    }
    if (dist(split_x_, split_y_, x, y) > kTouchSplitSlop) {
        split_x_ = x;
        split_y_ = y;
        split_t0_ = now;
    }
}

// The rest has lasted while the game still drags a stack: Shift goes down. A
// Shift that outlived a drop goes up once the frames that saw the drop are in.
void TouchMapper::split_tick(uint64_t now, std::vector<TouchAction> *out) {
    if (split_up_pending_ && now >= split_up_due_ &&
        (!presents_known_ || now >= split_up_deadline_ ||
         presents_ - split_up_presents_ >= kTouchClickHoldFrames)) {
        split_up_pending_ = false;
        key(out, SDL_SCANCODE_LSHIFT, false);
    }
    if (!hover_drag_ || !dragging_ || split_shift_ || fingers_.size() != 1)
        return;
    if ((context_ & 256) && now - split_t0_ >= kTouchSplitHoldNs) {
        split_shift_ = true;
        key(out, SDL_SCANCODE_LSHIFT, true);
    }
}

// The finger lifted on a split: Shift stays down until the button's release
// has been seen (split_tick).
void TouchMapper::split_drop(uint64_t now) {
    if (!split_shift_)
        return;
    split_shift_ = false;
    split_up_pending_ = true;
    split_up_due_ = now + kTouchClickHoldNs;
    split_up_deadline_ = now + kTouchClickHoldMaxNs;
    split_up_presents_ = presents_;
}

// No drop to split (the drag was given up or moved on): Shift goes at once.
void TouchMapper::split_release(std::vector<TouchAction> *out) {
    if (!split_shift_)
        return;
    split_shift_ = false;
    key(out, SDL_SCANCODE_LSHIFT, false);
}

bool TouchMapper::split_ring(uint64_t now, double *x, double *y, double *progress) const {
    if (!hover_drag_ || !dragging_ || fingers_.size() != 1 || !(context_ & 256))
        return false;
    if (!split_shift_ && now - split_t0_ < kTouchSplitShowNs)
        return false;
    *x = fingers_[0].x;
    *y = fingers_[0].y;
    *progress = split_shift_ ? 1.0
                             : double(now - split_t0_ - kTouchSplitShowNs) /
                                   double(kTouchSplitHoldNs - kTouchSplitShowNs);
    if (*progress > 1)
        *progress = 1;
    return true;
}

void TouchMapper::native_event(std::vector<TouchAction> *out, int kind, int phase, double x,
                               double y, int count) {
    TouchAction a;
    a.kind = TouchAction::Native;
    a.gesture = kind;
    a.phase = phase;
    a.x = x;
    a.y = y;
    a.button = count;
    out->push_back(a);
}

// A tap soon after the last one and near it is its second (third...) tap, at
// the first one's point: the game's double click wants the same spot twice.
void TouchMapper::native_tap(std::vector<TouchAction> *out, double x, double y, uint64_t now) {
    int count = 1;
    if (native_taps_ && now - native_tap_ns_ < kTouchDoubleTapNs &&
        dist(x, y, native_tap_x_, native_tap_y_) <= kTouchDoubleTapSlop) {
        count = native_taps_ + 1;
        x = native_tap_x_;
        y = native_tap_y_;
    }
    native_taps_ = count;
    native_tap_x_ = x;
    native_tap_y_ = y;
    native_tap_ns_ = now;
    native_event(out, RECOMP_TOUCH_EVENT_TAP, RECOMP_TOUCH_PHASE_END, x, y, count);
}

void TouchMapper::reset_gesture() {
    native_drag_ = native_press_ = native_pressed_moved_ = false;
    max_fingers_ = 0;
    dragging_ = false;
    hover_drag_ = false;
    long_fired_ = false;
    middle_held_ = false;
    pan_virtual_ = false;
    scrubbing_ = false;
    anchor_ = anchor_used_ = false;
    aiming_ = false;
    scrolling_ = false;
    drag_pending_ = false;
    two_ = Two::None;
    cam_used_ = false;
    zoom_acc_ = twist_acc_ = 0;
    pan_acc_x_ = pan_acc_y_ = 0;
}

// A hover pan is the game's own wheel-button drag, driven by a virtual pointer
// rather than the finger: the game reads only how far the pointer moves, and a
// game whose camera answers against the finger needs it to move the other way,
// which the finger's own point could not do without leaving the window. The
// virtual pointer starts at the middle of the game image; when it nears an
// edge it is placed back in the middle, and a placement drops the motion of
// that jump (TouchAction::place), so only the finger's own travel counts.
void TouchMapper::pan_begin(std::vector<TouchAction> *out, double fx, double fy) {
    pan_vx_ = bounds_w_ > 0 ? origin_x_ + bounds_w_ / 2 : fx;
    pan_vy_ = bounds_h_ > 0 ? origin_y_ + bounds_h_ / 2 : fy;
    pan_fx_ = fx;
    pan_fy_ = fy;
    middle_held_ = true;
    pan_virtual_ = true;
    snapped_ = false;
    motion(out, pan_vx_, pan_vy_); // placed: no motion yet
    button(out, 2, true, pan_vx_, pan_vy_);
}

void TouchMapper::pan_move(std::vector<TouchAction> *out, double fx, double fy) {
    const double dx = (fx - pan_fx_) * (invert_pan_x_ ? -1 : 1);
    const double dy = (fy - pan_fy_) * (invert_pan_y_ ? -1 : 1);
    pan_fx_ = fx;
    pan_fy_ = fy;
    if (dx == 0 && dy == 0)
        return;
    double nx = pan_vx_ + dx, ny = pan_vy_ + dy;
    if (bounds_w_ > 0 && bounds_h_ > 0) {
        const double margin = kTouchPanMargin;
        if (nx < origin_x_ + margin || nx > origin_x_ + bounds_w_ - margin ||
            ny < origin_y_ + margin || ny > origin_y_ + bounds_h_ - margin) {
            pan_vx_ = origin_x_ + bounds_w_ / 2;
            pan_vy_ = origin_y_ + bounds_h_ / 2;
            motion(out, pan_vx_, pan_vy_); // placed: the jump back does not count
            nx = pan_vx_ + dx;
            ny = pan_vy_ + dy;
        }
    }
    pan_vx_ = nx;
    pan_vy_ = ny;
    motion(out, nx, ny, false);
}

// The pan is over: let go, then put the cursor back under the finger.
void TouchMapper::pan_end(std::vector<TouchAction> *out, double fx, double fy) {
    button(out, 2, false, pan_vx_, pan_vy_);
    middle_held_ = false;
    pan_virtual_ = false;
    place(out, fx, fy, false);
}

// A one-finger drag on hover: what it does depends on what the game says lies
// under the cursor, and the game says so only once it has drawn a frame with
// the cursor under the finger. Until then the drag waits (nothing is pressed).
void TouchMapper::scroll(std::vector<TouchAction> *out, double x, double y) {
    while (fabs(y - scroll_y_) >= kTouchScrollStep) {
        const double step = y > scroll_y_ ? kTouchScrollStep : -kTouchScrollStep;
        TouchAction w;
        w.kind = TouchAction::Wheel;
        w.x = fingers_.empty() ? x : fingers_[0].x0;
        w.y = fingers_.empty() ? y : fingers_[0].y0;
        w.wheel = step > 0 ? 1 : -1; // finger down: the list moves down, as up a notch
        out->push_back(w);
        scroll_y_ += step;
    }
}

void TouchMapper::start_hover_drag(uint64_t now, std::vector<TouchAction> *out) {
    if (presents_known_ && presents_ - presents_at_down_ < kTouchContextFrames)
        return;
    drag_pending_ = false;
    const Finger &f = fingers_[0];
    if ((context_ & 1) && camera_) { // RECOMP_TOUCH_WORLD: the ground follows the finger
        camera(out, 0, f.x0, f.y0, 1, 0);
        camera(out, 1, f.x, f.y, 1, 0);
    } else if (context_ & 1) { // RECOMP_TOUCH_WORLD: the map follows the finger
        pan_begin(out, f.x0, f.y0);
        pan_move(out, f.x, f.y);
    } else if ((context_ & 16) && fabs(f.y - f.y0) > fabs(f.x - f.x0)) {
        // RECOMP_TOUCH_LIST: an up or down drag over a list scrolls it, a
        // notch of the wheel per step the finger travels, the content
        // following the finger.
        scrolling_ = true;
        scroll_y_ = f.y0;
        scroll(out, f.x, f.y);
    } else if (native_) {
        // Native: the battlefield and the interface alike are the game's to
        // read (it aims on a field, drags in a window).
        dragging_ = hover_drag_ = native_drag_ = true;
        native_event(out, RECOMP_TOUCH_EVENT_DRAG, RECOMP_TOUCH_PHASE_BEGIN, f.x0, f.y0);
        native_event(out, RECOMP_TOUCH_EVENT_DRAG, RECOMP_TOUCH_PHASE_MOVE, f.x, f.y);
        split_x_ = f.x;
        split_y_ = f.y;
        split_t0_ = now;
    } else if (context_ & 2) { // RECOMP_TOUCH_BATTLE: aim without pressing
        aiming_ = true;
        hover_state_ = 0;
        place(out, f.x, f.y, false);
    } else { // the interface: a left drag
        // Not snapped onto the window's edges, as a hold or a classic drag is:
        // an interface has nothing that scrolls there, and a stack dragged
        // near the bottom would jump onto the last row, away from the finger.
        dragging_ = true;
        hover_drag_ = true;
        place(out, f.x0, f.y0, false);
        const double px = out->back().x, py = out->back().y;
        button(out, 0, true, px, py);
        place(out, f.x, f.y, false);
        split_x_ = f.x;
        split_y_ = f.y;
        split_t0_ = now;
    }
}

void TouchMapper::camera(std::vector<TouchAction> *out, int phase, double x, double y,
                         double zoom, double turn) {
    TouchAction a;
    a.kind = TouchAction::Camera;
    a.phase = phase;
    a.x = cam_x_ = x;
    a.y = cam_y_ = y;
    a.zoom = cam_zoom_ = zoom;
    a.turn = cam_turn_ = turn;
    out->push_back(a);
    cam_active_ = phase != 2;
    cam_used_ = true;
}

void TouchMapper::end_camera(std::vector<TouchAction> *out) {
    if (cam_active_)
        camera(out, 2, cam_x_, cam_y_, cam_zoom_, cam_turn_);
}

void TouchMapper::release_twist(std::vector<TouchAction> *out) {
    if (!twist_key_)
        return;
    TouchAction a;
    a.kind = TouchAction::Key;
    a.scancode = twist_key_;
    a.down = false;
    out->push_back(a);
    twist_key_ = 0;
}

static double wrap_angle(double a) {
    while (a > M_PI)
        a -= 2 * M_PI;
    while (a < -M_PI)
        a += 2 * M_PI;
    return a;
}

// Two fingers on a world map: a pinch zooms (the wheel), a turn rotates (the
// rotate key held while turning), a common move steps the camera (arrows).
void TouchMapper::two_finger_world(uint64_t now, std::vector<TouchAction> *out) {
    const Finger &a = fingers_[0], &b = fingers_[1];
    const double d = dist(a.x, a.y, b.x, b.y), ang = atan2(b.y - a.y, b.x - a.x);
    const double cx = centroid_x(), cy = centroid_y();
    if (two_ == Two::Camera) {
        // The game's camera follows the pair: the midpoint's ground stays under
        // it, the distance scales with the spread, the map turns with them.
        if (d > 1)
            camera(out, 1, cx, cy, two_d0_ / d, -wrap_angle(ang - two_a0_));
        return;
    }
    if (two_ == Two::None) {
        if (two_d0_ > 1 && fabs(log(d / two_d0_)) > log(1 + kTouchPinchStart))
            two_ = Two::Pinch;
        else if (fabs(wrap_angle(ang - two_a0_)) > kTouchTwistStart)
            two_ = Two::Twist;
        else if (dist(cx, cy, tap2_x_, tap2_y_) > kTouchPanStep)
            two_ = Two::Pan;
        if (two_ != Two::None && camera_) {
            anchor_ = false;
            two_ = Two::Camera;
            camera(out, 0, tap2_x_, tap2_y_, 1, 0);
            camera(out, 1, cx, cy, two_d0_ / d, -wrap_angle(ang - two_a0_));
            return;
        }
        if (two_ != Two::None) {
            anchor_ = false; // a gesture, not a tap beside a resting finger
            two_dprev_ = d;
            two_aprev_ = ang;
            pan_cx_ = cx;
            pan_cy_ = cy;
            pan_acc_x_ = pan_acc_y_ = 0;
        }
        return;
    }
    if (two_ == Two::Pinch && two_dprev_ > 1 && d > 1) {
        zoom_acc_ += log2(d / two_dprev_) * kTouchZoomNotches * (invert_zoom_ ? -1 : 1);
        two_dprev_ = d;
        if (fabs(zoom_acc_) >= 0.05) {
            TouchAction w;
            w.kind = TouchAction::Wheel;
            w.x = cx;
            w.y = cy;
            w.wheel = zoom_acc_;
            out->push_back(w);
            zoom_acc_ = 0;
        }
    } else if (two_ == Two::Twist) {
        twist_acc_ += wrap_angle(ang - two_aprev_) * (invert_rotate_ ? -1 : 1);
        two_aprev_ = ang;
        if (fabs(twist_acc_) >= kTouchTwistStep) {
            const int key = twist_acc_ > 0 ? SDL_SCANCODE_INSERT : SDL_SCANCODE_DELETE;
            if (twist_key_ != key) {
                release_twist(out);
                TouchAction k;
                k.kind = TouchAction::Key;
                k.scancode = key;
                k.down = true;
                out->push_back(k);
                twist_key_ = key;
            }
            twist_acc_ = 0;
            twist_moved_ns_ = now;
        }
    } else if (two_ == Two::Pan) {
        pan_acc_x_ += cx - pan_cx_;
        pan_acc_y_ += cy - pan_cy_;
        pan_cx_ = cx;
        pan_cy_ = cy;
        while (fabs(pan_acc_x_) >= kTouchPanStep || fabs(pan_acc_y_) >= kTouchPanStep) {
            if (fabs(pan_acc_x_) >= fabs(pan_acc_y_)) {
                key_tap(out, pan_acc_x_ > 0 ? SDL_SCANCODE_RIGHT : SDL_SCANCODE_LEFT);
                pan_acc_x_ -= pan_acc_x_ > 0 ? kTouchPanStep : -kTouchPanStep;
            } else {
                key_tap(out, pan_acc_y_ > 0 ? SDL_SCANCODE_DOWN : SDL_SCANCODE_UP);
                pan_acc_y_ -= pan_acc_y_ > 0 ? kTouchPanStep : -kTouchPanStep;
            }
        }
    }
}

void TouchMapper::finger_down(TouchPoint p, uint64_t now, std::vector<TouchAction> *out) {
    // Finish an earlier tap before starting this finger, even if its press
    // has not reached a presented frame yet. Both taps keep their own point.
    if (press_pending_)
        press_held(out, now);
    if (release_pending_) {
        // A new finger before the last click released: release it first so
        // the two clicks stay distinct.
        release_held(out);
    }
    if (fingers_.empty())
        reset_gesture();
    fingers_.push_back({p.id, p.x, p.y, p.x, p.y, now});
    if ((int)fingers_.size() > max_fingers_)
        max_fingers_ = (int)fingers_.size();
    if (native_ && fingers_.size() == 1) {
        // Native: the game hears where the finger is and works out what lies
        // there itself; nothing is placed.
        placed_x_ = p.x;
        placed_y_ = p.y;
        native_event(out, RECOMP_TOUCH_EVENT_DOWN, RECOMP_TOUCH_PHASE_BEGIN, p.x, p.y);
        hover_state_ = 1;
        presents_at_down_ = presents_;
        return;
    }
    if (hover_ && fingers_.size() == 1) {
        // Hover: the cursor goes under the finger at once, so the game shows
        // what is there; nothing is pressed until the finger lifts. No
        // tooltip either, unless the finger rests.
        place(out, p.x, p.y, false);
        hover_state_ = 1;
        presents_at_down_ = presents_;
        return;
    }
    if (hover_ && fingers_.size() == 2 && !dragging_ && !anchor_used_ &&
        (long_fired_ || now - fingers_[0].t0 >= kTouchAnchorNs)) {
        // A tap beside a resting finger: right click where that finger is.
        anchor_ = true;
        anchor_tap_id_ = p.id;
        anchor_tap_t0_ = now;
        anchor_tap_x0_ = p.x;
        anchor_tap_y0_ = p.y;
    }
    if (fingers_.size() == 2) {
        // A second finger ends any one-finger gesture: release a drag, forget a tap.
        end_camera(out);
        if (native_drag_) {
            native_event(out, RECOMP_TOUCH_EVENT_DRAG, RECOMP_TOUCH_PHASE_END, fingers_[0].x,
                         fingers_[0].y);
            split_release(out);
            native_drag_ = dragging_ = false;
        } else if (dragging_) {
            button(out, 0, false, fingers_[0].x, fingers_[0].y);
            split_release(out);
            dragging_ = false;
        }
        if (middle_held_) {
            button(out, 2, false, pan_virtual_ ? pan_vx_ : fingers_[0].x,
                   pan_virtual_ ? pan_vy_ : fingers_[0].y);
            middle_held_ = false;
        }
        pan_cx_ = centroid_x();
        pan_cy_ = centroid_y();
        pan_acc_x_ = pan_acc_y_ = 0;
        tap2_x_ = pan_cx_;
        tap2_y_ = pan_cy_;
        two_ = Two::None;
        two_d0_ = two_dprev_ = dist(fingers_[0].x, fingers_[0].y, fingers_[1].x, fingers_[1].y);
        two_a0_ = two_aprev_ = atan2(fingers_[1].y - fingers_[0].y, fingers_[1].x - fingers_[0].x);
    }
}

void TouchMapper::finger_motion(TouchPoint p, uint64_t now, std::vector<TouchAction> *out) {
    for (Finger &f : fingers_)
        if (f.id == p.id) {
            f.x = p.x;
            f.y = p.y;
        }
    if (fingers_.size() == 1 && max_fingers_ == 1) {
        Finger &f = fingers_[0];
        if (cam_active_) {
            camera(out, 1, f.x, f.y, 1, 0);
            return;
        }
        if (middle_held_) {
            if (pan_virtual_)
                pan_move(out, f.x, f.y);
            else
                motion(out, f.x, f.y, false); // wheel-button drag: deltas only
            return;
        }
        if (aiming_) {
            place(out, f.x, f.y, false);
            return;
        }
        if (scrolling_) {
            scroll(out, f.x, f.y);
            return;
        }
        if (native_press_) {
            // A rested finger that moves: the game follows it (what is under
            // it now). Its tremor is not sent.
            if (!native_pressed_moved_ && dist(f.x0, f.y0, f.x, f.y) > kTouchTapTravel)
                native_pressed_moved_ = true;
            if (native_pressed_moved_)
                native_event(out, RECOMP_TOUCH_EVENT_PRESS, RECOMP_TOUCH_PHASE_MOVE, f.x, f.y);
            return;
        }
        if (native_drag_) {
            native_event(out, RECOMP_TOUCH_EVENT_DRAG, RECOMP_TOUCH_PHASE_MOVE, f.x, f.y);
            split_rest(f.x, f.y, now, out);
            return;
        }
        if (hover_ && long_fired_) {
            // Hover: the cursor follows a rested finger once it really moves.
            // A resting finger's tremor is not sent: any cursor motion restarts
            // the game's hover timer.
            if (!scrubbing_ && dist(f.x0, f.y0, f.x, f.y) > kTouchTapTravel)
                scrubbing_ = true;
            if (scrubbing_)
                place(out, f.x, f.y, false);
            return;
        }
        if (long_fired_) {
            // A drag after the long press scrolls the map the way the game's
            // own wheel-button drag does. The press point was placed already.
            if (dist(f.x0, f.y0, f.x, f.y) > kTouchTapTravel) {
                middle_held_ = true;
                snapped_ = false;
                button(out, 2, true, f.x0, f.y0);
                motion(out, f.x, f.y, false);
            }
            return;
        }
        if (hover_ && !dragging_ && (drag_pending_ || dist(f.x0, f.y0, f.x, f.y) > kTouchTapTravel)) {
            drag_pending_ = true;
            start_hover_drag(now, out);
            return;
        }
        if (!dragging_ && dist(f.x0, f.y0, f.x, f.y) > kTouchTapTravel) {
            dragging_ = true;
            place(out, f.x0, f.y0);
            const double px = out->back().x, py = out->back().y;
            button(out, 0, true, px, py);
        }
        if (dragging_) {
            place(out, f.x, f.y, !hover_drag_);
            if (hover_drag_)
                split_rest(f.x, f.y, now, out);
        }
        return;
    }
    // Two fingers on a world map, or on a battlefield whose camera the game
    // lets the fingers zoom and pan (RECOMP_TOUCH_ZOOM_VIEW; the turn they make
    // there is ignored by the game's adapter).
    if (hover_ && fingers_.size() == 2 && max_fingers_ == 2 &&
        ((context_ & 1) || ((context_ & 32) && camera_) || two_ != Two::None)) {
        two_finger_world(now, out);
        return;
    }
    if (fingers_.size() == 2 && max_fingers_ == 2) {
        const double cx = centroid_x(), cy = centroid_y();
        pan_acc_x_ += cx - pan_cx_;
        pan_acc_y_ += cy - pan_cy_;
        pan_cx_ = cx;
        pan_cy_ = cy;
        while (fabs(pan_acc_x_) >= kTouchPanStep || fabs(pan_acc_y_) >= kTouchPanStep) {
            if (fabs(pan_acc_x_) >= fabs(pan_acc_y_)) {
                key_tap(out, pan_acc_x_ > 0 ? SDL_SCANCODE_RIGHT : SDL_SCANCODE_LEFT);
                pan_acc_x_ -= pan_acc_x_ > 0 ? kTouchPanStep : -kTouchPanStep;
            } else {
                key_tap(out, pan_acc_y_ > 0 ? SDL_SCANCODE_DOWN : SDL_SCANCODE_UP);
                pan_acc_y_ -= pan_acc_y_ > 0 ? kTouchPanStep : -kTouchPanStep;
            }
        }
    }
}

void TouchMapper::finger_up(TouchPoint p, uint64_t now, std::vector<TouchAction> *out) {
    Finger lifted{p.id, p.x, p.y, p.x, p.y, now};
    for (size_t i = 0; i < fingers_.size(); ++i)
        if (fingers_[i].id == p.id) {
            lifted = fingers_[i];
            lifted.x = p.x;
            lifted.y = p.y;
            fingers_.erase(fingers_.begin() + (long)i);
            break;
        }
    if (fingers_.size() < 2) {
        release_twist(out);
        if (cam_active_ && max_fingers_ == 1)
            camera(out, 1, lifted.x, lifted.y, 1, 0);
        end_camera(out);
    }
    if (anchor_ && lifted.id == anchor_tap_id_) {
        // The finger beside the resting one lifted: a quick still tap right-
        // clicks at the resting finger. Either way the gesture clicks no more.
        anchor_ = false;
        anchor_used_ = true;
        if (fingers_.size() == 1 && now - anchor_tap_t0_ < kTouchSecondTapNs &&
            dist(anchor_tap_x0_, anchor_tap_y0_, lifted.x, lifted.y) <= kTouchTapTravel) {
            if (native_)
                native_event(out, RECOMP_TOUCH_EVENT_RIGHT_TAP, RECOMP_TOUCH_PHASE_END,
                             fingers_[0].x, fingers_[0].y);
            else
                click(out, 1, fingers_[0].x, fingers_[0].y, now);
        }
    }
    if (!fingers_.empty())
        return; // the gesture ends when the last finger lifts
    const bool moved = dist(lifted.x0, lifted.y0, lifted.x, lifted.y) > kTouchTapTravel;
    if (native_ && native_press_ && max_fingers_ > 1 && !anchor_used_) {
        // Fingers joined a rest for a gesture of their own: the press is over.
        native_event(out, RECOMP_TOUCH_EVENT_PRESS, RECOMP_TOUCH_PHASE_END, lifted.x, lifted.y);
        native_press_ = false;
    }
    if (native_ && (max_fingers_ == 1 || anchor_used_)) {
        // Native: what the finger did, for the game to act on. A camera drag
        // and a scrolled list are done already.
        if (anchor_used_) {
            // The rested finger lifts after its right tap: the press is over.
            if (native_press_)
                native_event(out, RECOMP_TOUCH_EVENT_PRESS, RECOMP_TOUCH_PHASE_END, lifted.x,
                             lifted.y);
        } else if (native_press_) {
            native_event(out, RECOMP_TOUCH_EVENT_PRESS, RECOMP_TOUCH_PHASE_END, lifted.x, lifted.y);
            if (context_ & 128) { // RECOMP_TOUCH_BATTLE_DIRECT: a rest says "only walk here"
                TouchAction g;
                g.kind = TouchAction::Gesture;
                g.gesture = 1; // RECOMP_TOUCH_GESTURE_HOLD
                out->push_back(g);
            }
        } else if (native_drag_) {
            native_event(out, RECOMP_TOUCH_EVENT_DRAG, RECOMP_TOUCH_PHASE_END, lifted.x, lifted.y);
            split_drop(now);
        } else if (middle_held_ && pan_virtual_) {
            pan_move(out, lifted.x, lifted.y);
            button(out, 2, false, pan_vx_, pan_vy_);
            middle_held_ = pan_virtual_ = false;
        } else if (!scrolling_ && !cam_used_ && !drag_pending_ && !moved &&
                   now - lifted.t0 < kTouchLongPressNs) {
            native_tap(out, lifted.x, lifted.y, now);
        }
        reset_gesture();
        return;
    }
    if (anchor_used_) {
        // The resting finger lifts after its right click: nothing more.
    } else if (max_fingers_ == 1 && scrolling_) {
        // A list was scrolled: no click where the finger lifts.
    } else if (max_fingers_ == 1 && hover_ && long_fired_) {
        // Hover: a finger that rested lifts without a click. The cursor stays
        // where it was, and with it whatever the game shows there. A
        // battlefield taking direct taps (RECOMP_TOUCH_BATTLE_DIRECT) hears
        // of it: there a rest says "only walk here".
        if (context_ & 128) {
            TouchAction g;
            g.kind = TouchAction::Gesture;
            g.gesture = 1; // RECOMP_TOUCH_GESTURE_HOLD
            out->push_back(g);
        }
    } else if (max_fingers_ == 1 && aiming_) {
        // Battlefield: an aimed point is a preview; a tap on it acts.
        preview_ = true;
        preview_x_ = placed_x_;
        preview_y_ = placed_y_;
    } else if (max_fingers_ == 1 && hover_ && (context_ & 2) && !moved &&
               now - lifted.t0 < kTouchLongPressNs) {
        // Battlefield tap: the first one previews, a second on the same spot acts.
        // A game that confirms in its own way (RECOMP_TOUCH_BATTLE_DIRECT: a
        // target, then the cell to strike it from) gets each tap as a click,
        // never chained into a double click with the one before.
        if (context_ & 128) {
            preview_ = false;
            hover_state_ = 1;
            click(out, 0, lifted.x, lifted.y, now, false);
            last_click_ = false; // nor the next tap (a cancel beside it) into it
        } else if (preview_ && dist(preview_x_, preview_y_, lifted.x, lifted.y) <= kTouchPreviewSlop) {
            preview_ = false;
            hover_state_ = 1;
            click(out, 0, preview_x_, preview_y_, now);
        } else {
            preview_ = true;
            preview_x_ = lifted.x;
            preview_y_ = lifted.y;
            hover_state_ = 0;
            place(out, lifted.x, lifted.y, false);
        }
    } else if (max_fingers_ == 1) {
        preview_ = false;
        if (cam_used_) {
            place(out, lifted.x, lifted.y, false); // a camera drag: the cursor back under it
        } else if (middle_held_ && pan_virtual_) {
            pan_move(out, lifted.x, lifted.y);
            pan_end(out, lifted.x, lifted.y);
        } else if (middle_held_) {
            motion(out, lifted.x, lifted.y, false);
            button(out, 2, false, lifted.x, lifted.y);
        } else if (dragging_) {
            place(out, lifted.x, lifted.y, !hover_drag_);
            const double px = out->back().x, py = out->back().y;
            button(out, 0, false, px, py);
            split_drop(now);
        } else if (long_fired_) {
            // A still long press that lifts is a right click, unless it was an
            // edge hold, which only scrolled.
            if (!moved && !snapped_)
                click(out, 1, lifted.x, lifted.y, now);
        } else if (!moved && now - lifted.t0 < kTouchLongPressNs) {
            click(out, 0, lifted.x, lifted.y, now);
        }
        end_edge_hold(out);
    } else if (max_fingers_ == 2 && two_ == Two::None && fabs(pan_acc_x_) < kTouchPanStep &&
               fabs(pan_acc_y_) < kTouchPanStep && !moved) {
        // Right click between the two fingers, where the second one landed.
        if (native_)
            native_event(out, RECOMP_TOUCH_EVENT_RIGHT_TAP, RECOMP_TOUCH_PHASE_END, tap2_x_, tap2_y_);
        else
            click(out, 1, tap2_x_, tap2_y_, now);
    } else if (max_fingers_ == 3 && !moved) {
        key_tap(out, SDL_SCANCODE_F10);
    } else if (max_fingers_ == 4 && !moved) {
        text_input_ = !text_input_;
    }
    reset_gesture();
}

void TouchMapper::finger_cancel(int64_t id, std::vector<TouchAction> *out) {
    for (size_t i = 0; i < fingers_.size(); ++i)
        if (fingers_[i].id == id) {
            fingers_.erase(fingers_.begin() + (long)i);
            break;
        }
    if (fingers_.size() < 2) {
        release_twist(out);
        end_camera(out);
    }
    if (!fingers_.empty())
        return;
    // Let go where the cursor was placed: a release carries a position, and a
    // release at the origin would put the game's cursor there.
    if (native_drag_)
        native_event(out, RECOMP_TOUCH_EVENT_DRAG, RECOMP_TOUCH_PHASE_CANCEL, placed_x_, placed_y_);
    else if (dragging_)
        button(out, 0, false, placed_x_, placed_y_);
    if (native_press_)
        native_event(out, RECOMP_TOUCH_EVENT_PRESS, RECOMP_TOUCH_PHASE_CANCEL, placed_x_, placed_y_);
    split_release(out);
    if (middle_held_)
        button(out, 2, false, pan_virtual_ ? pan_vx_ : placed_x_, pan_virtual_ ? pan_vy_ : placed_y_);
    end_edge_hold(out);
    reset_gesture();
}

void TouchMapper::cancel_all(std::vector<TouchAction> *out) {
    press_pending_ = false; // no new click after focus loss or backgrounding
    if (release_pending_)
        release_held(out);
    if (native_drag_)
        native_event(out, RECOMP_TOUCH_EVENT_DRAG, RECOMP_TOUCH_PHASE_CANCEL, placed_x_, placed_y_);
    else if (dragging_)
        button(out, 0, false, placed_x_, placed_y_);
    if (native_press_)
        native_event(out, RECOMP_TOUCH_EVENT_PRESS, RECOMP_TOUCH_PHASE_CANCEL, placed_x_, placed_y_);
    split_release(out);
    if (split_up_pending_) {
        split_up_pending_ = false;
        key(out, SDL_SCANCODE_LSHIFT, false);
    }
    if (middle_held_)
        button(out, 2, false, pan_virtual_ ? pan_vx_ : placed_x_, pan_virtual_ ? pan_vy_ : placed_y_);
    release_twist(out);
    end_camera(out);
    preview_ = false;
    fingers_.clear();
    end_edge_hold(out);
    reset_gesture();
}

void TouchMapper::tick(uint64_t now, std::vector<TouchAction> *out) {
    if (press_pending_ &&
        (presents_known_ ? presents_ != presents_at_place_ : now >= press_deadline_))
        press_held(out, now);
    if (release_pending_ && release_ready(now))
        release_held(out);
    if (twist_key_ && now - twist_moved_ns_ >= kTouchTwistIdleNs)
        release_twist(out); // the fingers stopped turning
    split_tick(now, out);
    if (drag_pending_ && fingers_.size() == 1 && max_fingers_ == 1)
        start_hover_drag(now, out); // the frame it waited for has come
    if (fingers_.size() != 1 || max_fingers_ != 1 || dragging_ || long_fired_ || middle_held_ ||
        aiming_)
        return;
    const Finger &f = fingers_[0];
    if (native_ && now - f.t0 >= kTouchLongPressNs && dist(f.x0, f.y0, f.x, f.y) <= kTouchTapTravel) {
        // Native: the finger rests; the game shows what is under it.
        long_fired_ = native_press_ = true;
        native_event(out, RECOMP_TOUCH_EVENT_PRESS, RECOMP_TOUCH_PHASE_BEGIN, f.x, f.y);
        return;
    }
    if (hover_ && now - f.t0 >= kTouchLongPressNs && dist(f.x0, f.y0, f.x, f.y) <= kTouchTapTravel) {
        // Hover: the cursor is already under the finger; from here on a lift
        // does not click, and the game shows its tooltip at once.
        long_fired_ = true;
        hover_state_ = 0;
        return;
    }
    if (now - f.t0 >= kTouchLongPressNs && dist(f.x0, f.y0, f.x, f.y) <= kTouchTapTravel) {
        // The finger has rested: put the cursor under it. What follows decides
        // the button - a lift is a right click, a drag a wheel-button drag, and
        // on a window edge the rest itself scrolls the map.
        long_fired_ = true;
        place(out, f.x0, f.y0);
    }
}
