#pragma once

#include "runtime.h"

#include <condition_variable>
#include <deque>
#include <mutex>

namespace simpsons::modkit {

// A local creator session. Its worker only reads/writes request files; Pump
// executes bounded requests on the game's owning thread. Shipping play does
// not create a session or poll the filesystem unless explicitly enabled.
class DeveloperBridge {
 public:
  explicit DeveloperBridge(fs::path root, Log log = {});
  ~DeveloperBridge();
  DeveloperBridge(const DeveloperBridge&) = delete;
  DeveloperBridge& operator=(const DeveloperBridge&) = delete;
  void Pump(const std::function<Json(const Json&)>& execute);
  const fs::path& SessionFile() const { return descriptor_; }

 private:
  struct Request {
    std::string id;
    Json arguments, reply;
    std::chrono::steady_clock::time_point deadline;
  };
  void Run();
  void Reply(const std::shared_ptr<Request>& request);
  fs::path descriptor_, inbox_, outbox_;
  std::string token_, session_;
  Log log_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<std::shared_ptr<Request>> pending_, replies_;
  bool stopping_ = false;
  std::thread worker_;
};

}  // namespace simpsons::modkit
