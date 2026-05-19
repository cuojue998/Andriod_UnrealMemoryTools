#include "AutoFixOffsets.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <initializer_list>
#include <string>

#include "PropertyOffsetFinder.hpp"
#include "StructLayout.hpp"
#include "../UE/UEMemory.hpp"
#include "../UE/UEGameProfile.hpp"
#include "../UE/UEWrappers.hpp"
#include "../Utils/Logger.hpp"

using namespace UEMemory;

namespace AutoFix
{
    namespace
    {
        constexpr uintptr_t kNameScanStart = 0x08;
        constexpr uintptr_t kNameScanEnd = 0x7C;
        constexpr int kStrictSampleCap = 0x40;
        constexpr int kLooseSampleCap = 0x400;

        UE_Offsets *Offsets() { return UEWrappers::GetOffsets(); }
        const UEVars *Vars() { return UEWrappers::GetUEVars(); }
        UE_UObjectArray *Objects() { return UEWrappers::GetObjects(); }

        uintptr_t BaseAddress()
        {
            return UEWrappers::GetBaseAddress();
        }

        uintptr_t ReadPtr(uintptr_t address)
        {
            return vm_rpm_ptr<uintptr_t>((const void *)address);
        }

        int32_t ReadI32(uintptr_t address)
        {
            return vm_rpm_ptr<int32_t>((const void *)address);
        }

        std::string ReadNameById(int32_t id)
        {
            if (id <= 0 || id > 0x800000)
                return "";
            return UEWrappers::GetNameByID(id);
        }

        std::string ReadNameAt(uintptr_t address)
        {
            return ReadNameById(ReadI32(address));
        }

        std::string ReadObjectName(uintptr_t object, uintptr_t nameOffset)
        {
            if (object == 0 || nameOffset == 0)
                return "";
            return ReadNameAt(object + nameOffset);
        }

        bool IsLikelyPtr(uintptr_t p)
        {
            return p >= 0x10000 && kPtrValidator.isPtrReadable((const void *)p);
        }

        bool IsExecutableInModule(uintptr_t p)
        {
            const uintptr_t base = BaseAddress();
            if (!base || p <= base)
                return false;

            const uintptr_t rva = p - base;
            return rva < 0x30000000 && kPtrValidator.isPtrExecutable((const void *)p);
        }

        bool StartsWith(const std::string &value, const char *prefix)
        {
            const std::string p = prefix ? prefix : "";
            return value.size() >= p.size() && value.compare(0, p.size(), p) == 0;
        }

        bool IsIdentifierLike(const std::string &name)
        {
            if (name.size() < 2 || name.size() > 128)
                return false;
            if (!std::isalpha(static_cast<unsigned char>(name[0])) && name[0] != '_')
                return false;

            for (unsigned char c : name)
            {
                if (!(std::isalnum(c) || c == '_'))
                    return false;
            }
            return name != "None" && name != "NULL";
        }

        bool IsUELikeName(const std::string &name)
        {
            if (name.empty())
                return false;
            if (StartsWith(name, "/Script/"))
                return true;
            if (name.find('.') != std::string::npos)
                return true;
            return IsIdentifierLike(name);
        }

        bool IsClassNameLike(const std::string &name)
        {
            if (name.size() < 2 || name.size() > 64)
                return false;
            const unsigned char first = static_cast<unsigned char>(name[0]);
            if (!std::isupper(first))
                return false;
            for (unsigned char c : name)
            {
                if (!(std::isalnum(c) || c == '_'))
                    return false;
            }
            return name != "None" && name != "NULL";
        }

        bool IsPropertyClassName(const std::string &name)
        {
            return !name.empty() && name.find("Property") != std::string::npos;
        }

        std::string ReadFFieldClassName(uintptr_t classPtr)
        {
            UE_Offsets *off = Offsets();
            if (!off || !IsLikelyPtr(classPtr))
                return "";

            const std::string nameAtZero = ReadNameAt(classPtr);
            if (IsPropertyClassName(nameAtZero))
                return nameAtZero;

            if (off->UObject.NamePrivate)
            {
                const std::string objectStyleName = ReadObjectName(classPtr, off->UObject.NamePrivate);
                if (IsPropertyClassName(objectStyleName))
                    return objectStyleName;
                if (IsClassNameLike(objectStyleName))
                    return objectStyleName;
            }

            if (IsClassNameLike(nameAtZero))
                return nameAtZero;

            return "";
        }

        bool MatchesAnyName(const std::string &value, std::initializer_list<const char *> candidates)
        {
            for (const char *candidate : candidates)
            {
                if (candidate && value == candidate)
                    return true;
            }
            return false;
        }

        bool IsKnownWorldPropertyName(const std::string &name)
        {
            return MatchesAnyName(name, {"PersistentLevel", "OwningGameInstance", "NetDriver", "DemoNetDriver"});
        }

        bool IsKnownWorldFunctionName(const std::string &name)
        {
            return MatchesAnyName(name, {"K2_GetWorldSettings", "UserConstructionScript", "ReceiveTick", "ExecuteUbergraph"});
        }

        uintptr_t FindNamedFFieldInOwner(const char *ownerFullName, std::initializer_list<const char *> fieldNames)
        {
            UE_Offsets *off = Offsets();
            if (!off || !Objects() || !off->UStruct.ChildProperties || !off->FField.NamePrivate || !off->FField.Next)
                return 0;

            const UE_UClass owner = Objects()->FindObject(ownerFullName).Cast<UE_UClass>();
            if (!owner)
                return 0;

            uintptr_t prop = ReadPtr(reinterpret_cast<uintptr_t>(owner.GetAddress()) + off->UStruct.ChildProperties);
            int depth = 0;
            while (IsLikelyPtr(prop) && depth < 256)
            {
                const std::string propName = ReadObjectName(prop, off->FField.NamePrivate);
                if (MatchesAnyName(propName, fieldNames))
                    return prop;
                prop = ReadPtr(prop + off->FField.Next);
                ++depth;
            }

            return 0;
        }

        uintptr_t FindPropertyLayoutAnchor()
        {
            if (const uintptr_t worldProp = FindNamedFFieldInOwner("Class Engine.World",
                                                                   {"PersistentLevel", "OwningGameInstance", "NetDriver", "DemoNetDriver"}))
            {
                return worldProp;
            }

            if (const uintptr_t engineProp = FindNamedFFieldInOwner("Class Engine.Engine",
                                                                    {"TinyFont", "SmallFont", "MediumFont", "LargeFont"}))
            {
                return engineProp;
            }

            return 0;
        }

        uintptr_t FindPropertySizeAnchor()
        {
            if (const uintptr_t engineProp = FindNamedFFieldInOwner("Class Engine.Engine",
                                                                    {"TinyFont", "SmallFont", "MediumFont", "LargeFont"}))
            {
                return engineProp;
            }

            return FindPropertyLayoutAnchor();
        }

        struct FPropertyLayoutScanResult
        {
            uintptr_t ArrayDim = 0;
            uintptr_t ElementSize = 0;
            uintptr_t PropertyFlags = 0;
            uintptr_t OffsetInternal = 0;
            uintptr_t Size = 0;
        };

        bool IsReasonablePropertyMemberOffset(int32_t value)
        {
            return value > 0 && value < 0x2000 && (value % 4) == 0;
        }

        bool MatchesExpectedPropertyClassName(const std::string &anchorFieldName, const std::string &targetObjectName)
        {
            if (anchorFieldName == "PersistentLevel")
                return targetObjectName == "Level";
            if (anchorFieldName == "OwningGameInstance")
                return targetObjectName == "GameInstance";
            if (anchorFieldName == "NetDriver" || anchorFieldName == "DemoNetDriver")
                return targetObjectName == "NetDriver";
            if (anchorFieldName == "TinyFont" || anchorFieldName == "SmallFont" ||
                anchorFieldName == "MediumFont" || anchorFieldName == "LargeFont")
            {
                return targetObjectName == "Font";
            }
            return false;
        }

