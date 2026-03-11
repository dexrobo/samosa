#include "dex/infrastructure/shared_memory/shared_memory.h"

#include <dlfcn.h>

#include <array>

#include "gtest/gtest.h"
#include "spdlog/sinks/stdout_sinks.h"

#include "dex/infrastructure/shared_memory/shared_memory_impl.h"

namespace {

constexpr size_t kFrameSize = 64;
using ArrayBuffer = std::array<char, kFrameSize>;

template <typename Buffer, size_t buffer_size = 2>
struct SharedMemoryBuffer {
  std::atomic<int> read_index;
  std::atomic<int> write_index;
  std::array<Buffer, buffer_size> buffers;
  uint32_t version{};
};

using SharedArrayBuffer = dex::shared_memory::SharedMemory<ArrayBuffer, 2, SharedMemoryBuffer>;

// Store pointers to real system calls
using ShmOpenFunc = int (*)(const char*, int, mode_t);
using FtruncateFunc = int (*)(int, off_t);
using MmapFunc = void* (*)(void*, size_t, int, int, int, off_t);

class SystemCallControls {
 public:
  static const SystemCallControls& Instance() {
    static const SystemCallControls instance;
    return instance;
  }

  struct State {
    bool should_fail_shm_open = false;
    bool should_fail_ftruncate = false;
    bool should_fail_mmap = false;
  };

  void ResetFailureFlags() const { state_ = State{}; }

  [[nodiscard]] State& GetState() const { return state_; }

  [[nodiscard]] int ShmOpen(const char* name, int oflag, mode_t mode) const {
    if (state_.should_fail_shm_open) {
      errno = EACCES;
      return -1;
    }
    return real_shm_open_(name, oflag, mode);
  }

  [[nodiscard]] int Ftruncate(int fd, off_t length) const {
    if (state_.should_fail_ftruncate) {
      errno = ENOSPC;
      return -1;
    }
    return real_ftruncate_(fd, length);
  }

  [[nodiscard]] void* Mmap(void* addr, size_t len, int prot, int flags, int fd, off_t offset) const {
    if (state_.should_fail_mmap) {
      errno = ENOMEM;
      return MAP_FAILED;
    }
    return real_mmap_(addr, len, prot, flags, fd, offset);
  }

 private:
  SystemCallControls()
      : real_shm_open_(reinterpret_cast<ShmOpenFunc>(dlsym(RTLD_NEXT, "shm_open"))),
        real_ftruncate_(reinterpret_cast<FtruncateFunc>(dlsym(RTLD_NEXT, "ftruncate"))),
        real_mmap_(reinterpret_cast<MmapFunc>(dlsym(RTLD_NEXT, "mmap"))) {}

  // System call function pointers
  ShmOpenFunc real_shm_open_;
  FtruncateFunc real_ftruncate_;
  MmapFunc real_mmap_;

  mutable State state_;  // mutable because it's modified through const GetState()
};

inline const SystemCallControls& syscall_controls = SystemCallControls::Instance();

class SharedMemoryTest : public testing::Test {
 protected:
  void SetUp() override {
    shared_memory_name_ =
        std::string("test_shared_memory_") + testing::UnitTest::GetInstance()->current_test_info()->name();
  }

  void TearDown() override { [[maybe_unused]] const bool result = SharedArrayBuffer::Destroy(shared_memory_name_); }

