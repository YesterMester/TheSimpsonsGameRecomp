#ifndef SIMPSONS_MODKIT_H
#define SIMPSONS_MODKIT_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define SIM_MOD_EXPORT __declspec(dllexport)
#else
#define SIM_MOD_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Only C types cross the plugin boundary. Strings are UTF-8 and borrowed for
 * the duration of the call. A writer must consume its bytes before returning.
 * Calls into the game are permitted only from host-dispatched callbacks. */
#define SIM_MOD_ABI_VERSION 1u
typedef void (*SimModWriter)(void* user, const char* bytes, size_t size);
typedef int32_t SimModStatus;
enum {
  SIM_MOD_OK = 0,
  SIM_MOD_INVALID_ARGUMENT = 1,
  SIM_MOD_NOT_READY = 2,
  SIM_MOD_STALE_HANDLE = 3,
  SIM_MOD_UNSUPPORTED = 4,
  SIM_MOD_ERROR = 5,
  SIM_MOD_WRONG_THREAD = 6
};

typedef struct SimModHost {
  uint32_t struct_size;
  uint32_t abi_version;
  void* context;
  /* The reply is {"ok":true,"value":...} or
   * {"ok":false,"code":"...","error":"..."}. Exactly one reply is
   * delivered, including on failure. A mutating operation runs only once. */
  SimModStatus (*call)(void* context, const char* method, const char* arguments,
                       SimModWriter reply, void* user);
  void (*log)(void* context, const char* message);
} SimModHost;

typedef struct SimModPlugin {
  uint32_t struct_size;
  uint32_t abi_version;
  void* context;
  /* Events carry JSON data. Return nonzero to disable this plugin. */
  int32_t (*event)(void* context, const char* name, const char* data);
  /* State must be a JSON object, with no host pointers or game handles.
   * Both callbacks are mandatory for save_mode="state". */
  int32_t (*save_state)(void* context, SimModWriter writer, void* user);
  int32_t (*load_state)(void* context, const char* json);
  void (*shutdown)(void* context);
} SimModPlugin;

/* Export exactly this entry point from each native plugin. It must not
 * change game state; register the descriptor and wait for initialize. */
typedef int32_t (*SimModEntry)(const SimModHost* host, SimModPlugin* plugin);

#ifdef __cplusplus
}
#endif
#endif
