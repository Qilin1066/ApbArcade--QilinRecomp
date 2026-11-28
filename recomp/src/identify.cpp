// `qilin identify` (identify.hpp).
#include "identify.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <system_error>
#include <vector>

namespace qilin::rom {

namespace fs = std::filesystem;

namespace {

// The other sets of APB in MAME (atarisy2.cpp, ROM_START( apb1 ) to
// ROM_START( apbf )): each one's own program ROMs, and the rev 7 files it
// has none of. Every other file is the same in all of them.
constexpr std::array<Known, 2> apb6_files{{
    {"136051-6128.6f", 0x010000, "1bc5c3130ad5c9eae40646db25a038f93a802822"},
    {"136051-6129.6n", 0x010000, "eb53d7272dd5f18f8fa3e484f957b68901009973"},
}};
constexpr std::array<std::string_view, 2> apb6_instead{"136051-7128.6f", "136051-7129.6n"};
constexpr std::array<Known, 2> apb5_files{{
    {"136051-5128.6f", 0x010000, "89b203c03501a8762b24baa7dc911eaa76e259b3"},
    {"136051-5129.6n", 0x010000, "9712efc5a19b8bef6fc5f8befce284e63537a3ce"},
}};
constexpr std::array<std::string_view, 2> apb5_instead{"136051-7128.6f", "136051-7129.6n"};
constexpr std::array<Known, 2> apb4_files{{
    {"136051-4128.6f", 0x010000, "344cbb6cc5c1ad5c1aec26ca12cafaf73305d801"},
    {"136051-4129.6n", 0x010000, "16d705e55a373d6e88792881ffa01e084faf58a9"},
}};
constexpr std::array<std::string_view, 2> apb4_instead{"136051-7128.6f", "136051-7129.6n"};
constexpr std::array<Known, 5> apb3_files{{
    {"136051-3128.6f", 0x010000, "2ae94264122d20903e760225468929396a7c855c"},
    {"136051-3129.6n", 0x010000, "1df1d6d3af9a8ef46ea22a0a22748ebb248361f0"},
    {"136051-1134.2a", 0x004000, "20d51300382543f09e47bee7273b9074e5a4618a"},
    {"136051-1135.2bc", 0x004000, "2b2d9638e012d326777e2e730e28cbacea6d9a72"},
    {"136051-1136.2d", 0x004000, "cfda3a191a5f7ee4157f9d226bcf3dd601cabee1"},
}};
constexpr std::array<std::string_view, 5> apb3_instead{"136051-7128.6f", "136051-7129.6n", "136051-5134.2a", "136051-5135.2bc",
                                                       "136051-5136.2d"};
constexpr std::array<Known, 5> apb2_files{{
    {"136051-2128.6f", 0x010000, "d4b964e4b1a801c9b4ed984d8b20a988d3a1f485"},
    {"136051-2129.6n", 0x010000, "3b73f4567d5b8430766df01f7ac65f1f1fb5dc1a"},
    {"136051-1134.2a", 0x004000, "20d51300382543f09e47bee7273b9074e5a4618a"},
    {"136051-1135.2bc", 0x004000, "2b2d9638e012d326777e2e730e28cbacea6d9a72"},
    {"136051-1136.2d", 0x004000, "cfda3a191a5f7ee4157f9d226bcf3dd601cabee1"},
}};
constexpr std::array<std::string_view, 5> apb2_instead{"136051-7128.6f", "136051-7129.6n", "136051-5134.2a", "136051-5135.2bc",
                                                       "136051-5136.2d"};
constexpr std::array<Known, 7> apb1_files{{
    {"136051-1126.7l", 0x004000, "647ceccc4b434fca5a58e64193a369defc3d9c49"},
    {"136051-1127.7n", 0x004000, "e23885e66ce2ad437db6e02313d2db11f3498bba"},
    {"136051-1128.6f", 0x010000, "96dcbd034f431dfad424331e0854a0c418988055"},
    {"136051-1129.6n", 0x010000, "74f06b548bdf11ce24b105e95f1ad63ece61e17f"},
    {"136051-1134.2a", 0x004000, "20d51300382543f09e47bee7273b9074e5a4618a"},
    {"136051-1135.2bc", 0x004000, "2b2d9638e012d326777e2e730e28cbacea6d9a72"},
    {"136051-1136.2d", 0x004000, "cfda3a191a5f7ee4157f9d226bcf3dd601cabee1"},
}};
constexpr std::array<std::string_view, 7> apb1_instead{"136051-2126.7l", "136051-2127.7n", "136051-7128.6f", "136051-7129.6n",
                                                       "136051-5134.2a", "136051-5135.2bc", "136051-5136.2d"};
constexpr std::array<Known, 5> apbg_files{{
    {"136051-1228.6f", 0x010000, "46659edbbbe664a22d3dbef63a62757d87403365"},
    {"136051-1229.6n", 0x010000, "3f34b2a1fa738fcbc45ac7873f98c8a0f34832d3"},
    {"136051-4134.2a", 0x004000, "d58f5e1095fd6a7c0253fcc9f0e55812c1677387"},
    {"136051-4135.2bc", 0x004000, "86461da310b334f6b32c0e079de6852792284cc6"},
    {"136051-4136.2d", 0x004000, "76446b09bf7cacd713ab88d58793460c9d1a8b9b"},
}};
constexpr std::array<std::string_view, 5> apbg_instead{"136051-7128.6f", "136051-7129.6n", "136051-5134.2a", "136051-5135.2bc",
                                                       "136051-5136.2d"};
constexpr std::array<Known, 2> apbf_files{{
    {"136051-1628.6f", 0x010000, "48376a51210f5967b355ea2ed83e967ac28de455"},
    {"136051-1629.6n", 0x010000, "d5aaf7543adba2298f1faf25e57e144829e93b5d"},
}};
constexpr std::array<std::string_view, 2> apbf_instead{"136051-7128.6f", "136051-7129.6n"};

// Newest first: where a folder holds several (a merged set), the first is named.
constexpr std::array<OtherSet, 8> others{{
    {"apb6", "APB - All Points Bulletin (rev 6)", "6", apb6_files, apb6_instead},
    {"apb5", "APB - All Points Bulletin (rev 5)", "5", apb5_files, apb5_instead},
    {"apb4", "APB - All Points Bulletin (rev 4)", "4", apb4_files, apb4_instead},
    {"apb3", "APB - All Points Bulletin (rev 3)", "3", apb3_files, apb3_instead},
    {"apb2", "APB - All Points Bulletin (rev 2)", "2", apb2_files, apb2_instead},
    {"apb1", "APB - All Points Bulletin (rev 1)", "1", apb1_files, apb1_instead},
    {"apbg", "APB - All Points Bulletin (German)", "German", apbg_files, apbg_instead},
    {"apbf", "APB - All Points Bulletin (French)", "French", apbf_files, apbf_instead},
}};

constexpr std::string_view kNeeded = "This download needs APB's rev 7 ROM set, MAME's apb: its 36 files, unzipped into a folder.";

Identity answer(int code, std::string message) {
    Identity i;
    i.code = code;
    i.ok = code == 0;
    i.message = std::move(message);
    return i;
}

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

// A file's bytes if it's a regular file of exactly `size` bytes.
std::optional<std::vector<std::uint8_t>> read_sized(const fs::path& path, std::uint64_t size) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || fs::file_size(path, ec) != size || ec) return std::nullopt;
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() != size) return std::nullopt;
    return bytes;
}

