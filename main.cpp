/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include <acl/acl.h>
#include <dlfcn.h>
#include <sys/mman.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

namespace {
constexpr uint32_t HOST_MEM_MAP_DEV = 3U;
constexpr uint8_t TEST_PATTERN = 0xA5U;
constexpr size_t MIB = 1024U * 1024U;
constexpr size_t GIB = 1024U * MIB;
constexpr size_t MAX_BATCH_COUNT = 4096U;
constexpr size_t KV_LAYER_COUNT = 61U;
constexpr size_t KV_K_BYTES = 128U * 1024U;
constexpr size_t KV_V_BYTES = 16U * 1024U;
constexpr size_t KV_DESCRIPTORS_PER_KEY = KV_LAYER_COUNT * 2U;
constexpr size_t KV_BYTES_PER_KEY = KV_LAYER_COUNT * (KV_K_BYTES + KV_V_BYTES);
constexpr int MAP_HUGE_SHIFT_VALUE = 26;
constexpr int MAP_HUGE_2M_FLAG = 21 << MAP_HUGE_SHIFT_VALUE;
constexpr int MAP_HUGE_1G_FLAG = 30 << MAP_HUGE_SHIFT_VALUE;
constexpr uint64_t HAL_MEM_HOST_FLAG = 0x2ULL << 10U;
constexpr uint64_t HAL_MEM_DDR_FLAG = 0x0ULL << 14U;
constexpr uint64_t HAL_MEM_NORMAL_PAGE_FLAG = 0x0ULL << 17U;
constexpr uint64_t HAL_MEM_HUGE_PAGE_FLAG = 0x1ULL << 17U;

using DVresult = int32_t;
using HalMemcpyBatchFunc = DVresult (*)(uint64_t[], uint64_t[], size_t[], size_t);
using HalHostRegisterFunc = int (*)(void *, uint64_t, uint32_t, uint32_t, void **);
using HalHostUnregisterExFunc = int (*)(void *, uint32_t, uint32_t);
using HalMemAllocFunc = int (*)(void **, uint64_t, uint64_t);
using HalMemFreeFunc = int (*)(void *);

enum class MemoryKind { MMAP_4K, MMAP_2M, MMAP_1G, HAL_NORMAL, HAL_HUGE };
enum class MemorySelection { ALL, MMAP_ALL, MMAP_4K, MMAP_2M, MMAP_1G, HAL_NORMAL, HAL_HUGE };
enum class SetupResult { READY, UNAVAILABLE, ERROR };
enum class TestResult { SUCCESS, SKIPPED, FAILURE };

struct Options {
    int32_t deviceId = 0;
    size_t totalBytes = 256U * MIB;
    size_t batchCount = 16U;
    size_t dataDim = 1U;
    size_t warmup = 5U;
    size_t iterations = 20U;
    MemorySelection memorySelection = MemorySelection::ALL;
};

void PrintUsage(const char *program)
{
    std::cout << "Usage: " << program << " [options]\n"
              << "  --device ID       logic device ID (default: 0)\n"
              << "  --data-dim N      1 for equal-size entries, 2 for MemCache KV layout (default: 1)\n"
              << "  --size-mb MB      data-dim=1 total bytes per batch call (default: 256)\n"
              << "  --batch-count N   data-dim=1 entry count; data-dim=2 key batch size (default: 16)\n"
              << "  --batch-size N    alias of --batch-count\n"
              << "  --warmup N        warmup calls (default: 5)\n"
              << "  --iterations N    measured calls (default: 20)\n"
              << "  --memory MODE     mmap-4k, mmap-2m, mmap-1g, hal-normal, hal-huge, mmap-all, or all\n"
              << "                    (default: all)\n"
              << "  --help            show this help\n";
}

bool ParseUnsigned(const char *text, size_t &value)
{
    if (text == nullptr || text[0] == '\0' || text[0] == '-') {
        std::cerr << "[ERROR] invalid unsigned integer: " << (text == nullptr ? "null" : text) << '\n';
        return false;
    }
    char *end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed > std::numeric_limits<size_t>::max()) {
        std::cerr << "[ERROR] invalid unsigned integer: " << text << '\n';
        return false;
    }
    value = static_cast<size_t>(parsed);
    return true;
}

