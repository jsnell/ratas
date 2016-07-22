// -*- mode: c++; c-basic-offset: 4 indent-tabs-mode: nil -*- */
//
// Copyright 2016 Juho Snellman, released under a MIT license (see
// LICENSE).
//
// A timer queue which allows events to be scheduled for execution
// at some later point. Reasons you might want to use this implementation
// instead of some other are:
//
// - Optimized for high occupancy rates, on the assumption that the
//   utilization of the timer queue is proportional to the utilization
//   of the system as a whole. When a tradeoff needs to be made
//   between efficiency of one operation at a low occupancy rate and
//   another operation at a high rate, we choose the latter.
// - Tries to minimize the cost of event rescheduling or cancelation,
//   on the assumption that a large percentage of events will never
//   be triggered. The implementation tries avoids unnecessary work when an
//   event is rescheduled, and provides a way for the user specify a
//   range of acceptable execution times instead of just an exact one.
// - An interface that at least the author finds more convenient than
//   the typical options.
//
// The exact implementation strategy is a hierarchical timer
// wheel. A timer wheel is effectively a ring buffer of linked lists
// of events, and a pointer to the ring buffer. As the time advances,
// the pointer moves forward, and any events in the ring buffer slots
// that the pointer passed will get executed.
//
// A hierarchical timer wheel layers multiple timer wheels running at
// different resolutions on top of each other. When an event is
// scheduled so far in the future than it does not fit the innermost
// (core) wheel, it instead gets scheduled on one of the outer
// wheels. On each rotation of the inner wheel, one slot's worth of
// events are promoted from the second wheel to the core. On each
// rotation of the second wheel, one slot's worth of events is
// promoted from the third wheel to the second, and so on.
//
// The basic usage is to create a single TimerWheel object and
// multiple TimerEvent or MemberTimerEvent objects. The events are
// scheduled for execution using TimerWheel::schedule() or
// TimerWheel::schedule_in_range(), or unscheduled using the event's
// cancel() method.
//
// Example usage:
//
//      typedef std::function<void()> Callback;
//      TimerWheel timers;
//      int count = 0;
//      TimerEvent<Callback> timer([&count] () { ++count; });
//
//      timers.schedule(&timer, 5);
//      timers.advance(4);
//      assert(count == 0);
//      timers.advance(1);
//      assert(count == 1);
//
//      timers.schedule(&timer, 5);
//      timer.cancel();
//      timers.advance(4);
//      assert(count == 1);
//
// To tie events to specific member functions of an object instead of
// a callback function, use MemberTimerEvent instead of TimerEvent.
// For example:
//
//      class Test {
//        public:
//            Test() : inc_timer_(this) {
//            }
//            void start(TimerWheel* timers) {
//                timers->schedule(&inc_timer_, 10);
//            }
//            void on_inc() {
//                count_++;
//            }
//            int count() { return count_; }
//        private:
//            MemberTimerEvent<Test, &Test::on_inc> inc_timer_;
//            int count_ = 0;
//      };

#ifndef _TIMER_WHEEL_H
#define _TIMER_WHEEL_H

#include <cassert>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>

typedef uint64_t Tick;

class TimerWheelSlot;
class TimerWheel;

// An abstract class representing an event that can be scheduled to
// happen at some later time.
class TimerEventInterface {
public:
    TimerEventInterface() {
    }

    // TimerEvents are automatically canceled on destruction.
    virtual ~TimerEventInterface() {
        cancel();
    }

    // Unschedule this event. It's safe to cancel an event that is inactive.
    void cancel();

    // Return true iff the event is currently scheduled for execution.
    bool active() const {
        return slot_ != NULL;
    }

    // Return the absolute tick this event is scheduled to be executed on.
    Tick scheduled_at() const { return scheduled_at_; }

private:
    TimerEventInterface(const TimerEventInterface& other) = delete;
    TimerEventInterface& operator=(const TimerEventInterface& other) = delete;
    friend TimerWheelSlot;
    friend TimerWheel;

    // Implement in subclasses. Executes the event callback.
    virtual void execute() = 0;

    void set_scheduled_at(Tick ts) { scheduled_at_ = ts; }
    // Move the event to another slot. (It's safe for either the current
    // or new slot to be NULL).
    void relink(TimerWheelSlot* slot);

    Tick scheduled_at_;
    // The slot this event is currently in (NULL if not currently scheduled).
    TimerWheelSlot* slot_ = NULL;
    // The events are linked together in the slot using an internal
    // doubly-linked list; this iterator does double duty as the
    // linked list node for this event.
    TimerEventInterface* next_ = NULL;
    TimerEventInterface* prev_ = NULL;
};

