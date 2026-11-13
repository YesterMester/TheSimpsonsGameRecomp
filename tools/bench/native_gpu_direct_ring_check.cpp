#include "simpsons_init.h"
#include "native_gpu_calls.h"
#include "guest_memory_fixture.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>

#include "native_gpu_direct_ring_reference.inc"

using simpsons::LoadGuestU32;
using simpsons::StoreGuestU32;
namespace direct = rex::graphics::native_gpu_direct;
namespace port = simpsons::native_gpu_calls;
namespace {
constexpr std::array<std::pair<uint32_t, uint32_t>, 1> state_headers = {
    std::pair{0x2102u, 0xC0007304u}};
constexpr uint32_t kSize = 1024 * 1024, kDevice = 0x1000, kRecorded = 0x20000;
constexpr uint32_t kRecordedAlias = 0xA0020000;
std::unique_ptr<GuestMemoryFixture> original, native;
std::array<uint8_t, kSize> pristine;
constexpr std::array<std::pair<uint32_t, uint32_t>, 8> headers = {
    std::pair{0xC0012200u, 0xC0016A00u},
    {0xC0012201, 0xC0016A01},
    {0xC0032200, 0xC0036A00},
    {0xC0032201, 0xC0036A01},
    {0xC0003600, 0xC0006A02},
    {0xC0003601, 0xC0006A03},
    {0xC0023600, 0xC0026A02},
    {0xC0023601, 0xC0026A03}};
uint64_t cases = 0, lowered = 0, rejected = 0;
uint64_t recorded_cases = 0, recorded_headers = 0, view_edge_headers = 0;
void Require(bool ok, const char* why) {
  if (!ok)
    throw std::runtime_error(why);
}
void Check(uint32_t store, uint32_t destination, uint32_t legacy, uint32_t converted,
           uint32_t probe, bool recorded = false) {
  std::memcpy(original->data(), pristine.data(), kSize);
  StoreGuestU32(original->data(), kDevice + 48, destination - 64);
  StoreGuestU32(original->data(), kDevice + 10940, 0);
  StoreGuestU32(original->data(), kDevice + 13216, 0);
  StoreGuestU32(original->data(), kDevice + 13492, 0);
  const uint32_t bytes = (((legacy >> 16) & 0x3FFF) + 2) * 4;
  StoreGuestU32(original->data(), kDevice + 52, destination + bytes);
  const uint32_t record = probe == 17 && recorded ? kRecordedAlias : kRecorded;
  if (recorded) {
    if (record == kRecordedAlias)
      std::memcpy(simpsons::GuestToHost(original->data(), record), pristine.data() + kRecorded,
                  184);
    StoreGuestU32(original->data(), kDevice + 13492, record);
    StoreGuestU32(original->data(), record + 108, 2);
    // Use a different physical view for each of the three alias destinations.
    const uint32_t owner = destination == 0xA0040000   ? 0xC003FFE0
                           : destination == 0xC0040000 ? 0xE003EFE0
                           : destination == 0xE003F000 ? 0xA003FFE0
                                                       : destination - 32;
    StoreGuestU32(original->data(), record + 152, owner);
    StoreGuestU32(original->data(), record + 156, 128);
    switch (probe) {
      case 13:
        StoreGuestU32(original->data(), record + 152, destination);
        StoreGuestU32(original->data(), record + 156, bytes);
        break;
      case 14:
        StoreGuestU32(original->data(), record + 156, bytes + 32);
        break;
      case 15:
        StoreGuestU32(original->data(), kDevice + 48, destination - 4);
        break;
      case 16:
        StoreGuestU32(original->data(), record + 108, 0x1802);
        break;
      case 18:
      case 19:
      case 20:
        StoreGuestU32(original->data(), record + 108, probe == 18 ? 1 : probe == 19 ? 0 : 7);
        break;
      case 21:
        StoreGuestU32(original->data(), record + 152, 0);
        break;
      case 22:
        StoreGuestU32(original->data(), record + 156, 0);
        break;
      case 23:
        StoreGuestU32(original->data(), record + 152, owner + 1);
        break;
      case 24:
        StoreGuestU32(original->data(), record + 156, 127);
        break;
      case 25:
        StoreGuestU32(original->data(), record + 152, destination);
        StoreGuestU32(original->data(), record + 156, bytes - 4);
        break;
      case 26:
        StoreGuestU32(original->data(), record + 152, destination + 4);
        break;
      case 27:
        StoreGuestU32(original->data(), kDevice + 13492, record + 1);
        break;
      case 28:
        StoreGuestU32(original->data(), kDevice + 13492, 0x7EFFFF80);
        break;
      case 29:
        StoreGuestU32(original->data(), kDevice + 13492, kDevice + 64);
        break;
      case 30:
        StoreGuestU32(original->data(), kDevice + 13492, destination);
        break;
      case 31:
        StoreGuestU32(original->data(), record + 152, kDevice);
        StoreGuestU32(original->data(), record + 156, 0x80000);
        break;
      case 32:
        StoreGuestU32(original->data(), record + 152, kRecorded);
        StoreGuestU32(original->data(), record + 156, 0x40000);
        break;
      case 33:
        StoreGuestU32(original->data(), record + 152, 0x7EFFFFF0);
        StoreGuestU32(original->data(), record + 156, 64);
        break;
      case 34:
        StoreGuestU32(original->data(), record + 152, destination < kSize ? 0xA003FFE0 : 0x3FFE0);
        break;
      case 35:
        StoreGuestU32(original->data(), record + 152, 0xFFFFEFF0);
        StoreGuestU32(original->data(), record + 156, 64);
        break;
      case 36:
        StoreGuestU32(original->data(), record + 152, 0xFFFFFFFC);
        StoreGuestU32(original->data(), record + 156, 8);
        break;
      case 37:
        StoreGuestU32(original->data(), kDevice + 52, destination + bytes - 4);
        break;
      case 38:
      case 39:
        StoreGuestU32(original->data(), record + 152, destination);
        StoreGuestU32(original->data(), record + 156, probe == 38 ? bytes : bytes - 4);
        StoreGuestU32(original->data(), kDevice + 48, destination - 4);
        break;
    }
  }
  uint32_t device = kDevice;
  if (probe == 1)
    StoreGuestU32(original->data(), kDevice + 52, destination + bytes - 4);
  if (probe == 2)
    StoreGuestU32(original->data(), kDevice + 48, destination);
  if (probe == 3)
    StoreGuestU32(original->data(), kDevice + 48, destination + 4);
  if (probe == 4)
    device += 1;
  if (probe == 5)
    destination += 1;
  if (probe == 7)
    *simpsons::GuestToHost(original->data(), kDevice + 10940) = 1;
  if (probe == 8)
    StoreGuestU32(original->data(), kDevice + 13216, 1);
  if (probe == 9)
    StoreGuestU32(original->data(), kDevice + 13492, 1);
  if (probe == 10)
    StoreGuestU32(original->data(), kDevice + 48, destination - 63);
  if (probe == 11)
    StoreGuestU32(original->data(), kDevice + 52, destination + bytes + 1);
  if (probe == 12)
    StoreGuestU32(original->data(), kDevice + 48, destination >= kSize ? 0x20000 : 0x7F000000);
  std::memcpy(native->data(), original->data(), kSize);
  if (recorded && record == kRecordedAlias)
    std::memcpy(simpsons::GuestToHost(native->data(), record),
                simpsons::GuestToHost(original->data(), record), 184);
  if (destination >= kSize)
    for (uint32_t i = 0; i < 64; ++i) {
      simpsons::GuestToHost(original->data(), destination)[int64_t(i) - 16] = uint8_t(i * 37);
      simpsons::GuestToHost(native->data(), destination)[int64_t(i) - 16] = uint8_t(i * 37);
    }
  PPCContext expected{};
  expected.r31.u64 = 0xFFFF000000000000ull | device;
  (expected.*header_registers[store]).u64 = 0xEEEEFFFF00000000ull | legacy;
  expected.xer.ca = 1;
  expected.cr0.compare<int32_t>(-5, 7, expected.xer);
  auto actual = expected;
  reference_stores[store](expected, original->data(), destination);
  port::direct_draw_headers_enabled = probe != 6;
  port::direct_state_headers_enabled = probe != 6;
  patched_stores[store](actual, native->data(), destination);
  const bool accept = probe == 0 || (recorded && ((probe >= 13 && probe <= 17) || probe == 38));
  const uint32_t wanted = accept ? converted : legacy;
  Require(LoadGuestU32(native->data(), destination) == wanted,
          "Draw store lowered an unsafe span or left a valid live/recorded draw unchanged");
  Require(!std::memcmp(&expected, &actual, sizeof(actual)),
          "Draw header store changed the complete CPU context");
  StoreGuestU32(native->data(), destination, legacy);
  Require(!std::memcmp(original->data(), native->data(), kSize),
          "Draw header changed another word, metadata or padding");
  if (destination >= kSize)
    Require(!std::memcmp(simpsons::GuestToHost(original->data(), destination) - 16,
                         simpsons::GuestToHost(native->data(), destination) - 16, 64),
            "Aliased draw header changed another word");
  if (recorded && record == kRecordedAlias)
    Require(!std::memcmp(simpsons::GuestToHost(original->data(), record),
                         simpsons::GuestToHost(native->data(), record), 184),
            "Aliased recorded object changed during a draw header store");
  ++cases;
  lowered += accept;
  rejected += !accept;
  recorded_cases += recorded;
  recorded_headers += recorded && accept;
  view_edge_headers += recorded && accept && probe == 38;
}
void ConsumerChecks() {
  port::direct_state_headers_enabled = true;
  for (uint32_t header : {0u, UINT32_MAX, 0x12102u, 0x80002102u, 0xC0002102u})
    Require(port::DirectRingStateHeader(native->data(), kDevice, 0x40000, header) == header,
            "An unsupported scalar state layout was lowered");
  Require(!port::OwnsDirectRingSpan(native->data(), kDevice, 0x40000, UINT32_MAX),
          "A wrapping command byte count was accepted");
  for (auto [legacy, native_header] : headers) {
    uint32_t encoded = 0;
    Require(direct::EncodeLegacyHeader(legacy, encoded) && encoded == native_header,
            "Legacy header changed length, predicate or query layout");
    const uint32_t count = ((legacy >> 16) & 0x3FFF) + 1, prefix = (native_header & 2) ? 0 : 1;
    for (bool indexed : {false, true}) {
      std::array<uint32_t, 4> words = {0xA5A50100, 0, 0, 0};
      words[prefix] = indexed ? 0x00030806 : 0x00030086;
      if (count - prefix == 3) {
        words[prefix + 1] = 0x120002;
        words[prefix + 2] = 0x80000006;
      }
      direct::Call call{};
      const bool ok = direct::Decode(encoded, {words.data(), count}, call);
      Require(ok == (!indexed || count - prefix == 3), "Direct argument length was misread");
      if (ok)
        Require(
            call.control == words[prefix] && call.query == (prefix ? words[0] : 0) &&
                call.index_base == (indexed ? words[prefix + 1] : 0) &&
                call.index_size == (indexed ? words[prefix + 2] : 0),
            "Direct arguments lost query/control, halfword indices or unused-operand semantics");
    }
    for (uint32_t mask : {4u, 0x80u, 0x8000u, 0x40000000u})
      Require(!direct::IsValidHeader(native_header ^ mask, count),
              "Malformed direct header accepted");
  }
  for (uint32_t header : {0u, UINT32_MAX, 0xC0012202u, 0xC0002200u, 0xC0013600u}) {
    uint32_t unchanged = 0x91E10DA5;
    Require(!direct::EncodeLegacyHeader(header, unchanged) && unchanged == 0x91E10DA5,
            "Invalid header changed the conversion output");
  }
  port::direct_draw_headers_enabled = true;
  Require(
      port::DirectRingDrawHeader(nullptr, kDevice, 0x40000, 0xC0012201) == 0xC0012201 &&
          port::DirectRingDrawHeader(native->data(), kDevice, 0xFFFFFFFC, 0xC0032201) == 0xC0032201,
      "Invalid backing or address overflow was lowered");
}
}  // namespace
std::span<const std::pair<uint32_t, uint32_t>> HeadersForStore(uint32_t store) {
  return state_stores[store] ? std::span<const std::pair<uint32_t, uint32_t>>(state_headers)
                             : std::span<const std::pair<uint32_t, uint32_t>>(headers);
}

