// This check runs the game's own decoder against creator output, without a
// game window, graphics backend, image loader, save files or guest threads.
#include "streams.h"
#include "../../simpsons/src/refpack_native.h"

#include <rex/ppc/context.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <new>
#include <span>
#include <string_view>
#include <vector>

namespace {
constexpr uint32_t Swap32(uint32_t value) {
  return value >> 24 | ((value >> 8) & 0xFF00) | ((value << 8) & 0xFF0000) | value << 24;
}
constexpr uint64_t Swap64(uint64_t value) {
  return uint64_t(Swap32(uint32_t(value))) << 32 | Swap32(uint32_t(value >> 32));
}
#define REX_FUNC_PROLOGUE() ((void)0)
#define REX_LOAD_U8(x) (*(volatile uint8_t*)(base + uint32_t(x)))
#define REX_LOAD_U32(x) Swap32(*(volatile uint32_t*)(base + uint32_t(x)))
#define REX_LOAD_U64(x) Swap64(*(volatile uint64_t*)(base + uint32_t(x)))
#define REX_STORE_U8(x, y) (*(volatile uint8_t*)(base + uint32_t(x)) = uint8_t(y))
#define REX_STORE_U32(x, y) (*(volatile uint32_t*)(base + uint32_t(x)) = Swap32(uint32_t(y)))
#define REX_STORE_U64(x, y) (*(volatile uint64_t*)(base + uint32_t(x)) = Swap64(uint64_t(y)))
#include "native_refpack.h"

using namespace simpsons::modkit;
uint64_t reference_ns = 0, native_ns = 0, validation_ns = 0, copy_ns = 0;
uint64_t native_bytes = 0, native_packets = 0;
uint64_t step_reference_ns = 0, step_native_ns = 0, step_calls = 0, step_bytes = 0;
uint32_t Big(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void CheckStepRollback() {
  for (const auto& commands :
       std::vector<std::vector<uint8_t>>{{0xE0, 'a', 'b', 'c', 'd', 0, 255},
                                         {0xE0, 'a', 'b', 'c', 'd', 0xC0},
                                         {0xE0, 'a', 'b', 'c', 'd', 0xC0, 0, 0, 255}}) {
    std::vector<uint64_t> aligned(0x11000 / 8, 0xBCBCBCBCBCBCBCBC);
    auto* base = reinterpret_cast<uint8_t*>(aligned.data());
    std::memcpy(base + 0x10020, commands.data(), commands.size());
    simpsons::StoreGuestU32(base, 0x9000, 0x10020);
    simpsons::StoreGuestU32(base, 0x9004, 0x10000);
    PPCContext ctx{};
    ctx.r1.u64 = 0x8000;
    ctx.r3.u64 = 0x9000;
    ctx.r4.u64 = 0x9004;
    ctx.r5.u64 = 32;
    ctx.r29.u64 = 0x1122334455667788;
    ctx.lr = 0x12345678;
    const auto initial = ctx;
    const auto before = aligned;
    std::vector<uint8_t> backup(32);
    simpsons::refpack::StepResult decoded;
    if (simpsons::native_refpack::TryStep(
            ctx, base, std::span<const uint8_t>(base + 0x10020, commands.size()),
            std::span<uint8_t>(base + 0x10000, 32), 0, 0x10020, 0x10000, 32, backup, decoded) ||
        decoded.produced != 4 || aligned != before || std::memcmp(&ctx, &initial, sizeof(ctx)))
      throw Failure("native_step_rollback", "Rejected streaming input changed guest state");
  }
}
void CheckStreamOwners() {
  simpsons::native_refpack::StreamOwners owners;
  owners.Begin(1, 100, 32);
  auto previous = owners.Find(1, 100, 1000);
  owners.Advance(previous, 110, 1004, 4, false);
  const auto next = owners.Find(1, 110, 1004);
  if (!next.cursor || next.produced != 4 || next.size != 32)
    throw Failure("native_stream_owner", "Decode progress was not retained");
  owners.Begin(1, 100, 32);
  owners.Advance(previous, 110, 1004, 4, false);
  if (owners.Find(1, 100, 1000).produced != 0)
    throw Failure("native_stream_owner", "A retired generation changed a reused cursor");
  if (owners.Find(1, 101, 1000).cursor || owners.Find(1, 100, 1000).cursor)
    throw Failure("native_stream_owner", "Changed source cursors must invalidate their owner");
  owners.Begin(1, 100, 32);
  previous = owners.Find(1, 100, 1000);
  owners.Advance(previous, 110, 1004, 4, false);
  if (owners.Find(1, 110, 1005).cursor || owners.Find(1, 110, 1004).cursor)
    throw Failure("native_stream_owner", "Changed output cursors must invalidate their owner");
  owners.Begin(1, 100, 32);
  previous = owners.Find(1, 100, 1000);
  owners.Advance(previous, 110, 1033, 33, false);
  if (owners.Find(1, 110, 1033).cursor)
    throw Failure("native_stream_owner", "A stream cannot exceed its declared output");
  for (uint32_t i = 1; i <= 65; ++i)
    owners.Begin(i, 100 + i, 32);
  if (owners.Find(1, 101, 1000).cursor || !owners.Find(65, 165, 1000).cursor)
    throw Failure("native_stream_owner", "Bounded cache eviction did not preserve recent owners");
  previous = owners.Find(65, 165, 1000);
  owners.Advance(previous, 170, 1004, 4, true);
  if (owners.Find(65, 170, 1004).cursor)
    throw Failure("native_stream_owner", "A finished resource retained its owner");
  owners.Begin(1, 100, 32);
  previous = owners.Find(1, 100, 1000);
  owners.Clear();
  owners.Begin(1, 100, 32);
  owners.Advance(previous, 110, 1004, 4, false);
  if (owners.Find(1, 100, 1000).produced)
    throw Failure("native_stream_owner", "Restored guest memory reused an old host generation");
  owners.Clear();
  owners.Begin(1, 100, 32);
  previous = owners.Find(2, 100, 1000);
  if (previous.cursor != 2 || !previous.destination_known)
    throw Failure("native_stream_owner", "A copied initial cursor lost its header identity");
  owners.Advance(previous, 110, 1004, 4, false);
  if (owners.Find(2, 110, 1004).produced != 4)
    throw Failure("native_stream_owner", "A transferred reader lost its progress");
  owners.Clear();
  owners.Begin(1, 100, 64);
  previous = owners.Find(2, 100, 1000);
  owners.Begin(1, 100, 32);
  const auto reused = owners.Find(2, 100, 1000);
  if (reused.size != 32 || reused.generation == previous.generation)
    throw Failure("native_stream_owner", "A reused source kept a cancelled resource's size");
  owners.Clear();
  owners.Begin(1, 100, 32);
  owners.Begin(2, 100, 32);
  if (owners.Find(3, 100, 1000).cursor)
    throw Failure("native_stream_owner", "An ambiguous initial source acquired an owner");
}
void DecodeSteps(const std::vector<uint8_t>& packed, const std::vector<uint8_t>& expected,
                 uint32_t allocation, uint32_t offset, const std::string& label) {
  const size_t size =
      (std::max<size_t>({allocation, size_t(offset) + packed.size(), expected.size()}) + 0x10000 +
       63) &
      ~size_t(63);
  const auto release = [](uint8_t* bytes) { ::operator delete[](bytes, std::align_val_t(64)); };
  const auto allocate = [&]() {
    return std::unique_ptr<uint8_t[], decltype(release)>(
        static_cast<uint8_t*>(::operator new[](size, std::align_val_t(64))), release);
  };
  auto original = allocate(), native = allocate();
  const auto header = simpsons::refpack::Header(packed);
  const uint32_t input_start = 0x10000 + offset;
  // Include tiny/zero boundaries, a normal block, and the larger time-budget
  // request observed in the live asynchronous loader.
  const uint32_t budgets[] = {0, 1, 2, 3, 4, 17, 4093, 16384, 4849140};
  std::vector<uint8_t> backup(std::min<size_t>(expected.size(), 4849140 + 1030));
  for (auto* memory : {original.get(), native.get()}) {
    std::memset(memory, 0, size);
    std::memcpy(memory + input_start, packed.data(), packed.size());
    const auto source = Swap32(input_start);
    const auto destination = Swap32(0x10000);
    std::memcpy(memory + 0x9000, &source, 4);
    std::memcpy(memory + 0x9004, &destination, 4);
    PPCContext ctx{};
    ctx.r3.u64 = 0x9000;
    GameRefPackReadSize(ctx, memory);
    if (ctx.r3.u32 != expected.size() || Big(memory + 0x9000) != input_start + header.consumed)
      throw Failure("native_size_header", label + " differs from the original size reader");
  }
  simpsons::native_refpack::StreamOwners owners;
  owners.Begin(0x9000, Big(native.get() + 0x9000), uint32_t(expected.size()));
  PPCContext reference{}, candidate{};
  reference.r1.u64 = 0x8000;
  reference.r29.u64 = 0x1122334455667788;
  reference.r30.u64 = 0x99AABBCCDDEEFF00;
  reference.r31.u64 = 0x0123456789ABCDEF;
  reference.lr = 0x8281E9B8;
  candidate = reference;
  for (size_t call = 0;; ++call) {
    const auto budget = budgets[call % std::size(budgets)];
    const auto source = Big(native.get() + 0x9000);
    const auto destination = Big(native.get() + 0x9004);
    if (source < input_start || source - input_start > packed.size() || destination < 0x10000 ||
        destination - 0x10000 > expected.size())
      throw Failure("native_step_cursor", label + " left its resource");
    reference.r3.u64 = candidate.r3.u64 = 0x9000;
    reference.r4.u64 = candidate.r4.u64 = 0x9004;
    reference.r5.u64 = candidate.r5.u64 = budget;
    const auto input =
        std::span<const uint8_t>(native.get() + source, packed.size() - (source - input_start));
    const auto owner = owners.Find(0x9000, source, destination);
    if (!owner.cursor || owner.produced != destination - 0x10000)
      throw Failure("native_stream_owner", label + " lost its resource identity");
    const size_t history = std::min<size_t>(131072, owner.produced);
    const auto output = std::span<uint8_t>(
        native.get() + destination - history,
        history + std::min<size_t>(owner.size - owner.produced, size_t(budget) + 1030));
    const auto original_start = std::chrono::steady_clock::now();
    GameRefPackStep(reference, original.get());
    step_reference_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now() - original_start)
                             .count();
    const auto native_start = std::chrono::steady_clock::now();
    simpsons::refpack::StepResult result;
    if (!simpsons::native_refpack::TryStep(candidate, native.get(), input, output, history, source,
                                           destination, budget, backup, result))
      throw Failure("native_step_plan",
                    label + ": " + std::string(simpsons::refpack::Describe(result.error)));
    step_native_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now() - native_start)
                          .count();
    ++step_calls;
    step_bytes += result.produced;
    owners.Advance(owner, Big(native.get() + 0x9000), Big(native.get() + 0x9004),
                   uint32_t(result.produced), result.stopped);
    if (!result || candidate.r3.u64 != reference.r3.u64 || candidate.r1.u64 != reference.r1.u64 ||
        candidate.lr != reference.lr || candidate.r29.u64 != reference.r29.u64 ||
        candidate.r30.u64 != reference.r30.u64 || candidate.r31.u64 != reference.r31.u64 ||
        candidate.r12.u64 != reference.r12.u64 ||
        std::memcmp(native.get() + 0x9000, original.get() + 0x9000, 8) ||
        std::memcmp(native.get() + 0x7FE0, original.get() + 0x7FE0, 32) ||
        std::memcmp(native.get() + destination, original.get() + destination, result.produced))
      throw Failure("native_step_mismatch",
                    label + " differs in step boundaries, cursors, output or ABI");
    if (reference.r3.u32)
      break;
    if (call > expected.size() * 2 + 32)
      throw Failure("native_step_loop", label + " did not terminate");
  }
  if (Big(native.get() + 0x9004) != 0x10000 + expected.size() ||
      std::memcmp(native.get(), original.get(), size) ||
      !std::equal(expected.begin(), expected.end(), native.get() + 0x10000))
    throw Failure("native_step_memory", label + " differs in final output or untouched memory");
}
void Decode(const std::vector<uint8_t>& packed, const std::vector<uint8_t>& expected,
            uint32_t allocation, uint32_t offset, const std::string& label) {
  const size_t size =
      (std::max<size_t>({allocation, size_t(offset) + packed.size(), expected.size()}) + 0x10000 +
       63) &
      ~size_t(63);
  // Use the real SDK's PPCContext and the unchanged generated scalar routine.
  // Guest addresses are offsets into this private aligned allocation.
  auto release = [](uint8_t* bytes) { ::operator delete[](bytes, std::align_val_t(64)); };
  std::unique_ptr<uint8_t[], decltype(release)> memory(
      static_cast<uint8_t*>(::operator new[](size, std::align_val_t(64))), release);
  auto* base = memory.get();
  std::memset(base, 0, size);
  std::memcpy(base + 0x10000 + offset, packed.data(), packed.size());
  PPCContext ctx{};
  ctx.r1.u64 = 0x8000;
  ctx.r3.u64 = 0x10000 + offset;
  ctx.r4.u64 = 0x10000;
  ctx.r5.u64 = 0x9000;
  ctx.r31.u64 = 0x0123456789ABCDEF;
  const PPCContext initial = ctx;
  const auto original_start = std::chrono::steady_clock::now();
  GameRefPackDecode(ctx, base);
  reference_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now() - original_start)
                      .count();
  if (!ctx.r3.u32 || Big(base + 0x9000) != expected.size() ||
      !std::equal(expected.begin(), expected.end(), base + 0x10000))
    throw Failure("native_decode_mismatch", label + " disagrees with the game's decoder");
  const auto original_memory = std::vector<uint8_t>(base, base + size);
  std::memset(base, 0, size);
  std::memcpy(base + 0x10000 + offset, packed.data(), packed.size());
  auto native_context = initial;
  const auto input = std::span<const uint8_t>(base + 0x10000 + offset, packed.size());
  const auto output = std::span<uint8_t>(base + 0x10000, expected.size());
  const auto native_start = std::chrono::steady_clock::now();
  const auto plan = simpsons::refpack::Validate(input, output, 0x10000 + offset, 0x10000);
  const auto validated = std::chrono::steady_clock::now();
  if (!plan)
    throw Failure("native_plan_mismatch",
                  label + ": " + std::string(simpsons::refpack::Describe(plan.error)));
  const auto result = simpsons::native_refpack::Commit(native_context, base, input, output);
  native_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now() - native_start)
                   .count();
  validation_ns +=
      std::chrono::duration_cast<std::chrono::nanoseconds>(validated - native_start).count();
  copy_ns = native_ns - validation_ns;
  if (!result || native_context.r3.u64 != ctx.r3.u64 || native_context.r31.u64 != ctx.r31.u64 ||
      native_context.r1.u64 != ctx.r1.u64 ||
      !std::equal(original_memory.begin(), original_memory.end(), base))
    throw Failure("native_decode_mismatch",
                  label + " differs in output, return value, ABI spill or untouched memory");
  native_bytes += expected.size();
  ++native_packets;
  DecodeSteps(packed, expected, allocation, offset, label);
}
}  // namespace

