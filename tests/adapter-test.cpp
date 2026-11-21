#define DIRECTINPUT_VERSION 0x0800
#include <initguid.h>
#include <windows.h>
#include <dinput.h>
#include <xinput.h>
#include <SDL3/SDL.h>
#include <cassert>
#include <cstdio>
#include <vector>
#include <utility>
#include <cstdlib>

struct VirtualPad {
  SDL_JoystickID id = 0;
  SDL_Joystick* joystick = nullptr;
  Uint16 rumble = 0, high = 0;
  bool reject = false;
  static bool SDLCALL Rumble(void* context, Uint16 low, Uint16 high) {
    auto pad = static_cast<VirtualPad*>(context);
    if (pad->reject) return false;
    pad->rumble = low; pad->high = high;
    return true;
  }
  void Attach(Uint16 product, SDL_GamepadButton button, Sint16 axis) {
    SDL_VirtualJoystickDesc spec;
    SDL_INIT_INTERFACE(&spec);
    spec.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    spec.vendor_id = 0x1209;
    spec.product_id = product;
    spec.naxes = SDL_GAMEPAD_AXIS_COUNT;
    spec.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    spec.name = "SplitSecond SDL test pad";
    spec.userdata = this;
    spec.Rumble = Rumble;
    id = SDL_AttachVirtualJoystick(&spec);
    assert(id);
    joystick = SDL_OpenJoystick(id);
    SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768);
    SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768);
    assert(joystick);
    assert(SDL_SetJoystickVirtualButton(joystick, button, true));
    assert(SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFTX, axis));
  }
  void Detach() {
    assert(SDL_DetachVirtualJoystick(id));
    SDL_CloseJoystick(joystick);
  }
};

std::vector<GUID> devices;
BOOL CALLBACK Collect(const DIDEVICEINSTANCEA* entry, void*) {
  if (entry->guidProduct.Data1 == 0x028e045e)
    devices.push_back(entry->guidInstance);
  return DIENUM_CONTINUE;
}

DIJOYSTATE2 Read(IDirectInputDevice8A* device) {
  DIJOYSTATE2 state{};
  assert(SUCCEEDED(device->Poll()));
  assert(SUCCEEDED(device->GetDeviceState(sizeof(state), &state)));
  return state;
}

