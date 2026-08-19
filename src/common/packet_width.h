// SPMD packet width in f32 lanes.
// Set by CMake as VLEN/32 on native RISC-V; defaults to 4.
// Must match the runtime PACKET_W.

#pragma once

#ifndef SHADER_PACKET_WIDTH
#define SHADER_PACKET_WIDTH 4   // f32 lanes; VLEN128/32 = 4. Override via -D.
#endif
