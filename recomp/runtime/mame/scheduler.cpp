#include "scheduler.hpp"

#include <algorithm>
#include <stdexcept>

namespace qilin::mame {

void Exec::configure(std::uint32_t hz, std::uint32_t min) {
    clock = hz;
    cycles_per_second = hz;
    attoseconds_per_cycle = hz_to_attoseconds(hz);
    std::int64_t attos = attoseconds_per_cycle;
    divshift = 0;
    while (attos >= (std::int64_t{1} << 31)) {
        ++divshift;
        attos >>= 1;
    }
    divisor = static_cast<std::uint32_t>(attos);
    min_cycles = min;
}

// ---- Timers ------------------------------------------------------------------

void Timer::adjust(Time start_delay, std::int32_t param, Time period) {
    auto& s = *scheduler_;
    if (s.callback_timer_ == this) s.callback_timer_modified_ = true;
    param_ = param;
    enabled_ = true;
    if (start_delay.seconds < 0) start_delay = Time{};
    start_ = s.time();
    expire_ = start_ + start_delay;
    period_ = period;
    s.timer_list_remove(*this);
    s.timer_list_insert(*this);
    if (s.logging()) {
        std::fprintf(s.log, "J %s %d ", name_, param);
        s.log_time(expire_);
        std::fputc('\n', s.log);
    }
    if (this == s.timer_list_) s.abort_timeslice();
}

bool Timer::enable(bool enable) {
    const bool old = enabled_;
    if (old != enable) {
        enabled_ = enable;
        scheduler_->timer_list_remove(*this);
        scheduler_->timer_list_insert(*this);
    }
    return old;
}

Timer& Scheduler::timer_alloc(const char* name, std::function<void(std::int32_t)> callback) {
    auto& t = *timers_.emplace_back(std::make_unique<Timer>());
    t.scheduler_ = this;
    t.name_ = name;
    t.callback_ = std::move(callback);
    t.param_ = 0;
    t.temporary_ = false;
    t.period_ = Time::never();
    t.start_ = time();
    t.expire_ = Time::never();
    t.enabled_ = false;
    timer_list_insert(t);
    return t;
}

void Scheduler::timer_set(Time duration, const char* name, std::function<void(std::int32_t)> callback, std::int32_t param) {
    Timer* t;
    if (!free_temporary_.empty()) {
        t = free_temporary_.back();
        free_temporary_.pop_back();
    } else {
        t = timers_.emplace_back(std::make_unique<Timer>()).get();
        t->scheduler_ = this;
    }
    t->next_ = t->prev_ = nullptr;
    t->name_ = name;
    t->callback_ = std::move(callback);
    t->param_ = param;
    t->temporary_ = true;
    t->period_ = Time::never();
    t->start_ = time();
    t->expire_ = t->start_ + duration;
    t->enabled_ = !t->expire_.is_never();
    timer_list_insert(*t);
    if (t->enabled_ && logging()) {
        std::fprintf(log, "T %s %d ", name, param);
        log_time(t->expire_);
        std::fputc('\n', log);
    }
    if (t == timer_list_) abort_timeslice();
}

void Scheduler::synchronize(const char* name, std::function<void(std::int32_t)> callback, std::int32_t param) {
    timer_set(Time{}, name, std::move(callback), param);
}

void Scheduler::timer_list_insert(Timer& timer) {
    if (!timer.expire_.is_never() && timer.enabled_) {
        Timer* prev = nullptr;
        for (Timer* cur = timer_list_; cur; prev = cur, cur = cur->next_) {
            if (cur->expire_ > timer.expire_) {
                timer.prev_ = prev;
                timer.next_ = cur;
                if (prev)
                    prev->next_ = &timer;
                else
                    timer_list_ = &timer;
                cur->prev_ = &timer;
                return;
            }
        }
        if (prev)
            prev->next_ = &timer;
        else
            timer_list_ = &timer;
        timer.prev_ = prev;
        timer.next_ = nullptr;
    } else {
        if (inactive_) inactive_->prev_ = &timer;
        timer.next_ = inactive_;
        timer.prev_ = nullptr;
        inactive_ = &timer;
    }
}

void Scheduler::timer_list_remove(Timer& timer) {
    if (timer.prev_)
        timer.prev_->next_ = timer.next_;
    else if (&timer == timer_list_)
        timer_list_ = timer.next_;
    else if (&timer == inactive_)
        inactive_ = timer.next_;
    if (timer.next_) timer.next_->prev_ = timer.prev_;
    timer.next_ = timer.prev_ = nullptr;
}

void Scheduler::execute_timers() {
    while (timer_list_->expire_ <= basetime_) {
        Timer& timer = *timer_list_;
        const bool was_enabled = timer.enabled_;
        if (timer.period_.is_zero() || timer.period_.is_never()) timer.enabled_ = false;
        callback_timer_modified_ = false;
        callback_timer_ = &timer;
        callback_timer_expire_time_ = timer.expire_;
        if (was_enabled) {
            if (logging()) {
                std::fprintf(log, "X %s %d ", timer.name_, timer.param_);
                log_time(timer.expire_);
                std::fputc('\n', log);
            }
            if (timer.callback_) timer.callback_(timer.param_);
        }
        if (!callback_timer_modified_) {
            if (!timer.temporary_) {
                // schedule_next_period
                timer.start_ = timer.expire_;
                timer.expire_ += timer.period_;
                timer_list_remove(timer);
                timer_list_insert(timer);
            } else {
                timer.callback_ = {};
                timer_list_remove(timer);
                free_temporary_.push_back(&timer);
            }
        }
    }
    callback_timer_ = nullptr;
}

// ---- Time, quanta, suspension -----------------------------------------------------

Time Scheduler::local_time(const Exec& e) const noexcept {
    if (executing_ == &e) return e.localtime + e.cycles_to_time(e.cycles_running - *e.icount);
    return e.localtime;
}

Time Scheduler::time() const noexcept {
    if (callback_timer_) return callback_timer_expire_time_;
    return executing_ ? local_time(*executing_) : basetime_;
}

void Scheduler::abort_timeslice() noexcept {
    if (!executing_) return;
    if (logging()) {
        std::fprintf(log, "A %s ", executing_->tag);
        log_time(local_time(*executing_));
        std::fputc('\n', log);
    }
    Exec& e = *executing_;
    const std::int32_t delta = *e.icount;
    e.cycles_stolen += delta;
    e.cycles_running -= delta;
    *e.icount -= delta;
}

void Scheduler::add_quantum(Time quantum, Time duration) {
    if (logging()) {
        std::fprintf(log, "Q %lld ", static_cast<long long>(quantum.attoseconds));
        log_time(time() + duration);
        std::fputc('\n', log);
    }
    const Time curtime = time();
    const Time expire = curtime + duration;
    const std::int64_t attos = quantum.attoseconds;
    // MAME's one pass: expired quanta go; we go after the last one no longer than us.
    std::erase_if(quanta_, [&](const Quantum& q) { return curtime >= q.expire; });
    std::ptrdiff_t insert_after = -1;
    for (std::size_t i = 0; i < quanta_.size(); ++i)
        if (quanta_[i].requested <= attos) insert_after = static_cast<std::ptrdiff_t>(i);
    if (insert_after >= 0 && quanta_[static_cast<std::size_t>(insert_after)].requested == attos) {
        auto& q = quanta_[static_cast<std::size_t>(insert_after)];
        q.expire = std::max(q.expire, expire);
    } else {
        quanta_.insert(quanta_.begin() + (insert_after + 1), Quantum{attos, std::max(attos, quantum_minimum_), expire});
    }
}

void Scheduler::compute_perfect_interleave() {
    if (execs.empty()) return;
    std::int64_t smallest = execs.front()->minimum_quantum();
    std::int64_t perfect = attos_per_second - 1;
    for (std::size_t i = 1; i < execs.size(); ++i) {
        const std::int64_t cur = execs[i]->minimum_quantum();
        if (cur < smallest) {
            perfect = smallest;
            smallest = cur;
        } else if (cur < perfect) {
            perfect = cur;
        }
    }
    if (quantum_minimum_ != perfect) {
        quantum_minimum_ = perfect;
        for (auto& q : quanta_) q.actual = std::max(q.requested, quantum_minimum_);
    }
}

void Scheduler::apply_suspend_changes() {
    std::uint32_t changed = 0;
    for (Exec* e : execs) {
        changed |= e->suspend ^ e->nextsuspend;
        e->suspend = e->nextsuspend;
        e->nextsuspend &= ~suspend_timeslice;
        e->eatcycles = e->nexteatcycles;
    }
    if (changed != 0) {
        // rebuild_execute_list: running devices first, in device order, then suspended ones.
        execs = devices;
        std::stable_partition(execs.begin(), execs.end(), [](const Exec* e) { return e->suspend == 0; });
    } else {
        suspend_changes_pending_ = false;
    }
}

void Scheduler::suspend(Exec& e, std::uint32_t reason, bool eat) {
    e.nextsuspend |= reason;
    e.nexteatcycles = eat;
    suspend_changes_pending_ = true;
    if (executing_ == &e) abort_timeslice();
}

void Scheduler::resume(Exec& e, std::uint32_t reason) {
    e.nextsuspend &= ~reason;
    suspend_changes_pending_ = true;
    if (executing_ == &e) abort_timeslice();
}

void Scheduler::set_input_line(Exec& e, int line, int state) {
    auto& in = e.inputs[static_cast<std::size_t>(line)];
    int index = in.qindex++;
    if (index >= static_cast<int>(in.queue.size())) {
        --in.qindex;
        empty_event_queue(e, line);
        index = in.qindex++;
    }
    if (index < static_cast<int>(in.queue.size())) {
        in.queue[static_cast<std::size_t>(index)] = state & 0xff;
        if (index == 0) synchronize("device_execute_interface::device_input::empty_event_queue", [this, &e, line](std::int32_t) { empty_event_queue(e, line); });
    }
}

void Scheduler::empty_event_queue(Exec& e, int line) {
    auto& in = e.inputs[static_cast<std::size_t>(line)];
    for (int i = 0; i < in.qindex; ++i) {
        in.curstate = in.queue[static_cast<std::size_t>(i)] & 0xff;
        if (line == input_line_reset) {
            if (in.curstate == assert_line) {
                suspend(e, suspend_reset, true);
            } else if ((e.nextsuspend & suspend_reset) != 0) {
                reset_device(e);
                resume(e, suspend_reset);
            }
        } else if (line == input_line_halt) {
            if (in.curstate == assert_line)
                suspend(e, suspend_halt, true);
            else if (in.curstate == clear_line)
                resume(e, suspend_halt);
        } else if (e.set_input) {
            e.set_input(line, in.curstate == clear_line ? clear_line : assert_line);
        }
    }
    in.qindex = 0;
}

void Scheduler::reset_device(Exec& e) {
    // interface_pre_reset
    e.totalcycles = 0;
    resume(e, suspend_any);
    if (e.device_reset) e.device_reset();
    // interface_post_reset: the periodic interrupt starts again from now
    if (e.periodic_timer) e.periodic_timer->adjust(e.periodic_period, 0, e.periodic_period);
}

Scheduler::Scheduler() {
    // A never-expiring timer at the end of the list, so there is always a head.
    auto& sentinel = *timers_.emplace_back(std::make_unique<Timer>());
    sentinel.scheduler_ = this;
    sentinel.name_ = "never";
    sentinel.temporary_ = true;
    sentinel.expire_ = Time::never();
    sentinel.enabled_ = false;
    timer_list_ = &sentinel;
}

void Scheduler::start() {
    execs = devices;
    std::stable_partition(execs.begin(), execs.end(), [](const Exec* e) { return e->suspend == 0; });
    // rebuild_execute_list's first quantum: machine_config::maximum_quantum(attotime::from_hz(60)).
    add_quantum(Time::from_hz(60), Time::never());
    compute_perfect_interleave();
    for (Exec* e : execs) {
        if (e->periodic_period.is_zero()) continue;
        e->periodic_timer = &timer_alloc("device_execute_interface::trigger_periodic_interrupt", [e](std::int32_t) {
            if ((e->nextsuspend & (suspend_halt | suspend_reset)) == 0 && e->periodic) e->periodic();
        });
    }
}

// ---- Timeslices ------------------------------------------------------------------

void Scheduler::account(Exec& e, std::int32_t ran) {
    e.totalcycles += static_cast<std::uint64_t>(ran);
    e.clock_cycles += ran;
    Time delta;
    if (static_cast<std::uint32_t>(ran) < e.cycles_per_second)
        delta = Time{0, e.attoseconds_per_cycle * ran};
    else
        delta = Time{static_cast<std::int32_t>(static_cast<std::uint32_t>(ran) / e.cycles_per_second),
                     static_cast<std::int64_t>(static_cast<std::uint32_t>(ran) % e.cycles_per_second) * e.attoseconds_per_cycle};
    e.localtime += delta;
    if (logging()) {
        std::fprintf(log, "R %s %d %d %u ", e.tag, e.cycles_running + e.cycles_stolen, ran, e.suspend);
        log_time(e.localtime);
        std::fputc('\n', log);
    }
    if (e.localtime < target_) target_ = std::max(e.localtime, basetime_);
}

std::int32_t Scheduler::run_until_main() {
    for (;;) {
        if (!in_slice_) {
            if (!slice_started_) {
                // timeslice(): expired quanta go
                while (basetime_ >= quanta_.front().expire) quanta_.erase(quanta_.begin());
                slice_started_ = true;
            }
            if (!(basetime_ < timer_list_->expire_)) {
                execute_timers();
                slice_started_ = false;   // the next timeslice() call
                continue;
            }
            target_ = basetime_ + Time{0, quanta_.front().actual};
            if (timer_list_->expire_ < target_) target_ = timer_list_->expire_;
            if (suspend_changes_pending_) apply_suspend_changes();
            if (logging()) {
                std::fputs("S ", log);
                log_time(basetime_);
                std::fputc(' ', log);
                log_time(target_);
                std::fputc('\n', log);
            }
            in_slice_ = true;
            next_exec_ = 0;
        }
        while (next_exec_ < execs.size()) {
            Exec& e = *execs[next_exec_++];
            if (!((e.suspend == 0 || e.eatcycles) && target_.seconds >= e.localtime.seconds)) continue;
            std::int64_t delta = target_.attoseconds - e.localtime.attoseconds;
            if (delta < 0 && target_.seconds > e.localtime.seconds) delta += attos_per_second;
            if (e.attoseconds_per_cycle == 0) {
                e.localtime = target_;
                continue;
            }
            if (delta < e.attoseconds_per_cycle) continue;
            const std::int32_t cycles =
                static_cast<std::int32_t>((static_cast<std::uint64_t>(delta) >> e.divshift) / e.divisor);
            e.cycles_running = cycles;
            std::int32_t ran = cycles;
            if (e.suspend == 0) {
                e.cycles_stolen = 0;
                executing_ = &e;
                *e.icount = cycles;
                if (!e.run) {
                    main_ran_from_ = cycles;
                    return cycles;   // the lifted CPU runs, and comes back through main_ran()
                }
                e.run();
                ran -= *e.icount;
                ran -= e.cycles_stolen;
                executing_ = nullptr;
            }
            account(e, ran);
        }
        basetime_ = target_;
        in_slice_ = false;
    }
}

void Scheduler::main_ran() {
    Exec& e = *executing_;
    std::int32_t ran = main_ran_from_;
    ran -= *e.icount;
    ran -= e.cycles_stolen;
    executing_ = nullptr;
    account(e, ran);
}

void Scheduler::log_time(Time t) {
    std::fprintf(log, "%d.%018lld", t.seconds, static_cast<long long>(t.attoseconds));
}

} // namespace qilin::mame
