#pragma once

#include "linux_raii.hpp"

#include <array>
#include <mutex>
#include <optional>
#include <vector>
#include <cstdint>

struct IntRect {
    uint32_t x, y, w, h;
};

struct FrameBufferSlot {
    SharedFd fd;
    MmapPtr mapping;
    std::optional<SharedHandleInfo> handle;
    std::vector<IntRect> damage;
    bool ready = false;
    bool fullUpdate = true;
};

class FrameBufferPool {
    public:
    void Reset() {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& slot : m_slots) {
            slot.fd.reset();
            slot.mapping.reset();
            slot.handle.reset();
            slot.damage.clear();
            slot.ready = false;
            slot.fullUpdate = true;
        }
        m_writeIndex = 0;
        m_latestIndex = -1;
    }

    void PushFrame(SharedFd fd, std::optional<SharedHandleInfo> handle, std::vector<IntRect> damage, bool fullUpdate, MmapPtr mapping = nullptr) {
        std::lock_guard<std::mutex> lock(m_mutex);
        size_t writeIdx = m_writeIndex;
        FrameBufferSlot& slot = m_slots[writeIdx];
        slot.fd = std::move(fd);
        slot.mapping = std::move(mapping);
        slot.handle = std::move(handle);
        slot.damage = std::move(damage);
        slot.ready = true;
        slot.fullUpdate = fullUpdate;
        m_latestIndex = writeIdx;
        m_writeIndex = (writeIdx + 1) % m_slots.size();
    }

    std::optional<FrameBufferSlot> AcquireReadFrame() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_latestIndex == -1 || !m_slots[m_latestIndex].ready) {
            return std::nullopt;
        }

        // Always take the newest frame (LIFO / Zero Latency)
        FrameBufferSlot result = m_slots[m_latestIndex];

        // Mark all frames as consumed - we don't want to read stale data
        for (auto& slot : m_slots) {
            slot.ready = false;
        }
        m_latestIndex = -1;
        return result;
    }

    private:
    std::array<FrameBufferSlot, 3> m_slots;
    size_t m_writeIndex = 0;
    int m_latestIndex = -1;
    std::mutex m_mutex;
};