bool ParseMemorySelection(const char *text, MemorySelection &selection)
{
    const std::string value(text);
    if (value == "mmap-4k") {
        selection = MemorySelection::MMAP_4K;
    } else if (value == "mmap-2m") {
        selection = MemorySelection::MMAP_2M;
    } else if (value == "mmap-1g") {
        selection = MemorySelection::MMAP_1G;
    } else if (value == "hal-normal") {
        selection = MemorySelection::HAL_NORMAL;
    } else if (value == "hal-huge") {
        selection = MemorySelection::HAL_HUGE;
    } else if (value == "mmap-all") {
        selection = MemorySelection::MMAP_ALL;
    } else if (value == "all") {
        selection = MemorySelection::ALL;
    } else {
        std::cerr << "[ERROR] invalid --memory value: " << value << '\n';
        return false;
    }
    return true;
}

bool ReadOptionValue(int argc, char *argv[], int &index, const char *&value)
{
    if (index + 1 >= argc) {
        std::cerr << "[ERROR] missing value for option: " << argv[index] << '\n';
        return false;
    }
    value = argv[++index];
    return true;
}

bool ParseOption(int argc, char *argv[], int &index, Options &options)
{
    const std::string arg(argv[index]);
    const char *value = nullptr;
    if (arg == "--help") {
        PrintUsage(argv[0]);
        std::exit(EXIT_SUCCESS);
    }
    if (!ReadOptionValue(argc, argv, index, value)) {
        return false;
    }
    if (arg == "--device") {
        size_t parsed = 0U;
        if (!ParseUnsigned(value, parsed) || parsed > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
            std::cerr << "[ERROR] invalid device ID: " << value << '\n';
            return false;
        }
        options.deviceId = static_cast<int32_t>(parsed);
        return true;
    }
    if (arg == "--size-mb") {
        size_t parsed = 0U;
        if (!ParseUnsigned(value, parsed) || parsed > std::numeric_limits<size_t>::max() / MIB) {
            std::cerr << "[ERROR] invalid size in MiB: " << value << '\n';
            return false;
        }
        options.totalBytes = parsed * MIB;
        return true;
    }
    if (arg == "--batch-count" || arg == "--batch-size") {
        return ParseUnsigned(value, options.batchCount);
    }
    if (arg == "--data-dim") {
        return ParseUnsigned(value, options.dataDim);
    }
    if (arg == "--warmup") {
        return ParseUnsigned(value, options.warmup);
    }
    if (arg == "--iterations") {
        return ParseUnsigned(value, options.iterations);
    }
    if (arg == "--memory") {
        return ParseMemorySelection(value, options.memorySelection);
    }
    std::cerr << "[ERROR] unknown option: " << arg << '\n';
    return false;
}

bool FinalizeOptions(Options &options)
{
    if (options.dataDim != 1U && options.dataDim != 2U) {
        std::cerr << "[ERROR] data-dim must be 1 or 2, dataDim=" << options.dataDim << '\n';
        return false;
    }
    if (options.batchCount == 0U || options.iterations == 0U) {
        std::cerr << "[ERROR] batch-count and iterations must be greater than zero\n";
        return false;
    }
    if (options.dataDim == 2U) {
        if (options.batchCount > std::numeric_limits<size_t>::max() / KV_BYTES_PER_KEY) {
            std::cerr << "[ERROR] KV workload size overflow, batchCount=" << options.batchCount << '\n';
            return false;
        }
        options.totalBytes = options.batchCount * KV_BYTES_PER_KEY;
        return true;
    }
    if (options.totalBytes == 0U || options.batchCount > MAX_BATCH_COUNT) {
        std::cerr << "[ERROR] invalid data-dim=1 size or batch count, totalBytes=" << options.totalBytes
                  << ", batchCount=" << options.batchCount << ", maxBatchCount=" << MAX_BATCH_COUNT << '\n';
        return false;
    }
    if (options.totalBytes % options.batchCount != 0U) {
        std::cerr << "[ERROR] total bytes must be divisible by batch-count, totalBytes=" << options.totalBytes
                  << ", batchCount=" << options.batchCount << '\n';
        return false;
    }
    return true;
}

