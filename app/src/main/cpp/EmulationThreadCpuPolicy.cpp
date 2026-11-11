#include "EmulationThreadCpuPolicy.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <limits>

#if defined(__ANDROID__) || defined(__linux__)
#include <sched.h>
#endif
#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace MelonDSAndroid
{
namespace
{
thread_local EmulationThreadCpuPolicy* currentPolicy = nullptr;

class SystemCpuAffinityBackend final : public CpuAffinityBackend
{
public:
    int getAffinity(CpuAffinityMask& mask) noexcept override
    {
        mask.reset();
#if defined(__ANDROID__) || defined(__linux__)
        static_assert(CPU_SETSIZE <= CpuAffinityMask{}.size());
        cpu_set_t native;
        CPU_ZERO(&native);
        if (sched_getaffinity(0, sizeof(native), &native) != 0)
            return errno;
        for (std::size_t cpu = 0; cpu < CPU_SETSIZE; ++cpu)
            if (CPU_ISSET(cpu, &native))
                mask.set(cpu);
        return mask.any() ? 0 : EINVAL;
#else
        (void)mask;
        return ENOTSUP;
#endif
    }

    int setAffinity(const CpuAffinityMask& mask) noexcept override
    {
#if defined(__ANDROID__) || defined(__linux__)
        if (mask.none())
            return EINVAL;
        cpu_set_t native;
        CPU_ZERO(&native);
        for (std::size_t cpu = 0; cpu < mask.size(); ++cpu)
        {
            if (!mask.test(cpu))
                continue;
            if (cpu >= CPU_SETSIZE)
                return EINVAL;
            CPU_SET(cpu, &native);
        }
        return sched_setaffinity(0, sizeof(native), &native) == 0 ? 0 : errno;
#else
        (void)mask;
        return ENOTSUP;
#endif
    }

    int getCapacity(std::size_t cpu, std::uint32_t& capacity) noexcept override
    {
        char path[96];
        std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%zu/cpu_capacity", cpu);
        FILE* file = std::fopen(path, "r");
        if (file == nullptr)
            return errno;
        unsigned long value = 0;
        char trailing = 0;
        const int count = std::fscanf(file, "%lu %c", &value, &trailing);
        const int readError = std::ferror(file) ? EIO : 0;
        std::fclose(file);
        if (readError != 0)
            return readError;
        if (count != 1 || value == 0 || value > std::numeric_limits<std::uint32_t>::max())
            return EINVAL;
        capacity = static_cast<std::uint32_t>(value);
        return 0;
    }

    void record(const char* operation, int error,
        const CpuAffinityMask& requested, const CpuAffinityMask& actual) noexcept override
    {
#ifdef __ANDROID__
        const auto requestedText = formatMask(requested);
        const auto actualText = formatMask(actual);
        __android_log_print(error == 0 ? ANDROID_LOG_INFO : ANDROID_LOG_WARN,
            "CpuPlacement", "operation=%s error=%d requested=%s actual=%s",
            operation, error, requestedText.data(), actualText.data());
#else
        (void)operation;
        (void)error;
        (void)requested;
        (void)actual;
#endif
    }

private:
    static std::array<char, 257> formatMask(const CpuAffinityMask& mask) noexcept
    {
        std::array<char, 257> text{};
        std::size_t offset = 0;
        bool started = false;
        for (std::size_t nibble = mask.size() / 4; nibble-- > 0;)
        {
            unsigned value = 0;
            for (unsigned bit = 0; bit < 4; ++bit)
                value |= static_cast<unsigned>(mask.test(nibble * 4 + bit)) << bit;
            if (!started && value == 0 && nibble != 0)
                continue;
            started = true;
            text[offset++] = "0123456789abcdef"[value];
        }
        return text;
    }
};

SystemCpuAffinityBackend systemBackend;
}

EmulationThreadCpuPolicy::EmulationThreadCpuPolicy() noexcept
    : EmulationThreadCpuPolicy(systemBackend)
{
}

EmulationThreadCpuPolicy::EmulationThreadCpuPolicy(CpuAffinityBackend& backend) noexcept
    : backend(backend), previousPolicy(currentPolicy)
{
    currentPolicy = this;
}

EmulationThreadCpuPolicy::~EmulationThreadCpuPolicy()
{
    restore();
    currentPolicy = previousPolicy;
}

void EmulationThreadCpuPolicy::setActive(bool active) noexcept
{
    if (wanted == active)
        return;
    wanted = active;
    if (!active)
    {
        eligible = false;
        failed = !restore();
        return;
    }
    if (failed)
        return;

    int error = backend.getAffinity(baseline);
    if (error != 0 || baseline.none())
    {
        failed = true;
        backend.record("baseline", error != 0 ? error : EINVAL, {}, baseline);
        return;
    }

    std::array<std::uint32_t, CpuAffinityMask{}.size()> capacities{};
    std::uint32_t minimum = std::numeric_limits<std::uint32_t>::max();
    for (std::size_t cpu = 0; cpu < baseline.size(); ++cpu)
    {
        if (!baseline.test(cpu))
            continue;
        error = backend.getCapacity(cpu, capacities[cpu]);
        if (error != 0 || capacities[cpu] == 0)
        {
            failed = true;
            backend.record("capacity", error != 0 ? error : EINVAL, baseline, {});
            return;
        }
        minimum = std::min(minimum, capacities[cpu]);
    }

    selected.reset();
    for (std::size_t cpu = 0; cpu < baseline.size(); ++cpu)
        if (baseline.test(cpu) && capacities[cpu] > minimum)
            selected.set(cpu);
    eligible = selected.any();
    if (eligible && creationDepth == 0)
        apply();
    else if (!eligible)
        backend.record("uniform", 0, baseline, baseline);
}

void EmulationThreadCpuPolicy::apply() noexcept
{
    int error = backend.setAffinity(selected);
    CpuAffinityMask actual;
    if (error == 0)
    {
        changed = true;
        error = backend.getAffinity(actual);
        if (error == 0 && (actual.none() || (actual & ~selected).any()))
            error = EINVAL;
    }
    backend.record("apply", error, selected, actual);
    if (error != 0)
    {
        failed = true;
        eligible = false;
        restore();
    }
}

bool EmulationThreadCpuPolicy::restore() noexcept
{
    if (!changed)
        return true;
    int error = backend.setAffinity(baseline);
    CpuAffinityMask actual;
    if (error == 0)
    {
        error = backend.getAffinity(actual);
        if (error == 0 && (actual.none() || (actual & ~baseline).any()))
            error = EINVAL;
    }
    backend.record("restore", error, baseline, actual);
    if (error == 0)
        changed = false;
    return error == 0;
}

void EmulationThreadCpuPolicy::beginThreadCreation() noexcept
{
    if (creationDepth++ == 0 && !restore())
    {
        failed = true;
        eligible = false;
    }
}

void EmulationThreadCpuPolicy::endThreadCreation() noexcept
{
    if (creationDepth == 0)
        return;
    if (--creationDepth == 0 && wanted && eligible && !failed)
        apply();
}

void setEmulationThreadCreationActive(bool active) noexcept
{
    if (currentPolicy == nullptr)
        return;
    if (active)
        currentPolicy->beginThreadCreation();
    else
        currentPolicy->endThreadCreation();
}
}
