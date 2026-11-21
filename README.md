# Split/Second SDL Controller Fix

Version 1.4 for the Windows PC release of Split/Second. SDL handles controller input and the game's original vibration signals. No external mapping utility or Steam configuration is required.

## Installation

[Download the ready-to-use v1.4 ZIP](dist/SplitSecond-Controller-Fix-v1.4.zip). Close the game and extract the ZIP contents into the folder containing `SplitSecond.exe`. All three DLLs must be in that folder. Then launch the game normally. Back up any existing `dinput8.dll` from another mod before replacing it; compatibility with other mods must be assessed separately.

The game executable is not modified and is not included in this repository. To remove the fix from an unmodified executable, delete the three supplied DLLs. See [INSTALL.txt](INSTALL.txt) for detailed instructions.

## Features

- Controller support in menus and races, with two independent controllers in local split-screen.
- Automatic switching between Xbox button prompts and keyboard prompts based on the most recently used input device.
- Xbox button names in the controls menu, including for remapped controls.
- Independent analog LT and RT input, including simultaneous use, with separate current and previous input buffers for each controller.
- The game's original vibration values are passed through SDL to both motors; no artificial effects are added.
- Inputs are neutralized on disconnect; controllers can reconnect during the same session.

For 8BitDo controllers, use Windows/XInput mode when available. Identical controllers without serial numbers cannot be reliably distinguished after they are disconnected simultaneously.

## Compatibility and test evidence

SHA-256 of the tested original PC executable:

```text
D11FC7F48DD504BDEFB16B548980BD41101F6C1A23D29C03487A81D4DE42173E
```

Game-specific hooks verify the expected instructions. Other executable versions are not guaranteed to work. If the layout differs, the game's combined DirectInput trigger axis may remain in use.

The integration test checks 10,201 simultaneous trigger combinations per controller through the original PC axis and action routines. It also checks remapping, input buffers, and reconnects. A running split-screen test logged different simultaneous trigger levels for both players. Adapter tests cover separate motor values and SDL output errors.

These tests used virtual SDL controllers. They do not fully verify physical Xbox One or 8BitDo hardware, USB/Bluetooth connections, or physical rumble motors. No direct comparison with the Xbox 360 version or quantitative drift-physics experiment was performed. The original PC vehicle physics is retained.

Details and measurements: [test report](tests/TEST-REPORT.txt), [integration test results](tests/independent-triggers-result.txt), [live measurements](tests/independent-triggers-live.csv).

## Building from source

Use Visual Studio 2022 **x86 Native Tools** and the official [SDL3-devel-3.4.20-VC package](https://github.com/libsdl-org/SDL/releases/tag/release-3.4.20). From the repository directory, with the extracted SDL package alongside it, run:

```bat
cl /LD /std:c++17 /EHsc /O2 /MT /D_CRT_SECURE_NO_WARNINGS /I SDL3-3.4.20\include sspad.cpp prompts.cpp triggers.cpp /link /DEF:sspad.def /OUT:sspad.dll SDL3-3.4.20\lib\x86\SDL3.lib user32.lib
copy sspad.dll dinput8.dll
```

Copy both DLLs and `SDL3-3.4.20\lib\x86\SDL3.dll` into the game folder. Test build commands are in the test report. The integration test requires a compatible game executable; it is not distributed here.

## Upstream work and licenses

The controller adapter is based on [Back on Air](https://github.com/xenophobentx/back-on-air), commit `57d17d88dc02e56ebcc6686efc6c20226d9cc5f1`, under the [MIT License](LICENSE.txt). Original copyright notices are preserved. This version adds device prompts, Xbox control labels, rumble forwarding, and independent analog triggers in the game input path.

SDL 3.4.20 is distributed under the [zlib License](SDL-LICENSE.txt). Split/Second and related trademarks belong to their respective owners. This is an unofficial modification.