bool ParseOptions(int argc, char *argv[], Options &options)
{
    for (int index = 1; index < argc; ++index) {
        if (!ParseOption(argc, argv, index, options)) {
            return false;
        }
    }
    return FinalizeOptions(options);
}

class HalApi {
public:
    bool Open()
    {
        handle_ = dlopen("libascend_hal.so", RTLD_NOW | RTLD_LOCAL);
        if (handle_ == nullptr) {
            std::cerr << "[ERROR] dlopen libascend_hal.so failed: " << dlerror() << '\n';
            return false;
        }
        if (!Load("halMemcpyBatch", memcpyBatch_) || !Load("halHostRegister", hostRegister_) ||
            !Load("halHostUnregisterEx", hostUnregister_)) {
            return false;
        }
        LoadOptional("halMemAlloc", memAlloc_);
        LoadOptional("halMemFree", memFree_);
        return true;
    }

    ~HalApi()
    {
        if (handle_ != nullptr) {
            dlclose(handle_);
        }
    }

    HalMemcpyBatchFunc memcpyBatch_ = nullptr;
    HalHostRegisterFunc hostRegister_ = nullptr;
    HalHostUnregisterExFunc hostUnregister_ = nullptr;
    HalMemAllocFunc memAlloc_ = nullptr;
    HalMemFreeFunc memFree_ = nullptr;

private:
    template <typename T> bool Load(const char *name, T &function)
    {
        dlerror();
        function = reinterpret_cast<T>(dlsym(handle_, name));
        const char *error = dlerror();
        if (error != nullptr || function == nullptr) {
            std::cerr << "[ERROR] dlsym failed, symbol=" << name << ", error="
                      << (error == nullptr ? "unknown" : error) << '\n';
            return false;
        }
        return true;
    }

    template <typename T> void LoadOptional(const char *name, T &function)
    {
        dlerror();
        function = reinterpret_cast<T>(dlsym(handle_, name));
        if (dlerror() != nullptr) {
            function = nullptr;
        }
    }

    void *handle_ = nullptr;
};

class AclRuntime {
public:
    bool Init(int32_t deviceId)
    {
        aclError ret = aclInit(nullptr);
        if (ret != ACL_SUCCESS) {
            std::cerr << "[ERROR] aclInit failed, ret=" << ret << '\n';
            return false;
        }
        initialized_ = true;
        ret = aclrtSetDevice(deviceId);
        if (ret != ACL_SUCCESS) {
            std::cerr << "[ERROR] aclrtSetDevice failed, deviceId=" << deviceId << ", ret=" << ret << '\n';
            return false;
        }
        deviceId_ = deviceId;
        deviceSet_ = true;
        return true;
    }

    ~AclRuntime()
    {
        if (deviceSet_) {
            const aclError ret = aclrtResetDevice(deviceId_);
            if (ret != ACL_SUCCESS) {
                std::cerr << "[ERROR] aclrtResetDevice failed, deviceId=" << deviceId_ << ", ret=" << ret << '\n';
            }
        }
        if (initialized_) {
            const aclError ret = aclFinalize();
            if (ret != ACL_SUCCESS) {
                std::cerr << "[ERROR] aclFinalize failed, ret=" << ret << '\n';
            }
        }
    }

private:
    bool initialized_ = false;
    bool deviceSet_ = false;
    int32_t deviceId_ = 0;
};

class DeviceBuffer {
public:
    bool Allocate(size_t size, size_t alignment = 1U)
    {
        if (alignment == 0U || (alignment & (alignment - 1U)) != 0U ||
            size > std::numeric_limits<size_t>::max() - (alignment - 1U)) {
            std::cerr << "[ERROR] invalid device allocation alignment or size, size=" << size
                      << ", alignment=" << alignment << '\n';
            return false;
        }
        size_ = size;
        allocationSize_ = size + alignment - 1U;
        const aclError ret = aclrtMalloc(&allocationAddress_, allocationSize_, ACL_MEM_MALLOC_HUGE_FIRST);
        if (ret != ACL_SUCCESS || allocationAddress_ == nullptr) {
            std::cerr << "[ERROR] aclrtMalloc failed, size=" << allocationSize_ << ", alignment=" << alignment
                      << ", ret=" << ret << '\n';
            return false;
        }
        const uintptr_t rawAddress = reinterpret_cast<uintptr_t>(allocationAddress_);
        const uintptr_t alignedAddress = (rawAddress + alignment - 1U) & ~(alignment - 1U);
        address_ = reinterpret_cast<void *>(alignedAddress);
        const aclError memsetRet = aclrtMemset(address_, size_, TEST_PATTERN, size_);
        if (memsetRet != ACL_SUCCESS) {
            std::cerr << "[ERROR] aclrtMemset failed, address=" << address_ << ", size=" << size_
                      << ", ret=" << memsetRet << '\n';
            return false;
        }
        return true;
    }

