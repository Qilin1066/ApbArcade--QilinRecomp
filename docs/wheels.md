# Wheels: direct drive (MOZA R5) and continuous inputs

APB reads a free-spinning optical encoder, not the wheel's position relative to a centre. The runtime sends movement to the cabinet's wrapping LETA counter, with 160 counts per physical revolution by default. Turning back sends movement back; holding a continuous wheel still does nothing.

## Which wheels

APB's cabinet wheel spins freely and never stops, so the PC wheel that drives it must too: a **direct-drive wheel turning freely**, with no stop. Wheels that stop at a lock (belt or gear driven, such as the Logitech G920, or a direct-drive base with a software stop) can't drive it and aren't supported; the keyboard and gamepads steer without any wheel. A settings file from an older version with a wheel set up for limited rotation loads with steering back on the keys and gamepad.

## Wheel input

Choose the **Wheel input**:

| Input | Use | Behaviour |
|---|---|---|
| MOZA physical angle | MOZA SDK on Windows x64, including the R5 | Reads the SDK's physical angle in degrees, separately from its joystick axis, which the driver clips. |
| Wrapping joystick axis | A wheel or encoder whose reported axis rolls from one end to the other as it keeps turning | Accumulates each SDL motion event and unwraps the seam. Set the degrees one complete axis cycle represents (**Degrees per reported axis cycle**, under **Advanced wheel input**: a conversion scale, not a limit), or measure one turn. |

Physical freedom to turn and the input available to software are different. Opening up a direct-drive base's stop does not prove its ordinary joystick axis reports unlimited movement. If the position meter stays at an endpoint while the wheel keeps turning, software cannot reconstruct those turns from that axis: use the physical-angle source. A wrapping axis must really wrap: one that stops at its ends would turn a large real movement into a false seam crossing.

## MOZA R5 on Windows x64