        bool ProbeFPropertySizeFromAnchor(uintptr_t prop, uintptr_t &outSize)
        {
            UE_Offsets *off = Offsets();
            if (!prop || !off || !off->FField.NamePrivate || !off->UObject.NamePrivate)
                return false;

            const std::string anchorName = ReadObjectName(prop, off->FField.NamePrivate);
            if (anchorName.empty())
                return false;

            for (uintptr_t sizeOff = 0x40; sizeOff <= 0xA0; sizeOff += sizeof(int32_t))
            {
                const uintptr_t candidate = ReadPtr(prop + sizeOff);
                if (!IsLikelyPtr(candidate))
                    continue;

                const std::string targetName = ReadObjectName(candidate, off->UObject.NamePrivate);
                if (!MatchesExpectedPropertyClassName(anchorName, targetName))
                    continue;

                outSize = sizeOff;
                return true;
            }

            return false;
        }

        bool TryResolveFPropertyLayout(uintptr_t prop,
                                       uintptr_t arrayDimOff,
                                       uintptr_t sizeOff,
                                       FPropertyLayoutScanResult &out)
        {
            if (!prop || !arrayDimOff)
                return false;

            const int32_t arrayDim = ReadI32(prop + arrayDimOff);
            const int32_t elemSize = ReadI32(prop + arrayDimOff + sizeof(int32_t));
            if (arrayDim != 1 || (elemSize != 4 && elemSize != 8))
                return false;

            for (uintptr_t offDelta : {static_cast<uintptr_t>(0x14), static_cast<uintptr_t>(0x18)})
            {
                const uintptr_t offsetInternalOff = arrayDimOff + offDelta;
                const int32_t offsetInternal = ReadI32(prop + offsetInternalOff);
                if (!IsReasonablePropertyMemberOffset(offsetInternal))
                    continue;

                out.ArrayDim = arrayDimOff;
                out.ElementSize = arrayDimOff + sizeof(int32_t);
                out.PropertyFlags = arrayDimOff + 0x8;
                out.OffsetInternal = offsetInternalOff;
                out.Size = sizeOff;
                return true;
            }

            return false;
        }

        bool ProbeFPropertyLayoutFromAnchor(uintptr_t prop,
                                            uintptr_t knownSize,
                                            FPropertyLayoutScanResult &out)
        {
            if (!prop)
                return false;

            for (uintptr_t arrayDimOff = 0x2C; arrayDimOff <= 0x48; arrayDimOff += sizeof(int32_t))
            {
                if (TryResolveFPropertyLayout(prop, arrayDimOff, knownSize, out))
                    return true;
            }

            if (knownSize)
            {
                for (uintptr_t delta : {static_cast<uintptr_t>(0x40), static_cast<uintptr_t>(0x44)})
                {
                    if (knownSize <= delta)
                        continue;

                    if (TryResolveFPropertyLayout(prop, knownSize - delta, knownSize, out))
                        return true;
                }
            }

            return false;
        }

        void ApplyDefaultsOnly()
        {
            UE_Offsets *off = Offsets();
            if (!off)
                return;

            if (off->FName.Size == 0)
            {
                off->FName.Size = UE_DefaultOffsets::kGetFNameSize(off->Config.isUsingCasePreservingName,
                                                                   off->Config.isUsingOutlineNumberName);
            }

            if (off->FUObjectArray.ObjObjects == 0)
                off->FUObjectArray.ObjObjects = sizeof(int32_t) * 4;
            if (off->TUObjectArray.Objects == 0)
                off->TUObjectArray.Objects = 0;
            if (off->TUObjectArray.NumElements == 0)
            {
                off->TUObjectArray.NumElements = (off->TUObjectArray.NumElementsPerChunk > 0)
                    ? ((sizeof(void *) * 2) + sizeof(int32_t))
                    : (sizeof(void *) + sizeof(int32_t));
            }
            if (off->FUObjectItem.Object == 0)
                off->FUObjectItem.Object = 0;
            if (off->FUObjectItem.Size == 0)
                off->FUObjectItem.Size = GetPtrAlignedOf(sizeof(void *) + (sizeof(int32_t) * 3));
        }

        void PrintField(const char *label, uintptr_t before, uintptr_t after)
        {
            if (before == after)
                LOGI("[AutoFix]   %-32s = 0x%lx", label, (unsigned long)after);
            else
                LOGI("[AutoFix]   %-32s = 0x%lx  (was 0x%lx)", label, (unsigned long)after, (unsigned long)before);
        }

        void DumpAutoFoundOffsets(const UE_Offsets &before, const UE_Offsets &after)
        {
            LOGI("[AutoFix] ============== Auto-Found Offsets ==============");
            PrintField("FUObjectArray.ObjObjects", before.FUObjectArray.ObjObjects, after.FUObjectArray.ObjObjects);
            PrintField("TUObjectArray.Objects", before.TUObjectArray.Objects, after.TUObjectArray.Objects);
            PrintField("TUObjectArray.NumElements", before.TUObjectArray.NumElements, after.TUObjectArray.NumElements);
            PrintField("TUObjectArray.NumElementsPerChunk", before.TUObjectArray.NumElementsPerChunk, after.TUObjectArray.NumElementsPerChunk);
            PrintField("FUObjectItem.Object", before.FUObjectItem.Object, after.FUObjectItem.Object);
            PrintField("FUObjectItem.Size", before.FUObjectItem.Size, after.FUObjectItem.Size);
            PrintField("UObject.ObjectFlags", before.UObject.ObjectFlags, after.UObject.ObjectFlags);
            PrintField("UObject.InternalIndex", before.UObject.InternalIndex, after.UObject.InternalIndex);
            PrintField("UObject.ClassPrivate", before.UObject.ClassPrivate, after.UObject.ClassPrivate);
            PrintField("UObject.NamePrivate", before.UObject.NamePrivate, after.UObject.NamePrivate);
            PrintField("UObject.OuterPrivate", before.UObject.OuterPrivate, after.UObject.OuterPrivate);
            PrintField("UField.Next", before.UField.Next, after.UField.Next);
            PrintField("UEnum.Names", before.UEnum.Names, after.UEnum.Names);
            PrintField("UStruct.SuperStruct", before.UStruct.SuperStruct, after.UStruct.SuperStruct);
            PrintField("UStruct.Children", before.UStruct.Children, after.UStruct.Children);
            PrintField("UStruct.ChildProperties", before.UStruct.ChildProperties, after.UStruct.ChildProperties);
            PrintField("UStruct.PropertiesSize", before.UStruct.PropertiesSize, after.UStruct.PropertiesSize);
            PrintField("UFunction.EFunctionFlags", before.UFunction.EFunctionFlags, after.UFunction.EFunctionFlags);
            PrintField("UFunction.NumParams", before.UFunction.NumParams, after.UFunction.NumParams);
            PrintField("UFunction.ParamSize", before.UFunction.ParamSize, after.UFunction.ParamSize);
            PrintField("UFunction.Func", before.UFunction.Func, after.UFunction.Func);
            PrintField("UProperty.ArrayDim", before.UProperty.ArrayDim, after.UProperty.ArrayDim);
            PrintField("UProperty.ElementSize", before.UProperty.ElementSize, after.UProperty.ElementSize);
            PrintField("UProperty.PropertyFlags", before.UProperty.PropertyFlags, after.UProperty.PropertyFlags);
            PrintField("UProperty.Offset_Internal", before.UProperty.Offset_Internal, after.UProperty.Offset_Internal);
            PrintField("UProperty.Size", before.UProperty.Size, after.UProperty.Size);
            PrintField("FField.ClassPrivate", before.FField.ClassPrivate, after.FField.ClassPrivate);
            PrintField("FField.Next", before.FField.Next, after.FField.Next);
            PrintField("FField.NamePrivate", before.FField.NamePrivate, after.FField.NamePrivate);
            PrintField("FField.FlagsPrivate", before.FField.FlagsPrivate, after.FField.FlagsPrivate);
            PrintField("FProperty.ArrayDim", before.FProperty.ArrayDim, after.FProperty.ArrayDim);
            PrintField("FProperty.ElementSize", before.FProperty.ElementSize, after.FProperty.ElementSize);
            PrintField("FProperty.PropertyFlags", before.FProperty.PropertyFlags, after.FProperty.PropertyFlags);
            PrintField("FProperty.Offset_Internal", before.FProperty.Offset_Internal, after.FProperty.Offset_Internal);
            PrintField("FProperty.Size", before.FProperty.Size, after.FProperty.Size);
            LOGI("[AutoFix] ================================================");
        }

