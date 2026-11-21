#pragma once
// --frame-out FRAME:FILE for the run tools: draws every frame with the CPU
// reference renderer, as MAME's screen would, and saves frame FRAME (counted
// from 0 at power-on, completed at the start of its vblank) as a PNG.
// --frame-hashes FILE writes a hash of every frame's colours, one line a
// frame, so a native run and a reference run can be compared frame by frame.
// --look EFFECT:WxH saves those frames as a W x H window would show them
// instead, through the Vulkan renderer: none, pixel_art or crt (scaled by
// whole multiples where they fit, else to fit; the monitor upright, as APB's).

#include "gfx.hpp"
#include "system2_video.hpp"

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <algorithm>
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
    // Parses "EFFECT:WxH" for --look and starts the renderer; false (with a message) if it can't.
    bool look(std::string_view value) {
        const auto colon = value.find(':'), x = value.find('x', colon == std::string_view::npos ? 0 : colon);
        gfx::Config config;
        config.width = sys2::Frame::height;
        config.height = sys2::Frame::width;
        config.upright_tube = true;
        const auto effect = value.substr(0, colon);
        if (colon == std::string_view::npos || x == std::string_view::npos ||
            std::from_chars(value.data() + colon + 1, value.data() + x, config.output_width).ec != std::errc{} ||
            std::from_chars(value.data() + x + 1, value.data() + value.size(), config.output_height).ec != std::errc{} ||
            (effect != "none" && effect != "pixel_art" && effect != "crt")) {
            std::print(stderr, "--look wants EFFECT:WxH (EFFECT none, pixel_art or crt)\n");
            return false;
        }
        config.effect = effect == "crt" ? gfx::Effect::crt : effect == "pixel_art" ? gfx::Effect::pixel_art : gfx::Effect::none;
        std::string error;
        look_ = gfx::Renderer::create(config, &error);
        if (!look_) std::print(stderr, "--look: {}\n", error);
        return look_ != nullptr;
    }
    // Hashes every frame to this file (opened by attach).
    std::string hashes_path;
    // Draws every frame, as the game does (--draw: to measure what video costs).
    bool draw_all = false;
    profile::Profiler* profiler = nullptr;
    // --mame-pixels 1: save frame N as MAME's Lua screen:pixels() shows it
    // from its frame_done callback for frame N, which runs after the screen
    // has swapped buffers: frame N-1's picture, in the palette as it stands
    // at frame N's vblank (for comparing with standalone MAME).
    bool mame_pixels = false;
    bool attach(const rom::RomSet& set, sys2::Machine& machine) {
        if (wanted_.empty() && !draw_all && hashes_path.empty()) return true;
        graphics_ = std::make_unique<sys2::Graphics>(set);
        video_ = std::make_unique<sys2::Video>(*graphics_);
        video_->profiler = profiler;
        std::vector<std::uint64_t> frames;
        for (const auto& w : wanted_) frames.push_back(mame_pixels && w.frame > 0 ? w.frame - 1 : w.frame);
        if (!draw_all && hashes_path.empty()) video_->draw_only(std::move(frames));
        machine.video_sink = video_.get();
        if (mame_pixels) {
            if (!hashes_path.empty() || look_) {
                std::print(stderr, "--mame-pixels saves plain frames only (no --frame-hashes or --look)\n");
                return false;
            }
            mame_sink_.capture = this;
            machine.video_sink = &mame_sink_;
        }
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
        if (mame_pixels) return mame_ok_;
        if (!video_ || video_->frames() == seen_) return true;
        seen_ = video_->frames();
        const std::uint64_t frame = seen_ - 1;
        bool ok = true;
        for (const auto& w : wanted_) {
            if (w.frame != frame) continue;
            gfx::Image image{sys2::Frame::height, sys2::Frame::width, sys2::to_rgba(video_->frame(), machine.palette())};
            if (look_) {
                std::vector<std::uint32_t> bgra(std::size_t{sys2::Frame::width} * sys2::Frame::height);
                sys2::to_bgra(video_->frame(), machine.palette(), bgra.data());
                look_->write_back_buffer(bgra.data());
                look_->present();
                image = look_->read_output();
            }
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
    // --mame-pixels: keeps the pens of each frame before a wanted one, and
    // writes them at the wanted frame's vblank in the palette then.
    struct MamePixels final : sys2::Machine::VideoSink {
        FrameCapture* capture = nullptr;
        std::vector<std::uint16_t> previous;
        std::uint64_t previous_frame = ~0ull;
        void draw_through(const sys2::Machine& machine, int scanline) override {
            capture->video_->draw_through(machine, scanline);
        }
        void vblank(const sys2::Machine& machine) override {
            auto& video = *capture->video_;
            video.vblank(machine);
            const std::uint64_t completed = video.frames() - 1;
            for (const auto& w : capture->wanted_) {
                if (w.frame != completed || previous_frame + 1 != completed) continue;
                const auto shown = std::make_unique<sys2::Frame>();
                std::copy(previous.begin(), previous.end(), shown->pens.begin());
                const gfx::Image image{sys2::Frame::height, sys2::Frame::width, sys2::to_rgba(*shown, machine.palette())};
                if (gfx::write_png(w.path, image)) {
                    std::print("Frame {} (MAME's pixels: frame {}'s picture) saved to {}.\n", completed, previous_frame, w.path);
                } else {
                    std::print(stderr, "could not write {}\n", w.path);
                    capture->mame_ok_ = false;
                }
            }
            for (const auto& w : capture->wanted_) {
                if (w.frame == completed + 1) {
                    const auto& pens = video.frame().pens;
                    previous.assign(pens.begin(), pens.end());
                    previous_frame = completed;
                }
            }
        }
    };
    MamePixels mame_sink_;
    bool mame_ok_ = true;
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
    std::unique_ptr<gfx::Renderer> look_;
    std::uint64_t seen_ = 0;
};

} // namespace qilin::tools
