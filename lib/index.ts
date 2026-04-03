import path from 'node:path';
import { fileURLToPath } from 'node:url';
import nodeGypBuild from 'node-gyp-build';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

export interface SharedHandleInfo {
    handle: bigint;
    width: number;
    height: number;
}

export interface IScreenCapture {
    start(): void;
    stop(): void;
    getSharedHandle(): SharedHandleInfo | null;
}

export interface INativeAddon {
    ScreenCapture: new () => IScreenCapture;
}

const rootDir = path.resolve(__dirname, '..');
const native = nodeGypBuild(rootDir) as INativeAddon;

export const ScreenCapture = native.ScreenCapture;
export default native;
