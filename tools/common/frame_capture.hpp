#pragma once
// --frame-out FRAME:FILE for the run tools: draws every frame with the CPU
// reference renderer, as MAME's screen would, and saves frame FRAME (counted
// from 0 at power-on, completed at the start of its vblank) as a PNG.
// --frame-hashes FILE writes a hash of every frame's colours, one line a
// frame, so a native run and a reference run can be compared frame by frame.

#include "gfx.hpp"
#include "system2_video.hpp"

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace qilin::tools {

class FrameCapture {
public:
    // Parses "FRAME:FILE"; false if malformed.
    bool add(std::string_view value) {
        const auto colon = value.find(':');
        std::uint64_t frame = 0;
        if (colon == std::string_view::npos ||
            std::from_chars(value.data(), value.data() + colon, frame).ec != std::errc{})
            return false;
        wanted_.push_back({frame, std::string(value.substr(colon + 1))});
        return true;
    }
    [[nodiscard]] bool empty() const noexcept { return wanted_.empty(); }
    // Hashes every frame to this file (opened by attach).
    std::string hashes_path;
    // Draws every frame, as the game does (--draw: to measure what video costs).
    bool draw_all = false;
    bool attach(const rom::RomSet& set, sys2::Machine& machine) {
        if (wanted_.empty() && !draw_all && hashes_path.empty()) return true;
        graphics_ = std::make_unique<sys2::Graphics>(set);
        video_ = std::make_unique<sys2::Video>(*graphics_);
        std::vector<std::uint64_t> frames;
        for (const auto& w : wanted_) frames.push_back(w.frame);
        if (!draw_all && hashes_path.empty()) video_->draw_only(std::move(frames));
        machine.video_sink = video_.get();
        if (!hashes_path.empty()) {
            hashes_.reset(std::fopen(hashes_path.c_str(), "w"));
            if (!hashes_) {
                std::print(stderr, "could not write {}\n", hashes_path);
                return false;
            }
            hasher_.capture = this;
            machine.video_sink = &hasher_;
        }
        return true;
    }
    // Call when frames may have completed. False if a file couldn't be written.
    bool poll(const sys2::Machine& machine) {
        if (!video_ || video_->frames() == seen_) return true;
        seen_ = video_->frames();
        const std::uint64_t frame = seen_ - 1;
        bool ok = true;
        for (const auto& w : wanted_) {
            if (w.frame != frame) continue;
            gfx::Image image{sys2::Frame::height, sys2::Frame::width, sys2::to_rgba(video_->frame(), machine.palette())};
            if (gfx::write_png(w.path, image)) {
                std::print("Frame {} saved to {}.\n", frame, w.path);
            } else {
                std::print(stderr, "could not write {}\n", w.path);
                ok = false;
            }
        }
        return ok;
    }

    [[nodiscard]] const sys2::Video* video() const noexcept { return video_.get(); }

private:
    // Passes the board's calls on to the renderer and hashes each frame it completes.
    struct Hasher final : sys2::Machine::VideoSink {
        FrameCapture* capture = nullptr;
        void draw_through(const sys2::Machine& machine, int scanline) override {
            capture->video_->draw_through(machine, scanline);
        }
        void vblank(const sys2::Machine& machine) override {
            capture->video_->vblank(machine);
            // FNV-1a over each pixel's colour, as shown.
            std::uint64_t h = 0xcbf29ce484222325ull;
            const auto& palette = machine.palette();
            for (const auto pen : capture->video_->frame().pens) {
                const auto c = palette.color(static_cast<std::uint8_t>(pen));
                for (const std::uint8_t byte : {c.red, c.green, c.blue}) h = (h ^ byte) * 0x100000001b3ull;
            }
            std::print(capture->hashes_.get(), "{} {:016x}\n", capture->video_->frames() - 1, h);
        }
    };
    struct CloseFile {
        void operator()(std::FILE* f) const { std::fclose(f); }
    };
    std::unique_ptr<std::FILE, CloseFile> hashes_;
    Hasher hasher_;
    struct Wanted {
        std::uint64_t frame;
        std::string path;
    };
    std::vector<Wanted> wanted_;
    std::unique_ptr<sys2::Graphics> graphics_;
    std::unique_ptr<sys2::Video> video_;
    std::uint64_t seen_ = 0;
};

} // namespace qilin::tools
