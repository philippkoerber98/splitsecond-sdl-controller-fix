#include <windows.h>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {
// Keep the two samples alongside the game's current/previous DirectInput
// buffers. Never poll SDL from an axis query: edge detection must see the
// previous frame, and querying one trigger must not advance the other.
struct Sample { const void* buffer = nullptr; float lt = 0, rt = 0; };
struct Device { const void* device; Sample samples[2]{}; unsigned next = 0; };
std::vector<Device> devices;
SRWLOCK lock = SRWLOCK_INIT;
using Axis = float(__thiscall*)(void*, const void*, int, int);
using Deadzone = void(__thiscall*)(void*, float*);
Axis original_axis = nullptr;
Deadzone original_deadzone = nullptr;

float __fastcall IndependentAxis(void* manager, void*, const void* entry, int axis, int buffer) {
    if (entry && (axis == 4 || axis == 5) && buffer >= 0 && buffer < 2) {
        auto bytes = static_cast<const unsigned char*>(entry);
        const void* device = *reinterpret_cast<void* const*>(bytes + 0x244);
        const void* state = bytes + 0x294 + buffer * 0x110;
        bool ours = false;
        float value = 0;
        AcquireSRWLockShared(&lock);
        for (const auto& record : devices) {
            if (record.device != device) continue;
            ours = true;
            for (const auto& sample : record.samples)
                if (sample.buffer == state) value = axis == 4 ? sample.lt : sample.rt;
            break;
        }
        ReleaseSRWLockShared(&lock);
        if (ours) {
            // Preserve the game's acquisition gate and original deadzone. All
            // action IDs, binding lookup, scaling and vehicle physics stay native.
            if (!bytes[0x558 + buffer]) return 0;
            original_deadzone(manager, &value);
            return value < 0 ? 0 : value > 1 ? 1 : value;
        }
    }
    return original_axis(manager, entry, axis, buffer);
}
}

void RecordGameTriggers(const void* device, const void* buffer, float lt, float rt, bool connected) {
    AcquireSRWLockExclusive(&lock);
    Device* found = nullptr;
    for (auto& record : devices) if (record.device == device) { found = &record; break; }
    if (!found) { devices.push_back({device}); found = &devices.back(); }
    if (!connected) for (auto& sample : found->samples) sample.lt = sample.rt = 0;
    Sample* sample = nullptr;
    for (auto& candidate : found->samples) if (candidate.buffer == buffer) sample = &candidate;
    if (!sample) sample = &found->samples[(found->next++) % 2];
    *sample = {buffer, lt, rt};
    ReleaseSRWLockExclusive(&lock);
}

void ClearGameTriggers(const void* device, bool remove) {
    AcquireSRWLockExclusive(&lock);
    for (auto it = devices.begin(); it != devices.end(); ++it) {
        if (it->device != device) continue;
        if (remove) devices.erase(it);
        else for (auto& sample : it->samples) sample.lt = sample.rt = 0;
        break;
    }
    ReleaseSRWLockExclusive(&lock);
}

bool InstallIndependentTriggers(void* image) {
    if (original_axis) return true;
    auto base = static_cast<unsigned char*>(image);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || dos->e_lfanew > 0x1000) return false;
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt->OptionalHeader.SizeOfImage < 0x910b00) return false;
    auto target = base + 0xdce0;
    const unsigned char prologue[] = {0x8b,0x44,0x24,0x0c,0x56,0x8b,0xf1};
    const unsigned char deadzone[] = {0xf3,0x0f,0x10,0x89,0x54,0xcf,0,0};
    const unsigned char dispatcher[] = {0xff,0x24,0x95,0xf4,0xde,0x40,0};
    // This edition uses absolute addresses. Refuse a different layout rather
    // than patching unrecognised instructions or an ASLR-relocated edition.
    if (reinterpret_cast<uintptr_t>(base) != 0x400000 || memcmp(target, prologue, sizeof prologue) ||
        memcmp(base + 0x10820, deadzone, sizeof deadzone) ||
        memcmp(base + 0xdd11, dispatcher, sizeof dispatcher)) return false;
    auto trampoline = static_cast<unsigned char*>(VirtualAlloc(nullptr, 12, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!trampoline) return false;
    memcpy(trampoline, target, 7); // Seven complete instructions bytes, no relative operands.
    trampoline[7] = 0xe9;
    *reinterpret_cast<int32_t*>(trampoline + 8) = static_cast<int32_t>((target + 7) - (trampoline + 12));
    DWORD old = 0;
    if (!VirtualProtect(trampoline, 12, PAGE_EXECUTE_READ, &old)) { VirtualFree(trampoline, 0, MEM_RELEASE); return false; }
    DWORD target_protection = 0;
    if (!VirtualProtect(target, 7, PAGE_EXECUTE_READWRITE, &target_protection)) { VirtualFree(trampoline, 0, MEM_RELEASE); return false; }
    original_axis = reinterpret_cast<Axis>(trampoline);
    original_deadzone = reinterpret_cast<Deadzone>(base + 0x10820);
    target[0] = 0xe9;
    *reinterpret_cast<int32_t*>(target + 1) = static_cast<int32_t>(reinterpret_cast<unsigned char*>(&IndependentAxis) - (target + 5));
    target[5] = target[6] = 0x90;
    VirtualProtect(target, 7, target_protection, &old);
    FlushInstructionCache(GetCurrentProcess(), trampoline, 12);
    FlushInstructionCache(GetCurrentProcess(), target, 7);
    return true;
}
