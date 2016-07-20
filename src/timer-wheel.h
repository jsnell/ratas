// -*- mode: c++; c-basic-offset: 4 indent-tabs-mode: nil -*- */
//
// Copyright 2016 Juho Snellman, released under a MIT license (see
// LICENSE).

#ifndef _TIMER_WHEEL_H
#define _TIMER_WHEEL_H

#include <cassert>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <list>

typedef uint64_t Tick;

class TimerWheelSlot;
class TimerWheel;

class TimerEventInterface {
public:
    TimerEventInterface()
        : slot_(NULL),
          next_(NULL),
          prev_(NULL) {
    }

    void cancel();
    void relink(TimerWheelSlot* slot);

    virtual void execute() = 0;

    bool active() {
        return slot_ != NULL;
    }

    Tick scheduled_at() { return scheduled_at_; }
    void set_scheduled_at(Tick ts) { scheduled_at_ = ts; }
private:
    TimerEventInterface(const TimerEventInterface& other) = delete;
    TimerEventInterface& operator=(const TimerEventInterface& other) = delete;
    friend TimerWheelSlot;

    Tick scheduled_at_;
    // The slot this event is currently in (NULL if not currently scheduled).
    TimerWheelSlot* slot_ = NULL;
    // The events are linked together in the slot using an internal
    // doubly-linked list; this iterator does double duty as the
    // linked list node for this event.
    TimerEventInterface* next_;
    TimerEventInterface* prev_;
};

template<typename CBType>
class TimerEvent : public TimerEventInterface {
public:
    TimerEvent<CBType>(const CBType& callback)
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

class TimerWheelSlot {
public:
    TimerWheelSlot() {
    }

    TimerEventInterface* events() { return events_; }
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
    void push_event(TimerEventInterface* event) {
        event->slot_ = this;
        event->next_ = events_;
        if (events_) {
            events_->prev_ = event;
        }
        events_ = event;
    }

private:
    TimerWheelSlot(const TimerWheelSlot& other) = delete;
    TimerWheelSlot& operator=(const TimerWheelSlot& other) = delete;

    TimerEventInterface* events_ = NULL;
};

class TimerWheel {
public:
    TimerWheel()
        : now_(0),
          up_(new TimerWheel(WIDTH_BITS, this)),
          down_(NULL) {
    }

    void advance(Tick delta) {
        while (delta--) {
            now_++;
            size_t slot_index = now_ & MASK;
            auto slot = &slots_[slot_index];
            if (slot_index == 0 && up_) {
                up_->advance(1);
            }
            while (slot->events()) {
                auto event = slot->pop_event();
                if (down_) {
                    assert((down_->now_ & MASK) == 0);
                    Tick now = down_->now();
                    if (now >= event->scheduled_at()) {
                        event->execute();
                    } else {
                        down_->schedule(event,
                                        event->scheduled_at() - now);
                    }
                } else {
                    event->execute();
                }
            }
        }
    }

    void schedule(TimerEventInterface* event, Tick delta) {
        if (!down_) {
            event->set_scheduled_at(now_ + delta);
        }

        if (delta >= NUM_SLOTS) {
            return up_->schedule(event, (delta + (now_ & MASK)) >> WIDTH_BITS);
        }

        size_t slot_index = (now_ + delta) & MASK;
        auto slot = &slots_[slot_index];
        if (event->active()) {
            event->relink(slot);
        } else {
            slot->push_event(event);
        }
    }

    // Return the current tick value. Note that if the timers advance by
    // multiple ticks during a single call to advance(), the value of now()
    // will be the tick on which the timer was first run. Not the tick that
    // the timer eventually will advance to.
    const Tick& now() const { return now_; }

private:
    TimerWheel(const TimerWheel& other) = delete;
    TimerWheel& operator=(const TimerWheel& other) = delete;

    TimerWheel(int offset, TimerWheel* down)
        : now_(0),
          down_(down) {
        if (offset + WIDTH_BITS < 64) {
            up_ = new TimerWheel(offset + WIDTH_BITS, down);
        }
    }

    Tick now_;

    static const int WIDTH_BITS = 8;
    static const int NUM_SLOTS = 1 << WIDTH_BITS;
    static const int MASK = (NUM_SLOTS - 1);
    TimerWheelSlot slots_[NUM_SLOTS];
    TimerWheel* up_;
    TimerWheel* down_;
};

void TimerEventInterface::cancel() {
    // It's ok to cancel a timer that's not running.
    if (!slot_) {
        return;
    }

    if (this == slot_->events()) {
        slot_->pop_event();
    } else {
        auto prev = prev_;
        auto next = next_;
        if (prev) {
            prev->next_ = next;
        }
        if (next) {
            next->prev_ = prev;
        }
        prev_ = NULL;
        next_ = NULL;
    }
    slot_ = NULL;
}

void TimerEventInterface::relink(TimerWheelSlot* slot) {
    assert(slot_);
    if (slot_ == slot) {
        return;
    }
    cancel();
    slot_ = slot;
    slot_->push_event(this);
}


#endif //  _TIMER_WHEEL_H
