# @maciejwojs/screen-capture

Native Node.js addon for screen capture. Packages are published to NPM with provenance (OIDC) and include prebuilt binaries via GitHub Actions.

## Current state

- Windows backend is implemented with `Windows.Graphics.Capture` + D3D11.
- Linux backend is implemented with `xdg-desktop-portal` (D-Bus) and PipeWire stream, supporting modern desktop environments (Wayland/X11).
- Runtime loading uses `node-gyp-build`, so local build and `prebuilds/` binaries are both supported.

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
- Keep per-platform backend in separate `.cpp` files and select in `binding.gyp` conditions.
- Current split is:
	- `src/addon.cpp` - shared N-API wrapper
	- `src/win/platform_capture_win.cpp` - Windows backend
	- `src/linux/platform_capture_linux.cpp` - Linux backend entry point
	- `src/platform_capture_stub.cpp` - fallback for other systems
	- `src/platform_capture.hpp` - common backend interface
- For unsupported combinations, return a clear runtime error (already done).

To add next platform, create next backend file (for example `src/macos/platform_capture_macos.cpp`) and add it in matching `binding.gyp` condition branch.

This gives you one npm package with many prebuilt binaries and no user-side compile step.
