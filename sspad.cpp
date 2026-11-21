// Split/Second controller patch, adapted from Back on Air (MIT).
// See LICENSE.txt for upstream attribution.
#define DIRECTINPUT_VERSION 0x0800
#include <initguid.h>  // first: defines the GUIDs of this translation unit
#include <windows.h>
#include <dinput.h>
#include <xinput.h>
#include <SDL3/SDL.h>

#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

void InstallXboxPrompts();
bool InstallIndependentTriggers(void* image);
void RecordGameTriggers(const void* device, const void* buffer, float lt, float rt, bool connected);
void ClearGameTriggers(const void* device, bool remove);

namespace {

void ss_log(const char* format, ...) {
#ifdef _DEBUG
  char message[512];
  va_list args;
  va_start(args, format);
  vsnprintf(message, sizeof message, format, args);
  va_end(args);
  OutputDebugStringA(message);
#else
  (void)format;
#endif
}

struct Config {
  int deadzone_left = 0;
  int deadzone_right = 0;
  int trigger_threshold = 0;
  bool translate = true;
} g_cfg;

CRITICAL_SECTION g_cs;

// The Xbox 360 pad as the game's table knows it: product GUID {028E045E-0000-0000-0000-504944564944}.
const GUID kProductX360 = {0x028E045E, 0x0000, 0x0000, {0x00, 0x00, 0x50, 0x49, 0x44, 0x56, 0x49, 0x44}};
const char kNameX360[] = "Controller (XBOX 360 For Windows)";
constexpr DWORD kDevTypeGamepad = 0x00010215;  // HID | standard gamepad subtype | DI8DEVTYPE_GAMEPAD

// The 15 product ids the game has a table for (first dword of the product GUID, PID<<16|VID).
// Index 0 and 1 are the Xbox 360 pad (wired, wireless); the rest are older pads and wheels.
const DWORD kGameKnownProducts[15] = {
    0x028E045E, 0x02A1045E, 0xCA04046D, 0xC218046D, 0xC299046D, 0xC29B046D, 0xC294046D, 0xC216046D,
    0x40010428, 0xFF0D06A3, 0x040C06A3, 0xB321044F, 0x990307B5, 0x00050C12, 0xC293046D};

bool GameKnows(DWORD p) {
  for (DWORD k : kGameKnownProducts)
    if (k == p) return true;
  return false;
}

DWORD Vid(DWORD p) { return p & 0xFFFF; }

// -------------------------------------------------------------------------------------- pad state

// One pad in the Xbox layout used by the game (all sticks up = +).
struct Pad {
  float lx = 0, ly = 0, rx = 0, ry = 0;  // -1..1
  float lt = 0, rt = 0;                  // 0..1
  WORD buttons = 0;                      // XINPUT_GAMEPAD_* bits
};



constexpr int kMaxPads = 4;
struct SdlSlot {
  SDL_Gamepad* pad = nullptr;
  SDL_JoystickID id = 0;
  SDL_GUID model{};
  std::string serial;
  DWORD product = 0;
  XINPUT_VIBRATION requested_rumble{};
  ULONGLONG rumble_refresh = 0;
};

std::vector<SdlSlot> sdl_slots;
SRWLOCK sdl_lock = SRWLOCK_INIT;
bool sdl_ready = false;
bool slots_initialized = false;
ULONGLONG last_reconnect_poll = 0;
HWND rumble_window = nullptr;

struct SdlGuard {
  SdlGuard() { AcquireSRWLockExclusive(&sdl_lock); }
  ~SdlGuard() { ReleaseSRWLockExclusive(&sdl_lock); }
};

void LoadSDL() {
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
  sdl_ready = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
  if (sdl_ready) {
    SDL_SetGamepadEventsEnabled(false);
    SDL_SetJoystickEventsEnabled(false);
  }
}

void UpdateSDL() {
  // XInput motor speeds remain in force until the next SetState. Preserve
  // exactly those requested speeds; this is transport refresh, not an effect.
  const auto rumble_now = GetTickCount64();
  const bool background = rumble_window && GetForegroundWindow() != rumble_window;
  for (auto& slot : sdl_slots) {
    if (!slot.pad) continue;
    if (!SDL_GamepadConnected(slot.pad)) {
      slot.requested_rumble = {};
      slot.rumble_refresh = 0;
    } else if (slot.requested_rumble.wLeftMotorSpeed || slot.requested_rumble.wRightMotorSpeed) {
      if (background) {
        SDL_RumbleGamepad(slot.pad, 0, 0, 0);
        slot.requested_rumble = {};
        slot.rumble_refresh = 0;
      } else if (rumble_now >= slot.rumble_refresh) {
        if (SDL_RumbleGamepad(slot.pad, slot.requested_rumble.wLeftMotorSpeed,
                             slot.requested_rumble.wRightMotorSpeed, 1000))
          slot.rumble_refresh = rumble_now + 250;
      }
    }
  }
  SDL_UpdateGamepads();
  for (auto& slot : sdl_slots) {
    if (slot.pad && !SDL_GamepadConnected(slot.pad)) {
      SDL_CloseGamepad(slot.pad);
      slot.pad = nullptr;
      slot.id = 0;
    }
  }
  bool vacant = false;
  for (const auto& slot : sdl_slots) vacant |= !slot.pad;
  const auto now = GetTickCount64();
  if (slots_initialized && (!vacant || now - last_reconnect_poll < 100)) return;
  last_reconnect_poll = now;

  int count = 0;
  SDL_JoystickID* ids = SDL_GetGamepads(&count);
  if (!ids) return;
  for (int i = 0; i < count; ++i) {
    if (!slots_initialized && sdl_slots.size() >= kMaxPads) break;
    bool already_open = false;
    for (const auto& slot : sdl_slots) already_open |= slot.id == ids[i];
    if (already_open) continue;

    SDL_Gamepad* pad = SDL_OpenGamepad(ids[i]);
    if (!pad) continue;
    const SDL_GUID model = SDL_GetGamepadGUIDForID(ids[i]);
    const char* serial = SDL_GetGamepadSerial(pad);
    if (!slots_initialized) {
      DWORD product = (static_cast<DWORD>(SDL_GetGamepadProduct(pad)) << 16) | SDL_GetGamepadVendor(pad);
      sdl_slots.push_back({pad, ids[i], model, serial ? serial : "", product});
      continue;
    }

    // Keep each existing game device in its original slot. Prefer exact serials
    // so another controller of the same model cannot claim a reserved slot.
    bool restored = false;
    for (auto& slot : sdl_slots) {
      if (slot.pad) continue;
      SDL_GUID empty{};
      const bool unassigned = memcmp(&slot.model, &empty, sizeof empty) == 0;
      if (!unassigned && (memcmp(&slot.model, &model, sizeof model) != 0 ||
          slot.serial != (serial ? serial : ""))) continue;
      slot.model = model;
      slot.serial = serial ? serial : "";
      slot.product = (static_cast<DWORD>(SDL_GetGamepadProduct(pad)) << 16) | SDL_GetGamepadVendor(pad);
      slot.pad = pad;
      slot.id = ids[i];
      restored = true;
      break;
    }
    if (!restored) SDL_CloseGamepad(pad);
  }
  SDL_free(ids);
  if (!slots_initialized) sdl_slots.resize(kMaxPads);
  slots_initialized = true;
}

std::vector<DWORD> InitialSDLProducts() {
  SdlGuard guard;
  std::vector<DWORD> products;
  if (!sdl_ready) return products;
  UpdateSDL();
  for (const auto& slot : sdl_slots) products.push_back(slot.product);
  return products;
}

float Norm16(int v) {
  return v < 0 ? v / 32768.0f : v / 32767.0f;
}

bool ReadSDL(int index, Pad& out) {
  out = {};
  SdlGuard guard;
  if (!sdl_ready || index < 0 || static_cast<size_t>(index) >= sdl_slots.size()) return false;
  UpdateSDL();
  SDL_Gamepad* pad = sdl_slots[index].pad;
  if (!pad) return false;

  Pad state;
  state.lx = Norm16(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX));
  state.ly = -Norm16(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY));
  state.rx = Norm16(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX));
  state.ry = -Norm16(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY));
  state.lt = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) / 32767.0f;
  state.rt = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) / 32767.0f;
  static constexpr struct { SDL_GamepadButton button; WORD bit; } buttons[] = {
    {SDL_GAMEPAD_BUTTON_SOUTH, XINPUT_GAMEPAD_A}, {SDL_GAMEPAD_BUTTON_EAST, XINPUT_GAMEPAD_B},
    {SDL_GAMEPAD_BUTTON_WEST, XINPUT_GAMEPAD_X}, {SDL_GAMEPAD_BUTTON_NORTH, XINPUT_GAMEPAD_Y},
    {SDL_GAMEPAD_BUTTON_BACK, XINPUT_GAMEPAD_BACK}, {SDL_GAMEPAD_BUTTON_START, XINPUT_GAMEPAD_START},
    {SDL_GAMEPAD_BUTTON_LEFT_STICK, XINPUT_GAMEPAD_LEFT_THUMB}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, XINPUT_GAMEPAD_RIGHT_THUMB},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, XINPUT_GAMEPAD_LEFT_SHOULDER}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER},
    {SDL_GAMEPAD_BUTTON_DPAD_UP, XINPUT_GAMEPAD_DPAD_UP}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_DOWN},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_LEFT}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT},
  };
  for (const auto& button : buttons)
    if (SDL_GetGamepadButton(pad, button.button)) state.buttons |= button.bit;
  if (!SDL_GamepadConnected(pad)) return false;
  out = state;
  return true;
}

