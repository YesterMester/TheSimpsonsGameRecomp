#pragma once

#include <cstdint>

// Preparation before the first game frame: 0 waiting, 1 reading storage,
// 2 translating shaders, 3 compiling pipelines, 4 finished. Counts describe
// actual work completed, including cache hits. Querying never waits for the GPU.
extern "C" void rex_graphics_shader_warmup_progress(uint32_t* done, uint32_t* total,
                                                    uint32_t* phase);

// Also complete the loading screen when persistent shader storage is disabled.
extern "C" void rex_graphics_shader_warmup_finish();
