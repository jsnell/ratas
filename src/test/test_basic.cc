// -*- mode: c++; c-basic-offset: 4 indent-tabs-mode: nil -*- */
//
// Copyright 2016 Juho Snellman, released under a MIT license (see
// LICENSE).

#include <algorithm>
#include <functional>
#include <vector>

#include "../timer-wheel.h"

#define TEST(fun) \
    do {                                              \
        if (fun()) {                                  \
            printf("[OK] %s\n", #fun);                \
        } else {                                      \
            ok = false;                               \
            printf("[FAILED] %s\n", #fun);            \
        }                                             \
    } while (0)

#define EXPECT(expr)                                    \
    do {                                                \
        if (!(expr))  {                                 \
            printf("%s:%d: Expect failed: %s\n",        \
                   __FILE__, __LINE__, #expr);          \
            return false;                               \
        }                                               \
    } while (0)

#define EXPECT_INTEQ(actual, expect)                    \
    do {                                                \
        if (expect != actual)  {                        \
            printf("%s:%d: Expect failed, wanted %ld"   \
                   " got %ld\n",                        \
                   __FILE__, __LINE__,                  \
                   (long) expect, (long) actual);       \
            return false;                               \
        }                                               \
    } while (0)

bool test_single_timer_no_hierarchy() {
    typedef std::function<void()> Callback;
    TimerWheel timers;
    int count = 0;
    TimerEvent<Callback> timer([&count] () { ++count; });

    // Unscheduled timer does nothing.
    timers.advance(10);
    EXPECT_INTEQ(count, 0);
    EXPECT(!timer.active());

    // Schedule timer, should trigger at right time.
    timers.schedule(&timer, 5);
    EXPECT(timer.active());
    timers.advance(5);
    EXPECT_INTEQ(count, 1);

    // Only trigger once, not repeatedly (even if wheel wraps
    // around).
    timers.advance(256);
    EXPECT_INTEQ(count, 1);

    // ... unless, of course, the timer gets scheduled again.
    timers.schedule(&timer, 5);
    timers.advance(5);
    EXPECT_INTEQ(count, 2);

    // Canceled timers don't run.
    timers.schedule(&timer, 5);
    timer.cancel();
    EXPECT(!timer.active());
    timers.advance(10);
    EXPECT_INTEQ(count, 2);

    // Test wraparound
    timers.advance(250);
    timers.schedule(&timer, 5);
    timers.advance(10);
    EXPECT_INTEQ(count, 3);

    // Timers that are scheduled multiple times only run at the last
    // scheduled tick.
    timers.schedule(&timer, 5);
    timers.schedule(&timer, 10);
    timers.advance(5);
    EXPECT_INTEQ(count, 3);
    timers.advance(5);
    EXPECT_INTEQ(count, 4);

    // Timer can safely be canceled multiple times.
    timers.schedule(&timer, 5);
    timer.cancel();
    timer.cancel();
    EXPECT(!timer.active());
    timers.advance(10);
    EXPECT_INTEQ(count, 4);

    {
        TimerEvent<Callback> timer2([&count] () { ++count; });
        timers.schedule(&timer2, 5);
    }
    timers.advance(10);
    EXPECT_INTEQ(count, 4);

    return true;
}

bool test_single_timer_hierarchy() {
    typedef std::function<void()> Callback;
    TimerWheel timers;
    int count = 0;
    TimerEvent<Callback> timer([&count] () { ++count; });

    EXPECT_INTEQ(count, 0);

    // Schedule timer one layer up (make sure timer ends up in slot 0 once
    // promoted to the innermost wheel, since that's a special case).
    timers.schedule(&timer, 256);
    timers.advance(255);
    EXPECT_INTEQ(count, 0);
    timers.advance(1);
    EXPECT_INTEQ(count, 1);

    // Then schedule one that ends up in some other slot
    timers.schedule(&timer, 257);
    timers.advance(256);
    EXPECT_INTEQ(count, 1);
    timers.advance(1);
    EXPECT_INTEQ(count, 2);

    // Schedule multiple rotations ahead in time, to slot 0.
    timers.schedule(&timer, 256*4 - 1);
    timers.advance(256*4 - 2);
    EXPECT_INTEQ(count, 2);
    timers.advance(1);
    EXPECT_INTEQ(count, 3);

    // Schedule multiple rotations ahead in time, to non-0 slot. (Do this
    // twice, once starting from slot 0, once starting from slot 5);
    for (int i = 0; i < 2; ++i) {
        timers.schedule(&timer, 256*4 + 5);
        timers.advance(256*4 + 4);
        EXPECT_INTEQ(count, 3 + i);
        timers.advance(1);
        EXPECT_INTEQ(count, 4 + i);
    }

    return true;
}

bool test_ticks_to_next_event() {
    typedef std::function<void()> Callback;
    TimerWheel timers;
    TimerEvent<Callback> timer([] () { });
    TimerEvent<Callback> timer2([] () { });

    // No timers scheduled, return the max value.
    EXPECT_INTEQ(timers.ticks_to_next_event(100), 100);
    EXPECT_INTEQ(timers.ticks_to_next_event(),
                 std::numeric_limits<Tick>::max());

    for (int i = 0; i < 10; ++i) {
        // Just vanilla tests
        timers.schedule(&timer, 1);
        EXPECT_INTEQ(timers.ticks_to_next_event(100), 1);

        timers.schedule(&timer, 20);
        EXPECT_INTEQ(timers.ticks_to_next_event(100), 20);

        // Check the the "max" parameters works.
        timers.schedule(&timer, 150);
        EXPECT_INTEQ(timers.ticks_to_next_event(100), 100);

        // Check that a timer on the next layer can be found.
        timers.schedule(&timer, 280);
        EXPECT_INTEQ(timers.ticks_to_next_event(100), 100);
        EXPECT_INTEQ(timers.ticks_to_next_event(1000), 280);

        // Test having a timer on the next wheel (still remaining from
        // the previous test), and another (earlier) timer on this
        // wheel.
        for (int i = 1; i < 256; ++i) {
            timers.schedule(&timer2, i);
            EXPECT_INTEQ(timers.ticks_to_next_event(1000), i);
        }

        timer.cancel();
        timer2.cancel();
        // And then run these same tests from a bunch of different
        // wheel locations.
        timers.advance(32);
    }

    // More thorough tests for cases where the next timer could be on
    // either of two different wheels.
    for (int i = 0; i < 20; ++i) {
        timers.schedule(&timer, 270);
        timers.advance(128);
        EXPECT_INTEQ(timers.ticks_to_next_event(512), 270 - 128);
        timers.schedule(&timer2, 250);
        EXPECT_INTEQ(timers.ticks_to_next_event(512), 270 - 128);
        timers.schedule(&timer2, 10);
        EXPECT_INTEQ(timers.ticks_to_next_event(512), 10);

        // Again, do this from a bunch of different locatoins.
        timers.advance(32);
    }

    timer.cancel();
    EXPECT_INTEQ(timers.ticks_to_next_event(),
                 std::numeric_limits<Tick>::max());

    return true;
}

bool test_schedule_in_range() {
    typedef std::function<void()> Callback;
    TimerWheel timers;
    TimerEvent<Callback> timer([] () { });

    // No useful rounding possible.
    timers.schedule_in_range(&timer, 281, 290);
    EXPECT_INTEQ(timers.ticks_to_next_event(), 290);

    // Pick a time aligned at slot boundary if possible.
    timers.schedule_in_range(&timer, 256*4 - 1, 256*5 - 1);
    EXPECT_INTEQ(timers.ticks_to_next_event(), 256*4);

    timers.schedule_in_range(&timer, 256*4 + 1, 256*5);
    EXPECT_INTEQ(timers.ticks_to_next_event(), 256*5);

    // Event already in right range.
    timers.schedule_in_range(&timer, 256*1, 256*10);
    EXPECT_INTEQ(timers.ticks_to_next_event(), 256*5);

    // Event canceled, but was previously scheduled in
    // the right range. Should be ignored, and scheduled
    // as normal to the end of the range.
    timer.cancel();
    timers.schedule_in_range(&timer, 256*1, 256*10);
    EXPECT_INTEQ(timers.ticks_to_next_event(), 256*10);

    // Make sure the decision on whether timer is in range or
    // not is done based on absolute ticks, not relative ticks.
    timers.advance(256*9);
    EXPECT_INTEQ(timers.ticks_to_next_event(), 256*1);
    timers.schedule_in_range(&timer, 256*9, 256*10);
    EXPECT_INTEQ(timers.ticks_to_next_event(), 256*10);

    // Try scheduling timers in random ranges.
    for (int i = 0; i < 10000; ++i) {
        int len1 = rand() % 20;
        int len2 = rand() % 20;
        int r1 = rand() % (1 << len1);
        int r2 = r1 + (1 + rand() % (1 << len2));
        timers.schedule_in_range(&timer, r1, r2);
        EXPECT(timers.ticks_to_next_event() >= r1);
        EXPECT(timers.ticks_to_next_event() <= r2);
    }

    return true;
}

bool test_reschedule_from_timer() {
    typedef std::function<void()> Callback;
    TimerWheel timers;
    int count = 0;
    TimerEvent<Callback> timer([&count] () { ++count; });

    // For every slot in the outermost wheel, try scheduling a timer from
    // a timer handler 258 ticks in the future. Then reschedule it in 257
    // ticks. It should never actually trigger.
    for (int i = 0; i < 256; ++i) {
        TimerEvent<Callback> rescheduler([&timers, &timer] () { timers.schedule(&timer, 258); });

        timers.schedule(&rescheduler, 1);
        timers.advance(257);
        EXPECT_INTEQ(count, 0);
    }
    // But once we stop rescheduling the timer, it'll trigger as intended.
    timers.advance(2);
    EXPECT_INTEQ(count, 1);

    return true;
}

bool test_single_timer_random() {
    typedef std::function<void()> Callback;
    TimerWheel timers;
    int count = 0;
    TimerEvent<Callback> timer([&count] () { ++count; });

    for (int i = 0; i < 10000; ++i) {
        int len = rand() % 20;
        int r = 1 + rand() % ( 1 << len);

        timers.schedule(&timer, r);
        if (r > 1)
            timers.advance(r - 1);
        EXPECT_INTEQ(count, i);
        timers.advance(1);
        EXPECT_INTEQ(count, i + 1);
    }

    return true;
}

bool test_maxexec() {
    typedef std::function<void()> Callback;
    TimerWheel timers;
    int count0 = 0;
    int count1 = 0;
    TimerEvent<Callback> timer0([&count0] () { ++count0; });
    TimerEvent<Callback> timer1a([&count1] () { ++count1; });
    TimerEvent<Callback> timer1b([&count1] () { ++count1; });

    // Schedule 3 timers to happen at the same time (on 2 different
    // wheels).
    timers.schedule(&timer1a, 256);
    timers.schedule(&timer1b, 256);
    timers.advance(1);
    timers.schedule(&timer0, 255);
    timers.advance(254);
    EXPECT_INTEQ(count0, 0);
    EXPECT_INTEQ(count1, 0);
    EXPECT_INTEQ(timers.ticks_to_next_event(), 1);
    EXPECT_INTEQ(timers.now(), 255);

    // Then run them one by one.
    EXPECT(!timers.advance(1, 1));
    EXPECT_INTEQ(count0, 0);
    EXPECT_INTEQ(count1, 1);
    EXPECT_INTEQ(timers.ticks_to_next_event(), 0);
    // Note that time has already advanced.
    EXPECT_INTEQ(timers.now(), 256);
    EXPECT(!timers.advance(0, 1));
    EXPECT_INTEQ(count0, 0);
    EXPECT_INTEQ(count1, 2);
    EXPECT(!timers.advance(0, 1));
    EXPECT_INTEQ(count0, 1);
    EXPECT_INTEQ(count1, 2);

    // We have not finished the tick yet, since the last call exactly
    // drained the queue. But the next call will finish the tick while
    // doing no actual work.
    EXPECT_INTEQ(timers.ticks_to_next_event(100), 0);
    EXPECT(timers.advance(0, 1));
    EXPECT_INTEQ(timers.ticks_to_next_event(100), 100);

    // Test scheduling while wheel is in the middle of partial tick handling.
    timers.schedule(&timer1a, 256);
    timers.advance(1);
    timers.schedule(&timer0, 255);
    timers.advance(254);
    EXPECT(!timers.advance(1, 1));
    // Now in the middle of the tick.
    std::vector<bool> done(false, 512);
    std::vector<TimerEvent<Callback>*> events;
    // Schedule 512 timers, each setting the matching bit in "done".
    for (int i = 0; i < done.size(); ++i) {
        auto event = new TimerEvent<Callback>([&done, i] () { done[i] = true; });
        events.push_back(event);
        timers.schedule(event, i + 1);
    }

    // Close the tick.
    EXPECT(timers.advance(0, 100));

    // Now check that all 512 timers were scheduled in the right location.
    for (int i = 0; i < done.size(); ++i) {
        EXPECT_INTEQ(std::count(done.begin(), done.end(), true), i);
        EXPECT(!done[i]);
        timers.advance(1);
        EXPECT(done[i]);
    }

    return true;
}


class Test {
public:
    Test()
        : inc_timer_(this), reset_timer_(this) {
    }

    void start(TimerWheel* timers) {
        timers->schedule(&inc_timer_, 10);
        timers->schedule(&reset_timer_, 15);
    }

    void on_inc() {
        count_++;
    }

    void on_reset() {
        count_ = 0;
    }

    int count() { return count_; }

private:
    MemberTimerEvent<Test, &Test::on_inc> inc_timer_;
    MemberTimerEvent<Test, &Test::on_reset> reset_timer_;
    int count_ = 0;
};

bool test_timeout_method() {
    TimerWheel timers;

    Test test;
    test.start(&timers);

    EXPECT_INTEQ(test.count(), 0);
    timers.advance(10);
    EXPECT_INTEQ(test.count(), 1);
    timers.advance(5);
    EXPECT_INTEQ(test.count(), 0);
    return true;
}

int main(void) {
    bool ok = true;
    TEST(test_single_timer_no_hierarchy);
    TEST(test_single_timer_hierarchy);
    TEST(test_ticks_to_next_event);
    TEST(test_schedule_in_range);
    TEST(test_single_timer_random);
    TEST(test_maxexec);
    TEST(test_reschedule_from_timer);
    TEST(test_timeout_method);
    // Test canceling timer from within timer
    return ok ? 0 : 1;
}
