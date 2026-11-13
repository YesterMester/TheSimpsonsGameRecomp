#include "developer.h"

#include <algorithm>
#include <array>
#include <random>
#include <set>

namespace simpsons::modkit {
namespace {
std::string RandomId() {
  std::random_device random;
  std::string value;
  constexpr char digits[] = "0123456789abcdef";
  for (int i = 0; i < 8; ++i) {
    auto word = random();
    for (int j = 0; j < 8; ++j) {
      value += digits[word & 15];
      word >>= 4;
    }
  }
  return value;
}
bool RequestId(std::string_view value) {
  return value.size() == 32 &&
         value.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}
Json Error(std::string_view code, std::string_view text) {
  return {{"ok", false}, {"code", code}, {"error", text}};
}
}  // namespace

DeveloperBridge::DeveloperBridge(fs::path root, Log log) : log_(std::move(log)) {
  token_ = RandomId();
  session_ = RandomId().substr(0, 32);
  const auto folder = fs::absolute(std::move(root)) / session_;
  inbox_ = folder / "requests";
  outbox_ = folder / "responses";
  descriptor_ = folder / "session.json";
  fs::create_directories(inbox_);
  fs::create_directories(outbox_);
#if !defined(_WIN32)
  fs::permissions(folder, fs::perms::owner_all, fs::perm_options::replace);
#endif
  WriteText(descriptor_, Json{{"api", 1}, {"session", session_}, {"token", token_}}.dump());
  worker_ = std::thread([this] { Run(); });
}

DeveloperBridge::~DeveloperBridge() {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
  }
  wake_.notify_all();
  if (worker_.joinable())
    worker_.join();
  // Keep responses for clients already waiting for their last result. The
  // descriptor disappearing tells new clients that this session has ended.
  std::error_code error;
  fs::remove(descriptor_, error);
}

void DeveloperBridge::Pump(const std::function<Json(const Json&)>& execute) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
  for (int count = 0; count < 4; ++count) {
    std::shared_ptr<Request> request;
    {
      std::lock_guard lock(mutex_);
      if (pending_.empty() || stopping_)
        break;
      request = pending_.front();
      pending_.pop_front();
    }
    try {
      if (std::chrono::steady_clock::now() >= request->deadline)
        request->reply = Error("request_expired",
                               "The request expired before a game-thread frame could execute it");
      else {
        request->reply = {{"ok", true}, {"value", execute(request->arguments)}};
        if (request->reply.dump().size() > 4 * 1024 * 1024)
          request->reply =
              Error("resource_limit", "Creator responses must fit 4 MiB; narrow the query");
      }
    } catch (const Failure& error) {
      request->reply = Error(error.code, error.what());
    } catch (const std::exception& error) {
      request->reply = Error("error", error.what());
    }
    {
      std::lock_guard lock(mutex_);
      replies_.push_back(std::move(request));
    }
    wake_.notify_one();
    if (std::chrono::steady_clock::now() >= deadline)
      break;
  }
}

void DeveloperBridge::Reply(const std::shared_ptr<Request>& request) {
  auto reply = request->reply;
  reply["id"] = request->id;
  reply["session"] = session_;
  WriteText(outbox_ / (request->id + ".json"), reply.dump());
}

void DeveloperBridge::Run() {
  std::set<std::string> seen;
  for (;;) {
    std::deque<std::shared_ptr<Request>> replies;
    bool stopping;
    {
      std::unique_lock lock(mutex_);
      wake_.wait_for(lock, std::chrono::milliseconds(20),
                     [&] { return stopping_ || !replies_.empty(); });
      stopping = stopping_;
      replies.swap(replies_);
      const auto now = std::chrono::steady_clock::now();
      for (auto it = pending_.begin(); it != pending_.end();) {
        if (!stopping && (*it)->deadline <= now) {
          (*it)->reply = Error("request_expired",
                               "No game-thread frame accepted the request before its deadline");
          replies.push_back(*it);
          it = pending_.erase(it);
        } else
          ++it;
      }
      if (stopping)
        while (!pending_.empty()) {
          auto request = std::move(pending_.front());
          pending_.pop_front();
          request->reply =
              Error("session_closed", "The game closed before this request could execute");
          replies.push_back(std::move(request));
        }
    }
    try {
      for (const auto& request : replies)
        Reply(request);
      if (stopping)
        return;
      size_t visited = 0;
      for (const auto& entry : fs::directory_iterator(inbox_)) {
        if (++visited > 64)
          break;
        if (entry.path().extension() != ".json" || !entry.is_regular_file() || entry.is_symlink())
          continue;
        const auto id = Utf8(entry.path().stem());
        if (!RequestId(id))
          continue;
        if (seen.contains(id)) {
          fs::remove(entry.path());
          continue;
        }
        auto request = std::make_shared<Request>();
        request->id = id;
        bool queued = false;
        try {
          const auto json = Json::parse(ReadText(entry.path(), 1024 * 1024));
          if (!json.is_object() || json.value("api", 0) != 1 || json.value("id", "") != id ||
              json.value("session", "") != session_ || json.value("token", "") != token_)
            throw Failure("unauthorized", "This request belongs to a different creator session");
          if (seen.size() >= 65536)
            throw Failure("resource_limit",
                          "This session has processed 65536 requests; restart the creator session");
          const auto timeout = json.value("timeout_ms", Json(10000));
          if (!timeout.is_number_integer() || timeout.get<int64_t>() < 1 ||
              timeout.get<int64_t>() > 30000)
            throw Failure("invalid_argument", "Creator requests need a timeout from 1 to 30000 ms");
          const auto expiry = json.at("expires_unix_ms");
          if (!expiry.is_number_integer())
            throw Failure("invalid_argument", "A creator request needs an expiry time");
          const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
          if (expiry.get<int64_t>() <= now)
            throw Failure("request_expired", "The client request has expired");
          const auto remaining =
              std::min<int64_t>(timeout.get<int64_t>(), expiry.get<int64_t>() - now);
          request->arguments = json.at("request");
          request->deadline =
              std::chrono::steady_clock::now() + std::chrono::milliseconds(remaining);
          fs::remove(entry.path());
          std::lock_guard lock(mutex_);
          if (pending_.size() >= 64)
            throw Failure("busy", "The creator queue is full; wait for existing requests");
          seen.insert(id);
          pending_.push_back(request);
          queued = true;
        } catch (const Failure& error) {
          request->reply = Error(error.code, error.what());
        } catch (const std::exception& error) {
          request->reply = Error("invalid_request", error.what());
        }
        if (!queued) {
          fs::remove(entry.path());
          Reply(request);
        }
      }
      // Callers remove their replies. Bound abandoned files too, without
      // touching source projects, saves or any directory outside this session.
      std::vector<fs::directory_entry> abandoned;
      for (const auto& entry : fs::directory_iterator(outbox_))
        if (entry.is_regular_file() && entry.path().extension() == ".json")
          abandoned.push_back(entry);
      if (abandoned.size() > 128) {
        std::sort(abandoned.begin(), abandoned.end(), [](const auto& a, const auto& b) {
          return a.last_write_time() < b.last_write_time();
        });
        for (size_t i = 0; i < abandoned.size() - 128; ++i)
          fs::remove(abandoned[i].path());
      }
    } catch (const std::exception& error) {
      if (log_)
        log_(std::string("[modkit:creator] ") + error.what());
      if (stopping)
        return;
    }
  }
}
}  // namespace simpsons::modkit