1. Run the **SDK-enabled MOZA Pit House build required by MOZA's SDK documentation**; an ordinary Pit House installation is not proof that its SDK service is available. Obtain the appropriate build through MOZA's SDK/support channels if needed. Configure the base for the free rotation you want and turn off the driver's mechanical centring/spring for the cabinet feel. The game does not modify global Pit House settings or manufacture rotation beyond a driver-imposed stop.
2. Obtain the official [MOZA Racing SDK](https://mozaracing.com/pages/sdk) (the [download inspected for this implementation](https://cdn.gudsen.vip/simulation_game/rs21repository/installer/MOZA_SDK.zip) includes version **1.0.1.8**). Copy **`MOZA_SDK/1.0.1.8/MSVC2022-64/bin/MOZA_SDK.dll`** beside `apb.exe` / `APB.exe`. The DLL is supplied by MOZA, not this project. The supported interface is that SDK version's Windows x64 ABI; the optional reader also works with the LLVM-MinGW player build without linking MOZA's C++ import library.
3. Open F1 → Controls → Wheel and choose **MOZA physical angle (Windows SDK)** as the **Wheel input**. Select the base in **Wheel device** (or use **Find it** and turn the wheel). The device selector works even if the joystick axis is already clipped and cannot move. Binding the base also excludes its axes from gamepad defaults. SDK details and connection retry are under **Advanced wheel input**.
4. Check the connection status and **Physical angle** reading. Pit House/SDK startup can take a few seconds. Turn several revolutions each way: the physical angle must keep tracking beyond the ordinary joystick meter's endpoints. An unavailable angle leaves wheel steering at rest and reports the problem; it does not silently fall back to the clipped axis. **Retry MOZA connection** retries a missing DLL or reinitialises the SDK connection. A custom DLL location can be saved as `moza_sdk` in `apb.ini`.
5. Set sensitivity to taste, starting at 160 counts per turn. Bind siren/start and fire to the desired wheel buttons, and calibrate the gas pedal separately. Pedals can be on the base's device or a separate USB device.

This backend calls only SDK install, remove and HID-data reading. It copies `HIDData::fSteeringWheelAngle`, the first float in the documented structure; it does not pass STL objects or ownership across runtimes. It issues no motor movement, spring, torque-limit, steering-range or calibration commands. Force feedback continues through SDL's passive friction/damping path. An error, non-finite angle or reconnection rebases motion instead of reusing an old angle. An implausible physical-angle discontinuity (more than ten turns per game frame) is also rebased, as can happen if the driver recentres its turn counter.

## Calibration, pausing and force feedback

For a wrapping axis, **Measure** accumulates signed movement from individual events, including seam crossings. One complete turn can therefore be measured even if a continuous axis ends on the same reading where it started. MOZA physical degrees need no axis-range calibration. Closing the menu, changing configuration or reconnecting a wheel discards pending movement and starts from a fresh position; menu movements are not played back into the game.

The game initialises SDL haptics explicitly, requests no driver autocentre where available, and requests friction (or damping when friction is unavailable). The Controls page reports whether that effect actually started, including driver errors. The wheel's own software may add forces independently. Unsupported haptics leave steering operational. The runtime stops its effects and releases its haptics reference on exit.

## What has been verified

- Arithmetic and SDL virtual-device tests: wrapping-axis movement, nothing when held still; reversed and separate pedals; wrap crossings in both directions; several complete rotations between frames; wrap-aware measurement while paused; no menu/reconnection steering jump; settings persistence (an older limited-rotation setup falls back to the keys and gamepad) and haptics subsystem lifecycle.
- Physical-angle arithmetic: large turn counts, both directions, invalid readings and rebasing. On Windows, a test DLL with the official SDK export names exercises the real loader, angle input, SDK errors, reconnect and shutdown without proprietary files.
- **Real R5 hardware and force feel remain unverified.** The official SDK's exports and structure prefix have been inspected, but the mocked DLL is not a firmware compatibility test. In particular, R5 continuous tracking must be confirmed with the physical-angle meter on the user's actual Pit House/firmware combination. Native/reference gameplay parity checks do not test wheel hardware.

Vendor references: [MOZA Pit House manual](https://support.mozaracing.com/en/support/solutions/articles/70000625635-moza-pit-house-user-manual), [MOZA SDK](https://mozaracing.com/pages/sdk).


## Saved wheel setups and focus

Version 0.18 remembers each bound wheel's entire control setup in `apb.ini`, keyed by SDL GUID: angle scale, calibration, input source, pedal/button bindings and friction. Bind each wheel once, then use **Saved wheel setup** to switch explicitly. If the selected wheel is absent, a connected wheel with an existing profile is restored automatically. When several saved wheels are connected, the selected connected wheel takes priority; otherwise the first connected known wheel is used. Reconnecting another wheel does not interrupt the one already selected and connected. Display and ROM-folder settings remain shared.

Old flat settings remain supported. Device GUIDs distinguish wheels; identical units sharing a GUID share a profile. The MOZA physical-angle setup still requires the user-supplied SDK and hardware prerequisites above. Profiles do not install drivers or change Pit House rotation limits.

Losing window focus or minimising pauses game time, releases game controls, clears queued sound and suspends game haptic effects. Returning rebases the wheel instead of replaying background turns. A menu already open stays open. This is tested with virtual controls; physical R5 verification remains outstanding.


## Saving a controller report

Open **F1 → Controls**, turn the wheel several revolutions each way and press/release each pedal fully, then choose **Save controller diagnostics**. The notice gives the location of `controllers.txt`, beside `apb.ini`. Each export replaces that report atomically.

The report includes the application/SDL version and platform; device GUID, vendor/product, serial and path; each axis's current and observed minimum/maximum readings since connection; steering mode, scale and bindings; pedal calibration; actual haptic status; and optional MOZA physical-angle minimum/maximum observed during the session. It is a local text file and is not uploaded automatically. Serial numbers and device paths are included, so review them before sharing publicly.

For the R5, compare increasing physical-angle travel over several turns with the ordinary SDL axis: an axis reaching an endpoint alone does not distinguish a stop, a wrapping seam or clipping. Confirm the pedals span their calibrated range. A report aids diagnosis; it does not replace physical verification of steering and force feel.


The Windows **Check controls and performance.bat** launcher collects that controller report automatically on exit, together with an input movie, display/audio timing report and a checklist. It creates a new session folder under `Hardware checks`; run once per wheel setup, fill in the driver/firmware versions and physical observations, and return the resulting files for diagnosis. For source builds, use `--controller-report controllers.txt` alongside `--record` and `--profile`. This avoids manually exporting a report, but it still requires a real R5 run; no remote software check can confirm its mechanical behaviour or force feel.
