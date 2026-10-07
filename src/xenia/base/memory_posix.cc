/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/memory.h"

#include <fcntl.h>
#include <linux/memfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cerrno>
#include <cstddef>
#include <cstring>

#include "xenia/base/math.h"
#include "xenia/base/platform.h"
#include "xenia/base/string.h"

#if XE_PLATFORM_ANDROID
#include <dlfcn.h>
#include <linux/ashmem.h>
#include <string.h>
#include <sys/ioctl.h>

#include "xenia/base/main_android.h"
#endif

namespace xe {
namespace memory {

#if XE_PLATFORM_ANDROID
// May be null if no dynamically loaded functions are required.
static void* libandroid_;
// API 26+.
static int (*android_ASharedMemory_create_)(const char* name, size_t size);

void AndroidInitialize() {
  if (xe::GetAndroidApiLevel() >= 26) {
    libandroid_ = dlopen("libandroid.so", RTLD_NOW);
    assert_not_null(libandroid_);
    if (libandroid_) {
      android_ASharedMemory_create_ =
          reinterpret_cast<decltype(android_ASharedMemory_create_)>(
              dlsym(libandroid_, "ASharedMemory_create"));
      assert_not_null(android_ASharedMemory_create_);
    }
  }
}

void AndroidShutdown() {
  android_ASharedMemory_create_ = nullptr;
  if (libandroid_) {
    dlclose(libandroid_);
    libandroid_ = nullptr;
  }
}
#endif

size_t page_size() { return getpagesize(); }
size_t allocation_granularity() { return page_size(); }

uint32_t ToPosixProtectFlags(PageAccess access) {
  switch (access) {
    case PageAccess::kNoAccess:
      return PROT_NONE;
    case PageAccess::kReadOnly:
      return PROT_READ;
    case PageAccess::kReadWrite:
      return PROT_READ | PROT_WRITE;
    case PageAccess::kExecuteReadOnly:
      return PROT_READ | PROT_EXEC;
    case PageAccess::kExecuteReadWrite:
      return PROT_READ | PROT_WRITE | PROT_EXEC;
    default:
      assert_unhandled_case(access);
      return PROT_NONE;
  }
}

bool IsWritableExecutableMemorySupported() { return true; }

void* AllocFixed(void* base_address, size_t length,
                 AllocationType allocation_type, PageAccess access) {
  // Change the protection of the existing mapping with mprotect instead of
  // mmap(MAP_FIXED | MAP_ANONYMOUS)'ing over it. Memory::Initialize() creates
  // ONE file mapping and Memory::MapViews() lays several MAP_SHARED views of
  // it over the guest address space, mapping the same file offsets at more
  // than one guest address -- that overlap is the virtual/physical aliasing
  // the guest depends on. An anonymous MAP_FIXED mapping over part of it
  // replaces the view and silently breaks the alias for that region.
  //
  // mprotect does not zero, so a commit here returns whatever the previous
  // tenant left behind, where the console's NtAllocateVirtualMemory hands
  // back zeroed pages. The zero-fill is done one layer up in BaseHeap
  // (src/xenia/memory.cc, --posix_allocfixed_zero_commit), where the page
  // table says which pages are NEWLY committed. It is not done here:
  // AllocFixed is also called with kNoAccess by Memory::AddVirtualMappedRange
  // (memset would fault) and repeatedly by X64CodeCache with a growing length
  // over already-generated code (memset would wipe live JIT output).
  uint32_t prot = ToPosixProtectFlags(access);
  if (mprotect(base_address, length, prot) == 0) {
    return base_address;
  }
  // Fallback: if mprotect fails (no mapping exists yet), create one. This
  // path never covers an aliased view: there is no mapping to preserve.
  void* result = mmap(base_address, length, prot,
                      MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0);
  if (result == MAP_FAILED) {
    return nullptr;
  }
  return result;
}

bool DeallocFixed(void* base_address, size_t length,
                  DeallocationType deallocation_type) {
  return munmap(base_address, length) == 0;
}

bool Protect(void* base_address, size_t length, PageAccess access,
             PageAccess* out_old_access) {
  // Linux does not have a syscall to query memory permissions.
  assert_null(out_old_access);

  uint32_t prot = ToPosixProtectFlags(access);
  return mprotect(base_address, length, prot) == 0;
}

namespace {
// Minimal /proc/self/maps reader for QueryProtect. QueryProtect runs inside
// the SIGSEGV handler (MMIOHandler::ExceptionCallback), so it sticks to
// open/read/close, which are async-signal-safe -- no stdio, no sscanf, no
// allocation.
bool ParseHex(const char*& p, const char* end, uintptr_t& out) {
  uintptr_t value = 0;
  const char* start = p;
  for (; p < end; ++p) {
    char c = *p;
    uint32_t digit;
    if (c >= '0' && c <= '9') {
      digit = uint32_t(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      digit = uint32_t(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
      digit = uint32_t(c - 'A' + 10);
    } else {
      break;
    }
    value = (value << 4) | digit;
  }
  out = value;
  return p != start;
}

// Parses one "start-end perms ..." line. Returns false if malformed.
bool ParseMapsLine(const char* p, const char* end, uintptr_t& start_out,
                   uintptr_t& end_out, PageAccess& access_out) {
  if (!ParseHex(p, end, start_out) || p >= end || *p++ != '-' ||
      !ParseHex(p, end, end_out) || p >= end || *p++ != ' ' || end - p < 3) {
    return false;
  }
  bool readable = p[0] == 'r';
  bool writable = p[1] == 'w';
  bool executable = p[2] == 'x';
  if (executable && writable) {
    access_out = PageAccess::kExecuteReadWrite;
  } else if (executable && readable) {
    access_out = PageAccess::kExecuteReadOnly;
  } else if (writable) {
    access_out = PageAccess::kReadWrite;
  } else if (readable) {
    access_out = PageAccess::kReadOnly;
  } else {
    access_out = PageAccess::kNoAccess;
  }
  return true;
}
}  // namespace

bool QueryProtect(void* base_address, size_t& length, PageAccess& access_out) {
  int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }
  const uintptr_t addr = reinterpret_cast<uintptr_t>(base_address);
  // Lines are short (well under 512 bytes up to the permissions field, which
  // is all that is parsed); a longer line is consumed and its tail skipped.
  char buffer[4096];
  size_t filled = 0;
  bool found = false;
  bool eof = false;
  while (!found) {
    if (!eof && filled < sizeof(buffer)) {
      ssize_t n = read(fd, buffer + filled, sizeof(buffer) - filled);
      if (n < 0) {
        if (errno == EINTR) {
          continue;
        }
        break;
      }
      if (n == 0) {
        eof = true;
      }
      filled += size_t(n);
    }
    char* line_end = static_cast<char*>(std::memchr(buffer, '\n', filled));
    if (!line_end) {
      if (eof || filled == sizeof(buffer)) {
        // Last line without a newline, or an over-long line: parse what we
        // have, then stop (EOF) or drop it and continue.
        line_end = buffer + filled;
      } else {
        continue;
      }
    }
    uintptr_t start, end;
    PageAccess access;
    if (ParseMapsLine(buffer, line_end, start, end, access) && addr >= start &&
        addr < end) {
      length = end - start;
      access_out = access;
      found = true;
      break;
    }
    size_t consumed = size_t(line_end - buffer) + (line_end < buffer + filled);
    std::memmove(buffer, buffer + consumed, filled - consumed);
    filled -= consumed;
    if (eof && filled == 0) {
      break;
    }
  }
  close(fd);
  return found;
}

FileMappingHandle CreateFileMappingHandle(const std::filesystem::path& path,
                                          size_t length, PageAccess access,
                                          bool commit) {
#if XE_PLATFORM_ANDROID
  // TODO(Triang3l): Check if memfd can be used instead on API 30+.
  if (android_ASharedMemory_create_) {
    int sharedmem_fd = android_ASharedMemory_create_(path.c_str(), length);
    return sharedmem_fd >= 0 ? sharedmem_fd : kFileMappingHandleInvalid;
  }

  // Use /dev/ashmem on API versions below 26, which added ASharedMemory.
  // /dev/ashmem was disabled on API 29 for apps targeting it.
  // https://chromium.googlesource.com/chromium/src/+/master/third_party/ashmem/ashmem-dev.c
  int ashmem_fd = open("/" ASHMEM_NAME_DEF, O_RDWR);
  if (ashmem_fd < 0) {
    return kFileMappingHandleInvalid;
  }
  char ashmem_name[ASHMEM_NAME_LEN];
  strlcpy(ashmem_name, path.c_str(), xe::countof(ashmem_name));
  if (ioctl(ashmem_fd, ASHMEM_SET_NAME, ashmem_name) < 0 ||
      ioctl(ashmem_fd, ASHMEM_SET_SIZE, length) < 0) {
    close(ashmem_fd);
    return kFileMappingHandleInvalid;
  }
  return ashmem_fd;
#else
  // Use memfd_create for anonymous file-backed memory. This avoids /dev/shm
  // which can fail with SIGBUS on some systems (e.g., tmpfs with usrquota on
  // Linux 6.18+). Fall back to shm_open if memfd_create is not available.
  int ret =
      static_cast<int>(syscall(SYS_memfd_create, path.c_str(), MFD_CLOEXEC));
  if (ret >= 0) {
    if (ftruncate64(ret, length) != 0) {
      close(ret);
      return kFileMappingHandleInvalid;
    }
    return ret;
  }

  // Fallback: shm_open
  int oflag;
  switch (access) {
    case PageAccess::kNoAccess:
      oflag = 0;
      break;
    case PageAccess::kReadOnly:
    case PageAccess::kExecuteReadOnly:
      oflag = O_RDONLY;
      break;
    case PageAccess::kReadWrite:
    case PageAccess::kExecuteReadWrite:
      oflag = O_RDWR;
      break;
    default:
      assert_always();
      return kFileMappingHandleInvalid;
  }
  oflag |= O_CREAT;
  auto full_path = "/" / path;
  int shm_ret = shm_open(full_path.c_str(), oflag, 0777);
  if (shm_ret < 0) {
    return kFileMappingHandleInvalid;
  }
  if (ftruncate64(shm_ret, length) != 0) {
    close(shm_ret);
    return kFileMappingHandleInvalid;
  }
  return shm_ret;
#endif
}

void CloseFileMappingHandle(FileMappingHandle handle,
                            const std::filesystem::path& path) {
  close(handle);
#if !XE_PLATFORM_ANDROID
  // Only unlink if this was a shm_open fd (not memfd_create).
  // Try shm_unlink — it will harmlessly fail if the name doesn't exist.
  auto full_path = "/" / path;
  shm_unlink(full_path.c_str());
#endif
}

void* MapFileView(FileMappingHandle handle, void* base_address, size_t length,
                  PageAccess access, size_t file_offset) {
  uint32_t prot = ToPosixProtectFlags(access);
  // MAP_FIXED_NOREPLACE, not MAP_FIXED: callers probe candidate bases
  // (Memory::MapViews tries 1 << 32, 1 << 33, ...) and must get a failure
  // when the range is already in use, not silently replace whatever the host
  // process had mapped there. Kernels before 4.17 treat the flag as a hint
  // and may return another address; that is a failure here too.
  int flags = MAP_SHARED | (base_address ? MAP_FIXED_NOREPLACE : 0);
  void* result = mmap64(base_address, length, prot, flags, handle, file_offset);
  if (result == MAP_FAILED) {
    return nullptr;
  }
  if (base_address && result != base_address) {
    munmap(result, length);
    return nullptr;
  }
  return result;
}

bool UnmapFileView(FileMappingHandle handle, void* base_address,
                   size_t length) {
  return munmap(base_address, length) == 0;
}

}  // namespace memory
}  // namespace xe
