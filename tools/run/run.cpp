// Headless native run: both CPUs' lifted code on the System 2 board model,
// scheduled by recomp/runtime/game, from power-on for a number of frames.
//
//   qilin_run <rom-dir> [--frames N] [--coin FRAME]... [--press MASK:FRAME]... [--pedal V] [--steer V] [--log FILE]
//             [--frame-out FRAME:FILE]... [--look EFFECT:WxH] [--frame-hashes FILE] [--io-trace FILE] [--draw 1] [--wav FILE] [--monkey SEED] [--test-switch 1]
//
// --mame-scheduling 1 interleaves the CPUs as standalone MAME 0.264 does
// (game/mame_mode.hpp), for comparing with MAME; --mame-log FILE writes its
// scheduler's decisions in the instrumented MAME's format (for the first
// --mame-log-until SECONDS, default 10). Diagnostic only.
//
// --audio-threaded 1 uses the player's ordered audio worker with --wav.
// --wav FILE records the board's sound (the YM2151, both POKEYs
// and the TMS5220's speech, mixed as MAME mixes them) as a 16-bit stereo WAV
// at 55,930 Hz.
//
// --draw 1 draws every frame with the reference renderer, as apb does, to
// measure what video costs.
//
// The inputs are qilin_refrun's and change on the same cycle (tools/common/script.hpp),
// so runs of the two can be compared frame by frame (--frame-hashes).
//
// Reports how far the game got (frames, the sound handshake, interrupts
// taken, sound resets) and why it stopped: the frame count, the watchdog, or
// control reaching an address nothing was lifted for. Needs your ROMs and a
// build with both -DQILIN_GEN_DIR (qilin lift) and -DQILIN_SOUND_GEN_DIR
// (qilin lift --cpu sound).

#include "frame_capture.hpp"
#include "script.hpp"
#include "system2_audio.hpp"
#include "audio_worker.hpp"
#include "input_movie.hpp"
#include "wav.hpp"
#include "game.hpp"
#include "mame_mode.hpp"
#include "romset.hpp"
#include "system2_machine.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <format>
#include <fstream>
#include <string>
#include <memory>
#include <optional>
#include <vector>
#include <charconv>
#include <print>
#include <string_view>

