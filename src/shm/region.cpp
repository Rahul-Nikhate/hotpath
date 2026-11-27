#include "shm/region.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>

namespace hp {
namespace {

[[maybe_unused]] constexpr size_t kHugePage = 2u << 20;

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error(what + ": " + std::strerror(errno));
}

std::string shm_name(const std::string& name) { return "/hp_" + name; }  // macOS: <= 31 chars
std::string huge_path(const std::string& name) { return "/dev/hugepages/hp_" + name; }

size_t round_up(size_t n, size_t to) { return (n + to - 1) / to * to; }

}  // namespace

Region::~Region() { release(); }

void Region::release() noexcept {
    if (addr_) ::munmap(addr_, size_);
    if (owner_) {
        if (hugetlbfs_) ::unlink(path_.c_str());
        else ::shm_unlink(path_.c_str());
    }
    addr_ = nullptr;
    owner_ = false;
}

Region& Region::operator=(Region&& o) noexcept {
    if (this == &o) return *this;
    release();
    addr_ = o.addr_;
    size_ = o.size_;
    path_ = std::move(o.path_);
    owner_ = o.owner_;
    hugetlbfs_ = o.hugetlbfs_;
    info_ = o.info_;
    o.addr_ = nullptr;
    o.owner_ = false;
    return *this;
}

Region Region::create(const std::string& name, size_t size, bool huge, bool prefault) {
    Region r;
    int fd = -1;
    int flags = MAP_SHARED;
#ifdef __linux__
    if (huge) {
        r.hugetlbfs_ = true;
        r.path_ = huge_path(name);
        ::unlink(r.path_.c_str());
        fd = ::open(r.path_.c_str(), O_CREAT | O_RDWR | O_EXCL, 0600);
        if (fd < 0) fail("open " + r.path_ + " (is hugetlbfs mounted and are huge pages reserved?)");
        size = round_up(size, kHugePage);
        r.info_.huge = true;
        r.info_.page_size = kHugePage;
        prefault = true;  // hugetlbfs pages are reserved up front anyway
    }
    if (prefault) flags |= MAP_POPULATE;
#else
    if (huge) throw std::runtime_error("huge pages are only supported on Linux");
#endif
    if (!r.hugetlbfs_) {
        r.path_ = shm_name(name);
        ::shm_unlink(r.path_.c_str());  // stale region from a crashed run
        fd = ::shm_open(r.path_.c_str(), O_CREAT | O_RDWR | O_EXCL, 0600);
        if (fd < 0) fail("shm_open " + r.path_);
        r.info_.page_size = size_t(::sysconf(_SC_PAGESIZE));  // 4 KB on x86 Linux, 16 KB on Apple Silicon
        size = round_up(size, r.info_.page_size);
    }
    r.owner_ = true;
    if (::ftruncate(fd, off_t(size)) != 0) {
        ::close(fd);
        fail("ftruncate");
    }
    void* p = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, flags, fd, 0);
    ::close(fd);
    if (p == MAP_FAILED) fail("mmap");
    r.addr_ = p;
    r.size_ = size;

    if (prefault) {
        // Write (not read) one byte per page: a read of untouched memory may map
        // the shared zero page and fault again on the first write.
        volatile char* c = static_cast<char*>(p);
        for (size_t off = 0; off < size; off += 4096) c[off] = 0;
        r.info_.prefaulted = true;
    }
    // Lock only when pre-faulting: mlock itself faults every page in, which
    // would silently turn a "lazy" region into a pre-faulted one.
    if (prefault) r.info_.locked = ::mlock(p, size) == 0;  // best effort (needs RLIMIT_MEMLOCK)
    return r;
}

Region Region::attach(const std::string& name, bool huge, int timeout_ms) {
    Region r;
    r.hugetlbfs_ = huge;
    r.path_ = huge ? huge_path(name) : shm_name(name);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    int fd = -1;
    for (;;) {
        fd = huge ? ::open(r.path_.c_str(), O_RDWR) : ::shm_open(r.path_.c_str(), O_RDWR, 0600);
        if (fd >= 0) {
            struct stat st {};
            if (::fstat(fd, &st) == 0 && st.st_size > 0) break;  // creator has sized it
            ::close(fd);
            fd = -1;
        }
        if (std::chrono::steady_clock::now() > deadline) fail("attach " + r.path_ + " (timed out)");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    struct stat st {};
    ::fstat(fd, &st);
    void* p = ::mmap(nullptr, size_t(st.st_size), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (p == MAP_FAILED) fail("mmap attach");
    r.addr_ = p;
    r.size_ = size_t(st.st_size);
    r.info_.huge = huge;
    return r;
}

}  // namespace hp
