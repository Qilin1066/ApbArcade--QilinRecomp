#pragma once
// --frame-out FRAME:FILE for the run tools: draws every frame with the CPU
// reference renderer, as MAME's screen would, and saves frame FRAME (counted
// from 0 at power-on, completed at the start of its vblank) as a PNG.

#include "gfx.hpp"
#include "system2_video.hpp"

#include <charconv>
#include <cstdint>
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
    void attach(const rom::RomSet& set, sys2::Machine& machine) {
        if (wanted_.empty()) return;
        graphics_ = std::make_unique<sys2::Graphics>(set);
        video_ = std::make_unique<sys2::Video>(*graphics_);
        std::vector<std::uint64_t> frames;
        for (const auto& w : wanted_) frames.push_back(w.frame);
        video_->draw_only(std::move(frames));
        machine.video_sink = video_.get();
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

private:
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