    ~DeviceBuffer()
    {
        if (allocationAddress_ != nullptr) {
            const aclError ret = aclrtFree(allocationAddress_);
            if (ret != ACL_SUCCESS) {
                std::cerr << "[ERROR] aclrtFree failed, allocationAddress=" << allocationAddress_
                          << ", alignedAddress=" << address_ << ", ret=" << ret << '\n';
            }
        }
    }

    uint64_t Address() const
    {
        return reinterpret_cast<uint64_t>(address_);
    }

private:
    void *allocationAddress_ = nullptr;
    void *address_ = nullptr;
    size_t size_ = 0U;
    size_t allocationSize_ = 0U;
};

class SourceBuffers {
public:
    bool Allocate(const Options &options)
    {
        if (options.dataDim == 1U) {
            return primary_.Allocate(options.totalBytes);
        }
        const size_t kBytes = options.batchCount * KV_LAYER_COUNT * KV_K_BYTES;
        const size_t vBytes = options.batchCount * KV_LAYER_COUNT * KV_V_BYTES;
        if (!primary_.Allocate(kBytes, 2U * MIB)) {
            std::cerr << "[ERROR] KV K source allocation failed, size=" << kBytes << '\n';
            return false;
        }
        if (!secondary_.Allocate(vBytes, 2U * MIB)) {
            std::cerr << "[ERROR] KV V source allocation failed, size=" << vBytes << '\n';
            return false;
        }
        return true;
    }

    uint64_t PrimaryAddress() const
    {
        return primary_.Address();
    }

    uint64_t SecondaryAddress() const
    {
        return secondary_.Address();
    }

private:
    DeviceBuffer primary_;
    DeviceBuffer secondary_;
};

const char *MemoryKindName(MemoryKind kind)
{
    switch (kind) {
        case MemoryKind::MMAP_4K:
            return "mmap-4k";
        case MemoryKind::MMAP_2M:
            return "mmap-2m";
        case MemoryKind::MMAP_1G:
            return "mmap-1g";
        case MemoryKind::HAL_NORMAL:
            return "hal-normal";
        case MemoryKind::HAL_HUGE:
            return "hal-huge";
        default:
            return "unknown";
    }
}

bool IsMmapKind(MemoryKind kind)
{
    return kind == MemoryKind::MMAP_4K || kind == MemoryKind::MMAP_2M || kind == MemoryKind::MMAP_1G;
}

size_t RoundUp(size_t value, size_t alignment)
{
    if (value > std::numeric_limits<size_t>::max() - (alignment - 1U)) {
        return 0U;
    }
    return ((value + alignment - 1U) / alignment) * alignment;
}

class RegisteredHostBuffer {
public:
    SetupResult Allocate(size_t copySize, MemoryKind kind, HalApi &hal, int32_t deviceId)
    {
        kind_ = kind;
        deviceId_ = static_cast<uint32_t>(deviceId);
        hal_ = &hal;
        SetupResult result = IsMmapKind(kind) ? AllocateMmap(copySize) : AllocateHal(copySize);
        if (result != SetupResult::READY) {
            return result;
        }
        std::memset(hostAddress_, 0, copySize);
        const int ret =
            hal_->hostRegister_(hostAddress_, allocationSize_, HOST_MEM_MAP_DEV, deviceId_, &deviceAddress_);
        if (ret != 0 || deviceAddress_ == nullptr) {
            std::cerr << "[ERROR] halHostRegister failed, memory=" << MemoryKindName(kind_) << ", hostAddress="
                      << hostAddress_ << ", size=" << allocationSize_ << ", deviceId=" << deviceId_ << ", ret=" << ret
                      << ", dva=" << deviceAddress_ << '\n';
            return SetupResult::ERROR;
        }
        registered_ = true;
        return SetupResult::READY;
    }