// An event that takes the callback (of type CBType) to execute as
// a constructor parameter.
template<typename CBType>
class TimerEvent : public TimerEventInterface {
public:
    explicit TimerEvent<CBType>(const CBType& callback)
      : callback_(callback) {
    }

    void execute() {
        callback_();
    }

private:
    TimerEvent<CBType>(const TimerEvent<CBType>& other) = delete;
    TimerEvent<CBType>& operator=(const TimerEvent<CBType>& other) = delete;
    CBType callback_;
};

// An event that's specialized with a (static) member function of class T,
// and a dynamic instance of T. Event execution causes an invocation of the
// member function on the instance.
template<typename T, void(T::*MFun)() >
class MemberTimerEvent : public TimerEventInterface {
public:
    MemberTimerEvent(T* obj) : obj_(obj) {
    }

    virtual void execute () {
        (obj_->*MFun)();
    }

private:
    T* obj_;
};

// Purely an implementation detail.
class TimerWheelSlot {
public:
    TimerWheelSlot() {
    }

private:
    // Return the first event queued in this slot.
    const TimerEventInterface* events() const { return events_; }
    // Deque the first event from the slot, and return it.
    TimerEventInterface* pop_event() {
        auto event = events_;
        events_ = event->next_;
        if (events_) {
            events_->prev_ = NULL;
        }
        event->next_ = NULL;
        event->slot_ = NULL;
        return event;
    }

    TimerWheelSlot(const TimerWheelSlot& other) = delete;
    TimerWheelSlot& operator=(const TimerWheelSlot& other) = delete;
    friend TimerEventInterface;
    friend TimerWheel;

    // Doubly linked (inferior) list of events.
    TimerEventInterface* events_ = NULL;
};

// A TimerWheel is the entity that TimerEvents can be scheduled on
// for execution (with schedule() or schedule_in_range()), and will
// eventually be executed once the time advances far enough with the
// advance() method.
//
// When the core timer wheel is created by the user, the appropriate
// outer wheels will be created automatically. The outer wheels are
// not accessible to the user.
class TimerWheel {
public:
    TimerWheel(Tick now = 0)
        : now_(now),
          out_(new TimerWheel(WIDTH_BITS, this)),
          core_(NULL) {
    }

    // Advance the TimerWheel by the specified number of ticks, and execute
    // any events scheduled for execution at or before that time.
    // - It is safe to cancel or schedule events from within event callbacks.
    // - During the execution of the callback the observable event tick will
    //   be the tick it was scheduled to run on; not the tick the clock will
    //   be advanced to.
    // - Events will happen in order; all events scheduled for tick X will
    //   be executed before any event scheduled for tick X+1.
    void advance(Tick delta);

    // Schedule the event to be executed delta ticks from the current time.
    // The delta must be non-0.
    void schedule(TimerEventInterface* event, Tick delta);

    // Schedule the event to happen at some time between start and end
    // ticks from the current time. The actual time will be determined
    // by the TimerWheel to minimize rescheduling and promotion overhead.
    // Both start and end must be non-0, and the end must be greater than
    // the start.
    void schedule_in_range(TimerEventInterface* event,
                           Tick start, Tick end);

    // Return the current tick value. Note that if the time increases
    // by multiple ticks during a single call to advance(), during the
    // execution of the event callback now() will return the tick that
    // the event was scheduled to run on.
    Tick now() const { return now_; }

    // Return the number of ticks remaining until the next event will get
    // executed. If the max parameter is passed, that will be the maximum
    // tick value that gets returned. The max parameter's value will also
    // be returned if no events have been scheduled.
    Tick ticks_to_next_event(const Tick& max = std::numeric_limits<Tick>::max());

private:
    TimerWheel(const TimerWheel& other) = delete;
    TimerWheel& operator=(const TimerWheel& other) = delete;

    TimerWheel(int offset, TimerWheel* down)
        : now_(0),
          core_(down) {
        if (offset + WIDTH_BITS < 64) {
            out_.reset(new TimerWheel(offset + WIDTH_BITS, down));
        }
     }

    // The current timestamp for this wheel. This will be right-shifted
    // such that each slot is separated by exactly one tick even on
    // the outermost wheels.
    Tick now_;

    static const int WIDTH_BITS = 8;
    static const int NUM_SLOTS = 1 << WIDTH_BITS;
    // A bitmask for looking at just the bits in the timestamp relevant to
    // this wheel.
    static const int MASK = (NUM_SLOTS - 1);
    TimerWheelSlot slots_[NUM_SLOTS];
    // The next timer wheel layer (coarser granularity).
    std::unique_ptr<TimerWheel> out_;
    // The core timer wheel (most granular).
    TimerWheel* core_;
};

// Implementation

