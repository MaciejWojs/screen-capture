# @maciejwojs/screen-capture

Native Node.js addon for screen capture. Packages are published to NPM with provenance (OIDC) and include prebuilt binaries via GitHub Actions.

## Current state

- Windows backend is implemented with `Windows.Graphics.Capture` + D3D11.
- Linux backend is implemented with `xdg-desktop-portal` (D-Bus) and PipeWire stream, supporting modern desktop environments (Wayland/X11).
- On Wayland with NVIDIA, the capture may use MemFd and CPU copy when DMA-BUF zero-copy is unavailable.
- Runtime loading uses `node-gyp-build`, so local build and `prebuilds/` binaries are both supported.

## Usage

```javascript
import { ScreenCapture } from '@maciejwojs/screen-capture';

const capture = new ScreenCapture();
capture.start();

// Get texture structure formatted for Electron's shared-texture API:
const textureInfo = capture.getSharedTextureInfo();

// Or get the raw handle data (legacy):
const rawHandle = capture.getSharedHandle();

// On Wayland with NVIDIA MemFd, getPixelData() can copy the frame through CPU:
const frameData = capture.getPixelData();

// You can request output format conversion for supported 4-byte layouts:
const rgbaData = capture.getPixelData('rgba'); // only on wayland

capture.stop();
```

## Install

```bash
bun install
```

## Local build

```bash
bun run build # to compile TypeScript
bun x node-gyp rebuild
```

## Prebuilt binaries (prebuildify)

Build prebuild for current platform:

```bash
bun run prebuildify
```

Build prebuilds for selected platforms:

```bash
bun run prebuildify:all
```

Output goes to `prebuilds/` and is loaded automatically by `dist/index.js`.
Scripts are configured to run `node-gyp` via `node-gyp` internally and build TypeScript beforehand.

## How to add other systems / window managers

Recommended backend mapping:

- Windows: `Windows.Graphics.Capture` (already in place)
- Linux: PipeWire portal (`xdg-desktop-portal`) with DMA-BUF limits (already in place)
- macOS: ScreenCaptureKit (or CGDisplayStream as fallback)

Practical architecture:

- Keep one JS API (`ScreenCapture`) for all OSes.
- Keep one addon target in `binding.gyp`.
- Keep per-platform backend in separate `.cpp` files and select them with `binding.gyp` conditions.
- Current split is:
	- `lib/index.ts` - JavaScript export entry point, compiled to `dist/index.js`
	- `src/addon.cpp` - shared N-API wrapper and native binding glue
	- `src/serialize.cpp` - N-API serialization for Electron shared textures and raw handles
	- `src/win/capture_winrt.cpp` - Windows WinRT capture backend
	- `src/win/capture_dxgi.cpp` - Windows DXGI capture backend
	- `src/win/capture_gdi.cpp` - Windows GDI capture backend
	- `src/win/capture_factory.cpp` - Windows backend selection and helper logic
	- `src/linux/platform_capture_linux.cpp` - Linux portal / PipeWire implementation
	- `src/pixel_conversion.cpp` - color conversion helper for supported pixel layouts
	- `src/platform_capture_stub.cpp` - compile-time fallback for unsupported systems
	- `src/platform_capture.hpp` - common backend interface and shared definitions
- `binding.gyp` also supports `force_api` build flags (`winrt`, `dxgi`, `gdi`) for Windows variants.
- For unsupported combinations, the stub backend returns a clear runtime error.

To add a new platform, implement the backend in a new source file (for example `src/macos/platform_capture_macos.cpp`) and add it to the matching `binding.gyp` condition branch.

This keeps one npm package with both local build support and published `prebuilds/` binaries, without requiring users to compile manually.
