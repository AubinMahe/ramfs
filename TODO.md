# TODO & Future Roadmap for `ramfs`

This document outlines potential future enhancements, optimizations, and feature ideas for upcoming releases of `ramfs`.

## 1. POSIX & Virtual Filesystem (VFS) Extensions
- [ ] **Memory-Mapped Files (`mmap`, `munmap`)**:
  - Provide in-memory memory mapping returning direct pointers to file inline buffers or contiguous virtual ranges.
  - Support `PROT_READ`, `PROT_WRITE`, and `MAP_SHARED` / `MAP_PRIVATE` flags.
- [ ] **Hard Links & Symbolic Links (`link`, `symlink`, `readlink`)**:
  - Enable multiple catalog entries pointing to the same `ram_file_t` inode (`nlink > 1`).
  - Add virtual symlink resolution in path normalization (`rfs_normalize_path`).
- [ ] **File Locking APIs (`fcntl(F_SETLK)`, `flock`)**:
  - Expose cooperative advisory file locking across handles or threads.

## 2. Resource Management & Quotas
- [ ] **Configurable Memory Quotas & Limits**:
  - Implement global and per-mount memory limits (`rfs_set_max_memory(size_t bytes)`).
  - Return `ENOSPC` when allocations exceed the configured ceiling.
- [ ] **Eviction & LRU Cache Strategy (Optional Cache Mode)**:
  - Add optional automatic eviction of unmodified clean chunks or temporary files when memory pressure occurs.

## 3. Integration & Ecosystem
- [ ] **CMake & Meson Build Configurations**:
  - Provide modern `CMakeLists.txt` and `meson.build` alongside the standard `Makefile` for streamlined integration with vcpkg, Conan, and CMake-based parent projects.
- [ ] **FUSE Daemon Wrapper (`ramfs-fuse`)**:
  - Create a lightweight FUSE (Filesystem in Userspace) frontend enabling external non-C applications or CLI tools to mount `ramfs` directly into the OS namespace.
- [ ] **CI/CD Automation (GitHub Actions)**:
  - Automated matrix workflow testing GCC/Clang on Linux, MinGW-w64 on Windows, macOS, AddressSanitizer, UndefinedBehaviorSanitizer, and ThreadSanitizer on every push and pull request.