int main(int argc, char** argv) {
  try {
    original = std::make_unique<GuestMemoryFixture>(kSize);
    native = std::make_unique<GuestMemoryFixture>(kSize);
    for (auto* memory : {original.get(), native.get()})
      for (uint32_t address : {kRecordedAlias, 0xA0040000u, 0xC0040000u, 0xE003F000u, 0x7F000000u,
                               0x80000000u, 0x90000000u, 0xA0000000u, 0xC0000000u, 0xE0000000u})
        memory->Commit(address - 16, 8192);
    for (uint32_t i = 0; i < kSize; ++i)
      pristine[i] = uint8_t(i * 37 + i / 7);
    for (uint32_t store = 0; store < patched_stores.size(); ++store)
      for (uint32_t destination : {0x40000u, 0xA0040000u, 0xC0040000u, 0xE003F000u})
        for (auto [legacy, converted] : HeadersForStore(store))
          for (uint32_t probe = 0; probe < 13; ++probe)
            Check(store, destination, legacy, converted, probe);
    for (uint32_t store = 0; store < patched_stores.size(); ++store)
      for (uint32_t destination : {0x40000u, 0xA0040000u, 0xC0040000u, 0xE003F000u})
        for (auto [legacy, converted] : HeadersForStore(store))
          for (uint32_t probe = 0; probe < 38; ++probe)
            Check(store, destination, legacy, converted, probe, true);
    for (uint32_t store = 0; store < patched_stores.size(); ++store)
      for (uint32_t destination :
           {0x7F000000u, 0x80000000u, 0x90000000u, 0xA0000000u, 0xC0000000u, 0xE0000000u})
        for (auto [legacy, converted] : HeadersForStore(store))
          for (uint32_t probe : {38u, 39u})
            Check(store, destination, legacy, converted, probe, true);
    ConsumerChecks();
    char report[512];
    std::snprintf(report, sizeof(report),
                  "{\"passed\":true,\"cases\":%llu,\"lowered_headers\":%llu,\"unchanged_"
                  "rejections\":%llu,\"actual_header_stores\":%zu,\"recorded_cases\":%llu,"
                  "\"recorded_headers\":%llu,\"view_edge_headers\":%llu}\n",
                  (unsigned long long)cases, (unsigned long long)lowered,
                  (unsigned long long)rejected, patched_stores.size(),
                  (unsigned long long)recorded_cases, (unsigned long long)recorded_headers,
                  (unsigned long long)view_edge_headers);
    FILE* out = argc == 2 ? std::fopen(argv[1], "wb") : stdout;
    Require(out, "Cannot open direct ring report");
    std::fputs(report, out);
    if (out != stdout)
      Require(!std::fclose(out), "Cannot close direct ring report");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "Direct ring: %s\n", e.what());
    return 1;
  }
}
