// -*- mode: c++; c-basic-offset: 4 indent-tabs-mode: nil -*-
//
// Copyright 2016 Juho Snellman, released under a MIT license (see
// LICENSE).

#include <algorithm>
#include <functional>
#include <sys/time.h>
#include <sys/resource.h>

#include "../timer-wheel.h"

static bool allow_schedule_in_range = true;
// Set to true to print a trace, to confirm that different timer
// implementations give the same results. (Or close enough results,
// if using non-deterministic features like schedule_in_range).
static bool print_trace = true;
static int pair_count = 5;
// The total number of response messages received on all units.
// Printed in the final output, useful as a poor man's output checksum.
static long total_rx_count = 0;

// Pretend we're using timer ticks of 20 microseconds. So 50000 ticks
// is one second.
static Tick time_ms = 50;
static Tick time_s = 1000*time_ms;

class Unit {
public:
    Unit(TimerWheel* timers, int request_interval=1*time_s)
        : timers_(timers),
          idle_timer_(this),
          close_timer_(this),
          pace_timer_(this),
          request_timer_(this),
          request_deadline_timer_(this),
          id_(id_counter_++),
          request_interval_ticks_(request_interval) {
    }

    ~Unit() {
        if (print_trace) {
            printf("delete %d, rx-count=%d\n", id_, rx_count_);
        }
        total_rx_count += rx_count_;
    }

    // Create a full work unit from the two halves.
    void pair_with(Unit* other) {
        other_ = other;
    }
    // Start the benchmark, acting either as the client or the server.
    void start(bool server) {
        unidle();
        // Start shutdown of this work unit in 180s.
        timers_->schedule(&close_timer_, 180*time_s);
        if (!server) {
            // Fire off the first server from the client.
            on_request();
        }
    }

    // Queue "count" messages to be transmitted.
    void transmit(int count) {
        tx_count_ += count;
        deliver();
    }
    // Deliver as many response messages as we have quota for. Then
    // start off a timer to refresh the quota.
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
    // Receive some number of response messages.
    void receive(int count) {
        unidle();
        // Receive the first response to a given request. Move the
        // deadline timer back in time (since this connection is now
        // clearly active).
        if (waiting_for_response_) {
            timers_->schedule(&request_deadline_timer_,
                              pace_interval_ticks_ * RESPONSE_SIZE * 2);
            waiting_for_response_ = false;
        }
        rx_count_++;
        // We've received the full response. Stop the deadline timer,
        // and start another timer that'll trigger the next request.
        if (rx_count_ % RESPONSE_SIZE == 0) {
            request_deadline_timer_.cancel();
            timers_->schedule(&request_timer_, request_interval_ticks_);
        }
    }

    // First time this timer gets executed, we put the object into a
    // closing state where it'll start winding down work. Then we
    // forcibly close it a bit later. We do it like this to remove any
    // non-determinism between the execution order of the close timer
    // and the pace timer.
    void on_close() {
        if (closing_) {
            delete this;
        } else {
            closing_ = true;
            timers_->schedule(&close_timer_, 10*time_s);
        }
    }
    // Refresh transmit quota.
    void on_pace() {
        if (tx_count_) {
            pace_quota_ = 1;
            deliver();
        }
    }
    // The endpoint has been idle for too long, kill it.
    void on_idle() {
        delete this;
    }
    // Send a new request (unless were draining traffic).
    void on_request() {
        if (!closing_) {
            // Expect a response within this time.
            timers_->schedule(&request_deadline_timer_,
                              pace_interval_ticks_ * RESPONSE_SIZE * 4);
            waiting_for_response_ = true;
            other_->transmit(RESPONSE_SIZE);
        }
    }
    // We've done some work. Move the idle timer further into the future.
    void unidle() {
        if (allow_schedule_in_range) {
            timers_->schedule_in_range(&idle_timer_, 60*time_s,
                                       61*time_s);
        } else {
            timers_->schedule(&idle_timer_, 60*time_s);
        }
    }
    // Something has gone wrong. Forcibly close down both sides.
    void on_request_deadline() {
        fprintf(stderr, "Request did not finish by deadline\n");
        delete this;
        delete other_;
    }

private:
    TimerWheel* timers_;
    // This timer gets rescheduled far into the future at very frequent
    // intervals.
    MemberTimerEvent<Unit, &Unit::on_idle> idle_timer_;
    // This timers gets scheduled twice, and executed twice.
    MemberTimerEvent<Unit, &Unit::on_close> close_timer_;
    // This gets scheduled very soon at frequent intervals, and is always
    // executed.
    MemberTimerEvent<Unit, &Unit::on_pace> pace_timer_;
    // This gets scheduled about 150-200 times during the benchmark a
    // medium duration from now, and is always executed
    MemberTimerEvent<Unit, &Unit::on_request> request_timer_;
    // This gets scheduled at a medium duration 150-200 times during a
    // benchmark, but always gets canceled (not rescheduled).
    MemberTimerEvent<Unit, &Unit::on_request_deadline> request_deadline_timer_;

