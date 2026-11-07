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
- [x] **CMake & CTest Build Configuration**:
  - Modern `CMakeLists.txt` supporting `FetchContent`, `find_package()`, exported targets (`ramfs::ramfs`, `ramfs::ramfs_mt`), and CTest suite alongside `Makefile`.
- [ ] **Meson Build Configuration**:
  - Add optional `meson.build` configuration.
- [ ] **FUSE Daemon Wrapper (`ramfs-fuse`)**:
  - Create a lightweight FUSE (Filesystem in Userspace) frontend enabling external non-C applications or CLI tools to mount `ramfs` directly into the OS namespace.
- [x] **CI/CD Automation (GitHub Actions)**:
  - Automated matrix workflow testing GCC/Clang on Linux, MinGW-w64 on Windows, AddressSanitizer, UndefinedBehaviorSanitizer, ThreadSanitizer, Valgrind, and Electric Fence on every push and pull request.
