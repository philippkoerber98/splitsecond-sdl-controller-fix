#include <windows.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <cstdio>

bool XboxPromptsActive();

namespace {
struct Value {
    uint32_t type = 0;
    uint32_t reserved = 0;
    union { const char* text; double number; uint64_t bits; };
    Value() : bits(0) {}
    static Value String(const char* text) { Value value; value.type = 4; value.text = text; return value; }
    static Value Number(double number) { Value value; value.type = 3; value.number = number; return value; }
};
static_assert(sizeof(Value) == 16);
using Invoke = uint32_t(__thiscall*)(void*, const char*, void*, const void*, uint32_t);
using SetVariable = uint32_t(__thiscall*)(void*, const char*, const Value*, int);
using GetVariable = uint32_t(__thiscall*)(void*, Value*, const char*);
using Display = void(__thiscall*)(void*);

SetVariable original_set = nullptr;

const char* XboxBindingName(const char* text) {
    static const char* const button_names[] = {"A", "B", "X", "Y", "LB", "RB", "Back / Ansicht", "Start / Menue", "LS druecken", "RS druecken"};
    int index=0, consumed=0;
    if ((sscanf(text,"Taste %d%n",&index,&consumed)==1 || sscanf(text,"Button %d%n",&index,&consumed)==1)
        && consumed && !text[consumed] && index>=1 && index<=10) return button_names[index-1];
    static const struct { const char* old_name; const char* xbox; } names[] = {
        {"Z-Achse Neg","RT"},{"Z-Achse Pos","LT"},
        {"X-Achse Neg","LS links"},{"X-Achse Pos","LS rechts"},
        {"Y-Achse Neg","LS oben"},{"Y-Achse Pos","LS unten"},
        {"R. X-Achse Neg","RS links"},{"R. X-Achse Pos","RS rechts"},
        {"R. Y-Achse Neg","RS oben"},{"R. Y-Achse Pos","RS unten"},
        {"Z Axis Neg","RT"},{"Z Axis Pos","LT"},
        {"X Axis Neg","LS left"},{"X Axis Pos","LS right"},
        {"Y Axis Neg","LS up"},{"Y Axis Pos","LS down"},
        {"R. X Axis Neg","RS left"},{"R. X Axis Pos","RS right"},
        {"R. Y Axis Neg","RS up"},{"R. Y Axis Pos","RS down"}
    };
    for (const auto& name:names) if (!strcmp(text,name.old_name)) return name.xbox;
    return nullptr;
}

uint32_t __fastcall BindingSet(void* movie, void*, const char* path, const Value* value, int mode) {
    // Translate the bound input, never the action. Rebinding either player's
    // controller therefore updates its label through the game's existing flow.
    if (path && strstr(path,".ActionBind.text") && value && value->type==4 && value->text) {
        if (const char* name=XboxBindingName(value->text)) {
            const Value translated=Value::String(name);
            return original_set(movie,path,&translated,mode);
        }
    }
    return original_set(movie,path,value,mode);
}

Invoke original_invoke = nullptr;
Display original_display = nullptr;
const char* const buttons[] = {"buttonA_mc", "buttonB_mc", "buttonX_mc", "buttonY_mc", "buttonSTART_mc"};
const char* const frames[] = {"Xenon_A", "Xenon_B", "Xenon_X", "Xenon_Y", "Xenon_Start"};

void SetNumber(void* movie, const std::string& path, double number) {
    auto table = *reinterpret_cast<uintptr_t**>(movie);
    Value value = Value::Number(number);
    reinterpret_cast<SetVariable>(table[0x2c / 4])(movie, path.c_str(), &value, 0);
}

bool GetNumber(void* movie, const char* path, double& number) {
    auto table = *reinterpret_cast<uintptr_t**>(movie);
    Value value;
    if (!(reinterpret_cast<GetVariable>(table[0x30 / 4])(movie, &value, path) & 255) || value.type != 3) return false;
    number = value.number;
    return true;
}

void SetFrame(void* movie, int index, bool xbox) {
    const std::string icon = std::string("main.bar_mc.") + buttons[index] + ".icon_mc";
    const Value frame = Value::String(xbox ? frames[index] : "PC_GenKeyLong");
    original_invoke(movie, (icon + ".gotoAndStop").c_str(), nullptr, &frame, 1);
}

uint32_t __fastcall PromptInvoke(void* movie, void*, const char* path, void* result,
                                 const void* args, uint32_t count) {
    if (movie && path && count == 1 && args) {
        const auto* value = static_cast<const Value*>(args);
        if (value->type == 4 && value->text && !strcmp(value->text, "PC_GenKeyLong")) {
            for (int index = 0; index < 5; ++index) {
                const std::string expected = std::string("main.bar_mc.") + buttons[index] + ".icon_mc.gotoAndStop";
                if (expected != path) continue;
                Value frame = Value::String(XboxPromptsActive() ? frames[index] : "PC_GenKeyLong");
                return original_invoke(movie, path, result, &frame, 1);
            }
        }
    }
    return original_invoke(movie, path, result, args, count);
}

// Run on the movie's own rendering thread. The movie owns the marker, so no
// pointer to a destroyed screen is retained, and reused screens stay correct.
void __fastcall PromptDisplay(void* movie, void*) {
    const bool xbox = XboxPromptsActive();
    double old_mode = -1;
    const bool has_marker = GetNumber(movie, "main.bar_mc.ssInputMode", old_mode);
    if (!has_marker || old_mode != static_cast<int>(xbox)) {
        double icon_x = 0;
        if (GetNumber(movie, "main.bar_mc.buttonA_mc.icon_mc._x", icon_x)) {
            for (int index = 0; index < 5; ++index) SetFrame(movie, index, xbox);
            SetNumber(movie, "main.bar_mc.ssInputMode", static_cast<int>(xbox));
        }
    }
    original_display(movie);
}

bool ReplaceSlot(uintptr_t* slot, uintptr_t value) {
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old)) return false;
    InterlockedExchange(reinterpret_cast<volatile LONG*>(slot), static_cast<LONG>(value));
    DWORD ignored;
    VirtualProtect(slot, sizeof(*slot), old, &ignored);
    return true;
}
}

