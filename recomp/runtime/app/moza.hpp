#pragma once

#include <SDL3/SDL.h>
#include <optional>
#include <string>

namespace qilin::app {

// Optional read-only physical-angle input for a MOZA wheel whose ordinary
// joystick axis clips. Loads the user's SDK DLL, never bundles it or alters
// Pit House settings. The narrow Win64 ABI uses no STL objects or ownership
// across the DLL boundary. See docs/wheels.md for the supported SDK version.
class MozaAngle {
public:
    ~MozaAngle() { close(); }
    MozaAngle() = default;
    MozaAngle(const MozaAngle&) = delete;
    MozaAngle& operator=(const MozaAngle&) = delete;
    bool open(const std::string& path);
    void close();
    std::optional<double> read();
    [[nodiscard]] const std::string& status() const { return status_; }
    [[nodiscard]] bool is_open() const { return module_ != nullptr; }
private:
    using Lifecycle = void (*)();
    using Read = const void* (*)(int&);
    SDL_SharedObject* module_ = nullptr;
    Lifecycle remove_ = nullptr;
    Read read_ = nullptr;
    std::string status_ = "MOZA physical-angle input is off";
};

} // namespace qilin::app
