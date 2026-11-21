// Integration test: execute the actual PC game's axis and action routines,
// using SDL virtual devices through the production adapter. No vehicle logic
// is recreated here. Build as x86 DLL; run from the reserved-image test launcher.
#include "../sspad.cpp"
#include <cassert>
#include <fstream>
#include <iterator>
#include <algorithm>
void InstallXboxPrompts() {}

template<class T> T& at(void* p, size_t offset) { return *reinterpret_cast<T*>(static_cast<BYTE*>(p)+offset); }
static void CheckNear(float actual, float expected) {
    if (fabsf(actual-expected)>0.0001f) { printf("FAIL actual %.8f expected %.8f\n",actual,expected); abort(); }
}
struct TestPad {
    SDL_JoystickID id; SDL_Joystick* joystick; int low=0, high=0;
    static bool SDLCALL Rumble(void* p, Uint16 low, Uint16 high) {
        static_cast<TestPad*>(p)->low=low; static_cast<TestPad*>(p)->high=high; return true;
    }
    void attach(int index) {
        SDL_VirtualJoystickDesc d; SDL_INIT_INTERFACE(&d);
        d.type=SDL_JOYSTICK_TYPE_GAMEPAD; d.vendor_id=0x1209; d.product_id=0x4300+index;
        d.naxes=SDL_GAMEPAD_AXIS_COUNT; d.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
        d.name="Independent trigger integration test"; d.userdata=this; d.Rumble=Rumble;
        id=SDL_AttachVirtualJoystick(&d); assert(id); joystick=SDL_OpenJoystick(id); assert(joystick);
        values(0,0);
    }
    void values(int lt,int rt) {
        assert(SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_LEFT_TRIGGER,static_cast<Sint16>(lt==32767?32767:lt*2-32768)));
        assert(SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,static_cast<Sint16>(rt==32767?32767:rt*2-32768)));
    }
    void detach() { assert(SDL_DetachVirtualJoystick(id)); SDL_CloseJoystick(joystick); }
};
extern "C" __declspec(dllexport) int RunTests(const char* path) {

    setvbuf(stdout,nullptr,_IONBF,0); puts("START");
    std::ifstream f(path,std::ios::binary); assert(f);
    std::vector<char> file((std::istreambuf_iterator<char>(f)),{});
    auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(file.data());
    auto nt=reinterpret_cast<IMAGE_NT_HEADERS32*>(file.data()+dos->e_lfanew);
    auto image=reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
    DWORD protection=0; assert(image==reinterpret_cast<void*>(0x400000));
    assert(VirtualProtect(image,nt->OptionalHeader.SizeOfImage,PAGE_EXECUTE_READWRITE,&protection));
    memcpy(image,file.data(),nt->OptionalHeader.SizeOfHeaders);
    auto section=IMAGE_FIRST_SECTION(nt);
    for(int i=0;i<nt->FileHeader.NumberOfSections;++i) memcpy(image+section[i].VirtualAddress,file.data()+section[i].PointerToRawData,section[i].SizeOfRawData);
 assert(InstallIndependentTriggers(image));
 assert(InstallIndependentTriggers(image)); // idempotent
    auto axis=reinterpret_cast<float(__thiscall*)(void*,const void*,int,int)>(image+0xdce0);
    auto action=reinterpret_cast<float(__thiscall*)(void*,const void*,const void*)>(image+0xf0f0);
    std::vector<BYTE> manager(0xe800,0),bindings(2*0x584,0);
    at<int>(manager.data(),0xcf6c)=2; at<void*>(manager.data(),0xcf70)=bindings.data();
    at<int>(manager.data(),0xcf80)=11; at<float>(manager.data(),0xcf58)=1;
    // The game initialises this shared normalisation divisor at runtime.
    at<float>(image,0xcff070-0x400000)=32768;
    puts("hook installed"); LoadSDL(); assert(sdl_ready); BuildObjects(); puts("SDL ready");
    TestPad pads[2]; pads[0].attach(0); pads[1].attach(1);
    SDL_UpdateGamepads(); InitialSDLProducts();
    VPad* devices[2]; BYTE* entries[2]; int pairs[2][2]={{2,0},{2,1}};
    for(int i=0;i<2;++i) {
        Entry entry; entry.kind=Kind::SdlSlot; entry.slot=i;
        devices[i]=new VPad(entry,nullptr);
        DIPROPRANGE range{{sizeof(DIPROPRANGE),sizeof(DIPROPHEADER),0,DIPH_DEVICE},-32767,32767};
        assert(SUCCEEDED(devices[i]->SetProperty(DIPROP_RANGE,&range.diph)));
        entries[i]=manager.data()+0xcf94+i*0x560;
        at<void*>(entries[i],0x244)=devices[i]; entries[i][0x558]=entries[i][0x559]=1;
        auto table=bindings.data()+i*0x584;
        at<int>(table,0)=41; at<int>(table,8)=5; at<int>(table,12)=1; // gas -> RT
        at<int>(table,20)=42; at<int>(table,28)=4; at<int>(table,32)=1; // brake -> LT
        at<int>(table,0x564)=2; at<int>(table,0x568)=i; table[0x56c]=1;
    }
    auto sample=[&](int buffer) { at<int>(manager.data(),0xe770)=buffer; for(int i=0;i<2;++i) assert(SUCCEEDED(devices[i]->GetDeviceState(0x110,entries[i]+0x294+buffer*0x110))); };
    auto check=[&](int buffer,const int values[2][2]) {
        at<int>(manager.data(),0xe770)=buffer;
        for(int i=0;i<2;++i) {
            CheckNear(axis(manager.data(),entries[i],4,buffer),values[i][0]/32767.0f);
            CheckNear(axis(manager.data(),entries[i],5,buffer),values[i][1]/32767.0f);
            int gas=41,brake=42;
            CheckNear(action(manager.data(),pairs[i],&gas),values[i][1]/32767.0f);
            CheckNear(action(manager.data(),pairs[i],&brake),values[i][0]/32767.0f);
        }
    };
    // 101 x 101 simultaneous levels, second player's complement independent.
    for(int l=0;l<=100;++l) for(int r=0;r<=100;++r) {
        int v[2][2]={{l*32767/100,r*32767/100},{(100-r)*32767/100,(100-l)*32767/100}};
        for(int i=0;i<2;++i) pads[i].values(v[i][0],v[i][1]);
        sample((l+r)&1); check((l+r)&1,v);
    }
    puts("PASS: 10201 simultaneous pairs per controller through original axis AND original action lookup");
    int before[2][2]={{8192,24575},{32767,16384}},after[2][2]={{24575,8192},{16384,32767}};
    for(int i=0;i<2;++i)pads[i].values(before[i][0],before[i][1]); sample(0);
    for(int i=0;i<2;++i)pads[i].values(after[i][0],after[i][1]); sample(1);
    check(0,before);check(1,after);
    puts("PASS: previous/current frame snapshots remain separate");
    // Swap the trigger bindings using the original action table, then bind gas
    // to LB. The fix must follow the selected input, never hardcode an action.
    for(int i=0;i<2;++i) {
        auto table=bindings.data()+i*0x584; int gas=41,brake=42;
        at<int>(table,8)=4; at<int>(table,28)=5;
        CheckNear(action(manager.data(),pairs[i],&gas),after[i][0]/32767.f);
        CheckNear(action(manager.data(),pairs[i],&brake),after[i][1]/32767.f);
        at<int>(table,8)=4; at<int>(table,12)=0;
        SDL_SetJoystickVirtualButton(pads[i].joystick,SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,true);
    }
    sample(1);
    for(int i=0;i<2;++i){int gas=41;CheckNear(action(manager.data(),pairs[i],&gas),1);}
    puts("PASS: trigger rebinding and LB digital binding through original action lookup");
    for(int i=0;i<2;++i){XINPUT_VIBRATION v{static_cast<WORD>(1234+i),static_cast<WORD>(5678+i)};assert(RumbleSDL(i,v)==ERROR_SUCCESS);assert(pads[i].low==v.wLeftMotorSpeed && pads[i].high==v.wRightMotorSpeed);}
    puts("PASS: independent original motor channel transport unaffected");
    pads[0].detach(); sample(1);
    int disconnected[2][2]={{0,0},{16384,32767}};
    for(int i=0;i<2;++i){CheckNear(axis(manager.data(),entries[i],4,1),disconnected[i][0]/32767.f);CheckNear(axis(manager.data(),entries[i],5,1),disconnected[i][1]/32767.f);}
    CheckNear(axis(manager.data(),entries[0],4,0),0);CheckNear(axis(manager.data(),entries[0],5,0),0);
    pads[0].attach(0); Sleep(110); pads[0].values(24575,8192); sample(1); for(int retry=0;retry<30;++retry) { Sleep(20); pads[0].values(24575,8192); sample(1); Pad state{}; ReadSDL(0,state); if(state.lt>0.7f) break; }
    CheckNear(axis(manager.data(),entries[0],4,1),24575/32767.f);CheckNear(axis(manager.data(),entries[0],5,1),8192/32767.f);
    puts("PASS: disconnect neutralises both cached frames; reconnect restores independent triggers");
    devices[0]->Unacquire();CheckNear(axis(manager.data(),entries[0],4,1),0);CheckNear(axis(manager.data(),entries[0],5,1),0);
    entries[1][0x559]=0;CheckNear(axis(manager.data(),entries[1],4,1),0);CheckNear(axis(manager.data(),entries[1],5,1),0);
    puts("PASS: acquisition gate and Unacquire neutralise triggers");
    for(int i=0;i<2;++i) devices[i]->Release();
    puts("ALL INTEGRATION TESTS PASSED"); fflush(stdout); TerminateProcess(GetCurrentProcess(),0); return 0;
}










