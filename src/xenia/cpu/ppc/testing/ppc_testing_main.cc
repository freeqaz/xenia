/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/clock.h"
#include "xenia/base/console_app_main.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/literals.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/platform.h"
#include "xenia/base/string_buffer.h"
#include "xenia/cpu/cpu_flags.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/ppc/ppc_frontend.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/raw_module.h"
#include "xenia/cpu/thread_state.h"

#if XE_ARCH_AMD64
#include "xenia/cpu/backend/x64/x64_backend.h"
#endif  // XE_ARCH

#if XE_COMPILER_MSVC
#include "xenia/base/platform_win.h"
#endif  // XE_COMPILER_MSVC

DEFINE_path(test_path, "src/xenia/cpu/ppc/testing/",
            "Directory scanned for test files.", "Other");
DEFINE_path(test_bin_path, "src/xenia/cpu/ppc/testing/bin/",
            "Directory with binary outputs of the test files.", "Other");
DEFINE_transient_string(test_name, "", "Test suite name.", "General");

namespace xe {
namespace cpu {
namespace test {

using xe::cpu::ppc::PPCContext;
using namespace xe::literals;

typedef std::vector<std::pair<std::string, std::string>> AnnotationList;

const uint32_t START_ADDRESS = 0x80000000;

struct TestCase {
  TestCase(uint32_t address, std::string& name)
      : address(address), name(name) {}
  uint32_t address;
  std::string name;
  AnnotationList annotations;
};

class TestSuite {
 public:
  TestSuite(const std::filesystem::path& src_file_path)
      : src_file_path_(src_file_path) {
    auto name = src_file_path.filename();
    name = name.replace_extension();

    name_ = xe::path_to_utf8(name);
    map_file_path_ = cvars::test_bin_path / name.replace_extension(".map");
    bin_file_path_ = cvars::test_bin_path / name.replace_extension(".bin");
  }

  bool Load() {
    if (!ReadMap()) {
      XELOGE("Unable to read map for test {}",
             xe::path_to_utf8(src_file_path_));
      return false;
    }
    if (!ReadAnnotations()) {
      XELOGE("Unable to read annotations for test {}",
             xe::path_to_utf8(src_file_path_));
      return false;
    }
    return true;
  }

  const std::string& name() const { return name_; }
  const std::filesystem::path& src_file_path() const { return src_file_path_; }
  const std::filesystem::path& map_file_path() const { return map_file_path_; }
  const std::filesystem::path& bin_file_path() const { return bin_file_path_; }
  std::vector<TestCase>& test_cases() { return test_cases_; }

 private:
  std::string name_;
  std::filesystem::path src_file_path_;
  std::filesystem::path map_file_path_;
  std::filesystem::path bin_file_path_;
  std::vector<TestCase> test_cases_;

  TestCase* FindTestCase(const std::string_view name) {
    for (auto& test_case : test_cases_) {
      if (test_case.name == name) {
        return &test_case;
      }
    }
    return nullptr;
  }

  bool ReadMap() {
    FILE* f = filesystem::OpenFile(map_file_path_, "r");
    if (!f) {
      return false;
    }
    char line_buffer[BUFSIZ];
    while (fgets(line_buffer, sizeof(line_buffer), f)) {
      if (!strlen(line_buffer)) {
        continue;
      }
      // 0000000000000000 t test_add1\n
      char* newline = strrchr(line_buffer, '\n');
      if (newline) {
        *newline = 0;
      }
      char* t_test_ = strstr(line_buffer, " t test_");
      if (!t_test_) {
        continue;
      }
      std::string address(line_buffer, t_test_ - line_buffer);
      std::string name(t_test_ + strlen(" t test_"));
      test_cases_.emplace_back(START_ADDRESS + std::stoul(address, 0, 16),
                               name);
    }
    fclose(f);
    return true;
  }

