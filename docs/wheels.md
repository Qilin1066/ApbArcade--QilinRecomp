# Wheels: MOZA R5, Logitech G920 and continuous inputs

APB reads a free-spinning optical encoder, not the wheel's position relative to a centre. The runtime sends movement to the cabinet's wrapping LETA counter, with 160 counts per physical revolution by default. Turning back sends movement back; holding a continuous wheel still does nothing.

## Choose the input the driver actually supplies

| Input in the Controls page | Use | Behaviour |
|---|---|---|
| Limited axis | G920, or any wheel with a finite reported range | Differences in angle, scaled by the driver's rotation range. Optional held-at-lock steering lets a stopped wheel keep turning the cabinet dial. |
| Continuous wrapping axis | An encoder/driver that rolls from one end of its reported axis to the other | Accumulates each SDL motion event and unwraps the seam. No held-at-lock steering. Set the degrees represented by one complete axis cycle, or measure one turn. |
| MOZA physical angle | MOZA SDK on Windows x64, including the R5 | Reads the SDK's physical angle in degrees separately from its finite joystick axis. No finite range scaling or held-at-lock steering. |

Physical freedom to turn and the input available to software are different. Opening up a direct-drive base's stop does not prove its ordinary joystick axis reports unlimited movement. If the position meter stays at an endpoint while the wheel keeps turning, software cannot reconstruct those turns from that axis. Use the physical-angle source instead. Do not enable wrapping for an axis that clips or for the G920's ordinary axis: it would turn a large real movement in the opposite direction into a false seam crossing.

## MOZA R5 on Windows x64

1. Run the **SDK-enabled MOZA Pit House build required by MOZA's SDK documentation**; an ordinary Pit House installation is not proof that its SDK service is available. Obtain the appropriate build through MOZA's SDK/support channels if needed. Configure the base for the free rotation you want and turn off the driver's mechanical centring/spring for the cabinet feel. The game does not modify global Pit House settings or manufacture rotation beyond a driver-imposed stop.
2. Obtain the official [MOZA Racing SDK](https://mozaracing.com/pages/sdk) (the [download inspected for this implementation](https://cdn.gudsen.vip/simulation_game/rs21repository/installer/MOZA_SDK.zip) includes version **1.0.1.8**). Copy **`MOZA_SDK/1.0.1.8/MSVC2022-64/bin/MOZA_SDK.dll`** beside `apb.exe` / `APB.exe`. The DLL is supplied by MOZA, not this project. The supported interface is that SDK version's Windows x64 ABI; the optional reader also works with the LLVM-MinGW player build without linking MOZA's C++ import library.
3. Open F1 → Controls → Wheel and select **MOZA R5 (physical angle)**. Select the base in **Wheel device for force feedback** (or use **Find it** and turn the wheel). The device selector works even if the joystick axis is already clipped and cannot move. Binding the base also excludes its axes from gamepad defaults.
4. Check the connection status and **Physical angle** reading. Pit House/SDK startup can take a few seconds. Turn several revolutions each way: the physical angle must keep tracking beyond the ordinary joystick meter's endpoints. An unavailable angle leaves wheel steering at rest and reports the problem; it does not silently fall back to the clipped axis. **Retry MOZA connection** retries a missing DLL or reinitialises the SDK connection. A custom DLL location can be saved as `moza_sdk` in `apb.ini`.
5. Set sensitivity to taste, starting at 160 counts per turn. Bind siren/start and fire to the desired wheel buttons, and calibrate the gas pedal separately. Pedals can be on the base's device or a separate USB device.

This backend calls only SDK install, remove and HID-data reading. It copies `HIDData::fSteeringWheelAngle`, the first float in the documented structure; it does not pass STL objects or ownership across runtimes. It issues no motor movement, spring, torque-limit, steering-range or calibration commands. Force feedback continues through SDL's passive friction/damping path. An error, non-finite angle or reconnection rebases motion instead of reusing an old angle. An implausible physical-angle discontinuity (more than ten turns per game frame) is also rebased, as can happen if the driver recentres its turn counter.

## Logitech G920

1. Set the wheel's operating range to **900°** in Logitech G HUB and use a linear steering response. Disable the driver's centring spring for the free-wheel cabinet feel; the gears still have their physical resistance and endpoints.
2. F1 → Controls → Wheel → **Find it**, then **Logitech G920 (900 degrees)**. This selects a limited axis and enables held-at-lock steering near either endpoint.
3. With that option on, turn toward a stop and hold it to keep the cabinet dial turning; move off the stop to stop the added turning. The zone and rate are adjustable. This is an adaptation for a wheel with endpoints, not physically unlimited rotation.
4. Calibrate the accelerator with **Set up a pedal**: start released, press fully, release and press Done. Reversed axes and separate pedal devices are supported. Bind the cabinet actions to the desired wheel buttons.

## Calibration, pausing and force feedback

For SDL axis input, **Measure** accumulates signed movement from individual events, including seam crossings. One complete turn can therefore be measured even if a continuous axis ends on the same reading where it started. MOZA physical degrees need no axis-range calibration. Closing the menu, changing configuration or reconnecting a wheel discards pending movement and starts from a fresh position; menu movements are not played back into the game.

The game initialises SDL haptics explicitly, requests no driver autocentre where available, and requests friction (or damping when friction is unavailable). The Controls page reports whether that effect actually started, including driver errors. The wheel's own software may add forces independently. Unsupported haptics leave steering operational. The runtime stops its effects and releases its haptics reference on exit.

## What has been verified

- Arithmetic and SDL virtual-device tests: limited-wheel movement and held-at-lock behaviour; reversed and separate pedals; wrap crossings in both directions; several complete rotations between frames; wrap-aware measurement while paused; no menu/reconnection steering jump; settings persistence and haptics subsystem lifecycle.
- Physical-angle arithmetic: large turn counts, both directions, invalid readings and rebasing. On Windows, a test DLL with the official SDK export names exercises the real loader, angle input, SDK errors, reconnect and shutdown without proprietary files.
- **Real R5/G920 hardware and force feel remain unverified.** The official SDK's exports and structure prefix have been inspected, but the mocked DLL is not a firmware compatibility test. In particular, R5 continuous tracking must be confirmed with the physical-angle meter on the user's actual Pit House/firmware combination. Native/reference gameplay parity checks do not test wheel hardware.

Vendor references: [MOZA Pit House manual](https://support.mozaracing.com/en/support/solutions/articles/70000625635-moza-pit-house-user-manual), [MOZA SDK](https://mozaracing.com/pages/sdk), [G920 specifications](https://support.logi.com/hc/en-nz/articles/360023304114-G920-Driving-Force-Racing-Wheel-Technical-Specifications).
