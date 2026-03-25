# JsonLens

A fast, lightweight, and resilient JSON viewer and editor built with Dear ImGui, SDL2, and C++.

## Features

* **Three Interchangeable Views**: 
  * **Text View**: A virtualized text viewer using ImGuiListClipper to instantly render gigabyte-sized files.
  * **Tree View**: Navigate and seamlessly edit deeply nested JSON objects and arrays.
  * **Graph View**: A node-based visual representation of your JSON tree.
* **Blazing Fast Parsing**: Powered by a custom C-based Arena allocator that parses JSON in background threads, keeping the UI perfectly smooth even when opening massive files.
* **Resilient to Errors**: Gracefully handles invalid JSON by highlighting exact syntax errors (Line and Column) directly in the editor.
* **Format & Minify**: Built-in actions to quickly prettify or aggressively minify your JSON payloads.
* **Permissive Mode**: Optional support for allowing comments within JSON files.
* **Cross-Platform**: Automated GitHub Actions CI/CD to natively build executables for Linux, Windows, and macOS.

## Dependencies

To build JsonLens from source, you will need the following development packages:
* `gcc` / `g++` or `clang`
* `meson` and `ninja-build` (Build System)
* `pkg-config`
* `libsdl2-dev`
* `libgl1-mesa-dev` (Linux)

### Ubuntu / Debian Linux
```bash
sudo apt update
sudo apt install build-essential meson ninja-build pkg-config libsdl2-dev libgl1-mesa-dev
```

### macOS (Homebrew)
```bash
brew install gcc meson ninja pkg-config sdl2
```

### Windows (MSYS2 UCRT64)
```bash
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-meson mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-pkgconf mingw-w64-ucrt-x86_64-SDL2
```

## Building

Before building for the first time, you must fetch the Dear ImGui dependency:
```bash
./setup_imgui.sh
```

JsonLens uses the Meson build system. To compile the project, run:

```bash
meson setup builddir
ninja -C builddir
```

## Running

Once compiled, you can launch the application directly from the build directory:

```bash
./builddir/json-lens
```