#pragma once

#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGISwapChain;

// A small JPEG of the game's picture for the co-driver ("what's that over there?"). It's taken
// from the back buffer before the overlay draws, so our own panels aren't in it. The GPU copy is
// read back a frame or two later, so the game never waits for it.
void capture_request();  // any thread: take one on the next frame

// Render thread: at the top of every Present, before the overlay draws.
void capture_frame(ID3D11Device* dev, ID3D11DeviceContext* ctx, IDXGISwapChain* sc);
// Render thread: drop a copy in flight (the device or swapchain is going away).
void capture_reset();

// Worker thread (COM initialized): waits up to timeout_ms for the requested picture and encodes
// it as a JPEG about `width` pixels wide. False if there was no frame (e.g. the game is minimized).
bool capture_wait_jpeg(std::vector<char>& jpeg, unsigned timeout_ms);