volatile LONG keyboard_event = 0;
WNDPROC previous_window_proc = nullptr;
HWND input_window = nullptr;

LRESULT CALLBACK InputWindowProc(HWND window, UINT message, WPARAM key, LPARAM data) {
  if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN)
    InterlockedExchange(&keyboard_event, 1);
  WNDPROC original = previous_window_proc;
  if (message == WM_NCDESTROY) input_window = nullptr;
  return CallWindowProcW(original, window, message, key, data);
}

void ObserveInputWindow(HWND window) {
  if (!window || window == input_window) return;
  DWORD owner = 0;
  GetWindowThreadProcessId(window, &owner);
  if (owner != GetCurrentProcessId()) return;
  SetLastError(0);
  auto original = reinterpret_cast<WNDPROC>(
      SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&InputWindowProc)));
  if (!original && GetLastError()) return;
  previous_window_proc = original;
  input_window = window;
}

bool CurrentInputIsXbox() {
  static SRWLOCK input_lock = SRWLOCK_INIT;
  AcquireSRWLockExclusive(&input_lock);
  static ULONGLONG last_check = 0;
  static bool xbox = false;
  static bool had_controller = false;
  static bool key_down[256]{};
  static Pad previous[kMaxPads]{};
  const ULONGLONG now = GetTickCount64();
  if (now - last_check < 16) {
    ReleaseSRWLockExclusive(&input_lock);
    return xbox;
  }
  last_check = now;
  HWND foreground = GetForegroundWindow();
  DWORD process = 0;
  if (foreground) GetWindowThreadProcessId(foreground, &process);
  const bool focused = process == GetCurrentProcessId();
  bool keyboard_used = InterlockedExchange(&keyboard_event, 0) != 0;
  for (int key = 8; key < 256; ++key) {
    const SHORT state = GetAsyncKeyState(key);
    bool down = (state & 0x8000) != 0;
    if (focused && ((down && !key_down[key]) || (state & 1))) keyboard_used = true;
    key_down[key] = down;
  }

  bool connected = false;
  bool controller_used = false;
  for (int index = 0; index < kMaxPads; ++index) {
    Pad state;
    const bool present = ReadSDL(index, state);
    connected |= present;
    const Pad& before = previous[index];
    controller_used |= focused && present && (
        (state.buttons & ~before.buttons) ||
        (fabsf(state.lx) > 0.25f && fabsf(state.lx - before.lx) > 0.05f) ||
        (fabsf(state.ly) > 0.25f && fabsf(state.ly - before.ly) > 0.05f) ||
        (fabsf(state.rx) > 0.25f && fabsf(state.rx - before.rx) > 0.05f) ||
        (fabsf(state.ry) > 0.25f && fabsf(state.ry - before.ry) > 0.05f) ||
        (state.lt > 0.1f && fabsf(state.lt - before.lt) > 0.05f) ||
        (state.rt > 0.1f && fabsf(state.rt - before.rt) > 0.05f));
    previous[index] = state;
  }
  if (connected && !had_controller) xbox = true;
  if (keyboard_used) xbox = false;
  if (controller_used) xbox = true;
  if (!connected) xbox = false;
  had_controller = connected;
  const bool result = xbox;
  ReleaseSRWLockExclusive(&input_lock);
  return result;
}

DWORD RumbleSDL(int index, const XINPUT_VIBRATION& vibration) {
  SdlGuard guard;
  if (!sdl_ready || index < 0 || static_cast<size_t>(index) >= sdl_slots.size()) return ERROR_DEVICE_NOT_CONNECTED;
  SDL_Gamepad* pad = sdl_slots[index].pad;
  if (!pad || !SDL_GamepadConnected(pad)) return ERROR_DEVICE_NOT_CONNECTED;
  auto& slot = sdl_slots[index];
  const bool background = rumble_window && GetForegroundWindow() != rumble_window;
  const XINPUT_VIBRATION requested = background ? XINPUT_VIBRATION{} : vibration;
  if (!SDL_RumbleGamepad(pad, requested.wLeftMotorSpeed, requested.wRightMotorSpeed, 1000))
    return ERROR_NOT_SUPPORTED;
  slot.requested_rumble = requested;
  slot.rumble_refresh = GetTickCount64() + 250;
  return ERROR_SUCCESS;
}

