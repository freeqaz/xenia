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
#include <cinttypes>
#include <cstddef>
#include <cstdio>

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
  // Use mprotect to change permissions on already-mapped memory.
  //
  // This deviates from upstream, which unconditionally
  // mmap(MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS)s the range. That is not
  // usable here: Memory::Initialize() creates ONE memfd (see
  // CreateFileMappingHandle below) and Memory::MapViews() lays several
  // MAP_SHARED views of it over the guest address space, deliberately mapping
  // the same file offsets at more than one guest address -- that overlap IS
  // the virtual/physical aliasing the guest depends on. A MAP_FIXED anonymous
  // mapping over any part of it replaces the view and silently breaks the
  // alias for that region, so the mprotect path is load-bearing on this fork's
  // mapping scheme and is kept.
  //
  // Consequence, and the reason this is not a free win: mprotect does not
  // zero, so a commit here returns whatever the previous tenant left behind,
  // where upstream's mmap and the console's NtAllocateVirtualMemory both hand
  // back zeroed pages. The zero-fill is done one layer up in BaseHeap
  // (src/xenia/memory.cc, --posix_allocfixed_zero_commit), where the page
  // table says which pages are NEWLY committed. It deliberately is NOT done
  // here: xe::memory::AllocFixed is also called with kNoAccess by
  // Memory::AddVirtualMappedRange (memset would fault) and is called
  // repeatedly by X64CodeCache with a growing length over already-generated
  // code (memset would wipe live JIT output while other threads execute it).
  uint32_t prot = ToPosixProtectFlags(access);
  if (mprotect(base_address, length, prot) == 0) {
    return base_address;
  }
  // Fallback: if mprotect fails (e.g., no mapping exists), create one.
  // MAP_PRIVATE per upstream; this path never covers an aliased view (there is
  // no mapping to preserve if mprotect just failed), and MAP_SHARED bought
  // nothing since xenia never forks.
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

bool QueryProtect(void* base_address, size_t& length, PageAccess& access_out) {
  // Parse /proc/self/maps to find the mapping containing base_address.
  FILE* fp = fopen("/proc/self/maps", "r");
  if (!fp) {
    return false;
  }

  uintptr_t addr = reinterpret_cast<uintptr_t>(base_address);
  char line[512];
  while (fgets(line, sizeof(line), fp)) {
    uintptr_t start, end;
    char perms[5];
    if (sscanf(line, "%" PRIxPTR "-%" PRIxPTR " %4s", &start, &end, perms) ==
        3) {
      if (addr >= start && addr < end) {
        fclose(fp);
        length = end - start;
        bool readable = (perms[0] == 'r');
        bool writable = (perms[1] == 'w');
        bool executable = (perms[2] == 'x');
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
    }
  }
  fclose(fp);
  return false;
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
  int ret = static_cast<int>(
      syscall(SYS_memfd_create, path.c_str(), 0));
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
  void* result = mmap64(base_address, length, prot, MAP_SHARED | MAP_FIXED,
                        handle, file_offset);
  if (result == MAP_FAILED) {
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
