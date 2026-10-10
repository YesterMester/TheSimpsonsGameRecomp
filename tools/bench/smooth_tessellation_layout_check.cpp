// Standalone check of the shader layouts admitted for character tessellation.
// Uses synthetic disassembly; no game files are needed.
#include <rex/graphics/pipeline/shader/smooth_tessellation.h>

#include <cstdio>
#include <stdexcept>
#include <string>

namespace {
const char* kLayout = R"(
/* 0.0 */ exec
/* 0 */ mul r0, r1, c[a0 + 20]
/* 1 */ dp4 oPos.x___, c0.zxyw, r2
/* 2 */ dp4 oPos._y__, c1.zxyw, r2
/* 3 */ dp4 oPos.__z_, c2.zxyw, r2
/* 4 */ dp4 oPos.___w, c3.zxyw, r2
/* 5 */ dp4 o2.x___, c12.zxyw, r2
/* 6 */ dp4 o2._y__, c13.zxyw, r2
/* 7 */ dp4 o2.__z_, c14.zxyw, r2
/* 8 */ dp3 o1.x___, c12.zxyy, r0.zyxx
/* 9 */ dp3 o1._y__, c13.zxyy, r0.zyxx
/* 10 */ dp3 o1.__z_, c14.zxyy, r0.zyxx
)";

std::string Replace(std::string text, const std::string& from, const std::string& to) {
  size_t offset = text.find(from);
  if (offset == std::string::npos) {
    throw std::runtime_error("Test input not found");
  }
  text.replace(offset, from.size(), to);
  return text;
}
}  // namespace

int main() {
  try {
    unsigned checks = 0;
    auto check = [&](const std::string& text, bool expected, bool skinned = true) {
      rex::graphics::SmoothTessellationLayout layout;
      bool found = rex::graphics::FindSmoothTessellationLayout(text, layout);
      if (found != expected) {
        throw std::runtime_error("Unexpected tessellation eligibility");
      }
      if (found &&
          (layout.position_interpolator != 2 || layout.normal_interpolator != 1 ||
           layout.clip_constant != 0 || layout.world_constant != 12 || layout.skinned != skinned)) {
        throw std::runtime_error("Incorrect position, normal or matrix mapping");
      }
      ++checks;
    };
    check(kLayout, true);
    check(Replace(kLayout, "c[a0 + 20]", "c20"), true, false);
    // Each component must use the same vector and consecutive matrix rows.
    check(Replace(kLayout, "c13.zxyw", "c15.zxyw"), false);
    check(Replace(kLayout, "c13.zxyy", "c16.zxyy"), false);
    check(Replace(kLayout, "c13.zxyw, r2", "c13.zxyw, r3"), false);
    check(Replace(kLayout, "c13.zxyw", "c13.xyzw"), false);
    check(Replace(kLayout, "c0.zxyw", "c0.zxwy"), false);
    // The position and normal must be definite, unmodified outputs.
    check(Replace(kLayout, "dp4 o2._y__", "(p0) dp4 o2._y__"), false);
    check(std::string(kLayout) + "mov o2.x___, r4\n", false);
    check(std::string(kLayout) + "mov oPos.___w, r4\n", false);
    check(Replace(kLayout, "/* 3 */", "mov r2, r5\n/* 3 */"), false);
    check(Replace(kLayout, "/* 9 */", "mov r0, r5\n/* 9 */"), false);
    check(Replace(kLayout, "/* 0.0 */ exec", "/* 0.0 */ cexec"), false);
    check(std::string("/* 0.1 */ jmp L1\n") + kLayout + "label L1\n", false);
    check(std::string(kLayout) + "/* 1.0 */ loop\n", false);
    std::printf("PASS: %u tessellation layout checks\n", checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