        void DumpObjectNameCandidates(int32_t idx, uintptr_t object)
        {
            const int32_t id18 = ReadI32(object + 0x18);
            const int32_t id20 = ReadI32(object + 0x20);
            const int32_t id28 = ReadI32(object + 0x28);

            const std::string n18 = ReadNameById(id18);
            const std::string n20 = ReadNameById(id20);
            const std::string n28 = ReadNameById(id28);

            LOGW("[AutoFix] Dump idx=%d obj=%p id@0x18=%d('%s') id@0x20=%d('%s') id@0x28=%d('%s')",
                 idx,
                 (void *)object,
                 id18,
                 n18.c_str(),
                 id20,
                 n20.c_str(),
                 id28,
                 n28.c_str());
        }

        bool ValidateUObjectLayout(uintptr_t nameOffset, int sampleCap, int *decodedOut = nullptr, int *totalOut = nullptr, int *classHitsOut = nullptr, int *scriptAnchorsOut = nullptr)
        {
            if (!Objects() || nameOffset == 0)
                return false;

            UE_Offsets *off = Offsets();
            if (!off)
                return false;

            const int32_t total = Objects()->GetNumElements();
            const int32_t limit = std::min(total, sampleCap);

            int decoded = 0;
            int scriptAnchors = 0;
            int classHits = 0;

            for (int32_t i = 0; i < limit; ++i)
            {
                const uintptr_t object = reinterpret_cast<uintptr_t>(Objects()->GetObjectPtr(i));
                if (!IsLikelyPtr(object))
                    continue;

                const std::string name = ReadObjectName(object, nameOffset);
                if (name.empty())
                    continue;

                if (IsUELikeName(name))
                    ++decoded;
                if (StartsWith(name, "/Script/"))
                    ++scriptAnchors;

                if (off->UObject.ClassPrivate)
                {
                    const uintptr_t classPtr = ReadPtr(object + off->UObject.ClassPrivate);
                    if (IsLikelyPtr(classPtr))
                    {
                        const std::string className = ReadObjectName(classPtr, nameOffset);
                        if (IsClassNameLike(className) || IsPropertyClassName(className))
                            ++classHits;
                    }
                }
            }

            if (decodedOut)
                *decodedOut = decoded;
            if (totalOut)
                *totalOut = limit;
            if (classHitsOut)
                *classHitsOut = classHits;
            if (scriptAnchorsOut)
                *scriptAnchorsOut = scriptAnchors;

            if (scriptAnchors >= 16)
                return true;

            if (!off->Config.IsUsingFNamePool && decoded >= 24 && classHits >= 24)
                return true;

            return false;
        }

        bool FixupNamePrivate()
        {
            UE_Offsets *off = Offsets();
            if (!off || !Objects())
                return false;

            const uintptr_t before = off->UObject.NamePrivate;
            if (before)
            {
                int decoded = 0;
                int total = 0;
                int classHits = 0;
                int scriptAnchors = 0;
                if (ValidateUObjectLayout(before, kLooseSampleCap, &decoded, &total, &classHits, &scriptAnchors))
                {
                    LOGI("[AutoFix] UObject.NamePrivate=0x%lx validated (decoded=%d classHits=%d scriptAnchors=%d)",
                         (unsigned long)before,
                         decoded,
                         classHits,
                         scriptAnchors);
                    return true;
                }

                LOGW("[AutoFix] Preset NamePrivate=0x%lx not validated (decoded=%d classHits=%d scriptAnchors=%d)",
                     (unsigned long)before,
                     decoded,
                     classHits,
                     scriptAnchors);
            }

            const int32_t total = Objects()->GetNumElements();
            const int32_t strictLimit = std::min(total, kStrictSampleCap);

            for (int32_t i = 0; i < strictLimit; ++i)
            {
                const uintptr_t object = reinterpret_cast<uintptr_t>(Objects()->GetObjectPtr(i));
                if (!IsLikelyPtr(object))
                    continue;

                for (uintptr_t candidate = kNameScanStart; candidate <= kNameScanEnd; candidate += 0x4)
                {
                    if (ReadObjectName(object, candidate) == "/Script/CoreUObject")
                    {
                        LOGI("[AutoFix] BruteForce NamePrivate=0x%lx (was 0x%lx) anchor='/Script/CoreUObject' @ idx=%d",
                             (unsigned long)candidate,
                             (unsigned long)before,
                             i);
                        off->UObject.NamePrivate = candidate;
                        return true;
                    }
                }
            }

            uintptr_t bestCandidate = 0;
            int bestScore = -1;
            int bestIndex = -1;
            std::string bestAnchor;

            const int32_t looseLimit = std::min(total, kLooseSampleCap);
            for (int32_t i = 0; i < looseLimit; ++i)
            {
                const uintptr_t object = reinterpret_cast<uintptr_t>(Objects()->GetObjectPtr(i));
                if (!IsLikelyPtr(object))
                    continue;

                for (uintptr_t candidate = kNameScanStart; candidate <= kNameScanEnd; candidate += 0x4)
                {
                    const std::string name = ReadObjectName(object, candidate);
                    if (!StartsWith(name, "/Script/"))
                        continue;

                    int decoded = 0;
                    int totalChecked = 0;
                    int classHits = 0;
                    int scriptAnchors = 0;
                    if (!ValidateUObjectLayout(candidate, kLooseSampleCap, &decoded, &totalChecked, &classHits, &scriptAnchors))
                        continue;

                    const int score = decoded + (classHits * 3) + (scriptAnchors * 8);
                    if (score > bestScore)
                    {
                        bestScore = score;
                        bestCandidate = candidate;
                        bestIndex = i;
                        bestAnchor = name;
                    }
                }
            }

            if (bestCandidate)
            {
                LOGI("[AutoFix] Rescanned NamePrivate=0x%lx (was 0x%lx) anchor='%s' @ idx=%d",
                     (unsigned long)bestCandidate,
                     (unsigned long)before,
                     bestAnchor.c_str(),
                     bestIndex);
                off->UObject.NamePrivate = bestCandidate;
                return true;
            }

            const uintptr_t dumpObject = reinterpret_cast<uintptr_t>(Objects()->GetObjectPtr(0));
            if (IsLikelyPtr(dumpObject))
                DumpObjectNameCandidates(0, dumpObject);

            LOGE("[AutoFix] UObject.NamePrivate fixup failed; keeping preset");
            return false;
        }

