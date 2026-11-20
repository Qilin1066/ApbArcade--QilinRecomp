#include "moza.hpp"

#include <cmath>
#include <cstring>

namespace qilin::app {

bool MozaAngle::open(const std::string& path) {
    close();
#ifdef SDL_PLATFORM_WINDOWS
    // Verified exports from MOZA's official MSVC2022-64 SDK 1.0.1.8. On
    // Win64 the calling convention for these pointer/int-only functions is
    // shared by MSVC and LLVM-MinGW (used by the player download).
    module_ = SDL_LoadObject(path.c_str());
    if (!module_) { status_ = std::string("MOZA SDK could not load: ") + SDL_GetError(); return false; }
    const auto install = reinterpret_cast<Lifecycle>(SDL_LoadFunction(module_, "?installMozaSDK@moza@@YAXXZ"));
    remove_ = reinterpret_cast<Lifecycle>(SDL_LoadFunction(module_, "?removeMozaSDK@moza@@YAXXZ"));
    read_ = reinterpret_cast<Read>(SDL_LoadFunction(module_, "?getHIDData@moza@@YAPEBUHIDData@@AEAW4ERRORCODE@@@Z"));
    if (!install || !remove_ || !read_) {
        // No lifecycle function has been called, so do not call remove.
        remove_ = nullptr;
        close();
        status_ = "Unsupported MOZA DLL: use MSVC2022-64 SDK 1.0.1.8";
        return false;
    }
    install();
    status_ = "Waiting for MOZA Pit House and wheel angle";
    return true;
#else
    (void)path;
    status_ = "MOZA's physical-angle SDK requires 64-bit Windows";
    return false;
#endif
}

void MozaAngle::close() {
    if (module_) {
        if (remove_) remove_();
        SDL_UnloadObject(module_);
    }
    module_ = nullptr;
    remove_ = nullptr;
    read_ = nullptr;
}

std::optional<double> MozaAngle::read() {
    if (!read_) return std::nullopt;
    int error = 0;
    const void* data = read_(error);
    // SDK code 5 is COLLECTIONCYCLEDATALOSS. With a valid absolute angle,
    // lost intermediate samples do not lose travel; a 60 Hz game need not
    // consume each high-rate device packet. Other errors invalidate it.
    if ((error != 0 && error != 5) || !data) {
        status_ = "MOZA angle unavailable (SDK error " + std::to_string(error) + "); check Pit House and the wheel";
        return std::nullopt;
    }
    // HIDData's first member is fSteeringWheelAngle (float, degrees).
    // Copy the prefix only: no dependency on the rest of its C++ layout.
    float degrees = 0;
    std::memcpy(&degrees, data, sizeof degrees);
    if (!std::isfinite(degrees)) {
        status_ = "MOZA has not supplied a physical wheel angle yet";
        return std::nullopt;
    }
    status_ = error == 5 ? "MOZA physical angle read (SDK missed intermediate samples)" : "MOZA physical angle is being read";
    return degrees;
}

} // namespace qilin::app
