#pragma once

#include "text_table.h"

namespace rex {
class Runtime;
}
namespace modkit_text {
void Init(rex::Runtime* runtime, const simpsons::modkit::GameTextTables& tables);
void Invalidate();
void Shutdown();
}  // namespace modkit_text
