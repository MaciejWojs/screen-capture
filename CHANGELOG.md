## 0.4.0 (2026-04-20)

* docs: update README.md to clarify getPixelData usage on Wayland with NVIDIA ([321ec09](https://github.com/MaciejWojs/screen-capture/commit/321ec09))
* refactor: replace unique_ptr with shared_ptr for memory management and simplify atomic operations in ([0ec0b0e](https://github.com/MaciejWojs/screen-capture/commit/0ec0b0e))
* fix: add alpha mask handling for pixel format detection in X11PlatformCapture ([9d42d59](https://github.com/MaciejWojs/screen-capture/commit/9d42d59))
* fix: add AVX2 target attribute to BroadcastShuffleMask and declare convertRow_avx2 function ([ad5362d](https://github.com/MaciejWojs/screen-capture/commit/ad5362d))
* fix: add fallback implementation for convertRow_avx2 when AVX2 is not defined ([5894293](https://github.com/MaciejWojs/screen-capture/commit/5894293))
* fix: add pixel format detection for X11 in X11PlatformCapture ([d7c918f](https://github.com/MaciejWojs/screen-capture/commit/d7c918f))
* fix: correct artifact name to use matrix.os in prebuilds upload step ([0c54a06](https://github.com/MaciejWojs/screen-capture/commit/0c54a06))
* fix: declaration duplication error convertRow_avx2 ([3e43335](https://github.com/MaciejWojs/screen-capture/commit/3e43335))
* fix: enhance build workflow by adding Node.js setup and caching for build output ([d9ce262](https://github.com/MaciejWojs/screen-capture/commit/d9ce262))
* fix: enhance pixel data retrieval by validating available bytes and adjusting stride handling ([ede8dfb](https://github.com/MaciejWojs/screen-capture/commit/ede8dfb))
* fix: enhance resource management with smart pointers in X11PlatformCapture ([e9e9b59](https://github.com/MaciejWojs/screen-capture/commit/e9e9b59))
* fix: implement GetPixelData method for X11PlatformCapture ([1420a79](https://github.com/MaciejWojs/screen-capture/commit/1420a79))
* fix: improve Linux texture serialization with buffer type validation and correct modifier handling ([bfdbe7b](https://github.com/MaciejWojs/screen-capture/commit/bfdbe7b))
* fix: refactor AVX2 support checks and implementation in pixel conversion ([ab4b3cf](https://github.com/MaciejWojs/screen-capture/commit/ab4b3cf))
* fix: refactor frame buffer management to use shared pointers for file descriptors ([bfa68e1](https://github.com/MaciejWojs/screen-capture/commit/bfa68e1))
* fix: refactor pixel data retrieval into ReadPixelDataFromSharedFd for X11 and Wayland ([aae0ea2](https://github.com/MaciejWojs/screen-capture/commit/aae0ea2))
* fix: refine CPU feature checks for SSSE3 and AVX2 support on x86 architectures ([ab537c6](https://github.com/MaciejWojs/screen-capture/commit/ab537c6))
* fix: remove duplicate entries and outdated references in CHANGELOG.md ([70dadb3](https://github.com/MaciejWojs/screen-capture/commit/70dadb3))
* fix: streamline AVX512 support checks and improve fallback handling in pixel conversion ([4c1f509](https://github.com/MaciejWojs/screen-capture/commit/4c1f509))
* fix: update architecture checks and add TARGET_ATTR for SIMD functions in pixel conversion ([32e76cd](https://github.com/MaciejWojs/screen-capture/commit/32e76cd))
* fix: update cache key to use matrix.os for build artifacts ([f599282](https://github.com/MaciejWojs/screen-capture/commit/f599282))
* fix: update upload-artifact action to version 7.0.1 in build workflow ([bff7065](https://github.com/MaciejWojs/screen-capture/commit/bff7065))
* feat: implement frame buffer pool and enhance pixel data retrieval for X11 and Wayland ([b78a7c4](https://github.com/MaciejWojs/screen-capture/commit/b78a7c4))
* v0.3.2 ([9f12946](https://github.com/MaciejWojs/screen-capture/commit/9f12946))
* v0.3.3 ([ea85f85](https://github.com/MaciejWojs/screen-capture/commit/ea85f85))
* v0.3.4 ([4ad9d95](https://github.com/MaciejWojs/screen-capture/commit/4ad9d95))

## <small>0.3.4 (2026-04-17)</small>

* fix: add AVX2 target attribute to BroadcastShuffleMask and declare convertRow_avx2 function ([ad5362d](https://github.com/MaciejWojs/screen-capture/commit/ad5362d))
* fix: declaration duplication error convertRow_avx2 ([3e43335](https://github.com/MaciejWojs/screen-capture/commit/3e43335))
* fix: refactor AVX2 support checks and implementation in pixel conversion ([ab4b3cf](https://github.com/MaciejWojs/screen-capture/commit/ab4b3cf))
* fix: refine CPU feature checks for SSSE3 and AVX2 support on x86 architectures ([ab537c6](https://github.com/MaciejWojs/screen-capture/commit/ab537c6))
* fix: streamline AVX512 support checks and improve fallback handling in pixel conversion ([4c1f509](https://github.com/MaciejWojs/screen-capture/commit/4c1f509))
* fix: update cache key to use matrix.os for build artifacts ([f599282](https://github.com/MaciejWojs/screen-capture/commit/f599282))
* fix: update upload-artifact action to version 7.0.1 in build workflow ([bff7065](https://github.com/MaciejWojs/screen-capture/commit/bff7065))

## <small>0.3.3 (2026-04-17)</small>

* fix: add fallback implementation for convertRow_avx2 when AVX2 is not defined ([5894293](https://github.com/MaciejWojs/screen-capture/commit/5894293))
* fix: correct artifact name to use matrix.os in prebuilds upload step ([0c54a06](https://github.com/MaciejWojs/screen-capture/commit/0c54a06))
* fix: declaration duplication error convertRow_avx2 ([3e43335](https://github.com/MaciejWojs/screen-capture/commit/3e43335))
* fix: enhance build workflow by adding Node.js setup and caching for build output ([d9ce262](https://github.com/MaciejWojs/screen-capture/commit/d9ce262))
* fix: update architecture checks and add TARGET_ATTR for SIMD functions in pixel conversion ([32e76cd](https://github.com/MaciejWojs/screen-capture/commit/32e76cd))

## <small>0.3.2 (2026-04-17)</small>

* fix: correct artifact name to use matrix.os in prebuilds upload step ([0c54a06](https://github.com/MaciejWojs/screen-capture/commit/0c54a06))

## 0.3.0 (2026-04-17)
* fix: conditionally define PrefetchIfNeeded for non-x86 architectures ([0fdcd71](https://github.com/MaciejWojs/screen-capture/commit/0fdcd71))
* fix: correct syntax errors in pixel layout parsing and AVX support checks ([1d76bea](https://github.com/MaciejWojs/screen-capture/commit/1d76bea))
* docs: update README to include details on Wayland NVIDIA capture and pixel format conversion ([4b6dd9b](https://github.com/MaciejWojs/screen-capture/commit/4b6dd9b))
* feat: enhance pixel format conversion with architecture-specific optimizations and prefetching suppo ([09d1d04](https://github.com/MaciejWojs/screen-capture/commit/09d1d04))

## 0.3.0 (2026-04-17)

* feat: add CHANGELOG.md for version 0.2.5 with updates on X11 screen capture functionality ([4daa70c](https://github.com/MaciejWojs/screen-capture/commit/4daa70c))
* feat: add FPS counter and GetFps method to DXGIPlatformCapture class ([68e8e69](https://github.com/MaciejWojs/screen-capture/commit/68e8e69))
* feat: add FPS counter and GetFps method to WinPlatformCapture class ([9b29f10](https://github.com/MaciejWojs/screen-capture/commit/9b29f10))
* feat: add FPS tracking and RecordFrame method to BaseLinuxPlatformCapture class ([d3fd46c](https://github.com/MaciejWojs/screen-capture/commit/d3fd46c))
* feat: add getFps method to IScreenCapture interface and implement in ScreenCapture class ([ab6d6c5](https://github.com/MaciejWojs/screen-capture/commit/ab6d6c5))
* feat: add pixel data retrieval and backend information methods for platform capture ([2425b46](https://github.com/MaciejWojs/screen-capture/commit/2425b46))
* feat: add pixel format conversion and update GetPixelData method to support format specification ([0c61817](https://github.com/MaciejWojs/screen-capture/commit/0c61817))
* feat: add PixelFormatToString function for improved pixel format handling in Linux ([077f62b](https://github.com/MaciejWojs/screen-capture/commit/077f62b))
* feat: add workspace instructions for native addon build and debugging ([c16a9c9](https://github.com/MaciejWojs/screen-capture/commit/c16a9c9))
* feat: enhance pixel format conversion with architecture-specific optimizations and prefetching suppo ([09d1d04](https://github.com/MaciejWojs/screen-capture/commit/09d1d04))
* feat: enhance task details and commands in tasks.json for improved clarity and functionality ([d3fd87e](https://github.com/MaciejWojs/screen-capture/commit/d3fd87e))
* feat: implement FPS counter and GetFps method in LegacyWinPlatformCapture class ([0c227bd](https://github.com/MaciejWojs/screen-capture/commit/0c227bd))
* feat: optimize pixel format conversion with AVX2 and SSSE3 support ([39f9e2e](https://github.com/MaciejWojs/screen-capture/commit/39f9e2e))
* feat: update build strategy to include architecture-specific optimizations ([646a42c](https://github.com/MaciejWojs/screen-capture/commit/646a42c))
* feat: update package.json to include README.md and CHANGELOG.md in files and scripts ([b5c7d66](https://github.com/MaciejWojs/screen-capture/commit/b5c7d66))
* feat: WIP enhance WaylandPlatformCapture to support Nvidia GPU detection and buffer type logging ([eb8c1b4](https://github.com/MaciejWojs/screen-capture/commit/eb8c1b4))
* fix: improve cleanup command in tasks.json for better artifact removal ([fb627fb](https://github.com/MaciejWojs/screen-capture/commit/fb627fb))
* fix: reduce logging for buffer types in WaylandPlatformCapture class ([b0ff2f1](https://github.com/MaciejWojs/screen-capture/commit/b0ff2f1))
* fix: update cleanup commands in tasks.json for consistency ([b51852e](https://github.com/MaciejWojs/screen-capture/commit/b51852e))
* fix: update devDependencies and peerDependencies for compatibility ([acd0335](https://github.com/MaciejWojs/screen-capture/commit/acd0335))
* fix: update prebuildify scripts for improved compatibility and functionality ([c84c122](https://github.com/MaciejWojs/screen-capture/commit/c84c122))
* docs: update README.md with detailed architecture and backend implementation for screen capture ([8baa356](https://github.com/MaciejWojs/screen-capture/commit/8baa356))

## <small>0.2.5 (2026-04-11)</small>

*  implement X11 screen capture functionality with shared memory support ([8aa6b7e](https://github.com/MaciejWojs/screen-capture/commit/8aa6b7e))
* v0.2.5 ([6111363](https://github.com/MaciejWojs/screen-capture/commit/6111363))
* fix: update Linux dependencies in build workflow ([51f2f5a](https://github.com/MaciejWojs/screen-capture/commit/51f2f5a))
* feat: add debug logging to X11 capture loop for better traceability ([f4476b5](https://github.com/MaciejWojs/screen-capture/commit/f4476b5))
* feat: add tasks configuration for build process in tasks.json ([13c7c05](https://github.com/MaciejWojs/screen-capture/commit/13c7c05))
* feat: add X11 support to build configuration in binding.gyp ([05a282f](https://github.com/MaciejWojs/screen-capture/commit/05a282f))
* feat: merge X11 and Wayland functionality ([bd37e8c](https://github.com/MaciejWojs/screen-capture/commit/bd37e8c))
* feat: refactor LinuxPlatformCapture to support X11 environments ([f3722de](https://github.com/MaciejWojs/screen-capture/commit/f3722de))

## <small>0.2.4 (2026-04-10)</small>

* chore: bump version to 0.2.4 in package.json ([774aaff](https://github.com/MaciejWojs/screen-capture/commit/774aaff))
* fix: enhance WinRT capture availability check with error handling and logging ([79aa422](https://github.com/MaciejWojs/screen-capture/commit/79aa422))

## <small>0.2.1 (2026-04-10)</small>

* v0.2.1 ([4e99850](https://github.com/MaciejWojs/screen-capture/commit/4e99850))
* fix: correct branch specification in workflow trigger ([6234f5c](https://github.com/MaciejWojs/screen-capture/commit/6234f5c))
* feat: implement support for multiple screen capture APIs (GDI, DXGI, WinRT) ([139ba97](https://github.com/MaciejWojs/screen-capture/commit/139ba97))

## 0.2.0 (2026-04-09)

* v0.2.0 ([8985a99](https://github.com/MaciejWojs/screen-capture/commit/8985a99))
* feat: add serialization for shared texture info and legacy shared handle ([af81925](https://github.com/MaciejWojs/screen-capture/commit/af81925))
* feat: enhance SharedHandleInfo and IScreenCapture interfaces with additional documentation ([eb126f0](https://github.com/MaciejWojs/screen-capture/commit/eb126f0))
* feat: update README with usage examples and serialization logic details ([53d86e1](https://github.com/MaciejWojs/screen-capture/commit/53d86e1))

## <small>0.1.7 (2026-04-08)</small>

* v0.1.7 ([322f84c](https://github.com/MaciejWojs/screen-capture/commit/322f84c))
* feat: enhance buffer parameter options for Linux platform capture ([fe0f7a2](https://github.com/MaciejWojs/screen-capture/commit/fe0f7a2))

## <small>0.1.6 (2026-04-08)</small>

* v0.1.6 ([7ec288c](https://github.com/MaciejWojs/screen-capture/commit/7ec288c))
* feat: enhance Linux platform capture with improved error handling and resource management ([2e08019](https://github.com/MaciejWojs/screen-capture/commit/2e08019))
* feat: enhance Linux platform capture with improved handling of GLib main loop and error states ([b529e74](https://github.com/MaciejWojs/screen-capture/commit/b529e74))
* feat: enhance Linux platform capture with improved resource management and logging ([837d429](https://github.com/MaciejWojs/screen-capture/commit/837d429))
* feat: enhance Linux platform capture with improved thread safety and resource management ([7d0300d](https://github.com/MaciejWojs/screen-capture/commit/7d0300d))
* feat: enhance Linux platform capture with improved thread safety and resource management ([bf3ab46](https://github.com/MaciejWojs/screen-capture/commit/bf3ab46))
* feat: improve Linux platform capture with enhanced token generation and request handling ([1f6e00d](https://github.com/MaciejWojs/screen-capture/commit/1f6e00d))
* feat: update prebuildify script for electron compatibility ([7eeed4d](https://github.com/MaciejWojs/screen-capture/commit/7eeed4d))

## <small>0.1.5 (2026-04-07)</small>

* v0.1.5 ([c0360e9](https://github.com/MaciejWojs/screen-capture/commit/c0360e9))
* fix: update cursor_mode to hide mouse cursor ([83ab404](https://github.com/MaciejWojs/screen-capture/commit/83ab404))

## <small>0.1.4 (2026-04-07)</small>

* v0.1.4 ([9ab0a62](https://github.com/MaciejWojs/screen-capture/commit/9ab0a62))
* fix: add --ignore-scripts flag to Bun install commands in build workflow ([322bc0e](https://github.com/MaciejWojs/screen-capture/commit/322bc0e))

## <small>0.1.3 (2026-04-07)</small>

* v0.1.3 ([79e5a47](https://github.com/MaciejWojs/screen-capture/commit/79e5a47))
* feat: add Bun setup and dependency installation to build workflow; create .npmignore file ([bd69a78](https://github.com/MaciejWojs/screen-capture/commit/bd69a78))

## <small>0.1.2 (2026-04-07)</small>

* v0.1.2 ([7e5e10c](https://github.com/MaciejWojs/screen-capture/commit/7e5e10c))
* fix: correct repository URL in package.json ([b43504e](https://github.com/MaciejWojs/screen-capture/commit/b43504e))

## <small>0.1.1 (2026-04-07)</small>

* v0.1.1 ([e3f826e](https://github.com/MaciejWojs/screen-capture/commit/e3f826e))
* refactor: enhance WinPlatformCapture for improved thread management and cleanup ([a88da2e](https://github.com/MaciejWojs/screen-capture/commit/a88da2e))
* refactor: streamline frame pool and session creation in WinPlatformCapture ([a2471d0](https://github.com/MaciejWojs/screen-capture/commit/a2471d0))
* refactor: update prebuildify script for consistency and improve dependency installation ([6f2ba08](https://github.com/MaciejWojs/screen-capture/commit/6f2ba08))
* feat: add GitHub Actions workflow for build and publish process ([a3cdbe5](https://github.com/MaciejWojs/screen-capture/commit/a3cdbe5))
* feat: add README and update package.json for project metadata ([48959cd](https://github.com/MaciejWojs/screen-capture/commit/48959cd))
* feat: enhance Linux platform capture with additional shared handle properties ([6145c17](https://github.com/MaciejWojs/screen-capture/commit/6145c17))
* feat: Initial commit ([658ad06](https://github.com/MaciejWojs/screen-capture/commit/658ad06))
* fix: update binding.gyp to compile linux artifacts ([abef210](https://github.com/MaciejWojs/screen-capture/commit/abef210))
* chore: renormalize file endings to lf ([21366bd](https://github.com/MaciejWojs/screen-capture/commit/21366bd))
