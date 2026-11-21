#pragma once
// MAME 0.264's device scheduler (src/emu/schedule.cpp and the timing parts
// of src/emu/diexec.cpp, BSD-3-Clause, copyright Aaron Giles), for the
// diagnostic MAME-scheduling mode: the timer list and its order, timeslices
// that run each CPU in turn up to the next timer or the end of the quantum,
// aborting the running CPU's timeslice when a timer is set at the head of
// the list, temporary quanta (perfect_quantum), suspension, and input lines
// changed through a queue a timer empties (set_state_synced). Times and
// cycle counts are computed exactly as MAME computes them.
//
// The player never uses this: its scheduler interleaves the CPUs every 64
// cycles, nearer the hardware's concurrent CPUs. This exists so a run can be
// compared with standalone MAME frame for frame.

#include "attotime.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace qilin::mame {

inline constexpr std::uint32_t suspend_halt = 0x0001, suspend_reset = 0x0002, suspend_timeslice = 0x0020,
                               suspend_any = ~0u;
enum LineState { clear_line = 0, assert_line = 1, hold_line = 2 };
inline constexpr int input_line_reset = 32, input_line_halt = 33;   // MAME's INPUT_LINE_RESET/HALT, as indices here

class Scheduler;

class Timer {
public:
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    [[nodiscard]] Time expire() const noexcept { return expire_; }
    [[nodiscard]] std::int32_t param() const noexcept { return param_; }
    void adjust(Time start_delay, std::int32_t param = 0, Time period = Time::never());
    bool enable(bool enable = true);
    [[nodiscard]] const char* name() const noexcept { return name_; }

private:
    friend class Scheduler;
    Scheduler* scheduler_ = nullptr;
    Timer *next_ = nullptr, *prev_ = nullptr;
    const char* name_ = "";
    std::function<void(std::int32_t)> callback_;
    std::int32_t param_ = 0;
    bool enabled_ = false, temporary_ = false;
    Time period_ = Time::never(), start_, expire_ = Time::never();
};

// One CPU, as device_execute_interface keeps it.
struct Exec {
    const char* tag = "";
    std::uint32_t clock = 0;
    std::int64_t attoseconds_per_cycle = 0;
    int divshift = 0;
    std::uint32_t divisor = 0;
    std::uint32_t min_cycles = 1;
    std::uint32_t cycles_per_second = 0;

    Time localtime;
    std::uint64_t totalcycles = 0;
    std::int64_t clock_cycles = 0;   // every cycle run or eaten, never reset: localtime is this many cycles
    std::int32_t cycles_running = 0, cycles_stolen = 0;
    std::uint32_t suspend = suspend_reset, nextsuspend = 0;
    bool eatcycles = false, nexteatcycles = false;

    // The core's icount, and how to run it for cycles_running (with icount
    // set): empty for the CPU whose code calls back (the lifted main CPU).
    std::int32_t* icount = nullptr;
    std::function<void()> run;
    // An input line changed (MAME's execute_set_input), and the device reset
    // when its RESET line is released (device_reset; the scheduler does the
    // interface's own pre- and post-reset work).
    std::function<void(int line, int state)> set_input;
    std::function<void()> device_reset;
    // set_periodic_int: a timer the post-reset re-arms.
    Time periodic_period;
    std::function<void()> periodic;   // the interrupt callback
    Timer* periodic_timer = nullptr;

    void configure(std::uint32_t hz, std::uint32_t min);
    [[nodiscard]] std::int64_t minimum_quantum() const noexcept { return attoseconds_per_cycle * min_cycles; }
    [[nodiscard]] Time cycles_to_time(std::int32_t cycles) const noexcept { return {0, attoseconds_per_cycle * cycles}; }

    // device_input: a queue of line changes a timer applies.
    struct Input {
        std::array<std::int32_t, 32> queue{};
        int qindex = 0;
        int curstate = clear_line;
    };
    std::array<Input, 34> inputs;
};

class Scheduler {
public:
    Scheduler();
    // A log of the scheduler's decisions, in the instrumented MAME's format,
    // for comparing the two line by line (nullptr: none).
    std::FILE* log = nullptr;
    double log_until = 1e9;

    // The executing devices in MAME's device order (the main CPU first).
    // Configure them before start(). 'execs' is the execute list: running
    // ones first, in device order, then suspended ones.
    std::vector<Exec*> devices;
    std::vector<Exec*> execs;

    Timer& timer_alloc(const char* name, std::function<void(std::int32_t)> callback);
    void timer_set(Time duration, const char* name, std::function<void(std::int32_t)> callback, std::int32_t param = 0);
    void synchronize(const char* name, std::function<void(std::int32_t)> callback, std::int32_t param = 0);
    void synchronize() { synchronize("-", {}, 0); }
    void perfect_quantum(Time duration) { add_quantum(Time{}, duration); }
    void abort_timeslice() noexcept;
    [[nodiscard]] Time time() const noexcept;
    [[nodiscard]] Time basetime() const noexcept { return basetime_; }
    [[nodiscard]] bool currently_executing() const noexcept { return executing_ != nullptr; }
    [[nodiscard]] Exec* executing() const noexcept { return executing_; }
    [[nodiscard]] Time local_time(const Exec& e) const noexcept;

    void suspend(Exec& e, std::uint32_t reason, bool eat);
    void resume(Exec& e, std::uint32_t reason);
    // set_input_line: queued, applied by a timer as MAME's set_state_synced.
    void set_input_line(Exec& e, int line, int state);
    // The device reset MAME's RESET line release makes (pre-reset, device_reset, post-reset).
    void reset_device(Exec& e);

    void start();   // after configuring: MAME's quantum and interleave set-up
    // Runs the machine until the CPU without its own 'run' (the lifted main
    // CPU) is to run next; returns the cycles it has (its icount is set).
    std::int32_t run_until_main();
    // That CPU's run has ended (its icount as it is now); go on.
    void main_ran();

private:
    friend class Timer;
    struct Quantum {
        std::int64_t requested = 0, actual = 0;
        Time expire;
    };
    void add_quantum(Time quantum, Time duration);
    void compute_perfect_interleave();
    void apply_suspend_changes();
    void timer_list_insert(Timer& t);
    void timer_list_remove(Timer& t);
    void execute_timers();
    void account(Exec& e, std::int32_t ran);
    void empty_event_queue(Exec& e, int line);
    void log_time(Time t);
    [[nodiscard]] bool logging() const noexcept { return log && basetime_.as_double() <= log_until; }

    Time basetime_;
    std::deque<std::unique_ptr<Timer>> timers_;   // every timer, permanent and temporary (temporary ones are reused)
    std::vector<Timer*> free_temporary_;
    Timer* timer_list_ = nullptr;
    Timer* inactive_ = nullptr;
    Timer* callback_timer_ = nullptr;
    bool callback_timer_modified_ = false;
    Time callback_timer_expire_time_;
    std::vector<Quantum> quanta_;   // MAME's quantum list: the first is in use
    std::int64_t quantum_minimum_ = 1'000'000;   // ATTOSECONDS_IN_NSEC(1) / 1000
    bool suspend_changes_pending_ = true;
    Exec* executing_ = nullptr;

    // Where run_until_main/main_ran are in timeslice().
    bool in_slice_ = false;
    bool slice_started_ = false;   // timeslice() entered: quanta tidied for this call
    std::size_t next_exec_ = 0;
    Time target_;
    std::int32_t main_ran_from_ = 0;   // the main CPU's cycles_running when it was given its run
};

} // namespace qilin::mame
