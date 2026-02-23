#include "xenia/dc3_hack_pack.h"

#include <atomic>
#include <cstring>
#include <set>
#include <string>

#include "config.h"

#if defined(__linux__)
#include <cerrno>
#include <sys/mman.h>
#endif

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_module.h"
#include "xenia/memory.h"

DECLARE_int32(dc3_crt_bisect_max);
DECLARE_string(dc3_crt_skip_indices);
DECLARE_bool(dc3_crt_skip_nui);
DECLARE_bool(fake_kinect_data);

namespace xe {

namespace {

constexpr uint32_t kPpcLiR3_0 = 0x38600000;
constexpr uint32_t kPpcBlr = 0x4E800020;
static std::atomic<uint32_t> g_dc3_errno_guest_ptr{0};

void Dc3ErrnoExtern(cpu::ppc::PPCContext* ppc_context,
                    kernel::KernelState* kernel_state) {
  (void)kernel_state;
  if (!ppc_context) return;
  uint32_t p = g_dc3_errno_guest_ptr.load(std::memory_order_relaxed);
  ppc_context->r[3] = p;
}

Dc3HackApplyResult& GetResult(Dc3HackPackSummary& summary, Dc3HackCategory category) {
  for (auto& result : summary.results) {
    if (result.category == category) {
      return result;
    }
  }
  summary.results.emplace_back();
  summary.results.back().category = category;
  return summary.results.back();
}

bool PatchStub8(Memory* memory, uint32_t address, uint32_t return_value,
                const char* name) {
  auto* heap = memory->LookupHeap(address);
  if (!heap) return false;
  auto* mem = memory->TranslateVirtual<uint8_t*>(address);
  if (!mem) return false;
  uint32_t w0 = xe::load_and_swap<uint32_t>(mem);
  if (w0 == 0x00000000) return false;
  heap->Protect(address, 8, kMemoryProtectRead | kMemoryProtectWrite);
  uint32_t li_instr = 0x38600000 | (return_value & 0xFFFF);
  xe::store_and_swap<uint32_t>(mem + 0, li_instr);
  xe::store_and_swap<uint32_t>(mem + 4, 0x4E800020);
  XELOGI("DC3: Stubbed {} at {:08X} (li r3,{}; blr)", name, address, return_value);
  return true;
}

void ApplyDc3ImportAndRuntimeStopgaps(const Dc3HackContext& ctx,
                                      Dc3HackPackSummary& summary) {
  auto& import_result = GetResult(summary, Dc3HackCategory::kImports);
  auto& stopgap_result =
      GetResult(summary, Dc3HackCategory::kDecompRuntimeStopgap);
  auto& debug_result = GetResult(summary, Dc3HackCategory::kDebug);
  auto& crt_result = GetResult(summary, Dc3HackCategory::kCrt);

  Memory* memory = ctx.memory;
  auto* module = ctx.module;
  if (!memory || !module) {
    import_result.failed++;
    stopgap_result.failed++;
    debug_result.failed++;
    crt_result.failed++;
    return;
  }

  // Stub XapiCallThreadNotifyRoutines and XRegisterThreadNotifyRoutine.
  const uint32_t kNotifyFuncs[] = {0x8302CEE0};
  for (uint32_t addr : kNotifyFuncs) {
    auto* heap = memory->LookupHeap(addr);
    if (heap) {
      heap->Protect(addr, 8, kMemoryProtectRead | kMemoryProtectWrite);
      auto* p = memory->TranslateVirtual<uint8_t*>(addr);
      xe::store_and_swap<uint32_t>(p + 0, 0x38600000);
      xe::store_and_swap<uint32_t>(p + 4, 0x4E800020);
      XELOGI("DC3: Stubbed XapiCallThreadNotifyRoutines at {:08X} "
             "(li r3,0; blr)",
             addr);
      import_result.applied++;
    } else {
      import_result.skipped++;
    }
  }

  // The decomp XEX specifies a 256KB stack which overflows during init.
  if (module->stack_size() < 4 * 1024 * 1024) {
    XELOGI("DC3: Increasing main thread stack from {}KB to 4096KB",
           module->stack_size() / 1024);
    module->set_stack_size(4 * 1024 * 1024);
    stopgap_result.applied++;
  } else {
    stopgap_result.skipped++;
  }

  // Make the full PE image writable (0x82000000-0x83ED0000).
  // The decomp XEX has global DataNode/DataArray objects in .data sections
  // (e.g. 0x834880E0) that the game writes to at runtime.  The linker marks
  // these sections read-only because they contain initialized data, but the
  // game expects them writable.  Also covers RODATA/BSS workaround.
  {
    auto* heap = memory->LookupHeap(0x82000000);
    if (heap) {
      heap->Protect(0x82000000, 0x1ED0000,
                    kMemoryProtectRead | kMemoryProtectWrite);
      XELOGI("DC3: Made full image 0x82000000-0x83ED0000 writable");
      stopgap_result.applied++;
    } else {
      stopgap_result.skipped++;
    }
  }

  // Map guard/overflow pages as readable zeros.
  // The decomp has stubs that return -1, causing game code to dereference
  // r31=0xFFFFFFFF in tight loops.  Guest addresses near 0xFFFFFFFF map to
  // file offsets past the shm backing size, so the kernel can't serve the
  // pages.  Also pre-map the 0x7F000000-0x7FFFFFFF GPU writeback region
  // and other unmapped areas to avoid tens of thousands of page faults
  // during register scanning.
#if defined(__linux__)
  {
    struct { uint32_t start; uint32_t size; const char* name; } regions[] = {
      {0x7F000000, 0x01000000, "GPU writeback 0x7F000000"},
      {0xFFFFF000, 0x00002000, "top-of-address-space guard"},
    };
    for (auto& r : regions) {
      uint8_t* host = memory->TranslateVirtual<uint8_t*>(r.start);
      void* result = mmap(host, r.size, PROT_READ | PROT_WRITE,
                          MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0);
      if (result != MAP_FAILED) {
        XELOGI("DC3: Mapped {} ({} bytes)", r.name, r.size);
        stopgap_result.applied++;
      } else {
        XELOGW("DC3: Failed to map {}: {}", r.name, strerror(errno));
        stopgap_result.skipped++;
      }
    }
  }
#endif

  // Stub _output_l / _woutput_l / XMP overrides.
  {
    struct OutputFunc {
      uint32_t address;
      const char* name;
    };
    OutputFunc output_funcs[] = {
        {0x835BAC88, "_output_l"},
        {0x835C0994, "_woutput_l"},
        {0x835FAA8C, "XMPOverrideBackgroundMusic"},
        {0x835FAB64, "XMPRestoreBackgroundMusic"},
    };
    for (const auto& func : output_funcs) {
      if (PatchStub8(memory, func.address, 0, func.name)) {
        debug_result.applied++;
      } else {
        debug_result.skipped++;
      }
    }
  }

  // Debug/Holmes/String-related stubs and deadlock breakers.
  {
    struct DebugFunc {
      uint32_t address;
      const char* name;
      uint32_t return_value;
    };
    DebugFunc debug_funcs[] = {
        {0x834B1DFC, "Debug::Fail", 0},
        {0x838DEE34, "XGetLocale", 0},
        {0x838DF03C, "XTLGetLanguage", 1},
        {0x838DF0DC, "DebugBreak", 0},
        {0x8333D160, "GetSystemLanguage", 0},
        {0x8333D620, "GetSystemLocale", 0},
        {0x830EDFF4, "DataNode::Print", 0},
        {0x832DD638, "DataArray::AddRef", 0},
        {0x82F1A8D4, "DataArray::Release", 0},
        {0x83140608, "RndMat::CreateMetaMaterial", 0},
        {0x8302D2EC, "NtAllocateVirtualMemoryWrapper", 0},
        {0x8302D524, "RtlpInsertUnCommittedPages", 0},
        {0x8310CD88, "HolmesClientInit", 0},
        {0x8310D058, "HolmesClientReInit", 0},
        {0x8310D0D8, "HolmesClientPoll", 0},
        {0x8310C958, "HolmesClientPollInternal", 0},
        {0x8310CA70, "HolmesClientInitOpcode", 0},
        {0x8310BF14, "HolmesClientTerminate", 0},
        {0x8310B284, "CanUseHolmes", 0},
        {0x8310B2F8, "UsingHolmes", 0},
        {0x8310B154, "ProtocolDebugString", 0},
        {0x8310B314, "HolmesSetFileShare", 0},
        {0x8310B36C, "HolmesFileHostName", 0},
        {0x8310B378, "HolmesFileShare", 0},
        {0x8310B384, "HolmesResolveIP", 0},
        {0x8310B7E0, "BeginCmd", 0},
        {0x8310B830, "CheckForResponse", 0},
        {0x8310BA34, "WaitForAnyResponse", 0},
        {0x8310C664, "EndCmd", 0},
        {0x8310C76C, "CheckReads", 0},
        {0x8310C850, "CheckInput", 0},
        {0x8310C8F0, "WaitForResponse", 0},
        {0x8310E13C, "WaitForReads", 0},
        {0x8310C9B8, "HolmesClientPollKeyboard", 0},
        {0x8310CA10, "HolmesClientPollJoypad", 0},
        {0x8310D53C, "HolmesClientOpen", 0},
        {0x8310DA24, "HolmesClientRead", 0},
        {0x8310DB5C, "HolmesClientReadDone", 0},
        {0x8310D778, "HolmesClientWrite", 0},
        {0x8310D8E8, "HolmesClientTruncate", 0},
        {0x8310E1D8, "HolmesClientClose", 0},
        {0x8310D244, "HolmesClientGetStat", 0},
        {0x8310D150, "HolmesClientSysExec", 0},
        {0x8310D35C, "HolmesClientMkDir", 0},
        {0x8310D44C, "HolmesClientDelete", 0},
        {0x8310E360, "HolmesClientEnumerate", 0},
        {0x8310DC14, "HolmesClientCacheFile", 0},
        {0x8310DDE4, "HolmesClientCacheResource", 0},
        {0x8310BBC0, "HolmesToLocal", 0},
        {0x8310BCB0, "HolmesFlushStreamBuffer", 0},
        {0x8310BD44, "DumpHolmesLog", 0},
        {0x8310DF18, "HolmesClientStackTrace", 0},
        {0x8310E048, "HolmesClientSendMessage", 0},
    };
    for (const auto& func : debug_funcs) {
      if (PatchStub8(memory, func.address, func.return_value, func.name)) {
        debug_result.applied++;
      } else {
        debug_result.skipped++;
      }
    }
  }

  // Stub String::operator+=(const char*).
  {
    const uint32_t kStringOpPlusEq = 0x834BE118;
    auto* heap = memory->LookupHeap(kStringOpPlusEq);
    if (heap) {
      auto* mem = memory->TranslateVirtual<uint8_t*>(kStringOpPlusEq);
      if (mem && xe::load_and_swap<uint32_t>(mem) != 0x00000000) {
        heap->Protect(kStringOpPlusEq, 4, kMemoryProtectRead | kMemoryProtectWrite);
        xe::store_and_swap<uint32_t>(mem, 0x4E800020);
        XELOGI("DC3: Stubbed String::operator+=(const char*) at {:08X} "
               "(blr — prevents unbounded PE image corruption)",
               kStringOpPlusEq);
        debug_result.applied++;
      } else {
        debug_result.skipped++;
      }
    } else {
      debug_result.skipped++;
    }
  }

  // Decomp-only crash stopgap: invalid-SP entry into String::~String causes a
  // write to 0xFFFFFFF8 at the function prologue (observed crash_guest=0x834BE09C).
  // Stub the destructor so corrupted control flow doesn't take down Thread 6
  // before we can advance further toward present/swap.
  if (ctx.is_decomp_layout) {
    if (PatchStub8(memory, 0x834BE094, 0, "String::~String (invalid SP stopgap)")) {
      stopgap_result.applied++;
    } else {
      stopgap_result.skipped++;
    }
  }

  // Decomp-only speech COM stopgap: first reproducible data-as-code / invalid-SP
  // crash is reached from NUISPEECH CSpCfgInst::AddWordWithCustomProns
  // (0x82B324A0, crash at +0x30 after D3D::UnlockResource calls into speech).
  // Returning failure avoids dereferencing a corrupted callback/vtable-like
  // object (`r3=0x83A89C78`, slots contain 0x4000xxxx import-ish pointers)
  // while we continue tracing the underlying corruption source.
  if (ctx.is_decomp_layout) {
    if (PatchStub8(memory, 0x82B324A0, 0xFFFFFFFFu,
                   "NUISPEECH::CSpCfgInst::AddWordWithCustomProns (stopgap)")) {
      stopgap_result.applied++;
    } else {
      stopgap_result.skipped++;
    }
  }

  // Decomp-only recursive error-reporting/assert loop stopgap. After stubbing
  // CSpCfgInst::AddWordWithCustomProns, the next blocker becomes a repeating
  // error formatting path (format string at r30 = "File: %s Line: %d Error: %s\\n")
  // with stack-frame recursion through 0x83346A2C/0x83347624/0x8337CF00.
  if (ctx.is_decomp_layout) {
    if (PatchStub8(memory, 0x83346A2C, 0,
                   "recursive error-report helper 0x83346A2C (stopgap)")) {
      stopgap_result.applied++;
    } else {
      stopgap_result.skipped++;
    }
  }

  // Decomp-only follow-on debug/assert helper crash after the recursive
  // formatter stopgap: function at 0x834B1240 re-enters a local debug pipeline
  // and dereferences a zeroed object (r3=0x83C16B20, all zeros) at +0x14.
  // Stub to avoid the null-object assert path while continuing bring-up.
  if (ctx.is_decomp_layout) {
    if (PatchStub8(memory, 0x834B1240, 0,
                   "debug/assert helper 0x834B1240 (null object stopgap)")) {
      stopgap_result.applied++;
    } else {
      stopgap_result.skipped++;
    }
  }

  // Decomp-only CRT/TLS stopgap: guest `_errno` currently returns a bogus
  // handle-like pointer (observed `0x400006A8`) on the decomp build, which
  // feeds `_vsnprintf_l` invalid-parameter loops. Provide a stable guest int*
  // backing store via a guest extern override.
  if (ctx.is_decomp_layout && ctx.processor) {
    const uint32_t kErrnoAddr = 0x835B2D68;
    auto* p_errno_fn = memory->TranslateVirtual<uint8_t*>(kErrnoAddr);
    if (p_errno_fn && xe::load_and_swap<uint32_t>(p_errno_fn) != 0x00000000) {
      uint32_t errno_ptr = g_dc3_errno_guest_ptr.load(std::memory_order_relaxed);
      if (!errno_ptr) {
        errno_ptr = memory->SystemHeapAlloc(4, 4);
        if (errno_ptr) {
          if (auto* p_errno = memory->TranslateVirtual<uint8_t*>(errno_ptr)) {
            xe::store_and_swap<uint32_t>(p_errno, 0);
          }
          g_dc3_errno_guest_ptr.store(errno_ptr, std::memory_order_relaxed);
        }
      }
      if (errno_ptr) {
        ctx.processor->RegisterGuestFunctionOverride(kErrnoAddr, Dc3ErrnoExtern,
                                                     "_errno (decomp stopgap)");
        XELOGI("DC3: Registered decomp stopgap guest override for _errno at {:08X} -> {:08X}",
               kErrnoAddr, errno_ptr);
        stopgap_result.applied++;
      } else {
        XELOGW("DC3: Failed to allocate guest errno backing storage for decomp _errno override");
        stopgap_result.failed++;
      }
    } else {
      stopgap_result.skipped++;
    }
  }

  // NOTE: We previously stubbed `except_data_82910450` at `0x82910448`, but
  // thread-6 telemetry later showed the `0x82910438..0x82910450` range on a
  // live NavListSortMgr path. Stubbing this 8-byte region to `li r3,0; blr`
  // can create a stable loop (returns without the surrounding epilogue). Keep
  // the original bytes until we root-cause the control-flow corruption that
  // reaches this region.

  // Stub unresolved import entries (PE thunks + XEX markers).
  {
    const uint32_t kTextStart = 0x822E0000;
    const uint32_t kTextSize = 0x171A414;
    const uint32_t kIdataStart = 0x822D8400;
    const uint32_t kIdataEnd = 0x822D8A34;
    const uint32_t kThunkAreaStart = 0x8395B000;
    const uint32_t kThunkAreaEnd = 0x8395D000;

    int pe_thunks_stubbed = 0;
    int xex_markers_stubbed = 0;

    for (uint32_t off = 0; off + 16 <= kTextSize; off += 4) {
      uint32_t addr = kTextStart + off;
      auto* p = memory->TranslateVirtual<uint8_t*>(addr);
      if (!p) continue;

      uint32_t w0 = xe::load_and_swap<uint32_t>(p);
      uint32_t w1 = xe::load_and_swap<uint32_t>(p + 4);
      uint32_t w2 = xe::load_and_swap<uint32_t>(p + 8);
      uint32_t w3 = xe::load_and_swap<uint32_t>(p + 12);

      bool need_stub = false;
      if ((w0 >> 16) == 0x3D60 && (w1 >> 16) == 0x816B && w2 == 0x7D6903A6 &&
          w3 == 0x4E800420) {
        uint32_t hi = w0 & 0xFFFF;
        int16_t lo = static_cast<int16_t>(w1 & 0xFFFF);
        uint32_t iat_addr = (hi << 16) + lo;
        if (iat_addr >= kIdataStart && iat_addr < kIdataEnd) {
          need_stub = true;
          pe_thunks_stubbed++;
          XELOGI("DC3: Stubbed unrewritten PE import thunk at {:08X} "
                 "(IAT {:08X})",
                 addr, iat_addr);
        }
      }

      if (!need_stub && addr >= kThunkAreaStart && addr < kThunkAreaEnd &&
          (w0 >> 24) == 0x01 && w1 == 0 && w2 == 0 && w3 == 0) {
        need_stub = true;
        xex_markers_stubbed++;
        uint16_t ordinal = w0 & 0xFFFF;
        XELOGI("DC3: Stubbed unresolved XEX import marker at {:08X} "
               "(ordinal 0x{:04X})",
               addr, ordinal);
      }
      if (!need_stub) continue;

      auto* heap = memory->LookupHeap(addr);
      if (!heap) {
        import_result.failed++;
        continue;
      }
      heap->Protect(addr, 16, kMemoryProtectRead | kMemoryProtectWrite);
      xe::store_and_swap<uint32_t>(p + 0, 0x38600000);
      xe::store_and_swap<uint32_t>(p + 4, 0x4E800020);
      xe::store_and_swap<uint32_t>(p + 8, 0x60000000);
      xe::store_and_swap<uint32_t>(p + 12, 0x60000000);
    }
    XELOGI("DC3: Import thunk cleanup: {} PE thunks + {} XEX markers = {} "
           "total stubbed",
           pe_thunks_stubbed, xex_markers_stubbed,
           pe_thunks_stubbed + xex_markers_stubbed);
    import_result.applied += pe_thunks_stubbed + xex_markers_stubbed;
  }

  // Zero page mapping (all zeros) + null-deref guard below virtual_membase.
  {
    auto* heap = memory->LookupHeap(0x00000000);
    if (heap) {
      heap->Protect(0x00000000, 0x10000, kMemoryProtectRead | kMemoryProtectWrite);
      auto* base = memory->TranslateVirtual<uint8_t*>(0x00000000);
      std::memset(base, 0, 0x10000);
      XELOGI("DC3: Mapped zero page 0x0-0x10000 (all zeros — null "
             "object reads return 0, null checks work correctly)");
      stopgap_result.applied++;
    } else {
      stopgap_result.failed++;
    }

#if defined(__linux__)
    {
      auto* vmbase = memory->virtual_membase();
      void* guard_base = vmbase - 0x10000;
      void* result =
          mmap(guard_base, 0x10000, PROT_READ,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
      if (result != MAP_FAILED) {
        XELOGI("DC3: Mapped null-deref guard page at {:p} "
               "(64KB below virtual_membase {:p})",
               result, (void*)vmbase);
        stopgap_result.applied++;
      } else {
        XELOGI("DC3: Could not map null-deref guard page below "
               "virtual_membase (errno={})",
               errno);
        stopgap_result.skipped++;
      }
    }
#else
    stopgap_result.skipped++;
#endif
  }

  // CRT sanitizer + injection.
  {
    const uint32_t kCodeStart = 0x822C0000;
    struct CrtTable {
      uint32_t start;
      uint32_t end;
      const char* name;
    };
    CrtTable tables[] = {
        {0x83A7B2E0, 0x83A7B8F8, "__xc_a..__xc_z (C++ constructors)"},
        {0x83A7B8FC, 0x83A7B908, "__xi_a..__xi_z (C initializers)"},
    };
    const int32_t bisect_max = cvars::dc3_crt_bisect_max;
    if (bisect_max >= 0) {
      XELOGI("DC3: CRT bisect mode ACTIVE - only allowing constructor "
             "indices 0..{} (inclusive)",
             bisect_max);
    }

    std::set<int> skip_set;
    auto parse_skip_list = [&](const std::string& str) {
      size_t pos = 0;
      while (pos < str.size()) {
        size_t comma = str.find(',', pos);
        if (comma == std::string::npos) comma = str.size();
        std::string token = str.substr(pos, comma - pos);
        if (!token.empty()) {
          size_t dash = token.find('-');
          if (dash != std::string::npos && dash > 0) {
            int lo = std::stoi(token.substr(0, dash));
            int hi = std::stoi(token.substr(dash + 1));
            for (int i = lo; i <= hi; ++i) skip_set.insert(i);
          } else {
            skip_set.insert(std::stoi(token));
          }
        }
        pos = comma + 1;
      }
    };
    if (cvars::dc3_crt_skip_nui && cvars::dc3_crt_skip_indices.empty()) {
      parse_skip_list("69,75,98-101,210-328");
      XELOGI("DC3: Auto-NUI skip enabled (69,75,98-101,210-328)");
    }
    if (!cvars::dc3_crt_skip_indices.empty()) {
      parse_skip_list(cvars::dc3_crt_skip_indices);
    }
    if (!skip_set.empty()) {
      XELOGI("DC3: CRT skip list has {} entries", skip_set.size());
    }

    for (const auto& table : tables) {
      auto* crt_heap = memory->LookupHeap(table.start);
      if (crt_heap) {
        uint32_t page_start = table.start & ~0xFFFu;
        uint32_t page_end = (table.end + 0xFFFu) & ~0xFFFu;
        crt_heap->Protect(page_start, page_end - page_start,
                          kMemoryProtectRead | kMemoryProtectWrite);
      }
      int nullified_oob = 0;
      int nullified_bisect = 0;
      int nullified_skip = 0;
      int valid_count = 0;
      int already_null = 0;
      int total = (table.end - table.start) / 4;
      int index = 0;
      for (uint32_t addr = table.start; addr < table.end; addr += 4, ++index) {
        auto* p = memory->TranslateVirtual<uint8_t*>(addr);
        uint32_t entry = xe::load_and_swap<uint32_t>(p);
        if (entry == 0) {
          already_null++;
          continue;
        }
        if (entry < kCodeStart) {
          XELOGI("DC3: CRT[{:3d}] = {:08X} (nullified-oob, not in code section)",
                 index, entry);
          xe::store_and_swap<uint32_t>(p, 0);
          nullified_oob++;
        } else if (bisect_max >= 0 && index > bisect_max) {
          XELOGI("DC3: CRT[{:3d}] = {:08X} (nullified-bisect, index > {})",
                 index, entry, bisect_max);
          xe::store_and_swap<uint32_t>(p, 0);
          nullified_bisect++;
        } else if (!skip_set.empty() && skip_set.count(index)) {
          XELOGI("DC3: CRT[{:3d}] = {:08X} (nullified-skip, in skip list)", index,
                 entry);
          auto* fn = memory->TranslateVirtual<uint8_t*>(entry);
          if (fn) {
            XELOGI("DC3:   PPC instructions at {:08X}:", entry);
            for (int i = 0; i < 8; ++i) {
              uint32_t insn = xe::load_and_swap<uint32_t>(fn + i * 4);
              XELOGI("DC3:     {:08X}: {:08X}", entry + i * 4, insn);
            }
          }
          xe::store_and_swap<uint32_t>(p, 0);
          nullified_skip++;
        } else {
          XELOGI("DC3: CRT[{:3d}] = {:08X} (valid)", index, entry);
          valid_count++;
        }
      }
      XELOGI("DC3: CRT table {}: {} total entries, {} already null, {} valid, "
             "{} nullified-oob, {} nullified-bisect, {} nullified-skip",
             table.name, total, already_null, valid_count, nullified_oob,
             nullified_bisect, nullified_skip);
      crt_result.applied++;
    }

    struct CrtInject {
      int slot;
      uint32_t func_addr;
      const char* name;
    };
    CrtInject injections[] = {
        {69, 0x833468CC, "InitMakeString"},
    };
    const uint32_t xc_start = 0x83A7B2E0;
    for (const auto& inj : injections) {
      uint32_t slot_addr = xc_start + inj.slot * 4;
      auto* p = memory->TranslateVirtual<uint8_t*>(slot_addr);
      uint32_t current = xe::load_and_swap<uint32_t>(p);
      if (current == 0) {
        xe::store_and_swap<uint32_t>(p, inj.func_addr);
        XELOGI("DC3: CRT[{:3d}] injected {:08X} ({})", inj.slot, inj.func_addr,
               inj.name);
        crt_result.applied++;
      } else {
        XELOGI("DC3: CRT[{:3d}] injection SKIPPED - slot not empty "
               "(contains {:08X}), wanted to inject {:08X} ({})",
               inj.slot, current, inj.func_addr, inj.name);
        crt_result.skipped++;
      }
    }
  }

  // Diagnostics: check JIT indirection table state for import thunk area.
  {
    auto* code_cache = reinterpret_cast<uint8_t*>(0x80000000);
    uint32_t check_addrs[] = {
        0x8395C668,
        0x8395C000,
        0x8395B000,
        0x822E0000,
    };
    for (auto addr : check_addrs) {
      uint32_t* slot =
          reinterpret_cast<uint32_t*>(code_cache + (addr - 0x80000000));
      XELOGI("DC3: Indirection table [{:08X}] = {:08X}", addr, *slot);
      import_result.applied++;
    }
  }
}

}  // namespace

const char* Dc3HackCategoryName(Dc3HackCategory category) {
  switch (category) {
    case Dc3HackCategory::kCrt:
      return "crt";
    case Dc3HackCategory::kSkeleton:
      return "skeleton";
    case Dc3HackCategory::kDebug:
      return "debug";
    case Dc3HackCategory::kDecompRuntimeStopgap:
      return "decomp_stopgap";
    case Dc3HackCategory::kImports:
      return "imports";
    default:
      return "unknown";
  }
}

void Dc3MaybeCleanStaleContentCache(const std::filesystem::path& content_root) {
  auto dc3_content = content_root / "373307D9";
  if (std::filesystem::exists(dc3_content)) {
    XELOGI("DC3: Cleaning stale content cache at {}", xe::path_to_utf8(dc3_content));
    std::error_code ec;
    std::filesystem::remove_all(dc3_content, ec);
    if (ec) {
      XELOGI("DC3: Failed to clean content cache: {}", ec.message());
    }
  }
}

Dc3HackPackSummary ApplyDc3HackPack(const Dc3HackContext& ctx) {
  Dc3HackPackSummary summary;
  // Seed categories so emulator summary logging is stable even before skeleton
  // extraction lands.
  GetResult(summary, Dc3HackCategory::kImports);
  GetResult(summary, Dc3HackCategory::kDecompRuntimeStopgap);
  GetResult(summary, Dc3HackCategory::kDebug);
  GetResult(summary, Dc3HackCategory::kCrt);
  GetResult(summary, Dc3HackCategory::kSkeleton);

  ApplyDc3ImportAndRuntimeStopgaps(ctx, summary);
  return summary;
}

Dc3HackApplyResult ApplyDc3SkeletonHackPack(const Dc3HackContext& ctx) {
  Dc3HackApplyResult result;
  result.category = Dc3HackCategory::kSkeleton;

  if (!ctx.memory) {
    result.failed++;
    return result;
  }
  if (!cvars::fake_kinect_data || ctx.is_decomp_layout) {
    result.skipped++;
    return result;
  }

  Memory* memory = ctx.memory;
  const uint32_t kGetNextFrameAddr = 0x829C2790;
  const uint32_t kSkeletonFrameSize = 0xAB0;  // 2736 bytes
  const uint32_t kDataSize = kSkeletonFrameSize + 4;  // +4 for counter

  uint32_t data_guest_addr = memory->SystemHeapAlloc(kDataSize, 0x10);
  if (!data_guest_addr) {
    XELOGW("DC3: Failed to allocate guest memory for fake skeleton data");
    result.failed++;
    return result;
  }
  const uint32_t kSkeletonDataAddr = data_guest_addr;
  const uint32_t kCounterAddr = data_guest_addr + kSkeletonFrameSize;

  auto* heap = memory->LookupHeap(kGetNextFrameAddr);
  if (!heap) {
    result.failed++;
    return result;
  }

  heap->Protect(kGetNextFrameAddr, 0x4C, kMemoryProtectRead | kMemoryProtectWrite);
  auto* stub_mem = memory->TranslateVirtual<uint8_t*>(kGetNextFrameAddr);
  auto* counter_mem = memory->TranslateVirtual<uint8_t*>(kCounterAddr);
  auto* data_mem = memory->TranslateVirtual<uint8_t*>(kSkeletonDataAddr);
  if (!stub_mem || !counter_mem || !data_mem) {
    XELOGW("DC3: Failed to translate memory for fake Kinect skeleton injection");
    result.failed++;
    return result;
  }

  uint32_t ppc_stub[] = {
      0x7C882378,                                // mr r8, r4
      0x3CA00000 | (kSkeletonDataAddr >> 16),    // lis r5, hi16(data)
      0x60A50000 | (kSkeletonDataAddr & 0xFFFF), // ori r5, r5, lo16(data)
      0x38C00000 | (kSkeletonFrameSize / 4),     // li r6, word_count
      0x7CC903A6,                                // mtctr r6
      0x80E50000,                                // lwz r7, 0(r5)
      0x90E40000,                                // stw r7, 0(r4)
      0x38A50004,                                // addi r5, r5, 4
      0x38840004,                                // addi r4, r4, 4
      0x4200FFF0,                                // bdnz -16 (to lwz)
      0x3CA00000 | (kCounterAddr >> 16),         // lis r5, hi16(counter)
      0x60A50000 | (kCounterAddr & 0xFFFF),      // ori r5, r5, lo16(counter)
      0x80C50000,                                // lwz r6, 0(r5)
      0x38C60001,                                // addi r6, r6, 1
      0x90C50000,                                // stw r6, 0(r5)
      0x90C80004,                                // stw r6, 4(r8) (timestamp)
      0x90C80008,                                // stw r6, 8(r8) (frame num)
      0x38600000,                                // li r3, 0 (S_OK)
      0x4E800020,                                // blr
  };
  for (size_t i = 0; i < sizeof(ppc_stub) / sizeof(ppc_stub[0]); i++) {
    xe::store_and_swap<uint32_t>(stub_mem + i * 4, ppc_stub[i]);
  }

  xe::store_and_swap<uint32_t>(counter_mem, 0);
  std::memset(data_mem, 0, kSkeletonFrameSize);

  auto write_float = [data_mem](uint32_t offset, float value) {
    xe::store_and_swap<float>(data_mem + offset, value);
  };
  auto write_u32 = [data_mem](uint32_t offset, uint32_t value) {
    xe::store_and_swap<uint32_t>(data_mem + offset, value);
  };

  write_u32(0x0008, 1);
  write_float(0x0014, 1.0f);
  write_float(0x0024, 1.0f);

  const uint32_t skel0 = 0x30;
  write_u32(skel0 + 0x00, 2);
  write_u32(skel0 + 0x04, 1);
  write_u32(skel0 + 0x0C, 0);
  write_float(skel0 + 0x10, 0.0f);
  write_float(skel0 + 0x14, 0.9f);
  write_float(skel0 + 0x18, 2.0f);
  write_float(skel0 + 0x1C, 1.0f);

  struct JointPos {
    float x, y, z;
  };
  JointPos joints[20] = {
      {0.00f, 0.90f, 2.0f},   {0.00f, 1.10f, 2.0f},   {0.00f, 1.35f, 2.0f},
      {0.00f, 1.60f, 2.0f},   {-0.20f, 1.35f, 2.0f},  {-0.50f, 1.35f, 2.0f},
      {-0.75f, 1.35f, 2.0f},  {-0.85f, 1.35f, 2.0f},  {0.20f, 1.35f, 2.0f},
      {0.50f, 1.35f, 2.0f},   {0.75f, 1.35f, 2.0f},   {0.85f, 1.35f, 2.0f},
      {-0.15f, 0.90f, 2.0f},  {-0.15f, 0.50f, 2.0f},  {-0.15f, 0.05f, 2.0f},
      {0.15f, 0.90f, 2.0f},   {0.15f, 0.50f, 2.0f},   {0.15f, 0.05f, 2.0f},
      {-0.15f, 0.00f, 2.0f},  {0.15f, 0.00f, 2.0f},
  };
  const uint32_t joints_offset = skel0 + 0x20;
  for (int j = 0; j < 20; j++) {
    uint32_t off = joints_offset + j * 16;
    write_float(off + 0, joints[j].x);
    write_float(off + 4, joints[j].y);
    write_float(off + 8, joints[j].z);
    write_float(off + 12, 1.0f);
  }
  const uint32_t tracking_offset = skel0 + 0x160;
  for (int j = 0; j < 20; j++) {
    write_u32(tracking_offset + j * 4, 2);
  }

  struct BinaryPatch {
    uint32_t address;
    uint32_t value;
    const char* name;
  };
  BinaryPatch skel_patches[] = {
      {0x8242E74C, 0x3B800021,
       "SkeletonUpdateThread: timeout INFINITE -> 33ms"},
      {0x8242E1B0, 0x60000000, "SkeletonUpdate::Update: NOP IsOverride branch"},
  };
  for (const auto& p : skel_patches) {
    auto* h = memory->LookupHeap(p.address);
    if (!h) {
      result.failed++;
      continue;
    }
    h->Protect(p.address, 4, kMemoryProtectRead | kMemoryProtectWrite);
    auto* m = memory->TranslateVirtual<uint8_t*>(p.address);
    if (!m) {
      result.failed++;
      continue;
    }
    xe::store_and_swap<uint32_t>(m, p.value);
    XELOGI("  Patched {:08X}: {}", p.address, p.name);
    result.applied++;
  }

  XELOGI("DC3: Fake Kinect skeleton data written at {:08X} ({} bytes), "
         "PPC stub at {:08X}, counter at {:08X}",
         kSkeletonDataAddr, kSkeletonFrameSize, kGetNextFrameAddr, kCounterAddr);
  result.applied++;
  return result;
}

}  // namespace xe
