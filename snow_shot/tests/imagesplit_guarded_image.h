// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QImage>
#include <QColorSpace>

#include <cstring>
#include <stdexcept>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace image_split_test {

// Keep the last source scanline immediately before an inaccessible page. Unlike ordinary heap
// storage, this deterministically exposes even a one-byte read past the image's declared storage.
class GuardedImage final {
  public:
    explicit GuardedImage(const QImage& source) {
        if (source.isNull())
            throw std::runtime_error("guarded image requires a nonempty source");
#ifdef Q_OS_WIN
        SYSTEM_INFO information;
        GetSystemInfo(&information);
        const size_t page = information.dwPageSize;
#else
        const long pageSize = sysconf(_SC_PAGESIZE);
        if (pageSize <= 0)
            throw std::runtime_error("read guard page size");
        const size_t page = static_cast<size_t>(pageSize);
#endif
        const size_t bytes = static_cast<size_t>(source.sizeInBytes());
        const size_t accessible = (bytes + page - 1) / page * page;
        m_allocationBytes = accessible + page;
#ifdef Q_OS_WIN
        m_allocation = static_cast<uchar*>(
            VirtualAlloc(nullptr, m_allocationBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        DWORD previous = 0;
        const bool protectedPage = m_allocation && VirtualProtect(m_allocation + accessible, page,
                                                                  PAGE_NOACCESS, &previous);
#else
        void* allocation =
            mmap(nullptr, m_allocationBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        m_allocation = allocation == MAP_FAILED ? nullptr : static_cast<uchar*>(allocation);
        const bool protectedPage =
            m_allocation && mprotect(m_allocation + accessible, page, PROT_NONE) == 0;
#endif
        if (!protectedPage) {
            release();
            throw std::runtime_error("allocate protected image boundary");
        }
        uchar* pixels = m_allocation + accessible - bytes;
        std::memcpy(pixels, source.constBits(), bytes);
        m_image = QImage(static_cast<const uchar*>(pixels), source.width(), source.height(),
                         source.bytesPerLine(), source.format());
        m_image.setColorTable(source.colorTable());
        m_image.setColorSpace(source.colorSpace());
        m_image.setDevicePixelRatio(source.devicePixelRatio());
        if (m_image.constBits() != pixels) {
            release();
            throw std::runtime_error("guarded image must retain the protected source storage");
        }
    }
    ~GuardedImage() {
        release();
    }
    GuardedImage(const GuardedImage&) = delete;
    GuardedImage& operator=(const GuardedImage&) = delete;

    const QImage& image() const {
        return m_image;
    }

  private:
    void release() {
        m_image = {};
        if (!m_allocation)
            return;
#ifdef Q_OS_WIN
        VirtualFree(m_allocation, 0, MEM_RELEASE);
#else
        munmap(m_allocation, m_allocationBytes);
#endif
        m_allocation = nullptr;
    }
    uchar* m_allocation = nullptr;
    size_t m_allocationBytes = 0;
    QImage m_image;
};

} // namespace image_split_test
