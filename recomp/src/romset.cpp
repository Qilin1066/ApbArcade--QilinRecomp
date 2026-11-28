#include "romset.hpp"

#include <bit>
#include <format>
#include <fstream>
#include <iterator>

namespace qilin::rom {
namespace {

constexpr std::array<RegionInfo, region_count> regions{{
    {"maincpu", 0x90000, false},
    {"audiocpu", 0x10000, false},
    {"tiles", 0x80000, false},
    {"sprites", 0x100000, true},
    {"chars", 0x4000, false},
}};

using enum Region;

// ROM_START( apb ) in MAME's atarisy2.cpp.
constexpr std::array<File, 36> files{{
    // T-11 program: 16 KiB fixed at 0x8000, banks from 0x10000 (9 x 64 KiB region)
    {"136051-2126.7l", 0x4000, "48ee833c84808abe0eb8b7932abf082af09066da", main_cpu, true, {{{0x008000, 0x4000}, {}}}},
    {"136051-2127.7n", 0x4000, "bded098d56d68a95333eb68bc0c9629a2257aa16", main_cpu, true, {{{0x008001, 0x4000}, {}}}},
    {"136051-7128.6f", 0x10000, "91762807270b6294f676efb40a2a7deb3732f358", main_cpu, true, {{{0x010000, 0x10000}, {}}}},
    {"136051-7129.6n", 0x10000, "c5a3bf4e3da221ebf05b7175f975d9564f0e8c32", main_cpu, true, {{{0x010001, 0x10000}, {}}}},
    {"136051-1130.6j", 0x10000, "a0a7a0629b228ffd2a0c92725305b8d094484ddb", main_cpu, true, {{{0x030000, 0x10000}, {}}}},
    {"136051-1131.6p", 0x10000, "98ff0de15632397e7371d8473b0174251b1ef9a1", main_cpu, true, {{{0x030001, 0x10000}, {}}}},
    {"136051-1132.6l", 0x10000, "75aae74571c50d36639d0ae69b0614e5aedeb6e3", main_cpu, true, {{{0x070000, 0x10000}, {}}}},
    {"136051-1133.6s", 0x10000, "432720afd4179d3df871226e0eb576d2ffde44c1", main_cpu, true, {{{0x070001, 0x10000}, {}}}},
    // 6502 sound program
    {"136051-5134.2a", 0x4000, "bbdbbb9a4903f98842d9a697410a2e3a2069284c", sound_cpu, false, {{{0x4000, 0x4000}, {}}}},
    {"136051-5135.2bc", 0x4000, "b1f1f0d1bda445a53de798fb6847c605afe53e3c", sound_cpu, false, {{{0x8000, 0x4000}, {}}}},
    {"136051-5136.2d", 0x4000, "4cea39c0d8551ce7193e51de341f7297a94b8d9b", sound_cpu, false, {{{0xc000, 0x4000}, {}}}},
    // Playfield tiles
    {"136051-1118.6a", 0x8000, "6b4ed2defab85ab4d4943bfdf43c04dd42cc2ec5", tiles, false, {{{0x000000, 0x8000}, {}}}},
    {"136051-1120.6bc", 0x10000, "8920c8aab37d7b0ecfa17381c65cc00b9b99d4da", tiles, false, {{{0x028000, 0x8000}, {0x008000, 0x8000}}}},
    {"136051-1122.7a", 0x10000, "82a0eee49cdfe08aeb8619bc7b83c915a8de95c7", tiles, false, {{{0x030000, 0x8000}, {0x010000, 0x8000}}}},
    {"136051-1124.7bc", 0x10000, "5c6201743d973389f7ed13ec4253c2034ab23c62", tiles, false, {{{0x038000, 0x8000}, {0x018000, 0x8000}}}},
    {"136051-1117.6cd", 0x8000, "7fed2a40c2ba28444c5f5470d0b7c86198469651", tiles, false, {{{0x040000, 0x8000}, {}}}},
    {"136051-1119.6de", 0x10000, "de899a0eb0b9fdbc96ccd3987d1dc942b54f2cc6", tiles, false, {{{0x068000, 0x8000}, {0x048000, 0x8000}}}},
    {"136051-1121.7de", 0x10000, "3a2f8da5c4cd7693575ad13246c6dbb6e2f02131", tiles, false, {{{0x070000, 0x8000}, {0x050000, 0x8000}}}},
    {"136051-1123.7cd", 0x10000, "a90a0c14ee5d5cdf60a1d3ecd9984b74c31b9f36", tiles, false, {{{0x078000, 0x8000}, {0x058000, 0x8000}}}},
    // Motion objects (the region is inverted after loading)
    {"136051-1105.6t", 0x10000, "d7dff7a72041ebb7ff4f56da74cc7eb4b71bc5cc", sprites, false, {{{0x020000, 0x8000}, {0x000000, 0x8000}}}},
    {"136051-1106.6rs", 0x10000, "35b276eb1ad1ce8b143485502430335c08aa9474", sprites, false, {{{0x028000, 0x8000}, {0x008000, 0x8000}}}},
    {"136051-1107.6pr", 0x10000, "f78769962b4d6f6b4eaac5b6ae8e08ff90ad55ac", sprites, false, {{{0x030000, 0x8000}, {0x010000, 0x8000}}}},
    {"136051-1108.6n", 0x10000, "c1cdc428058d5a6a9706431d4aa3d2ae81815d9c", sprites, false, {{{0x038000, 0x8000}, {0x018000, 0x8000}}}},
    {"136051-1113.6m", 0x10000, "0aa995bc1234c4b84b72c2e779968b1052cfa6e5", sprites, false, {{{0x060000, 0x8000}, {0x040000, 0x8000}}}},
    {"136051-1114.6kl", 0x10000, "ef3378a2569ccf34360edf6ea1630bed2b83c438", sprites, false, {{{0x068000, 0x8000}, {0x048000, 0x8000}}}},
    {"136051-1115.6jk", 0x10000, "faaa088b43f354ad5b5bcf96ed9eab54a2ad162e", sprites, false, {{{0x070000, 0x8000}, {0x050000, 0x8000}}}},
    {"136051-1116.6h", 0x10000, "4a5994898d546fa914d5eb6e5a1a9c4c2febea5a", sprites, false, {{{0x078000, 0x8000}, {0x058000, 0x8000}}}},
    {"136051-1101.5t", 0x10000, "ee9c1088f203c607b10c9e7927c8c4a29d727726", sprites, false, {{{0x0a0000, 0x8000}, {0x080000, 0x8000}}}},
    {"136051-1102.5rs", 0x10000, "a0c3a90fd400d146b7b9cc12a8dfaa5db0e4426a", sprites, false, {{{0x0a8000, 0x8000}, {0x088000, 0x8000}}}},
    {"136051-1103.5pr", 0x10000, "28473949570d402754737c6d38de2a096f804676", sprites, false, {{{0x0b0000, 0x8000}, {0x090000, 0x8000}}}},
    {"136051-1104.5n", 0x10000, "87e0eb5910ec7042628378bcfd58d9b7beca690d", sprites, false, {{{0x0b8000, 0x8000}, {0x098000, 0x8000}}}},
    {"136051-1109.5m", 0x10000, "cb05fa401456177170c32958dbcfec95667944cd", sprites, false, {{{0x0e0000, 0x8000}, {0x0c0000, 0x8000}}}},
    {"136051-1110.5kl", 0x10000, "475b5c3224ee2e5b5330cc2b4ad018b6bcf0abd5", sprites, false, {{{0x0e8000, 0x8000}, {0x0c8000, 0x8000}}}},
    {"136051-1111.5jk", 0x10000, "6464b089395ce52f26a604357a4783fe5c673362", sprites, false, {{{0x0f0000, 0x8000}, {0x0d0000, 0x8000}}}},
    {"136051-1112.5h", 0x10000, "e100ec8a8aee643d3175a8f54cc7e9266cf50e42", sprites, false, {{{0x0f8000, 0x8000}, {0x0d8000, 0x8000}}}},
    // Alphanumerics
    {"136051-1125.4t", 0x4000, "90b96e0645a01939c681a7a5fe5d236f3dfc71b7", chars, false, {{{0x000000, 0x4000}, {}}}},
}};

std::optional<std::vector<std::uint8_t>> read_file(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        return std::nullopt;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return bytes;
}

FileCheck check(const File& file, const std::optional<std::vector<std::uint8_t>>& bytes) {
    if (!bytes) {
        return {&file, FileCheck::Status::missing};
    }
    if (bytes->size() != file.size) {
        return {&file, FileCheck::Status::wrong_size, bytes->size()};
    }
    if (sha1(*bytes) != file.sha1) {
        return {&file, FileCheck::Status::wrong_digest, bytes->size()};
    }
    return {&file, FileCheck::Status::ok, bytes->size()};
}

std::string describe(const FileCheck& c) {
    switch (c.status) {
    case FileCheck::Status::missing: return std::format("{} is missing", c.file->name);
    case FileCheck::Status::wrong_size:
        return std::format("{} has {} bytes, expected {}", c.file->name, c.actual_size, c.file->size);
    case FileCheck::Status::wrong_digest: return std::format("{} has the wrong SHA-1 (another revision or a bad dump)", c.file->name);
    case FileCheck::Status::ok: break;
    }
    return std::format("{} matches", c.file->name);
}

} // namespace

const RegionInfo& region_info(Region region) noexcept {
    return regions[static_cast<std::size_t>(region)];
}

std::span<const File> rev7_files() noexcept {
    return files;
}

std::vector<FileCheck> verify(const std::filesystem::path& dir) {
    return verify(dir, files);
}

std::vector<FileCheck> verify(const std::filesystem::path& dir, std::span<const File> list) {
    std::vector<FileCheck> result;
    for (const auto& file : list) {
        result.push_back(check(file, read_file(dir / file.name)));
    }
    return result;
}

std::expected<RomSet, std::string> RomSet::load(const std::filesystem::path& dir) {
    RomSet set;
    for (std::size_t r = 0; r < region_count; ++r) {
        set.data_[r].assign(regions[r].size, 0);   // MAME zero-fills regions of 4 MB or less
        set.populated_[r].assign(regions[r].size, false);
    }
    std::string problems;
    unsigned matched = 0;
    for (const auto& file : files) {
        const auto bytes = read_file(dir / file.name);
        const auto result = check(file, bytes);
        if (result.status != FileCheck::Status::ok) {
            problems += "\n  " + describe(result);
            continue;
        }
        ++matched;
        const auto r = static_cast<std::size_t>(file.region);
        std::uint32_t from = 0;
        for (const auto& piece : file.pieces) {
            for (std::uint32_t i = 0; i < piece.length; ++i) {
                const auto to = piece.offset + (file.byte_lane ? 2 * i : i);
                set.data_[r][to] = (*bytes)[from + i];
                set.populated_[r][to] = true;
            }
            from += piece.length;
        }
    }
    if (!problems.empty()) {
        return std::unexpected(std::format("the APB rev 7 ROM set in {} doesn't verify ({} of {} files match):{}",
                                           utf8_path(dir), matched, files.size(), problems));
    }
    for (std::size_t r = 0; r < region_count; ++r) {
        if (regions[r].inverted) {
            for (auto& byte : set.data_[r]) {
                byte = static_cast<std::uint8_t>(~byte);
            }
        }
    }
    return set;
}

RomSet RomSet::from_regions(std::array<std::vector<std::uint8_t>, region_count> data,
                            std::array<std::vector<bool>, region_count> populated) {
    RomSet set;
    set.data_ = std::move(data);
    set.populated_ = std::move(populated);
    return set;
}

std::optional<std::uint16_t> RomSet::word(Region r, std::uint32_t offset) const noexcept {
    const auto& d = data_[static_cast<std::size_t>(r)];
    if ((offset & 1) != 0 || offset + 1 >= d.size() || !populated(r, offset) || !populated(r, offset + 1)) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(d[offset] | (d[offset + 1] << 8));
}

// FIPS 180-4 SHA-1.
std::string sha1(std::span<const std::uint8_t> bytes) {
    std::array<std::uint32_t, 5> h{0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    std::vector<std::uint8_t> message(bytes.begin(), bytes.end());
    const std::uint64_t bits = static_cast<std::uint64_t>(bytes.size()) * 8;
    message.push_back(0x80);
    while (message.size() % 64 != 56) {
        message.push_back(0);
    }
    for (int shift = 56; shift >= 0; shift -= 8) {
        message.push_back(static_cast<std::uint8_t>(bits >> shift));
    }
    for (std::size_t block = 0; block < message.size(); block += 64) {
        std::array<std::uint32_t, 80> w{};
        for (int i = 0; i < 16; ++i) {
            const auto* p = &message[block + 4 * static_cast<std::size_t>(i)];
            w[static_cast<std::size_t>(i)] = (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) |
                                             (std::uint32_t{p[2]} << 8) | p[3];
        }
        for (std::size_t i = 16; i < 80; ++i) {
            w[i] = std::rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        auto [a, b, c, d, e] = h;
        for (std::size_t i = 0; i < 80; ++i) {
            std::uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            const auto t = std::rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = std::rotl(b, 30);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    std::string hex;
    for (const auto word : h) {
        hex += std::format("{:08x}", word);
    }
    return hex;
}

} // namespace qilin::rom
