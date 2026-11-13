#pragma once
#if defined(SIMPSONS_BUILD_LUA) && !defined(LUA_CORE)
#define LUA_CORE
#endif
// Lua supports C++ exception unwinding. Its public API keeps C linkage while
// errors unwind C++ values in bindings before reaching lua_pcall.
extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}
