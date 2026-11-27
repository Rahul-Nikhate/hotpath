// Named shared-memory region that two processes can map.
//
//   Linux, huge=true  : file on hugetlbfs (/dev/hugepages), 2 MB pages,
//                       MAP_POPULATE so every page is faulted in at creation
//   Linux, huge=false : POSIX shm (/dev/shm), 4 KB pages, MAP_POPULATE if prefault
//   macOS             : POSIX shm; no huge pages; prefault by touching each page
//
// The creator owns the name and unlinks it on destruction; processes that
// already mapped it keep their mapping. Objects are constructed in the region
// with placement new by the creator; attachers validate a layout hash first.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace hp {

struct RegionInfo {
    bool huge = false;        // backed by huge pages
    bool prefaulted = false;  // every page touched at creation
    bool locked = false;      // mlock succeeded (never swapped)
    size_t page_size = 4096;
};

class Region {
public:
    Region() = default;
    ~Region();
    Region(Region&& o) noexcept { *this = static_cast<Region&&>(o); }
    Region& operator=(Region&& o) noexcept;
    Region(const Region&) = delete;
    Region& operator=(const Region&) = delete;

    // Throws std::runtime_error on failure (e.g. no huge pages reserved).
    static Region create(const std::string& name, size_t size, bool huge, bool prefault);
    // Waits up to timeout_ms for the creator. Throws on timeout.
    static Region attach(const std::string& name, bool huge, int timeout_ms);

    void* data() const { return addr_; }
    size_t size() const { return size_; }
    const RegionInfo& info() const { return info_; }
    explicit operator bool() const { return addr_ != nullptr; }

private:
    void release() noexcept;

    void* addr_ = nullptr;
    size_t size_ = 0;
    std::string path_;      // shm name or hugetlbfs path
    bool owner_ = false;
    bool hugetlbfs_ = false;
    RegionInfo info_;
};

}  // namespace hp
