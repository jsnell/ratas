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

typedef uint64_t Timestamp;

template<typename CBType>
class TimerWheelSlot;

template<typename CBType>
class HierarchicalTimerWheel;

template<typename CBType>
class TimerEvent {
public:
    TimerEvent<CBType>(const CBType& callback,
                       HierarchicalTimerWheel<CBType>* timers)
      : callback_(callback),
        prev_(NULL),
        next_(NULL) {
    }

    void cancel() {
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

    void relink(TimerWheelSlot<CBType>* slot) {
        assert(slot_);
        if (slot_ == slot) {
            return;
        }
        cancel();
        slot_ = slot;
        slot_->push_event(this);
    }

    void execute() {
        callback_();
    }

    bool active() {
        return slot_ != NULL;
    }

    Timestamp scheduled_at() { return scheduled_at_; }
    void set_scheduled_at(Timestamp ts) { scheduled_at_ = ts; }

private:
    TimerEvent(const TimerEvent& other) = delete;
    TimerEvent& operator=(const TimerEvent& other) = delete;

    Timestamp scheduled_at_;
    CBType callback_;
    // The slot this event is currently in (NULL if not currently scheduled).
    TimerWheelSlot<CBType>* slot_ = NULL;
    // The events are linked together in the slot using an internal
    // doubly-linked list; this iterator does double duty as the
    // linked list node for this event.
    // typename std::list<TimerEvent<CBType>*>::iterator it_;
    TimerEvent<CBType>* prev_;
    TimerEvent<CBType>* next_;
    friend TimerWheelSlot<CBType>;
};

template<typename CBType>
class TimerWheelSlot {
public:
    TimerWheelSlot() : events_(NULL) {
    }

    TimerEvent<CBType>* events() { return events_; }
    TimerEvent<CBType>* pop_event() {
        auto event = events_;
        events_ = event->next_;
        if (events_) {
            events_->prev_ = NULL;
        }
        event->next_ = NULL;
        event->slot_ = NULL;
        return event;
    }
    void push_event(TimerEvent<CBType>* event) {
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

    TimerEvent<CBType>* events_;
};

template<typename CBType>
class HierarchicalTimerWheel {
public:
    HierarchicalTimerWheel()
        : now_(0),
          up_(new HierarchicalTimerWheel<CBType>(WIDTH_BITS, this)),
          down_(NULL) {
    }

    void advance(Timestamp delta) {
        while (delta--) {
            now_++;
            size_t slot_index = now_ & (NUM_SLOTS - 1);
            auto slot = &slots_[slot_index];
            while (slot->events()) {
                auto event = slot->pop_event();
                if (down_) {
                    assert((down_->now_ & (NUM_SLOTS - 1)) == 0);
                    down_->schedule_absolute(event, event->scheduled_at());
                } else {
                    event->execute();
                }
            }
            if (slot_index == 0 && up_) {
                up_->advance(1);
            }
        }
    }

    void schedule(TimerEvent<CBType>* event, Timestamp delta) {
        if (!down_) {
            event->set_scheduled_at(now_ + delta);
        }

        if (delta >= NUM_SLOTS) {
            return up_->schedule(event, delta >> WIDTH_BITS);
        }

        size_t slot_index = (now_ + delta) & (NUM_SLOTS - 1);
        auto slot = &slots_[slot_index];
        slot->push_event(event);
    }

private:
    HierarchicalTimerWheel(const HierarchicalTimerWheel& other) = delete;
    HierarchicalTimerWheel& operator=(const HierarchicalTimerWheel& other) = delete;

    HierarchicalTimerWheel(int offset, HierarchicalTimerWheel* down)
        : now_(0),
          down_(down) {
        if (offset + WIDTH_BITS < 64) {
            up_ = new HierarchicalTimerWheel<CBType>(offset + WIDTH_BITS, this);
        }
    }

    void schedule_absolute(TimerEvent<CBType>* event, Timestamp absolute) {
        Timestamp delta;
        delta = absolute - now_;
        assert(absolute > now_);
        assert(delta < NUM_SLOTS);
        if (delta == 0) {
            if (!down_) {
                event->execute();
            } else {
                down_->schedule_absolute(event, absolute);
            }
            return;
        }

        schedule(event, delta);
    }

    Timestamp now_;

    static const int WIDTH_BITS = 8;
    static const int NUM_SLOTS = 1 << WIDTH_BITS;
    TimerWheelSlot<CBType> slots_[NUM_SLOTS];
    HierarchicalTimerWheel<CBType>* up_;
    HierarchicalTimerWheel<CBType>* down_;
};

#endif //  _TIMER_WHEEL_H
