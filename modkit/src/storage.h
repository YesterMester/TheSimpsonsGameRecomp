#pragma once

#include "runtime.h"

#include <condition_variable>
#include <mutex>
#include <set>

namespace simpsons::modkit {

// One cache and writer survive a transactional VM replacement. Reads in mod
// callbacks never touch the filesystem; writes become visible immediately and
// are committed in order by the worker. The last owner drains it at shutdown.
class PersistentStorage {
 public:
  PersistentStorage(fs::path root, const std::vector<std::string>& owners);
  ~PersistentStorage();
  Json Read(std::string_view owner, std::string_view path) const;
  void Write(std::string_view owner, std::string_view path, const Json& value);
  Json Status(std::string_view owner) const;
  void Flush();

 private:
  using Key = std::pair<std::string, std::string>;
  struct Value { Json data; size_t bytes = 0; std::string error; };
  Key CheckKey(std::string_view owner, std::string_view path) const;
  void Run();
  fs::path root_;
  std::set<std::string> owners_;
  mutable std::mutex mutex_;
  std::condition_variable wake_, drained_;
  std::map<Key, Value> values_;
  std::map<Key, std::string> paths_;
  std::map<Key, std::string> pending_, errors_;
  std::map<std::string, size_t> sizes_, counts_;
  size_t total_bytes_ = 0;
  std::string writing_;
  bool stopping_ = false;
  std::thread worker_;
};

}  // namespace simpsons::modkit