    ~RegisteredHostBuffer()
    {
        if (registered_) {
            const int ret = hal_->hostUnregister_(hostAddress_, deviceId_, HOST_MEM_MAP_DEV);
            if (ret != 0) {
                std::cerr << "[ERROR] halHostUnregisterEx failed, hostAddress=" << hostAddress_
                          << ", deviceId=" << deviceId_ << ", ret=" << ret << '\n';
            }
        }
        FreeAllocation();
    }

    size_t AllocationSize() const
    {
        return allocationSize_;
    }

    uint64_t DeviceAddress() const
    {
        return reinterpret_cast<uint64_t>(deviceAddress_);
    }

    const uint8_t *HostAddress() const
    {
        return static_cast<const uint8_t *>(hostAddress_);
    }

private:
    SetupResult AllocateMmap(size_t copySize)
    {
        size_t alignment = 1U;
        int flags = MAP_PRIVATE | MAP_ANONYMOUS;
        if (kind_ == MemoryKind::MMAP_2M) {
            alignment = 2U * MIB;
            flags |= MAP_HUGETLB | MAP_HUGE_2M_FLAG;
        } else if (kind_ == MemoryKind::MMAP_1G) {
            alignment = GIB;
            flags |= MAP_HUGETLB | MAP_HUGE_1G_FLAG;
        }
        allocationSize_ = RoundUp(copySize, alignment);
        if (allocationSize_ == 0U) {
            std::cerr << "[ERROR] mmap allocation size overflow, memory=" << MemoryKindName(kind_)
                      << ", copySize=" << copySize << '\n';
            return SetupResult::ERROR;
        }
        hostAddress_ = mmap(nullptr, allocationSize_, PROT_READ | PROT_WRITE, flags, -1, 0);
        if (hostAddress_ == MAP_FAILED) {
            std::cerr << "[WARN] mmap unavailable, memory=" << MemoryKindName(kind_) << ", size=" << allocationSize_
                      << ", errno=" << errno << ", reason=" << std::strerror(errno) << '\n';
            hostAddress_ = nullptr;
            return SetupResult::UNAVAILABLE;
        }
        if (kind_ == MemoryKind::MMAP_4K && madvise(hostAddress_, allocationSize_, MADV_NOHUGEPAGE) != 0) {
            std::cerr << "[ERROR] madvise MADV_NOHUGEPAGE failed, address=" << hostAddress_
                      << ", size=" << allocationSize_ << ", errno=" << errno << '\n';
            return SetupResult::ERROR;
        }
        return SetupResult::READY;
    }

    SetupResult AllocateHal(size_t copySize)
    {
        if (hal_->memAlloc_ == nullptr || hal_->memFree_ == nullptr) {
            std::cerr << "[WARN] halMemAlloc/halMemFree symbols are unavailable, memory=" << MemoryKindName(kind_)
                      << '\n';
            return SetupResult::UNAVAILABLE;
        }
        allocationSize_ = copySize;
        uint64_t flags = HAL_MEM_HOST_FLAG | HAL_MEM_DDR_FLAG;
        flags |= kind_ == MemoryKind::HAL_HUGE ? HAL_MEM_HUGE_PAGE_FLAG : HAL_MEM_NORMAL_PAGE_FLAG;
        const int ret = hal_->memAlloc_(&hostAddress_, allocationSize_, flags);
        if (ret != 0 || hostAddress_ == nullptr) {
            std::cerr << "[WARN] halMemAlloc unavailable, memory=" << MemoryKindName(kind_)
                      << ", size=" << allocationSize_ << ", flags=0x" << std::hex << flags << std::dec
                      << ", ret=" << ret << ", address=" << hostAddress_ << '\n';
            return SetupResult::UNAVAILABLE;
        }
        halAllocated_ = true;
        return SetupResult::READY;
    }

