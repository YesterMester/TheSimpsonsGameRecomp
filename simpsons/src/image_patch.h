#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <vector>

#include <rex/system/xmemory.h>

namespace simpsons {

struct ImageWordPatch {
  uint32_t address;
  uint32_t original;
  uint32_t replacement;
};

// Check the whole patch and make every affected page writable before changing
// any word. A mismatch or protection failure leaves the image untouched.
inline bool ApplyImagePatch(rex::memory::Memory* memory, std::span<const ImageWordPatch> words) {
  struct Page {
    rex::memory::BaseHeap* heap;
    uint32_t address;
    uint32_t old_protect = 0;
  };
  std::vector<Page> pages;
  for (const auto& word : words) {
    auto* heap = memory->LookupHeap(word.address);
    uint32_t protect = 0;
    if (!heap || !heap->QueryProtect(word.address, &protect) ||
        !(protect & rex::memory::kMemoryProtectRead) || (word.address & 3)) {
      return false;
    }
    uint32_t value;
    std::memcpy(&value, memory->TranslateVirtual(word.address), sizeof(value));
    if (std::byteswap(value) != word.original) {
      return false;
    }
    if (word.original != word.replacement) {
      const uint32_t page = word.address & ~(heap->page_size() - 1);
      if (std::none_of(pages.begin(), pages.end(),
                       [&](const Page& p) { return p.heap == heap && p.address == page; })) {
        pages.push_back({heap, page});
      }
    }
  }
  auto restore = [&](size_t count) {
    bool okay = true;
    while (count) {
      auto& page = pages[--count];
      okay = page.heap->Protect(page.address, page.heap->page_size(), page.old_protect) && okay;
    }
    if (!okay) {
      throw std::runtime_error("Could not restore game image page protection");
    }
  };
  for (size_t i = 0; i < pages.size(); ++i) {
    auto& page = pages[i];
    if (!page.heap->Protect(page.address, page.heap->page_size(),
                            rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite,
                            &page.old_protect)) {
      restore(i);
      return false;
    }
  }
  for (const auto& word : words) {
    if (word.original != word.replacement) {
      const uint32_t value = std::byteswap(word.replacement);
      std::memcpy(memory->TranslateVirtual(word.address), &value, sizeof(value));
    }
  }
  restore(pages.size());
  return true;
}

}  // namespace simpsons
