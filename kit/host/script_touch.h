// A smoke tap uses the app's gesture mapper, with the host clock and presents.
#pragma once
#include <stddef.h>
#include "input_touch.h"

class HostScriptTouch {
  public:
    // One stationary finger, in window points. The caller delivers every action
    // through the window input gate and keeps polling until the release arrives.
    void start(double x, double y, double w, double h, uint64_t now, uint32_t presents,
               std::vector<TouchAction> *out) {
        point_ = {1, x, y};
        started_ = now;
        active_ = finger_ = true;
        mapper_.set_bounds(w, h);
        mapper_.frames_presented(presents);
        mapper_.finger_down(point_, now, out);
    }

    void tick(uint64_t now, uint32_t presents, std::vector<TouchAction> *out) {
        last_tick_ = now;
        last_presents_ = presents;
        const size_t first = out->size();
        if (finger_ && now - started_ >= 80ull * 1000000ull) {
            finger_ = false;
            mapper_.finger_up(point_, now, out);
        }
        // Like after_events in the app: finger events, then presents and tick.
        mapper_.frames_presented(presents);
        mapper_.tick(now, out);
        for (size_t i = first; i < out->size(); ++i)
            if ((*out)[i].kind == TouchAction::Button && !(*out)[i].down)
                active_ = false;
    }

    bool active() const {
        return active_;
    }
    bool pending(uint64_t now, uint32_t presents) const {
        return active_ && (now != last_tick_ || presents != last_presents_);
    }

  private:
    TouchMapper mapper_;
    TouchPoint point_{};
    uint64_t started_ = 0, last_tick_ = 0;
    uint32_t last_presents_ = 0;
    bool active_ = false, finger_ = false;
};
