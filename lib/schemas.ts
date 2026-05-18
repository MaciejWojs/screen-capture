import { z } from 'zod';

export const PixelDataFormatSchema = z.enum(['rgba', 'bgra', 'rgbx', 'bgrx', 'xrgb', 'xbgr']);

export const WindowsBackendSchema = z.enum(['winrt', 'dxgi', 'gdi']);
export const LinuxBackendSchema = z.enum(['wayland', 'x11']);
export const BackendSchema = z.union([WindowsBackendSchema, LinuxBackendSchema, z.literal('stub'), z.literal('unknown')]);

export const MonitorMetadataSchema = z.object({
    id: z.string(),
    name: z.string(),
    index: z.number().int().nonnegative(),
    x: z.number().int(),
    y: z.number().int(),
    width: z.number().int().nonnegative(),
    height: z.number().int().nonnegative(),
    pipewireStream: z.number().int().optional(),
});

export const SharedHandleInfoSchema = z.object({
    handle: z.bigint(),
    width: z.number().int().nonnegative(),
    height: z.number().int().nonnegative(),
    stride: z.number().int().nonnegative(),
    offset: z.number().int().nonnegative(),
    planeSize: z.bigint(),
    pixelFormat: z.number().int().nonnegative(),
    modifier: z.bigint(),
    bufferType: z.number().int().nonnegative(),
    chunkSize: z.number().int().nonnegative(),
});

export const SharedTextureHandleSchema = z.object({
    ntHandle: z.instanceof(Buffer).optional(),
    nativePixmap: z.any().optional(),
    ioSurface: z.instanceof(Buffer).optional(),
}).passthrough();

export const SharedTextureImportTextureInfoSchema = z.object({
    pixelFormat: z.string(),
    codedSize: z.object({ width: z.number().int().nonnegative(), height: z.number().int().nonnegative() }),
    handle: SharedTextureHandleSchema,
}).strict();

export const FrameUpdateSchema = z.object({
    backend: BackendSchema,
    width: z.number().int().nonnegative(),
    height: z.number().int().nonnegative(),
    stride: z.number().int().nonnegative(),
    pixelFormat: z.number().int().nonnegative(),
    timestamp: z.number().int().nonnegative(),
    sharedTextureInfo: SharedTextureImportTextureInfoSchema.nullable(),
    sharedHandle: SharedHandleInfoSchema.nullable(),
    pixelData: z.instanceof(Buffer).nullable(),
}).strict();

export const MonitorUpdateSchema = z.object({
    backend: BackendSchema,
    id: z.string(),
    name: z.string(),
    index: z.number().int().nonnegative(),
    x: z.number().int(),
    y: z.number().int(),
    width: z.number().int().nonnegative(),
    height: z.number().int().nonnegative(),
    pipewireStream: z.number().int().optional(),
}).strict();

export const ScreenCaptureOptionsSchema = z.object({
    disableLogging: z.boolean().optional(),
    logLevel: z.union([z.literal('none'), z.literal('error'), z.literal('warn'), z.literal('info'), z.literal('debug')]).optional(),
    portalSessionHandle: z.string().optional(),
    pipewireRemoteFd: z.number().int().optional(),
    portalMonitors: z.array(MonitorMetadataSchema).optional(),
}).strict();

export const MonitorIndexSchema = z.number().int().nonnegative();

export function parseOptions(opt: unknown) {
    return ScreenCaptureOptionsSchema.safeParse(opt);
}

export function parsePixelFormat(v: unknown) {
    return PixelDataFormatSchema.safeParse(v);
}

export function parseWindowsBackend(v: unknown) {
    return WindowsBackendSchema.safeParse(v);
}

export function parseMonitorIndex(v: unknown) {
    return MonitorIndexSchema.safeParse(v);
}

export function parseFrameUpdate(v: unknown) {
    return FrameUpdateSchema.safeParse(v);
}

export function parseMonitorUpdate(v: unknown) {
    return MonitorUpdateSchema.safeParse(v);
}

export function parseSharedHandle(v: unknown) {
    return SharedHandleInfoSchema.safeParse(v);
}

export function parseSharedTextureInfo(v: unknown) {
    return SharedTextureImportTextureInfoSchema.safeParse(v);
}

export type PixelDataFormat = z.infer<typeof PixelDataFormatSchema>;
export type WindowsBackend = z.infer<typeof WindowsBackendSchema>;
export type LinuxBackend = z.infer<typeof LinuxBackendSchema>;
export type Backend = z.infer<typeof BackendSchema>;
export type MonitorMetadata = z.infer<typeof MonitorMetadataSchema>;
export type SharedHandleInfo = z.infer<typeof SharedHandleInfoSchema>;
export type SharedTextureImportTextureInfo = z.infer<typeof SharedTextureImportTextureInfoSchema>;
export type FrameUpdate = z.infer<typeof FrameUpdateSchema>;
export type MonitorUpdate = z.infer<typeof MonitorUpdateSchema>;
export type ScreenCaptureOptions = z.infer<typeof ScreenCaptureOptionsSchema>;

export default {
    PixelDataFormatSchema,
    WindowsBackendSchema,
    LinuxBackendSchema,
    BackendSchema,
    MonitorMetadataSchema,
    SharedHandleInfoSchema,
    SharedTextureHandleSchema,
    SharedTextureImportTextureInfoSchema,
    FrameUpdateSchema,
    MonitorUpdateSchema,
    ScreenCaptureOptionsSchema,
    MonitorIndexSchema,
    parseOptions,
    parsePixelFormat,
    parseWindowsBackend,
    parseMonitorIndex,
    parseFrameUpdate,
    parseMonitorUpdate,
    parseSharedHandle,
    parseSharedTextureInfo,
};
