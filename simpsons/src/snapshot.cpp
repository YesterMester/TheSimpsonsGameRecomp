// Exact saves (development): the whole game, saved at a pause and put back
// under the running game.
//
// An exact save keeps the game's memory (every heap's page table and committed
// pages), each guest thread's registers, and the runtime's state around them.
// It is put back while the game is frozen at the same point: the main thread at
// the start of a frame, every other guest thread parked in the same kernel
// wait as when the save was made. The recompiled code keeps all guest
// registers in the thread's context and its frames on the guest stack, so a
// parked thread given the saved memory and registers carries on as the saved
// one would have.
//
// 'info LABEL' logs the threads and where they wait, kernel objects, open
// files and the memory a save would hold (hashed per 64 KB). 'save NAME' and
// 'load NAME' keep and put back a snapshot in this process.

#include "snapshot.h"

#include "host_memory.h"
#include "native_ragdoll_world.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include <snappy.h>

#include <rex/audio/audio_system.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/graphics/graphics_system.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/platform.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/stream.h>
#include <rex/system/util/object_table.h>
#include <rex/system/xevent.h>
#include <rex/system/xfile.h>
#include <rex/system/xthread.h>
#include <rex/thread/mutex.h>

#if REX_PLATFORM_WIN32
#include <windows.h>
#else
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include "guest_memory.h"

REXCVAR_DEFINE_STRING(snapshot_test_file, "", "Game",
                      "Development: exact save commands read from this file ('info LABEL', "
                      "'save NAME', 'load NAME')");

// The game's graphics interrupt callback (registered with
// VdSetGraphicsInterruptCallback), run by the runtime's vblank and command
// processor threads.
REX_EXTERN(__imp__sub_82456830);

