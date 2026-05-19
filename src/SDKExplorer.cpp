#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "SDKExplorer.hpp"

#include "UE/UEMemory.hpp"
#include "UE/UEWrappers.hpp"

#include "imgui.h"

namespace SDKExplorer
{
    namespace
    {
        int gLang = 0;
        inline const char *Tr(const char *zh, const char *en) { return gLang == 0 ? zh : en; }

        // ---------- pagination / browser ----------
        int gPage = 0;
        int gPageSize = 100;
        bool gSearchMode = false;
        char gSearchBuf[128] = {};
        uint32_t gLastGen = 0xFFFFFFFFu;

        // ---------- search cache ----------
        std::vector<int32_t> gFilteredIdx;
        std::string gCachedNeedle;
        int32_t gScanCursor = 0;
        int32_t gScanTotal = 0;
        bool gScanning = false;
        bool gNeedRebuildSearch = false;

        // ---------- selection ----------
        uint8_t *gSelectedObj = nullptr;
        std::string gSelectedFullName;

        // ---------- inspector ----------
        char gInspectAddrBuf[32] = {};
        uint8_t *gInspectObj = nullptr;
        std::string gInspectError;
        struct TagEntry { std::string name; uint8_t *addr; };
        std::vector<TagEntry> gTags;
        std::vector<uint8_t *> gBackStack;

        // ---------- container view ----------
        int gContainerMaxRows = 64;

        // ---------- virtual keyboard ----------
        bool gKbOpen = false;
        char *gKbTarget = nullptr;
        size_t gKbTargetCap = 0;
        const char *gKbTitle = "";
        bool gKbShifted = true;  // 大写默认开
        // 触发回调：键盘上"确认"按钮按下后回填后做什么
        // 0 = 仅关闭；1 = 触发对象搜索；2 = 触发 Inspect
        int gKbConfirmAction = 0;

        void OpenKeyboard(char *buf, size_t cap, const char *title, int confirmAction)
        {
            gKbTarget = buf;
            gKbTargetCap = cap;
            gKbTitle = title;
            gKbConfirmAction = confirmAction;
            gKbOpen = true;
        }

        void KbAppend(const char *s)
        {
            if (!gKbTarget || !s) return;
            size_t curLen = std::strlen(gKbTarget);
            size_t addLen = std::strlen(s);
            if (curLen + addLen + 1 >= gKbTargetCap) return;
            std::strcat(gKbTarget, s);
        }

        void KbBackspace()
        {
            if (!gKbTarget) return;
            size_t n = std::strlen(gKbTarget);
            if (n == 0) return;
            gKbTarget[n - 1] = '\0';
        }

        void KbClear()
        {
            if (!gKbTarget) return;
            gKbTarget[0] = '\0';
        }

        // -------------------------------------------------------------------
        bool IsRuntimeReady()
        {
            return UEWrappers::GetUEVars() != nullptr &&
                   UEWrappers::GetObjects() != nullptr &&
                   UEWrappers::GetOffsets() != nullptr;
        }

        // 安全 tolower：避免 ::tolower 对负 char 出现 UB
        inline char SafeLower(char c)
        {
            unsigned char u = (unsigned char)c;
            if (u >= 'A' && u <= 'Z') return (char)(u - 'A' + 'a');
            return (char)u;
        }

        std::string ToLowerSafe(const std::string &s)
        {
            std::string out;
            out.resize(s.size());
            for (size_t i = 0; i < s.size(); ++i) out[i] = SafeLower(s[i]);
            return out;
        }

        // 被动匹配：仅检查给定对象名是否包含小写 needle
        bool MatchNeedle(const std::string &name, const std::string &lneedle)
        {
            if (lneedle.empty()) return true;
            if (name.empty()) return false;
            if (name.size() > 512) return false;
            for (size_t i = 0; i + lneedle.size() <= name.size(); ++i)
            {
                bool ok = true;
                for (size_t j = 0; j < lneedle.size(); ++j)
                {
                    if (SafeLower(name[i + j]) != lneedle[j]) { ok = false; break; }
                }
                if (ok) return true;
            }
            return false;
        }

