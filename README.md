# `ramfs`: High-Performance In-Memory Virtual Filesystem (VFS) in C11

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE.txt)
[![C Standard](https://img.shields.io/badge/Standard-C11-orange.svg)](#)
[![Platforms](https://img.shields.io/badge/Platform-Linux%20%7C%20Windows%20(MinGW)-lightgrey.svg)](#)
[![QA](https://img.shields.io/badge/QA-ASan%20%7C%20UBSan%20%7C%20Valgrind%20%7C%20TSan-brightgreen.svg)](#)

`ramfs` is a standalone, cross-platform (Linux & Windows), ultra-fast in-memory filesystem written in C11. It provides a complete virtual filesystem (VFS) emulating standard POSIX low-level I/O (`open`, `read`, `write`, `lseek`, `close`, `stat`, `unlink`, `dup`, `truncate`, `<dirent.h>`) and standard C library streams (`<stdio.h>`, `fopen`, `fread`, `fwrite`, `fprintf`, `fgets`, `tmpfile`, `getline`).

Designed for performance-critical tasks, compiler passes, test suites, database caches, and legacy software acceleration, `ramfs` eliminates disk latency and filesystem bottlenecks by keeping files entirely in RAM.

---

## Key Highlights

- **Transparent Drop-in Integration**: Enable `#define USE_RAMFS` and standard I/O calls (`open`, `read`, `fopen`, `fprintf`, etc.) are redirected to memory without rewriting application logic.
- **Hybrid VFS & Passthrough**: Define a virtual mount prefix (e.g., `rfs_mount("/ramfs")` or `export RAMFS_MOUNT_PREFIX=/ram/`). Paths matching the prefix run entirely in RAM, while all other paths pass transparently to the underlying OS filesystem.
- **Descriptor Partitioning**: Virtual descriptors reside in an isolated range (`0x40000000`), guaranteeing zero collision with host OS file descriptors and standard streams (`stdin`, `stdout`, `stderr`).
- **4-Tier Storage Architecture**:
  1. *Small File Optimization (SFO)*: Files $\le 1\text{ KB}$ use an inlined buffer with zero heap allocation.
  2. *Chunked Pagination*: Files $> 1\text{ KB}$ scale using fixed 64 KB memory chunks.
  3. *Slab Allocator*: Chunks are carved from 1 MB slabs and recycled via a LIFO pool.
  4. *Preheating*: Initial slab preheated at startup for instant zero-latency allocation.
- **Dynamic Auto-Extensible Catalog**:
  - Open-addressing hash table with backward-shift deletion (zero tombstones).
  - Hardware-accelerated CRC32C (SSE4.2) with seamless portable software fallback.
  - Automatic doubling and $O(N)$ re-indexing maintaining load factor $\le 0.5$.
- **Zero-Cost Thread Safety (Dual Build)**:
  - `libramfs`: Single-threaded build with all synchronization compiled to no-ops (32 ns per I/O op, 31.1 M ops/s).
  - `libramfs_mt`: Multi-threaded build with fine-grained per-file reader/writer locks (`pthread_rwlock_t`) and recursive mutexes, validated clean under ThreadSanitizer (`-fsanitize=thread`).
- **Full POSIX Directory & Navigation**:
  - `<dirent.h>` directory scanning (`opendir`, `readdir`, `closedir`, `seekdir`, `telldir`).
  - Virtual working directory context (`chdir`, `getcwd`).
  - Descriptor cloning (`dup`, `dup2`) with synchronized shared seek offsets.
  - File truncation (`ftruncate`, `truncate`) and extended stdio (`tmpfile`, `getline`).
- **Native `.tar.gz` Archive & Directory Synchronization**:
  - Export and import virtual trees to/from standard `.tar.gz` archives (`rfs_save_archive`, `rfs_load`).
  - Sync virtual trees to/from physical directories (`rfs_save_dir`, `rfs_load_dir`).

---

## Performance Benchmark

Measured on Linux (5,000 files of 1,024 bytes each, 27,500 total filesystem operations):

```text
================================================================================
       BENCHMARK: PHYSICAL DISK vs RAMFS (5,000 files of 1,024 bytes)
================================================================================
Operation                      | Physical Disk | ramfs          | Speedup
--------------------------------------------------------------------------------
Sequential create & write (5k) |      0.2628 s |       0.0036 s |   73.5x
Double read verification (10k) |      0.2671 s |       0.0052 s |   51.6x
In-place seek & updates (2.5k) |      0.0755 s |       0.0008 s |   91.6x
Metadata stat queries (5k)     |      0.0109 s |       0.0011 s |    9.8x
Removal unlink (5k)            |      0.1727 s |       0.0016 s |  109.9x
================================================================================
TOTAL WORKLOAD                 |      0.7891 s |       0.0123 s |   64.4x
================================================================================
```

### Micro-Benchmark (Pure Memory I/O Latency)
- **Single-Thread (`libramfs`)**: **32.1 ns / op** (~31.1 million operations/second)
- **Multi-Thread (`libramfs_mt`)**: **88.3 ns / op** (~11.3 million operations/second)

---

## Quick Start

### 1. Direct API Usage

```c
#include <ramfs.h>
#include <stdio.h>
#include <fcntl.h>

int main(void) {
    /* Initialize ramfs with auto-extending catalog */
    if (rfs_init(0, 0) != 0) {
        perror("Failed to init ramfs");
        return 1;
    }

    /* POSIX low-level I/O */
    rfs_handle_t fd = rfs_open("my_folder/data.bin", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    rfs_write(fd, "Hello in-memory world!\n", 23);
    rfs_close(fd);

    /* Standard C stdio stream */
    FILE *fp = rfs_fopen("my_folder/data.bin", "r");
    char buffer[64];
    if (rfs_fgets(buffer, sizeof(buffer), fp)) {
        printf("Read back: %s", buffer);
    }
    rfs_fclose(fp);

    /* Clean up all memory structures */
    rfs_destroy();
    return 0;
}
```

### 2. Transparent Redirection (`USE_RAMFS`)

Include `ramfs.h` with `USE_RAMFS` defined to transparently redirect standard calls:

```c
#define USE_RAMFS
#include <ramfs.h>

void process_data(void) {
    /* Transparently intercepted by ramfs */
    FILE *f = fopen("/ramfs/temp_matrix.dat", "w+b");
    fprintf(f, "Processing block: %d\n", 42);
    fclose(f);
}
```

### 3. Hybrid VFS Mount

```c
/* Route /ramfs/* to memory, let all other paths reach disk */
rfs_mount("/ramfs");

/* Written to RAM */
FILE *ram = rfs_fopen("/ramfs/scratch.tmp", "w");
/* Written to physical disk through libc */
FILE *disk = rfs_fopen("config.json", "r");
```

---

## Building & Quality Assurance

### Prerequisites

- **Debian / Ubuntu**:
  ```bash
  sudo apt install build-essential zlib1g-dev libz-mingw-w64-dev valgrind wine lcov pmccabe
  ```
- **RHEL / Rocky Linux 9**:
  ```bash
  sudo dnf install -y epel-release
  sudo dnf install -y gcc make zlib-devel mingw64-gcc mingw64-zlib valgrind wine
  ```

### Build Commands

| Target | Description |
| :--- | :--- |
| `make all` | Builds both Linux and Windows targets |
| `make linux` | Builds `libramfs.so` (single-thread) and `libramfs_mt.so` (multi-thread) |
| `make windows` | Cross-compiles `ramfs.dll` and `ramfs_mt.dll` using MinGW-w64 |
| `make test` | Runs the full Linux and Windows test suite |
| `make test_mt` | Runs the 16-worker concurrent multi-threaded stress test |
| `make bench` | Runs the comparative disk vs. ramfs benchmark suite |
| `make tsan` | Runs ThreadSanitizer verification (zero data races) |
| `make asan` | Runs AddressSanitizer & UndefinedBehaviorSanitizer suite |
| `make valgrind` | Runs Valgrind memory leak and memory error verification |
| `make efence` | Runs Electric Fence buffer overrun checks |
| `make complexity` | Audits cyclomatic complexity with `pmccabe` (100% functions CCN < 25) |
| `make coverage` | Generates HTML code coverage report via `lcov` / `genhtml` |
| `make format-sources` | Automatically formats all source code according to `.clang-format` |
| `make clean` | Removes all build artifacts |

---

## Architecture Overview

```text
       Application Code (POSIX open, read, write / stdio fopen, fread...)
                                     │
                 ┌───────────────────┴───────────────────┐
                 │       Hybrid VFS Router (ramfs_vfs)   │
                 └───────────────────┬───────────────────┘
                                     │
           Is path under mount prefix (or fd >= 0x40000000)?
                       ├── NO  ──► Host OS libc / Disk
                       └── YES ──► In-Memory Subsystem
                                     │
        ┌────────────────────────────┼────────────────────────────┐
        ▼                            ▼                            ▼
POSIX Layer (ramfs_posix)   stdio Layer (ramfs_stdio)   Archive (ramfs_archive)
Descriptor management       0xBA5E0000 stream tags      .tar.gz export & import
opendir, dup, truncate      getline, ungetc, tmpfile    Recursive directory sync
        │                            │                            │
        └────────────────────────────┼────────────────────────────┘
                                     ▼
                   Core Engine & Memory (ramfs_core)
        ├── Dynamic Catalog (CRC32C hash table, linear probing)
        ├── 4-Tier Storage (SFO 1 KB, Chunks 64 KB, Slabs 1 MB)
        └── Zero-Cost Sync (ramfs_sync: rwlocks & mutexes)
```

---

## License

`ramfs` is licensed under the **GNU General Public License v3.0 or later** (GPL-3.0-or-later). See [LICENSE.txt](LICENSE.txt) for details.
