#pragma once

#include "runtime.h"

namespace simpsons::modkit {

// Portable authoring data only: original definition identities and world poses.
// Scene documents never contain guest addresses or temporary template handles.
Json EmptyScene();
Json ValidateScene(const Json& document);
bool ObjectTag(std::string_view tag);

}  // namespace simpsons::modkit