        bool FollowSuperChain(uintptr_t start, uintptr_t superOffset, uintptr_t nameOffset, std::string *endName, int *depthOut)
        {
            if (!start || !superOffset || !nameOffset)
                return false;

            uintptr_t current = start;
            int depth = 0;
            std::string lastName;

            while (current && depth < 32)
            {
                current = ReadPtr(current + superOffset);
                if (!IsLikelyPtr(current))
                    break;

                lastName = ReadObjectName(current, nameOffset);
                if (lastName == "Object")
                {
                    if (endName)
                        *endName = lastName;
                    if (depthOut)
                        *depthOut = depth + 1;
                    return true;
                }
                ++depth;
            }

            if (endName)
                *endName = lastName;
            if (depthOut)
                *depthOut = depth;
            return false;
        }

        bool FixupSuperStruct()
        {
            UE_Offsets *off = Offsets();
            if (!off || off->UObject.NamePrivate == 0 || !Objects())
                return false;

            const uintptr_t before = off->UStruct.SuperStruct;
            const UE_UClass worldClass = Objects()->FindObject("Class Engine.World").Cast<UE_UClass>();
            if (!worldClass)
            {
                LOGW("[AutoFix] UStruct anchor 'World' class not found");
                return false;
            }

            const uintptr_t worldAddr = reinterpret_cast<uintptr_t>(worldClass.GetAddress());
            std::string endName;
            int depth = 0;

            if (before && FollowSuperChain(worldAddr, before, off->UObject.NamePrivate, &endName, &depth))
            {
                LOGI("[AutoFix] UStruct.SuperStruct=0x%lx validated (chain reached '%s' depth=%d)",
                     (unsigned long)before,
                     endName.c_str(),
                     depth);
                return true;
            }

            if (before)
            {
                LOGW("[AutoFix] UStruct.SuperStruct=0x%lx: chain didn't reach Object", (unsigned long)before);
            }

            for (uintptr_t candidate = 0x0; candidate <= 0x100; candidate += 0x4)
            {
                if (!FollowSuperChain(worldAddr, candidate, off->UObject.NamePrivate, &endName, &depth))
                    continue;

                LOGI("[AutoFix] Rescanned UStruct.SuperStruct=0x%lx (was 0x%lx) chain end='%s'",
                     (unsigned long)candidate,
                     (unsigned long)before,
                     endName.c_str());
                off->UStruct.SuperStruct = candidate;
                return true;
            }

            LOGW("[AutoFix] UStruct.SuperStruct fixup failed; keeping preset");
            return false;
        }

        bool FixupUFunctionFunc()
        {
            UE_Offsets *off = Offsets();
            if (!off || off->UObject.NamePrivate == 0 || !Objects())
                return false;

            const uintptr_t before = off->UFunction.Func;
            const UE_UClass functionClass = Objects()->FindObject("Class CoreUObject.Function").Cast<UE_UClass>();
            if (!functionClass)
            {
                LOGW("[AutoFix] UFunction.Func fixup failed; keeping preset");
                return false;
            }

            auto validateOffset = [&](uintptr_t candidate, bool logOk) -> bool
            {
                bool ok = false;
                Objects()->ForEachObjectOfClass(functionClass, [&](UE_UObject object)
                {
                    const uintptr_t address = reinterpret_cast<uintptr_t>(object.GetAddress());
                    const uintptr_t func = ReadPtr(address + candidate);
                    if (!IsExecutableInModule(func))
                        return false;

                    if (logOk)
                    {
                        LOGI("[AutoFix] UFunction.Func=0x%lx validated ('%s'.Func=%p)",
                             (unsigned long)candidate,
                             object.GetName().c_str(),
                             (void *)func);
                    }
                    ok = true;
                    return true;
                });
                return ok;
            };

            if (before && validateOffset(before, true))
                return true;

            int probed = 0;
            for (uintptr_t candidate = 0x80; candidate <= 0x140; candidate += 0x8)
            {
                ++probed;
                if (!validateOffset(candidate, false))
                    continue;

                std::string ownerName;
                Objects()->ForEachObjectOfClass(functionClass, [&](UE_UObject object)
                {
                    const uintptr_t address = reinterpret_cast<uintptr_t>(object.GetAddress());
                    const uintptr_t func = ReadPtr(address + candidate);
                    if (!IsExecutableInModule(func))
                        return false;
                    ownerName = object.GetName();
                    return true;
                });

                LOGI("[AutoFix] Rescanned UFunction.Func=0x%lx (was 0x%lx) on '%s'",
                     (unsigned long)candidate,
                     (unsigned long)before,
                     ownerName.c_str());
                off->UFunction.Func = candidate;
                return true;
            }

            LOGW("[AutoFix] UFunction.Func=0x%lx: no probed function had executable Func (probed=%d)",
                 (unsigned long)before,
                 probed);
            LOGW("[AutoFix] UFunction.Func fixup failed; keeping preset");
            return false;
        }

        // ----- UObject.ClassPrivate fixup ------------------------------------------------
        // 验证：ClassPrivate 指向的对象自身是 "Class" 类（自指环），并且其 NamePrivate
        // 解出的名字以大写字母开头。沿用 dumper-7 中通过 "Class" 锚定 UObjectToClass 的
        // 思路（见 main.cpp DumpOffset_ 的 Engine 入口），但用更稳健的双重链路验证。
        bool ValidateClassPrivate(uintptr_t candidate, int sampleCap)
        {
            if (!candidate || !Objects())
                return false;
            UE_Offsets *off = Offsets();
            const uintptr_t nameOff = off->UObject.NamePrivate;
            if (!nameOff)
                return false;

            const int32_t total = Objects()->GetNumElements();
            const int32_t limit = std::min(total, sampleCap);

            int hits = 0;
            int strictHits = 0;
            for (int32_t i = 0; i < limit; ++i)
            {
                const uintptr_t object = reinterpret_cast<uintptr_t>(Objects()->GetObjectPtr(i));
                if (!IsLikelyPtr(object))
                    continue;
                const uintptr_t classPtr = ReadPtr(object + candidate);
                if (!IsLikelyPtr(classPtr) || classPtr == object)
                    continue;
                const std::string className = ReadObjectName(classPtr, nameOff);
                if (!IsClassNameLike(className) && !IsPropertyClassName(className))
                    continue;

                const uintptr_t metaClass = ReadPtr(classPtr + candidate);
                if (IsLikelyPtr(metaClass))
                {
                    const std::string metaClassName = ReadObjectName(metaClass, nameOff);
                    if (metaClassName == "Class")
                        ++strictHits;
                }
                ++hits;
            }

            if (strictHits >= 4)
                return true;

            if (off->Config.IsUsingFNamePool)
                return hits >= std::max(8, limit / 4);

            return hits >= 8;
        }

        bool FixupClassPrivate()
        {
            UE_Offsets *off = Offsets();
            if (!off || off->UObject.NamePrivate == 0 || !Objects())
                return false;

            const uintptr_t before = off->UObject.ClassPrivate;
            if (before && ValidateClassPrivate(before, kStrictSampleCap))
            {
                LOGI("[AutoFix] UObject.ClassPrivate=0x%lx validated", (unsigned long)before);
                return true;
            }

            for (uintptr_t candidate = 0x8; candidate <= 0x40; candidate += 0x4)
            {
                if (candidate == off->UObject.NamePrivate)
                    continue;
                if (!ValidateClassPrivate(candidate, kStrictSampleCap))
                    continue;
                LOGI("[AutoFix] Rescanned UObject.ClassPrivate=0x%lx (was 0x%lx)",
                     (unsigned long)candidate, (unsigned long)before);
                off->UObject.ClassPrivate = candidate;
                return true;
            }

            LOGI("[AutoFix] UObject.ClassPrivate not confirmed by scan; keeping preset");
            return false;
        }

