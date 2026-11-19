#ifndef ROMGBASLOTCONFIG_H
#define ROMGBASLOTCONFIG_H

#include <string>

namespace MelonDSAndroid
{
    enum RomGbaSlotConfigType {
        NONE = 0,
        GBA_ROM = 1,
        RUMBLE_PAK = 2,
        MEMORY_EXPANSION = 3,
        ANALOG_INPUT = 4,
        MOTION_PAK_HOMEBREW = 5,
        MOTION_PAK_RETAIL = 6,
    } ;

    struct RomGbaSlotConfig {
        RomGbaSlotConfigType type;
    };

    struct RomGbaSlotConfigNone {
        RomGbaSlotConfig _base = { .type = RomGbaSlotConfigType::NONE };
    };

    struct RomGbaSlotConfigGbaRom {
        RomGbaSlotConfig _base = { .type = RomGbaSlotConfigType::GBA_ROM };
        std::string romPath;
        std::string savePath;
    };

    struct RomGbaSlotRumblePak {
        RomGbaSlotConfig _base = { .type = RomGbaSlotConfigType::RUMBLE_PAK };
    };

    struct RomGbaSlotConfigMemoryExpansion {
        RomGbaSlotConfig _base = { .type = RomGbaSlotConfigType::MEMORY_EXPANSION };
    };

    struct RomGbaSlotConfigAnalogInput {
        RomGbaSlotConfig _base = { .type = RomGbaSlotConfigType::ANALOG_INPUT };
    };
}

#endif //ROMGBASLOTCONFIG_H
