SugarBot quick start
====================

Full documentation is in README.md.

Requirements:
- CMake 3.16 or newer
- A C++20 compiler
- Windows: Visual Studio 2022 MSVC x64
- Linux/WSL: GCC or Clang on x86-64 or ARM64/aarch64

Linux / WSL:
  cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON -DSUGAR_OUT_ROOT="$PWD/build/artifacts"
  cmake --build build --parallel
  ctest --test-dir build --output-on-failure
  ./build/artifacts/RelWithDebInfo/sugarbot --help

Windows PowerShell:
  cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=ON
  cmake --build build --config RelWithDebInfo --target sugarbot sugar_server sugar_client sugar_contracts
  ctest --test-dir build -C RelWithDebInfo --output-on-failure
  .\build\RelWithDebInfo\sugarbot.exe --help

Prebuilt binaries are architecture-specific. If a Linux binary reports
"Exec format error" on an ARM64 machine, build from source on that machine or
use a linux-arm64 binary.
