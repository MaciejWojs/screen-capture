import { z } from 'zod';

export const PixelDataFormatSchema = z.enum(['rgba', 'bgra', 'rgbx', 'bgrx', 'xrgb', 'xbgr']);

export const WindowsBackendSchema = z.enum(['winrt', 'dxgi', 'gdi']);
export const LinuxBackendSchema = z.enum(['wayland', 'x11']);
export const BackendSchema = z.union([WindowsBackendSchema, LinuxBackendSchema, z.literal('stub'), z.literal('unknown')]);

export const MonitorMetadataSchema = z.object({
    id: z.string(),
    name: z.string(),
    index: z.number().int().min(0),
    x: z.number().int(),
    y: z.number().int(),
    width: z.number().int().min(0),
    height: z.number().int().min(0),
    pipewireStream: z.number().int().optional(),
});

export const SharedHandleInfoSchema = z.object({
    handle: z.bigint(),
    width: z.number().int().min(0),
    height: z.number().int().min(0),
    stride: z.number().int().min(0),
    offset: z.number().int().min(0),
    planeSize: z.bigint(),
    pixelFormat: z.number().int().min(0),
    modifier: z.bigint(),
    bufferType: z.number().int().min(0),
    chunkSize: z.number().int().min(0),
});

export const SharedTextureHandleSchema = z.object({
    ntHandle: z.instanceof(Buffer).optional(),
    nativePixmap: z.any().optional(),
    ioSurface: z.instanceof(Buffer).optional(),
}).loose();

export type SharedTextureHandle = z.infer<typeof SharedTextureHandleSchema>;

export const SharedTextureImportTextureInfoSchema = z.object({
    pixelFormat: z.string(),
    codedSize: z.object({ width: z.number().int().min(0), height: z.number().int().min(0) }),
    handle: SharedTextureHandleSchema,
}).strict();

export const FrameUpdateSchema = z.object({
    backend: BackendSchema,
    width: z.number().int().min(0),
    height: z.number().int().min(0),
    stride: z.number().int().min(0),
    pixelFormat: z.number().int().min(0),
    timestamp: z.number().int().min(0),
    sharedTextureInfo: SharedTextureImportTextureInfoSchema.nullable(),
    sharedHandle: SharedHandleInfoSchema.nullable(),
    pixelData: z.instanceof(Buffer).nullable(),
}).strict();

export const MonitorUpdateSchema = z.object({
    backend: BackendSchema,
    id: z.string(),
    name: z.string(),
    index: z.number().int().min(0),
    x: z.number().int(),
    y: z.number().int(),
    width: z.number().int().min(0),
    height: z.number().int().min(0),
    pipewireStream: z.number().int().optional(),
}).strict();

export const ScreenCaptureOptionsSchema = z.object({
    disableLogging: z.boolean().optional(),
    logLevel: z.union([z.literal('none'), z.literal('error'), z.literal('warn'), z.literal('info'), z.literal('debug')]).optional(),
    portalSessionHandle: z.string().optional(),
    pipewireRemoteFd: z.number().int().optional(),
    portalMonitors: z.array(MonitorMetadataSchema).optional(),
}).strict();

export const MonitorIndexSchema = z.number().int().min(0);

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