        void RefreshIfNeeded()
        {
            if (!IsRuntimeReady()) return;
            uint32_t gen = UEWrappers::GetInitGeneration();
            if (gen != gLastGen)
            {
                gLastGen = gen;
                gSelectedObj = nullptr;
                gInspectObj = nullptr;
                gSelectedFullName.clear();
                gPage = 0;
                gFilteredIdx.clear();
                gCachedNeedle.clear();
                gScanning = false;
                gScanCursor = 0;
                gScanTotal = 0;
                gNeedRebuildSearch = true;
            }

            // 如果开启搜索且 needle 与缓存不一致，启动一次新增量扫描
            if (gSearchMode)
            {
                std::string cur = gSearchBuf;
                std::string lcur = ToLowerSafe(cur);
                if (gNeedRebuildSearch || lcur != gCachedNeedle)
                {
                    gCachedNeedle = lcur;
                    gFilteredIdx.clear();
                    gScanCursor = 0;
                    auto *arr0 = UEWrappers::GetObjects();
                    gScanTotal = arr0 ? arr0->GetNumElements() : 0;
                    gScanning = !lcur.empty() && gScanTotal > 0;
                    gNeedRebuildSearch = false;
                }
            }
            else
            {
                gScanning = false;
            }

            // 增量扫描：每帧固定预算，避免卡 UI
            if (gScanning)
            {
                const int kBudget = 800;
                auto *arr = UEWrappers::GetObjects();
                if (!arr) { gScanning = false; return; }
                int32_t end = gScanCursor + kBudget;
                if (end > gScanTotal) end = gScanTotal;
                for (; gScanCursor < end; ++gScanCursor)
                {
                    uint8_t *p = arr->GetObjectPtr(gScanCursor);
                    if (!p) continue;
                    UE_UObject obj(p);
                    std::string name = obj.GetName();
                    if (!MatchNeedle(name, gCachedNeedle)) continue;
                    gFilteredIdx.push_back(gScanCursor);
                    if (gFilteredIdx.size() >= 5000) { gScanning = false; break; }
                }
                if (gScanCursor >= gScanTotal) gScanning = false;
            }
        }

        int32_t GetTotal()
        {
            auto *arr = UEWrappers::GetObjects();
            return arr ? arr->GetNumElements() : 0;
        }

        void SelectObject(uint8_t *addr)
        {
            if (!addr) return;
            if (gSelectedObj && gSelectedObj != addr) gBackStack.push_back(gSelectedObj);
            gSelectedObj = addr;
            UE_UObject o(addr);
            gSelectedFullName = o.GetFullName();
            gInspectObj = addr;
            std::snprintf(gInspectAddrBuf, sizeof(gInspectAddrBuf), "0x%llx",
                          (unsigned long long)(uintptr_t)addr);
            gInspectError.clear();
        }

