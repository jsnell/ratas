//

#include <functional>

#include "../timer-wheel.h"

void test_single_timer_no_hierarchy() {
    typedef std::function<void()> Callback;
    HierarchicalTimerWheel<Callback> timers;
    int count = 0;
    TimerEvent<Callback> timer([&count] () { ++count; },
        &timers);

    timers.advance(10);
    assert(count == 0);
    assert(!timer.active());

    timers.schedule(&timer, 5);
    assert(timer.active());
    timers.advance(10);
    assert(count == 1);

    timers.advance(10);
    assert(count == 1);

    timers.schedule(&timer, 5);
    timers.advance(10);
    assert(count == 2);

    timers.schedule(&timer, 5);
    timer.cancel();
    assert(!timer.active());
    timers.advance(10);
    assert(count == 2);

    // Test wraparound
    timers.advance(250);
    timers.schedule(&timer, 5);
    timers.advance(10);
    assert(count == 3);
}

void test_single_timer_hierarchy() {
    typedef std::function<void()> Callback;
    HierarchicalTimerWheel<Callback> timers;
    int count = 0;
    TimerEvent<Callback> timer([&count] () { ++count; },
        &timers);

    timers.advance(10);
    assert(count == 0);

    // Schedule timer one layer up.
    timers.schedule(&timer, 261);
    timers.advance(260);
    assert(count == 0);
    timers.advance(1);
    assert(count == 1);

    timers.schedule(&timer, 256*4);
    timers.advance(256*4 - 1);
    assert(count == 1);
    timers.advance(1);
    assert(count == 2);
}

int main(void) {
    test_single_timer_no_hierarchy();
    test_single_timer_hierarchy();
    // Test canceling timer from within timer
    // Test rescheduling timer from within timer
}