        // ----- UObject.OuterPrivate fixup ------------------------------------------------
        // 验证：跟随 OuterPrivate 直到 nullptr，链尾对象的名字应当是包名形式 "/Script/*"
        // 或全局根。dumper-7 中通过 "/Script/Engine" 锚定 UObjectToOuter，这里改为对样本
        // 集做"链路终点 /Script/* 命中数"统计。
        bool ValidateOuterPrivate(uintptr_t candidate, int sampleCap)
        {
            if (!candidate || !Objects())
                return false;
            UE_Offsets *off = Offsets();
            const uintptr_t nameOff = off->UObject.NamePrivate;
            if (!nameOff)
                return false;

            const int32_t total = Objects()->GetNumElements();
            const int32_t limit = std::min(total, sampleCap);

            int packageHits = 0;
            int readableHits = 0;
            for (int32_t i = 0; i < limit; ++i)
            {
                uintptr_t cur = reinterpret_cast<uintptr_t>(Objects()->GetObjectPtr(i));
                if (!IsLikelyPtr(cur))
                    continue;

                std::string lastName;
                int depth = 0;
                while (depth < 16)
                {
                    const uintptr_t outer = ReadPtr(cur + candidate);
                    if (!IsLikelyPtr(outer))
                        break;
                    ++readableHits;
                    cur = outer;
                    lastName = ReadObjectName(cur, nameOff);
                    ++depth;
                }
                if (StartsWith(lastName, "/Script/") || StartsWith(lastName, "/Game/"))
                    ++packageHits;
                else if (!off->Config.IsUsingFNamePool && (IsUELikeName(lastName) || IsClassNameLike(lastName)))
                    ++packageHits;
            }

            if (off->Config.IsUsingFNamePool)
                return packageHits >= std::max(4, limit / 4);

            return packageHits >= 4 || readableHits >= 16;
        }

        bool FixupOuterPrivate()
        {
            UE_Offsets *off = Offsets();
            if (!off || off->UObject.NamePrivate == 0 || !Objects())
                return false;

            const uintptr_t before = off->UObject.OuterPrivate;
            if (before && ValidateOuterPrivate(before, kStrictSampleCap))
            {
                LOGI("[AutoFix] UObject.OuterPrivate=0x%lx validated", (unsigned long)before);
                return true;
            }

            for (uintptr_t candidate = 0x10; candidate <= 0x50; candidate += 0x4)
            {
                if (candidate == off->UObject.NamePrivate || candidate == off->UObject.ClassPrivate)
                    continue;
                if (!ValidateOuterPrivate(candidate, kStrictSampleCap))
                    continue;
                LOGI("[AutoFix] Rescanned UObject.OuterPrivate=0x%lx (was 0x%lx)",
                     (unsigned long)candidate, (unsigned long)before);
                off->UObject.OuterPrivate = candidate;
                return true;
            }

            LOGI("[AutoFix] UObject.OuterPrivate not confirmed by scan; keeping preset");
            return false;
        }

        // ----- UStruct.Children fixup ----------------------------------------------------
        // dumper-7 对 Children 的锚点分两类：
        // 1. 老版本/无 FField：Children 链里能看到 PersistentLevel / NetDriver 这类 UProperty。
        // 2. 使用 FNamePool/ChildProperties：Children 更常指向 UFunction 链，典型名字是 K2_GetWorldSettings。
        // 因此这里不能只拿属性名验证，否则会把正确 preset 误判成失败。
        bool ValidateChildrenChain(uintptr_t candidate)
        {
            UE_Offsets *off = Offsets();
            if (!candidate || !off->UObject.NamePrivate || !Objects())
                return false;

            const UE_UClass worldClass = Objects()->FindObject("Class Engine.World").Cast<UE_UClass>();
            if (!worldClass)
                return false;

            const uintptr_t worldAddr = reinterpret_cast<uintptr_t>(worldClass.GetAddress());
            const uintptr_t nameOff = off->UObject.NamePrivate;
            const uintptr_t fieldNext = off->UField.Next ? off->UField.Next : (sizeof(void *) * 4);

            uintptr_t child = ReadPtr(worldAddr + candidate);
            int depth = 0;
            while (IsLikelyPtr(child) && depth < 64)
            {
                const std::string name = ReadObjectName(child, nameOff);
                if (IsKnownWorldPropertyName(name) || IsKnownWorldFunctionName(name))
                    return true;

                if (off->Config.IsUsingFNamePool)
                {
                    const uintptr_t classPtr = ReadPtr(child + off->UObject.ClassPrivate);
                    const std::string className = ReadObjectName(classPtr, nameOff);
                    if (className == "Function" && IsIdentifierLike(name))
                        return true;
                }

                child = ReadPtr(child + fieldNext);
                ++depth;
            }
            return false;
        }

        bool FixupChildren()
        {
            UE_Offsets *off = Offsets();
            if (!off || off->UObject.NamePrivate == 0 || off->UStruct.SuperStruct == 0 || !Objects())
                return false;

            const UE_UClass worldClass = Objects()->FindObject("Class Engine.World").Cast<UE_UClass>();
            const uintptr_t worldAddr = worldClass ? reinterpret_cast<uintptr_t>(worldClass.GetAddress()) : 0;

            const uintptr_t before = off->UStruct.Children;
            if (before && ValidateChildrenChain(before))
            {
                LOGI("[AutoFix] UStruct.Children=0x%lx validated", (unsigned long)before);
                return true;
            }

            const uintptr_t derived = off->UStruct.SuperStruct + sizeof(void *);
            if (ValidateChildrenChain(derived))
            {
                LOGI("[AutoFix] Derived UStruct.Children=0x%lx (was 0x%lx) from SuperStruct+ptr",
                     (unsigned long)derived, (unsigned long)before);
                off->UStruct.Children = derived;
                return true;
            }

            if (off->Config.IsUsingFNamePool && off->UStruct.ChildProperties && worldAddr)
            {
                const uintptr_t maybeChild = ReadPtr(worldAddr + derived);
                if (!before && IsLikelyPtr(maybeChild))
                {
                    LOGI("[AutoFix] Derived UStruct.Children=0x%lx from SuperStruct+ptr under FField layout",
                         (unsigned long)derived);
                    off->UStruct.Children = derived;
                    return true;
                }

                if (before == derived)
                {
                    LOGI("[AutoFix] UStruct.Children kept at 0x%lx under FField layout", (unsigned long)before);
                    return true;
                }
            }

            for (uintptr_t candidate = off->UStruct.SuperStruct + sizeof(void *) * 2;
                 candidate <= off->UStruct.SuperStruct + 0x40;
                 candidate += sizeof(void *))
            {
                if (!ValidateChildrenChain(candidate))
                    continue;
                LOGI("[AutoFix] Rescanned UStruct.Children=0x%lx (was 0x%lx)",
                     (unsigned long)candidate, (unsigned long)before);
                off->UStruct.Children = candidate;
                return true;
            }

            LOGI("[AutoFix] UStruct.Children not confirmed by scan; keeping preset");
            return false;
        }

        // ----- UStruct.ChildProperties fixup --------------------------------------------
        // 仅 UE >= 4.25 启用。锚点：World 类的 ChildProperties 链表中应当出现 FField 形式的
        // 子属性，且 FField 的 NamePrivate 解码出合法标识符（比如 "OwningGameInstance"）。
        // 同步派生 PropertiesSize = ChildProperties + sizeof(void*)（dumper-7 的兜底逻辑）。
        bool ValidateChildPropertiesChain(uintptr_t candidate)
        {
            UE_Offsets *off = Offsets();
            if (!candidate || !off->FField.NamePrivate || !Objects())
                return false;
            if (!off->FField.Next)
                return false;

            const UE_UClass worldClass = Objects()->FindObject("Class Engine.World").Cast<UE_UClass>();
            if (!worldClass)
                return false;

            const uintptr_t worldAddr = reinterpret_cast<uintptr_t>(worldClass.GetAddress());
            uintptr_t prop = ReadPtr(worldAddr + candidate);
            int knownNameHits = 0;
            int propertyClassHits = 0;
            int identifierHits = 0;
            int depth = 0;
            while (IsLikelyPtr(prop) && depth < 256)
            {
                const std::string name = ReadObjectName(prop, off->FField.NamePrivate);
                if (IsIdentifierLike(name))
                    ++identifierHits;
                if (IsKnownWorldPropertyName(name))
                    ++knownNameHits;
                if (off->FField.ClassPrivate)
                {
                    const std::string className = ReadFFieldClassName(ReadPtr(prop + off->FField.ClassPrivate));
                    if (IsPropertyClassName(className))
                        ++propertyClassHits;
                }
                if (knownNameHits >= 1 && (propertyClassHits >= 2 || identifierHits >= 3))
                    return true;
                prop = ReadPtr(prop + off->FField.Next);
                ++depth;
            }
            return knownNameHits >= 1 && (propertyClassHits >= 1 || identifierHits >= 2);
        }

