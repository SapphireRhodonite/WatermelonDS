#pragma once

#include <bitset>
#include <cstddef>
#include <cstdint>

namespace MelonDSAndroid
{
using CpuAffinityMask = std::bitset<1024>;

class CpuAffinityBackend
{
public:
    virtual ~CpuAffinityBackend() = default;
    virtual int getAffinity(CpuAffinityMask& mask) noexcept = 0;
    virtual int setAffinity(const CpuAffinityMask& mask) noexcept = 0;
    virtual int getCapacity(std::size_t cpu, std::uint32_t& capacity) noexcept = 0;
    virtual void record(const char* operation, int error,
        const CpuAffinityMask& requested, const CpuAffinityMask& actual) noexcept = 0;
};

class EmulationThreadCpuPolicy
{
public:
    EmulationThreadCpuPolicy() noexcept;
    explicit EmulationThreadCpuPolicy(CpuAffinityBackend& backend) noexcept;
    ~EmulationThreadCpuPolicy();

    EmulationThreadCpuPolicy(const EmulationThreadCpuPolicy&) = delete;
    EmulationThreadCpuPolicy& operator=(const EmulationThreadCpuPolicy&) = delete;

    void setActive(bool active) noexcept;

private:
    friend void setEmulationThreadCreationActive(bool active) noexcept;
    bool restore() noexcept;
    void apply() noexcept;
    void beginThreadCreation() noexcept;
    void endThreadCreation() noexcept;

    CpuAffinityBackend& backend;
    EmulationThreadCpuPolicy* previousPolicy;
    CpuAffinityMask baseline;
    CpuAffinityMask selected;
    bool wanted = false;
    bool eligible = false;
    bool changed = false;
    bool failed = false;
    unsigned creationDepth = 0;
};

void setEmulationThreadCreationActive(bool active) noexcept;
}
