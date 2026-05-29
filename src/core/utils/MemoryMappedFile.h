// Cross-platform Memory-Mapped File for Claymore Engine
// Provides zero-copy file access for maximum I/O performance
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace claymore {

class MemoryMappedFile {
public:
    MemoryMappedFile() = default;
    ~MemoryMappedFile() { Close(); }
    
    // Non-copyable
    MemoryMappedFile(const MemoryMappedFile&) = delete;
    MemoryMappedFile& operator=(const MemoryMappedFile&) = delete;
    
    // Movable
    MemoryMappedFile(MemoryMappedFile&& other) noexcept {
        *this = std::move(other);
    }
    
    MemoryMappedFile& operator=(MemoryMappedFile&& other) noexcept {
        if (this != &other) {
            Close();
            m_Data = other.m_Data;
            m_Size = other.m_Size;
#ifdef _WIN32
            m_File = other.m_File;
            m_Mapping = other.m_Mapping;
            other.m_File = INVALID_HANDLE_VALUE;
            other.m_Mapping = nullptr;
#else
            m_Fd = other.m_Fd;
            other.m_Fd = -1;
#endif
            other.m_Data = nullptr;
            other.m_Size = 0;
        }
        return *this;
    }
    
    // Open file for reading
    bool Open(const std::string& path) {
        Close();
        
#ifdef _WIN32
        // Windows implementation
        m_File = CreateFileA(
            path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr
        );
        
        if (m_File == INVALID_HANDLE_VALUE) {
            return false;
        }
        
        LARGE_INTEGER fileSize;
        if (!GetFileSizeEx(m_File, &fileSize)) {
            CloseHandle(m_File);
            m_File = INVALID_HANDLE_VALUE;
            return false;
        }
        m_Size = static_cast<size_t>(fileSize.QuadPart);
        
        if (m_Size == 0) {
            // Empty file - valid but no mapping needed
            return true;
        }
        
        m_Mapping = CreateFileMappingA(m_File, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!m_Mapping) {
            CloseHandle(m_File);
            m_File = INVALID_HANDLE_VALUE;
            return false;
        }
        
        m_Data = static_cast<uint8_t*>(MapViewOfFile(m_Mapping, FILE_MAP_READ, 0, 0, 0));
        if (!m_Data) {
            CloseHandle(m_Mapping);
            CloseHandle(m_File);
            m_Mapping = nullptr;
            m_File = INVALID_HANDLE_VALUE;
            return false;
        }
        
#else
        // Unix implementation
        m_Fd = open(path.c_str(), O_RDONLY);
        if (m_Fd < 0) {
            return false;
        }
        
        struct stat st;
        if (fstat(m_Fd, &st) < 0) {
            close(m_Fd);
            m_Fd = -1;
            return false;
        }
        m_Size = static_cast<size_t>(st.st_size);
        
        if (m_Size == 0) {
            return true;
        }
        
        m_Data = static_cast<uint8_t*>(mmap(nullptr, m_Size, PROT_READ, MAP_PRIVATE, m_Fd, 0));
        if (m_Data == MAP_FAILED) {
            m_Data = nullptr;
            close(m_Fd);
            m_Fd = -1;
            return false;
        }
        
        // Hint to OS about sequential access
        madvise(m_Data, m_Size, MADV_SEQUENTIAL);
#endif
        
        return true;
    }
    
    void Close() {
#ifdef _WIN32
        if (m_Data) {
            UnmapViewOfFile(m_Data);
            m_Data = nullptr;
        }
        if (m_Mapping) {
            CloseHandle(m_Mapping);
            m_Mapping = nullptr;
        }
        if (m_File != INVALID_HANDLE_VALUE) {
            CloseHandle(m_File);
            m_File = INVALID_HANDLE_VALUE;
        }
#else
        if (m_Data && m_Size > 0) {
            munmap(m_Data, m_Size);
            m_Data = nullptr;
        }
        if (m_Fd >= 0) {
            close(m_Fd);
            m_Fd = -1;
        }
#endif
        m_Size = 0;
    }
    
    bool IsOpen() const { return m_Data != nullptr || m_Size == 0; }
    const uint8_t* Data() const { return m_Data; }
    size_t Size() const { return m_Size; }
    
    // Convenience accessors
    template<typename T>
    const T* As(size_t offset = 0) const {
        if (offset + sizeof(T) > m_Size) return nullptr;
        return reinterpret_cast<const T*>(m_Data + offset);
    }
    
    template<typename T>
    T Read(size_t offset) const {
        T value{};
        if (offset + sizeof(T) <= m_Size) {
            memcpy(&value, m_Data + offset, sizeof(T));
        }
        return value;
    }

private:
    uint8_t* m_Data = nullptr;
    size_t m_Size = 0;
    
#ifdef _WIN32
    HANDLE m_File = INVALID_HANDLE_VALUE;
    HANDLE m_Mapping = nullptr;
#else
    int m_Fd = -1;
#endif
};

} // namespace claymore

