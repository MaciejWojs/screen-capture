
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
