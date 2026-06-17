#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>

#ifdef GEM5
#include <gem5/m5ops.h>
#define ROI_BEGIN() m5_work_begin(0, 0)
#define ROI_END() m5_work_end(0, 0)
#else
#define ROI_BEGIN() do {} while (0)
#define ROI_END() do {} while (0)
#endif

template <typename T>
class AlignedBuffer
{
  public:
    explicit AlignedBuffer(std::size_t count) : count_(count)
    {
        std::size_t bytes = count * sizeof(T);
        bytes = (bytes + 63) & ~std::size_t(63);
        ptr_ = static_cast<T *>(std::aligned_alloc(64, bytes));
        if (!ptr_) {
            std::perror("aligned_alloc");
            std::abort();
        }
    }

    ~AlignedBuffer() { std::free(ptr_); }

    T *data() { return ptr_; }
    const T *data() const { return ptr_; }
    std::size_t size() const { return count_; }

  private:
    T *ptr_ = nullptr;
    std::size_t count_ = 0;
};

static inline std::uint64_t spm_addr(unsigned way, unsigned set,
                                     unsigned offset = 0)
{
    return (std::uint64_t(way) << 16) | (std::uint64_t(set) << 6) | offset;
}
