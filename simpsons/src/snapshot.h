#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

struct PPCContext;
namespace rex {
class Runtime;
}

// Exact saves: the whole game, copied while it is frozen at the start of a
// frame and put back under the running game (snapshot.cpp,
// simpsons/re/exact_saves.md).
namespace snapshot {

// A frozen copy of the running game.
struct Image;
struct ImageDeleter {
  void operator()(Image* image) const;
};
using ImagePtr = std::unique_ptr<Image, ImageDeleter>;

// Game thread, at the start of a frame: gets the game ready to be frozen (its
// audio paused, which can take a frame or two while the game runs on). True
// once Take or Put can run; until then they fail without changing anything.
// Release undoes it when neither follows.
bool Ready();
void Release();
// Game thread, at the start of a frame while a level runs: freezes the game
// for about half a second and copies it. Null if the game can't be frozen.
ImagePtr Take(PPCContext& ctx);
// Game thread, at the start of a frame while a level runs: puts an image under
// the running game, which carries on from the moment it was taken. False, with
// nothing changed, if the game's threads aren't parked where the image's were.
bool Put(PPCContext& ctx, const Image& image);
// Any thread: whether an image can go under this build at all (the memory the
// recompilation set aside for itself is where the image's was).
bool Fits(const Image& image);

struct Stats {
  uint64_t memory = 0;   // guest memory the image holds
  uint64_t stored = 0;   // bytes this write added on disk
  uint32_t chunks = 0;   // non-empty pieces
  uint32_t new_chunks = 0;
};
// Any thread: writes an image's file, with its memory in the shared chunk
// store (deduplicated and compressed).
bool Write(const Image& image, const std::filesystem::path& file,
           const std::filesystem::path& chunks, Stats* stats = nullptr);
// Any thread: reads an image and its memory back. Null if the file or a chunk
// is missing or damaged, or the file was made by an incompatible build.
ImagePtr Read(const std::filesystem::path& file, const std::filesystem::path& chunks);
// Any thread: removes the chunks none of these files use. Returns how many.
size_t CollectChunks(const std::filesystem::path& chunks,
                     const std::vector<std::filesystem::path>& files);

void Init(rex::Runtime* runtime);
// Game thread, at the start of a frame: development commands
// (snapshot_test_file).
void Tick(PPCContext& ctx, uint8_t* base);

}  // namespace snapshot