    void FreeAllocation() noexcept
    {
        if (hostAddress_ == nullptr) {
            return;
        }
        if (halAllocated_) {
            const int ret = hal_->memFree_(hostAddress_);
            if (ret != 0) {
                std::cerr << "[ERROR] halMemFree failed, address=" << hostAddress_ << ", ret=" << ret << '\n';
            }
            return;
        }
        if (munmap(hostAddress_, allocationSize_) != 0) {
            std::cerr << "[ERROR] munmap failed, hostAddress=" << hostAddress_ << ", size=" << allocationSize_
                      << ", errno=" << errno << '\n';
        }
    }

    HalApi *hal_ = nullptr;
    void *hostAddress_ = nullptr;
    void *deviceAddress_ = nullptr;
    size_t allocationSize_ = 0U;
    uint32_t deviceId_ = 0U;
    MemoryKind kind_ = MemoryKind::MMAP_4K;
    bool registered_ = false;
    bool halAllocated_ = false;
};

struct BatchArguments {
    std::vector<uint64_t> dst;
    std::vector<uint64_t> src;
    std::vector<size_t> size;
};

using BatchWorkload = std::vector<BatchArguments>;

BatchWorkload MakeOneDimensionalWorkload(uint64_t dstBase, uint64_t srcBase, size_t totalBytes, size_t count)
{
    BatchArguments args{std::vector<uint64_t>(count), std::vector<uint64_t>(count), std::vector<size_t>(count)};
    const size_t bytesPerEntry = totalBytes / count;
    for (size_t index = 0; index < count; ++index) {
        const size_t offset = index * bytesPerEntry;
        args.dst[index] = dstBase + offset;
        args.src[index] = srcBase + offset;
        args.size[index] = bytesPerEntry;
    }
    BatchWorkload workload;
    workload.emplace_back(std::move(args));
    return workload;
}

BatchArguments MakeKvKeyArguments(uint64_t dstBase, uint64_t kBase, uint64_t vBase, size_t batchSize,
                                  size_t keyIndex)
{
    BatchArguments args;
    args.dst.reserve(KV_DESCRIPTORS_PER_KEY);
    args.src.reserve(KV_DESCRIPTORS_PER_KEY);
    args.size.reserve(KV_DESCRIPTORS_PER_KEY);
    size_t dstOffset = keyIndex * KV_BYTES_PER_KEY;
    for (size_t layer = 0; layer < KV_LAYER_COUNT; ++layer) {
        const size_t blockIndex = layer * batchSize + keyIndex;
        args.dst.push_back(dstBase + dstOffset);
        args.src.push_back(kBase + blockIndex * KV_K_BYTES);
        args.size.push_back(KV_K_BYTES);
        dstOffset += KV_K_BYTES;
        args.dst.push_back(dstBase + dstOffset);
        args.src.push_back(vBase + blockIndex * KV_V_BYTES);
        args.size.push_back(KV_V_BYTES);
        dstOffset += KV_V_BYTES;
    }
    return args;
}

BatchWorkload MakeKvWorkload(uint64_t dstBase, uint64_t kBase, uint64_t vBase, size_t batchSize)
{
    BatchWorkload workload;
    workload.reserve(batchSize);
    for (size_t keyIndex = 0; keyIndex < batchSize; ++keyIndex) {
        workload.emplace_back(MakeKvKeyArguments(dstBase, kBase, vBase, batchSize, keyIndex));
    }
    return workload;
}

BatchWorkload MakeWorkload(const Options &options, uint64_t dstBase, const SourceBuffers &source)
{
    if (options.dataDim == 2U) {
        return MakeKvWorkload(dstBase, source.PrimaryAddress(), source.SecondaryAddress(), options.batchCount);
    }
    return MakeOneDimensionalWorkload(dstBase, source.PrimaryAddress(), options.totalBytes, options.batchCount);
}

bool RunWorkloadOnce(HalApi &hal, BatchWorkload &workload, size_t iteration)
{
    for (size_t callIndex = 0; callIndex < workload.size(); ++callIndex) {
        auto &args = workload[callIndex];
        const size_t count = args.size.size();
        const DVresult ret = hal.memcpyBatch_(args.dst.data(), args.src.data(), args.size.data(), count);
        if (ret != 0) {
            std::cerr << "[ERROR] halMemcpyBatch failed, iteration=" << iteration << ", callIndex=" << callIndex
                      << ", count=" << count << ", ret=" << ret << '\n';
            return false;
        }
    }
    return true;
}