  bool ReadAnnotations() {
    TestCase* current_test_case = nullptr;
    FILE* f = filesystem::OpenFile(src_file_path_, "r");
    if (!f) {
      return false;
    }
    char line_buffer[BUFSIZ];
    while (fgets(line_buffer, sizeof(line_buffer), f)) {
      if (!strlen(line_buffer)) {
        continue;
      }
      // Eat leading whitespace.
      char* start = line_buffer;
      while (*start == ' ') {
        ++start;
      }
      if (strncmp(start, "test_", strlen("test_")) == 0) {
        // Global test label.
        std::string label(start + strlen("test_"), strchr(start, ':'));
        current_test_case = FindTestCase(label);
        if (!current_test_case) {
          XELOGE("Test case {} not found in corresponding map for {}", label,
                 xe::path_to_utf8(src_file_path_));
          return false;
        }
      } else if (strlen(start) > 3 && start[0] == '#' && start[1] == '_') {
        // Annotation.
        // We don't actually verify anything here.
        char* next_space = strchr(start + 3, ' ');
        if (next_space) {
          // Looks legit.
          std::string key(start + 3, next_space);
          std::string value(next_space + 1);
          while (value.find_last_of(" \t\n") == value.size() - 1) {
            value.erase(value.end() - 1);
          }
          if (!current_test_case) {
            XELOGE("Annotation outside of test case in {}",
                   xe::path_to_utf8(src_file_path_));
            return false;
          }
          current_test_case->annotations.emplace_back(key, value);
        }
      }
    }
    fclose(f);
    return true;
  }
};

class TestRunner {
 public:
  TestRunner() : memory_size_(64_MiB) {
    memory_.reset(new Memory());
    memory_->Initialize();
  }

  ~TestRunner() {
    thread_state_.reset();
    processor_.reset();
    memory_.reset();
  }

  bool Setup(TestSuite& suite) {
    // Reset memory.
    memory_->Reset();

    std::unique_ptr<xe::cpu::backend::Backend> backend;
    if (!backend) {
#if XE_ARCH_AMD64
      if (cvars::cpu == "x64") {
        backend.reset(new xe::cpu::backend::x64::X64Backend());
      }
#endif  // XE_ARCH
      if (cvars::cpu == "any") {
        if (!backend) {
#if XE_ARCH_AMD64
          backend.reset(new xe::cpu::backend::x64::X64Backend());
#endif  // XE_ARCH
        }
      }
    }

    // Setup a fresh processor.
    processor_.reset(new Processor(memory_.get(), nullptr));
    processor_->Setup(std::move(backend));
    processor_->set_debug_info_flags(DebugInfoFlags::kDebugInfoAll);

    // Load the binary module.
    auto module = std::make_unique<xe::cpu::RawModule>(processor_.get());
    if (!module->LoadFile(START_ADDRESS, suite.bin_file_path())) {
      XELOGE("Unable to load test binary {}",
             xe::path_to_utf8(suite.bin_file_path()));
      return false;
    }
    processor_->AddModule(std::move(module));

    processor_->backend()->CommitExecutableRange(START_ADDRESS,
                                                 START_ADDRESS + 1024 * 1024);

    // Add dummy space for memory.
    processor_->memory()->LookupHeap(0)->AllocFixed(
        0x10001000, 0xEFFF, 0,
        kMemoryAllocationReserve | kMemoryAllocationCommit,
        kMemoryProtectRead | kMemoryProtectWrite);

    // Simulate a thread.
    uint32_t stack_size = 64 * 1024;
    uint32_t stack_address = START_ADDRESS - stack_size;
    uint32_t pcr_address = stack_address - 0x1000;
    thread_state_.reset(
        new ThreadState(processor_.get(), 0x100, stack_address, pcr_address));

    return true;
  }

  bool Run(TestCase& test_case) {
    // Setup test state from annotations.
    if (!SetupTestState(test_case)) {
      XELOGE("Test setup failed");
      return false;
    }

    // Execute test.
    auto fn = processor_->ResolveFunction(test_case.address);
    if (!fn) {
      XELOGE("Entry function not found");
      return false;
    }

    auto ctx = thread_state_->context();
    ctx->lr = 0xBCBCBCBC;
    fn->Call(thread_state_.get(), uint32_t(ctx->lr));

    // Assert test state expectations.
    bool result = CheckTestResults(test_case);
    if (!result) {
      // Also dump all disasm/etc.
      if (fn->is_guest()) {
        static_cast<xe::cpu::GuestFunction*>(fn)->debug_info()->Dump();
      }
    }

    return result;
  }

