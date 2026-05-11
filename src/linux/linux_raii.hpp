#pragma once

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <pipewire/pipewire.h>
#include <pipewire/thread-loop.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>

#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/shm.h>

#include <unistd.h>

#include <cstddef>
#include <memory>

struct MmapDeleter {
    size_t length = 0;
    void operator()(void* ptr) const {
        if (ptr && ptr != MAP_FAILED && length > 0) {
            munmap(ptr, length);
        }
    }
};

template <typename T, auto FreeFunc>
struct GenericDeleter {
    void operator()(T* ptr) const { if (ptr) FreeFunc(ptr); }
};

struct GObjectDeleter {
    void operator()(void* ptr) const { if (ptr) g_object_unref(ptr); }
};

struct FdDeleter {
    void operator()(int* fd) const { if (fd && *fd >= 0) { close(*fd); delete fd; } }
};

struct DisplayDeleter {
    void operator()(Display* display) const {
        if (display) {
            XCloseDisplay(display);
        }
    }
};

using DisplayPtr = std::unique_ptr<Display, DisplayDeleter>;

struct XImageDeleter {
    void operator()(XImage* image) const {
        if (image) {
            XDestroyImage(image);
        }
    }
};

using XImagePtr = std::unique_ptr<XImage, XImageDeleter>;

struct XShmSegmentInfoWrapper {
    XShmSegmentInfo info{};
    Display* display = nullptr;
    bool attached = false;

    bool Attach(Display* display_) {
        display = display_;
        attached = XShmAttach(display, &info);
        return attached;
    }

    ~XShmSegmentInfoWrapper() {
        if (attached && display) {
            XShmDetach(display, &info);
        }
        if (info.shmaddr) {
            shmdt(info.shmaddr);
        }
        if (info.shmid >= 0) {
            shmctl(info.shmid, IPC_RMID, 0);
        }
    }
};

using GMainLoopPtr = std::unique_ptr<GMainLoop, GenericDeleter<GMainLoop, g_main_loop_unref>>;
using GMainContextPtr = std::unique_ptr<GMainContext, GenericDeleter<GMainContext, g_main_context_unref>>;
using GDBusConnectionPtr = std::unique_ptr<GDBusConnection, GObjectDeleter>;
using PwThreadLoopPtr = std::unique_ptr<pw_thread_loop, GenericDeleter<pw_thread_loop, pw_thread_loop_destroy>>;
using PwContextPtr = std::unique_ptr<pw_context, GenericDeleter<pw_context, pw_context_destroy>>;
using PwCorePtr = std::unique_ptr<pw_core, GenericDeleter<pw_core, pw_core_disconnect>>;
using PwStreamPtr = std::unique_ptr<pw_stream, GenericDeleter<pw_stream, pw_stream_destroy>>;
using GVariantPtr = std::unique_ptr<GVariant, GenericDeleter<GVariant, g_variant_unref>>;
using GErrorPtr = std::unique_ptr<GError, GenericDeleter<GError, g_error_free>>;
using GUnixFDListPtr = std::unique_ptr<GUnixFDList, GObjectDeleter>;
using UniqueFd = std::unique_ptr<int, FdDeleter>;
using MmapPtr = std::shared_ptr<void>;
using SharedFd = std::shared_ptr<int>;
