#include "developer.h"

#include <iostream>

using namespace simpsons::modkit;

namespace {
int checks = 0;
void Require(bool condition, const char* text) {
  ++checks;
  if (!condition)
    throw std::runtime_error(text);
}
Json ClientRequest(const Json& session, const std::string& id) {
  return {{"api", 1},
          {"id", id},
          {"session", session.at("session")},
          {"token", session.at("token")},
          {"timeout_ms", 10000},
          {"expires_unix_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                      .count() +
                                  10000},
          {"request", {{"action", "inspect"}}}};
}
}  // namespace

int main() {
  const auto root = fs::temp_directory_path() /
                    ("simpsons-creator-check-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  try {
    fs::path folder, descriptor;
    int executed = 0;
    const std::string id(32, 'a'), wrong(32, 'b'), expired(32, 'c'), closing(32, 'd'),
        queued(32, 'e');
    {
      DeveloperBridge bridge(root);
      descriptor = bridge.SessionFile();
      folder = descriptor.parent_path();
      const auto session = Json::parse(ReadText(descriptor));
      const auto send = [&](const std::string& name, const Json& request) {
        WriteText(folder / "requests" / (name + ".json"), request.dump());
      };
      const auto wait = [&](const std::string& name, bool pump) {
        const auto path = folder / "responses" / (name + ".json");
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!fs::is_regular_file(path) && std::chrono::steady_clock::now() < end) {
          if (pump)
            bridge.Pump([&](const Json& request) {
              Require(request.at("action") == "inspect", "Creator request payload was lost");
              ++executed;
              return Json{{"executed", executed}};
            });
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return Json::parse(ReadText(path));
      };
      send(id, ClientRequest(session, id));
      Require(wait(id, true).at("value").at("executed") == 1 && executed == 1,
              "A creator request must execute on Pump and return its result");
      send(id, ClientRequest(session, id));
      for (int i = 0; i < 50; ++i) {
        bridge.Pump([&](const Json&) {
          ++executed;
          return Json();
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      Require(executed == 1, "Replaying the same request id must not repeat a mutation");
      auto denied = ClientRequest(session, wrong);
      denied["token"] = "wrong-session";
      send(wrong, denied);
      Require(wait(wrong, true).at("code") == "unauthorized" && executed == 1,
              "A different session cannot execute creator operations");
      auto old = ClientRequest(session, expired);
      old["expires_unix_ms"] = 1;
      send(expired, old);
      Require(wait(expired, true).at("code") == "request_expired" && executed == 1,
              "An expired file request must not execute later");
      auto delayed = ClientRequest(session, queued);
      delayed["timeout_ms"] = 20;
      send(queued, delayed);
      Require(wait(queued, false).at("code") == "request_expired",
              "Queued requests expire even when the game thread is paused or unavailable");
      bridge.Pump([&](const Json&) {
        ++executed;
        return Json();
      });
      Require(executed == 1, "An expired queued mutation cannot run after the game thread resumes");
      send(closing, ClientRequest(session, closing));
      const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
      while (fs::exists(folder / "requests" / (closing + ".json")) &&
             std::chrono::steady_clock::now() < end)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Require(!fs::exists(descriptor), "Closing a session must retire its descriptor");
    const auto reply = Json::parse(ReadText(folder / "responses" / (closing + ".json")));
    Require(reply.at("code") == "session_closed",
            "Queued requests must get a shutdown result without running");
    fs::remove_all(root);
    std::cout << checks << " creator bridge checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << "\nEvidence: " << root << '\n';
    return 1;
  }
}