        // ----- Virtual keyboard ---------------------------------------------
        // 适配地址/Object 名常见字符：0-9 A-F 完整 A-Z _ . / : - + * x ：等
        void RenderVirtualKeyboard()
        {
            if (!gKbOpen || !gKbTarget) return;

            ImGui::SetNextWindowSize(ImVec2(720, 360), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowPos(ImVec2(80, 120), ImGuiCond_FirstUseEver);
            char wndTitle[128];
            std::snprintf(wndTitle, sizeof(wndTitle), "%s###sdkx_kb",
                          gKbTitle && *gKbTitle ? gKbTitle : Tr("虚拟键盘", "Keyboard"));
            if (!ImGui::Begin(wndTitle, &gKbOpen))
            {
                ImGui::End();
                return;
            }

            ImGui::TextDisabled("%s", Tr("当前内容：", "Current: "));
            ImGui::SameLine();
            ImGui::TextUnformatted(gKbTarget);

            ImVec2 sz(46.0f, 36.0f);
            const ImVec2 wide(96.0f, 36.0f);

            auto drawRow = [&](const char *row)
            {
                size_t n = std::strlen(row);
                for (size_t i = 0; i < n; ++i)
                {
                    char lab[2] = {row[i], '\0'};
                    ImGui::PushID((int)(intptr_t)(row + i));
                    if (ImGui::Button(lab, sz)) KbAppend(lab);
                    ImGui::PopID();
                    if (i + 1 < n) ImGui::SameLine();
                }
            };

            // 数字行
            drawRow("0123456789");

            // 十六进制 A-F 单独便利行
            ImGui::SameLine();
            if (ImGui::Button("0x", sz)) KbAppend("0x");

            // 字母 4 行（受 Shift 影响）
            const char *r1 = gKbShifted ? "QWERTYUIOP" : "qwertyuiop";
            const char *r2 = gKbShifted ? "ASDFGHJKL" : "asdfghjkl";
            const char *r3 = gKbShifted ? "ZXCVBNM" : "zxcvbnm";
            drawRow(r1);
            drawRow(r2);
            drawRow(r3);

            // 符号行：地址 / Object 路径常见
            const char *syms = "._/:-+*";
            drawRow(syms);
            ImGui::SameLine();
            if (ImGui::Button(" ", sz)) KbAppend(" ");

            // 控制键
            if (ImGui::Button(gKbShifted ? "Shift*" : "shift", wide)) gKbShifted = !gKbShifted;
            ImGui::SameLine();
            if (ImGui::Button(Tr("退格", "Back"), wide)) KbBackspace();
            ImGui::SameLine();
            if (ImGui::Button(Tr("清空", "Clear"), wide)) KbClear();
            ImGui::SameLine();
            if (ImGui::Button(Tr("关闭", "Close"), wide)) gKbOpen = false;
            ImGui::SameLine();
            if (ImGui::Button(Tr("确认", "Confirm"), wide))
            {
                if (gKbConfirmAction == 1)
                {
                    gSearchMode = true;
                    gPage = 0;
                }
                else if (gKbConfirmAction == 2)
                {
                    gInspectError.clear();
                    uintptr_t v = 0;
                    if (std::sscanf(gKbTarget, "0x%llx", (unsigned long long *)&v) != 1 &&
                        std::sscanf(gKbTarget, "%llx", (unsigned long long *)&v) != 1)
                    {
                        gInspectError = Tr("地址格式错误", "Bad address");
                    }
                    else
                    {
                        gInspectObj = (uint8_t *)v;
                        gSelectedObj = gInspectObj;
                        UE_UObject o(gInspectObj);
                        gSelectedFullName = o.GetFullName();
                    }
                }
                gKbOpen = false;
            }

            ImGui::End();
        }

        // ----- Object Browser -------------------------------------------------
        void RenderObjectBrowser()
        {
            auto *arr = UEWrappers::GetObjects();
            if (!arr) { ImGui::TextDisabled("no array"); return; }
            int32_t total = GetTotal();
            int pageSize = gPageSize <= 0 ? 100 : gPageSize;

            // 仅"全部"模式下使用分页；搜索模式下不分页，按需扫描前 N 条命中
            bool searching = gSearchMode && gSearchBuf[0] != '\0';
            int totalPages = total > 0 ? (total + pageSize - 1) / pageSize : 1;
            if (gPage >= totalPages) gPage = totalPages - 1;
            if (gPage < 0) gPage = 0;

            ImGui::Text("Total: %d  Page: %d/%d", total, gPage + 1, totalPages);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80.0f);
            ImGui::InputInt("PageSize", &gPageSize, 0, 0);
            if (gPageSize < 10) gPageSize = 10;
            if (gPageSize > 1000) gPageSize = 1000;

            // 搜索状态/进度
            if (searching)
            {
                ImGui::SameLine();
                if (gScanning)
                {
                    float pct = gScanTotal > 0 ? (float)gScanCursor / (float)gScanTotal : 0.0f;
                    ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1),
                                       "%s %d/%d (%.1f%%) %s%d",
                                       Tr("搜索中...", "Scanning..."),
                                       gScanCursor, gScanTotal, pct * 100.0f,
                                       Tr("命中:", "Hits:"),
                                       (int)gFilteredIdx.size());
                }
                else
                {
                    ImGui::TextColored(ImVec4(0.4f, 1, 0.4f, 1),
                                       "%s %d",
                                       Tr("命中:", "Hits:"),
                                       (int)gFilteredIdx.size());
                }
            }

            ImGui::SetNextItemWidth(260.0f);
            if (ImGui::InputText("##search", gSearchBuf, sizeof(gSearchBuf),
                                 ImGuiInputTextFlags_EnterReturnsTrue))
            {
                gSearchMode = true;
                gPage = 0;
                gNeedRebuildSearch = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(Tr("键盘##obj_search_kb", "KB##obj_search_kb")))
            {
                OpenKeyboard(gSearchBuf, sizeof(gSearchBuf),
                             Tr("对象搜索 - 虚拟键盘", "Object Search Keyboard"), 1);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(Tr("搜索", "Search")))
            {
                gSearchMode = true;
                gPage = 0;
                gNeedRebuildSearch = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(Tr("全部", "All")))
            {
                gSearchMode = false;
                gPage = 0;
                gFilteredIdx.clear();
                gCachedNeedle.clear();
                gScanning = false;
            }

            // 搜索模式下也支持分页（基于已命中条目）
            int filteredTotal = (int)gFilteredIdx.size();
            int searchPages = filteredTotal > 0 ? (filteredTotal + pageSize - 1) / pageSize : 1;
            if (searching)
            {
                if (gPage >= searchPages) gPage = searchPages - 1;
                if (gPage < 0) gPage = 0;
            }