int main(int argc, char** argv) {
    using namespace qilin;
    if (argc < 2) {
        std::print(stderr, "usage: qilin_run <rom-dir> [--frames N] [--record FILE.qir | --replay FILE.qir]\n"
                           "                 [--profile FILE] [--draw 1] [--wav FILE] [--audio-threaded 1]\n"
                           "                 [--frame-hashes FILE] [--frame-out FRAME:FILE]\n");
        return 2;
    }
    if (argc % 2 != 0) {
        std::fprintf(stderr, "Each option needs a value\n"); return 2;
    }
    std::uint64_t frames = 600;
    bool threaded = false, frames_specified = false;
    std::string record_path, replay_path, profile_path;
    std::int32_t quantum = 64;
    bool mame_scheduling = false;
    std::string_view mame_log_path;
    double mame_log_until = 10;
    std::string_view log_path, wav_path, io_trace_path, input_trace_path;
    tools::Script script;
    tools::FrameCapture capture;
    for (int i = 2; i + 1 < argc; i += 2) {
        const std::string_view flag = argv[i], value = argv[i + 1];
        std::uint64_t number = 0;
        std::from_chars(value.data(), value.data() + value.size(), number);
        if (flag == "--frames") { frames = number; frames_specified = true; }
        else if (flag == "--profile") profile_path = value;
        else if (flag == "--record") record_path = value;
        else if (flag == "--replay") replay_path = value;
        else if (flag == "--quantum") {
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), quantum);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || quantum < 1) {
                std::fprintf(stderr, "--quantum needs a positive 32-bit cycle count\n"); return 2;
            }
        }
        else if (flag == "--mame-scheduling") mame_scheduling = number != 0;
        else if (flag == "--mame-log") mame_log_path = value;
        else if (flag == "--mame-log-until") mame_log_until = std::strtod(std::string(value).c_str(), nullptr);
        else if (flag == "--log") log_path = value;
        else if (flag == "--wav") wav_path = value;
        else if (flag == "--audio-threaded") threaded = number != 0;
        else if (script.option(flag, value)) {}
        else if (flag == "--frame-hashes") capture.hashes_path = value;
        else if (flag == "--io-trace") io_trace_path = value;
        else if (flag == "--input-trace") input_trace_path = value;
        else if (flag == "--draw") capture.draw_all = number != 0;
        else if (flag == "--mame-pixels") capture.mame_pixels = number != 0;
        else if (flag == "--look") {
            if (!capture.look(value)) return 2;
        }
        else if (flag == "--frame-out") {
            if (!capture.add(value)) {
                std::print(stderr, "qilin_run: --frame-out wants FRAME:FILE\n");
                return 2;
            }
        }
        else {
            std::print(stderr, "qilin_run: unknown option {}\n", flag);
            return 2;
        }
    }
    if (!script.error().empty()) {
        std::fprintf(stderr, "%s\n", script.error().c_str()); return 2;
    }
    if ((!record_path.empty() && !replay_path.empty()) || (!replay_path.empty() && script.used()) ||
        ((!record_path.empty() || !replay_path.empty()) && quantum != 64)) {
        std::fprintf(stderr, "Input movies require the default quantum; replay cannot combine recording or scripted controls\n"); return 2;
    }
    if ((!profile_path.empty() && (profile_path == record_path || profile_path == replay_path)) ||
        (!wav_path.empty() && (wav_path == record_path || wav_path == replay_path))) {
        std::fprintf(stderr, "Movie and diagnostic output paths must differ\n"); return 2;
    }
    auto profiler = profile_path.empty() ? nullptr : std::make_unique<profile::Profiler>();
    capture.profiler = profiler.get();
    const auto set = rom::RomSet::load(argv[1]);
    if (!set) {
        std::print(stderr, "qilin_run: {}\n", set.error());
        return 1;
    }
    sys2::Machine machine(set->region(rom::Region::main_cpu), set->region(rom::Region::sound_cpu));
    std::vector<sys2::Machine::LogEntry> log;
    if (!log_path.empty()) machine.log = &log;
    if (!capture.attach(*set, machine)) return 1;
    // --io-trace FILE: the sound CPU's chip and latch accesses, time in main-CPU cycles, as MAME's Lua tap logs them.
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> io_trace(nullptr, std::fclose);
    if (!io_trace_path.empty()) {
        io_trace.reset(std::fopen(std::string(io_trace_path).c_str(), "w"));
        if (!io_trace) {
            std::print(stderr, "qilin_run: can't write {}\n", io_trace_path);
            return 1;
        }
        machine.io_trace = [&](char kind, std::uint16_t address, std::uint8_t value, std::int64_t sound_time) {
            std::print(io_trace.get(), "{} {:04X} {:02X} {}\n", kind, address, value, sys2::main_cycles_at(sound_time));
        };
    }
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> input_trace(nullptr, std::fclose);
    if (!input_trace_path.empty()) {
        input_trace.reset(std::fopen(std::string(input_trace_path).c_str(), "w"));
        if (!input_trace) { std::print(stderr, "qilin_run: can't write {}\n", input_trace_path); return 1; }
        machine.input_trace = [&](std::uint16_t address, std::uint16_t value, std::int64_t time) {
            std::print(input_trace.get(), "r {:04X} {:04X} {}\n", address, value, time);
        };
    }
    script.attach(machine);
    movie::Reader replay;
    movie::Writer recording;
    if (!replay_path.empty()) {
        if (!replay.load(replay_path)) { std::fprintf(stderr, "%s\n", replay.error().c_str()); return 1; }
        machine.eeprom() = replay.eeprom();
        replay.apply(0, machine.inputs);
        frames = frames_specified ? std::min(frames, replay.duration()) : replay.duration();
        machine.on_vblank = [&](std::uint64_t frame) { replay.apply(frame, machine.inputs); };
        if (replay.interrupted()) std::fprintf(stderr, "Recording interrupted; replaying validated input prefix\n");
    }
    if (!record_path.empty()) {
        if (!recording.open(record_path, machine.eeprom(), machine.inputs)) { std::fprintf(stderr, "%s\n", recording.error().c_str()); return 1; }
        const auto controls = machine.on_vblank;
        machine.on_vblank = [&, controls](std::uint64_t frame) {
            controls(frame);
            if (!recording.frame(frame, machine.inputs)) throw game::Stop{game::Stop::frames, recording.error()};
        };
    }
    std::unique_ptr<sys2::Audio> audio;
    std::vector<std::int16_t> sound;
    std::unique_ptr<sys2::AudioWorker> worker;
    if (!wav_path.empty()) {
        if (threaded) {
            worker = std::make_unique<sys2::AudioWorker>([&](std::span<const std::int16_t> pcm) {
                sound.insert(sound.end(), pcm.begin(), pcm.end()); return true;
            }, profiler.get());
            machine.sound_sink = worker.get();
        } else {
            audio = std::make_unique<sys2::Audio>();
            machine.sound_sink = audio.get();
        }
    }
    game::Scheduler scheduler(machine);
    scheduler.quantum = quantum;
    std::unique_ptr<game::MameRun> mame_run;
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> mame_log(nullptr, &std::fclose);
    if (mame_scheduling) {
        if (quantum != 64 || !record_path.empty() || !replay_path.empty()) {
            std::fprintf(stderr, "--mame-scheduling has its own interleave: no --quantum or input movies\n"); return 2;
        }
        mame_run = std::make_unique<game::MameRun>(machine, scheduler);
        if (!mame_log_path.empty()) {
            mame_log.reset(std::fopen(std::string(mame_log_path).c_str(), "w"));
            if (!mame_log) {
                std::fprintf(stderr, "Can't write %s\n", std::string(mame_log_path).c_str()); return 1;
            }
            mame_run->set_log(mame_log.get(), mame_log_until);
        }
    } else if (!mame_log_path.empty()) {
        std::fprintf(stderr, "--mame-log needs --mame-scheduling 1\n"); return 2;
    }
    // The controls change at each vblank (script.attach), as in qilin_refrun.
    scheduler.on_frame = [&](std::uint64_t frame) {
        profile::FrameMark frame_profile(profiler.get(), frame);
        capture.poll(machine);
        if (worker || audio) machine.advance_speech(machine.sound_time);
        if (worker && !worker->submit(machine.sound_time)) throw game::Stop{game::Stop::frames, "audio worker failed"};
        if (audio) {
            audio->advance_batch(machine.sound_time);
            sound.insert(sound.end(), audio->frames().begin(), audio->frames().end());
            audio->frames().clear();
        }
    };
    const auto start = std::chrono::steady_clock::now();
    const auto stop = scheduler.run(frames);
    if (io_trace) std::print(io_trace.get(), "# end {}\n", sys2::main_cycles_at(machine.sound_time));
    if (!record_path.empty() && (!recording.active() || !recording.finish(machine.frames()))) {
        std::fprintf(stderr, "%s\n", recording.error().c_str()); return 1;
    }
    if (worker && !worker->drain()) {
        std::print(stderr, "qilin_run: audio worker: {}\n", worker->error()); return 1;
    }
    if (worker) worker->finish();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (profiler && !profiler->save(profile_path, "headless native run; board frames=" + std::to_string(machine.frames()))) { std::fprintf(stderr, "Cannot save profile\n"); return 1; }

    const char* reasons[] = {"the frame count", "the watchdog", "unlifted main-CPU code", "unlifted sound-CPU code"};
    std::print("Stopped by {}{}{} at frame {} ({:.2f} s of game time, {:.2f} s here: {:.1f}x real time).\n",
               reasons[stop.reason], stop.detail.empty() ? "" : ": ", stop.detail, machine.frames(),
               static_cast<double>(machine.main_time) / 1e7, seconds, static_cast<double>(machine.main_time) / 1e7 / seconds);
    const auto& c = machine.counters;
    const auto& s = scheduler.counters;
    std::print("Sound: released from reset {} times; {} commands ({} read), {} replies ({} read); {} NMIs, {} IRQs.\n",
               s.sound_resets, c.sound_commands, c.sound_command_reads, c.sound_replies, c.sound_reply_reads, s.sound_nmis,
               s.sound_irqs);
    std::print("Main: {} interrupts taken, {} services; {} watchdog strobes; {} EEPROM writes.\n", s.main_interrupts,
               s.main_services, c.watchdog_strobes, c.eeprom_writes);
    std::print("Speech: {} bytes written to the TMS5220, {} samples made, {} of them louder than idle.\n", c.speech_bytes,
               machine.speech().samples(), machine.speech().loud_samples());
    std::print("T-11 at {:04X}, PSW {:03o}, banks {}/{}; CP lines {:X}, interrupt enable {:X}; 6502 at {:04X}.\n",
               scheduler.main.cpu.r[7], scheduler.main.cpu.psw, machine.bank(0), machine.bank(1), machine.main_cp_lines(),
               machine.interrupt_enable(), scheduler.sound.cpu.pc);
    if (const auto* v = capture.video(); v && capture.draw_all)
        std::print("Video: {} frames drawn in {} bands.\n", v->frames(), v->bands());
    if (!wav_path.empty()) {
        if (!tools::write_wav(std::string(wav_path), sound, 55930, 14318181, 256)) {
            std::print(stderr, "qilin_run: can't write {}\n", wav_path);
            return 1;
        }
        std::print("Sound: {:.1f} s written to {} ({} chip frames).\n", static_cast<double>(sound.size() / 2) / sys2::Audio::sample_rate, wav_path, sound.size() / 2);
    }
    if (!log_path.empty()) {
        std::ofstream out{std::string(log_path), std::ios::trunc};
        for (const auto& e : log) out << std::format("{} {:02X} {}\n", e.what, e.value, e.time);
    }
    return stop.reason == game::Stop::frames ? 0 : 1;
}