// ---------------------------------------------------------------------------- the virtual DirectInput pad

// DIJOYSTATE2 as the game's data format describes it (its size is 0x110; the first 0x50 bytes are DIJOYSTATE).
constexpr DWORD kStateSize2 = 0x110;
constexpr DWORD kStateSize1 = 0x50;

struct PadObject {
  const GUID* type;
  DWORD ofs;
  DWORD dwtype;
  DWORD flags;
  const char* name;
};

constexpr int kButtons = 10;
std::vector<PadObject> g_objects;  // X Y Z Rx Ry, POV 0, buttons 0..9, at their DIJOYSTATE2 offsets

void BuildObjects() {
  static const struct {
    const GUID* g;
    const char* n;
  } axes[5] = {{&GUID_XAxis, "X Axis"}, {&GUID_YAxis, "Y Axis"}, {&GUID_ZAxis, "Z Axis"},
               {&GUID_RxAxis, "X Rotation"}, {&GUID_RyAxis, "Y Rotation"}};
  for (int i = 0; i < 5; ++i)
    g_objects.push_back({axes[i].g, static_cast<DWORD>(i * 4), static_cast<DWORD>(DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(i)), DIDOI_ASPECTPOSITION,
                         axes[i].n});
  g_objects.push_back({&GUID_POV, 0x20, DIDFT_POV | DIDFT_MAKEINSTANCE(0), 0, "Hat Switch"});
  static char names[kButtons][12];
  for (int i = 0; i < kButtons; ++i) {
    snprintf(names[i], sizeof names[i], "Button %d", i);
    g_objects.push_back({&GUID_Button, static_cast<DWORD>(0x30 + i), static_cast<DWORD>(DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(i)), 0, names[i]});
  }
}

bool ObjectMatches(const PadObject& o, DWORD flags) {
  if (flags & DIDFT_FFACTUATOR) return false;  // no force-feedback objects here
  if (flags & DIDFT_FFEFFECTTRIGGER) return false;
  DWORD kinds = flags & 0xFF;
  if (kinds == DIDFT_ALL) return true;
  return (o.dwtype & kinds & 0x1F) != 0;
}

enum class Kind { SdlSlot, DirectInputPad };

// How a real DirectInput pad's layout is read as an Xbox pad.
enum class Profile { Identity, Sony, Nintendo };

struct Entry {  // one device the game was offered in its last EnumDevices
  Kind kind = Kind::SdlSlot;
  int slot = -1;  // Stable SDL slot
  GUID instance{};
  GUID real_instance{};  // the real device's instance (translated or passed through)
  DWORD product = 0;
  std::string name;
  Profile profile = Profile::Identity;
  bool virt = true;     // false: a real device passed through untouched
  int list_index = -1;  // position in the game's own list (order of CreateDevice), for the rumble routing
};

std::vector<Entry> g_entries;
int g_created = 0;

int g_log_budget_input = 400;  // button-change lines per run

void DeadzoneStick(float x, float y, float dz, float& ox, float& oy) {
  float m = sqrtf(x * x + y * y);
  if (m <= dz || m <= 0.0f) {
    ox = oy = 0.0f;
    return;
  }
  float s = m > 1.0f ? 1.0f : m;
  float k = (s - dz) / (1.0f - dz) / m;
  ox = x * k;
  oy = y * k;
  if (ox > 1) ox = 1;
  if (ox < -1) ox = -1;
  if (oy > 1) oy = 1;
  if (oy < -1) oy = -1;
}

float DeadzoneTrigger(float t, float th) {
  if (t <= th) return 0.0f;
  float v = (t - th) / (1.0f - th);
  return v > 1.0f ? 1.0f : v;
}

LONG ScaleAxis(float n, LONG lo, LONG hi) {
  float c = (static_cast<float>(lo) + static_cast<float>(hi)) * 0.5f;
  float h = (static_cast<float>(hi) - static_cast<float>(lo)) * 0.5f;
  return static_cast<LONG>(lroundf(c + n * h));
}

DWORD PovFromDpad(WORD b) {
  bool u = b & XINPUT_GAMEPAD_DPAD_UP, d = b & XINPUT_GAMEPAD_DPAD_DOWN, l = b & XINPUT_GAMEPAD_DPAD_LEFT,
       r = b & XINPUT_GAMEPAD_DPAD_RIGHT;
  if (u && r) return 4500;
  if (r && d) return 13500;
  if (d && l) return 22500;
  if (l && u) return 31500;
  if (u) return 0;
  if (r) return 9000;
  if (d) return 18000;
  if (l) return 27000;
  return 0xFFFFFFFF;
}

// XInput button bit of each DirectInput button index, in the order a wired Xbox 360 pad reports them.
const WORD kButtonBits[kButtons] = {XINPUT_GAMEPAD_A,           XINPUT_GAMEPAD_B,            XINPUT_GAMEPAD_X,
                                    XINPUT_GAMEPAD_Y,           XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER,
                                    XINPUT_GAMEPAD_BACK,        XINPUT_GAMEPAD_START,         XINPUT_GAMEPAD_LEFT_THUMB,
                                    XINPUT_GAMEPAD_RIGHT_THUMB};
const char* const kButtonNames[kButtons] = {"A", "B", "X", "Y", "LB", "RB", "Back", "Start", "LS", "RS"};

// Real DirectInput device behind a translated virtual pad: a fixed private data format.
struct RawJoy {
  LONG a[8];  // X Y Z Rx Ry Rz slider0 slider1
  DWORD pov[4];
  BYTE b[32];
};

DIOBJECTDATAFORMAT g_rawfmt_objs[8 + 4 + 32];
DIDATAFORMAT g_rawfmt;

void BuildRawFormat() {
  const GUID* ax[8] = {&GUID_XAxis, &GUID_YAxis, &GUID_ZAxis, &GUID_RxAxis, &GUID_RyAxis, &GUID_RzAxis, &GUID_Slider, &GUID_Slider};
  int n = 0;
  for (int i = 0; i < 8; ++i)
    g_rawfmt_objs[n++] = {ax[i], static_cast<DWORD>(offsetof(RawJoy, a) + i * 4), 0x80000000u /* optional object */ | DIDFT_AXIS | DIDFT_ANYINSTANCE, 0};
  for (int i = 0; i < 4; ++i)
    g_rawfmt_objs[n++] = {&GUID_POV, static_cast<DWORD>(offsetof(RawJoy, pov) + i * 4), 0x80000000u /* optional object */ | DIDFT_POV | DIDFT_ANYINSTANCE, 0};
  for (int i = 0; i < 32; ++i)
    g_rawfmt_objs[n++] = {nullptr, static_cast<DWORD>(offsetof(RawJoy, b) + i), 0x80000000u /* optional object */ | DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0};
  g_rawfmt.dwSize = sizeof(DIDATAFORMAT);
  g_rawfmt.dwObjSize = sizeof(DIOBJECTDATAFORMAT);
  g_rawfmt.dwFlags = DIDF_ABSAXIS;
  g_rawfmt.dwDataSize = sizeof(RawJoy);
  g_rawfmt.dwNumObjs = n;
  g_rawfmt.rgodf = g_rawfmt_objs;
}

