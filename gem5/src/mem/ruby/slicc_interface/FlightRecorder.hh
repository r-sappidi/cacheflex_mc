/* Bounded host-side Ruby diagnostic recorder. No simulated events or traffic. */
#ifndef __MEM_RUBY_FLIGHT_RECORDER_HH__
#define __MEM_RUBY_FLIGHT_RECORDER_HH__

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace gem5 { namespace ruby {

// Linux MAP_SHARED pages survive process abort/SIGKILL in the host page cache.
// This is NOT a power-loss durability guarantee. Commit tags and a checksum
// allow readers to discard an interrupted overwrite rather than trust it.
class FlightRecorder
{
  public:
    static constexpr size_t Header = 64, Record = 1024, Text = 992;
    FlightRecorder(const std::string &path, uint64_t capacity)
      : capacity(capacity)
    {
        if (!capacity || capacity > (std::numeric_limits<size_t>::max()-Header)/Record)
            throw std::runtime_error("Invalid flight-recorder capacity");
        bytes = Header + capacity*Record;
        int fd = ::open(path.c_str(), O_RDWR|O_CREAT|O_EXCL, 0600);
        if (fd < 0) throw std::runtime_error("Cannot create flight recorder: " + path);
        if (::ftruncate(fd, bytes) != 0) {
            ::close(fd); throw std::runtime_error("Cannot size flight recorder: " + path);
        }
        void *ptr = ::mmap(nullptr, bytes, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
        ::close(fd);
        if (ptr == MAP_FAILED) throw std::runtime_error("Cannot map flight recorder: " + path);
        data = static_cast<unsigned char *>(ptr);
        std::memcpy(data, "CFRUBY01", 8);
        put<uint64_t>(data+8, capacity);
        put<uint64_t>(data+16, Record);
        put<uint64_t>(data+24, 0x0102030405060708ULL);
        ::msync(data, Header, MS_SYNC);
    }
    ~FlightRecorder() { if (data) { ::msync(data, bytes, MS_SYNC); ::munmap(data, bytes); } }
    FlightRecorder(const FlightRecorder &) = delete;
    FlightRecorder &operator=(const FlightRecorder &) = delete;

    void append(uint64_t tick, const std::string &text)
    {
        const uint64_t seq = ++sequence;
        auto *slot = data+Header+((seq-1)%capacity)*Record;
        __atomic_store_n(reinterpret_cast<uint64_t *>(slot), 0, __ATOMIC_SEQ_CST);
        __atomic_store_n(reinterpret_cast<uint64_t *>(slot+1016), 0, __ATOMIC_SEQ_CST);
        const uint32_t length = std::min(text.size(), Text);
        put<uint64_t>(slot+8, tick);
        put<uint32_t>(slot+16, length);
        std::memcpy(slot+24, text.data(), length);
        if (text.size() > Text) std::memcpy(slot+24+Text-11, "[TRUNCATED]", 11);
        uint32_t hash = 2166136261U;
        for (size_t i=8; i<20; ++i) hash = (hash ^ slot[i])*16777619U;
        for (size_t i=0; i<length; ++i) hash = (hash ^ slot[24+i])*16777619U;
        put<uint32_t>(slot+20, hash);
        __atomic_store_n(reinterpret_cast<uint64_t *>(slot+1016), seq, __ATOMIC_RELEASE);
        __atomic_store_n(reinterpret_cast<uint64_t *>(slot), seq, __ATOMIC_RELEASE);
    }

  private:
    template<class T> static void put(unsigned char *p, T value) { std::memcpy(p, &value, sizeof(value)); }
    uint64_t capacity, sequence = 0;
    size_t bytes = 0;
    unsigned char *data = nullptr;
};

} }
#endif
