import path from 'node:path';
import { fileURLToPath } from 'node:url';
import nodeGypBuild from 'node-gyp-build';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

/**
 * Information about the shared memory handle for a captured frame.
 */
export interface SharedHandleInfo {
    /** The shared memory handle (e.g., file descriptor on Linux or HANDLE on Windows). */
    handle: bigint;
    /** Width of the captured frame in pixels. */
    width: number;
    /** Height of the captured frame in pixels. */
    height: number;
    /** Number of bytes per row of pixels (pitch/stride). */
    stride: number;
    /** Byte offset to the start of the pixel data. */
    offset: number;
    /** Size of the image plane in bytes. */
    planeSize: bigint;
    /** The pixel format code (e.g., DRM format or DXGI format). */
    pixelFormat: number;
    /** Format modifier (e.g., describing tiling or compression). */
    modifier: bigint;
    /** The type of buffer used. */
    bufferType: number;
    /** The total size of the allocated memory chunk. */
    chunkSize: number;
}

/**
 * Windows-only capture backends:
 * - `winrt`: modern Graphics Capture API with best performance and support for protected content when available,
 * - `dxgi`: desktop duplication API good for most desktops but may fail with exclusive fullscreen or older drivers,
 * - `gdi`: legacy BitBlt fallback compatible with older Windows versions but slower and less efficient.
 */
export type WindowsBackend = "winrt" | "dxgi" | "gdi";

/**
 * Linux-only capture backends: `wayland` for Wayland compositors and `x11` for X11.
 */
export type LinuxBackend = "wayland" | "x11";

/**
 * All supported capture backends across platforms.
 */
export type Backend = WindowsBackend | LinuxBackend | "stub" | "unknown";


/** The format of the pixel data to retrieve. */
export type PixelDataFormat = "rgba" | "bgra" | "rgbx" | "bgrx" | "xrgb" | "xbgr";

/**
 * Configuration options for creating a `ScreenCapture` instance.
 */
export interface ScreenCaptureOptions {
    /** Set to `true` to disable all internal ScreenCapture logs. */
    disableLogging?: boolean;
    /** Set explicit log level. Defaults to `info`. */
    logLevel?: "none" | "error" | "warn" | "info" | "debug";
}

export interface FrameUpdate {
    /** Backend name, e.g. 'winrt', 'dxgi', 'gdi', 'wayland', 'x11'. */
    backend: Backend;
    /** Frame width in pixels. */
    width: number;
    /** Frame height in pixels. */
    height: number;
    /** Frame pitch/stride in bytes. */
    stride: number;
    /** Pixel format code for the captured frame. */
    pixelFormat: number;
    /** Shared texture information for Electron import, when available. */
    sharedTextureInfo: SharedTextureImportTextureInfo | null;
    /** Legacy shared handle information for the current frame. */
    sharedHandle: SharedHandleInfo | null;
    /** Raw pixel buffer for the current frame if shared texture export is unavailable. */
    pixelData: Buffer | null;
}

export interface IScreenCapture {
    /** Starts the screen capture process. Resolves when the capture backend has completed initialization and the shared handle is ready. */
    start(): Promise<void>;
    /** Stops the screen capture process. */
    stop(): void;
    /**
     * Registers a callback that receives frame data whenever a new frame is available.
     * This is the preferred push-based API for Node/Electron integration.
     * @param callback Function called with frame metadata and optional pixel data.
     */
    onFrame(callback: (frame: FrameUpdate) => void): void;
    /**
     * Unregisters the frame callback previously passed to `onFrame()`.
     */
    offFrame(): void;
    /**
     * Retrieves the legacy shared handle information for the latest captured frame.
     * @returns The shared handle info if available, otherwise null.
     */
    getSharedHandle(): SharedHandleInfo | null;
    /**
     * Retrieves raw pixel data for the latest captured frame.
     * The returned pixel buffer is normalized to RGBA by default.
     * @param format Optional target color layout, e.g. 'rgba' or 'bgra'.
     * @returns A Buffer containing pixel bytes, or null if unavailable.
     */
    getPixelData(format?: PixelDataFormat): Buffer | null;
    /**
     * Forces a Windows capture backend.
     * @throws When called on non-Windows systems or when the requested backend is unavailable.
     * @param backend The Windows-only backend to use: 'winrt', 'dxgi', or 'gdi'.
     */
    forceBackend(backend: WindowsBackend): void;
    /**
     * Returns the backend identifier used for the current capture implementation.
     */
    getBackend(): Backend;
    /**
     * Returns the width of the current frame, or 0 if unavailable.
     */
    getWidth(): number;
    /**
     * Returns the height of the current frame, or 0 if unavailable.
     */
    getHeight(): number;
    /**
     * Returns the stride/pitch of the current frame, or 0 if unavailable.
     */
    getStride(): number;
    /**
     * Returns the pixel format code of the current frame, or 0 if unavailable.
     */
    getPixelFormat(): number;
    /**
     * Retrieves the texture info formulated for Electron's shared-texture.
     * @returns The shared texture info if available, otherwise null.
     */
    getSharedTextureInfo(): SharedTextureImportTextureInfo | null;
    /**
     * Returns the current frames per second (FPS) or -1 if not implemented.
     */
    getFps(): number;
}

