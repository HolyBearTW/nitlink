# Contributing to NitLink

NitLink is a small C++17 project with one maintainer and a bench of Elgato cards. Contributions are welcome, and the notes below are what keeps them mergeable.

## Before starting

- Open an issue first for anything larger than a bug fix, so the direction is agreed before the code exists.
- NitLink is a viewer. It plays a console on a PC screen with the lowest latency a capture path allows and with real HDR10. Recording, streaming, and editing are out of scope; other tools do those well.
- Read the README's Scope and Architecture sections. They explain why the present path looks the way it does.

## Building

Windows 10 or 11, CMake, and Visual Studio 2022 or later with the Desktop C++ workload.

```
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Or open `CMakeLists.txt` in Visual Studio, pick the `x64-Release` configuration, and build. `package.bat` assembles a release zip from the build output.

## Pull requests

- One change per pull request. The description states the symptom, the cause, and what was measured or tested.
- Name the card and the Windows build the change was tested on. Paths that could not be tested are fine to submit when they are labeled as such.
- Follow the existing conventions: comments explain the reason in plain third person, no TODO markers, no first person, ASCII hyphens rather than dashes.
- Every HRESULT is checked. Nothing on the render loop may block on the GPU.
- Contributions are accepted under the repository's MIT license.

## Test hardware

The maintainer tests on an Elgato 4K Pro. The 4K S and 4K X paths depend on people who own those cards, so saying which one you have, in an issue or a pull request, is the single most useful thing a contributor can add.
