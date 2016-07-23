// -*- mode: c++; c-basic-offset: 4 indent-tabs-mode: nil -*- */
//
// Copyright 2016 Juho Snellman, released under a MIT license (see
// LICENSE).

#include <algorithm>
#include <functional>

#include "../timer-wheel.h"

static bool allow_schedule_in_range = true;
// Set to true to print a trace, to confirm that different timer
// implementations give the same results. (Or close enough results,
// if using non-deterministic features like schedule_in_range).
static bool print_trace = false;

class Unit {
public:
    Unit(TimerWheel* timers, int request_interval=1000*50)
        : timers_(timers),
          idle_timer_(this),
          close_timer_(this),
          pace_timer_(this),
          request_timer_(this),
          id_(id_counter_++),
          request_interval_(request_interval) {
    }

    ~Unit() {
        if (print_trace) {
            printf("delete %d, rx-count=%d\n", id_, rx_count_);
        }
    }

    void start(bool server) {
        unidle();
        timers_->schedule(&close_timer_, 180*1000*50);
        if (!server) {
            on_request();
        }
    }

    void transmit(int count) {
        tx_count_ += count;
        deliver();
    }
    void deliver() {
        unidle();
        int amount = std::min(pace_quota_, tx_count_);
        pace_quota_ -= amount;
        tx_count_ -= amount;
        other_->receive(amount);
        if (!pace_quota_) {
            timers_->schedule(&pace_timer_, pace_interval_ticks_);
        }
    }
    void receive(int count) {
        unidle();
        rx_count_++;
        if (rx_count_ % 100 == 0) {
            timers_->schedule(&request_timer_, request_interval_);
        }
    }
    void pair_with(Unit* other) {
        other_ = other;
    }

    void on_close() {
        if (closing_) {
            delete this;
        } else {
            // First time the timeout triggers, we just put the
            // object into a closing state where it'll start winding down
            // work. Then we forcibly close it a bit later. We do it like
            // this to remove any non-determinism between the execution order
            // of the close timer and the pace timer.
            closing_ = true;
            timers_->schedule(&close_timer_, 10*1000*50);
        }
    }
    void on_pace() {
        if (tx_count_) {
            pace_quota_ = 1;
            deliver();
        }
    }
    void on_idle() {
        delete this;
    }
    void on_request() {
        if (!closing_) {
            other_->transmit(100);
        }
    }

    void unidle() {
        if (allow_schedule_in_range) {
            timers_->schedule_in_range(&idle_timer_,
                                       60*1000*50,
                                       61*1000*50);
        } else {
            timers_->schedule(&idle_timer_, 60*1000*50);
        }
    }

private:
    TimerWheel* timers_;
    MemberTimerEvent<Unit, &Unit::on_idle> idle_timer_;
    MemberTimerEvent<Unit, &Unit::on_close> close_timer_;
    MemberTimerEvent<Unit, &Unit::on_pace> pace_timer_;
    MemberTimerEvent<Unit, &Unit::on_request> request_timer_;

    static int id_counter_;
    int id_;
    int tx_count_ = 0;
    int rx_count_ = 0;
    Unit* other_ = NULL;
    int pace_quota_ = 1;
    int pace_interval_ticks_ = 10;
    int request_interval_ = 100;
    bool closing_ = false;
};

int Unit::id_counter_ = 0;

static void make_unit_pair(TimerWheel* timers, int request_interval) {
    Unit* server = new Unit(timers);
    Unit* client = new Unit(timers, request_interval);
    server->pair_with(client);
    client->pair_with(server);

    server->start(true);
    client->start(false);
}

bool bench() {
    TimerWheel timers;

    while (timers.now() < 1000*50) {
        while (rand() % 2 == 0) {
            make_unit_pair(&timers, 1000*50 + rand() % 100);
        }
        timers.advance(10);
    }

    while (timers.now() < 300*1000*50) {
        Tick t = timers.ticks_to_next_event(100);
        timers.advance(t);
    }

    return true;
}

int main(void) {
    bench();
    return 0;
}
