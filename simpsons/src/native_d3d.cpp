// Native Direct3D layer: hooks on the game's statically linked XDK Direct3D
// code that hand rendering state to the renderer directly instead of encoding
// it as Xbox command packets for the command processor to decode.
//
// Each hook leaves a small marker packet where its packets would have gone,
// so the renderer consumes the native record in exactly the command stream
// order the packets had (see rex/graphics/native_records.h).
//
// native_d3d_constants, native_d3d_registers: 0 = original packets, 1 = native
// records, 2 = verify (write both; the renderer compares the registers the
// packets set against the record and logs mismatches).

#include <bit>
#include <cstdint>
#include <cstring>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/native_records.h>
#include <rex/logging.h>
#include <rex/ppc.h>

REXCVAR_DEFINE_INT32(native_d3d_constants, 0, "GPU",
                     "Shader float constants from the Direct3D device straight to the renderer: "
                     "0 = command packets, 1 = native records, 2 = verify records against packets");

REXCVAR_DEFINE_INT32(native_d3d_registers, 0, "GPU",
                     "Render state and bool constant registers from the Direct3D device straight "
                     "to the renderer: 0 = command packets, 1 = native records, 2 = verify records "
                     "against packets");

REXCVAR_DEFINE_INT32(native_d3d_fetch_constants, 0, "GPU",
                     "Texture and vertex fetch constants from the Direct3D device straight to the "
                     "renderer: 0 = command packets, 1 = native records, 2 = verify records "
                     "against packets");

// Writes the Direct3D device's dirty shader float constants to the command
// buffer: r3 = device, r4 = dirty mask (bit 63 - n covers constants 4n to
// 4n + 3), r5 = first register (0x4000 vertex, 0x4400 pixel), r6 = the
// device's big-endian shadow of the 256 constants. One type-0 packet per run
// of dirty groups. Called by every draw before its draw packet.
REX_EXTERN(__imp__sub_8245EC68);
REX_EXTERN(sub_8245EC68);

// Writes dirty registers from one of the Direct3D device's register shadows:
// r3 = device, r4 = dirty mask (bit 63 - n covers register r5 + n), r5 = first
// register, r6 = the big-endian shadow. One type-0 packet (a header, then the
// values) per run of dirty registers. The draw state flushes (sub_8244BFE8
// and its siblings) call it for each group of render state (0x2000, 0x2100,
// 0x2180, 0x2200, 0x2280, 0x2300, 0x2380) and for the bool constants (0x4900).
REX_EXTERN(__imp__sub_8245E8C8);
REX_EXTERN(sub_8245E8C8);

// Writes the Direct3D device's dirty fetch constants: r3 = device, r4 = dirty
// mask (bit 63 - n covers fetch constant n, registers 0x4800 + 6n to + 5, from
// the device's big-endian shadow at +1152). Per run of dirty constants: a
// type-2 no-op if needed to align the values to 8 bytes, a type-0 header and
// the values. Then a type-0 packet writing registers 0x5000 to 0x5002.
REX_EXTERN(__imp__sub_8245EB08);
REX_EXTERN(sub_8245EB08);

namespace {

namespace native_records = rex::graphics::native_records;

// Direct3D device fields: the command buffer write pointer (at the last
// written dword) and the end of the space reserved for writing.
constexpr uint32_t kDeviceCommandWritePointer = 48;
constexpr uint32_t kDeviceCommandLimit = 52;

uint32_t LoadBE32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, base + address, sizeof(value));
  return rex::byte_swap(value);
}

void StoreBE32(uint8_t* base, uint32_t address, uint32_t value) {
  value = rex::byte_swap(value);
  std::memcpy(base + address, &value, sizeof(value));
}

int ConstantsMode() {
  static const int mode = REXCVAR_GET(native_d3d_constants);
  return mode;
}

int RegistersMode() {
  static const int mode = REXCVAR_GET(native_d3d_registers);
  return mode;
}

int FetchConstantsMode() {
  static const int mode = REXCVAR_GET(native_d3d_fetch_constants);
  return mode;
}

// A Direct3D function writing dirty registers from a device shadow, as type-0
// packets, one per run of set bits in the dirty mask.
struct RegisterWriter {
  void (*original)(PPCContext& __restrict ctx, uint8_t* base);
  // Registers each dirty mask bit covers.
  uint32_t registers_per_bit;
  // Most dwords the original writes per run on top of the values.
  uint32_t packet_overhead_per_run;
};

// The float constant writer aligns its packets: a header and up to 3
// alignment dwords per run.
constexpr RegisterWriter kConstantsWriter = {__imp__sub_8245EC68, 16, 4};
constexpr RegisterWriter kRegistersWriter = {__imp__sub_8245E8C8, 1, 1};

// Only the guest render thread submits draws.
std::vector<uint32_t> g_payload;

// Appends {first register, count, values} for every run of dirty bits, in the
// order the original writes its packets.
void AppendRuns(const uint8_t* base, uint64_t mask, uint32_t first_register, uint32_t shadow,
                uint32_t registers_per_bit) {
  uint32_t bit = 0;
  while (mask) {
    uint32_t clear = uint32_t(std::countl_zero(mask));
    mask <<= clear;
    bit += clear;
    uint32_t run = uint32_t(std::countl_one(mask));
    uint32_t register_count = run * registers_per_bit;
    g_payload.push_back(first_register + bit * registers_per_bit);
    g_payload.push_back(register_count);
    const uint8_t* values = base + shadow + bit * registers_per_bit * sizeof(uint32_t);
    size_t offset = g_payload.size();
    g_payload.resize(offset + register_count);
    std::memcpy(g_payload.data() + offset, values, register_count * sizeof(uint32_t));
    mask = run >= 64 ? 0 : mask << run;
    bit += run;
  }
}