/**
 * Platform-specific handles for shared textures supported by Electron.
 */
export interface SharedTextureHandle {
    /** Windows - NT HANDLE that holds the shared texture. */
    ntHandle?: Buffer;
    /** Linux - Structure containing planes of the shared texture. */
    nativePixmap?: any;
    /** macOS - IOSurfaceRef that holds the shared texture. */
    ioSurface?: Buffer;
}

/**
 * Information required by Electron to import a shared texture.
 */
export interface SharedTextureImportTextureInfo {
    /** The pixel format of the texture (e.g., 'bgra', 'rgba', 'nv12'). */
    pixelFormat: string;
    /** The full dimensions of the shared texture. */
    codedSize: { width: number; height: number };
    /** The platform-specific shared texture handle. */
    handle: SharedTextureHandle;
}

/**
 * Interface representing the native addon exports.
 */
export interface INativeAddon {
    ScreenCapture: new (options?: ScreenCaptureOptions) => IScreenCapture;
}

const rootDir = path.resolve(__dirname, '..');
const native = nodeGypBuild(rootDir) as INativeAddon;

async function delay(ms: number): Promise<void> {
    return new Promise((resolve) => setTimeout(resolve, ms));
}

class ScreenCaptureWrapper implements IScreenCapture {
    private readonly inner: IScreenCapture;

    constructor(options?: ScreenCaptureOptions) {
        this.inner = new native.ScreenCapture(options);
    }

    async start(): Promise<void> {
        this.inner.start();

        const timeoutMs = 5000;
        const pollIntervalMs = 30;
        const deadline = Date.now() + timeoutMs;

        while (Date.now() < deadline) {
            // Sprawdzamy wymiary zamiast pobierać dane, aby nie "ukraść" klatki z onFrame
            if (this.inner.getWidth() > 0 && this.inner.getHeight() > 0) {
                return;
            }
            await delay(pollIntervalMs);
        }

        throw new Error('ScreenCapture.start() timed out waiting for capture readiness');
    }

    stop(): void {
        this.inner.stop();
    }

    onFrame(callback: (frame: FrameUpdate) => void): void {
        this.inner.onFrame(callback);
    }

    offFrame(): void {
        this.inner.offFrame();
    }

    getSharedHandle(): SharedHandleInfo | null {
        return this.inner.getSharedHandle();
    }

    getPixelData(format?: PixelDataFormat): Buffer | null {
        return this.inner.getPixelData(format);
    }

    forceBackend(backend: WindowsBackend): void {
        this.inner.forceBackend(backend);
    }

    getBackend(): Backend {
        return this.inner.getBackend();
    }

    getWidth(): number {
        return this.inner.getWidth();
    }

    getHeight(): number {
        return this.inner.getHeight();
    }

    getStride(): number {
        return this.inner.getStride();
    }

    getPixelFormat(): number {
        return this.inner.getPixelFormat();
    }

    getSharedTextureInfo(): SharedTextureImportTextureInfo | null {
        return this.inner.getSharedTextureInfo();
    }

    getFps(): number {
        return this.inner.getFps();
    }
}

export const ScreenCapture = ScreenCaptureWrapper;
export const NativeScreenCapture = native.ScreenCapture;
export default ScreenCaptureWrapper;
