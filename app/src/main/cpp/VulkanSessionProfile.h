#ifndef VULKAN_SESSION_PROFILE_H
#define VULKAN_SESSION_PROFILE_H

#include "VulkanPipelineProfile.h"

namespace MelonDSAndroid
{

class VulkanSessionProfile final
{
public:
    constexpr VulkanSessionProfile(
        bool startedWithVulkan,
        melonDS::VulkanPipelineProfile requestedProfile) noexcept
        : sessionUsesVulkanStrategy(startedWithVulkan),
          effectiveProfile(requestedProfile)
    {
    }

    [[nodiscard]] constexpr melonDS::VulkanPipelineProfile get() const noexcept
    {
        return effectiveProfile;
    }

    [[nodiscard]] constexpr bool usesVulkanStrategy() const noexcept
    {
        return sessionUsesVulkanStrategy;
    }

    [[nodiscard]] constexpr bool normalize(
        bool requestedUsesVulkanStrategy,
        melonDS::VulkanPipelineProfile* requestedProfile) const noexcept
    {
        if (!sessionUsesVulkanStrategy
            || !requestedUsesVulkanStrategy
            || requestedProfile == nullptr
            || *requestedProfile == effectiveProfile)
        {
            return false;
        }

        *requestedProfile = effectiveProfile;
        return true;
    }

private:
    bool sessionUsesVulkanStrategy;
    melonDS::VulkanPipelineProfile effectiveProfile;
};

}

#endif