void TimerEventInterface::relink(TimerWheelSlot* new_slot) {
    if (new_slot == slot_) {
        return;
    }

    // Unlink from old location.
    if (slot_) {
        auto prev = prev_;
        auto next = next_;
        if (next) {
            next->prev_ = prev;
        }
        if (prev) {
            prev->next_ = next;
        } else {
            // Must be at head of slot. Move the next item to the head.
            slot_->events_ = next;
        }
    }

    // Insert in new slot.
    {
        if (new_slot) {
            auto old = new_slot->events_;
            next_ = old;
            if (old) {
                old->prev_ = this;
            }
            new_slot->events_ = this;
        } else {
            next_ = NULL;
        }
        prev_ = NULL;
    }
    slot_ = new_slot;
}

void TimerEventInterface::cancel() {
    // It's ok to cancel a event that's not scheduled.
    if (!slot_) {
        return;
    }

    relink(NULL);
}

void TimerWheel::advance(Tick delta) {
    assert(delta > 0);
    while (delta--) {
        now_++;
        size_t slot_index = now_ & MASK;
        auto slot = &slots_[slot_index];
        if (slot_index == 0 && out_) {
            out_->advance(1);
        }
        while (slot->events()) {
            auto event = slot->pop_event();
            if (core_) {
                assert((core_->now_ & MASK) == 0);
                Tick now = core_->now();
                if (now >= event->scheduled_at()) {
                    event->execute();
                } else {
                    core_->schedule(event,
                                    event->scheduled_at() - now);
                }
            } else {
                event->execute();
            }
        }
    }
}

void TimerWheel::schedule(TimerEventInterface* event, Tick delta) {
    assert(delta > 0);

    if (!core_) {
        event->set_scheduled_at(now_ + delta);
    }

    if (delta >= NUM_SLOTS) {
        return out_->schedule(event, (delta + (now_ & MASK)) >> WIDTH_BITS);
    }

    size_t slot_index = (now_ + delta) & MASK;
    auto slot = &slots_[slot_index];
    event->relink(slot);
}

void TimerWheel::schedule_in_range(TimerEventInterface* event,
                                   Tick start, Tick end) {
    assert(end > start);
    if (event->active()) {
        auto current = event->scheduled_at() - now_;
        // Event is already scheduled to happen in this range. Instead
        // of always using the old slot, we could check compute the
        // new slot and switch iff it's aligned better than the old one.
        // But it seems hard to believe that could be worthwhile.
        if (current >= start && current <= end) {
            return;
        }
    }

    // Zero as many bits (in WIDTH_BITS chunks) as possible
    // from "end" while still keeping the output in the
    // right range.
    Tick mask = ~0;
    while ((start & mask) != (end & mask)) {
        mask = (mask << WIDTH_BITS);
    }

    Tick delta = end & (mask >> WIDTH_BITS);

    schedule(event, delta);
}

Tick TimerWheel::ticks_to_next_event(const Tick& max) {
    // The actual current time (not the bitshifted time)
    Tick now = core_ ? core_->now() : now_;

    // Smallest tick (relative to now) we've found.
    Tick min = max;
    for (int i = 0; i < NUM_SLOTS; ++i) {
        // Note: Unlike the uses of "now", slot index calculations really
        // need to use now_.
        auto slot_index = (now_ + 1 + i) & MASK;
        // We've reached slot 0. In normal scheduling this would
        // mean advancing the next wheel and promoting or executing
        // those events.  So we need to look in that slot too
        // before proceeding with the rest of this wheel. But we
        // can't just accept those results outright, we need to
        // check the best result there against the next slot on
        // this wheel.
        if (slot_index == 0 && out_) {
            // Exception: If we're in the core wheel, and slot 0 is
            // not empty, there's no point in looking in the outer wheel.
            // It's guaranteed that the events actually in slot 0 will be
            // executed no later than anything in the outer wheel.
            if (core_ || !slots_[0].events()) {
                const auto& slot = out_->slots_[(out_->now_ + 1) & MASK];
                for (auto event = slot.events(); event != NULL;
                     event = event->next_) {
                    min = std::min(min, event->scheduled_at() - now);
                }
            }
        }
        bool found = false;
        const auto& slot = slots_[slot_index];
        for (auto event = slot.events(); event != NULL;
             event = event->next_) {
            min = std::min(min, event->scheduled_at() - now);
            // In the core wheel all the events in a slot are guaranteed to
            // run at the same time, so it's enough to just look at the first
            // one.
            if (!core_) {
                return min;
            } else {
                found = true;
            }
        }
        if (found) {
            return min;
        }
    }

    // Nothing found on this wheel, try the next one.
    if (out_) {
        return out_->ticks_to_next_event(max);
    }
    return max;
}

#endif //  _TIMER_WHEEL_H
