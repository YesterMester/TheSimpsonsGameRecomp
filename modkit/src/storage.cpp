#include "storage.h"

#include <algorithm>

namespace simpsons::modkit {
namespace {
constexpr size_t kFileBytes = 1024 * 1024;
constexpr size_t kOwnerBytes = 16 * kFileBytes;
constexpr size_t kOwnerFiles = 256;
constexpr size_t kTotalBytes = 64 * kFileBytes;
}

PersistentStorage::Key PersistentStorage::CheckKey(std::string_view owner,
                                                  std::string_view path) const {
  if (!owners_.contains(std::string(owner)))
    throw Failure("invalid_argument", "Unknown storage owner");
  RelativePath(path);
  if (path.empty() || path.size() > 256 || path.front() == '/' ||
      path.find_first_of("\\:") != std::string_view::npos ||
      path.find('\0') != std::string_view::npos)
    throw Failure("invalid_argument", "Storage needs a relative file path");
  size_t first = 0, depth = 0;
  while (first <= path.size()) {
    const auto end = path.find('/', first);
    const auto part = path.substr(first, end == std::string_view::npos ? end : end - first);
    if (part.empty() || part.front() == '.' || ++depth > 8)
      throw Failure("invalid_argument", "Storage paths cannot contain hidden, empty or parent components");
    if (end == std::string_view::npos) break;
    first = end + 1;
  }
  if (path.ends_with(".part"))
    throw Failure("invalid_argument", "The .part suffix is reserved for atomic storage writes");
  std::string canonical(path);
  for (char& c : canonical) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  return {std::string(owner), std::move(canonical)};
}

PersistentStorage::PersistentStorage(fs::path root, const std::vector<std::string>& owners)
    : root_(std::move(root)), owners_(owners.begin(), owners.end()) {
  // Called while loading packages, before any game-thread callbacks start.
  for (const auto& owner : owners_) {
    const auto folder = root_ / owner;
    if (fs::is_symlink(folder)) throw Failure("invalid_path", "Mod storage cannot be a symlink");
    if (!fs::is_directory(folder)) continue;
    for (const auto& entry : fs::recursive_directory_iterator(folder)) {
      if (entry.is_symlink()) throw Failure("invalid_path", "Mod storage cannot contain symlinks");
      if (!entry.is_regular_file()) continue;
      const auto relative = Utf8(entry.path().lexically_relative(folder));
      if (relative.ends_with(".part")) continue;
      const auto key = CheckKey(owner, relative);
      if (paths_.contains(key)) throw Failure("invalid_path", "Mod storage paths differ only in letter case");
      paths_[key] = relative;
      const auto bytes = entry.file_size();
      if (++counts_[owner] > kOwnerFiles || bytes > kFileBytes ||
          sizes_[owner] + bytes > kOwnerBytes || total_bytes_ + bytes > kTotalBytes)
        throw Failure("resource_limit", "Mod storage exceeds its file or size budget: " + owner);
      sizes_[owner] += bytes;
      total_bytes_ += bytes;
      auto& value = values_[key];
      value.bytes = bytes;
      try { value.data = Json::parse(ReadText(Inside(folder, relative), kFileBytes)); }
      catch (const std::exception& error) { value.error = error.what(); }
    }
  }
  worker_ = std::thread([this] { Run(); });
}

PersistentStorage::~PersistentStorage() {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
  }
  wake_.notify_one();
  if (worker_.joinable()) worker_.join();
}

Json PersistentStorage::Read(std::string_view owner, std::string_view path) const {
  const auto key = CheckKey(owner, path);
  std::lock_guard lock(mutex_);
  const auto found = values_.find(key);
  if (found == values_.end()) return nullptr;
  if (!found->second.error.empty()) throw Failure("storage_error", found->second.error);
  return found->second.data;
}

void PersistentStorage::Write(std::string_view owner, std::string_view path, const Json& value) {
  const auto key = CheckKey(owner, path);
  auto text = value.dump();
  if (text.size() > kFileBytes)
    throw Failure("file_too_large", "Mod storage files are limited to 1 MiB");
  std::lock_guard lock(mutex_);
  const auto found = values_.find(key);
  const auto previous = found == values_.end() ? 0 : found->second.bytes;
  if ((found == values_.end() && counts_[key.first] >= kOwnerFiles) ||
      sizes_[key.first] - previous + text.size() > kOwnerBytes ||
      total_bytes_ - previous + text.size() > kTotalBytes)
    throw Failure("resource_limit", "Mod storage is limited to 256 files and 16 MiB per mod");
  if (found == values_.end()) {
    ++counts_[key.first];
    paths_[key] = std::string(path);
  }
  sizes_[key.first] = sizes_[key.first] - previous + text.size();
  total_bytes_ = total_bytes_ - previous + text.size();
  values_[key] = {value, text.size(), {}};
  pending_[key] = std::move(text);  // Keep only the latest queued value per file.
  wake_.notify_one();
}

Json PersistentStorage::Status(std::string_view owner) const {
  std::lock_guard lock(mutex_);
  size_t pending = writing_ == owner ? 1 : 0;
  for (const auto& [key, text] : pending_) if (key.first == owner) ++pending;
  Json errors = Json::array();
  for (const auto& [key, error] : errors_)
    if (key.first == owner) errors.push_back({{"path", key.second}, {"message", error}});
  return {{"pending", pending}, {"errors", std::move(errors)}};
}

void PersistentStorage::Flush() {
  std::unique_lock lock(mutex_);
  drained_.wait(lock, [this] { return pending_.empty() && writing_.empty(); });
}

void PersistentStorage::Run() {
  std::unique_lock lock(mutex_);
  for (;;) {
    wake_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
    if (pending_.empty() && stopping_) break;
    auto next = pending_.extract(pending_.begin());
    writing_ = next.key().first;
    const auto path = paths_.at(next.key());
    lock.unlock();
    std::string error;
    try { WriteText(Inside(root_ / next.key().first, path), next.mapped()); }
    catch (const std::exception& failure) { error = failure.what(); }
    lock.lock();
    if (error.empty()) errors_.erase(next.key());
    else errors_[next.key()] = std::move(error);
    writing_.clear();
    if (pending_.empty()) drained_.notify_all();
  }
}

}  // namespace simpsons::modkit