  bool SetupTestState(TestCase& test_case) {
    auto ppc_context = thread_state_->context();
    for (auto& it : test_case.annotations) {
      if (it.first == "REGISTER_IN") {
        size_t space_pos = it.second.find(" ");
        auto reg_name = it.second.substr(0, space_pos);
        auto reg_value = it.second.substr(space_pos + 1);
        ppc_context->SetRegFromString(reg_name.c_str(), reg_value.c_str());
      } else if (it.first == "MEMORY_IN") {
        size_t space_pos = it.second.find(" ");
        auto address_str = it.second.substr(0, space_pos);
        auto bytes_str = it.second.substr(space_pos + 1);
        uint32_t address = std::strtoul(address_str.c_str(), nullptr, 16);
        auto p = memory_->TranslateVirtual(address);
        const char* c = bytes_str.c_str();
        while (*c) {
          while (*c == ' ') ++c;
          if (!*c) {
            break;
          }
          char ccs[3] = {c[0], c[1], 0};
          c += 2;
          uint32_t b = std::strtoul(ccs, nullptr, 16);
          *p = static_cast<uint8_t>(b);
          ++p;
        }
      }
    }
    return true;
  }

  bool CheckTestResults(TestCase& test_case) {
    auto ppc_context = thread_state_->context();

    bool any_failed = false;
    for (auto& it : test_case.annotations) {
      if (it.first == "REGISTER_OUT") {
        size_t space_pos = it.second.find(" ");
        auto reg_name = it.second.substr(0, space_pos);
        auto reg_value = it.second.substr(space_pos + 1);
        std::string actual_value;
        if (!ppc_context->CompareRegWithString(
                reg_name.c_str(), reg_value.c_str(), actual_value)) {
          any_failed = true;
          XELOGE("Register {} assert failed:\n", reg_name);
          XELOGE("  Expected: {} == {}\n", reg_name, reg_value);
          XELOGE("    Actual: {} == {}\n", reg_name, actual_value);
        }
      } else if (it.first == "MEMORY_OUT") {
        size_t space_pos = it.second.find(" ");
        auto address_str = it.second.substr(0, space_pos);
        auto bytes_str = it.second.substr(space_pos + 1);
        uint32_t address = std::strtoul(address_str.c_str(), nullptr, 16);
        auto base_address = memory_->TranslateVirtual(address);
        auto p = base_address;
        const char* c = bytes_str.c_str();
        bool failed = false;
        size_t count = 0;
        StringBuffer expecteds;
        StringBuffer actuals;
        while (*c) {
          while (*c == ' ') ++c;
          if (!*c) {
            break;
          }
          char ccs[3] = {c[0], c[1], 0};
          c += 2;
          count++;
          uint32_t current_address =
              address + static_cast<uint32_t>(p - base_address);
          uint32_t expected = std::strtoul(ccs, nullptr, 16);
          uint8_t actual = *p;

          expecteds.AppendFormat(" {:02X}", expected);
          actuals.AppendFormat(" {:02X}", actual);

          if (expected != actual) {
            any_failed = true;
            failed = true;
          }
          ++p;
        }
        if (failed) {
          XELOGE("Memory {} assert failed:\n", address_str);
          XELOGE("  Expected:{}\n", expecteds.to_string());
          XELOGE("    Actual:{}\n", actuals.to_string());
        }
      }
    }
    return !any_failed;
  }

