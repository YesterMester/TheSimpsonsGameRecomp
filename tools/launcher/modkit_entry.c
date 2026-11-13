/* Separate application entry using the package's shared Python/Qt runtime. */
#ifdef _WIN32
#ifndef UNICODE
#define UNICODE 1
#endif
#ifndef _UNICODE
#define _UNICODE 1
#endif
#include <windows.h>
#include <wchar.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR arguments, int show) {
  (void)instance; (void)previous; (void)show;
  wchar_t program[32768], command[32768];
  DWORD length = GetModuleFileNameW(NULL, program, 32768);
  if (!length || length >= 32768) return 1;
  wchar_t *slash = wcsrchr(program, L'\\');
  if (!slash) return 1;
  *slash = L'\0';
  const wchar_t *runtime = L"\\simpsons-launcher.exe";
  if (wcslen(program) + wcslen(runtime) + 1 >= 32768) return 1;
  wcscat(program, runtime);
  int written = swprintf(command, 32768, L"\"%ls\" --modkit %ls", program, arguments);
  if (written < 0 || written >= 32768) return 1;
  STARTUPINFOW startup = {0}; startup.cb = sizeof(startup);
  PROCESS_INFORMATION process = {0};
  SetEnvironmentVariableW(L"PYINSTALLER_RESET_ENVIRONMENT", L"1");
  if (!CreateProcessW(program, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process)) {
    MessageBoxW(NULL, L"Keep Modkit beside simpsons-launcher.exe in the complete game package.",
                L"The Simpsons Game Modkit", MB_OK | MB_ICONERROR);
    return 1;
  }
  CloseHandle(process.hThread); CloseHandle(process.hProcess);
  return 0;
}
#else
#define _XOPEN_SOURCE 700
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
  char program[PATH_MAX];
  ssize_t length = readlink("/proc/self/exe", program, sizeof(program) - 1);
  if (length <= 0 || (size_t)length >= sizeof(program) - 1) return 1;
  program[length] = '\0';
  char *slash = strrchr(program, '/');
  if (!slash || (size_t)(slash - program) + sizeof("/simpsons-launcher") > sizeof(program)) return 1;
  char script[PATH_MAX];
  int count = snprintf(script, sizeof(script), "%.*s/simpsons-modkit.sh", (int)(slash - program), program);
  if (count > 0 && (size_t)count < sizeof(script) && access(script, X_OK) == 0) {
    char **script_args = calloc((size_t)argc + 1, sizeof(char *));
    if (!script_args) return 1;
    script_args[0] = script;
    for (int index = 1; index < argc; ++index) script_args[index] = argv[index];
    setenv("PYINSTALLER_RESET_ENVIRONMENT", "1", 1);
    execv(script, script_args);
    free(script_args);
  }
  strcpy(slash, "/simpsons-launcher");
  char **args = calloc((size_t)argc + 2, sizeof(char *));
  if (!args) return 1;
  args[0] = program; args[1] = "--modkit";
  for (int index = 1; index < argc; ++index) args[index + 1] = argv[index];
  setenv("PYINSTALLER_RESET_ENVIRONMENT", "1", 1);
  execv(program, args);
  perror("Open Modkit from the complete game package"); free(args); return 1;
}
#endif
