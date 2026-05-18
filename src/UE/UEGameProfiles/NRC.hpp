#pragma once

#include "../UEGameProfile.hpp"
using namespace UEMemory;

class NRCProfile : public IGameProfile
{
public:
    NRCProfile() = default;

    bool ArchSupprted() const override
    {
        auto e_machine = GetUnrealELF().header().e_machine;
        // only arm64
        return e_machine == EM_AARCH64;
    }

    std::string GetAppName() const override
    {
        return "洛克王国世界";
    }

    std::vector<std::string> GetAppIDs() const override
    {
        return {"com.tencent.nrc"};
    }

    bool isUsingCasePreservingName() const override
    {
        return false;
    }

    bool IsUsingFNamePool() const override
    {
        return true;
    }

    bool isUsingOutlineNumberName() const override
    {
        return false;
    }

    uintptr_t GetGUObjectArrayPtr() const override
    {
        std::vector<std::pair<std::string, int>> idaPatterns = {
            {"B4 21 0C 40 B9 ? ? ? ? ? ? ? 91", 5},
            {"9F E5 00 ? 00 E3 FF ? 40 E3 ? ? A0 E1", -2},
            {"96 df 02 17 ? ? ? ? 54 ? ? ? ? ? ? ? 91 e1 03 13 aa", 9},
            {"f4 03 01 2a ? 00 00 34 ? ? ? ? ? ? ? ? ? ? 00 54 ? 00 00 14 ? ? ? ? ? ? ? 91", 0x18},
            {"69 3e 40 b9 1f 01 09 6b ? ? ? 54 e1 03 13 aa ? ? ? ? f4 4f ? a9 ? ? ? ? ? ? ? 91", 0x18},
        };

        PATTERN_MAP_TYPE map_type = isEmulator() ? PATTERN_MAP_TYPE::ANY_R : PATTERN_MAP_TYPE::ANY_X;

        for (const auto &it : idaPatterns)
        {
            std::string ida_pattern = it.first;
            const int step = it.second;

            uintptr_t adrl = Arm64::Decode_ADRP_ADD(findIdaPattern(map_type, ida_pattern, step));
            if (adrl != 0) return adrl;
        }

        return 0;
    }
    uintptr_t GetFrameCount() const override
    {
        PATTERN_MAP_TYPE map_type = isEmulator() ? PATTERN_MAP_TYPE::ANY_R : PATTERN_MAP_TYPE::ANY_X;

        std::string ida_pattern = "C0 03 5F D6 ? ? ? ? 08 ? ? ? 00 01 40 F9 C0 03 5F D6 FD 7B BF A9";
        const int step = 4;
        auto FrameOff = Arm64::Decode_ADRP_LDR(findIdaPattern(map_type, ida_pattern, step));
        if (FrameOff !=0)
            return vm_rpm_ptr<uintptr_t>((void*)FrameOff);
        return  0;
    }
    uintptr_t GetMatrix() const override
    {
        std::vector<std::pair<std::string, int>> idaPatterns = {
            {"08 3D 40 F9 00 01 3F D6 E8 03 13 AA ? ? ? F9", 0x18},
            {"FD ? ? A9 28 ? ? F9 F3 ? ? F8 C0 03 5F D6", -0x1c},
            {"00 01 3F D6 E8 03 13 AA 60 ? 00 F9 ? ? ? A9", 14},
        };

        PATTERN_MAP_TYPE map_type = isEmulator() ? PATTERN_MAP_TYPE::ANY_R : PATTERN_MAP_TYPE::ANY_X;

        for (const auto &it : idaPatterns)
        {
            std::string ida_pattern = it.first;
            const int step = it.second;

            uintptr_t adrl = Arm64::Decode_ADRP_LDR(findIdaPattern(map_type, ida_pattern, step));
            if (adrl != 0) return adrl;
        }
        return 0;
    }
    uintptr_t GetPhysx() const override
    {
        std::vector<std::pair<std::string, int>> idaPatterns = {
            {"E1 ? ? ? 40 00 40 BD F4 03 02 AA",0x28},
            {"48 ? ? ? F3 03 04 AA F5 03 03 2A", -0xc},
        };

        PATTERN_MAP_TYPE map_type = isEmulator() ? PATTERN_MAP_TYPE::ANY_R : PATTERN_MAP_TYPE::ANY_X;

        for (const auto &it : idaPatterns)
        {
            std::string ida_pattern = it.first;
            const int step = it.second;

            uintptr_t adrl = Arm64::Decode_ADRP_LDR(findIdaPattern(map_type, ida_pattern, step),8);
            //printf("%lx\n", vm_rpm_ptr<uintptr_t>((void*)adrl));
            if (adrl != 0) return vm_rpm_ptr<uintptr_t>((void*)adrl);
        }
        return 0;
    }
    uintptr_t GetNamesPtr() const override
    {
        std::vector<std::pair<std::string, int>> idaPatterns = {
                {"68 FE 50 ? 88 2E 28 ? 08 21 40 F9", -0x10},
        };

        PATTERN_MAP_TYPE map_type = isEmulator() ? PATTERN_MAP_TYPE::ANY_R : PATTERN_MAP_TYPE::ANY_X;

        for (const auto &it : idaPatterns)
        {
            std::string ida_pattern = it.first;
            const int step = it.second;

            uintptr_t adrl = Arm64::Decode_ADRP_ADD(findIdaPattern(map_type, ida_pattern, step),8);
            if (adrl != 0) return adrl;
        }

        return 0;
    }

    UE_Offsets *GetOffsets() const override
    {
        static UE_Offsets offsets = UE_DefaultOffsets::UE4_25_27(isUsingCasePreservingName());

        static bool once = false;
        if (!once)
        {
            once = true;



        }

        return &offsets;
    }
};