// Worst case of the original's packets for the runs in g_payload.
uint32_t PacketDwordsMax(uint32_t overhead_per_run) {
  uint32_t packet_dwords_max = 0;
  for (size_t i = 0; i < g_payload.size(); i += 2 + g_payload[i + 1]) {
    packet_dwords_max += overhead_per_run + g_payload[i + 1];
  }
  return packet_dwords_max;
}

// Runs the original packet writer with the record in g_payload: records
// replacing the packets (mode 1) or next to them for verification (mode 2).
// r3 must be the device. packet_dwords_max bounds what the original writes.
void EmitRecordAround(PPCContext& __restrict ctx, uint8_t* base, int mode,
                      void (*original)(PPCContext& __restrict ctx, uint8_t* base),
                      uint32_t packet_dwords_max) {
  uint32_t device = ctx.r3.u32;
  constexpr uint32_t kMarkerDwords = 1 + native_records::kMarkerPayloadDwords;
  uint32_t write_pointer = LoadBE32(base, device + kDeviceCommandWritePointer);
  uint32_t limit = LoadBE32(base, device + kDeviceCommandLimit);
  if (write_pointer + (kMarkerDwords + packet_dwords_max + 3) * sizeof(uint32_t) >= limit) {
    // The original would have to make space, possibly in another segment.
    original(ctx, base);
    return;
  }
  if (mode == 2) {
    // Packets first, so their registers are set when the marker is reached.
    original(ctx, base);
    write_pointer = LoadBE32(base, device + kDeviceCommandWritePointer);
    uint32_t sequence = native_records::Push(native_records::RecordType::kRegisterRuns,
                                             native_records::kRecordFlagVerify, g_payload.data(),
                                             g_payload.size());
    StoreBE32(base, write_pointer + 4, native_records::kMarkerHeader);
    StoreBE32(base, write_pointer + 8, sequence);
    StoreBE32(base, write_pointer + 12, 0);
    StoreBE32(base, device + kDeviceCommandWritePointer, write_pointer + 12);
    return;
  }
  // The marker, then the original packets it replaces, executed instead only
  // when the record is gone (a replayed recorded command buffer).
  uint32_t marker = write_pointer + 4;
  StoreBE32(base, marker, native_records::kMarkerHeader);
  StoreBE32(base, marker + 4, 0);
  StoreBE32(base, marker + 8, 0);
  StoreBE32(base, device + kDeviceCommandWritePointer, write_pointer + kMarkerDwords * 4);
  original(ctx, base);
  uint32_t packets_end = LoadBE32(base, device + kDeviceCommandWritePointer);
  uint32_t packets_start = write_pointer + kMarkerDwords * 4;
  if (packets_end < packets_start || packets_end - packets_start > packet_dwords_max * 4) {
    // Not contiguous after all: leave the marker empty, the packets execute.
    return;
  }
  uint32_t sequence = native_records::Push(native_records::RecordType::kRegisterRuns, 0,
                                           g_payload.data(), g_payload.size());
  StoreBE32(base, marker + 4, sequence);
  StoreBE32(base, marker + 8, (packets_end - packets_start) / 4);
}

void HookRegisterWriter(PPCContext& __restrict ctx, uint8_t* base, int mode,
                        const RegisterWriter& writer) {
  if (!mode || !native_records::Enabled() || !ctx.r4.u64) {
    writer.original(ctx, base);
    return;
  }
  g_payload.clear();
  AppendRuns(base, ctx.r4.u64, ctx.r5.u32, ctx.r6.u32, writer.registers_per_bit);
  EmitRecordAround(ctx, base, mode, writer.original,
                   PacketDwordsMax(writer.packet_overhead_per_run));
}

void HookFetchConstants(PPCContext& __restrict ctx, uint8_t* base, int mode) {
  if (!mode || !native_records::Enabled() || !ctx.r4.u64) {
    __imp__sub_8245EB08(ctx, base);
    return;
  }
  constexpr uint32_t kShadow = 1152;
  constexpr uint32_t kFirstRegister = 0x4800;
  constexpr uint32_t kRegistersPerConstant = 6;
  g_payload.clear();
  AppendRuns(base, ctx.r4.u64, kFirstRegister, ctx.r3.u32 + kShadow, kRegistersPerConstant);
  // A no-op and a header per run, then the 4 dwords of the closing packet.
  uint32_t packet_dwords_max = PacketDwordsMax(2) + 4;
  // The closing packet, as the original writes it when every run fit in the
  // reserved space (EmitRecordAround only records when they do): header
  // 0x00025000, then 0, 0x00025000, 0 for registers 0x5000 to 0x5002.
  g_payload.push_back(0x5000);
  g_payload.push_back(3);
  g_payload.push_back(0);
  g_payload.push_back(rex::byte_swap(uint32_t(0x00025000)));
  g_payload.push_back(0);
  EmitRecordAround(ctx, base, mode, __imp__sub_8245EB08, packet_dwords_max);
}

}  // namespace

REX_FUNC(sub_8245EC68) {
  HookRegisterWriter(ctx, base, ConstantsMode(), kConstantsWriter);
}

REX_FUNC(sub_8245E8C8) {
  HookRegisterWriter(ctx, base, RegistersMode(), kRegistersWriter);
}

REX_FUNC(sub_8245EB08) {
  HookFetchConstants(ctx, base, FetchConstantsMode());
}