            if (ImGui::SmallButton("<<")) gPage = 0;
            ImGui::SameLine();
            if (ImGui::SmallButton("<") && gPage > 0) --gPage;
            ImGui::SameLine();
            int maxPage = searching ? searchPages - 1 : totalPages - 1;
            if (ImGui::SmallButton(">") && gPage < maxPage) ++gPage;
            ImGui::SameLine();
            if (ImGui::SmallButton(">>")) gPage = maxPage;

            if (ImGui::BeginChild("##objlist", ImVec2(0.0f, 0.0f), true))
            {
                if (!searching)
                {
                    int start = gPage * pageSize;
                    int end = std::min(start + pageSize, total);
                    for (int row = start; row < end; ++row)
                    {
                        uint8_t *p = arr->GetObjectPtr(row);
                        if (!p) continue;
                        UE_UObject obj(p);
                        std::string name = obj.GetName();
                        if (name.empty()) continue;
                        char head[32];
                        std::snprintf(head, sizeof(head), "[%d] ", row);
                        bool sel = (gSelectedObj == p);
                        std::string line = head + name;
                        if (ImGui::Selectable(line.c_str(), sel))
                            SelectObject(p);
                    }
                }
                else
                {
                    int start = gPage * pageSize;
                    int end = std::min(start + pageSize, filteredTotal);
                    for (int i = start; i < end; ++i)
                    {
                        int32_t row = gFilteredIdx[i];
                        uint8_t *p = arr->GetObjectPtr(row);
                        if (!p) continue;
                        UE_UObject obj(p);
                        std::string name = obj.GetName();
                        if (name.empty()) continue;
                        char head[32];
                        std::snprintf(head, sizeof(head), "[%d] ", row);
                        bool sel = (gSelectedObj == p);
                        std::string line = head + name;
                        if (ImGui::Selectable(line.c_str(), sel))
                            SelectObject(p);
                    }
                }
            }
            ImGui::EndChild();
        }

        // ----- Properties (selected object meta) ------------------------------
        void RenderPropertiesPanel()
        {
            if (!gSelectedObj)
            {
                ImGui::TextDisabled("%s", Tr("未选中对象", "No object selected"));
                return;
            }
            UE_UObject o(gSelectedObj);
            uintptr_t addr = (uintptr_t)gSelectedObj;
            uintptr_t vftable = UEMemory::vm_rpm_ptr<uintptr_t>((void *)addr);
            auto cls = o.GetClass();
            auto outer = o.GetOuter();
            auto *off = UEWrappers::GetOffsets();
            uint8_t *fnameAddr = gSelectedObj + off->UObject.NamePrivate;

            if (ImGui::BeginTable("##props", 2,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn(Tr("字段", "Field"));
                ImGui::TableSetupColumn(Tr("值", "Value"));
                ImGui::TableHeadersRow();

                auto row = [&](const char *k, const std::string &v)
                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(k);
                    ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(v.c_str());
                };
                char buf[64];
                std::snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)addr);
                row("Address", buf);
                std::snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)vftable);
                row("VFTable", buf);
                std::snprintf(buf, sizeof(buf), "0x%llx %s",
                              (unsigned long long)(uintptr_t)cls.GetAddress(),
                              cls ? cls.GetName().c_str() : "");
                row("ClassPrivate", buf);
                std::snprintf(buf, sizeof(buf), "0x%x",
                              (unsigned)o.GetFlags());
                row("ObjectFlags", buf);
                std::snprintf(buf, sizeof(buf), "0x%llx %s",
                              (unsigned long long)(uintptr_t)outer.GetAddress(),
                              outer ? outer.GetName().c_str() : "");
                row("OuterPrivate", buf);
                std::snprintf(buf, sizeof(buf), "0x%llx %s",
                              (unsigned long long)(uintptr_t)fnameAddr,
                              o.GetName().c_str());
                row("NamePrivate", buf);
                std::snprintf(buf, sizeof(buf), "%d", o.GetIndex());
                row("InternalIndex", buf);
                row("FullName", o.GetFullName());
                row("CppName", o.GetCppName());

                ImGui::EndTable();
            }
        }

        // ----- Functions (UClass child UFunctions) ----------------------------
        void RenderFunctionsList()
        {
            if (!gSelectedObj)
            {
                ImGui::TextDisabled("%s", Tr("未选中对象", "No object selected"));
                return;
            }
            UE_UObject o(gSelectedObj);
            UE_UStruct s = o.IsA<UE_UStruct>() ? o.Cast<UE_UStruct>() : o.GetClass().Cast<UE_UStruct>();
            if (!s)
            {
                ImGui::TextDisabled("%s", Tr("无 UStruct 信息", "No UStruct info"));
                return;
            }
            ImGui::Text("%s: %s", Tr("类", "Class"), s.GetCppName().c_str());

            if (ImGui::BeginTable("##fns", 4,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_ScrollY |
                                      ImGuiTableFlags_SizingStretchProp,
                                  ImVec2(0, 0)))
            {
                ImGui::TableSetupColumn(Tr("签名", "Signature"), ImGuiTableColumnFlags_WidthStretch, 3.5f);
                ImGui::TableSetupColumn(Tr("Flags", "Flags"), ImGuiTableColumnFlags_WidthStretch, 2.0f);
                ImGui::TableSetupColumn("Func", ImGuiTableColumnFlags_WidthStretch, 1.4f);
                ImGui::TableSetupColumn(Tr("所属", "Class"), ImGuiTableColumnFlags_WidthStretch, 1.6f);
                ImGui::TableHeadersRow();

                int rows = 0;
                for (auto child = s.GetChildren(); child; child = child.GetNext())
                {
                    if (!child.IsA<UE_UFunction>()) continue;
                    auto fn = child.Cast<UE_UFunction>();
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(fn.GetName().c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(fn.GetFunctionFlags().c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("0x%llx", (unsigned long long)fn.GetFunc());
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(s.GetName().c_str());
                    if (++rows > 1024) break;
                }
                ImGui::EndTable();
            }
        }
        // ----- Inspector value reader ------------------------------------
        std::string FormatHexQword(uintptr_t v)
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)v);
            return buf;
        }

        std::string ReadFieldValue(uint8_t *base, const UE_FProperty &prop, UEPropertyType ty)
        {
            if (!base) return "";
            uint8_t *p = base + prop.GetOffset();
            switch (ty)
            {
                case UEPropertyType::FloatProperty:
                {
                    char b[32];
                    std::snprintf(b, sizeof(b), "%.4f", UEMemory::vm_rpm_ptr<float>(p));
                    return b;
                }
                case UEPropertyType::DoubleProperty:
                {
                    char b[32];
                    std::snprintf(b, sizeof(b), "%.6f", UEMemory::vm_rpm_ptr<double>(p));
                    return b;
                }
                case UEPropertyType::IntProperty:
                case UEPropertyType::Int32Property:
                {
                    char b[32];
                    std::snprintf(b, sizeof(b), "%d", UEMemory::vm_rpm_ptr<int32_t>(p));
                    return b;
                }
                case UEPropertyType::Int8Property:
                {
                    char b[32];
                    std::snprintf(b, sizeof(b), "%d", (int)UEMemory::vm_rpm_ptr<int8_t>(p));
                    return b;
                }
                case UEPropertyType::Int16Property:
                {
                    char b[32];
                    std::snprintf(b, sizeof(b), "%d", (int)UEMemory::vm_rpm_ptr<int16_t>(p));
                    return b;
                }
                case UEPropertyType::Int64Property:
                {
                    char b[32];
                    std::snprintf(b, sizeof(b), "%lld", (long long)UEMemory::vm_rpm_ptr<int64_t>(p));
                    return b;
                }
                case UEPropertyType::ByteProperty:
                {
                    char b[32];
                    std::snprintf(b, sizeof(b), "%u", (unsigned)UEMemory::vm_rpm_ptr<uint8_t>(p));
                    return b;
                }
                case UEPropertyType::UInt16Property:
                {
                    char b[32];
                    std::snprintf(b, sizeof(b), "%u", (unsigned)UEMemory::vm_rpm_ptr<uint16_t>(p));
                    return b;
                }
                case UEPropertyType::UInt32Property:
                {
                    char b[32];
                    std::snprintf(b, sizeof(b), "%u", UEMemory::vm_rpm_ptr<uint32_t>(p));
                    return b;
                }
                case UEPropertyType::UInt64Property:
                {
                    char b[32];
                    std::snprintf(b, sizeof(b), "%llu",
                                  (unsigned long long)UEMemory::vm_rpm_ptr<uint64_t>(p));
                    return b;
                }
                case UEPropertyType::BoolProperty:
                    return UEMemory::vm_rpm_ptr<uint8_t>(p) ? "true" : "false";
                case UEPropertyType::NameProperty:
                {
                    UE_FName fn(p);
                    return fn.GetName();
                }
                case UEPropertyType::ObjectProperty:
                case UEPropertyType::ClassProperty:
                case UEPropertyType::WeakObjectProperty:
                case UEPropertyType::LazyObjectProperty:
                case UEPropertyType::SoftObjectProperty:
                case UEPropertyType::SoftClassProperty:
                case UEPropertyType::InterfaceProperty:
                {
                    uint8_t *ptr = UEMemory::vm_rpm_ptr<uint8_t *>(p);
                    if (!ptr) return "nullptr";
                    UE_UObject obj(ptr);
                    std::string nm = obj.GetName();
                    return FormatHexQword((uintptr_t)ptr) +
                           (nm.empty() ? "" : (" " + nm));
                }
                case UEPropertyType::StrProperty:
                {
                    auto fs = UEMemory::vm_rpm_ptr<FString>(p);
                    return fs.ToString();
                }
                case UEPropertyType::ArrayProperty:
                case UEPropertyType::SetProperty:
                case UEPropertyType::MapProperty:
                {
                    auto h = UEMemory::vm_rpm_ptr<TArray<uint8_t>>(p);
                    char b[64];
                    std::snprintf(b, sizeof(b), "Count=%d Max=%d Data=0x%llx",
                                  h.Num(), h.Max(),
                                  (unsigned long long)(uintptr_t)h.GetData());
                    return b;
                }
                default:
                    return FormatHexQword((uintptr_t)UEMemory::vm_rpm_ptr<uintptr_t>(p));
            }
        }

        struct FieldRow
        {
            std::string type;
            std::string name;
            int32_t offset;
            int32_t size;
            UEPropertyType ty;
            uint8_t *propAddr;
            bool isF;
        };

        void CollectFields(const UE_UStruct &s, std::vector<FieldRow> &out, int depth = 0)
        {
            if (!s || depth > 16) return;
            UE_UStruct super = s.GetSuper();
            if (super) CollectFields(super, out, depth + 1);

            for (auto prop = s.GetChildProperties().Cast<UE_FProperty>();
                 prop; prop = prop.GetNext().Cast<UE_FProperty>())
            {
                FieldRow r;
                auto t = prop.GetType();
                r.type = t.second;
                r.ty = t.first;
                r.name = prop.GetName();
                r.offset = prop.GetOffset();
                r.size = prop.GetSize() * prop.GetArrayDim();
                r.propAddr = prop.GetAddress();
                r.isF = true;
                out.push_back(r);
            }
            for (auto child = s.GetChildren(); child; child = child.GetNext())
            {
                if (!child.IsA<UE_UProperty>()) continue;
                auto prop = child.Cast<UE_UProperty>();
                FieldRow r;
                auto t = prop.GetType();
                r.type = t.second;
                r.ty = t.first;
                r.name = prop.GetName();
                r.offset = prop.GetOffset();
                r.size = prop.GetSize() * prop.GetArrayDim();
                r.propAddr = prop.GetAddress();
                r.isF = false;
                out.push_back(r);
            }
        }

        // ----- Object Inspector ------------------------------------------
        void RenderInspectorPanel()
        {
            ImGui::SetNextItemWidth(220.0f);
            ImGui::InputText("##inspaddr", gInspectAddrBuf, sizeof(gInspectAddrBuf));
            ImGui::SameLine();
            if (ImGui::SmallButton(Tr("键盘##inspect_kb", "KB##inspect_kb")))
            {
                OpenKeyboard(gInspectAddrBuf, sizeof(gInspectAddrBuf),
                             Tr("地址输入 - 虚拟键盘", "Address Input Keyboard"), 2);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Inspect"))
            {
                gInspectError.clear();
                uintptr_t v = 0;
                if (std::sscanf(gInspectAddrBuf, "0x%llx", (unsigned long long *)&v) != 1 &&
                    std::sscanf(gInspectAddrBuf, "%llx", (unsigned long long *)&v) != 1)
                {
                    gInspectError = Tr("地址格式错误", "Bad address");
                }
                else
                {
                    gInspectObj = (uint8_t *)v;
                    gSelectedObj = gInspectObj;
                    UE_UObject o(gInspectObj);
                    gSelectedFullName = o.GetFullName();
                }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Add Tag") && gInspectObj)
            {
                UE_UObject o(gInspectObj);
                gTags.push_back({o.GetFullName(), gInspectObj});
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Back") && !gBackStack.empty())
            {
                gInspectObj = gBackStack.back();
                gSelectedObj = gInspectObj;
                gBackStack.pop_back();
                UE_UObject o(gInspectObj);
                gSelectedFullName = o.GetFullName();
                std::snprintf(gInspectAddrBuf, sizeof(gInspectAddrBuf), "0x%llx",
                              (unsigned long long)(uintptr_t)gInspectObj);
            }

            if (!gInspectError.empty())
                ImGui::TextColored(ImVec4(1, .4f, .4f, 1), "%s", gInspectError.c_str());

            if (!gTags.empty())
            {
                ImGui::TextDisabled("Tags:");
                ImGui::SameLine();
                for (size_t i = 0; i < gTags.size(); ++i)
                {
                    ImGui::PushID((int)i);
                    if (ImGui::SmallButton(gTags[i].name.c_str()))
                    {
                        gInspectObj = gTags[i].addr;
                        gSelectedObj = gInspectObj;
                        UE_UObject o(gInspectObj);
                        gSelectedFullName = o.GetFullName();
                        std::snprintf(gInspectAddrBuf, sizeof(gInspectAddrBuf), "0x%llx",
                                      (unsigned long long)(uintptr_t)gInspectObj);
                    }
                    ImGui::PopID();
                    ImGui::SameLine();
                }
                ImGui::NewLine();
            }

            if (!gInspectObj)
            {
                ImGui::TextDisabled("%s", Tr("未输入对象地址", "No address"));
                return;
            }

            UE_UObject o(gInspectObj);
            UE_UClass cls = o.GetClass();
            if (!cls)
            {
                ImGui::TextDisabled("%s", Tr("无效对象", "Invalid object"));
                return;
            }
            ImGui::Text("%s: %s", Tr("类", "Class"), cls.GetCppName().c_str());
            ImGui::Text("%s: %s", Tr("全名", "FullName"),
                        gSelectedFullName.c_str());

            std::vector<FieldRow> rows;
            CollectFields(cls.Cast<UE_UStruct>(), rows);

            if (ImGui::BeginTable("##insp", 5,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_ScrollY |
                                      ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch, 2.4f);
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 2.0f);
                ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthStretch, 0.7f);
                ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthStretch, 0.6f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 3.0f);
                ImGui::TableHeadersRow();

                int n = 0;
                for (auto &r : rows)
                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(r.type.c_str());
                    ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(r.name.c_str());
                    ImGui::TableSetColumnIndex(2); ImGui::Text("0x%X", r.offset);
                    ImGui::TableSetColumnIndex(3); ImGui::Text("0x%X", r.size);
                    ImGui::TableSetColumnIndex(4);
                    if (r.isF)
                    {
                        UE_FProperty p(r.propAddr);
                        std::string v = ReadFieldValue(gInspectObj, p, r.ty);
                        ImGui::TextUnformatted(v.c_str());
                        if ((r.ty == UEPropertyType::ObjectProperty ||
                             r.ty == UEPropertyType::ClassProperty) &&
                            ImGui::IsItemClicked(ImGuiMouseButton_Left))
                        {
                            uint8_t *ptr = UEMemory::vm_rpm_ptr<uint8_t *>(gInspectObj + r.offset);
                            if (ptr) SelectObject(ptr);
                        }
                    }
                    else
                    {
                        ImGui::TextDisabled("(UProperty)");
                    }
                    if (++n > 2048) break;
                }
                ImGui::EndTable();
            }
        }

        // ----- Container View --------------------------------------------
        void RenderContainerView()
        {
            if (!gSelectedObj)
            {
                ImGui::TextDisabled("%s", Tr("未选中对象", "No object selected"));
                return;
            }
            UE_UObject o(gSelectedObj);
            UE_UClass cls = o.GetClass();
            if (!cls)
            {
                ImGui::TextDisabled("%s", Tr("无效对象", "Invalid object"));
                return;
            }

            std::vector<FieldRow> rows;
            CollectFields(cls.Cast<UE_UStruct>(), rows);

            ImGui::SetNextItemWidth(120.0f);
            ImGui::InputInt("MaxRows", &gContainerMaxRows, 0, 0);
            if (gContainerMaxRows < 1) gContainerMaxRows = 1;
            if (gContainerMaxRows > 4096) gContainerMaxRows = 4096;

            if (ImGui::BeginTable("##ctn", 6,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_ScrollY |
                                      ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn("Index");
                ImGui::TableSetupColumn("Offset");
                ImGui::TableSetupColumn("Count");
                ImGui::TableSetupColumn("Max");
                ImGui::TableSetupColumn("KeyType");
                ImGui::TableSetupColumn("Value");
                ImGui::TableHeadersRow();

                int idx = 0;
                for (auto &r : rows)
                {
                    if (r.ty != UEPropertyType::ArrayProperty &&
                        r.ty != UEPropertyType::SetProperty &&
                        r.ty != UEPropertyType::MapProperty)
                        continue;

                    uint8_t *p = gSelectedObj + r.offset;
                    auto h = UEMemory::vm_rpm_ptr<TArray<uint8_t>>(p);

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::Text("%d", idx++);
                    ImGui::TableSetColumnIndex(1); ImGui::Text("0x%X", r.offset);
                    ImGui::TableSetColumnIndex(2); ImGui::Text("%d", h.Num());
                    ImGui::TableSetColumnIndex(3); ImGui::Text("%d", h.Max());
                    ImGui::TableSetColumnIndex(4); ImGui::TextUnformatted(r.type.c_str());
                    ImGui::TableSetColumnIndex(5);
                    ImGui::Text("Data:0x%llx %s",
                                (unsigned long long)(uintptr_t)h.GetData(),
                                r.name.c_str());

                    if (r.isF && r.ty == UEPropertyType::ArrayProperty)
                    {
                        UE_FProperty self(r.propAddr);
                        UE_FProperty inner = self.Cast<UE_FArrayProperty>().GetInner();
                        if (inner)
                        {
                            int32_t elemSize = inner.GetSize();
                            auto t = inner.GetType();
                            int n = std::min<int>(h.Num(), gContainerMaxRows);
                            for (int i = 0; i < n; ++i)
                            {
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0); ImGui::Text(" [%d]", i);
                                ImGui::TableSetColumnIndex(1);
                                ImGui::Text("0x%llx",
                                            (unsigned long long)(uintptr_t)h.GetData() +
                                                (unsigned long long)(i * elemSize));
                                ImGui::TableSetColumnIndex(4);
                                ImGui::TextUnformatted(t.second.c_str());
                                ImGui::TableSetColumnIndex(5);
                                uint8_t *elem = (uint8_t *)h.GetData() + i * elemSize;
                                std::string v;
                                switch (t.first)
                                {
                                    case UEPropertyType::IntProperty:
                                    case UEPropertyType::Int32Property:
                                    {
                                        char b[32];
                                        std::snprintf(b, sizeof(b), "%d",
                                                      UEMemory::vm_rpm_ptr<int32_t>(elem));
                                        v = b; break;
                                    }
                                    case UEPropertyType::FloatProperty:
                                    {
                                        char b[32];
                                        std::snprintf(b, sizeof(b), "%.4f",
                                                      UEMemory::vm_rpm_ptr<float>(elem));
                                        v = b; break;
                                    }
                                    case UEPropertyType::ObjectProperty:
                                    case UEPropertyType::ClassProperty:
                                    {
                                        uint8_t *ptr =
                                            UEMemory::vm_rpm_ptr<uint8_t *>(elem);
                                        v = FormatHexQword((uintptr_t)ptr);
                                        break;
                                    }
                                    case UEPropertyType::NameProperty:
                                    {
                                        UE_FName fn(elem);
                                        v = fn.GetName();
                                        break;
                                    }
                                    default:
                                        v = FormatHexQword(
                                            (uintptr_t)UEMemory::vm_rpm_ptr<uintptr_t>(elem));
                                }
                                ImGui::TextUnformatted(v.c_str());
                            }
                        }
                    }
                }
                ImGui::EndTable();
            }
        }

    }  // namespace

    void SetLanguage(int lang) { gLang = lang; }

    void Render()
    {
        if (!IsRuntimeReady())
        {
            ImGui::TextDisabled(
                "%s",
                Tr("尚未初始化 UE 环境，请先选择进程并\"开始探测\"。",
                   "UE not initialized. Probe a process first."));
            return;
        }
        RefreshIfNeeded();

        ImVec2 avail = ImGui::GetContentRegionAvail();
        float leftW = avail.x * 0.42f;

        ImGui::BeginChild("##sdkx_left", ImVec2(leftW, 0), true);
        ImGui::Text("%s", Tr("对象浏览器", "Object Browser"));
        ImGui::Separator();
        if (ImGui::BeginChild("##sdkx_left_top", ImVec2(0, avail.y * 0.55f), false))
            RenderObjectBrowser();
        ImGui::EndChild();
        ImGui::Separator();
        if (ImGui::BeginTabBar("##sdkx_left_tabs"))
        {
            if (ImGui::BeginTabItem(Tr("容器视图", "Container View")))
            {
                RenderContainerView();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(Tr("属性", "Properties")))
            {
                RenderPropertiesPanel();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::EndChild();

        ImGui::SameLine();

        ImGui::BeginChild("##sdkx_right", ImVec2(0, 0), true);
        if (ImGui::BeginChild("##sdkx_right_top", ImVec2(0, avail.y * 0.6f), false))
        {
            ImGui::Text("%s", Tr("对象成员视图", "Object Inspector"));
            ImGui::Separator();
            RenderInspectorPanel();
        }
        ImGui::EndChild();
        ImGui::Separator();
        ImGui::Text("%s", Tr("函数列表", "Functions"));
        RenderFunctionsList();
        ImGui::EndChild();

        RenderVirtualKeyboard();
    }
}  // namespace SDKExplorer

