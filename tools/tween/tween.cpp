// In-between frames, a prototype: the game run natively with a script, its
// video state recorded band by band (each band of scanlines with its own
// scroll, tile banks, video RAM and resolved motion objects), and frames
// drawn between each pair of game frames at in-between scroll and sprite
// positions, at a raised resolution so moves of a fraction of a pixel show.
//
//   qilin_tween <rom-dir> --from F --to G [--factor N] [--scale S] [--out FILE.mp4] [script options]
//
// First it checks itself: each recorded frame drawn at t = 0 and scale 1 must
// be the frame the CPU reference renderer drew, pixel for pixel. Then, with
// --out, it pipes frames F..G to ffmpeg: on the left as the game shows them
// (each repeated N times), on the right with N-1 drawn between each pair, at
// 60.1 x N frames a second. It reports how many frames could be interpolated
// and how many sprites matched.
#include "frame_capture.hpp"
#include "script.hpp"
#include "game.hpp"
#include "romset.hpp"
#include "system2_machine.hpp"
#include "system2_video.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <format>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#endif

namespace {

using namespace qilin;

// Division rounding down, for coordinates that may be negative.
constexpr long floor_div(long a, long b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
constexpr int W = sys2::Frame::width, H = sys2::Frame::height;   // 512 x 384, as the board scans

struct Object {   // a motion object as atarimo.cpp places it, after links and neighbours
    int x = 0, y = 0;
    unsigned code = 0, color = 0;   // color: palette base | priority << 12
    int height = 1;
    bool hflip = false;
};

struct Band {
    int first = 0, last = 0;
    int sx = 0, sy = 0;
    unsigned banks[2]{};
    std::array<std::array<std::uint16_t, 0x1000>, 3> pages{};
    std::vector<Object> objects;
};

struct FrameState {
    std::vector<Band> bands;
    sys2::PaletteRam palette;
};

// The motion-object list as render() walks it (system2_video.cpp).
std::vector<Object> resolve(const std::array<std::array<std::uint16_t, 0x1000>, 3>& pages) {
    std::vector<Object> out;
    std::array<bool, 256> visited{};
    int last_xpos = 0;
    for (unsigned link = 0, n = 0; n < 256 && !visited[link]; ++n) {
        visited[link] = true;
        const std::uint16_t* e = &pages[0][0xc00 + link * 4];
        link = (e[3] >> 3) & 0xff;
        Object o;
        o.code = (e[1] & 0x7ff) | ((e[0] & 7u) << 11);
        o.color = ((e[3] >> 12) & 3) * 16 | (static_cast<unsigned>((e[3] >> 14) & 3) << 12);
        int xpos = (e[2] >> 6) & 0x3ff;
        o.height = ((e[1] >> 11) & 7) + 1;
        o.hflip = (e[1] & 0x4000) != 0;
        int ypos = -((e[0] >> 6) & 0x1ff) - o.height * 16;
        if (e[1] & 0x8000) xpos = last_xpos + 16;
        last_xpos = xpos;
        xpos &= 1023;
        ypos &= 511;
        if (xpos >= 640) xpos -= 1024;
        if (ypos >= 416) ypos -= 512;
        if (xpos <= -16 || xpos >= W) continue;
        o.x = xpos;
        o.y = ypos;
        out.push_back(o);
    }
    return out;
}

// Records the video state of every band of every frame, alongside the reference renderer.
class Recorder final : public sys2::Machine::VideoSink {
public:
    explicit Recorder(sys2::Video& video) : video_(video) {}
    void draw_through(const sys2::Machine& machine, int scanline) override {
        if (scanline < H && scanline >= next_) {   // vblank's requests draw nothing, as MAME's (system2_video.cpp)
            if (recording) {
                Band b;
                b.first = next_;
                b.last = scanline;
                b.sx = machine.playfield_scroll_x();
                b.sy = machine.playfield_scroll_y();
                b.banks[0] = machine.playfield_tile_bank(0);
                b.banks[1] = machine.playfield_tile_bank(1);
                b.pages = machine.video().pages();
                b.objects = resolve(b.pages);
                current_.bands.push_back(std::move(b));
            }
            next_ = scanline + 1;
        }
        video_.draw_through(machine, scanline);
    }
    void vblank(const sys2::Machine& machine) override {
        draw_through(machine, H - 1);
        video_.vblank(machine);
        if (recording) {
            current_.palette = machine.palette();
            frames.push_back(std::move(current_));
            references.push_back(video_.frame());
        }
        current_ = {};
        next_ = 0;
    }
    bool recording = false;
    std::vector<FrameState> frames;
    std::vector<sys2::Frame> references;

private:
    sys2::Video& video_;
    FrameState current_;
    int next_ = 0;
};

// A band as it's drawn between two frames: the nearer frame's video RAM, with
// scroll and objects possibly moved.
struct Drawn {
    const Band* band = nullptr;   // video RAM, banks and the alphanumerics
    double sx = 0, sy = 0;
    struct Placed {
        Object object;
        double x, y;
    };
    std::vector<Placed> objects;
};

// Draws scanlines [first, last] (board scanlines) of a band at scale s into
// pens (W*s x H*s), as render() does at s = 1.
void draw(const sys2::Graphics& g, const Drawn& d, int first, int last, int s, std::vector<std::uint16_t>& pens) {
    const int WS = W * s;
    const auto& pages = d.band->pages;
    auto playfield = [&](int tx, int ty) {
        const unsigned index = static_cast<unsigned>((ty >> 3) * 128 + (tx >> 3));
        return index < 0x1000 ? pages[1][index] : pages[2][index & 0xfff];
    };
    // Scroll in output pixels: a scaled-up pixel is s output pixels.
    const auto ssx = static_cast<long>(std::lround(d.sx * s)), ssy = static_cast<long>(std::lround(d.sy * s));
    std::vector<std::uint16_t> mo(static_cast<std::size_t>(WS), 0xffff);
    for (int Y = first * s; Y <= last * s + s - 1; ++Y) {
        const long vy = Y + ssy;
        const int ty = static_cast<int>((floor_div(vy, s) % 512 + 512) % 512);
        std::uint16_t* out = &pens[static_cast<std::size_t>(Y) * WS];
        for (int X = 0; X < WS; ++X) {
            const long vx = X + ssx;
            const int tx = static_cast<int>((floor_div(vx, s) % 1024 + 1024) % 1024);
            const std::uint16_t data = playfield(tx, ty);
            const unsigned code = (d.band->banks[(data >> 10) & 1] << 10) | (data & 0x3ff);
            out[X] = static_cast<std::uint16_t>(128 + ((data >> 11) & 7) * 16 + g.tile(code)[(ty & 7) * 8 + (tx & 7)]);
        }
        // Motion objects on this output row, in list order (later on top).
        std::fill(mo.begin(), mo.end(), std::uint16_t{0xffff});
        for (const auto& p : d.objects) {
            const long top = std::lround(p.y * s), left = std::lround(p.x * s);
            const long row = Y - top;
            if (row < 0 || row >= 16L * p.object.height * s) continue;
            const int py = static_cast<int>(row / s), tile = py / 16;
            const std::uint8_t* pixels = g.sprite(p.object.code + static_cast<unsigned>(tile)) + (py % 16) * 16;
            for (long X = std::max(0L, left); X < std::min<long>(WS, left + 16L * s); ++X) {
                const int px = static_cast<int>((X - left) / s);
                const auto pen = pixels[p.object.hflip ? 15 - px : px];
                if (pen != 15) mo[static_cast<std::size_t>(X)] = static_cast<std::uint16_t>(pen + p.object.color);
            }
        }
        for (int X = 0; X < WS; ++X) {
            const std::uint16_t m = mo[static_cast<std::size_t>(X)];
            if (m == 0xffff) continue;
            const int tx = static_cast<int>((floor_div(X + ssx, s) % 1024 + 1024) % 1024);
            const int category = (~playfield(tx, ty) >> 14) & 3;
            if (((m >> 12) + category) & 2) {
                if (!(out[X] & 0x08)) out[X] = m & 0x0fff;
            } else {
                out[X] = m & 0x0fff;
            }
        }
        // Alphanumerics, unmoved.
        const int y = Y / s;
        const std::uint16_t* cells = &pages[0][(y >> 3) * 64];
        for (int X = 0; X < WS; ++X) {
            const int x = X / s;
            const std::uint16_t data = cells[x >> 3];
            const std::uint8_t pen = g.character(data & 0x3ffu)[(y & 7) * 8 + (x & 7)];
            if (pen) out[X] = static_cast<std::uint16_t>(64 + ((data >> 13) & 7) * 4 + pen);
        }
    }
}

struct Stats {
    std::uint64_t frames = 0, interpolated = 0, objects = 0, matched = 0, cuts = 0, scrolling = 0;
};

// The scroll between a and b (b - a), wrapped to the shortest way round; a
// jump of more than 'cut' is a cut, not a move.
std::optional<int> delta(int a, int b, int period, int cut) {
    int d = ((b - a) % period + period) % period;
    if (d > period / 2) d -= period;
    if (std::abs(d) > cut) return std::nullopt;
    return d;
}

// Frame a + t (0 <= t < 1) between a and b, drawn at scale s.
void tween(const sys2::Graphics& g, const FrameState& a, const FrameState& b, double t, int s, std::vector<std::uint16_t>& pens,
           Stats& stats) {
    const bool near_a = t < 0.5;
    const FrameState& n = near_a ? a : b;
    const FrameState& f = near_a ? b : a;
    const bool same_bands = a.bands.size() == b.bands.size() &&
                            std::ranges::equal(a.bands, b.bands, [](const Band& x, const Band& y) { return x.first == y.first && x.last == y.last; });
    if (t > 0) {
        ++stats.frames;
        stats.interpolated += same_bands;
    }
    for (std::size_t i = 0; i < n.bands.size(); ++i) {
        const Band& nb = n.bands[i];
        Drawn d;
        d.band = &nb;
        d.sx = nb.sx;
        d.sy = nb.sy;
        for (const auto& o : nb.objects) d.objects.push_back({o, double(o.x), double(o.y)});
        if (same_bands && t > 0) {
            const Band& fb = f.bands[i];
            const double k = near_a ? t : 1.0 - t;   // how far from the nearer frame to the farther one
            const auto dx = delta(nb.sx, fb.sx, 1024, 64), dy = delta(nb.sy, fb.sy, 512, 64);
            if (dx && dy) {
                stats.scrolling += (*dx != 0 || *dy != 0) && i == 0;
                d.sx += *dx * k;
                d.sy += *dy * k;
            } else {
                ++stats.cuts;
            }
            // Objects: the same sprite (code, colour, height, flip) nearest
            // within 32 pixels, each used once; else the same colour and
            // height within 16 (a car turning shows another code); else it stays.
            std::vector<bool> used(fb.objects.size());
            for (auto& p : d.objects) {
                ++stats.objects;
                int best = -1;
                double best_d = 1e9;
                for (int pass = 0; pass < 2 && best < 0; ++pass) {
                    for (std::size_t j = 0; j < fb.objects.size(); ++j) {
                        const Object& o = fb.objects[j];
                        if (used[j] || o.height != p.object.height || o.color != p.object.color) continue;
                        if (pass == 0 && (o.code != p.object.code || o.hflip != p.object.hflip)) continue;
                        const double dist = std::hypot(o.x - p.object.x, o.y - p.object.y);
                        if (dist <= (pass == 0 ? 32.0 : 16.0) && dist < best_d) {
                            best_d = dist;
                            best = static_cast<int>(j);
                        }
                    }
                }
                if (best >= 0) {
                    used[static_cast<std::size_t>(best)] = true;
                    ++stats.matched;
                    p.x += (fb.objects[static_cast<std::size_t>(best)].x - p.object.x) * k;
                    p.y += (fb.objects[static_cast<std::size_t>(best)].y - p.object.y) * k;
                }
            }
        }
        draw(g, d, nb.first, nb.last, s, pens);
    }
}

// RGBA through the palette, rotated as the cabinet shows it (ROT270), at scale s.
void to_rgba(const std::vector<std::uint16_t>& pens, const sys2::PaletteRam& palette, int s, std::uint8_t* out, int out_width, int x0) {
    const int WS = W * s, HS = H * s;
    for (int Y = 0; Y < HS; ++Y)
        for (int X = 0; X < WS; ++X) {
            const auto c = palette.color(static_cast<std::uint8_t>(pens[static_cast<std::size_t>(Y) * WS + X]));
            std::uint8_t* p = out + (static_cast<std::size_t>(WS - 1 - X) * out_width + x0 + Y) * 4;
            p[0] = c.red;
            p[1] = c.green;
            p[2] = c.blue;
            p[3] = 255;
        }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::print(stderr, "usage: qilin_tween <rom-dir> --from F --to G [--factor N] [--scale S] [--out FILE.mp4] [script options]\n");
        return 2;
    }
    if (argc % 2 != 0) {
        std::fprintf(stderr, "Each option needs a value\n"); return 2;
    }
    std::uint64_t from = 0, to = 0;
    int factor = 4, scale = 2;
    std::string out_path;
    tools::Script script;
    for (int i = 2; i + 1 < argc; i += 2) {
        const std::string_view flag = argv[i], value = argv[i + 1];
        std::uint64_t number = 0;
        std::from_chars(value.data(), value.data() + value.size(), number);
        if (flag == "--from") from = number;
        else if (flag == "--to") to = number;
        else if (flag == "--factor") factor = static_cast<int>(number);
        else if (flag == "--scale") scale = static_cast<int>(number);
        else if (flag == "--out") out_path = value;
        else if (!script.option(flag, value)) {
            std::print(stderr, "qilin_tween: unknown option {}\n", flag);
            return 2;
        }
    }
    if (to <= from || factor < 1 || factor > 8 || scale < 1 || scale > 8) {
        std::print(stderr, "qilin_tween: --to must be after --from; --factor and --scale 1..8\n");
        return 2;
    }
    if (!script.error().empty()) {
        std::fprintf(stderr, "%s\n", script.error().c_str()); return 2;
    }
    const auto set = rom::RomSet::load(argv[1]);
    if (!set) {
        std::print(stderr, "qilin_tween: {}\n", set.error());
        return 1;
    }
    sys2::Machine machine(set->region(rom::Region::main_cpu), set->region(rom::Region::sound_cpu));
    const sys2::Graphics graphics(*set);
    sys2::Video video(graphics);
    Recorder recorder(video);
    machine.video_sink = &recorder;
    script.attach(machine);
    game::Scheduler scheduler(machine);
    scheduler.on_frame = [&](std::uint64_t frame) { recorder.recording = frame + 1 >= from && frame + 1 <= to; };
    scheduler.run(to + 1);
    std::print("Recorded {} frames, {} bands.\n", recorder.frames.size(),
               [&] { std::size_t n = 0; for (const auto& f : recorder.frames) n += f.bands.size(); return n; }());

    // Self-check: t = 0 at scale 1 is the reference frame.
    std::vector<std::uint16_t> pens(static_cast<std::size_t>(W) * H);
    Stats unused;
    std::size_t differing = 0;
    for (std::size_t i = 0; i < recorder.frames.size(); ++i) {
        tween(graphics, recorder.frames[i], recorder.frames[i], 0.0, 1, pens, unused);
        for (std::size_t p = 0; p < pens.size(); ++p) differing += (pens[p] & 0xff) != (recorder.references[i].pens[p] & 0xff);
    }
    std::print("Self-check: {} pixels differ from the reference renderer over {} frames.\n", differing, recorder.frames.size());
    if (out_path.empty()) return differing != 0;

    const int fw = H * scale, fh = W * scale;   // one picture, upright
    const int vw = fw * 2 + 8, vh = fh;
    const std::string command = std::format("ffmpeg -loglevel error -y -f rawvideo -pix_fmt rgba -s {}x{} -r {} -i - -c:v libx264 -preset veryfast "
                                            "-crf 16 -pix_fmt yuv420p \"{}\"",
                                            vw, vh, 60.1 * factor, out_path);
    std::FILE* pipe = popen(command.c_str(), "w");
    if (!pipe) {
        std::print(stderr, "qilin_tween: can't run ffmpeg\n");
        return 1;
    }
    std::vector<std::uint8_t> picture(static_cast<std::size_t>(vw) * vh * 4, 0);
    std::vector<std::uint16_t> left(static_cast<std::size_t>(W) * scale * H * scale), right(left.size());
    Stats stats;
    for (std::size_t i = 0; i + 1 < recorder.frames.size(); ++i) {
        const auto& a = recorder.frames[i];
        const auto& b = recorder.frames[i + 1];
        tween(graphics, a, a, 0.0, scale, left, unused);
        to_rgba(left, a.palette, scale, picture.data(), vw, 0);
        for (int k = 0; k < factor; ++k) {
            const double t = double(k) / factor;
            tween(graphics, a, b, t, scale, right, stats);
            to_rgba(right, t < 0.5 ? a.palette : b.palette, scale, picture.data(), vw, fw + 8);
            std::fwrite(picture.data(), 1, picture.size(), pipe);
        }
    }
    const int status = pclose(pipe);
    std::print("In-between frames: {}; of them {} interpolated (the same bands in both frames), {} scrolling, {} scroll cuts.\n"
               "Objects: {} placed, {} matched to the next frame.\n",
               stats.frames, stats.interpolated, stats.scrolling, stats.cuts, stats.objects, stats.matched);
    std::print("Video: {} ({} x {}, {:.1f} frames a second; left as the game shows it, right with in-between frames). ffmpeg: {}.\n",
               out_path, vw, vh, 60.1 * factor, status);
    return status != 0;
}
