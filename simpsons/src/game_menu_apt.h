#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

struct PPCContext;
namespace rex {
class Runtime;
}

namespace simpsons {

// The front end's own APT variable and function interface. Calls stay on the
// game's thread and use its object ownership, text layout and movie clips.
class GameMenuApt {
 public:
  explicit GameMenuApt(rex::Runtime* runtime);
  bool ready() const { return scratch_ != 0; }
  void Bind(PPCContext& ctx, uint8_t* base, std::string_view movie);
  uint32_t Get(std::string_view path);
  void Set(std::string_view path, uint32_t value);
  void SetText(std::string_view path, std::string_view text);
  void SetNumber(std::string_view path, int32_t value);
  void SetReal(std::string_view path, float value);
  float Real(std::string_view path);
  std::string Text(std::string_view path);
  int32_t Number(std::string_view path);
  void Invoke(std::string_view method, std::span<const std::string> args = {});
  void SetArray(std::string_view path, std::span<const std::string> items);

 private:
  uint32_t Call(uint32_t function, std::span<const uint32_t> args);
  uint32_t Copy(std::string_view text);
  std::string Path(std::string_view name) const;

  rex::Runtime* runtime_;
  PPCContext* ctx_ = nullptr;
  uint8_t* base_ = nullptr;
  uint32_t scratch_ = 0;
  uint32_t offset_ = 0;
  std::string movie_;
};

}  // namespace simpsons