  std::string shared_memory_name_;
};

class SystemCallFixture : public SharedMemoryTest {
 protected:
  void TearDown() override {
    SharedMemoryTest::TearDown();
    syscall_controls.ResetFailureFlags();
  }
};

// System call overrides
extern "C" {
int shm_open(const char* name, int oflag, mode_t mode) { return syscall_controls.ShmOpen(name, oflag, mode); }

int ftruncate(int fd, off_t length) { return syscall_controls.Ftruncate(fd, length); }

void* mmap(void* addr, size_t len, int prot, int flags, int fd, off_t offset) {
  return syscall_controls.Mmap(addr, len, prot, flags, fd, offset);
}
}

TEST_F(SharedMemoryTest, Create) {
  const SharedArrayBuffer shared_memory = SharedArrayBuffer::Create(shared_memory_name_);
  EXPECT_TRUE(shared_memory.IsValid());
  EXPECT_NE(shared_memory.Get(), nullptr);
}

TEST_F(SharedMemoryTest, OpenNonexistent) {
  const SharedArrayBuffer shared_memory = SharedArrayBuffer::Open(shared_memory_name_);
  EXPECT_FALSE(shared_memory.IsValid());
  EXPECT_EQ(shared_memory.Get(), nullptr);
}

TEST_F(SharedMemoryTest, CreateInitFailure) {
  const SharedArrayBuffer shared_memory = SharedArrayBuffer::Create(shared_memory_name_, [](auto) { return false; });
  EXPECT_FALSE(shared_memory.IsValid());
  EXPECT_EQ(shared_memory.Get(), nullptr);
}

TEST_F(SharedMemoryTest, OpenValidateFailure) {
  {
    const SharedArrayBuffer shared_memory = SharedArrayBuffer::Create(shared_memory_name_);
    EXPECT_TRUE(shared_memory.IsValid());
  }
  const SharedArrayBuffer shared_memory = SharedArrayBuffer::Open(shared_memory_name_, [](auto) { return false; });
  EXPECT_FALSE(shared_memory.IsValid());
  EXPECT_EQ(shared_memory.Get(), nullptr);
}

TEST_F(SharedMemoryTest, Destroy) {
  // First create
  {
    const SharedArrayBuffer shared_memory = SharedArrayBuffer::Create(shared_memory_name_);
    EXPECT_TRUE(shared_memory.IsValid());
  }

  // Then destroy
  EXPECT_TRUE(SharedArrayBuffer::Destroy(shared_memory_name_));

  // Verify can't open after destroy
  const SharedArrayBuffer shared_memory = SharedArrayBuffer::Open(shared_memory_name_);
  EXPECT_FALSE(shared_memory.IsValid());
}

TEST_F(SharedMemoryTest, GetBufferByState) {
  const SharedArrayBuffer shared_memory = SharedArrayBuffer::Create(shared_memory_name_);
  EXPECT_TRUE(shared_memory.IsValid());

  auto buffer =
      dex::shared_memory::detail::GetBufferByState(shared_memory, dex::shared_memory::detail::BufferState::BufferA);
  EXPECT_NE(buffer, nullptr);

  buffer =
      dex::shared_memory::detail::GetBufferByState(shared_memory, dex::shared_memory::detail::BufferState::Unavailable);
  EXPECT_EQ(buffer, nullptr);

  buffer =
      dex::shared_memory::detail::GetBufferByState(shared_memory, dex::shared_memory::detail::BufferState::BufferB);
  EXPECT_NE(buffer, nullptr);

  const SharedArrayBuffer shared_memory_invalid = SharedArrayBuffer::Open("invalid_name");
  buffer = dex::shared_memory::detail::GetBufferByState(shared_memory_invalid,
                                                        dex::shared_memory::detail::BufferState::BufferB);
  EXPECT_EQ(buffer, nullptr);
}

TEST_F(SystemCallFixture, HandlesShmOpenFailure) {
  syscall_controls.GetState().should_fail_shm_open = true;
  const SharedArrayBuffer shared_memory = SharedArrayBuffer::Create(shared_memory_name_);
  EXPECT_FALSE(shared_memory.IsValid());
}

TEST_F(SystemCallFixture, HandlesFtruncateFailure) {
  syscall_controls.GetState().should_fail_ftruncate = true;
  const SharedArrayBuffer shared_memory = SharedArrayBuffer::Create(shared_memory_name_);
  EXPECT_FALSE(shared_memory.IsValid());
}

TEST_F(SystemCallFixture, HandlesMmapFailure) {
  syscall_controls.GetState().should_fail_mmap = true;
  const SharedArrayBuffer shared_memory = SharedArrayBuffer::Create(shared_memory_name_);
  EXPECT_FALSE(shared_memory.IsValid());
}

TEST_F(SharedMemoryTest, SizeMismatch) {
  using SharedSmallMemory = dex::shared_memory::SharedMemory<std::array<char, 64>, 2, SharedMemoryBuffer>;
  using SharedLargeMemory = dex::shared_memory::SharedMemory<std::array<char, 128>, 2, SharedMemoryBuffer>;

  // Scenario 1: Small Producer, Large Consumer -> Fail
  {
    {
      const SharedSmallMemory small_mem = SharedSmallMemory::Create(shared_memory_name_);
      EXPECT_TRUE(small_mem.IsValid());
    }
    const SharedLargeMemory large_mem = SharedLargeMemory::Open(shared_memory_name_);
    EXPECT_FALSE(large_mem.IsValid());
    EXPECT_TRUE(SharedLargeMemory::Destroy(shared_memory_name_));
  }

  // Scenario 2: Large Producer, Small Consumer -> Fail (Strict Equality)
  {
    {
      const SharedLargeMemory large_mem = SharedLargeMemory::Create(shared_memory_name_);
      EXPECT_TRUE(large_mem.IsValid());
    }
    const SharedSmallMemory small_mem = SharedSmallMemory::Open(shared_memory_name_);
    EXPECT_FALSE(small_mem.IsValid());
    EXPECT_TRUE(SharedSmallMemory::Destroy(shared_memory_name_));
  }

  // Scenario 3: Exact Match -> Pass
  {
    {
      const SharedSmallMemory small_mem = SharedSmallMemory::Create(shared_memory_name_);
      EXPECT_TRUE(small_mem.IsValid());
    }
    const SharedSmallMemory small_mem_again = SharedSmallMemory::Open(shared_memory_name_);
    EXPECT_TRUE(small_mem_again.IsValid());
  }
}

}  // namespace
