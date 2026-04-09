import re
import sys

def main():
    with open('src/linux/platform_capture_linux.cpp', 'r') as f:
        content = f.read()

    # 1. Rename LinuxPlatformCapture to WaylandPlatformCapture everywhere inside the file
    content = content.replace("class LinuxPlatformCapture", "class WaylandPlatformCapture")
    content = content.replace("LinuxPlatformCapture::", "WaylandPlatformCapture::")
    content = content.replace("~LinuxPlatformCapture", "~WaylandPlatformCapture")
    content = content.replace("LinuxPlatformCapture()", "WaylandPlatformCapture()")

    # 2. Extract BaseLinuxPlatformCapture
    base_class = """class BaseLinuxPlatformCapture : public IPlatformCapture {
protected:
    mutable std::shared_mutex m_stateMutex;
    std::thread m_worker;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};
    mutable std::atomic<bool> m_frameConsumed{false};
    std::mutex m_captureMutex;
    std::condition_variable m_captureCv;

    std::optional<SharedHandleInfo> m_sharedHandle;
    UniqueFd m_sharedFd;

public:
    virtual ~BaseLinuxPlatformCapture() = default;

    std::optional<SharedHandleInfo> GetSharedHandle() const override {
        std::unique_lock<std::shared_mutex> lock(m_stateMutex);
        if (!m_sharedHandle.has_value() || m_frameConsumed || !m_sharedFd || *m_sharedFd < 0) {
            return std::nullopt;
        }

        int duplicatedFd = dup(*m_sharedFd);
        if (duplicatedFd < 0) {
            return std::nullopt;
        }

        SharedHandleInfo info = *m_sharedHandle;
        info.handle = static_cast<uint64_t>(duplicatedFd);
        m_frameConsumed = true;
        return info;
    }
};

class WaylandPlatformCapture final : public BaseLinuxPlatformCapture {
"""
    content = content.replace("class WaylandPlatformCapture final : public IPlatformCapture {\n    public:", base_class + "    public:")

    # 3. Remove GetSharedHandle from WaylandPlatformCapture
    get_shared_handle_pattern = r"    std::optional<SharedHandleInfo> GetSharedHandle\(\) const override \{.*?    \}\n"
    content = re.sub(get_shared_handle_pattern, "", content, count=1, flags=re.DOTALL)
    
    # Remove duplicates from protected/private:
    content = block_replace_once(content, "mutable std::shared_mutex m_stateMutex;\n")
    content = block_replace_once(content, "std::thread m_worker;\n")
    content = block_replace_once(content, "std::atomic<bool> m_running{false};\n")
    content = block_replace_once(content, "std::atomic<bool> m_stopRequested{false};\n")
    content = block_replace_once(content, "mutable std::atomic<bool> m_frameConsumed{false};\n")
    content = block_replace_once(content, "std::optional<SharedHandleInfo> m_sharedHandle;\n")
    content = block_replace_once(content, "UniqueFd m_sharedFd;\n")
    content = block_replace_once(content, "std::mutex m_captureMutex;\n")
    content = block_replace_once(content, "std::condition_variable m_captureCv;\n")

    # Add X11PlatformCapture and Detect logic replacing the last CreatePlatformCapture
    x11_logic = """

// ==========================================
// Implementacja dla środowiska X11
// Wymagane zależności: libX11, libXext, libXfixes
// W binding.gyp należy dodać `x11` i `xext` w sekcji cflags_cc oraz libraries
// ==========================================
class X11PlatformCapture final : public BaseLinuxPlatformCapture {
    public:
    X11PlatformCapture() = default;

    ~X11PlatformCapture() override {
        Stop();
    }

    void Start(Napi::Env) override {
        bool expected = false;
        if (!m_running.compare_exchange_strong(expected, true)) {
            return;
        }
        m_worker = std::thread(&X11PlatformCapture::CaptureLoop, this);
    }

    void Stop() override {
        m_stopRequested.store(true);
        m_captureCv.notify_all();

        if (m_worker.joinable() && std::this_thread::get_id() != m_worker.get_id()) {
            m_worker.join();
        }
        m_running.store(false);
    }

    private:
    void CaptureLoop() {
        // Główna pętla przechwytywania (60 FPS z fallbackiem)
        while (!m_stopRequested.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            
            // TODO dla X11: 
            // 1. Otwarcie XOpenDisplay(NULL) jeśli jeszcze nie ma
            // 2. Utworzenie obrazu z użyciem XShmGetImage (wydajniejsze) lub XGetImage
            // 3. Zapisanie zawartości pikseli do tymczasowego memfd (memfd_create)
            // 4. Przypisanie fd do m_sharedFd
            // 5. Ustawienie m_sharedHandle podając wymiary, stride i deskryptor
        }
    }
};

bool IsWayland() {
    const char* waylandDisplay = std::getenv("WAYLAND_DISPLAY");
    if (waylandDisplay && waylandDisplay[0] != '\\0') return true;
    
    const char* sessionType = std::getenv("XDG_SESSION_TYPE");
    if (sessionType && std::string(sessionType) == "wayland") return true;
    
    return false;
}

std::unique_ptr<IPlatformCapture> CreatePlatformCapture() {
    if (IsWayland()) {
        std::cout << "[Capture] Wykryto środowisko Wayland." << std::endl;
        return std::make_unique<WaylandPlatformCapture>();
    } else {
        std::cout << "[Capture] Wykryto środowisko X11." << std::endl;
        return std::make_unique<X11PlatformCapture>();
    }
}
"""
    
    # Find CreatePlatformCapture and replace it (and everything after it, until #endif)
    create_idx = content.rfind("std::unique_ptr<IPlatformCapture> CreatePlatformCapture()")
    if create_idx != -1:
        # Keep everything before create_idx, replace after with x11_logic + "\n#endif\n"
        content = content[:create_idx] + x11_logic + "\n#endif\n"

    with open('src/linux/platform_capture_linux.cpp', 'w') as f:
        f.write(content)

def block_replace_once(content, string):
    parts = content.split(string, 1)
    if len(parts) > 1:
         return parts[0] + parts[1]
    return content

if __name__ == '__main__':
    main()