int Run(int argc, const fs::path& source, const fs::path& report_path, bool decode_only) {
  Json report = {
      {"ok", false}, {"reference", "sub_8281E5B0"}, {"source_sha256", kGameRefPackSource},
      {"files", 0},  {"packed_entries", 0},         {"encoded_entries", 0},
      {"bytes", 0},  {"original_stored_bytes", 0},  {"encoded_stored_bytes", 0}};
  const auto start = std::chrono::steady_clock::now();
  bool write_report = false;
  try {
    if (argc != 3 && !(argc == 4 && decode_only))
      throw Failure(
          "usage",
          "Usage: modkit_refpack_check <game-data-or-stream> <report.json> [--decode-only]");
    report["decode_only"] = decode_only;
    if (report_path.extension() != ".json" ||
        fs::weakly_canonical(source) == fs::weakly_canonical(report_path))
      throw Failure("invalid_path", "Choose a separate JSON report file");
    write_report = true;
    if (source == "--self-test") {
      CheckStepRollback();
      CheckStreamOwners();
      const std::vector<std::vector<uint8_t>> inputs = {
          {0x10, 0xFB, 0, 0, 7, 0xE0, 'a', 'b', 'c', 'd', 0, 0, 0xFC},
          {0x10, 0xFB, 0, 0, 8, 0xE0, 'a', 'b', 'c', 'd', 0x80, 0, 3, 0xFC},
          {0x90, 0xFB, 0, 0, 0, 13, 0xE0, 'a', 'b', 'c', 'd', 0xC0, 0, 3, 4, 0xFC},
          {0x11, 0xFB, 0, 0, 12, 0, 0, 3, 0xFF, 'a', 'b', 'c'},
          {0x10, 0xFB, 0, 0, 0, 0xFC}};
      const size_t sizes[] = {7, 8, 13, 3, 0};
      for (size_t i = 0; i < inputs.size(); ++i) {
        const auto decoded = DecodeRefPack(inputs[i], sizes[i], sizes[i]);
        Decode(inputs[i], decoded, 2048, 32, "synthetic original");
        uint32_t offset = 0;
        const auto encoded = EncodeRefPack(decoded, &offset);
        Decode(encoded, decoded, offset + uint32_t(encoded.size()), offset, "synthetic encoded");
        report["bytes"] = report["bytes"].get<uint64_t>() + decoded.size();
      }
      // Make the encoder emit long-distance references and cross its window
      // boundary. These go through the game routine, not just our own decoder.
      std::vector<uint8_t> input(140000);
      uint32_t random = 0x71A43u;
      for (auto& byte : input) {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        byte = uint8_t(random);
      }
      for (const size_t gap : {512, 8192, 65536, 131072}) {
        auto test = input;
        std::copy_n(input.begin(), 1028, test.begin() + gap);
        uint32_t offset = 0;
        const auto encoded = EncodeRefPack(test, &offset);
        Decode(encoded, test, offset + uint32_t(encoded.size()), offset,
               "encoder reference window");
        report["bytes"] = report["bytes"].get<uint64_t>() + test.size();
      }
      report["packed_entries"] = inputs.size();
      report["encoded_entries"] = inputs.size() + 4;
      report["ok"] = true;
    } else {
      std::vector<fs::path> files;
      if (fs::is_regular_file(source))
        files.push_back(source);
      else {
        for (auto it = fs::recursive_directory_iterator(source);
             it != fs::recursive_directory_iterator(); ++it) {
          if (it->is_symlink()) {
            if (it->is_directory())
              it.disable_recursion_pending();
            continue;
          }
          auto extension = Utf8(it->path().extension());
          for (auto& c : extension)
            if (c >= 'A' && c <= 'Z')
              c += 'a' - 'A';
          if (it->is_regular_file() && extension == ".str")
            files.push_back(it->path());
        }
      }
      if (files.empty())
        throw Failure("missing_streams", "No STR files were found");
      std::sort(files.begin(), files.end());
      for (const auto& file : files) {
        StreamFile stream(file);
        std::ifstream input(file, std::ios::binary);
        for (size_t i = 0; i < stream.entries().size(); ++i) {
          const auto& entry = stream.entries()[i];
          if (entry.stored > 512 * 1024 * 1024 || entry.allocation > 512 * 1024 * 1024)
            throw Failure("resource_limit", "A reference check entry exceeds 512 MiB");
          std::vector<uint8_t> packed(entry.stored);
          input.seekg(std::streamoff(entry.offset));
          if (!input ||
              (packed.size() && !input.read(reinterpret_cast<char*>(packed.data()), packed.size())))
            throw Failure("read_failed", "A stream changed during the reference check");
          if (packed.size() < 2 || (packed[0] & 0x3E) != 0x10 || packed[1] != 0xFB)
            continue;
          const auto decoded = stream.Read(i);
          const auto label = Utf8(file) + ":" + std::to_string(i);
          Decode(packed, decoded, entry.allocation, entry.read_offset, label + " original");
          report["packed_entries"] = report["packed_entries"].get<size_t>() + 1;
          report["original_stored_bytes"] =
              report["original_stored_bytes"].get<uint64_t>() + entry.stored;
          if (!decode_only) {
            uint32_t offset = 0;
            const auto encoded = EncodeRefPack(decoded, &offset);
            const auto stored = uint32_t((encoded.size() + 2047) & ~size_t(2047));
            const auto allocation =
                uint32_t((std::max<size_t>(decoded.size(), stored + offset) + 255) & ~size_t(255));
            Decode(encoded, decoded, allocation, offset, label + " encoded");
            report["encoded_stored_bytes"] =
                report["encoded_stored_bytes"].get<uint64_t>() + stored;
            report["encoded_entries"] = report["encoded_entries"].get<size_t>() + 1;
          }
          report["bytes"] = report["bytes"].get<uint64_t>() + decoded.size();
        }
        report["files"] = report["files"].get<size_t>() + 1;
      }
      report["ok"] = true;
    }
  } catch (const std::exception& error) {
    report["error"] = error.what();
  }
  report["seconds"] =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  report["native_comparison"] = {
      {"packets", native_packets},
      {"bytes", native_bytes},
      {"original_ms", reference_ns / 1000000.0},
      {"checked_native_ms", native_ns / 1000000.0},
      {"validation_ms", validation_ns / 1000000.0},
      {"copy_ms", copy_ns / 1000000.0},
      {"speedup", native_ns ? double(reference_ns) / double(native_ns) : 0},
      {"includes_guest_heap_queries", false}};
  report["stream_comparison"] = {
      {"calls", step_calls},
      {"bytes", step_bytes},
      {"original_ms", step_reference_ns / 1000000.0},
      {"checked_native_ms", step_native_ns / 1000000.0},
      {"speedup", step_native_ns ? double(step_reference_ns) / double(step_native_ns) : 0},
      {"source_sha256", kGameRefPackStepSource},
      {"size_reader_sha256", kGameRefPackSizeSource},
      {"save_sha256", kGameRefPackSaveSource},
      {"restore_sha256", kGameRefPackRestoreSource},
      {"includes_guest_heap_queries", false}};
  if (write_report) {
    try {
      WriteText(report_path, report.dump(2));
    } catch (const std::exception& error) {
      report["ok"] = false;
      report["error"] = error.what();
    }
  }
  std::cout << report.dump() << '\n';
  return report.at("ok") == true ? 0 : 1;
}

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
  return Run(argc, argc > 1 ? fs::path(argv[1]) : fs::path(),
             argc > 2 ? fs::path(argv[2]) : fs::path(),
             argc == 4 && std::wstring_view(argv[3]) == L"--decode-only");
}
#else
int main(int argc, char** argv) {
  return Run(argc, argc > 1 ? Utf8Path(argv[1]) : fs::path(),
             argc > 2 ? Utf8Path(argv[2]) : fs::path(),
             argc == 4 && std::string_view(argv[3]) == "--decode-only");
}
#endif
