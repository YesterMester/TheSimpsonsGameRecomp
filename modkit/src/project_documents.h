#pragma once

#include "runtime.h"

namespace simpsons::modkit {

// Portable authoring data; neither an entity handle nor a saved simulation.
Json EmptyProjectDocument(std::string_view kind);
Json ValidateProjectDocument(std::string_view kind, const Json& document);
Json MissionProjectDefinition(const Json& document);

}  // namespace simpsons::modkit