  size_t memory_size_;
  std::unique_ptr<Memory> memory_;
  std::unique_ptr<Processor> processor_;
  std::unique_ptr<ThreadState> thread_state_;
};

bool DiscoverTests(const std::filesystem::path& test_path,
                   std::vector<std::filesystem::path>& test_files) {
  auto file_infos = xe::filesystem::ListFiles(test_path);
  for (auto& file_info : file_infos) {
    if (file_info.name.extension() == ".s") {
      test_files.push_back(test_path / file_info.name);
    }
  }
  return true;
}

#if XE_COMPILER_MSVC
int filter(unsigned int code) {
  if (code == EXCEPTION_ILLEGAL_INSTRUCTION) {
    return EXCEPTION_EXECUTE_HANDLER;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}
#endif  // XE_COMPILER_MSVC

void ProtectedRunTest(TestSuite& test_suite, TestRunner& runner,
                      TestCase& test_case, int& failed_count,
                      int& passed_count) {
#if XE_COMPILER_MSVC
  __try {
#endif  // XE_COMPILER_MSVC

    if (!runner.Setup(test_suite)) {
      XELOGE("    TEST FAILED SETUP");
      ++failed_count;
    }
    if (runner.Run(test_case)) {
      ++passed_count;
    } else {
      XELOGE("    TEST FAILED");
      ++failed_count;
    }

#if XE_COMPILER_MSVC
  } __except (filter(GetExceptionCode())) {
    XELOGE("    TEST FAILED (UNSUPPORTED INSTRUCTION)");
    ++failed_count;
  }
#endif  // XE_COMPILER_MSVC
}

// Built-in check that Processor::RegisterGuestFunctionOverride intercepts
// INDIRECT calls (bctrl through a function pointer or vtable), not only direct
// bl calls. It needs a host handler, which a .s suite cannot express.
namespace guest_override_test {
constexpr uint32_t kBase = 0x82000000;
constexpr uint32_t kCallee = kBase + 0x00;           // li r3, 1; blr
constexpr uint32_t kIndirectCaller = kBase + 0x10;   // via ctr + bctrl
constexpr uint32_t kDirectCaller = kBase + 0x40;     // via bl
constexpr uint32_t kFunctionPointer = kBase + 0x60;  // holds kCallee
const uint32_t kCode[] = {
    // kCallee
    0x38600001,  // li r3, 1
    0x4E800020,  // blr
    0x60000000,
    0x60000000,
    // kIndirectCaller: the target is loaded from memory (r4 points at
    // kFunctionPointer), so the JIT cannot fold the bctrl into a direct call.
    0x7D8802A6,  // mflr r12
    0x81640000,  // lwz r11, 0(r4)
    0x7D6903A6,  // mtctr r11
    0x4E800421,  // bctrl
    0x7D8803A6,  // mtlr r12
    0x4E800020,  // blr
    0x60000000,
    0x60000000,
    0x60000000,
    0x60000000,
    0x60000000,
    0x60000000,
    // kDirectCaller
    0x7D8802A6,  // mflr r12
    0x4BFFFFBD,  // bl kCallee
    0x7D8803A6,  // mtlr r12
    0x4E800020,  // blr
    0x60000000,
    0x60000000,
    0x60000000,
    0x60000000,
    // kFunctionPointer
    kCallee,
};

void OverrideHandler(ppc::PPCContext* ctx, kernel::KernelState*) {
  ctx->r[3] = 42;
}

// Runs `entry` on a fresh processor (optionally with the callee overridden)
// and returns r3, or ~0 on setup failure.
uint64_t RunOnce(uint32_t entry, bool override_callee) {
  auto memory = std::make_unique<Memory>();
  memory->Initialize();
  std::unique_ptr<xe::cpu::backend::Backend> backend;
#if XE_ARCH_AMD64
  backend.reset(new xe::cpu::backend::x64::X64Backend());
#endif  // XE_ARCH
  if (!backend) {
    return ~0ull;
  }
  auto processor = std::make_unique<Processor>(memory.get(), nullptr);
  processor->Setup(std::move(backend));

  auto bin_path = std::filesystem::temp_directory_path() /
                  fmt::format("xenia_guest_override_{}.bin",
                              xe::Clock::QueryHostTickCount());
  {
    std::vector<uint8_t> bytes;
    for (uint32_t word : kCode) {
      for (int shift = 24; shift >= 0; shift -= 8) {
        bytes.push_back(uint8_t(word >> shift));
      }
    }
    FILE* f = xe::filesystem::OpenFile(bin_path, "wb");
    if (!f) {
      return ~0ull;
    }
    fwrite(bytes.data(), 1, bytes.size(), f);
    fclose(f);
  }
  auto module = std::make_unique<xe::cpu::RawModule>(processor.get());
  bool loaded = module->LoadFile(kBase, bin_path);
  std::filesystem::remove(bin_path);
  if (!loaded) {
    return ~0ull;
  }
  processor->AddModule(std::move(module));
  processor->backend()->CommitExecutableRange(kBase, kBase + 0x10000);

  if (override_callee) {
    processor->RegisterGuestFunctionOverride(kCallee, &OverrideHandler,
                                             "override_test_callee");
  }

  // As in TestRunner: the test code never touches the guest stack.
  uint32_t stack_size = 64 * 1024;
  uint32_t stack_address = kBase - stack_size;
  auto thread_state = std::make_unique<ThreadState>(
      processor.get(), 0x100, stack_address, stack_address - 0x1000);
  auto fn = processor->ResolveFunction(entry);
  if (!fn) {
    return ~0ull;
  }
  auto ctx = thread_state->context();
  ctx->r[3] = 0;
  ctx->r[4] = kFunctionPointer;
  ctx->lr = 0xBCBCBCBC;
  fn->Call(thread_state.get(), uint32_t(ctx->lr));
  uint64_t r3 = ctx->r[3];
  thread_state.reset();
  processor.reset();
  return r3;
}

void Run(int& failed_count, int& passed_count) {
  struct Case {
    const char* name;
    uint32_t entry;
    bool override_callee;
    uint64_t expected;
  };
  const Case cases[] = {
      {"control: indirect call runs the guest body", kIndirectCaller, false, 1},
      {"override intercepts a direct call", kDirectCaller, true, 42},
      {"override intercepts an indirect call", kIndirectCaller, true, 42},
  };
  XELOGI("guest_function_override (built-in):");
  for (const auto& c : cases) {
    XELOGI("  - {}", c.name);
    uint64_t r3 = RunOnce(c.entry, c.override_callee);
    if (r3 == c.expected) {
      ++passed_count;
    } else {
      XELOGE("    TEST FAILED: r3 = {:X}, expected {:X}", r3, c.expected);
      ++failed_count;
    }
  }
  XELOGI("");
}
}  // namespace guest_override_test

// Built-in check that guest 128-bit vector loads/stores (lvx/stvx) to an
// MMIO range reach the range's callbacks as four 32-bit register accesses.
// The XMA HAL kicks contexts with one stvx128 to the Kick registers; before
// the MMIO handler decoded VEX vector moves, that store could not be emulated.
namespace mmio_vector_test {
constexpr uint32_t kCodeBase = 0x82000000;
constexpr uint32_t kMmioBase = 0x7FEA0000;
constexpr uint32_t kMmioTarget = kMmioBase + 0x1940;
const uint32_t kCode[] = {
    // kCodeBase: store v1 to [r3] (= kMmioTarget), load it back into v2.
    0x38800000,  // li r4, 0
    0x7C2321CE,  // stvx v1, r3, r4
    0x7C4320CE,  // lvx v2, r3, r4
    0x4E800020,  // blr
};

struct Recorder {
  std::vector<std::pair<uint32_t, uint32_t>> writes;
  std::vector<uint32_t> reads;
};

uint32_t ReadCallback(void*, void* context, uint32_t addr) {
  auto* recorder = static_cast<Recorder*>(context);
  recorder->reads.push_back(addr);
  return 0xA0000000u | (addr & 0xFFFF);
}
void WriteCallback(void*, void* context, uint32_t addr, uint32_t value) {
  static_cast<Recorder*>(context)->writes.emplace_back(addr, value);
}

bool RunOnce(Recorder& recorder, vec128_t& v2_out) {
  auto memory = std::make_unique<Memory>();
  memory->Initialize();
  std::unique_ptr<xe::cpu::backend::Backend> backend;
#if XE_ARCH_AMD64
  backend.reset(new xe::cpu::backend::x64::X64Backend());
#endif  // XE_ARCH
  if (!backend) {
    return false;
  }
  auto processor = std::make_unique<Processor>(memory.get(), nullptr);
  processor->Setup(std::move(backend));
  if (!memory->AddVirtualMappedRange(kMmioBase, 0xFFFF0000, 0xFFFF, &recorder,
                                     &ReadCallback, &WriteCallback)) {
    return false;
  }
  auto bin_path =
      std::filesystem::temp_directory_path() /
      fmt::format("xenia_mmio_vector_{}.bin", xe::Clock::QueryHostTickCount());
  {
    std::vector<uint8_t> bytes;
    for (uint32_t word : kCode) {
      for (int shift = 24; shift >= 0; shift -= 8) {
        bytes.push_back(uint8_t(word >> shift));
      }
    }
    FILE* f = xe::filesystem::OpenFile(bin_path, "wb");
    if (!f) {
      return false;
    }
    fwrite(bytes.data(), 1, bytes.size(), f);
    fclose(f);
  }
  auto module = std::make_unique<xe::cpu::RawModule>(processor.get());
  bool loaded = module->LoadFile(kCodeBase, bin_path);
  std::filesystem::remove(bin_path);
  if (!loaded) {
    return false;
  }
  processor->AddModule(std::move(module));
  processor->backend()->CommitExecutableRange(kCodeBase, kCodeBase + 0x10000);
  uint32_t stack_address = kCodeBase - 64 * 1024;
  auto thread_state = std::make_unique<ThreadState>(
      processor.get(), 0x100, stack_address, stack_address - 0x1000);
  auto fn = processor->ResolveFunction(kCodeBase);
  if (!fn) {
    return false;
  }
  auto ctx = thread_state->context();
  ctx->r[3] = kMmioTarget;
  ctx->v[1] = vec128i(0x11111111, 0x22222222, 0x33333333, 0x44444444);
  ctx->lr = 0xBCBCBCBC;
  fn->Call(thread_state.get(), uint32_t(ctx->lr));
  v2_out = ctx->v[2];
  thread_state.reset();
  processor.reset();
  return true;
}

void Run(int& failed_count, int& passed_count) {
  XELOGI("mmio_vector (built-in):");
  Recorder recorder;
  vec128_t v2 = {};
  bool ran = RunOnce(recorder, v2);
  const std::vector<std::pair<uint32_t, uint32_t>> want_writes = {
      {kMmioTarget + 0x0, 0x11111111},
      {kMmioTarget + 0x4, 0x22222222},
      {kMmioTarget + 0x8, 0x33333333},
      {kMmioTarget + 0xC, 0x44444444}};
  XELOGI("  - stvx to MMIO = four 32-bit register writes, in order");
  if (ran && recorder.writes == want_writes) {
    ++passed_count;
  } else {
    XELOGE("    TEST FAILED: {} writes recorded", recorder.writes.size());
    for (auto& w : recorder.writes) {
      XELOGE("      {:08X} <- {:08X}", w.first, w.second);
    }
    ++failed_count;
  }
  XELOGI("  - lvx from MMIO = four 32-bit register reads into the vector");
  bool reads_ok = ran && recorder.reads.size() == 4;
  for (uint32_t lane = 0; reads_ok && lane < 4; ++lane) {
    reads_ok =
        recorder.reads[lane] == kMmioTarget + lane * 4 &&
        v2.u32[lane] == (0xA0000000u | ((kMmioTarget + lane * 4) & 0xFFFF));
  }
  if (reads_ok) {
    ++passed_count;
  } else {
    XELOGE("    TEST FAILED: {} reads; v2 = {:08X} {:08X} {:08X} {:08X}",
           recorder.reads.size(), v2.u32[0], v2.u32[1], v2.u32[2], v2.u32[3]);
    ++failed_count;
  }
  XELOGI("");
}
}  // namespace mmio_vector_test

bool RunTests(const std::string_view test_name) {
  int result_code = 1;
  int failed_count = 0;
  int passed_count = 0;

#if XE_ARCH_AMD64
  XELOGI("Instruction feature mask {}.", cvars::x64_extension_mask);
#endif  // XE_ARCH_AMD64

  auto test_path_root = cvars::test_path;
  std::vector<std::filesystem::path> test_files;
  if (!DiscoverTests(test_path_root, test_files)) {
    return false;
  }
  if (!test_files.size()) {
    XELOGE("No tests discovered - invalid path?");
    return false;
  }
  XELOGI("{} tests discovered.", test_files.size());
  XELOGI("");

  std::vector<TestSuite> test_suites;
  bool load_failed = false;
  for (auto& test_path : test_files) {
    TestSuite test_suite(test_path);
    if (!test_name.empty() && test_suite.name() != test_name) {
      continue;
    }
    if (!test_suite.Load()) {
      XELOGE("TEST SUITE {} FAILED TO LOAD", xe::path_to_utf8(test_path));
      load_failed = true;
      continue;
    }
    test_suites.push_back(std::move(test_suite));
  }
  if (load_failed) {
    XELOGE("One or more test suites failed to load.");
  }

  XELOGI("{} tests loaded.", test_suites.size());
  // Before TestRunner: Memory is a process-wide singleton, and this test
  // builds (and tears down) its own.
  if (test_name.empty() || test_name == "guest_function_override") {
    guest_override_test::Run(failed_count, passed_count);
  }
  if (test_name.empty() || test_name == "mmio_vector") {
    mmio_vector_test::Run(failed_count, passed_count);
  }
  TestRunner runner;
  for (auto& test_suite : test_suites) {
    XELOGI("{}.s:", test_suite.name());

    for (auto& test_case : test_suite.test_cases()) {
      XELOGI("  - {}", test_case.name);
      ProtectedRunTest(test_suite, runner, test_case, failed_count,
                       passed_count);
    }

    XELOGI("");
  }

  XELOGI("");
  XELOGI("Total tests: {}", failed_count + passed_count);
  XELOGI("Passed: {}", passed_count);
  XELOGI("Failed: {}", failed_count);

  return failed_count ? false : true;
}

int main(const std::vector<std::string>& args) {
  return RunTests(cvars::test_name) ? 0 : 1;
}

}  // namespace test
}  // namespace cpu
}  // namespace xe

XE_DEFINE_CONSOLE_APP("xenia-cpu-ppc-test", xe::cpu::test::main, "[test name]",
                      "test_name");