        bool FixupChildProperties()
        {
            UE_Offsets *off = Offsets();
            if (!off || !Objects())
                return false;
            if (!off->Config.IsUsingFNamePool && off->FField.NamePrivate == 0)
                return false;

            const uintptr_t before = off->UStruct.ChildProperties;
            if (before && ValidateChildPropertiesChain(before))
            {
                LOGI("[AutoFix] UStruct.ChildProperties=0x%lx validated", (unsigned long)before);
                return true;
            }

            const uintptr_t childrenOff = off->UStruct.Children;
            if (childrenOff)
            {
                const uintptr_t derived = childrenOff + sizeof(void *);
                if (ValidateChildPropertiesChain(derived))
                {
                    LOGI("[AutoFix] Derived UStruct.ChildProperties=0x%lx (was 0x%lx)",
                         (unsigned long)derived, (unsigned long)before);
                    off->UStruct.ChildProperties = derived;
                    if (off->UStruct.PropertiesSize == 0)
                        off->UStruct.PropertiesSize = derived + sizeof(void *);
                    return true;
                }
            }

            const uintptr_t scanFrom = childrenOff ? (childrenOff + sizeof(void *))
                                                  : (off->UStruct.SuperStruct + sizeof(void *) * 2);
            for (uintptr_t candidate = scanFrom;
                 candidate <= scanFrom + 0x40;
                 candidate += sizeof(void *))
            {
                if (!ValidateChildPropertiesChain(candidate))
                    continue;
                LOGI("[AutoFix] Rescanned UStruct.ChildProperties=0x%lx (was 0x%lx)",
                     (unsigned long)candidate, (unsigned long)before);
                off->UStruct.ChildProperties = candidate;
                if (off->UStruct.PropertiesSize == 0)
                    off->UStruct.PropertiesSize = candidate + sizeof(void *);
                return true;
            }

            LOGW("[AutoFix] UStruct.ChildProperties fixup skipped/failed; keeping preset");
            return false;
        }

        // ----- UEnum.Names fixup ---------------------------------------------------------
        // 验证：UEnum 对象在 Names 偏移处应当是一个 TArray<TPair<FName,int64>>，其 Num 在
        // 合理范围内（2..4096），Data 指针有效，第一项的 FName 能被解码成合法标识符。
        bool ValidateUEnumNames(uintptr_t candidate, int sampleCap)
        {
            if (!candidate || !Objects())
                return false;
            UE_Offsets *off = Offsets();
            const uintptr_t nameOff = off->UObject.NamePrivate;
            if (!nameOff)
                return false;

            int hits = 0;
            int probed = 0;
            Objects()->ForEachObject([&](UE_UObject object)
            {
                if (probed >= sampleCap)
                    return true;

                const uintptr_t addr = reinterpret_cast<uintptr_t>(object.GetAddress());
                const uintptr_t classPtr = ReadPtr(addr + off->UObject.ClassPrivate);
                const std::string className = ReadObjectName(classPtr, nameOff);
                if (className != "Enum" && className != "UserDefinedEnum")
                    return false;

                ++probed;
                const uintptr_t dataPtr = ReadPtr(addr + candidate);
                const int32_t num = ReadI32(addr + candidate + sizeof(void *));
                const int32_t maxN = ReadI32(addr + candidate + sizeof(void *) + sizeof(int32_t));
                if (!IsLikelyPtr(dataPtr) || num < 2 || num > 65536 || maxN < num)
                    return false;

                const std::string firstName = ReadNameById(ReadI32(dataPtr));
                const std::string secondName = ReadNameById(ReadI32(dataPtr + 0x10));
                if (IsIdentifierLike(firstName) && IsIdentifierLike(secondName))
                    ++hits;
                return false;
            });
            return hits >= 1;
        }

        bool FixupUEnumNames()
        {
            UE_Offsets *off = Offsets();
            if (!off || off->UObject.NamePrivate == 0 || !Objects())
                return false;

            const uintptr_t before = off->UEnum.Names;

            uintptr_t scannedOffset = 0;
            Objects()->ForEachObject([&](UE_UObject object)
            {
                if (scannedOffset != 0)
                    return true;

                const uintptr_t addr = reinterpret_cast<uintptr_t>(object.GetAddress());
                const std::string objName = object.GetName();
                if (objName != "EInterpCurveMode")
                    return false;

                for (int d = 0; d < 50; ++d)
                {
                    const uintptr_t memberPtr = ReadPtr(addr + d * sizeof(void *));
                    if (!IsLikelyPtr(memberPtr))
                        continue;

                    const std::string memberName = ReadObjectName(memberPtr, off->UObject.NamePrivate);
                    if (memberName == "CIM_Linear")
                    {
                        scannedOffset = d * sizeof(void *);
                        LOGI("[AutoFix] Rescanned UEnum.Names=0x%lx (was 0x%lx) from EInterpCurveMode.CIM_Linear anchor",
                             (unsigned long)scannedOffset, (unsigned long)before);
                        return true;
                    }
                }
                return false;
            });

            if (scannedOffset != 0)
            {
                off->UEnum.Names = scannedOffset;
                return true;
            }

            for (uintptr_t candidate = 0x28; candidate <= 0x80; candidate += sizeof(void *))
            {
                if (!ValidateUEnumNames(candidate, kStrictSampleCap))
                    continue;
                LOGI("[AutoFix] Rescanned UEnum.Names=0x%lx (was 0x%lx) from generic Enum scan",
                     (unsigned long)candidate, (unsigned long)before);
                off->UEnum.Names = candidate;
                return true;
            }

            LOGI("[AutoFix] UEnum.Names not confirmed by scan; keeping preset");
            return false;
        }

        // ----- FField 布局 (UE >= 4.25) ---------------------------------------------------
        // FField 是 UE 4.25 起把 UProperty 从 UField 派生改成独立的 FField/FFieldClass 类型。
        // 布局:
        //   FField {
        //     void* vtable;
        //     FFieldClass* ClassPrivate;     // 类似 UClass，有 Name/SuperClass
        //     void* Owner;                   // FFieldVariant（UStruct* 或 FField*）
        //     FField* Next;
        //     FName NamePrivate;
        //     EObjectFlags FlagsPrivate;
        //   }
        // 锚点：World 类 ChildProperties 链上的某个属性，其 FFieldClass.Name 应当是已知
        // 类名（ObjectProperty / StructProperty 等），其 NamePrivate 应当是已知字段名
        // （PersistentLevel / OwningGameInstance / NetDriver 等）。
        bool ValidateFFieldClass(uintptr_t classOff)
        {
            UE_Offsets *off = Offsets();
            if (!classOff || !off->UObject.NamePrivate || !off->UStruct.ChildProperties)
                return false;
            const UE_UClass worldClass = Objects()->FindObject("Class Engine.World").Cast<UE_UClass>();
            if (!worldClass)
                return false;

            const uintptr_t worldAddr = reinterpret_cast<uintptr_t>(worldClass.GetAddress());
            uintptr_t prop = ReadPtr(worldAddr + off->UStruct.ChildProperties);
            int hits = 0;
            int depth = 0;
            const uintptr_t nextOff = off->FField.Next ? off->FField.Next : 0x20;
            while (IsLikelyPtr(prop) && depth < 64)
            {
                const uintptr_t fclass = ReadPtr(prop + classOff);
                if (IsLikelyPtr(fclass))
                {
                    const std::string clsName = ReadFFieldClassName(fclass);
                    if (IsPropertyClassName(clsName))
                        ++hits;
                    if (hits >= 2)
                        return true;
                }
                prop = ReadPtr(prop + nextOff);
                ++depth;
            }
            return hits >= 1;
        }

