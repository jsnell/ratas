// -*- mode: c++; c-basic-offset: 4 indent-tabs-mode: nil -*- */
//
// Copyright 2016 Juho Snellman, released under a MIT license (see
// LICENSE).

#include <functional>

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
            printf("%s:%d: Expect failed, wanted %d"    \
                   " got %d\n",                         \
                   __FILE__, __LINE__,                  \
                   expect, actual);                     \
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

bool test_single_timer_random() {
    typedef std::function<void()> Callback;
    TimerWheel timers;
    int count = 0;
    TimerEvent<Callback> timer([&count] () { ++count; });

    for (int i = 0; i < 10000; ++i) {
        int len = rand() % 20;
        int r = 1 + rand() % ( 1 << len);

        timers.schedule(&timer, r);
        timers.advance(r - 1);
        EXPECT_INTEQ(count, i);
        timers.advance(1);
        EXPECT_INTEQ(count, i + 1);
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
    TEST(test_single_timer_random);
    TEST(test_timeout_method);
    // Test canceling timer from within timer
    // Test rescheduling timer from within timer
    return ok ? 0 : 1;
}
