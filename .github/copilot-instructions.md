---
name: "Workspace instructions for screen-capture"
description: "Workspace instructions for @maciejwojs/screen-capture: native addon build, platform backend conventions, packaging workflows, and C++20 compliance."
applyTo:
  "**/*.ts,**/*.js,**/*.cpp,**/*.hpp,**/*.h"
---

# Workspace instructions

## When to use these instructions

- Changing the native Node addon or platform-specific capture backend.
- Updating build, packaging, or prebuild workflows.
- Improving runtime loading, ABI stability, or TypeScript integration.
- Debugging crashes, frame drops, memory issues, or capture pipeline bugs.
- Modifying any C++ code in `src/`.

---

## Project overview

This repository implements a cross-platform native Node.js screen capture addon.

Core layers:
- `lib/index.ts` — JavaScript API entry point.
- `src/addon.cpp` — N-API bridge.
- `src/serialize.cpp`, `src/serialize.hpp` — frame serialization.
- `src/platform_capture.hpp` — backend interface.
- `src/linux/` and `src/win/` — OS-specific capture implementations.

Build tooling:
- `node-gyp-build` for local native builds.
- `prebuildify` for prebuilt binaries.

---

## Screen capture pipeline (critical)

Always reason in this order:
1. Platform capture backend (Windows / Linux)
2. Frame acquisition in native code
3. Frame serialization (`serialize.cpp`)
4. N-API bridge (`addon.cpp`)
5. JS API surface (`lib/index.ts`)

If a bug involves capture data, trace it through these layers before changing any API.

---

## C++ Language Standard (MANDATORY)

The entire native codebase is written in **C++20** (`-std=c++20` / `-std=gnu++20`).

- All `.cpp` and `.hpp` files in `src/` and its subdirectories must be valid C++20.
- Prefer C++20 features that improve clarity, safety, or maintainability, as long as they do not violate performance constraints:
  - `std::span` for buffer views instead of raw pointer + length pairs (e.g., frame data)
  - `std::format` / `std::print` where logging is needed, but **only outside hot paths** (never in the capture loop)
  - `concepts` to constrain template interfaces if templates are used
  - `constexpr` / `consteval` for compile-time computations
  - structured bindings, designated initializers, etc.
- Do NOT use features not yet widely supported by compilers targeting Windows (MSVC 2022) and Linux (GCC 12+, Clang 16+). Stick to C++20 features available in the default toolchains.
- Ensure the build configuration (`binding.gyp`) sets the required C++ standard flag. If missing, add it as part of the build work, not as a separate refactor.
- Node-API headers (`napi.h`) must compile cleanly with C++20. Avoid constructs that conflict with N-API internals; prefer modern alternatives only where safe.

---

## Important files

- `README.md` — usage, examples, and high-level behavior.
- `package.json` — scripts, dependencies, test commands.
- `binding.gyp` — native build configuration.
- `lib/index.ts` — exported JS API.
- `src/addon.cpp` — Node/N-API binding.
- `src/serialize.cpp` / `.hpp` — binary frame layout and serialization.
- `src/platform_capture.hpp` — capture backend abstraction.
- `src/linux/` — Linux capture implementation.
- `src/win/` — Windows capture implementation.
- `prebuilds/` — published prebuilt binaries.

---

## Build and verification

Preferred commands:
- `bun install` — install dependencies.
- `bun run build` — compile TypeScript.
- `bun x node-gyp rebuild` — build native addon.
- `bun run test:load` — verify runtime loading of native addon.
- `bun run prebuildify` — create prebuilt binaries for current platform.

Use `bun x node-gyp rebuild` when changing native code.

---

## Debugging policy

When troubleshooting:
- Do NOT refactor unrelated code.
- Do NOT assume root cause without evidence.
- Start from the screen capture pipeline.
- If unclear, collect logs, stack traces, and reproduction steps.
- Avoid broad architectural changes unless proven necessary.
- Prefer minimal, surgical patches.
- Fix correctness first, then clean up style.

---

## Debugging checklist

Before proposing a fix, answer:
- Which pipeline stage is failing?
- What is the minimal failing condition?
- Can the failure be reproduced consistently?
- Is it platform-specific (Linux/Windows) or shared?

If any answer is unknown, ask for more data.

---

## Conventions

- Keep a single JS-facing API: `ScreenCapture`.
- Do NOT break public API without explicit request.
- Do NOT ship public Node/package API changes without adding TSDoc / JSDoc comments.
- Keep platform-specific behavior inside the backend layer.
- Add OS support by extending the backend interface, not rewriting core logic.
- Prefer `README.md` over duplicating documentation.
- Write code comments only in English.
- **All new native code must comply with C++20.** Legacy C-style code may be gradually modernized only with explicit approval.

---

## Forbidden actions

- Do NOT change public JS API without permission.
- Do NOT replace the build system (`node-gyp` / `prebuildify`).
- Do NOT introduce new tooling unless requested.
- Do NOT refactor native core only for style.
- Do NOT guess OS-specific behavior without verifying the backend.
- Do NOT downgrade the C++ standard or introduce code that requires C++17 (or earlier) unless you can prove that C++20 is insufficient for the specific low-level system API. In that case, request explicit permission.

---

## Notes for agents

- Keep responses minimal, accurate, and code-focused.
- Base conclusions only on code, logs, or reproducible behavior.
- If unsure, ask for reproduction steps or runtime output.
- Treat this repo as performance-critical native runtime code.
- When proposing code changes, always ensure they are valid C++20 and match the toolchain requirements (MSVC 2022, GCC 12+, Clang 16+).

---

## Golden rule

Never assume runtime behavior.
Only reason from:
- actual code
- logs or stack traces
- reproduction steps
- the defined pipeline architecture