float Axis01(LONG v) { return static_cast<float>(v) / 65535.0f; }
float AxisSigned(LONG v) {
  float f = (static_cast<float>(v) - 32767.5f) / 32767.5f;
  return f < -1.0f ? -1.0f : (f > 1.0f ? 1.0f : f);
}

WORD DpadFromPov(DWORD pov) {
  if (LOWORD(pov) == 0xFFFF) return 0;
  DWORD a = pov % 36000;
  WORD d = 0;
  if (a >= 31500 || a <= 4500) d |= XINPUT_GAMEPAD_DPAD_UP;
  if (a >= 4500 && a <= 13500) d |= XINPUT_GAMEPAD_DPAD_RIGHT;
  if (a >= 13500 && a <= 22500) d |= XINPUT_GAMEPAD_DPAD_DOWN;
  if (a >= 22500 && a <= 31500) d |= XINPUT_GAMEPAD_DPAD_LEFT;
  return d;
}

void RawToPad(const RawJoy& r, Profile prof, Pad& p) {
  p = Pad();
  p.buttons = DpadFromPov(r.pov[0]);
  auto btn = [&](int i) { return (r.b[i] & 0x80) != 0; };
  auto set = [&](bool on, WORD bit) {
    if (on) p.buttons |= bit;
  };
  p.lx = AxisSigned(r.a[0]);
  p.ly = -AxisSigned(r.a[1]);
  switch (prof) {
    case Profile::Identity:  // an Xbox-like pad: X Y left stick, Rx Ry right stick, Z both triggers
      p.rx = AxisSigned(r.a[3]);
      p.ry = -AxisSigned(r.a[4]);
      {
        float z = AxisSigned(r.a[2]);
        p.lt = z > 0 ? z : 0;
        p.rt = z < 0 ? -z : 0;
      }
      for (int i = 0; i < kButtons; ++i) set(btn(i), kButtonBits[i]);
      break;
    case Profile::Sony:  // DualShock 4 / DualSense as Windows reports them
      p.rx = AxisSigned(r.a[2]);
      p.ry = -AxisSigned(r.a[5]);
      p.lt = Axis01(r.a[3]);
      p.rt = Axis01(r.a[4]);
      set(btn(1), XINPUT_GAMEPAD_A);               // cross
      set(btn(2), XINPUT_GAMEPAD_B);               // circle
      set(btn(0), XINPUT_GAMEPAD_X);               // square
      set(btn(3), XINPUT_GAMEPAD_Y);               // triangle
      set(btn(4), XINPUT_GAMEPAD_LEFT_SHOULDER);   // L1
      set(btn(5), XINPUT_GAMEPAD_RIGHT_SHOULDER);  // R1
      set(btn(8), XINPUT_GAMEPAD_BACK);            // share / create
      set(btn(9), XINPUT_GAMEPAD_START);           // options
      set(btn(10), XINPUT_GAMEPAD_LEFT_THUMB);
      set(btn(11), XINPUT_GAMEPAD_RIGHT_THUMB);
      break;
    case Profile::Nintendo:  // Switch Pro: buttons in positional order (B A Y X), digital ZL / ZR
      p.rx = AxisSigned(r.a[2]);
      p.ry = -AxisSigned(r.a[5]);
      p.lt = btn(6) ? 1.0f : 0.0f;
      p.rt = btn(7) ? 1.0f : 0.0f;
      set(btn(0), XINPUT_GAMEPAD_A);
      set(btn(1), XINPUT_GAMEPAD_B);
      set(btn(2), XINPUT_GAMEPAD_X);
      set(btn(3), XINPUT_GAMEPAD_Y);
      set(btn(4), XINPUT_GAMEPAD_LEFT_SHOULDER);
      set(btn(5), XINPUT_GAMEPAD_RIGHT_SHOULDER);
      set(btn(8), XINPUT_GAMEPAD_BACK);
      set(btn(9), XINPUT_GAMEPAD_START);
      set(btn(10), XINPUT_GAMEPAD_LEFT_THUMB);
      set(btn(11), XINPUT_GAMEPAD_RIGHT_THUMB);
      break;
  }
}

void FillInstance(const Entry& en, LPDIDEVICEINSTANCEA i) {
  DWORD sz = i->dwSize ? i->dwSize : sizeof(DIDEVICEINSTANCEA);
  if (sz > sizeof(DIDEVICEINSTANCEA)) sz = sizeof(DIDEVICEINSTANCEA);
  memset(i, 0, sz);
  i->dwSize = sz;
  i->guidInstance = en.instance;
  i->guidProduct = kProductX360;
  i->dwDevType = kDevTypeGamepad;
  snprintf(i->tszInstanceName, sizeof i->tszInstanceName, "%s", en.name.c_str());
  snprintf(i->tszProductName, sizeof i->tszProductName, "%s", kNameX360);
  i->wUsagePage = 1;
  i->wUsage = 5;
}

struct VPad final : IDirectInputDevice8A {
  LONG refs = 1;
  Entry e;
  IDirectInputDevice8A* inner = nullptr;  // DirectInputPad: the real device
  LONG rmin[5] = {0, 0, 0, 0, 0};
  LONG rmax[5] = {65535, 65535, 65535, 65535, 65535};  // DirectInput's default range until the game sets its own
  VPad(const Entry& en, IDirectInputDevice8A* in) : e(en), inner(in) {}

  // ---- IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** o) override {
    if (IsEqualIID(r, IID_IUnknown) || IsEqualIID(r, IID_IDirectInputDevice8A) || IsEqualIID(r, IID_IDirectInputDevice2A) ||
        IsEqualIID(r, IID_IDirectInputDeviceA)) {
      *o = static_cast<IDirectInputDevice8A*>(this);
      AddRef();
      return S_OK;
    }
    *o = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
  ULONG STDMETHODCALLTYPE Release() override {
    LONG n = InterlockedDecrement(&refs);
    if (n == 0) {
      ClearGameTriggers(this, true);
      if (e.kind == Kind::SdlSlot) RumbleSDL(e.slot, {});
      if (inner) inner->Release();
      delete this;
    }
    return n;
  }

