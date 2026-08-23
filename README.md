# Backwalk

**Backwalk** is a lightweight stack backtracing library written in C. On Unix it walks frame
pointers on x86_64, AArch64, and ARM. On Windows it uses `CaptureStackBackTrace` (x86, x64, ARM64).
It provides a simple callback-based interface for collecting stack traces with symbol resolution.

## Features

- **Unix and Windows**: Frame-pointer walk plus `dladdr()` on Unix; `CaptureStackBackTrace` plus DbgHelp on Windows
- **Symbol resolution**: `dladdr()` on Unix, `SymFromAddr` on Windows (names need `-rdynamic` / PDB)
- **Thread-safe**: Safe for use in multithreaded environments
- **C++ compatible**: Full C++ support with proper linkage

## Building

### Prerequisites

- **CMake** 3.20 or newer
- **GCC** 10+, **Clang**, or **MSVC** (C99 / C++11)
- **Unix**: libdl, and frame pointers (`-fno-omit-frame-pointer`)
- **Windows**: DbgHelp (`dbghelp.lib`); PDB recommended for function names

### Quick Build

```bash
git clone https://github.com/whalbawi/backwalk.git
cd backwalk
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Run All Tests

```bash
cmake --build build
ctest --test-dir build -V
```

Individual test executables are available in the `build/` directory after building.


## Example

```c
#include <stdio.h>
#include <backwalk/backwalk.h>

bool print_frame(uintptr_t addr, const char* fname, const char* sname, void* arg) {
    int* frame_index = (int*)arg;
    printf("[%d] 0x%012lx: %s (%s)\n", (*frame_index)++, addr, sname, fname);
    return true; // Continue walking
}

void deep_function() {
    printf("Stack trace:\n");
    int index = 0;
    bw_backtrace(print_frame, &index);
}

void middle_function() {
    deep_function();
}

int main() {
    middle_function();
    return 0;
}
```

**Sample output:**
```
Stack trace:
[0] 0x000000001234: deep_function (./example)
[1] 0x000000001278: middle_function (./example)
[2] 0x0000000012ab: main (./example)
[3] 0x000000029000: __libc_start_main (/lib/x86_64-linux-gnu/libc.so.6)
```

Note: Addresses are module-relative offsets, not absolute virtual addresses. See [usage documentation](doc/usage.md#usage) for details on PIE vs non-PIE behavior.

**Compile and Link:**
```bash
gcc -fno-omit-frame-pointer -o example example.c -lbackwalk -ldl -rdynamic
```

**Unix**: compile with `-fno-omit-frame-pointer` and link with `-rdynamic` for named frames.

**Windows**: link `dbghelp`. Function names need a PDB next to the executable. Frame pointers are
not required; `CaptureStackBackTrace` uses the OS unwind tables.

For detailed usage patterns and complete examples, see the [`doc/`](doc/) directory.

### Limitations

- Unix walk requires frame pointers (`-fno-omit-frame-pointer`)
- Unix arches: x86_64, AArch64, ARM. Windows: x86, x64, ARM64 via `CaptureStackBackTrace`
- Windows traces are capped at 64 frames (`CaptureStackBackTrace` / `RtlCaptureStackBackTrace`)
- Symbol resolution limited by available symbol information (`-rdynamic` / PDB)
- NOT async-signal-safe (yet)

## License

MIT License - see [LICENSE](LICENSE) file for details.
