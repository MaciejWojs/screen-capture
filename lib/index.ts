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

export interface IScreenCapture {
    /** Starts the screen capture process. */
    start(): void;
    /** Stops the screen capture process. */
    stop(): void;
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
    ScreenCapture: new () => IScreenCapture;
}

const rootDir = path.resolve(__dirname, '..');
const native = nodeGypBuild(rootDir) as INativeAddon;

export const ScreenCapture = native.ScreenCapture;
export default native;