int main() {
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
  assert(SDL_Init(SDL_INIT_GAMEPAD));
  int real_count = 0;
  SDL_JoystickID* physical = SDL_GetGamepads(&real_count);
  SDL_Gamepad* hardware = nullptr;
  if (real_count) {
    hardware = SDL_OpenGamepad(physical[0]);
    assert(hardware && SDL_GamepadConnected(hardware));
    printf("Physical SDL controller: %s\n", SDL_GetGamepadName(hardware));
  } else puts("No physical controller currently connected.");
  SDL_free(physical);

  VirtualPad first, second;
  first.Attach(1, SDL_GAMEPAD_BUTTON_SOUTH, 16000);
  second.Attach(2, SDL_GAMEPAD_BUTTON_EAST, -16000);

  HMODULE dll = LoadLibraryW(L".\\sspad.dll");
  assert(dll);
  auto create = reinterpret_cast<HRESULT(WINAPI*)(HINSTANCE,DWORD,REFIID,void**,IUnknown*)>(GetProcAddress(dll,"DirectInput8Create"));
  auto rumble = reinterpret_cast<DWORD(WINAPI*)(DWORD,XINPUT_VIBRATION*)>(GetProcAddress(dll,MAKEINTRESOURCEA(3)));
  assert(create && rumble);
  IDirectInput8A* input = nullptr;
  assert(SUCCEEDED(create(GetModuleHandleW(nullptr),0x800,IID_IDirectInput8A,reinterpret_cast<void**>(&input),nullptr)));
  assert(SUCCEEDED(input->EnumDevices(DI8DEVCLASS_GAMECTRL,Collect,nullptr,DIEDFL_ATTACHEDONLY)));
  assert(devices.size() == 4);
  std::vector<IDirectInputDevice8A*> pads;
  int south = -1, east = -1, real = -1;
  for (const GUID& id : devices) {
    IDirectInputDevice8A* pad = nullptr;
    assert(SUCCEEDED(input->CreateDevice(id,&pad,nullptr)));
    DIPROPRANGE range{{sizeof(DIPROPRANGE),sizeof(DIPROPHEADER),0,DIPH_BYOFFSET},-32767,32767};
    for (DWORD axis = 0; axis <= 16; axis += 4) {
      range.diph.dwObj = axis;
      assert(SUCCEEDED(pad->SetProperty(DIPROP_RANGE,&range.diph)));
    }
    auto state = Read(pad);
    if (state.rgbButtons[0] && state.lX == 16000) south = static_cast<int>(pads.size());
    else if (state.rgbButtons[1] && state.lX == -16000) east = static_cast<int>(pads.size());
    else if (hardware && real < 0) real = static_cast<int>(pads.size());
    pads.push_back(pad);
  }
  assert(south >= 0 && east >= 0);
  XINPUT_VIBRATION motors{1234,5678};
  if (real >= 0) assert(rumble(real,&motors) == ERROR_SUCCESS);
  XINPUT_VIBRATION stopped{};
  if (real >= 0) assert(rumble(real,&stopped) == ERROR_SUCCESS);
  puts("Adapter enumeration and independent SDL device state verified.");


  for (auto test : {std::pair<VirtualPad*,int>{&first,south}, {&second,east}}) {
    int previous = 1;
    for (int level=0; level<=100; ++level) {
      const Sint16 raw = static_cast<Sint16>(-32768 + (65535 * level) / 100);
      assert(SDL_SetJoystickVirtualAxis(test.first->joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, raw));
      const auto z = Read(pads[test.second]).lZ;
      const int expected = static_cast<int>(32767.0 * level / 100.0);
      assert(abs(z + expected)<=2 && z <= previous);
      previous=z;
    }
    SDL_SetJoystickVirtualAxis(test.first->joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,-32768);
    for (int level=0; level<=100; ++level) {
      SDL_SetJoystickVirtualAxis(test.first->joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER,
        static_cast<Sint16>(-32768 + (65535 * level)/100));
      const auto z = Read(pads[test.second]).lZ;
      assert(abs(z-static_cast<int>(32767.0 * level/100.0))<=2);
    }
    SDL_SetJoystickVirtualAxis(test.first->joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER,-32768);
    assert(Read(pads[test.second]).lZ==0);
  }
  puts("Both pads: 101 monotonic RT levels and 101 LT levels passed, signed Z range and neutral exact.");
  for (XINPUT_VIBRATION value : {XINPUT_VIBRATION{65535,0}, {0,65535}, {1234,5678}, {54321,9876}, {0,0}}) {
    assert(rumble(south,&value)==ERROR_SUCCESS);
    assert(first.rumble==value.wLeftMotorSpeed && first.high==value.wRightMotorSpeed);
    assert(rumble(east,&value)==ERROR_SUCCESS);
    assert(second.rumble==value.wLeftMotorSpeed && second.high==value.wRightMotorSpeed);
  }
  assert(rumble(south,&motors)==ERROR_SUCCESS);
  for(int n=0;n<60;++n) { Read(pads[south]); Read(pads[east]); Sleep(25); }
  assert(first.rumble==1234 && first.high==5678);
  assert(rumble(south,&stopped)==ERROR_SUCCESS && first.rumble==0 && first.high==0);
  first.reject=true;
  assert(rumble(south,&motors)==ERROR_NOT_SUPPORTED);
  first.reject=false;
  puts("Both motors: independent intensities, endpoints, stop, 1500 ms held original state, SDL failure propagation passed.");

  for (int cycle = 0; cycle < 5; ++cycle) {
    first.Detach();
    auto disconnected = Read(pads[south]);
    assert(disconnected.lX == 0 && disconnected.lY == 0 && disconnected.lZ == 0);
    assert(disconnected.rgdwPOV[0] == 0xffffffffu);
    for (BYTE button : disconnected.rgbButtons) assert(button == 0);
    assert(Read(pads[east]).rgbButtons[1] == 128);
    assert(rumble(south,&motors) == ERROR_DEVICE_NOT_CONNECTED);
    assert(rumble(east,&motors) == ERROR_SUCCESS && second.rumble == 1234 && second.high == 5678);

    auto previous = first.id;
    first.Attach(1, SDL_GAMEPAD_BUTTON_SOUTH, 16000);
    assert(first.id != previous);
    bool recovered = false;
    for (int attempt = 0; attempt < 50; ++attempt) {
      auto current = Read(pads[south]);
      if (current.rgbButtons[0] == 128 && current.lX == 16000) {
        recovered = true;
        break;
      }
      Sleep(10);
    }
    assert(recovered);
    assert(Read(pads[east]).lX == -16000);
    assert(rumble(south,&motors) == ERROR_SUCCESS && first.rumble == 1234 && first.high == 5678);
  }
  puts("Five SDL detach/reattach cycles: neutral input, independent second pad, same restored slot, correct rumble.");
  for (auto* pad : pads) pad->Release();
  input->Release();
  second.Detach();
  first.Detach();
  if (hardware) SDL_CloseGamepad(hardware);
}
