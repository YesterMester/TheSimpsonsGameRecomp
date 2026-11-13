#include "simpsons_init.h"
#include "native_gpu_recorded.h"
#include "native_gpu_recorded_view.h"
#include "guest_memory.h"
#include "guest_memory_fixture.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "native_gpu_recorded_reference.inc"

using simpsons::LoadGuestU32;
using simpsons::StoreGuestU32;
using simpsons::native_gpu_calls::RecordedLogicalHeader;
namespace {
constexpr uint32_t kSize = 1024 * 1024, kStack = 0xE0000, kRecord = 0x1000;
constexpr uint32_t kDescriptor = 0x2000, kIterator = 0x2200, kTag = 0x2300, kMeta = 0x2400;
constexpr uint32_t kWriter = 0x2500, kResource = 0x3000, kChain = 0x4000;
constexpr uint32_t kCopiedIterator = 0x2600;
constexpr uint32_t kHigh = 0xC003F000, kHighSize = 0x60000;
std::unique_ptr<GuestMemoryFixture> original, native;
std::array<uint8_t, kSize> pristine;
struct Record {
  uint32_t legacy, native;
  std::vector<uint32_t> payload;
  std::vector<uint32_t> native_payload;
};
struct Header {
  uint32_t address, legacy, native;
};
std::vector<Record> records;
std::vector<Header> headers;
struct CompoundSpan {
  uint32_t first, words;
};
std::vector<CompoundSpan> compounds;
uint64_t continuation_checks = 0, copied_iterators = 0;
uint64_t rejected_compounds = 0;
uint64_t comparisons = 0, iterator_steps = 0, invalidations = 0, copies = 0, patches = 0;
uint32_t active_case = 0;
void Require(bool ok, const char* why) {
  if (!ok)
    throw std::runtime_error(why);
}
void Both(uint32_t address, uint32_t value) {
  StoreGuestU32(original->data(), address, value);
  StoreGuestU32(native->data(), address, value);
}
void Reset() {
  for (auto* memory : {original.get(), native.get()}) {
    std::memcpy(memory->data(), pristine.data(), kSize);
    std::memcpy(simpsons::GuestToHost(memory->data(), kHigh), pristine.data(), kHighSize);
  }
  headers.clear();
  compounds.clear();
}
PPCContext Context(uint32_t seed) {
  PPCContext ctx{};
  std::array<PPCRegister PPCContext::*, 32> regs = {
      &PPCContext::r0,  &PPCContext::r1,  &PPCContext::r2,  &PPCContext::r3,  &PPCContext::r4,
      &PPCContext::r5,  &PPCContext::r6,  &PPCContext::r7,  &PPCContext::r8,  &PPCContext::r9,
      &PPCContext::r10, &PPCContext::r11, &PPCContext::r12, &PPCContext::r13, &PPCContext::r14,
      &PPCContext::r15, &PPCContext::r16, &PPCContext::r17, &PPCContext::r18, &PPCContext::r19,
      &PPCContext::r20, &PPCContext::r21, &PPCContext::r22, &PPCContext::r23, &PPCContext::r24,
      &PPCContext::r25, &PPCContext::r26, &PPCContext::r27, &PPCContext::r28, &PPCContext::r29,
      &PPCContext::r30, &PPCContext::r31};
  for (uint32_t i = 0; i < regs.size(); ++i)
    (ctx.*regs[i]).u64 = 0xABCD987600000000ull | ((seed * 17 + i * 59) & 0xFFFF);
  ctx.r1.u64 = 0xFFFF111100000000ull | kStack;
  ctx.lr = 0x82654320;
  ctx.ctr.u64 = 0x1234567800000001ull;
  ctx.xer.ca = seed & 1;
  ctx.xer.so = (seed >> 1) & 1;
  ctx.cr0.compare<int32_t>(-1, 2, ctx.xer);
  ctx.cr6.compare<uint32_t>(seed, 3, ctx.xer);
  return ctx;
}
void Equal(const PPCContext& expected, const PPCContext& actual) {
  Require(!std::memcmp(&expected, &actual, sizeof(actual)), "Complete CPU context differs");
  std::vector<std::pair<uint32_t, uint32_t>> continuations;
  for (uint32_t state : {kIterator, kCopiedIterator, kWriter + 4, kStack - 108, kStack - 92,
                         kStack - 124, kStack - 80, kStack - 96}) {
    const uint32_t wanted = LoadGuestU32(original->data(), state + 24);
    const uint32_t observed = LoadGuestU32(native->data(), state + 24);
    if (wanted == observed)
      continue;
    const uint32_t next = LoadGuestU32(original->data(), state);
    auto span = std::find_if(compounds.begin(), compounds.end(), [&](const auto& c) {
      const uint32_t header = LoadGuestU32(original->data(), state + 8);
      return next > c.first && uint64_t(next) <= uint64_t(c.first) + c.words * 4 &&
             header >= c.first && uint64_t(header) < uint64_t(c.first) + c.words * 4;
    });
    Require(span != compounds.end() && wanted <= 1 && !((next - span->first) & 3) &&
                observed == (0x4E560000u | (((next - span->first) / 4) << 1) | wanted),
            "Iterator continuation lost its parent, relative position or predicate");
    continuations.emplace_back(state + 24, observed);
    StoreGuestU32(native->data(), state + 24, wanted);
    ++continuation_checks;
  }
  for (const auto& h : headers) {
    Require(LoadGuestU32(original->data(), h.address) == h.legacy &&
                LoadGuestU32(native->data(), h.address) == h.native,
            "An editor replaced a command header or a copy lost its native encoding");
    StoreGuestU32(native->data(), h.address, h.legacy);
  }
  Require(!std::memcmp(original->data(), native->data(), kSize),
          "An edit changed payloads, pointers, stack, patch tables or padding");
  Require(!std::memcmp(simpsons::GuestToHost(original->data(), kHigh),
                       simpsons::GuestToHost(native->data(), kHigh), kHighSize),
          "An edit changed a physical-view buffer outside its header encoding");
  for (const auto& h : headers)
    StoreGuestU32(native->data(), h.address, h.native);
  for (auto [address, flags] : continuations)
    StoreGuestU32(native->data(), address, flags);
  ++comparisons;
}
uint32_t Emit(uint32_t address, size_t first = 0, size_t end = SIZE_MAX) {
  end = std::min(end, records.size());
  const auto start = address;
  for (size_t i = first; i < end; ++i) {
    const auto& r = records[i];
    StoreGuestU32(original->data(), address, r.legacy);
    StoreGuestU32(native->data(), address, r.native);
    headers.push_back({address, r.legacy, r.native});
    if (!r.native_payload.empty())
      compounds.push_back({address, uint32_t(r.payload.size() + 1)});
    address += 4;
    for (size_t j = 0; j < r.payload.size(); ++j) {
      const uint32_t word = r.payload[j];
      const uint32_t native_word = r.native_payload.empty() ? word : r.native_payload[j];
      StoreGuestU32(original->data(), address, word);
      StoreGuestU32(native->data(), address, native_word);
      if (word != native_word)
        headers.push_back({address, word, native_word});
      address += 4;
    }
  }
  return (address - start) / 4;
}
void Add(uint32_t legacy, uint32_t header, uint32_t count) {
  Require(RecordedLogicalHeader(header) == legacy, "A supported logical header differs");
  Record record{legacy, header, {}, {}};
  for (uint32_t i = 0; i < count; ++i)
    record.payload.push_back(0x3F000000u + i * 0x10203u);
  records.push_back(std::move(record));
}
template <size_t N>
void Aggregate(std::array<uint32_t, N> words,
               std::initializer_list<std::pair<uint32_t, uint32_t>> logical_headers) {
  auto legacy = words;
  for (auto [offset, header] : logical_headers)
    legacy[offset] = header;
  records.push_back(
      {legacy[0], words[0], {legacy.begin() + 1, legacy.end()}, {words.begin() + 1, words.end()}});
}
void CompoundCorpus() {
  namespace codec = rex::graphics::native_gpu_calls;
  const std::array<uint32_t, 6> fetch = {1, 0x80000010, 1, 0, 0x40083, 0x10000020};
  const std::array<uint32_t, 3> flush = {0, 0x25000, 0};
  for (bool indexed : {false, true}) {
    codec::DrawCall call{fetch,
                         0x4000,
                         0x40000,
                         0x05000000,
                         flush,
                         0,
                         indexed ? 0x30006u : 0x30086u,
                         indexed ? 0x40080u : 0u,
                         indexed ? 0x80000006u : 0u};
    if (indexed) {
      std::array<uint32_t, 27> words;
      Require(codec::EncodeDraw(call, true, words), "Invalid full indexed fixture");
      Aggregate(words, {{0, 0x548BA},
                        {7, 0xA31},
                        {9, 0x10A2F},
                        {12, 0xC0043C00},
                        {18, 0x25000},
                        {22, 0xC0032200}});
    } else {
      std::array<uint32_t, 24> words;
      Require(codec::EncodeDraw(call, false, words), "Invalid full auto fixture");
      Aggregate(words, {{0, 0x548BA},
                        {7, 0xA31},
                        {9, 0x10A2F},
                        {12, 0xC0043C00},
                        {18, 0x25000},
                        {22, 0xC0003600}});
    }
    codec::VerticesDrawCall vertices{32,
                                     fetch,
                                     flush,
                                     0xA5A50100,
                                     indexed ? 0x30006u : 0x30086u,
                                     indexed ? 0x40080u : 0u,
                                     indexed ? 0x80000006u : 0u};
    codec::RangeDrawCall range{32, vertices.viz_query_condition, vertices.initiator,
                               vertices.index_base, vertices.index_size};
    if (indexed) {
      std::array<uint32_t, 18> words;
      Require(codec::EncodeVertices(vertices, words), "Invalid indexed vertices fixture");
      Aggregate(words, {{0, 0x2102}, {2, 0x548BA}, {9, 0x25000}, {13, 0xC0032201}});
      std::array<uint32_t, 7> range_words;
      Require(codec::EncodeRange(range, range_words), "Invalid indexed range fixture");
      Aggregate(range_words, {{0, 0x2102}, {2, 0xC0032201}});
    } else {
      std::array<uint32_t, 16> words;
      Require(codec::EncodeVertices(vertices, words), "Invalid auto vertices fixture");
      Aggregate(words, {{0, 0x2102}, {2, 0x548BA}, {9, 0x25000}, {13, 0xC0012201}});
      std::array<uint32_t, 5> range_words;
      Require(codec::EncodeRange(range, range_words), "Invalid auto range fixture");
      Aggregate(range_words, {{0, 0x2102}, {2, 0xC0012201}});
    }
  }
  Aggregate(rex::graphics::native_gpu_coherency::Encode({0x40000, 0x10000, 0, 0x1000}),
            {{0, 0x2007}, {2, 0xA31}, {4, 0x10A2F}, {7, 0xC0043C00}});
  Aggregate(rex::graphics::native_gpu_sync::Encode({0x5000000, 0x4000, 0x40000}),
            {{0, 0xA31}, {2, 0x10A2F}, {5, 0xC0043C00}});
}
void Corpus() {
  Add(0x80000000, 0x80000000, 0);
  Add(0x57F, 0x57F, 1);
  records.back().payload[0] = 0x1234;
  for (auto [legacy, header] :
       std::array<std::pair<uint32_t, uint32_t>, 8>{{{0xC0012200, 0xC0016A00},
                                                     {0xC0012201, 0xC0016A01},
                                                     {0xC0032200, 0xC0036A00},
                                                     {0xC0032201, 0xC0036A01},
                                                     {0xC0003600, 0xC0006A02},
                                                     {0xC0003601, 0xC0006A03},
                                                     {0xC0023600, 0xC0026A02},
                                                     {0xC0023601, 0xC0026A03}}}) {
    const uint32_t count = ((legacy >> 16) & 0x3FFF) + 1, prefix = (header & 2) ? 0 : 1;
    Add(legacy, header, count);
    auto& p = records.back().payload;
    p[prefix] = count - prefix == 3 ? 0x30006 : 0x30086;
    if (count - prefix == 3) {
      p[prefix + 1] = 0x40080;
      p[prefix + 2] = 0x80000006;
    }
  }
  Add(0x57F, 0x57F, 1);
  records.back().payload[0] = 0x5678;
  for (uint32_t predicate : {0u, 1u}) {
    Add(0xC0003600 | predicate, 0xC0007B00 | predicate, 1);
    records.back().payload[0] = 0x10081;
    Add(0xC0012700 | predicate, 0xC0016B00 | predicate, 2);
    records.back().payload = {0x50000 | predicate, 9};
  }
  for (uint32_t header : {0xC0026C00u, 0xC0026C02u}) {
    Add(0xC0022F00, header, 3);
    records.back().payload = {0x60000, 0x40000, 16};
  }
  for (uint32_t stage : {0u, 1u})
    for (auto [offset, count] :
         std::array<std::pair<uint32_t, uint32_t>, 4>{{{0, 16}, {16, 32}, {1008, 16}, {0, 1024}}}) {
      uint32_t header, reg = 0x4000 + stage * 0x400 + offset;
      Require(rex::graphics::native_gpu_constants::EncodeHeader(reg, count, header),
              "Invalid fixture float range");
      Add(((count - 1) << 16) | reg, header, count);
    }
  for (uint32_t page : {0x2000u, 0x2100u, 0x2200u, 0x2300u})
    for (auto [offset, count] :
         std::array<std::pair<uint32_t, uint32_t>, 4>{{{0, 1}, {127, 2}, {240, 16}, {0, 256}}}) {
      uint32_t header;
      Require(rex::graphics::native_gpu_state::EncodeHeader(page + offset, count, header),
              "Invalid fixture state range");
      Add(((count - 1) << 16) | (page + offset), header, count);
    }
  for (auto [reg, count] : std::array<std::pair<uint32_t, uint32_t>, 10>{{{0x4800, 6},
                                                                          {0x4806, 12},
                                                                          {0x48BA, 6},
                                                                          {0x4800, 192},
                                                                          {0x4900, 8},
                                                                          {0x4908, 32},
                                                                          {0x4900, 40},
                                                                          {0x5000, 3},
                                                                          {0x5001, 2},
                                                                          {0x5002, 1}}}) {
    uint32_t header;
    Require(rex::graphics::native_gpu_state::EncodeHeader(reg, count, header),
            "Invalid fixture descriptor range");
    Add(((count - 1) << 16) | reg, header, count);
    if (reg >= 0x4800 && reg < 0x48C0)
      for (uint32_t i = 0; i < count; i += 6) {
        auto& p = records.back().payload;
        p[i] = (i % 12) ? 2 : 0x40083;
        p[i + 1] = (i % 12) ? 0x12345001 : 0x80000010;
        p[i + 2] = 0x100003;
        p[i + 4] = 0x400C3;
        p[i + 5] = 0x23456002;
      }
  }
  for (uint32_t count : {3u, 11u, 16384u})
    Add(0xC0002B00 | ((count - 1) << 16), 0xC0007002 | ((count - 1) << 16), count);
  Add(0xC0003B00, 0xC0007004, 1);
  Add(0xC0006000, 0xC0006F00, 1);
  Add(0xC0006100, 0xC0006F02, 1);
  for (uint32_t count : {2u, 3u, 257u}) {
    Add(0xC0002D01 | ((count - 1) << 16), 0xC0006F05 | ((count - 1) << 16), count);
    records.back().payload[0] = 0x40000;
  }
  Add(0x5C8, 0xC0006E00, 1);
  Add(0xD00, 0xC0006E02, 1);
  Add(0x78D00, 0xC0076E02, 8);
  Add(0xC0025800, 0xC0026D02, 3);
  CompoundCorpus();
  // Existing non-native metadata and commands must keep their old editor view.
  Add(0xC0015A00, 0xC0015A00, 2);
  records.back().payload = {0xCAFE, 0x40040};
  Add(0xC0013F00, 0xC0013F00, 2);
  records.back().payload = {0x30000, 3};
  Add(0x80000000, 0x80000000, 0);
}
void Iterator(uint32_t address, uint32_t seed) {
  Reset();
  const auto count = Emit(address);
  for (uint32_t i = 0; i < 28; i += 4)
    Both(kIterator + i, 0);
  Both(kIterator, address);
  Both(kIterator + 4, address + count * 4);
  auto expected = Context(seed), actual = expected;
  for (uint32_t i = 0;; ++i) {
    Require(i < 10000, "Iterator did not finish or a descriptor group underflowed");
    for (auto* ctx : {&expected, &actual}) {
      ctx->r3.u64 = kIterator;
      ctx->r4.u64 = kTag;
      ctx->r5.u64 = kMeta;
    }
    reference_sub_82458B80(expected, original->data());
    native_sub_82458B80(actual, native->data());
    Equal(expected, actual);
    ++iterator_steps;
    if (!actual.r3.u32)
      break;
  }
}
void Invalidate(uint32_t address, uint32_t seed) {
  Reset();
  auto expected = Context(seed);
  expected.r3.u64 = address;
  expected.r4.u64 = Emit(address);
  expected.r5.u64 = 0x100000;
  expected.r6.u64 = 0x100080;
  auto actual = expected;
  reference_sub_82459260(expected, original->data());
  native_sub_82459260(actual, native->data());
  Equal(expected, actual);
  ++invalidations;
}
void PrepareRecord(uint32_t count, uint32_t flags) {
  Both(kRecord + 108, 1);
  Both(kRecord + 112, kChain);
  Both(kRecord + 116, kDescriptor);
  Both(kRecord + 124, 0);
  Both(kRecord + 128, 0);
  Both(kRecord + 132, 0);
  Both(kRecord + 136, 0x30000);
  Both(kRecord + 140, 0x34000);
  Both(kRecord + 156, 0x18000);
  Both(kDescriptor + 8, count);
  Both(kDescriptor + 12, 0x40000);
  Both(kResource, flags);
  Both(kResource + 24, 0xA0040000);
  Both(kResource + 28, 0x10000);
  Both(kResource + 32, 0x50000);
  Both(kResource + 48, 0x30000);
  Both(kResource + 152, 0x30000);
  Both(kResource + 880, 0x20000);
  Both(kChain, 0);
  Both(kChain + 4, 2);
  Both(kChain + 8, kResource >> 2);
  Both(kChain + 12, 0x100);
  Both(kChain + 16, (kResource >> 2) | 0x80000000);
  Both(kChain + 20, 0x200);
}
void Editor(uint32_t filter, uint32_t flags, uint32_t seed) {
  Reset();
  PrepareRecord(Emit(0xC0040000), flags);
  auto expected = Context(seed);
  expected.r3.u64 = kWriter;
  expected.r4.u64 = kRecord;
  expected.r5.u64 = 8;
  expected.r6.u64 = seed & 1;
  expected.r7.u64 = flags ? kResource : 0;
  expected.r8.u64 = filter;
  expected.r9.u64 = 0x113355;
  auto actual = expected;
  reference_sub_82459A70(expected, original->data());
  native_sub_82459A70(actual, native->data());
  Equal(expected, actual);
  for (uint32_t i = 0;; ++i) {
    Require(i < 10000, "Recorded editor did not finish");
    for (auto* ctx : {&expected, &actual}) {
      ctx->r3.u64 = kWriter;
      ctx->r4.u64 = kTag;
      ctx->r5.u64 = kMeta;
    }
    reference_sub_82459CF0(expected, original->data());
    native_sub_82459CF0(actual, native->data());
    Equal(expected, actual);
    ++iterator_steps;
    if (!actual.r3.u32)
      break;
    if (LoadGuestU32(original->data(), kTag) != 34)
      continue;
    for (auto* ctx : {&expected, &actual}) {
      ctx->r3.u64 = kWriter;
      ctx->r4.u64 = 3;
      ctx->r5.u64 = 0x4567;
    }
    reference_sub_82458818(expected, original->data());
    native_sub_82458818(actual, native->data());
    Equal(expected, actual);
    ++patches;
  }
  expected.r3.u64 = actual.r3.u64 = kWriter;
  reference_sub_82458890(expected, original->data());
  native_sub_82458890(actual, native->data());
  Equal(expected, actual);
}
void DrawPatch(uint32_t filter, uint32_t flags, uint32_t seed) {
  Reset();
  PrepareRecord(Emit(0xC0040000), flags);
  auto expected = Context(seed);
  expected.r3.u64 = kRecord;
  expected.r4.u64 = seed & 1;
  expected.r5.u64 = kResource;
  expected.r6.u64 = filter;
  expected.r7.u64 = 0x113355;
  auto actual = expected;
  reference_sub_8245BB48(expected, original->data());
  native_sub_8245BB48(actual, native->data());
  Equal(expected, actual);
  ++patches;
}
void PatchFamilies(uint32_t filter, uint32_t seed) {
  using Function = void (*)(PPCContext&, uint8_t*);
  const std::array<std::pair<Function, Function>, 10> families = {{
      {reference_sub_8245AF30, native_sub_8245AF30},
      {reference_sub_8245B2E8, native_sub_8245B2E8},
      {reference_sub_8245B410, native_sub_8245B410},
      {reference_sub_8245B578, native_sub_8245B578},
      {reference_sub_8245B798, native_sub_8245B798},
      {reference_sub_8245B9B8, native_sub_8245B9B8},
      {reference_sub_8245BC90, native_sub_8245BC90},
      {reference_sub_8245BE28, native_sub_8245BE28},
      {reference_sub_8245BFC0, native_sub_8245BFC0},
      {reference_sub_8245C0F8, native_sub_8245C0F8},
  }};
  for (uint32_t i = 0; i < families.size(); ++i) {
    Reset();
    PrepareRecord(Emit(0xC0040000), 0);
    Both(kResource + 28, 0x30000);
    if (i == 4) {
      Both(kResource + 32, 0x12345000);
      Both(kResource + 48, 0x23456000);
    }
    auto expected = Context(seed);
    expected.r3.u64 = kRecord;
    expected.r4.u64 = seed & 1;
    expected.r5.u64 = i == 2 ? (seed & 15) : kResource;
    expected.r6.u64 = i == 2 ? 8 : filter;
    expected.r7.u64 = i == 2 ? filter : 0x113355;
    expected.r8.u64 = 0x113355;
    if (i == 0 || i == 1 || i == 3) {
      // These state-only builders take filter/cookie, without a resource.
      expected.r5.u64 = filter;
      expected.r6.u64 = 0x113355;
    }
    auto actual = expected;
    families[i].first(expected, original->data());
    families[i].second(actual, native->data());
    Equal(expected, actual);
    if (!filter)
      if (!LoadGuestU32(native->data(), kRecord + 124) &&
          !LoadGuestU32(native->data(), kRecord + 128))
        throw std::runtime_error("Patch family " + std::to_string(i) +
                                 " did not exercise its matching record");
    ++patches;
  }
}
void Relocate(uint32_t source, uint32_t seed) {
  Reset();
  PrepareRecord(Emit(0xC0060000), 0);
  auto expected = Context(seed);
  expected.r3.u64 = kRecord;
  expected.r4.u64 = 0xC0060000;
  expected.r5.u64 = source;
  auto actual = expected;
  reference_sub_82459630(expected, original->data());
  native_sub_82459630(actual, native->data());
  Equal(expected, actual);
  ++patches;
}
void Copy(uint32_t seed) {
  Reset();
  const size_t split = records.size() / 2;
  const uint32_t first = Emit(0x40004, 0, split), second = Emit(0x70004, split);
  Both(0x40000, 0x40004 + first * 4);
  Both(0x40004 + first * 4, 0x70000);
  Both(0x70000, 0x70004 + second * 4);
  Both(0x70004 + second * 4, 0xC0000000);
  const auto initial = headers;
  for (auto h : initial) {
    h.address = h.address < 0x70000 ? 0xA0004 + (h.address - 0x40004)
                                    : 0xA0004 + first * 4 + (h.address - 0x70004);
    headers.push_back(h);
  }
  const auto spans = compounds;
  for (auto c : spans) {
    c.first = c.first < 0x70000 ? 0xA0004 + (c.first - 0x40004)
                                : 0xA0004 + first * 4 + (c.first - 0x70004);
    compounds.push_back(c);
  }
  auto expected = Context(seed);
  expected.r3.u64 = 0xA0000;
  expected.r4.u64 = 0x40000;
  auto actual = expected;
  reference_sub_824586B0(expected, original->data());
  native_sub_824586B0(actual, native->data());
  Equal(expected, actual);
  Require(actual.r3.u32 == 0xA0000 + (first + second + 2) * 4,
          "Copied buffer returned an unexpected end");
  ++copies;
}
void CopiedIterators() {
  for (size_t record = 0; record < records.size(); ++record) {
    if (records[record].native_payload.empty())
      continue;
    Reset();
    const uint32_t source = 0x40000, destination = 0xC0080000;
    const uint32_t count = Emit(source, record, record + 1);
    Emit(destination, record, record + 1);
    for (uint32_t i = 0; i < 28; i += 4)
      Both(kIterator + i, 0);
    Both(kIterator, source);
    Both(kIterator + 4, source + count * 4);
    auto expected = Context(3), actual = expected;
    for (uint32_t step = 0;; ++step) {
      Require(step < 32, "Aggregate iterator failed to end");
      expected.r3.u64 = actual.r3.u64 = kIterator;
      expected.r4.u64 = actual.r4.u64 = kTag;
      expected.r5.u64 = actual.r5.u64 = kMeta;
      reference_sub_82458B80(expected, original->data());
      native_sub_82458B80(actual, native->data());
      Equal(expected, actual);
      if (!actual.r3.u32)
        break;
      for (auto* memory : {original.get(), native.get()}) {
        std::memcpy(memory->data() + kCopiedIterator, memory->data() + kIterator, 28);
        for (uint32_t field : {0u, 4u, 8u}) {
          const uint32_t value = LoadGuestU32(memory->data(), kCopiedIterator + field);
          StoreGuestU32(memory->data(), kCopiedIterator + field, value + destination - source);
        }
      }
      auto copied_expected = expected, copied_actual = actual;
      for (uint32_t remaining = 0;; ++remaining) {
        Require(remaining < 32, "Relocated iterator failed to end");
        copied_expected.r3.u64 = copied_actual.r3.u64 = kCopiedIterator;
        copied_expected.r4.u64 = copied_actual.r4.u64 = kTag;
        copied_expected.r5.u64 = copied_actual.r5.u64 = kMeta;
        reference_sub_82458B80(copied_expected, original->data());
        native_sub_82458B80(copied_actual, native->data());
        Equal(copied_expected, copied_actual);
        if (!copied_actual.r3.u32)
          break;
      }
      ++copied_iterators;
    }
  }
}
void OpaqueCompound(std::vector<uint32_t> words) {
  Reset();
  constexpr uint32_t source = 0x40000;
  for (uint32_t i = 0; i < words.size(); ++i)
    Both(source + i * 4, words[i]);
  for (uint32_t i = 0; i < 28; i += 4)
    Both(kIterator + i, 0);
  Both(kIterator, source);
  Both(kIterator + 4, source + uint32_t(words.size()) * 4);
  auto expected = Context(1);
  expected.r3.u64 = kIterator;
  expected.r4.u64 = kTag;
  expected.r5.u64 = kMeta;
  auto actual = expected;
  reference_sub_82458B80(expected, original->data());
  native_sub_82458B80(actual, native->data());
  Equal(expected, actual);
  ++rejected_compounds;
}
void RejectedCompounds() {
  using View = simpsons::native_gpu_calls::RecordedIteratorView;
  for (const auto& record : records) {
    if (record.native_payload.empty())
      continue;
    std::vector<uint32_t> valid{record.native};
    valid.insert(valid.end(), record.native_payload.begin(), record.native_payload.end());
    // Every replaced inner header/version must remain canonical. Corrupting
    // one of those metadata words keeps the record opaque, as with old unknown
    // packets, rather than partially exposing its arguments to an editor.
    for (uint32_t i = 1; i < valid.size(); ++i)
      if (record.payload[i - 1] != record.native_payload[i - 1]) {
        auto bad = valid;
        bad[i] ^= 0x01010101;
        OpaqueCompound(std::move(bad));
      }
    auto predicated = valid;
    predicated[0] |= 1;
    OpaqueCompound(std::move(predicated));
    for (uint32_t probe = 0; probe < 10; ++probe) {
      Reset();
      constexpr uint32_t source = 0x40000;
      for (uint32_t i = 0; i < valid.size(); ++i)
        Both(source + i * 4, valid[i]);
      for (uint32_t i = 0; i < 28; i += 4)
        Both(kIterator + i, 0);
      Both(kIterator, source);
      Both(kIterator + 4, source + uint32_t(valid.size()) * 4);
      Both(kIterator + 8, source);
      uint32_t tag = kTag, metadata = kMeta, stack = kStack;
      if (probe == 0)
        Both(kIterator + 4, source + uint32_t(valid.size()) * 4 - 4);
      if (probe == 1)
        Both(kIterator + 4, source + uint32_t(valid.size()) * 4 + 1);
      if (probe == 2)
        tag = source + 4;
      if (probe == 3)
        metadata = source + 4;
      if (probe == 4)
        tag = kIterator + 24;
      if (probe == 5)
        metadata = kIterator;
      if (probe == 6)
        metadata = tag;
      if (probe == 7)
        stack = source + 32;
      if (probe == 8)
        Both(kIterator + 24, 0x90000001);
      if (probe == 9)
        Both(kIterator + 8, source + 4);
      auto expected = Context(2), actual = expected;
      {
        View view(native->data(), kIterator, tag, metadata, stack);
        Require(view.Header(valid[0], source) == valid[0],
                "An unproved aggregate span was exposed");
      }
      Equal(expected, actual);
      ++rejected_compounds;
    }
  }
}
void Unsupported() {
  // Compound records and partial fetch/flush groups need separate contracts.
  for (uint32_t h : {0xC0157D00u, 0xC0197D00u, 0xC00E7A00u, 0xC0107A00u, 0xC0037900u, 0xC0057900u,
                     0xC00B7100u, 0xC0096D00u, 0u, 0xFFFFFFFFu, 0xC0016A04u, 0xC0006A00u,
                     0xC0006B00u, 0xC0016B02u, 0xC0026C01u, 0xC0027002u | 1})
    Require(RecordedLogicalHeader(h) == h, "An unsupported layout was exposed to an editor");
  for (auto [reg, count] : std::array<std::pair<uint32_t, uint32_t>, 4>{
           {{0x4802, 4}, {0x4800, 5}, {0x5000, 1}, {0x5000, 2}}}) {
    uint32_t header;
    Require(rex::graphics::native_gpu_state::EncodeHeader(reg, count, header),
            "Invalid fixture partial state range");
    Require(RecordedLogicalHeader(header) == header, "An incomplete editor group was exposed");
  }
}
}  // namespace
int main(int argc, char** argv) {
  try {
    original = std::make_unique<GuestMemoryFixture>(kSize);
    native = std::make_unique<GuestMemoryFixture>(kSize);
    for (auto* memory : {original.get(), native.get()})
      memory->Commit(kHigh, kHighSize);
    for (uint32_t i = 0; i < kSize; ++i)
      pristine[i] = uint8_t(i * 37 + i / 7);
    Corpus();
    Unsupported();
    CopiedIterators();
    RejectedCompounds();
    for (uint32_t seed : {0u, 1u, 2u, 3u}) {
      ++active_case;
      Iterator(0x40000, seed);
      Iterator(0xC0040000, seed);
      Invalidate(0x40000, seed);
      Invalidate(0xC0040000, seed);
      Copy(seed);
      for (uint32_t filter : {0u, 0x1234u, 0x5678u, 0xDEADu})
        for (uint32_t flags : {0u, 0x400000u}) {
          ++active_case;
          Editor(filter, flags, seed);
          DrawPatch(filter, flags, seed);
        }
      Relocate(0, seed);
      Relocate(0xA0080000, seed);
      PatchFamilies(0, seed);
      PatchFamilies(0x5678, seed);
    }
    char report[768];
    std::snprintf(report, sizeof(report),
                  "{\"passed\":true,\"record_layouts\":%zu,\"context_memory_comparisons\":%llu,"
                  "\"iterator_steps\":%llu,\"address_invalidations\":%llu,"
                  "\"buffer_copies\":%llu,\"patch_checks\":%llu,"
                  "\"continuation_checks\":%llu,\"copied_iterators\":%llu,"
                  "\"rejected_compounds\":%llu,\"game_launched\":false}\n",
                  records.size(), (unsigned long long)comparisons,
                  (unsigned long long)iterator_steps, (unsigned long long)invalidations,
                  (unsigned long long)copies, (unsigned long long)patches,
                  (unsigned long long)continuation_checks, (unsigned long long)copied_iterators,
                  (unsigned long long)rejected_compounds);
    FILE* out = argc == 2 ? std::fopen(argv[1], "wb") : stdout;
    Require(out, "Cannot open recorded buffer report");
    std::fputs(report, out);
    if (out != stdout)
      Require(!std::fclose(out), "Cannot close recorded buffer report");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "Recorded buffer case=%u: %s\n", active_case, e.what());
    return 1;
  }
}