bool matches(const fs::path& dir, const Known& k) {
    const auto bytes = read_sized(dir / utf8_path(k.name), k.size);
    return bytes && sha1(*bytes) == k.sha1;
}

// "a, b and c", at most `most` names ("a, b, c and 4 more").
std::string name_list(const std::vector<std::string_view>& names, std::size_t most = 6) {
    std::string out;
    const std::size_t shown = names.size() > most ? most - 1 : names.size();
    for (std::size_t i = 0; i < shown; ++i) {
        if (i > 0) out += i + 1 == names.size() ? " and " : ", ";
        out += names[i];
    }
    if (shown < names.size()) out += std::format(" and {} more", names.size() - shown);
    return out;
}

std::string json_string(std::string_view s) {
    std::string out = "\"";
    for (const char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else if (u < 0x20) {
            out += std::format("\\u{:04x}", u);
        } else {
            out += c;  // UTF-8 as it is
        }
    }
    return out + "\"";
}

} // namespace

std::span<const OtherSet> other_sets() noexcept {
    return others;
}

Identity identify(const fs::path& input, std::span<const File> files, std::span<const OtherSet> other) {
    const std::string shown = utf8_text(input);
    std::error_code ec;
    const fs::file_status status = fs::status(input, ec);
    if (ec || !fs::exists(status)) return answer(2, std::format("Can't find {}.", shown));
    if (!fs::is_directory(status)) {
        if (lower(utf8_text(input.extension())) == ".zip")
            return answer(2, std::format("{} is a .zip file. Unzip it into a folder of its own, then choose that folder. {}",
                                         utf8_text(input.filename()), kNeeded));
        return answer(2, std::format("{} is a file, not a folder. {}", utf8_text(input.filename()), kNeeded));
    }

    // The supported set, file by file.
    const auto checks = verify(input, files);
    std::vector<std::string_view> missing, damaged;
    std::size_t good = 0;
    for (const auto& c : checks) {
        if (c.status == FileCheck::Status::ok) ++good;
        else if (c.status == FileCheck::Status::missing) missing.push_back(c.file->name);
        else damaged.push_back(c.file->name);
    }
    if (good == checks.size()) {
        Identity i = answer(0, std::format("This is {}, MAME's {} set: the right ROM set.", rev7_title, rev7_set));
        i.title = std::string(rev7_title);
        i.id = std::string(rev7_set);
        i.version = std::string(rev7_version);
        return i;
    }
    const auto good_here = [&](std::string_view name) {
        return std::ranges::any_of(checks, [&](const FileCheck& c) { return c.file->name == name && c.status == FileCheck::Status::ok; });
    };

    // Another set of APB: all of its own program ROMs here, and not every
    // rev 7 file it replaces (else it's rev 7 with something else beside it).
    for (const OtherSet& o : other) {
        if (o.files.empty() || !std::ranges::all_of(o.files, [&](const Known& k) { return matches(input, k); })) continue;
        if (!o.instead.empty() && std::ranges::all_of(o.instead, good_here)) continue;
        Identity i = answer(3, std::format("This is {}, MAME's {} set. This download is for {}, MAME's {}: those ROMs, not these.",
                                           o.title, o.set, rev7_title, rev7_set));
        i.title = std::string(o.title);
        i.id = std::string(o.set);
        i.version = std::string(o.version);
        return i;
    }

    if (good == 0 && damaged.empty()) {
        // None of APB's files: what is there instead?
        bool zip = false, apb_zip = false, any = false, apb_folder = false;
        for (fs::directory_iterator it(input, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code type;
            const std::string name = lower(utf8_text(it->path().filename()));
            if (it->is_directory(type)) {
                apb_folder = apb_folder || name == rev7_set;
                continue;
            }
            if (!it->is_regular_file(type)) continue;
            any = true;
            if (name.ends_with(".zip")) {
                zip = true;
                apb_zip = apb_zip || name == std::string(rev7_set) + ".zip";
            }
        }
        if (ec) return answer(2, std::format("Can't read the folder {}: {}.", shown, ec.message()));
        if (apb_zip)
            return answer(2, "This folder holds apb.zip, MAME's zipped ROM set. Unzip it into a folder of its own, then choose that folder.");
        if (apb_folder) return answer(2, "This folder holds no ROM files, but a folder called apb: choose that one.");
        if (zip)
            return answer(2, std::format("This folder holds .zip files, not ROM files. {} If you have MAME's apb.zip, unzip it into a folder of its own and choose that.",
                                         kNeeded));
        if (!any) return answer(2, std::format("This folder is empty. {}", kNeeded));
        Identity i = answer(3, std::format("This folder holds no APB ROMs: none of the {} files of MAME's apb set (136051-1101.5t and the "
                                           "rest) is in it. It may be another game's ROM set. {}",
                                           checks.size(), kNeeded));
        return i;
    }

    // Part of the rev 7 set, the rest missing or not as MAME lists it.
    std::string message = "This is APB's ROM set, but not all of it:";
    if (!missing.empty())
        message += std::format(" {} of its {} files {} missing ({}).", missing.size(), checks.size(), missing.size() == 1 ? "is" : "are",
                               name_list(missing));
    if (!damaged.empty())
        message += std::format(" {} {} the wrong size or contents ({}): a bad dump, or from another set.", damaged.size(),
                               damaged.size() == 1 ? "file has" : "files have", name_list(damaged));
    message += std::format(" {}", kNeeded);
    Identity i = answer(2, std::move(message));
    i.title = "APB - All Points Bulletin";
    i.id = std::string(rev7_set);
    return i;
}

std::string to_json(const Identity& i) {
    return std::string("{\"ok\": ") + (i.ok ? "true" : "false") + ", \"title\": " + json_string(i.title) + ", \"id\": " +
           json_string(i.id) + ", \"version\": " + json_string(i.version) + ", \"message\": " + json_string(i.message) + "}\n";
}

} // namespace qilin::rom