  // ---- the state
  bool ReadPad(Pad& p) {
    if (e.kind == Kind::SdlSlot) return ReadSDL(e.slot, p);
    if (!inner) return false;
    RawJoy raw;
    for (int attempt = 0; attempt < 2; ++attempt) {
      inner->Poll();
      memset(&raw, 0, sizeof raw);
      HRESULT hr = inner->GetDeviceState(sizeof raw, &raw);
      if (SUCCEEDED(hr)) {
        RawToPad(raw, e.profile, p);
        return true;
      }
      if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
        if (FAILED(inner->Acquire())) break;
        continue;
      }
      break;
    }
    return false;
  }

  void Fill(const Pad& in, void* data, DWORD cb) {
    memset(data, 0, cb);
    LONG* ax = static_cast<LONG*>(data);
    DWORD* pov = reinterpret_cast<DWORD*>(static_cast<BYTE*>(data) + 0x20);
    BYTE* btn = static_cast<BYTE*>(data) + 0x30;
    for (int i = 0; i < 4; ++i) pov[i] = 0xFFFFFFFF;
    float lx, ly, rx, ry;
    DeadzoneStick(in.lx, in.ly, g_cfg.deadzone_left / 100.0f, lx, ly);
    DeadzoneStick(in.rx, in.ry, g_cfg.deadzone_right / 100.0f, rx, ry);
    float th = g_cfg.trigger_threshold / 100.0f;
    float lt = DeadzoneTrigger(in.lt, th), rt = DeadzoneTrigger(in.rt, th);
    // DirectInput: Y grows downwards; a wired Xbox 360 pad puts both triggers on Z (left +, right -).
    ax[0] = ScaleAxis(lx, rmin[0], rmax[0]);
    ax[1] = ScaleAxis(-ly, rmin[1], rmax[1]);
    ax[2] = ScaleAxis(lt - rt, rmin[2], rmax[2]);
    ax[3] = ScaleAxis(rx, rmin[3], rmax[3]);
    ax[4] = ScaleAxis(-ry, rmin[4], rmax[4]);
    ax[5] = ScaleAxis(0, rmin[0], rmax[0]);  // Rz: a pad that has none, centred
    ax[6] = 0;                               // sliders: none
    ax[7] = 0;
    pov[0] = PovFromDpad(in.buttons);
    for (int i = 0; i < kButtons; ++i)
      if (in.buttons & kButtonBits[i]) btn[i] = 0x80;
  }

  // ---- IDirectInputDevice8A
  HRESULT STDMETHODCALLTYPE GetCapabilities(LPDIDEVCAPS c) override {
    if (!c || c->dwSize < 24) return DIERR_INVALIDPARAM;
    DWORD sz = c->dwSize;
    memset(c, 0, sz);
    c->dwSize = sz;
    c->dwFlags = DIDC_ATTACHED;
    c->dwDevType = kDevTypeGamepad;
    c->dwAxes = 5;
    c->dwButtons = kButtons;
    c->dwPOVs = 1;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE EnumObjects(LPDIENUMDEVICEOBJECTSCALLBACKA cb, LPVOID ref, DWORD flags) override {
    if (!cb) return DIERR_INVALIDPARAM;
    for (const PadObject& o : g_objects) {
      if (!ObjectMatches(o, flags)) continue;
      DIDEVICEOBJECTINSTANCEA d;
      memset(&d, 0, sizeof d);
      d.dwSize = sizeof d;
      d.guidType = *o.type;
      d.dwOfs = o.ofs;
      d.dwType = o.dwtype;
      d.dwFlags = o.flags;
      snprintf(d.tszName, sizeof d.tszName, "%s", o.name);
      if (cb(&d, ref) == DIENUM_STOP) break;
    }
    return S_OK;
  }
  static int AxisOfProp(const DIPROPHEADER* h) {
    if (h->dwHow == DIPH_BYOFFSET) {
      if (h->dwObj < 20 && (h->dwObj % 4) == 0) return static_cast<int>(h->dwObj / 4);
      return -1;
    }
    if (h->dwHow == DIPH_DEVICE) return 0;
    return -1;
  }
  HRESULT STDMETHODCALLTYPE GetProperty(REFGUID prop, LPDIPROPHEADER h) override {
    uintptr_t id = reinterpret_cast<uintptr_t>(&prop);  // the property ids are small integers passed as GUID references
    if (!h) return DIERR_INVALIDPARAM;
    if (id == 4) {  // DIPROP_RANGE
      auto* r = reinterpret_cast<DIPROPRANGE*>(h);
      int a = AxisOfProp(h);
      if (a < 0) return DIERR_OBJECTNOTFOUND;
      r->lMin = rmin[a];
      r->lMax = rmax[a];
      return S_OK;
    }
    if (id == 5 || id == 6 || id == 3 || id == 1 || id == 2 || id == 9) {  // dead zone, saturation, granularity ...
      reinterpret_cast<DIPROPDWORD*>(h)->dwData = 0;
      return S_OK;
    }
    return DIERR_UNSUPPORTED;
  }
  HRESULT STDMETHODCALLTYPE SetProperty(REFGUID prop, LPCDIPROPHEADER h) override {
    uintptr_t id = reinterpret_cast<uintptr_t>(&prop);
    if (!h) return DIERR_INVALIDPARAM;
    if (id == 4) {  // DIPROP_RANGE
      auto* r = reinterpret_cast<const DIPROPRANGE*>(h);
      if (h->dwHow == DIPH_DEVICE) {
        for (int i = 0; i < 5; ++i) {
          rmin[i] = r->lMin;
          rmax[i] = r->lMax;
        }
        return S_OK;
      }
      int a = AxisOfProp(h);
      if (a < 0) return DIERR_OBJECTNOTFOUND;
      rmin[a] = r->lMin;
      rmax[a] = r->lMax;
      return S_OK;
    }
    // Dead zone, saturation, axis mode, auto-centre, buffer size, FF gain ...: accepted, nothing to do
    // (the layer applies its own dead zone; the data is absolute and unbuffered).
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Acquire() override {
    if (inner) inner->Acquire();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Unacquire() override {
    ClearGameTriggers(this, false);
    if (e.kind == Kind::SdlSlot) RumbleSDL(e.slot, {});
    if (inner) inner->Unacquire();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetDeviceState(DWORD cb, LPVOID data) override {
    if (!data) return DIERR_INVALIDPARAM;
    if (cb != kStateSize2 && cb != kStateSize1) return DIERR_INVALIDPARAM;
    Pad p;
    const bool connected = ReadPad(p);
    if (!connected) p = Pad();
    Fill(p, data, cb);
    if (e.kind == Kind::SdlSlot) {
      const float threshold = g_cfg.trigger_threshold / 100.0f;
      RecordGameTriggers(this, data, DeadzoneTrigger(p.lt, threshold), DeadzoneTrigger(p.rt, threshold), connected);
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetDeviceData(DWORD, LPDIDEVICEOBJECTDATA, LPDWORD n, DWORD) override {
    if (n) *n = 0;
    return S_OK;  // unbuffered device: nothing queued
  }
  HRESULT STDMETHODCALLTYPE SetDataFormat(LPCDIDATAFORMAT) override { return S_OK; }  // the game's c_dfDIJoystick2 is what we fill
  HRESULT STDMETHODCALLTYPE SetEventNotification(HANDLE) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND h, DWORD) override {
    ObserveInputWindow(h);
    rumble_window = h;
    // The real device (translated pads) is read non-exclusively and in the background; the foreground rule
    // is applied in GetDeviceState.
    if (inner) inner->SetCooperativeLevel(h, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetObjectInfo(LPDIDEVICEOBJECTINSTANCEA o, DWORD obj, DWORD how) override {
    if (!o) return DIERR_INVALIDPARAM;
    for (const PadObject& po : g_objects) {
      bool hit = (how == DIPH_BYOFFSET && po.ofs == obj) || (how == DIPH_BYID && po.dwtype == obj);
      if (!hit) continue;
      DWORD sz = o->dwSize;
      memset(o, 0, sz);
      o->dwSize = sz;
      o->guidType = *po.type;
      o->dwOfs = po.ofs;
      o->dwType = po.dwtype;
      o->dwFlags = po.flags;
      snprintf(o->tszName, sizeof o->tszName, "%s", po.name);
      return S_OK;
    }
    return DIERR_OBJECTNOTFOUND;
  }
  HRESULT STDMETHODCALLTYPE GetDeviceInfo(LPDIDEVICEINSTANCEA i) override {
    if (!i) return DIERR_INVALIDPARAM;
    FillInstance(e, i);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE RunControlPanel(HWND, DWORD) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE, DWORD, REFGUID) override { return DIERR_ALREADYINITIALIZED; }
  HRESULT STDMETHODCALLTYPE CreateEffect(REFGUID, LPCDIEFFECT, LPDIRECTINPUTEFFECT*, LPUNKNOWN) override { return DIERR_UNSUPPORTED; }
  HRESULT STDMETHODCALLTYPE EnumEffects(LPDIENUMEFFECTSCALLBACKA, LPVOID, DWORD) override { return S_OK; }  // none: rumble goes through SDL
  HRESULT STDMETHODCALLTYPE GetEffectInfo(LPDIEFFECTINFOA, REFGUID) override { return DIERR_UNSUPPORTED; }
  HRESULT STDMETHODCALLTYPE GetForceFeedbackState(LPDWORD) override { return DIERR_UNSUPPORTED; }
  HRESULT STDMETHODCALLTYPE SendForceFeedbackCommand(DWORD) override { return DIERR_UNSUPPORTED; }
  HRESULT STDMETHODCALLTYPE EnumCreatedEffectObjects(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK, LPVOID, DWORD) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE Escape(LPDIEFFESCAPE) override { return DIERR_UNSUPPORTED; }
  HRESULT STDMETHODCALLTYPE Poll() override { return S_OK; }  // the state is read when asked for
  HRESULT STDMETHODCALLTYPE SendDeviceData(DWORD, LPCDIDEVICEOBJECTDATA, LPDWORD, DWORD) override { return DIERR_UNSUPPORTED; }
  HRESULT STDMETHODCALLTYPE EnumEffectsInFile(LPCSTR, LPDIENUMEFFECTSINFILECALLBACK, LPVOID, DWORD) override { return DIERR_UNSUPPORTED; }
  HRESULT STDMETHODCALLTYPE WriteEffectToFile(LPCSTR, DWORD, LPDIFILEEFFECT, DWORD) override { return DIERR_UNSUPPORTED; }
  HRESULT STDMETHODCALLTYPE BuildActionMap(LPDIACTIONFORMATA, LPCSTR, DWORD) override { return DIERR_UNSUPPORTED; }
  HRESULT STDMETHODCALLTYPE SetActionMap(LPDIACTIONFORMATA, LPCSTR, DWORD) override { return DIERR_UNSUPPORTED; }
  HRESULT STDMETHODCALLTYPE GetImageInfo(LPDIDEVICEIMAGEINFOHEADERA) override { return DIERR_UNSUPPORTED; }
};

// ------------------------------------------------------------------------------ the DirectInput wrapper

GUID SyntheticInstance(int slot) {
  // {53535850-4144-000N-5353-504144303030}: ASCII "SSXP", "AD", slot, "SSPAD000"
  GUID g = {0x53535850, 0x4144, static_cast<WORD>(slot), {'S', 'S', 'P', 'A', 'D', '0', '0', '0'}};
  return g;
}

GUID TranslatedInstance(const GUID& real) {
  GUID g = real;
  g.Data1 ^= 0x53534D50;  // stays unique, never collides with a real instance id
  return g;
}

struct Candidate {  // a real DirectInput game controller
  DIDEVICEINSTANCEA inst;
};

BOOL CALLBACK CollectReal(const DIDEVICEINSTANCEA* d, LPVOID ref) {
  static_cast<std::vector<Candidate>*>(ref)->push_back({*d});
  return DIENUM_CONTINUE;
}

// The file name of the module that holds an address ("?" when none does).
std::string ModuleNameOf(const void* addr) {
  HMODULE m = nullptr;
  char path[MAX_PATH] = "";
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          static_cast<LPCSTR>(addr), &m) ||
      !GetModuleFileNameA(m, path, sizeof path))
    return "?";
  const char* slash = strrchr(path, '\\');
  return slash ? slash + 1 : path;
}

bool IsPadClass(DWORD devtype) {
  BYTE t = static_cast<BYTE>(devtype & 0xFF);
  return t == DI8DEVTYPE_GAMEPAD || t == DI8DEVTYPE_JOYSTICK;
}

Profile ProfileFor(DWORD product) {
  switch (Vid(product)) {
    case 0x054C: return Profile::Sony;
    case 0x057E: return Profile::Nintendo;
    default: return Profile::Identity;  // Xbox family and anything else: the Xbox order is assumed
  }
}

struct WrapDI final : IDirectInput8A {
  LONG refs = 1;
  IDirectInput8A* real;
  explicit WrapDI(IDirectInput8A* r) : real(r) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** o) override {
    if (IsEqualIID(r, IID_IUnknown) || IsEqualIID(r, IID_IDirectInput8A)) {
      *o = static_cast<IDirectInput8A*>(this);
      AddRef();
      return S_OK;
    }
    return real->QueryInterface(r, o);
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
  ULONG STDMETHODCALLTYPE Release() override {
    LONG n = InterlockedDecrement(&refs);
    if (n == 0) {
      real->Release();
      delete this;
    }
    return n;
  }

  HRESULT STDMETHODCALLTYPE CreateDevice(REFGUID guid, LPDIRECTINPUTDEVICE8A* out, LPUNKNOWN outer) override {
    Entry found;
    bool have = false;
    EnterCriticalSection(&g_cs);
    for (Entry& en : g_entries) {
      if (IsEqualGUID(en.instance, guid)) {
        en.list_index = g_created++;
        found = en;
        have = true;
        break;
      }
    }
    LeaveCriticalSection(&g_cs);
    if (!have) {  // the keyboard, the mouse: the settings menu (when it is wanted) keeps them quiet while it is open
      HRESULT hr = real->CreateDevice(guid, out, outer);
      if (SUCCEEDED(hr) && out && *out) {  // a game controller this layer did not offer: say which (diagnosis)
        DIDEVICEINSTANCEA di;
        memset(&di, 0, sizeof di);
        di.dwSize = sizeof di;
        if (SUCCEEDED((*out)->GetDeviceInfo(&di)) && IsPadClass(di.dwDevType))
          ss_log("[pad] the game opens a controller this layer did not offer: '%s' product %04X:%04X (its own table decides)",
                 di.tszInstanceName, (unsigned)Vid(di.guidProduct.Data1), (unsigned)(di.guidProduct.Data1 >> 16));
      }
      return hr;
    }
    ss_log("[pad] game opens device #%d: %s%s", found.list_index, found.name.c_str(), found.virt ? "" : " (passed through)");
    if (!found.virt) return real->CreateDevice(found.real_instance, out, outer);
    if (found.kind == Kind::SdlSlot) {
      *out = new VPad(found, nullptr);
      return S_OK;
    }
    IDirectInputDevice8A* dev = nullptr;
    HRESULT hr = real->CreateDevice(found.real_instance, &dev, nullptr);
    if (FAILED(hr)) return hr;
    dev->SetDataFormat(&g_rawfmt);
    for (DWORD i = 0; i < 6; ++i) {
      DIPROPRANGE r;
      r.diph.dwSize = sizeof r;
      r.diph.dwHeaderSize = sizeof(DIPROPHEADER);
      r.diph.dwObj = i * 4;
      r.diph.dwHow = DIPH_BYOFFSET;
      r.lMin = 0;
      r.lMax = 65535;
      dev->SetProperty(DIPROP_RANGE, &r.diph);
      DIPROPDWORD dz;
      dz.diph.dwSize = sizeof dz;
      dz.diph.dwHeaderSize = sizeof(DIPROPHEADER);
      dz.diph.dwObj = i * 4;
      dz.diph.dwHow = DIPH_BYOFFSET;
      dz.dwData = 0;
      dev->SetProperty(DIPROP_DEADZONE, &dz.diph);
    }
    *out = new VPad(found, dev);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE EnumDevices(DWORD type, LPDIENUMDEVICESCALLBACKA cb, LPVOID ref, DWORD flags) override {
    if (type != DI8DEVCLASS_GAMECTRL || !cb) return real->EnumDevices(type, cb, ref, flags);

    std::vector<Candidate> reals;
    real->EnumDevices(DI8DEVCLASS_GAMECTRL, CollectReal, &reals, flags);

    const auto products = InitialSDLProducts();
    std::vector<Entry> list;
    for (int s = 0; s < static_cast<int>(products.size()); ++s) {
      Entry en;
      en.kind = Kind::SdlSlot;
      en.slot = s;
      en.instance = SyntheticInstance(s);
      en.product = kProductX360.Data1;
      char nm[96];
      snprintf(nm, sizeof nm, "%s (SDL slot %d)", kNameX360, s + 1);
      en.name = nm;
      list.push_back(en);
    }
    for (const Candidate& c : reals) {
      const DIDEVICEINSTANCEA& d = c.inst;
      DWORD prod = d.guidProduct.Data1;
      const char* verdict;
      Entry en;
      en.real_instance = d.guidInstance;
      en.product = prod;
      en.name = d.tszInstanceName;
      bool xbox_vid = Vid(prod) == 0x045E;
      bool known_other = GameKnows(prod) && !xbox_vid;  // the game has its own table (older pads, wheels)
      bool covered_by_sdl = false;
      for (DWORD product : products) {
        // Some XUSB drivers expose a different Xbox VID/PID to DirectInput.
        covered_by_sdl |= product == prod || (xbox_vid && Vid(product) == 0x045E);
      }
      if (covered_by_sdl && IsPadClass(d.dwDevType)) {
        verdict = "hidden (SDL carries it)";
      } else if (known_other || !IsPadClass(d.dwDevType)) {
        verdict = known_other ? "passed through (the game has a table for this product)" : "passed through (not a pad)";
        en.virt = false;
        en.instance = d.guidInstance;
        list.push_back(en);
      } else if (!g_cfg.translate) {
        verdict = "passed through (pad_translate_dinput off)";
        en.virt = false;
        en.instance = d.guidInstance;
        list.push_back(en);
      } else {
        en.kind = Kind::DirectInputPad;
        en.virt = true;
        en.instance = TranslatedInstance(d.guidInstance);
        en.profile = xbox_vid ? Profile::Identity : ProfileFor(prod);
        char nm[200];
        snprintf(nm, sizeof nm, "%s (as Xbox 360 pad)", d.tszInstanceName);
        en.name = nm;
        verdict = en.profile == Profile::Sony       ? "translated (Sony layout)"
                  : en.profile == Profile::Nintendo ? "translated (Nintendo layout)"
                                                    : "translated (Xbox layout assumed)";
        list.push_back(en);
      }
      ss_log("[pad] real device '%s' product %04X:%04X type 0x%lx: %s", d.tszInstanceName, (unsigned)Vid(prod), (unsigned)(prod >> 16),
             static_cast<unsigned long>(d.dwDevType), verdict);
    }
    if (list.size() > 4) {
      ss_log("[pad] %u devices found, the game takes four: the rest is left out", (unsigned)list.size());
      list.resize(4);
    }

    EnterCriticalSection(&g_cs);
    g_entries = list;
    g_created = 0;
    LeaveCriticalSection(&g_cs);
    // The game's callback opens each device it is given right away (CreateDevice inside the callback), so a
    // callback from another module that opens none of them is a filter in between: Steam's overlay
    // (GameOverlayRenderer.dll) does this while Steam Input is on for the game, and the game then never sees these pads.
    const std::string cb_module = ModuleNameOf(reinterpret_cast<const void*>(cb));
    const bool cb_is_game = GetModuleHandleA(nullptr) == [&] {
      HMODULE m = nullptr;
      GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(cb), &m);
      return m;
    }();
    if (!cb_is_game)
      ss_log("[pad] the device list goes to a callback in %s, not the game's own: the offered pads go without a HID usage "
             "(Steam Input hides Xbox-type HID pads from the game)", cb_module.c_str());
    for (const Entry& en : list) {
      DIDEVICEINSTANCEA d;
      memset(&d, 0, sizeof d);
      d.dwSize = sizeof d;
      if (en.virt) {
        FillInstance(en, &d);
        // Steam's overlay (Steam Input on) wraps this interface and drops, from what it passes on to the game, every
        // device whose HID usage is joystick/gamepad (page 1, usage 4 or 5) and whose product GUID is a VID/PID
        // ("...PIDVID") of a controller it knows, 045E:028E among them; then it appends its own emulated pads.
        // The game never reads the usage fields (only the product GUID picks its table), so a filter in between
        // gets them blank: the pads pass, the game sees the same Xbox 360 pad, and takes them first (four at most).
        if (!cb_is_game) {
          d.wUsagePage = 0;
          d.wUsage = 0;
        }
      } else {
        for (const Candidate& c : reals)
          if (IsEqualGUID(c.inst.guidInstance, en.instance)) d = c.inst;
      }
      if (cb(&d, ref) == DIENUM_STOP) break;
    }
    EnterCriticalSection(&g_cs);
    const int opened = g_created;
    LeaveCriticalSection(&g_cs);
    if (!list.empty() && opened == 0 && !cb_is_game)
      ss_log("[pad] none of the %u offered controllers was opened: %s hid them from the game (Steam Input on for this game?); "
             "the game reads only the pads that module gives it, not this layer's",
             (unsigned)list.size(), cb_module.c_str());
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetDeviceStatus(REFGUID g) override { return real->GetDeviceStatus(g); }
  HRESULT STDMETHODCALLTYPE RunControlPanel(HWND h, DWORD f) override { return real->RunControlPanel(h, f); }
  HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE h, DWORD v) override { return real->Initialize(h, v); }
  HRESULT STDMETHODCALLTYPE FindDevice(REFGUID g, LPCSTR n, LPGUID o) override { return real->FindDevice(g, n, o); }
  HRESULT STDMETHODCALLTYPE EnumDevicesBySemantics(LPCSTR u, LPDIACTIONFORMATA a, LPDIENUMDEVICESBYSEMANTICSCBA c, LPVOID r,
                                                   DWORD f) override {
    return real->EnumDevicesBySemantics(u, a, c, r, f);
  }
  HRESULT STDMETHODCALLTYPE ConfigureDevices(LPDICONFIGUREDEVICESCALLBACK c, LPDICONFIGUREDEVICESPARAMSA p, DWORD f, LPVOID r) override {
    return real->ConfigureDevices(c, p, f, r);
  }
};

// ----------------------------------------------------------------------------------- the rumble routing

DWORD WINAPI HookedSetState(DWORD game_index, XINPUT_VIBRATION* v) {
  if (!v) return ERROR_BAD_ARGUMENTS;
  int slot = -1;
  EnterCriticalSection(&g_cs);
  for (const Entry& en : g_entries) {
    if (en.list_index == static_cast<int>(game_index) && en.kind == Kind::SdlSlot && en.virt) {
      slot = en.slot;
      break;
    }
  }
  LeaveCriticalSection(&g_cs);
  return RumbleSDL(slot, *v);
}

// Redirect the game's rumble import in memory, so an untouched game EXE
// also uses the stable SDL player slots after reconnecting.
void HookGameRumble() {
  auto image = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
  auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image);
  auto pe = reinterpret_cast<IMAGE_NT_HEADERS32*>(image + dos->e_lfanew);
  if (pe->Signature != IMAGE_NT_SIGNATURE || pe->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) return;
  const auto rva = pe->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
  if (!rva) return;
  auto module = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(image + rva);
  for (; module->Name; ++module) {
    if (_stricmp(reinterpret_cast<const char*>(image + module->Name), "xinput1_3.dll")) continue;
    if (!module->OriginalFirstThunk) return;
    auto names = reinterpret_cast<IMAGE_THUNK_DATA32*>(image + module->OriginalFirstThunk);
    auto imports = reinterpret_cast<IMAGE_THUNK_DATA32*>(image + module->FirstThunk);
    for (; names->u1.AddressOfData; ++names, ++imports) {
      bool set_state = IMAGE_SNAP_BY_ORDINAL32(names->u1.Ordinal) && IMAGE_ORDINAL32(names->u1.Ordinal) == 3;
      if (!IMAGE_SNAP_BY_ORDINAL32(names->u1.Ordinal)) {
        auto symbol = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(image + names->u1.AddressOfData);
        set_state = !strcmp(reinterpret_cast<const char*>(symbol->Name), "XInputSetState");
      }
      if (!set_state) continue;
      DWORD old;
      if (VirtualProtect(&imports->u1.Function, 4, PAGE_READWRITE, &old)) {
        InterlockedExchange(reinterpret_cast<volatile LONG*>(&imports->u1.Function),
                            reinterpret_cast<LONG>(&HookedSetState));
        DWORD ignored;
        VirtualProtect(&imports->u1.Function, 4, old, &ignored);
      }
    }
    return;
  }
}

INIT_ONCE init = INIT_ONCE_STATIC_INIT;
using DirectInputCreate = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
DirectInputCreate create_direct_input = nullptr;

BOOL CALLBACK InitializePatch(PINIT_ONCE, PVOID, PVOID*) {
  InitializeCriticalSection(&g_cs);
  BuildObjects();
  BuildRawFormat();
  LoadSDL();
  InstallXboxPrompts();
  InstallIndependentTriggers(GetModuleHandleW(nullptr));
  HookGameRumble();
  wchar_t system_path[MAX_PATH] = {};
  GetSystemDirectoryW(system_path, MAX_PATH);
  wcscat_s(system_path, L"\\dinput8.dll");
  HMODULE module = LoadLibraryW(system_path);
  if (module)
    create_direct_input = reinterpret_cast<DirectInputCreate>(GetProcAddress(module, "DirectInput8Create"));
  return TRUE;
}

}  // namespace

extern "C" HRESULT WINAPI CreateGameDirectInput(HINSTANCE instance, DWORD version, REFIID iid, LPVOID* out, LPUNKNOWN outer) {
  InitOnceExecuteOnce(&init, InitializePatch, nullptr, nullptr);
  if (!create_direct_input) return DIERR_NOTINITIALIZED;
  HRESULT result = create_direct_input(instance, version, iid, out, outer);
  if (SUCCEEDED(result) && out && *out && !outer && IsEqualIID(iid, IID_IDirectInput8A))
    *out = static_cast<IDirectInput8A*>(new WrapDI(static_cast<IDirectInput8A*>(*out)));
  return result;
}

extern "C" DWORD WINAPI SetGameVibration(DWORD game_index, XINPUT_VIBRATION* vibration) {
  InitOnceExecuteOnce(&init, InitializePatch, nullptr, nullptr);
  return HookedSetState(game_index, vibration);
}


bool XboxPromptsActive() {
  return CurrentInputIsXbox();
}


