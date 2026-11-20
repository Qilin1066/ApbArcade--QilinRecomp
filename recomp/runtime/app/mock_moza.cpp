// A test DLL exposing only the pointer/int Win64 ABI used by MozaAngle.
// No vendor code, dependencies or wheel hardware.
namespace {
float angle = 0;
int error = 0, installs = 0, removes = 0;
}
extern "C" {
void mock_install() { ++installs; }
void mock_remove() { ++removes; }
const void* mock_read(int& result) { result = error; return error && error != 5 ? nullptr : &angle; }
void mock_set_angle(float value) { angle = value; }
void mock_set_error(int value) { error = value; }
int mock_installs() { return installs; }
int mock_removes() { return removes; }
}