        bool FixupFFieldClass()
        {
            UE_Offsets *off = Offsets();
            if (!off || !off->Config.IsUsingFNamePool || !off->UStruct.ChildProperties)
                return false;

            const uintptr_t before = off->FField.ClassPrivate;
            if (before && ValidateFFieldClass(before))
            {
                LOGI("[AutoFix] FField.ClassPrivate=0x%lx validated", (unsigned long)before);
                return true;
            }
            for (uintptr_t candidate = 0x8; candidate <= 0x18; candidate += sizeof(void *))
            {
                if (!ValidateFFieldClass(candidate))
                    continue;
                LOGI("[AutoFix] Rescanned FField.ClassPrivate=0x%lx (was 0x%lx)",
                     (unsigned long)candidate, (unsigned long)before);
                off->FField.ClassPrivate = candidate;
                return true;
            }
            LOGI("[AutoFix] FField.ClassPrivate not confirmed by scan; keeping preset");
            return false;
        }

        bool ValidateFFieldNext(uintptr_t nextOff)
        {
            UE_Offsets *off = Offsets();
            if (!nextOff || !off->FField.NamePrivate || !off->UStruct.ChildProperties)
                return false;
            if (nextOff == off->FField.NamePrivate || nextOff == off->FField.ClassPrivate)
                return false;
            const UE_UClass worldClass = Objects()->FindObject("Class Engine.World").Cast<UE_UClass>();
            if (!worldClass)
                return false;

            const uintptr_t worldAddr = reinterpret_cast<uintptr_t>(worldClass.GetAddress());
            uintptr_t prop = ReadPtr(worldAddr + off->UStruct.ChildProperties);
            int validHits = 0;
            int depth = 0;
            while (IsLikelyPtr(prop) && depth < 256)
            {
                const std::string currentName = ReadObjectName(prop, off->FField.NamePrivate);
                const uintptr_t nextPtr = ReadPtr(prop + nextOff);
                if (IsLikelyPtr(nextPtr) && nextPtr != prop)
                {
                    const std::string nextName = ReadObjectName(nextPtr, off->FField.NamePrivate);
                    if (IsIdentifierLike(currentName) && IsIdentifierLike(nextName) && nextName != currentName &&
                        (IsKnownWorldPropertyName(currentName) || IsKnownWorldPropertyName(nextName)))
                    {
                        if (off->FField.ClassPrivate)
                        {
                            const std::string nextClassName = ReadFFieldClassName(ReadPtr(nextPtr + off->FField.ClassPrivate));
                            if (!IsPropertyClassName(nextClassName))
                            {
                                prop = nextPtr;
                                ++depth;
                                continue;
                            }
                        }
                        ++validHits;
                    }
                }
                if (validHits >= 3)
                    return true;
                prop = nextPtr;
                ++depth;
            }
            return validHits >= 2;
        }

        bool FixupFFieldNext()
        {
            UE_Offsets *off = Offsets();
            if (!off || !off->Config.IsUsingFNamePool || !off->UStruct.ChildProperties)
                return false;

            const uintptr_t before = off->FField.Next;
            if (before && ValidateFFieldNext(before))
            {
                LOGI("[AutoFix] FField.Next=0x%lx validated", (unsigned long)before);
                return true;
            }
            for (uintptr_t candidate = 0x18; candidate <= 0x30; candidate += sizeof(void *))
            {
                if (!ValidateFFieldNext(candidate))
                    continue;
                LOGI("[AutoFix] Rescanned FField.Next=0x%lx (was 0x%lx)",
                     (unsigned long)candidate, (unsigned long)before);
                off->FField.Next = candidate;
                return true;
            }
            LOGW("[AutoFix] FField.Next fixup failed; keeping preset");
            return false;
        }

        bool ValidateFFieldName(uintptr_t nameOff)
        {
            UE_Offsets *off = Offsets();
            if (!nameOff || !off->UStruct.ChildProperties)
                return false;
            const UE_UClass worldClass = Objects()->FindObject("Class Engine.World").Cast<UE_UClass>();
            if (!worldClass)
                return false;

            const uintptr_t worldAddr = reinterpret_cast<uintptr_t>(worldClass.GetAddress());
            const uintptr_t nextOff = off->FField.Next ? off->FField.Next : 0x20;
            uintptr_t prop = ReadPtr(worldAddr + off->UStruct.ChildProperties);
            int hits = 0;
            int depth = 0;
            while (IsLikelyPtr(prop) && depth < 64)
            {
                const std::string nm = ReadObjectName(prop, nameOff);
                if (IsKnownWorldPropertyName(nm))
                    ++hits;
                if (hits >= 1)
                    return true;
                prop = ReadPtr(prop + nextOff);
                ++depth;
            }
            return false;
        }

        bool FixupFFieldName()
        {
            UE_Offsets *off = Offsets();
            if (!off || !off->Config.IsUsingFNamePool || !off->UStruct.ChildProperties)
                return false;

            const uintptr_t before = off->FField.NamePrivate;
            if (before && ValidateFFieldName(before))
            {
                LOGI("[AutoFix] FField.NamePrivate=0x%lx validated", (unsigned long)before);
                return true;
            }
            for (uintptr_t candidate = 0x18; candidate <= 0x40; candidate += 0x4)
            {
                if (!ValidateFFieldName(candidate))
                    continue;
                LOGI("[AutoFix] Rescanned FField.NamePrivate=0x%lx (was 0x%lx)",
                     (unsigned long)candidate, (unsigned long)before);
                off->FField.NamePrivate = candidate;
                return true;
            }
            LOGW("[AutoFix] FField.NamePrivate fixup failed; keeping preset");
            return false;
        }

        // ----- FProperty 布局 (UE >= 4.25) -----------------------------------------------
        // FProperty 继承 FField，紧接其后是 5 个核心字段（ArrayDim/ElementSize/Flags/Offset/Size）。
        // dumper-7 通过寻找 ArrayDim==1 + ElementSize==8 (大多数指针属性) 的双 int32 模式定位。
        // 锚点：World.PersistentLevel（ObjectProperty，ElementSize=8，ArrayDim=1）。
        bool FindPersistentLevelProp(uintptr_t &outAddr)
        {
            UE_Offsets *off = Offsets();
            if (!off->UStruct.ChildProperties || !off->FField.NamePrivate || !off->FField.Next)
                return false;
            outAddr = FindPropertyLayoutAnchor();
            return outAddr != 0;
        }

        bool ValidateFPropertyLayout(const FPropertyLayoutScanResult &layout)
        {
            if (!layout.ArrayDim || !layout.ElementSize || !layout.PropertyFlags || !layout.OffsetInternal)
                return false;

            const uintptr_t layoutAnchor = FindPropertyLayoutAnchor();
            if (!layoutAnchor)
                return false;

            FPropertyLayoutScanResult probed;
            if (!TryResolveFPropertyLayout(layoutAnchor, layout.ArrayDim, 0, probed))
                return false;
            if (probed.ElementSize != layout.ElementSize ||
                probed.PropertyFlags != layout.PropertyFlags ||
                probed.OffsetInternal != layout.OffsetInternal)
            {
                return false;
            }

            if (layout.Size)
            {
                const uintptr_t sizeAnchor = FindPropertySizeAnchor();
                uintptr_t sizeOff = 0;
                if (!sizeAnchor || !ProbeFPropertySizeFromAnchor(sizeAnchor, sizeOff) || sizeOff != layout.Size)
                    return false;
            }

            return true;
        }

