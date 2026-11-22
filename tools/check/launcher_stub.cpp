// A real Windows process for checking the launcher's Unicode/space quoting.
// No ROMs, SDL, wheel driver or graphics device are used.
#include "command_line.hpp"
#include <cstring>
#include <filesystem>
#include <fstream>

int main(int argc, char** argv) {
    const qilin::CommandLine command_line(argc, argv);
    argc = command_line.count(); argv = command_line.values();
    auto native = [](const char* text) { const std::string utf8(text); return std::filesystem::path(std::u8string(utf8.begin(), utf8.end())); };
    // Mock lifter/ninja work to exercise rebuild and executable installation.
    if (argc == 3 && std::strcmp(argv[1], "verify") == 0) return 0;
    if (argc >= 5 && std::strcmp(argv[1], "lift") == 0) {
        const auto out = native(argv[argc - 1]);
        std::filesystem::create_directories(out);
        std::ofstream(out / "fixture.cpp") << "// fixture\n";
        return 0;
    }
    if (argc == 3 && std::strcmp(argv[1], "-C") == 0) {
        std::filesystem::copy_file(native(argv[0]), native(argv[2]) / "APB.exe",
                                   std::filesystem::copy_options::overwrite_existing);
        return 0;
    }
    if (argc != 7 || std::strcmp(argv[1], "--record") || std::strcmp(argv[3], "--profile") ||
        std::strcmp(argv[5], "--controller-report")) return 2;
    const auto folder = native(argv[2]).parent_path();
    for (int i = 2; i < argc; i += 2) {
        const auto path = native(argv[i]);
        if (path.parent_path() != folder || !std::filesystem::is_directory(folder) || std::filesystem::exists(path)) return 3;
        std::ofstream out(path);
        out << "launcher stub\n";
        if (!out) return 4;
    }
    return 0;
}