bool RunCopies(HalApi &hal, BatchWorkload &workload, size_t iterations, std::vector<double> &latenciesMs)
{
    latenciesMs.reserve(iterations);
    for (size_t index = 0; index < iterations; ++index) {
        const auto begin = std::chrono::steady_clock::now();
        const bool success = RunWorkloadOnce(hal, workload, index);
        const auto end = std::chrono::steady_clock::now();
        if (!success) {
            return false;
        }
        latenciesMs.push_back(std::chrono::duration<double, std::milli>(end - begin).count());
    }
    return true;
}

bool Verify(const uint8_t *data, size_t size)
{
    for (size_t index = 0; index < size; ++index) {
        if (data[index] != TEST_PATTERN) {
            std::cerr << "[ERROR] verification failed, offset=" << index << ", actual=0x" << std::hex
                      << static_cast<uint32_t>(data[index]) << ", expected=0x" << static_cast<uint32_t>(TEST_PATTERN)
                      << std::dec << '\n';
            return false;
        }
    }
    return true;
}

double Percentile(std::vector<double> values, double ratio)
{
    std::sort(values.begin(), values.end());
    const size_t index = static_cast<size_t>(ratio * static_cast<double>(values.size() - 1U));
    return values[index];
}

size_t CountDescriptors(const BatchWorkload &workload)
{
    return std::accumulate(workload.begin(), workload.end(), size_t{0},
                           [](size_t total, const BatchArguments &args) { return total + args.size.size(); });
}

void PrintExperimentHeader(const char *memoryName, const Options &options)
{
    constexpr const char *separator = "================================================================";
    const double copyMib = static_cast<double>(options.totalBytes) / static_cast<double>(MIB);
    std::cout << '\n' << separator << '\n'
              << std::fixed << std::setprecision(3) << "[EXPERIMENT] memory=" << memoryName
              << " dataDim=" << options.dataDim << " batchSize=" << options.batchCount << " copyMiB=" << copyMib
              << '\n'
              << separator << '\n';
}

void PrintResult(const char *memoryName, const Options &options, size_t allocationSize,
                 const BatchWorkload &workload, const std::vector<double> &latenciesMs)
{
    const double totalMs = std::accumulate(latenciesMs.begin(), latenciesMs.end(), 0.0);
    const double averageMs = totalMs / static_cast<double>(latenciesMs.size());
    const double gib = static_cast<double>(options.totalBytes) / static_cast<double>(1ULL << 30U);
    const double gibPerSecond = gib / (averageMs / 1000.0);
    const double copyMib = static_cast<double>(options.totalBytes) / static_cast<double>(MIB);
    const double allocatedMib = static_cast<double>(allocationSize) / static_cast<double>(MIB);
    std::cout << std::fixed << std::setprecision(3) << "[RESULT] memory=" << memoryName
              << " dataDim=" << options.dataDim << " copyMiB=" << copyMib << " allocatedMiB=" << allocatedMib
              << " batchSize=" << options.batchCount << " halCalls=" << workload.size()
              << " descriptors=" << CountDescriptors(workload);
    if (options.dataDim == 1U) {
        std::cout << " bytesPerEntry=" << options.totalBytes / options.batchCount;
    } else {
        std::cout << " descriptorsPerCall=" << KV_DESCRIPTORS_PER_KEY << " kBytes=" << KV_K_BYTES
                  << " vBytes=" << KV_V_BYTES;
    }
    std::cout << " avgMs=" << averageMs << " p50Ms=" << Percentile(latenciesMs, 0.50)
              << " p95Ms=" << Percentile(latenciesMs, 0.95) << " bandwidthGiB/s=" << gibPerSecond << '\n';
}

