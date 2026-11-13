#if defined(_WIN32)
// Laptop drivers read these exports before the graphics provider chooses an
// adapter. Keep the request in the game executable rather than the launcher.
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 1;
__declspec(dllexport) unsigned long AmdPowerXpressRequestHighPerformance = 1;
}
#endif
