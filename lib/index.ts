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
 * Interface for controlling the screen capture process and accessing captured frames.
 */
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
     * Retrieves the texture info formulated for Electron's shared-texture.
     * @returns The shared texture info if available, otherwise null.
     */
    getSharedTextureInfo(): SharedTextureImportTextureInfo | null;
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