    static int id_counter_;
    const static int RESPONSE_SIZE = 128;
    int id_;
    int tx_count_ = 0;
    int rx_count_ = 0;
    Unit* other_ = NULL;
    int pace_quota_ = 1;
    int pace_interval_ticks_ = 10;
    int request_interval_ticks_;
    bool closing_ = false;
    bool waiting_for_response_ = false;
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
    // Create the events evenly spread during this time range.
    int create_period = 1*time_s;
    double create_progress_per_iter = (double) pair_count / create_period * 2;
    double current_progress = 0;
    long int count = 0;

    while (timers.now() < create_period) {
        current_progress += (rand() * create_progress_per_iter) / RAND_MAX;
        while (current_progress > 1) {
            --current_progress;
            make_unit_pair(&timers, 1*time_s + rand() % 100);
            ++count;
        }
        timers.advance(1);
    }

    fprintf(stderr, "%ld work units (%ld timers)\n",
            count, count * 10);

    while (timers.now() < 300*time_s) {
        Tick t = timers.ticks_to_next_event(100*time_ms);
        timers.advance(t);
    }

    return true;
}

int main(int argc, char** argv) {
    if (char* s = getenv("BENCH_ALLOW_SCHEDULE_IN_RANGE")) {
        std::string value = s;
        if (value == "yes") {
            allow_schedule_in_range = true;
        } else if (value == "no") {
            allow_schedule_in_range = false;
        } else {
            fprintf(stderr, "BENCH_ALLOW_SCHEDULE_IN_RANGE should be yes, no or not set");
            return 1;
        }
    }
    if (char* s = getenv("BENCH_PRINT_TRACE")) {
        std::string value = s;
        if (value == "yes") {
            print_trace = true;
        } else if (value == "no") {
            print_trace = false;
        } else {
            fprintf(stderr, "BENCH_PRINT_TRACE should be yes, no or not set");
            return 1;
        }
    }
    if (char* s = getenv("BENCH_PAIR_COUNT")) {
        char dummy;
        if (sscanf(s, "%d%c", &pair_count, &dummy) != 1) {
            fprintf(stderr, "BENCH_PAIR_COUNT should an integer");
            return 1;
        }
    }

    struct rusage start;
    struct rusage end;
    getrusage(RUSAGE_SELF, &start);
    bench();
    getrusage(RUSAGE_SELF, &end);

    printf("%s,%d,%s,%lf,%ld\n", argv[0], pair_count,
           (allow_schedule_in_range ? "yes" : "no"),
           (end.ru_utime.tv_sec + end.ru_utime.tv_usec / 1000000.0) -
           (start.ru_utime.tv_sec + start.ru_utime.tv_usec / 1000000.0),
           total_rx_count);
    return 0;
}