void InstallXboxPrompts() {
    auto invoke_slot = reinterpret_cast<uintptr_t*>(0xc41d3c);
    auto display_slot = reinterpret_cast<uintptr_t*>(0xc41d70);
    if (GetModuleHandleW(nullptr) != reinterpret_cast<HMODULE>(0x400000) ||
        IsBadReadPtr(invoke_slot, 4) || *invoke_slot != 0x98d2e0 ||
        IsBadReadPtr(display_slot, 4) || *display_slot != 0x987e60 ||
        IsBadReadPtr(reinterpret_cast<void*>(0xbbaca8), 13) ||
        strcmp(reinterpret_cast<const char*>(0xbbaca8), "PC_GenKeyLong")) return;

    auto set_slot=reinterpret_cast<uintptr_t*>(0xc41d20);
    if (*set_slot != 0x991010) return;
    original_set=reinterpret_cast<SetVariable>(*set_slot);
    if (!ReplaceSlot(set_slot,reinterpret_cast<uintptr_t>(&BindingSet))) return;
    original_invoke = reinterpret_cast<Invoke>(*invoke_slot);
    original_display = reinterpret_cast<Display>(*display_slot);
    if (!ReplaceSlot(invoke_slot, reinterpret_cast<uintptr_t>(&PromptInvoke))) return;
    if (!ReplaceSlot(display_slot, reinterpret_cast<uintptr_t>(&PromptDisplay)))
        ReplaceSlot(invoke_slot, reinterpret_cast<uintptr_t>(original_invoke));
}