namespace snapshot {
namespace {

using simpsons::LoadGuestU32;

rex::Runtime* g_runtime = nullptr;

const char* TypeName(rex::system::XObject::Type type) {
  using T = rex::system::XObject::Type;
  switch (type) {
    case T::Enumerator:
      return "enumerator";
    case T::Event:
      return "event";
    case T::File:
      return "file";
    case T::IOCompletion:
      return "io-completion";
    case T::Module:
      return "module";
    case T::Mutant:
      return "mutant";
    case T::NotifyListener:
      return "notify";
    case T::Semaphore:
      return "semaphore";
    case T::Session:
      return "session";
    case T::Socket:
      return "socket";
    case T::SymbolicLink:
      return "symlink";
    case T::Thread:
      return "thread";
    case T::Timer:
      return "timer";
    default:
      return "other";
  }
}

// Whether a guest word can be read without faulting: committed and readable.
bool Readable(uint32_t address) {
  auto* heap = g_runtime->memory()->LookupHeap(address);
  rex::memory::HeapAllocationInfo info{};
  return heap && heap->QueryRegionInfo(address & ~0xFFFu, &info) &&
         (info.state & rex::memory::kMemoryAllocationCommit) &&
         (info.protect & rex::memory::kMemoryProtectRead);
}

// The return addresses on a parked thread's guest stack: the call it is in
// (lr), then each caller's saved LR, 8 bytes below its frame's back chain.
std::string Chain(const uint8_t* base, const PPCContext& context) {
  std::string chain = fmt::format("{:08X}", uint32_t(context.lr));
  uint32_t frame = context.r1.u32;
  for (int depth = 0; depth < 24; ++depth) {
    if (frame < 0x10000 || (frame & 3) || !Readable(frame))
      break;
    const uint32_t back = LoadGuestU32(base, frame);
    if (back <= frame || back - frame > 0x100000 || !Readable(back - 8))
      break;
    chain += fmt::format(" {:08X}", LoadGuestU32(base, back - 8));
    frame = back;
  }
  return chain;
}

void Info(const uint8_t* base, const std::string& label, const std::string& out_prefix) {
  auto* kernel = g_runtime->kernel_state();
  auto* table = kernel->object_table();
  const uint32_t current = rex::system::XThread::GetCurrentThreadId();
  auto threads = table->GetObjectsByType<rex::system::XThread>();
  std::sort(threads.begin(), threads.end(),
            [](const auto& a, const auto& b) { return a->thread_id() < b->thread_id(); });
  REXLOG_INFO("[snapshot] {}: {} threads", label, threads.size());
  for (const auto& thread : threads) {
    const auto* params = thread->creation_params();
    const auto* state = thread->thread_state();
    const PPCContext* context = state ? state->context() : nullptr;
    REXLOG_INFO("[snapshot]   thread {} '{}' handle {:08X} {}{}{} start {:08X}({:08X}) r1 {:08X}",
                thread->thread_id(), thread->name(), thread->handle(),
                thread->is_guest_thread() ? "guest" : "host", thread->main_thread() ? " main" : "",
                thread->thread_id() == current ? " (this)" : "", params->start_address,
                params->start_context, context ? context->r1.u32 : 0);
    if (context && thread->is_guest_thread())
      REXLOG_INFO("[snapshot]     chain {}", Chain(base, *context));
  }
  std::map<std::string, int> types;
  for (const auto& object : table->GetAllObjects())
    ++types[TypeName(object->type())];
  std::string summary;
  for (const auto& [type, count] : types)
    summary += fmt::format(" {} {}", type, count);
  REXLOG_INFO("[snapshot] {}: objects{}", label, summary);
  for (const auto& file : table->GetObjectsByType<rex::system::XFile>())
    REXLOG_INFO("[snapshot]   file {:08X} {} at {}", file->handle(), file->path(),
                file->position());

  // The memory a save would hold: every committed page of the guest heaps
  // (the physical views at 0xA0000000 and up share the physical heap's pages).
  auto* memory = g_runtime->memory();
  struct Heap {
    const char* name;
    rex::memory::BaseHeap* heap;
  };
  const Heap heaps[] = {{"v00000000", memory->LookupHeap(0x00010000)},
                        {"v40000000", memory->LookupHeap(0x40000000)},
                        {"v80000000", memory->LookupHeap(0x80000000)},
                        {"v90000000", memory->LookupHeap(0x90000000)},
                        {"physical", memory->GetPhysicalHeap()}};
  FILE* hashes = std::fopen((out_prefix + "." + label + ".hashes").c_str(), "w");
  uint64_t total = 0;
  for (const auto& [name, heap] : heaps) {
    if (!heap)
      continue;
    uint64_t committed = 0;
    uint32_t regions = 0;
    for (uint64_t relative = 0; relative < heap->heap_size();) {
      rex::memory::HeapAllocationInfo info{};
      if (!heap->QueryRegionInfo(heap->heap_base() + uint32_t(relative), &info) ||
          !info.region_size)
        break;
      if ((info.state & rex::memory::kMemoryAllocationCommit) &&
          (info.protect & rex::memory::kMemoryProtectRead)) {
        committed += info.region_size;
        ++regions;
        if (hashes) {
          for (uint64_t offset = 0; offset < info.region_size; offset += 65536) {
            const uint64_t size = std::min<uint64_t>(65536, info.region_size - offset);
            const uint8_t* data = heap->TranslateRelative(relative + offset);
            std::fprintf(hashes, "%s %08" PRIx64 " %016" PRIx64 "\n", name,
                         uint64_t(heap->heap_base()) + relative + offset,
                         uint64_t(XXH3_64bits(data, size)));
          }
        }
      }
      relative += info.region_size;
    }
    total += committed;
    REXLOG_INFO("[snapshot] {}: heap {} committed {:.1f} MB in {} regions", label, name,
                committed / 1048576.0, regions);
  }
  if (hashes)
    std::fclose(hashes);
  REXLOG_INFO("[snapshot] {}: {:.1f} MB committed in all", label, total / 1048576.0);
}

// --- Freezing the game ----------------------------------------------------

// While frozen, the game's interrupt callback is skipped.
std::mutex g_gate_mutex;
std::condition_variable g_gate_cv;
bool g_frozen = false;
int g_in_interrupt = 0;

rex::graphics::GraphicsSystem* Graphics() {
  return dynamic_cast<rex::graphics::GraphicsSystem*>(g_runtime->graphics_system());
}
rex::audio::AudioSystem* Audio() {
  return dynamic_cast<rex::audio::AudioSystem*>(g_runtime->audio_system());
}

using ThreadRef = rex::system::object_ref<rex::system::XThread>;

// Development: what Freeze is doing, for a watchdog that reports a step
// that never finishes, and progress written straight to stderr (the log can
// still be buffered when something hangs).
std::atomic<const char*> g_freeze_step{nullptr};

// Progress goes to stderr only in development runs (snapshot_test_file).
bool Verbose() {
  return !REXCVAR_GET(snapshot_test_file).empty();
}

void Trace(const char* text) {
  if (!Verbose())
    return;
  std::fprintf(stderr, "[snapshot] %s\n", text);
  std::fflush(stderr);
}

void Step(const char* step) {
  g_freeze_step = step;
  if (!Verbose())
    return;
  std::fprintf(stderr, "[snapshot] freeze: %s\n", step ? step : "done");
  std::fflush(stderr);
}

// Stops everything that runs guest code or touches guest memory, except the
// calling (main) thread: the GPU's command processor, audio, the interrupt
// callback, and every other guest thread, each blocked in a kernel wait or
// parked as its wait returns (XThread::WaitScope). Fails if a guest thread
// keeps running guest code; then nothing stays frozen.
// Audio pauses once its worker is between the game's callbacks, and a callback
// can be waiting on the main thread until later in its frame: pausing it from
// the main thread at a frame start can wait forever. So it is asked of another
// thread (Ready), the game runs on for a frame or two without audio, and the
// freeze happens at a frame start once it has paused.
std::thread g_audio_pausing;
std::atomic<bool> g_audio_paused{false};
bool g_audio_asked = false;

bool AudioReady() {
  auto* audio = Audio();
  if (!audio || g_audio_paused.load())
    return true;
  if (!g_audio_asked) {
    g_audio_asked = true;
    g_audio_pausing = std::thread([audio] {
      audio->Pause();
      g_audio_paused = true;
    });
  }
  return g_audio_paused.load();
}

// Audio plays again, once its pause has happened (resuming before then would
// leave the pause waiting for good).
void AudioResume() {
  if (!g_audio_asked || !g_audio_paused.load())
    return;
  if (g_audio_pausing.joinable())
    g_audio_pausing.join();
  if (auto* audio = Audio())
    audio->Resume();
  g_audio_paused = false;
  g_audio_asked = false;
}

bool Freeze(std::vector<ThreadRef>& threads) {
  if (!AudioReady()) {
    REXLOG_WARN("[snapshot] audio isn't paused yet: no freeze this frame");
    return false;
  }
  std::thread watchdog([] {
    const char* last = nullptr;
    for (int i = 0; i < 200; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      const char* step = g_freeze_step.load();
      if (!step)
        return;
      if (i == 40 || (i > 40 && step != last))
        REXLOG_WARN("[snapshot] freeze stuck in: {}", step);
      last = step;
    }
  });
  Step("pausing the GPU");
  if (auto* graphics = Graphics())
    graphics->Pause();
  Step("closing the interrupt gate");
  {
    std::unique_lock lock(g_gate_mutex);
    g_frozen = true;
    g_gate_cv.wait(lock, [] { return g_in_interrupt == 0; });
  }
  Step("parking guest threads");
  rex::system::XThread::RequestFreeze();
  std::vector<uint32_t> busy;
  bool parked = false;
  for (int i = 0; i < 300 && !parked; ++i) {
    busy.clear();
    parked = rex::system::XThread::AllFrozen(&busy);
    if (!parked)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  // Deferred I/O completions write guest memory and finish reads a saved
  // game would wait on forever: let them all finish, then hold the rest.
  Step("draining deferred I/O");
  bool drained = parked && g_runtime->kernel_state()->PauseDispatch(std::chrono::seconds(3));
  // Nothing else may hold the kernel's global lock either.
  Step("waiting for the global lock");
  bool unlocked = false;
  for (int i = 0; i < 100 && drained && !unlocked; ++i) {
    rex::thread::global_critical_region region;
    unlocked = region.TryAcquire().owns_lock();
    if (!unlocked)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  Step(nullptr);
  watchdog.join();
  threads.clear();
  const uint32_t current = rex::system::XThread::GetCurrentThreadId();
  for (auto& thread :
       g_runtime->kernel_state()->object_table()->GetObjectsByType<rex::system::XThread>())
    if (thread->is_guest_thread() && thread->thread_id() != current && thread->is_running())
      threads.push_back(thread);
  if (parked && drained && unlocked)
    return true;
  std::string ids;
  for (const uint32_t id : busy)
    ids += fmt::format(" {}", id);
  const char* why = !parked    ? "threads still running:"
                    : !drained ? "deferred I/O did not finish"
                               : "the global lock stayed held";
  std::fprintf(stderr, "[snapshot] could not freeze: %s%s\n", why, ids.c_str());
  std::fflush(stderr);
  REXLOG_WARN("[snapshot] could not freeze the game ({}{})", why, ids);
  if (drained)
    g_runtime->kernel_state()->ResumeDispatch();
  rex::system::XThread::ReleaseFreeze();
  {
    std::lock_guard lock(g_gate_mutex);
    g_frozen = false;
  }
  AudioResume();
  if (auto* graphics = Graphics())
    graphics->Resume();
  return false;
}

void Thaw(std::vector<ThreadRef>&) {
  g_runtime->kernel_state()->ResumeDispatch();
  rex::system::XThread::ReleaseFreeze();
  {
    std::lock_guard lock(g_gate_mutex);
    g_frozen = false;
  }
  AudioResume();
  if (auto* graphics = Graphics())
    graphics->Resume();
}

// --- Snapshots ---------------------------------------------------------------

struct HeapSpec {
  const char* name;
  uint32_t address;  // an address in the heap; physical means the physical heap
  bool data;         // whether its pages are saved (the 0xA0000000+ views share
                     // the physical heap's pages; only their page tables are)
};
constexpr HeapSpec kHeaps[] = {
    {"v00000000", 0x00010000, true}, {"v40000000", 0x40000000, true},
    {"v80000000", 0x80000000, true}, {"v90000000", 0x90000000, true},
    {"physical", 0, true},           {"vA0000000", 0xA0000000, false},
    {"vC0000000", 0xC0000000, false}, {"vE0000000", 0xE0000000, false}};

rex::memory::BaseHeap* HeapOf(const HeapSpec& spec) {
  auto* memory = g_runtime->memory();
  return std::string_view(spec.name) == "physical" ? memory->GetPhysicalHeap()
                                                    : memory->LookupHeap(spec.address);
}

struct SavedThread {
  uint32_t id = 0;
  bool main = false;
  uint32_t start_address = 0;
  PPCContext context{};
};

// A kernel object, by its handles; the types the runtime can rebuild keep
// their own serialized state (XObject::Save).
struct SavedObject {
  uint32_t type = 0;
  std::vector<uint32_t> handles;
  std::vector<int32_t> refs;  // each handle's open count
  std::vector<uint8_t> blob;
  std::string path;  // files
  uint64_t position = 0;
  uint32_t signaled = 0;  // events
  uint32_t thread_id = 0;     // threads
  uint32_t guest_object = 0;  // objects that live in guest memory
  std::string name;
};

struct Snapshot {
  std::vector<std::vector<rex::memory::PageEntry>> pages;  // per heap, as kHeaps
  std::vector<uint8_t> data;  // the data heaps' committed pages, in order
  std::vector<SavedThread> threads;
  std::vector<uint8_t> gpu;  // the command processor's ring state
  std::vector<SavedObject> objects;
};
std::map<std::string, Snapshot> g_snapshots;

bool Committed(const rex::memory::PageEntry& page) {
  return page.state & rex::memory::kMemoryAllocationCommit;
}

// The host's protection of the guest's memory, from one read of the process's
// memory map: ranges of host addresses that can be read (and written).
struct HostAccess {
  struct Range {
    uintptr_t start, end;
    bool read, write;
  };
  std::vector<Range> ranges;
#if REX_PLATFORM_WIN32
  // Windows asks the memory manager as it goes.
  bool Allows(const uint8_t* address, size_t size, bool write) const {
    const uint8_t* at = address;
    const uint8_t* const end = address + size;
    while (at < end) {
      MEMORY_BASIC_INFORMATION info;
      if (!VirtualQuery(at, &info, sizeof(info)) || info.State != MEM_COMMIT ||
          (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        return false;
      const DWORD protect = info.Protect & 0xFF;
      const bool writable = protect == PAGE_READWRITE || protect == PAGE_EXECUTE_READWRITE ||
                            protect == PAGE_WRITECOPY || protect == PAGE_EXECUTE_WRITECOPY;
      const bool readable = writable || protect == PAGE_READONLY || protect == PAGE_EXECUTE_READ;
      if (!readable || (write && !writable))
        return false;
      at = static_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize;
    }
    return true;
  }
#else
  HostAccess() {
    FILE* maps = std::fopen("/proc/self/maps", "r");
    if (!maps)
      return;
    char line[512];
    while (std::fgets(line, sizeof(line), maps)) {
      unsigned long long start = 0, end = 0;
      char perms[8] = {};
      if (std::sscanf(line, "%llx-%llx %7s", &start, &end, perms) == 3)
        ranges.push_back({uintptr_t(start), uintptr_t(end), perms[0] == 'r', perms[1] == 'w'});
    }
    std::fclose(maps);
  }
  // Whether all of [address, address + size) can be read (and written).
  bool Allows(const uint8_t* address, size_t size, bool write) const {
    uintptr_t at = uintptr_t(address);
    const uintptr_t end = at + size;
    auto it = std::upper_bound(ranges.begin(), ranges.end(), at,
                               [](uintptr_t a, const Range& r) { return a < r.start; });
    if (it != ranges.begin())
      --it;
    for (; it != ranges.end() && at < end; ++it) {
      if (it->start > at || it->end <= at || !it->read || (write && !it->write))
        return false;
      at = it->end;
    }
    return at >= end;
  }
#endif
};

// Runs of consecutive committed pages in a heap's page table.
template <typename F>
void ForCommittedRuns(rex::memory::BaseHeap* heap, const std::vector<rex::memory::PageEntry>& pages,
                      F&& run) {
  for (size_t i = 0; i < pages.size();) {
    if (!Committed(pages[i])) {
      ++i;
      continue;
    }
    size_t j = i + 1;
    while (j < pages.size() && Committed(pages[j]))
      ++j;
    run(heap->TranslateRelative(i << heap->page_size_shift()), size_t(j - i) * heap->page_size(),
        i, j);
    i = j;
  }
}

void CaptureMemory(Snapshot& snapshot) {
  size_t total = 0;
  for (const auto& spec : kHeaps) {
    auto* heap = HeapOf(spec);
    snapshot.pages.push_back(heap->page_table());
    if (spec.data)
      for (const auto& page : snapshot.pages.back())
        total += Committed(page) ? heap->page_size() : 0;
  }
  snapshot.data.resize(total);
  const HostAccess host;
  size_t offset = 0, protected_runs = 0;
  for (size_t n = 0; n < std::size(kHeaps); ++n) {
    if (!kHeaps[n].data)
      continue;
    auto* heap = HeapOf(kHeaps[n]);
    ForCommittedRuns(heap, snapshot.pages[n], [&](uint8_t* address, size_t size, size_t, size_t) {
      if (host.Allows(address, size, false)) {
        std::memcpy(snapshot.data.data() + offset, address, size);
      } else {
        // Rare (a guard page, a watched range): open it for the copy page by
        // page and put each page's protection back.
        ++protected_runs;
        for (size_t at = 0; at < size; at += heap->page_size()) {
          rex::memory::PageAccess old_access;
          rex::memory::Protect(address + at, heap->page_size(), rex::memory::PageAccess::kReadWrite,
                               &old_access);
          std::memcpy(snapshot.data.data() + offset + at, address + at, heap->page_size());
          rex::memory::Protect(address + at, heap->page_size(), old_access, nullptr);
        }
      }
      offset += size;
    });
  }
  REXLOG_INFO("[snapshot] memory: {:.1f} MB, {} runs needed opening", total / 1048576.0,
              protected_runs);
}

rex::memory::PageAccess GuestAccess(const rex::memory::PageEntry& page) {
  if ((page.current_protect & rex::memory::kMemoryProtectRead) &&
      (page.current_protect & rex::memory::kMemoryProtectWrite))
    return rex::memory::PageAccess::kReadWrite;
  if (page.current_protect & rex::memory::kMemoryProtectRead)
    return rex::memory::PageAccess::kReadOnly;
  return rex::memory::PageAccess::kNoAccess;
}

// The runtime's table of host functions for indirect calls lives in guest
// memory after the image (REX_IMAGE_BASE + REX_IMAGE_SIZE, two bytes per code
// byte plus the thunk reserve). Its host pointers belong to this process, so
// it is never restored.
constexpr uint32_t kFunctionTable = 0x82000000 + 0xEC0000;
constexpr uint32_t kFunctionTableSize = (0xA93CE4 + 0x10000) * 2;

// The memory set aside for the recompilation's own code (host_memory.h) in a
// heap's addresses (physical addresses for the physical heap). It is never
// restored: its allocations are the same in every run of a build, and a save
// only goes back under a game whose are where the save's were (Fits).
struct Range {
  uint64_t start, end;
};
std::vector<Range> Kept(size_t heap_index) {
  auto* heap = HeapOf(kHeaps[heap_index]);
  const bool physical = std::string_view(kHeaps[heap_index].name) == "physical";
  auto* memory = g_runtime->memory();
  std::vector<Range> kept;
  for (const auto& region : host_memory::Regions()) {
    uint64_t start = region.address;
    if (physical) {
      if (region.address < 0xA0000000u)
        continue;
      start = memory->GetPhysicalAddress(region.address);
    } else if (memory->LookupHeap(region.address) != heap) {
      continue;
    }
    kept.push_back({start, start + region.size});
  }
  return kept;
}

bool Excluded(rex::memory::BaseHeap* heap, size_t page, const std::vector<Range>& kept) {
  const uint64_t address = uint64_t(heap->heap_base()) + (uint64_t(page) << heap->page_size_shift());
  const uint64_t end = address + heap->page_size();
  if (end > kFunctionTable && address < uint64_t(kFunctionTable) + kFunctionTableSize)
    return true;
  for (const auto& range : kept)
    if (end > range.start && address < range.end)
      return true;
  return false;
}

// Whether a snapshot's memory is laid out like the running game's where the
// recompilation's own memory is: allocated the same way, by the same build.
bool Fits(const Snapshot& snapshot) {
  if (snapshot.pages.size() != std::size(kHeaps))
    return false;
  for (size_t n = 0; n < std::size(kHeaps); ++n) {
    auto* heap = HeapOf(kHeaps[n]);
    const auto& saved = snapshot.pages[n];
    const auto& live = heap->page_table();
    if (saved.size() != live.size())
      return false;
    for (const auto& range : Kept(n)) {
      for (uint64_t at = range.start; at < range.end; at += heap->page_size()) {
        const size_t i = size_t((at - heap->heap_base()) >> heap->page_size_shift());
        if (i >= live.size())
          return false;
        if (saved[i].base_address != live[i].base_address ||
            saved[i].region_page_count != live[i].region_page_count ||
            saved[i].state != live[i].state)
          return false;
      }
    }
  }
  return true;
}

void RestoreMemory(const Snapshot& snapshot) {
  size_t offset = 0;
  for (size_t n = 0; n < std::size(kHeaps); ++n) {
    auto* heap = HeapOf(kHeaps[n]);
    const auto& pages = snapshot.pages[n];
    auto& live = heap->page_table();
    if (live.size() != pages.size())
      continue;
    const auto kept = Kept(n);
    const auto Excluded = [&](rex::memory::BaseHeap* heap, size_t page) {
      return ::snapshot::Excluded(heap, page, kept);
    };
    // Allocation state as saved, except the function table's and the
    // recompilation's own memory's.
    for (size_t i = 0; i < pages.size(); ++i)
      if (!Excluded(heap, i))
        live[i] = pages[i];
    if (!kHeaps[n].data)
      continue;
    ForCommittedRuns(heap, pages, [&](uint8_t* address, size_t size, size_t first, size_t last) {
      const size_t page_size = heap->page_size();
      for (size_t i = first; i < last;) {
        // Pages to copy: as many in a row as aren't excluded.
        if (Excluded(heap, i)) {
          ++i;
          continue;
        }
        size_t j = i + 1;
        while (j < last && !Excluded(heap, j))
          ++j;
        uint8_t* at = address + (i - first) * page_size;
        const size_t bytes = (j - i) * page_size;
        rex::memory::AllocFixed(at, bytes, rex::memory::AllocationType::kCommit,
                                rex::memory::PageAccess::kReadWrite);
        rex::memory::Protect(at, bytes, rex::memory::PageAccess::kReadWrite, nullptr);
        std::memcpy(at, snapshot.data.data() + offset + (i - first) * page_size, bytes);
        // The guest's own protection, in runs of pages that share it.
        for (size_t k = i; k < j;) {
          const auto access = GuestAccess(pages[k]);
          size_t l = k + 1;
          while (l < j && GuestAccess(pages[l]) == access)
            ++l;
          if (access != rex::memory::PageAccess::kReadWrite)
            rex::memory::Protect(heap->TranslateRelative(k << heap->page_size_shift()),
                                 (l - k) * page_size, access, nullptr);
          k = l;
        }
        i = j;
      }
      offset += size;
    });
  }
}

std::vector<uint8_t> SaveGpu() {
  std::vector<uint8_t> buffer(256);
  rex::stream::ByteStream stream(buffer.data(), buffer.size());
  if (auto* graphics = Graphics())
    graphics->Save(&stream);
  buffer.resize(stream.offset());
  return buffer;
}

using rex::system::XObject;

bool Rebuildable(XObject::Type type) {
  return type == XObject::Type::Event || type == XObject::Type::Semaphore ||
         type == XObject::Type::File || type == XObject::Type::Mutant;
}

void CaptureObjects(Snapshot& snapshot) {
  for (const auto& object : g_runtime->kernel_state()->object_table()->GetAllObjects()) {
    SavedObject saved;
    saved.type = uint32_t(object->type());
    saved.handles = object->handles();
    for (const uint32_t handle : saved.handles)
      saved.refs.push_back(
          g_runtime->kernel_state()->object_table()->HandleRefCount(handle));
    saved.guest_object = object->guest_object();
    saved.name = object->name();
    if (object->type() == XObject::Type::Thread)
      saved.thread_id = static_cast<rex::system::XThread*>(object.get())->thread_id();
    if (object->type() == XObject::Type::File) {
      auto* file = static_cast<rex::system::XFile*>(object.get());
      saved.path = file->path();
      saved.position = file->position();
    } else if (object->type() == XObject::Type::Event) {
      uint32_t type = 0;
      static_cast<rex::system::XEvent*>(object.get())->Query(&type, &saved.signaled);
    }
    if (Rebuildable(object->type())) {
      saved.blob.resize(8192);
      rex::stream::ByteStream stream(saved.blob.data(), saved.blob.size());
      if (object->Save(&stream))
        saved.blob.resize(stream.offset());
      else
        saved.blob.clear();
    }
    snapshot.objects.push_back(std::move(saved));
  }
}

// Pairs saved objects with live ones that are the same thing: a thread by its
// id, an object living in guest memory by its address, a file by its path, a
// named object by its name; the rest by type, in handle order. Returns, per
// saved object, the live object (or null).
std::vector<rex::system::object_ref<XObject>> Match(
    const Snapshot& snapshot, const std::vector<rex::system::object_ref<XObject>>& live) {
  std::vector<rex::system::object_ref<XObject>> matched(snapshot.objects.size());
  std::vector<bool> used(live.size(), false);
  auto pick = [&](size_t s, auto&& same) {
    for (size_t l = 0; l < live.size(); ++l) {
      if (used[l] || uint32_t(live[l]->type()) != snapshot.objects[s].type || !same(*live[l]))
        continue;
      used[l] = true;
      matched[s] = live[l];
      return true;
    }
    return false;
  };
  // Strongest identities first.
  for (size_t s = 0; s < snapshot.objects.size(); ++s) {
    const auto& saved = snapshot.objects[s];
    if (saved.type == uint32_t(XObject::Type::Thread))
      pick(s, [&](XObject& o) {
        return static_cast<rex::system::XThread&>(o).thread_id() == saved.thread_id;
      });
    else if (saved.guest_object)
      pick(s, [&](XObject& o) { return o.guest_object() == saved.guest_object; });
    else if (saved.type == uint32_t(XObject::Type::File))
      pick(s, [&](XObject& o) { return static_cast<rex::system::XFile&>(o).path() == saved.path; });
    else if (!saved.name.empty())
      pick(s, [&](XObject& o) { return o.name() == saved.name; });
  }
  for (size_t s = 0; s < snapshot.objects.size(); ++s) {
    const auto& saved = snapshot.objects[s];
    if (!matched[s] && saved.type != uint32_t(XObject::Type::Thread) && !saved.guest_object &&
        saved.type != uint32_t(XObject::Type::File) && saved.name.empty())
      pick(s, [&](XObject& o) { return !o.guest_object() && o.name().empty(); });
  }
  return matched;
}

void ReportMatch(const Snapshot& snapshot) {
  auto live = g_runtime->kernel_state()->object_table()->GetAllObjects();
  std::sort(live.begin(), live.end(),
            [](const auto& a, const auto& b) { return a->handle() < b->handle(); });
  const auto matched = Match(snapshot, live);
  int same_handle = 0, moved = 0, unmatched = 0;
  for (size_t s = 0; s < snapshot.objects.size(); ++s) {
    const auto& saved = snapshot.objects[s];
    const uint32_t handle = saved.handles.empty() ? 0 : saved.handles[0];
    const auto& object = matched[s];
    if (object && object->handle() == handle) {
      ++same_handle;
      continue;
    }
    object ? ++moved : ++unmatched;
    REXLOG_INFO("[snapshot]   {} {:08X}{}{}{}{} -> {}", TypeName(XObject::Type(saved.type)), handle,
                saved.thread_id ? fmt::format(" thread {}", saved.thread_id) : "",
                saved.guest_object ? fmt::format(" at {:08X}", saved.guest_object) : "",
                saved.path.empty() ? "" : " ", saved.path,
                object ? fmt::format("here {:08X}", object->handle()) : std::string("none here"));
  }
  int extra = 0;
  for (const auto& object : live)
    if (std::none_of(matched.begin(), matched.end(),
                     [&](const auto& m) { return m.get() == object.get(); })) {
      ++extra;
      REXLOG_INFO("[snapshot]   extra here: {} {:08X}{}", TypeName(object->type()), object->handle(),
                  object->guest_object() ? fmt::format(" at {:08X}", object->guest_object()) : "");
    }
  REXLOG_INFO("[snapshot] match: {} same handle, {} moved, {} unmatched, {} extra here",
              same_handle, moved, unmatched, extra);
}

// Gives the object table the saved numbering: each saved object's live
// counterpart moves to the saved handles (keeping waiters on it), saved
// objects with no counterpart are rebuilt from their saved state, and live
// objects the save doesn't have get fresh handles.
void RestoreObjects(const Snapshot& snapshot) {
  auto* kernel = g_runtime->kernel_state();
  auto* table = kernel->object_table();
  auto live = table->GetAllObjects();
  std::sort(live.begin(), live.end(),
            [](const auto& a, const auto& b) { return a->handle() < b->handle(); });
  const auto matched = Match(snapshot, live);
  // Take every live object out of the table (our references keep them).
  int moved = 0, rebuilt = 0, readded = 0, failed = 0;
  Trace("objects: matched");
  for (const auto& object : live) {
    const auto handles = object->handles();
    for (const uint32_t handle : handles) {
      table->RemoveHandle(handle);
    }
  }
  Trace("objects: table emptied");
  for (size_t s = 0; s < snapshot.objects.size(); ++s) {
    const auto& saved = snapshot.objects[s];
    const auto type = XObject::Type(saved.type);
    if (const auto& object = matched[s]) {
      for (size_t h = 0; h < saved.handles.size(); ++h) {
        const int refs = h < saved.refs.size() && saved.refs[h] > 0 ? saved.refs[h] : 1;
        if (XFAILED(table->RestoreHandle(saved.handles[h], object.get(), refs)))
          ++failed;
      }
      if (!saved.name.empty())
        table->AddNameMapping(saved.name, saved.handles[0]);
      ++moved;
      if (type == XObject::Type::File) {
        static_cast<rex::system::XFile*>(object.get())->set_position(saved.position);
      } else if (type == XObject::Type::Event) {
        auto* event = static_cast<rex::system::XEvent*>(object.get());
        if (saved.signaled)
          event->Set(0, false);
        else
          event->Reset();
      }
      continue;
    }
    if (saved.blob.empty()) {
      ++failed;
      REXLOG_WARN("[snapshot] {} {:08X} has no counterpart here and can't be rebuilt",
                  TypeName(type), saved.handles.empty() ? 0 : saved.handles[0]);
      continue;
    }
    std::vector<uint8_t> blob = saved.blob;
    rex::stream::ByteStream stream(blob.data(), blob.size());
    if (auto object = XObject::Restore(kernel, type, &stream)) {
      for (size_t h = 0; h < saved.handles.size(); ++h)
        table->RestoreHandle(saved.handles[h], object.get(),
                             h < saved.refs.size() && saved.refs[h] > 0 ? saved.refs[h] : 1);
      ++rebuilt;
      if (Verbose())
        REXLOG_INFO("[snapshot] rebuilt {} at {:08X} {}", TypeName(type),
                    saved.handles.empty() ? 0u : saved.handles[0], saved.path);
    } else {
      ++failed;
    }
  }
  for (const auto& object : live) {
    if (std::any_of(matched.begin(), matched.end(),
                    [&](const auto& m) { return m.get() == object.get(); }))
      continue;
    rex::X_HANDLE handle = 0;
    table->AddHandle(object.get(), &handle);
    ++readded;
  }
  REXLOG_INFO("[snapshot] objects: {} moved to their saved handles, {} rebuilt, {} given new "
              "handles, {} failed",
              moved, rebuilt, readded, failed);
}

// --- Files -------------------------------------------------------------------
//
// A snapshot file holds the threads, page tables, kernel objects and GPU ring
// state, and lists its memory as pieces: the committed pages of one 64 KB
// range of guest addresses. A piece that isn't all zeros lives in the shared
// chunk store under its content hash, snappy-compressed, so snapshots share
// everything they have in common (most of a level).

constexpr uint32_t kMagic = 0x53534E50;  // 'SSNP'
constexpr uint32_t kVersion = 4;
constexpr uint32_t kPieceShift = 16;

struct Writer {
  FILE* file;
  template <typename T>
  void Put(const T& value) {
    std::fwrite(&value, sizeof(T), 1, file);
  }
  void Bytes(const void* data, size_t size) {
    Put<uint64_t>(size);
    if (size)
      std::fwrite(data, 1, size, file);
  }
};
struct Reader {
  FILE* file;
  bool ok = true;
  template <typename T>
  T Get() {
    T value{};
    ok = ok && std::fread(&value, sizeof(T), 1, file) == 1;
    return value;
  }
  template <typename V>
  void Bytes(V& out) {
    const uint64_t size = Get<uint64_t>();
    if (!ok || size > (uint64_t(1) << 33)) {
      ok = false;
      return;
    }
    out.resize(size / sizeof(typename V::value_type));
    if (size)
      ok = ok && std::fread(out.data(), 1, size, file) == size;
  }
};

struct Piece {
  uint32_t address = 0, size = 0;
  uint64_t offset = 0;  // in the snapshot's data
  uint8_t heap = 0;
  bool zero = false;
  XXH128_hash_t hash{};
};

// The data heaps' committed pages cut into pieces, in capture order.
std::vector<Piece> Pieces(const Snapshot& snapshot) {
  std::vector<Piece> pieces;
  uint64_t offset = 0;
  for (size_t n = 0; n < std::size(kHeaps); ++n) {
    if (!kHeaps[n].data || n >= snapshot.pages.size())
      continue;
    auto* heap = HeapOf(kHeaps[n]);
    const auto& pages = snapshot.pages[n];
    for (size_t i = 0; i < pages.size(); ++i) {
      if (!Committed(pages[i]))
        continue;
      const uint32_t address = heap->heap_base() + uint32_t(i << heap->page_size_shift());
      if (pieces.empty() || pieces.back().heap != n ||
          (pieces.back().address >> kPieceShift) != (address >> kPieceShift)) {
        Piece piece;
        piece.heap = uint8_t(n);
        piece.address = address;
        piece.offset = offset;
        pieces.push_back(piece);
      }
      pieces.back().size += heap->page_size();
      offset += heap->page_size();
    }
  }
  return pieces;
}

std::string ChunkName(const XXH128_hash_t& hash) {
  return fmt::format("{:016x}{:016x}", hash.high64, hash.low64);
}
std::filesystem::path ChunkPath(const std::filesystem::path& chunks, const std::string& name) {
  return chunks / name.substr(0, 2) / (name + ".snz");
}

bool WriteFile(const std::filesystem::path& path, const void* data, size_t size) {
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  auto temporary = path;
  temporary += ".tmp";
  FILE* file = rex::filesystem::OpenFile(temporary, "wb");
  if (!file)
    return false;
  const bool written = std::fwrite(data, 1, size, file) == size;
  const bool closed = std::fclose(file) == 0;
  if (!written || !closed) {
    std::filesystem::remove(temporary, error);
    return false;
  }
  std::filesystem::rename(temporary, path, error);
  return !error;
}

bool WriteSnapshot(const std::filesystem::path& path, const std::filesystem::path& chunks,
                   const Snapshot& snapshot, Stats* stats) {
  static const std::vector<uint8_t> zeros(size_t(1) << kPieceShift, 0);
  auto pieces = Pieces(snapshot);
  Stats counted;
  std::string compressed;
  for (auto& piece : pieces) {
    const uint8_t* data = snapshot.data.data() + piece.offset;
    counted.memory += piece.size;
    piece.zero = std::memcmp(data, zeros.data(), piece.size) == 0;
    if (piece.zero)
      continue;
    ++counted.chunks;
    piece.hash = XXH3_128bits(data, piece.size);
    const auto file = ChunkPath(chunks, ChunkName(piece.hash));
    std::error_code error;
    if (std::filesystem::exists(file, error))
      continue;
    snappy::Compress(reinterpret_cast<const char*>(data), piece.size, &compressed);
    if (!WriteFile(file, compressed.data(), compressed.size()))
      return false;
    ++counted.new_chunks;
    counted.stored += compressed.size();
  }
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  auto temporary = path;
  temporary += ".tmp";
  FILE* file = rex::filesystem::OpenFile(temporary, "wb");
  if (!file)
    return false;
  Writer w{file};
  w.Put(kMagic);
  w.Put(kVersion);
  w.Put<uint32_t>(uint32_t(sizeof(PPCContext)));
  w.Put<uint32_t>(uint32_t(snapshot.threads.size()));
  for (const auto& thread : snapshot.threads) {
    w.Put(thread.id);
    w.Put<uint8_t>(thread.main);
    w.Put(thread.start_address);
    w.Put(thread.context);
  }
  w.Put<uint32_t>(uint32_t(snapshot.pages.size()));
  for (const auto& pages : snapshot.pages) {
    snappy::Compress(reinterpret_cast<const char*>(pages.data()),
                     pages.size() * sizeof(rex::memory::PageEntry), &compressed);
    w.Put<uint64_t>(pages.size());
    w.Bytes(compressed.data(), compressed.size());
  }
  w.Put<uint32_t>(uint32_t(pieces.size()));
  for (const auto& piece : pieces) {
    w.Put(piece.heap);
    w.Put(piece.address);
    w.Put(piece.size);
    w.Put<uint8_t>(piece.zero);
    w.Put(piece.hash.high64);
    w.Put(piece.hash.low64);
  }
  w.Bytes(snapshot.gpu.data(), snapshot.gpu.size());
  w.Put<uint32_t>(uint32_t(snapshot.objects.size()));
  for (const auto& object : snapshot.objects) {
    w.Put(object.type);
    w.Bytes(object.handles.data(), object.handles.size() * sizeof(uint32_t));
    w.Bytes(object.refs.data(), object.refs.size() * sizeof(int32_t));
    w.Bytes(object.blob.data(), object.blob.size());
    w.Bytes(object.path.data(), object.path.size());
    w.Put(object.position);
    w.Put(object.signaled);
    w.Put(object.thread_id);
    w.Put(object.guest_object);
    w.Bytes(object.name.data(), object.name.size());
  }
  const bool written = std::ferror(file) == 0;
  const bool closed = std::fclose(file) == 0;
  if (!written || !closed)
    return false;
  std::filesystem::rename(temporary, path, error);
  if (error)
    return false;
  counted.stored += std::filesystem::file_size(path, error);
  if (stats)
    *stats = counted;
  return true;
}

// The pieces a snapshot file lists (for reading it, or for the chunks it uses).
bool ReadSnapshot(const std::filesystem::path& path, const std::filesystem::path* chunks,
                  Snapshot& snapshot, std::vector<Piece>* listed) {
  FILE* file = rex::filesystem::OpenFile(path, "rb");
  if (!file)
    return false;
  Reader r{file};
  const bool header = r.Get<uint32_t>() == kMagic && r.Get<uint32_t>() == kVersion &&
                      r.Get<uint32_t>() == sizeof(PPCContext);
  if (!header) {
    std::fclose(file);
    return false;
  }
  snapshot.threads.resize(std::min<uint32_t>(r.Get<uint32_t>(), 256));
  for (auto& thread : snapshot.threads) {
    thread.id = r.Get<uint32_t>();
    thread.main = r.Get<uint8_t>();
    thread.start_address = r.Get<uint32_t>();
    thread.context = r.Get<PPCContext>();
  }
  snapshot.pages.resize(std::min<uint32_t>(r.Get<uint32_t>(), 16));
  std::string compressed;
  for (auto& pages : snapshot.pages) {
    const uint64_t count = r.Get<uint64_t>();
    r.Bytes(compressed);
    size_t length = 0;
    if (!r.ok || count > (uint64_t(1) << 24) ||
        !snappy::GetUncompressedLength(compressed.data(), compressed.size(), &length) ||
        length != count * sizeof(rex::memory::PageEntry)) {
      r.ok = false;
      break;
    }
    pages.resize(count);
    r.ok = snappy::RawUncompress(compressed.data(), compressed.size(),
                                 reinterpret_cast<char*>(pages.data()));
  }
  std::vector<Piece> pieces(r.ok ? std::min<uint32_t>(r.Get<uint32_t>(), 1u << 20) : 0);
  for (auto& piece : pieces) {
    piece.heap = r.Get<uint8_t>();
    piece.address = r.Get<uint32_t>();
    piece.size = r.Get<uint32_t>();
    piece.zero = r.Get<uint8_t>();
    piece.hash.high64 = r.Get<uint64_t>();
    piece.hash.low64 = r.Get<uint64_t>();
  }
  r.Bytes(snapshot.gpu);
  snapshot.objects.resize(r.ok ? std::min<uint32_t>(r.Get<uint32_t>(), 1u << 16) : 0);
  for (auto& object : snapshot.objects) {
    object.type = r.Get<uint32_t>();
    r.Bytes(object.handles);
    r.Bytes(object.refs);
    r.Bytes(object.blob);
    r.Bytes(object.path);
    object.position = r.Get<uint64_t>();
    object.signaled = r.Get<uint32_t>();
    object.thread_id = r.Get<uint32_t>();
    object.guest_object = r.Get<uint32_t>();
    r.Bytes(object.name);
  }
  std::fclose(file);
  if (!r.ok || snapshot.pages.size() != std::size(kHeaps))
    return false;
  if (listed)
    *listed = pieces;
  if (!chunks)
    return true;
  // The memory, in the order the pieces were cut; they must still match the
  // page tables they came from.
  const auto expected = Pieces(snapshot);
  if (expected.size() != pieces.size())
    return false;
  uint64_t total = 0;
  for (size_t i = 0; i < pieces.size(); ++i) {
    if (expected[i].address != pieces[i].address || expected[i].size != pieces[i].size ||
        expected[i].heap != pieces[i].heap)
      return false;
    pieces[i].offset = total;
    total += pieces[i].size;
  }
  snapshot.data.assign(total, 0);
  std::vector<char> stored;
  for (const auto& piece : pieces) {
    if (piece.zero)
      continue;
    const auto chunk = ChunkPath(*chunks, ChunkName(piece.hash));
    FILE* in = rex::filesystem::OpenFile(chunk, "rb");
    if (!in)
      return false;
    std::fseek(in, 0, SEEK_END);
    const long size = std::ftell(in);
    std::fseek(in, 0, SEEK_SET);
    stored.resize(size > 0 ? size_t(size) : 0);
    const bool read = size > 0 && std::fread(stored.data(), 1, stored.size(), in) == stored.size();
    std::fclose(in);
    size_t length = 0;
    if (!read || !snappy::GetUncompressedLength(stored.data(), stored.size(), &length) ||
        length != piece.size ||
        !snappy::RawUncompress(stored.data(), stored.size(),
                               reinterpret_cast<char*>(snapshot.data.data() + piece.offset)))
      return false;
  }
  return true;
}

// --- Saving and loading ----------------------------------------------------

auto Ms(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
  return std::chrono::duration<double, std::milli>(b - a).count();
}

bool Capture(PPCContext& ctx, Snapshot& snapshot) {
  if (!native_ragdoll::SnapshotReady()) {
    REXLOG_WARN("[snapshot] Stop native ragdolls before taking an exact save");
    return false;
  }
  const auto start = std::chrono::steady_clock::now();
  std::vector<ThreadRef> threads;
  if (!Freeze(threads))
    return false;
  const auto frozen = std::chrono::steady_clock::now();
  const uint32_t current = rex::system::XThread::GetCurrentThreadId();
  auto* kernel = g_runtime->kernel_state();
  {
    auto main = kernel->GetThreadByID(current);
    snapshot.threads.push_back(
        {current, true, main ? main->creation_params()->start_address : 0, ctx});
  }
  for (auto& thread : threads)
    snapshot.threads.push_back({thread->thread_id(), false,
                                thread->creation_params()->start_address,
                                *thread->thread_state()->context()});
  CaptureMemory(snapshot);
  const auto copied = std::chrono::steady_clock::now();
  snapshot.gpu = SaveGpu();
  CaptureObjects(snapshot);
  Thaw(threads);
  const auto end = std::chrono::steady_clock::now();
  REXLOG_INFO("[snapshot] captured {:.1f} MB, {} threads, {} objects in {:.0f} ms (freeze "
              "{:.0f}, memory {:.0f}, rest and thaw {:.0f})",
              snapshot.data.size() / 1048576.0, snapshot.threads.size(), snapshot.objects.size(),
              Ms(start, end), Ms(start, frozen), Ms(frozen, copied), Ms(copied, end));
  return true;
}

bool Apply(PPCContext& ctx, const Snapshot& snapshot) {
  if (!native_ragdoll::SnapshotReady()) {
    REXLOG_WARN("[snapshot] Stop native ragdolls before restoring an exact save");
    return false;
  }
  const auto start = std::chrono::steady_clock::now();
  if (!Fits(snapshot)) {
    REXLOG_WARN("[snapshot] made by a build whose own memory is elsewhere; nothing changed");
    return false;
  }
  std::vector<ThreadRef> threads;
  if (!Freeze(threads))
    return false;
  // Every guest thread must be parked where the saved one was: the same stack
  // pointer and the same call (a different call means a different chain of
  // host frames, which the saved registers would not fit).
  const uint32_t current = rex::system::XThread::GetCurrentThreadId();
  bool fits = threads.size() + 1 == snapshot.threads.size();
  if (!fits)
    REXLOG_WARN("[snapshot] {} guest threads here, {} saved", threads.size() + 1,
                snapshot.threads.size());
  for (const auto& saved : snapshot.threads) {
    const PPCContext* live = nullptr;
    if (saved.main) {
      live = saved.id == current ? &ctx : nullptr;
    } else {
      for (auto& thread : threads)
        if (thread->thread_id() == saved.id &&
            thread->creation_params()->start_address == saved.start_address)
          live = thread->thread_state()->context();
    }
    if (!live || live->r1.u32 != saved.context.r1.u32 || live->lr != saved.context.lr) {
      std::fprintf(stderr, "[snapshot] thread %u is not where it was\n", saved.id);
      REXLOG_WARN("[snapshot] thread {} is not where it was: r1 {:08X} lr {:08X}, saved r1 "
                  "{:08X} lr {:08X}",
                  saved.id, live ? live->r1.u32 : 0, live ? uint32_t(live->lr) : 0,
                  saved.context.r1.u32, uint32_t(saved.context.lr));
      fits = false;
    }
  }
  if (!fits) {
    Thaw(threads);
    REXLOG_WARN("[snapshot] the snapshot does not fit the running game; nothing changed");
    return false;
  }
  Trace("apply: threads fit");
  const auto checked = std::chrono::steady_clock::now();
  RestoreMemory(snapshot);
  native_ragdoll::MemoryRestored();
  const auto copied = std::chrono::steady_clock::now();
  Trace("apply: memory restored");
  for (const auto& saved : snapshot.threads) {
    if (saved.main)
      continue;
    for (auto& thread : threads)
      if (thread->thread_id() == saved.id)
        *thread->thread_state()->context() = saved.context;
  }
  if (auto* graphics = Graphics()) {
    std::vector<uint8_t> gpu = snapshot.gpu;
    rex::stream::ByteStream stream(gpu.data(), gpu.size());
    graphics->Restore(&stream);
    graphics->InvalidateGpuMemory();
    graphics->ClearCaches();
  }
  Trace("apply: GPU restored");
  RestoreObjects(snapshot);
  Trace("apply: objects restored");
  Thaw(threads);
  Trace("apply: thawed");
  // The main thread carries on from the saved frame start.
  for (const auto& saved : snapshot.threads)
    if (saved.main)
      ctx = saved.context;
  const auto end = std::chrono::steady_clock::now();
  REXLOG_INFO("[snapshot] applied in {:.0f} ms (freeze and check {:.0f}, memory {:.0f}, rest and "
              "thaw {:.0f})",
              Ms(start, end), Ms(start, checked), Ms(checked, copied), Ms(copied, end));
  return true;
}

void Save(PPCContext& ctx, const std::string& name) {
  Snapshot snapshot;
  if (!Capture(ctx, snapshot))
    return;
  g_snapshots[name] = std::move(snapshot);
  REXLOG_INFO("[snapshot] saved '{}'", name);
}

bool Load(PPCContext& ctx, const std::string& name) {
  const auto found = g_snapshots.find(name);
  if (found == g_snapshots.end()) {
    REXLOG_WARN("[snapshot] no snapshot '{}'", name);
    return false;
  }
  const bool ok = Apply(ctx, found->second);
  if (ok)
    REXLOG_INFO("[snapshot] loaded '{}'", name);
  return ok;
}

std::filesystem::path ChunksBeside(const std::string& path) {
  return std::filesystem::path(path).parent_path() / "chunks";
}

void SaveFile(PPCContext& ctx, const std::string& path) {
  Snapshot snapshot;
  if (!Capture(ctx, snapshot))
    return;
  const auto start = std::chrono::steady_clock::now();
  Stats stats;
  const bool ok = WriteSnapshot(path, ChunksBeside(path), snapshot, &stats);
  REXLOG_INFO("[snapshot] {} {} in {:.0f} ms: {:.1f} MB of memory, {} chunks ({} new), {:.1f} MB "
              "written",
              ok ? "wrote" : "could not write", path, Ms(start, std::chrono::steady_clock::now()),
              stats.memory / 1048576.0, stats.chunks, stats.new_chunks, stats.stored / 1048576.0);
}

void MatchFile(const std::string& path) {
  Snapshot snapshot;
  const auto chunks = ChunksBeside(path);
  if (!ReadSnapshot(path, &chunks, snapshot, nullptr)) {
    REXLOG_WARN("[snapshot] could not read {}", path);
    return;
  }
  std::vector<ThreadRef> threads;
  if (!Freeze(threads))
    return;
  ReportMatch(snapshot);
  Thaw(threads);
}

void LoadFile(PPCContext& ctx, const std::string& path) {
  const auto start = std::chrono::steady_clock::now();
  Snapshot snapshot;
  const auto chunks = ChunksBeside(path);
  if (!ReadSnapshot(path, &chunks, snapshot, nullptr)) {
    REXLOG_WARN("[snapshot] could not read {}", path);
    return;
  }
  REXLOG_INFO("[snapshot] read {} in {:.0f} ms", path, Ms(start, std::chrono::steady_clock::now()));
  if (Apply(ctx, snapshot))
    REXLOG_INFO("[snapshot] loaded {}", path);
}

// Development: the object a guest pointer points to, as words, with the text
// any word points to ('dump ADDRESS LENGTH' reads the pointer at ADDRESS).
// 'dump ADDRESS+A+B:LENGTH' follows the pointer at ADDRESS, then the ones at
// +A and +B in the objects reached.
void Dump(const uint8_t* base, const std::string& args) {
  auto readable = [](uint32_t a) { return a >= 0x10000 && Readable(a); };
  uint32_t length = 256;
  const size_t colon = args.find(':');
  if (colon != std::string::npos)
    length = uint32_t(std::strtoul(args.c_str() + colon + 1, nullptr, 10));
  const std::string path = args.substr(0, colon);
  uint32_t address = uint32_t(std::strtoul(path.c_str(), nullptr, 16));
  if (!readable(address))
    return;
  uint32_t object = LoadGuestU32(base, address);
  for (size_t plus = path.find('+'); plus != std::string::npos; plus = path.find('+', plus + 1)) {
    const uint32_t offset = uint32_t(std::strtoul(path.c_str() + plus + 1, nullptr, 10));
    if (!readable(object + offset))
      return;
    object = LoadGuestU32(base, object + offset);
  }
  REXLOG_INFO("[snapshot] dump {:08X} -> {:08X}", address, object);
  if (!readable(object))
    return;
  for (uint32_t offset = 0; offset < length; offset += 4) {
    if (!readable(object + offset))
      break;
    const uint32_t word = LoadGuestU32(base, object + offset);
    std::string text;
    if (word >= 0x30000000 && readable(word)) {
      const char* string = reinterpret_cast<const char*>(simpsons::GuestToHost(base, word));
      size_t n = 0;
      while (n < 48 && readable(word + uint32_t(n)) && string[n] >= 32 && string[n] < 127)
        ++n;
      if (n >= 3 && (string[n] == 0 || n == 48))
        text = fmt::format(" \"{}\"", std::string_view(string, n));
    }
    REXLOG_INFO("[snapshot]   +{:<4} {:08X}{}", offset, word, text);
  }
}

}  // namespace

struct Image {
  Snapshot snapshot;
};

void ImageDeleter::operator()(Image* image) const {
  delete image;
}

ImagePtr Take(PPCContext& ctx) {
  ImagePtr image(new Image);
  if (!Capture(ctx, image->snapshot))
    return nullptr;
  return image;
}

bool Put(PPCContext& ctx, const Image& image) {
  return Apply(ctx, image.snapshot);
}

bool Fits(const Image& image) {
  return Fits(image.snapshot);
}

bool Ready() {
  return AudioReady();
}

void Release() {
  AudioResume();
}

bool Write(const Image& image, const std::filesystem::path& file,
           const std::filesystem::path& chunks, Stats* stats) {
  return WriteSnapshot(file, chunks, image.snapshot, stats);
}

ImagePtr Read(const std::filesystem::path& file, const std::filesystem::path& chunks) {
  ImagePtr image(new Image);
  if (!ReadSnapshot(file, &chunks, image->snapshot, nullptr))
    return nullptr;
  return image;
}

size_t CollectChunks(const std::filesystem::path& chunks,
                     const std::vector<std::filesystem::path>& files) {
  // Every chunk some snapshot still lists; a file that can't be read keeps
  // the whole store (deleting chunks it might use would break it for good).
  std::set<std::string> used;
  for (const auto& file : files) {
    Snapshot snapshot;
    std::vector<Piece> pieces;
    if (!ReadSnapshot(file, nullptr, snapshot, &pieces))
      return 0;
    for (const auto& piece : pieces)
      if (!piece.zero)
        used.insert(ChunkName(piece.hash));
  }
  size_t removed = 0;
  std::error_code error;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(chunks, error)) {
    if (!entry.is_regular_file(error) || entry.path().extension() != ".snz")
      continue;
    if (!used.contains(entry.path().stem().string()) && std::filesystem::remove(entry.path(), error))
      ++removed;
  }
  return removed;
}

#if REX_PLATFORM_LINUX
// Development: names the thread a CPU-time signal (SIGXCPU) hits before the
// default action ends the process.
void OnCpuTimeSignal(int signal) {
  char name[32] = "?";
  prctl(PR_GET_NAME, name, 0, 0, 0);
  char line[128];
  const int length = std::snprintf(line, sizeof(line), "[snapshot] SIGXCPU in thread %ld (%s)\n",
                                   long(syscall(SYS_gettid)), name);
  if (length > 0)
    (void)!write(2, line, size_t(length));
  ::signal(signal, SIG_DFL);
  raise(signal);
}
#endif

void Init(rex::Runtime* runtime) {
  g_runtime = runtime;
#if REX_PLATFORM_LINUX
  // Development: test runs dump every thread's stack with a debugger that
  // isn't this process's parent, and say which thread a CPU-time limit hits.
  if (!REXCVAR_GET(snapshot_test_file).empty()) {
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    ::signal(SIGXCPU, OnCpuTimeSignal);
  }
#endif
}

void Tick(PPCContext& ctx, uint8_t* base) {
  const auto& path = REXCVAR_GET(snapshot_test_file);
  if (path.empty() || !g_runtime)
    return;
  FILE* file = std::fopen(path.c_str(), "r");
  if (!file)
    return;
  char op[16] = {}, label[512] = {};
  const int fields = std::fscanf(file, "%15s %511s", op, label);
  std::fclose(file);
  // Commands that freeze the game wait until it can be.
  const std::string_view name(op);
  if ((name == "save" || name == "load" || name == "savefile" || name == "loadfile") && !Ready())
    return;
  std::remove(path.c_str());
  if (fields == 2 && std::string_view(op) == "info")
    Info(base, label, path);
  else if (fields == 2 && std::string_view(op) == "save")
    Save(ctx, label);
  else if (fields == 2 && std::string_view(op) == "load")
    Load(ctx, label);
  else if (fields == 2 && std::string_view(op) == "savefile")
    SaveFile(ctx, label);
  else if (fields == 2 && std::string_view(op) == "loadfile")
    LoadFile(ctx, label);
  else if (fields == 2 && std::string_view(op) == "matchfile")
    MatchFile(label);
  else if (fields == 2 && std::string_view(op) == "dump")
    Dump(base, label);
}

}  // namespace snapshot

// The runtime holds its global lock around the callback, so a frozen game
// skips it (a few vblanks go unseen) rather than waiting in it.
REX_FUNC(sub_82456830) {
  {
    std::lock_guard lock(snapshot::g_gate_mutex);
    if (snapshot::g_frozen)
      return;
    ++snapshot::g_in_interrupt;
  }
  __imp__sub_82456830(ctx, base);
  {
    std::lock_guard lock(snapshot::g_gate_mutex);
    --snapshot::g_in_interrupt;
  }
  snapshot::g_gate_cv.notify_all();
}