        bool FixupFPropertyLayout()
        {
            UE_Offsets *off = Offsets();
            if (!off || !off->Config.IsUsingFNamePool || !off->FField.NamePrivate)
                return false;

            const FPropertyLayoutScanResult before{
                off->FProperty.ArrayDim,
                off->FProperty.ElementSize,
                off->FProperty.PropertyFlags,
                off->FProperty.Offset_Internal,
                off->FProperty.Size,
            };
            if (ValidateFPropertyLayout(before))
            {
                LOGI("[AutoFix] FProperty layout validated: ArrayDim=0x%lx ElementSize=0x%lx PropFlags=0x%lx Offset=0x%lx Size=0x%lx",
                     (unsigned long)before.ArrayDim,
                     (unsigned long)before.ElementSize,
                     (unsigned long)before.PropertyFlags,
                     (unsigned long)before.OffsetInternal,
                     (unsigned long)before.Size);
                return true;
            }

            FPropertyLayoutScanResult found;
            const uintptr_t sizeAnchor = FindPropertySizeAnchor();
            if (sizeAnchor)
            {
                ProbeFPropertySizeFromAnchor(sizeAnchor, found.Size);
            }

            const uintptr_t layoutAnchor = FindPropertyLayoutAnchor();
            if (layoutAnchor)
            {
                ProbeFPropertyLayoutFromAnchor(layoutAnchor, found.Size, found);
                if (!found.ArrayDim && off->FProperty.Size)
                    ProbeFPropertyLayoutFromAnchor(layoutAnchor, off->FProperty.Size, found);
            }

            if (!found.ArrayDim && layoutAnchor)
            {
                for (uintptr_t candidate = 0x28; candidate <= 0x60; candidate += sizeof(int32_t))
                {
                    if (TryResolveFPropertyLayout(layoutAnchor, candidate, found.Size, found))
                    {
                        break;
                    }
                }
            }

            if (!found.ArrayDim || !found.OffsetInternal)
            {
                LOGI("[AutoFix] FProperty layout not confirmed by scan; keeping preset");
                return false;
            }

            off->FProperty.ArrayDim = found.ArrayDim;
            off->FProperty.ElementSize = found.ElementSize;
            off->FProperty.PropertyFlags = found.PropertyFlags;
            off->FProperty.Offset_Internal = found.OffsetInternal;
            if (found.Size)
                off->FProperty.Size = found.Size;

            LOGI("[AutoFix] FProperty layout: ArrayDim=0x%lx ElementSize=0x%lx PropFlags=0x%lx Offset=0x%lx Size=0x%lx",
                 (unsigned long)off->FProperty.ArrayDim,
                 (unsigned long)off->FProperty.ElementSize,
                 (unsigned long)off->FProperty.PropertyFlags,
                 (unsigned long)off->FProperty.Offset_Internal,
                 (unsigned long)off->FProperty.Size);
            return true;
        }

        // ----- UField.Next (UE < 4.25) ---------------------------------------------------
        // UE4.24 之前，UProperty 派生自 UField，Children 链节点是 UField*，靠 UField.Next 串联。
        // 锚点：World 类的 Children 链中应当能找到 PersistentLevel/NetDriver。
        bool ValidateUFieldNext(uintptr_t nextOff)
        {
            UE_Offsets *off = Offsets();
            if (!nextOff || !off->UObject.NamePrivate || !off->UStruct.Children)
                return false;
            const UE_UClass worldClass = Objects()->FindObject("Class Engine.World").Cast<UE_UClass>();
            if (!worldClass)
                return false;

            const uintptr_t worldAddr = reinterpret_cast<uintptr_t>(worldClass.GetAddress());
            uintptr_t child = ReadPtr(worldAddr + off->UStruct.Children);
            int depth = 0;
            int hits = 0;
            while (IsLikelyPtr(child) && depth < 128)
            {
                const std::string nm = ReadObjectName(child, off->UObject.NamePrivate);
                if (IsIdentifierLike(nm))
                    ++hits;
                if (hits >= 3)
                    return true;
                child = ReadPtr(child + nextOff);
                ++depth;
            }
            return hits >= 2;
        }

        bool FixupUFieldNext()
        {
            UE_Offsets *off = Offsets();
            if (!off || !off->UStruct.Children)
                return false;

            const uintptr_t before = off->UField.Next;
            if (before && ValidateUFieldNext(before))
            {
                LOGI("[AutoFix] UField.Next=0x%lx validated", (unsigned long)before);
                return true;
            }
            for (uintptr_t candidate = 0x20; candidate <= 0x40; candidate += sizeof(void *))
            {
                if (!ValidateUFieldNext(candidate))
                    continue;
                LOGI("[AutoFix] Rescanned UField.Next=0x%lx (was 0x%lx)",
                     (unsigned long)candidate, (unsigned long)before);
                off->UField.Next = candidate;
                return true;
            }
            LOGW("[AutoFix] UField.Next fixup failed; keeping preset");
            return false;
        }
    }  // namespace

    bool RunFixup(IGameProfile *profile)
    {
        (void)profile;
        if (!Offsets() || !Objects() || !Vars())
        {
            LOGE("[AutoFix] RunFixup: UEWrappers not initialized");
            return false;
        }

        LOGI("[AutoFix] === RunFixup begin ===");
        ApplyDefaultsOnly();

        UE_Offsets *off = Offsets();
        UE_Offsets before = *off;

        const bool nameOk = FixupNamePrivate();
        // 后续修复全部依赖 NamePrivate 解码 FName，因此 nameOk 失败时保留 preset。
        // 这些修补**只写**会失败兜底的字段，不影响 SDK 浏览器其它路径。
        const bool classOk = nameOk ? FixupClassPrivate() : false;
        if (nameOk) FixupOuterPrivate();
        const bool superOk = nameOk ? FixupSuperStruct() : false;
        if (nameOk) FixupChildren();
        // UField.Next 仅 UE<4.25 真正用，但即使 UE>=4.25 也可能需要它来遍历 UFunction 链；
        // 内部已根据 ChildProperties/Children 等情况自适应。
        if (nameOk) FixupUFieldNext();
        if (nameOk) FixupChildProperties();
        // FField 三件套：仅 IsUsingFNamePool && ChildProperties 有效时生效，
        // 顺序为 Name -> Class -> Next（Class 验证依赖 ChildProperties 链遍历，
        // Next 验证依赖 NamePrivate 已知，故先 Name）。
        if (nameOk) FixupFFieldName();
        if (nameOk) FixupFFieldNext();
        if (nameOk) FixupFFieldClass();
        if (nameOk) FixupFPropertyLayout();
        if (nameOk) FixupUEnumNames();
        const bool funcOk = nameOk ? FixupUFunctionFunc() : false;

        if (nameOk)
        {
            AutoFixPropertyOffsets::Invalidate();
            AutoFixStructLayout::Invalidate();

            AutoFixPropertyOffsets::EnsureResolved();
            AutoFixStructLayout::Warmup();
            LOGI("[AutoFix] Property-specific offsets and struct layout cache warmed");
        }

        DumpAutoFoundOffsets(before, *off);

        const bool success = nameOk && classOk && superOk && funcOk;
        LOGI("[AutoFix] === RunFixup done (success=%d) ===", success ? 1 : 0);
        return success;
    }
}  // namespace AutoFix
