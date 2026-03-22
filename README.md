# JsonLens

A fast, lightweight, and resilient JSON viewer and editor built with GTK4 and Vala.

## Features

* **Three Interchangeable Views**: 
  * **Text View**: A full-featured code editor utilizing GtkSourceView with syntax highlighting, bracket matching, code folding, and line numbers.
  * **Column View**: Navigate deeply nested JSON objects and arrays using cascading lists (Miller columns).
  * **Graph View**: A node-based visual representation of your JSON tree.
* **Blazing Fast Parsing**: Powered by a custom C-based Arena allocator that parses JSON in background threads, keeping the UI perfectly smooth even when opening massive files.
* **Resilient to Errors**: Gracefully handles invalid JSON by highlighting exact syntax errors (Line and Column) directly in the editor.
* **Format & Minify**: Built-in actions to quickly prettify or aggressively minify your JSON payloads.
* **Permissive Mode**: Optional support for allowing comments within JSON files.
* **Cross-Platform**: Automated GitHub Actions CI/CD to natively build executables for Linux, Windows, and macOS.

## Dependencies

To build JsonLens from source, you will need the following development packages:
* `valac` (Vala Compiler)
* `meson` and `ninja-build` (Build System)
* `pkg-config`
* `gtk4`
* `gtksourceview-5`

### Ubuntu / Debian Linux
```bash
sudo apt update
sudo apt install valac meson ninja-build pkg-config libgtk-4-dev libgtksourceview-5-dev
```

### macOS (Homebrew)
```bash
brew install vala meson ninja pkg-config gtk4 gtksourceview5
```

### Windows (MSYS2 UCRT64)
```bash
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-meson mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-vala mingw-w64-ucrt-x86_64-pkgconf mingw-w64-ucrt-x86_64-gtk4 mingw-w64-ucrt-x86_64-gtksourceview5
```

## Building

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