TestResult RunMemoryTest(MemoryKind kind, const Options &options, HalApi &hal, const SourceBuffers &source)
{
    const char *memoryName = MemoryKindName(kind);
    PrintExperimentHeader(memoryName, options);
    RegisteredHostBuffer destination;
    const SetupResult setup = destination.Allocate(options.totalBytes, kind, hal, options.deviceId);
    if (setup == SetupResult::UNAVAILABLE) {
        std::cout << "[SKIPPED] memory=" << memoryName << '\n';
        return TestResult::SKIPPED;
    }
    if (setup == SetupResult::ERROR) {
        std::cerr << "[ERROR] destination setup failed, memory=" << memoryName << '\n';
        return TestResult::FAILURE;
    }
    std::cout << "[INFO] memory=" << memoryName << " allocatedBytes=" << destination.AllocationSize()
              << " hostVa=" << static_cast<const void *>(destination.HostAddress()) << " dstDva=0x" << std::hex
              << destination.DeviceAddress() << " primarySrcDva=0x" << source.PrimaryAddress();
    if (options.dataDim == 2U) {
        std::cout << " secondarySrcDva=0x" << source.SecondaryAddress();
    }
    std::cout << std::dec << '\n';
    const uint64_t hostDestination = reinterpret_cast<uint64_t>(destination.HostAddress());
    BatchWorkload workload = MakeWorkload(options, hostDestination, source);
    std::vector<double> ignored;
    if (!RunCopies(hal, workload, options.warmup, ignored)) {
        std::cerr << "[ERROR] warmup failed, memory=" << memoryName << '\n';
        return TestResult::FAILURE;
    }
    std::vector<double> latenciesMs;
    if (!RunCopies(hal, workload, options.iterations, latenciesMs)) {
        std::cerr << "[ERROR] measured copy failed, memory=" << memoryName << '\n';
        return TestResult::FAILURE;
    }
    if (!Verify(destination.HostAddress(), options.totalBytes)) {
        std::cerr << "[ERROR] data verification failed, memory=" << memoryName << '\n';
        return TestResult::FAILURE;
    }
    PrintResult(memoryName, options, destination.AllocationSize(), workload, latenciesMs);
    return TestResult::SUCCESS;
}

std::vector<MemoryKind> SelectedMemoryKinds(MemorySelection selection)
{
    if (selection == MemorySelection::MMAP_ALL) {
        return {MemoryKind::MMAP_4K, MemoryKind::MMAP_2M, MemoryKind::MMAP_1G};
    }
    if (selection == MemorySelection::ALL) {
        return {MemoryKind::HAL_NORMAL, MemoryKind::HAL_HUGE, MemoryKind::MMAP_4K, MemoryKind::MMAP_2M,
                MemoryKind::MMAP_1G};
    }
    switch (selection) {
        case MemorySelection::MMAP_4K:
            return {MemoryKind::MMAP_4K};
        case MemorySelection::MMAP_2M:
            return {MemoryKind::MMAP_2M};
        case MemorySelection::MMAP_1G:
            return {MemoryKind::MMAP_1G};
        case MemorySelection::HAL_NORMAL:
            return {MemoryKind::HAL_NORMAL};
        case MemorySelection::HAL_HUGE:
            return {MemoryKind::HAL_HUGE};
        default:
            std::cerr << "[ERROR] invalid memory selection\n";
            return {};
    }
}

int Run(const Options &options)
{
    AclRuntime runtime;
    if (!runtime.Init(options.deviceId)) {
        std::cerr << "[ERROR] ACL runtime initialization failed, deviceId=" << options.deviceId << '\n';
        return EXIT_FAILURE;
    }
    HalApi hal;
    if (!hal.Open()) {
        std::cerr << "[ERROR] HAL API initialization failed\n";
        return EXIT_FAILURE;
    }
    SourceBuffers source;
    if (!source.Allocate(options)) {
        std::cerr << "[ERROR] source device allocation failed, size=" << options.totalBytes << '\n';
        return EXIT_FAILURE;
    }
    size_t successfulTests = 0U;
    bool failed = false;
    for (const MemoryKind kind : SelectedMemoryKinds(options.memorySelection)) {
        const TestResult result = RunMemoryTest(kind, options, hal, source);
        successfulTests += result == TestResult::SUCCESS ? 1U : 0U;
        failed = result == TestResult::FAILURE || failed;
    }
    if (successfulTests == 0U) {
        std::cerr << "[ERROR] no memory backend completed successfully\n";
        return EXIT_FAILURE;
    }
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
} // namespace

int main(int argc, char *argv[])
{
    Options options;
    if (!ParseOptions(argc, argv, options)) {
        std::cerr << "[ERROR] option parsing failed; use --help for usage\n";
        return EXIT_FAILURE;
    }
    return Run(options);
}
