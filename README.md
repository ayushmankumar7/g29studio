<div align="center">

# 🏎️ G29 Studio

### Drive **any** game with your racing wheel — even the ones that don't support wheels.

**Tested driving GTA V.**

[![License: MIT](https://img.shields.io/badge/License-MIT-00C853.svg?style=for-the-badge)](LICENSE)
[![Platform](https://img.shields.io/badge/Windows-10%20%7C%2011-0078D6.svg?style=for-the-badge&logo=windows&logoColor=white)](#)
[![Built with](https://img.shields.io/badge/C%2B%2B-20-00599C.svg?style=for-the-badge&logo=cplusplus&logoColor=white)](#)
[![Dependencies](https://img.shields.io/badge/Runtime%20deps-0-brightgreen.svg?style=for-the-badge)](#)
[![PRs Welcome](https://img.shields.io/badge/PRs-welcome-ff69b4.svg?style=for-the-badge)](#-lets-build-this-together)

**[Quick Start](#-quick-start-5-minutes) · [Fast Setup Profile](#-fast-setup-use-my-profile) · [Build](#-build-from-source) · [How It Works](#-how-it-works) · [Contribute](#-lets-build-this-together)**

</div>

---

## The problem every wheel owner knows

You bought a £250 force-feedback wheel. Then you find out your favourite game only supports a gamepad.

GTA V. Watch Dogs. Cyberpunk. Mafia. Sleeping Dogs. Saints Row. Half the open-world games ever made have cars you'd love to actually *drive* — and not one of them knows your wheel exists. The pedals do nothing. The shifter does nothing. The wheel sits there as an expensive paperweight while you steer with a thumbstick.

## The fix

**G29 Studio makes your wheel look like an Xbox 360 controller.**

Not a hack. Not an injector. Not a mod. The game sees a plain, boring, completely ordinary controller — and you drive it with a wheel, pedals, clutch and H-pattern shifter.

```
   ┌──────────────┐        ┌──────────────┐        ┌──────────────┐
   │  Your wheel  │        │  G29 Studio  │        │   ANY game   │
   │              │ ─────► │              │ ─────► │              │
   │  🎡 Wheel    │        │  Calibrate   │        │  Sees a      │
   │  🦶 Pedals   │        │  Shape       │        │  standard    │
   │  🎚️ Shifter  │        │  Translate   │        │  Xbox pad    │
   └──────────────┘        └──────────────┘        └──────────────┘
          ▲                                               │
          └───────────── force feedback ◄─────────────────┘
```

Because the game only ever sees a controller:

- ✅ **No game files touched.** Nothing injected, nothing patched, nothing modded.
- ✅ **Anti-cheat safe by design.** We never go near the game process.
- ✅ **Works in every game that supports a gamepad.** Which is all of them.
- ✅ **Nothing to configure in-game.** Pick "Controller" and drive.

---

## 🎯 What makes this different

Plenty of tools map a wheel onto a virtual pad. This one actually cares how it *feels*.

### Your H-pattern shifter genuinely works

GTA V has no gears. It has one pedal that means "go". So G29 Studio **simulates the gearbox itself** — and shapes the throttle it sends to the game per gear.

Grab 2nd and the car pulls hard. Grab 6th at 20 km/h and it bogs down exactly like it should. Let the clutch out too fast and you stall. The game has no idea any of this is happening; it just receives a throttle value that behaves like a real drivetrain.

**Six gears, reverse, a clutch with an adjustable bite point, and stalling.** In a game that thinks gearboxes don't exist.

### Force feedback that's been *measured*, not guessed

Here's something nobody tells you: **your wheel lies to you.** Ask a G29 for 10% force and it gives you nothing at all — the gear train's internal friction swallows it. The relationship between what you ask for and what you feel is a bent curve with a dead hole in the middle.

G29 Studio **measures your specific wheel** and corrects for it.

It applies a known force, times how long the wheel takes to travel a short fixed distance, and derives real torque from `s = at²/2`. Repeat across the whole force range, invert the result, and you get a lookup table that linearises your hardware.

On the test unit, the measured dead zone ran to **16% of full force** — every bit of fine detail below that was being thrown away before this existed:

```
requested force  →  torque actually produced
         0.04    →   0.0000      ← nothing
         0.08    →   0.0000      ← nothing
         0.12    →   0.0000      ← nothing
         0.17    →   0.0558      ← finally, movement
         0.42    →   0.6306
         0.88    →   1.0000      ← saturated before full scale
```

The correction curve exports as a standard **Assetto Corsa `.lut`**, so you can use it in other sims too.

### A test drive that tells the truth

Tuning force feedback by repeatedly launching a game is miserable. So there's a vehicle simulation built in — a real tyre model with a saturating slip curve.

It exists because of an honest limitation: **an unmodified game publishes no telemetry.** No slip angle, no tyre load, no surface material. GTA V publishes nothing at all.

Inside the test drive we own the physics, so the force feedback is driven by genuine slip angle and load — including the part that matters most: **force peaks just before the front tyres let go, then falls away.** That's how a driver feels understeer arriving. Pneumatic trail collapsing as the tyre saturates. Modelled properly.

Tune until it feels right here, then go drive.

### Nothing gets installed

DirectInput and Direct3D 11 ship with Windows. Dear ImGui compiles into the `.exe`. The result is **one 570 KB executable**.

Delete the folder and nothing remains on your system. No service, no registry keys, no background updater, no account.

---

## 📋 Features

| Tab | What it does |
|:---|:---|
| **Device** | Finds every DirectInput controller, flags force-feedback capability, live axis + button readout |
| **Calibration** | Auto-assigns axes, captures real pedal travel, sets steering centre, deadzone and response curve |
| **Gearbox** | Learn each shifter position, set per-gear ratios, clutch bite point, stalling |
| **Force Feedback** | Master gain, self-aligning torque, surface detail, min force, spring, damper — plus the measured-response calibration |
| **Game Output** | Creates the virtual Xbox 360 pad, live view of exactly what the game is receiving |
| **Test Drive** | Real vehicle physics so you can feel and tune FFB without launching anything |

---

## 🚀 Quick Start (5 minutes)

### Step 1 — Install the virtual controller driver

This is the only install, and it's a one-time thing. Windows has no user-mode way to create a virtual gamepad, so this needs a driver. [ViGEmBus](https://github.com/nefarius/ViGEmBus) is the standard, open-source one:

```powershell
winget install --id ViGEm.ViGEmBus
```

Then **reboot**. (Windows loads the bus driver at boot.)

### Step 2 — Get G29 Studio

Grab `g29studio.exe` from [Releases](https://github.com/ayushmankumar7/g29studio/releases), or [build it yourself](#-build-from-source) in about a minute.

Put it in its own folder — it writes its profile and force curve next to itself.

### Step 3 — Plug in your wheel and launch

```
g29studio.exe
```

1. **Device** tab → **Scan for devices**
2. Select your wheel (it should show `[force feedback]`) → **Connect**
3. **Game Output** tab → **Start game output**

That's it. A virtual Xbox 360 controller now exists, driven by your wheel.

### Step 4 — Launch your game

Set the game's input to **Controller / Gamepad**. Drive.

> ⚠️ **Leave G29 Studio running.** It owns the wheel and feeds the pad. If the game *also* reads your real wheel directly you'll get doubled steering — make sure the game is set to use a controller, not a wheel.

---

## ⚡ Fast Setup: use my profile

Calibrating from scratch is fine, but you don't have to. **A fully tuned, hardware-measured profile ships in this repo.**

Copy both files into the same folder as `g29studio.exe`:

```powershell
# from the repo root, assuming the exe is in build\Release
copy profiles\g29studio_profile.ini  build\Release\
copy profiles\g29studio.lut          build\Release\
```

Launch, connect your wheel, and hit **Load profile** at the bottom of the window. Everything comes up configured:

| Setting | Value | Why |
|:---|:---|:---|
| Steering range | 900° | Full G29 lock |
| Steering gamma | 0.40 | Sharper off-centre — open-world games need quick response |
| Bridge steer scale | 0.90 | Tamed slightly so GTA V's own steering assist doesn't overshoot |
| Master gain | 1.10 | Firm but not fighting you |
| Centring spring | 0.45 | Replaces the wheel's autocentre, which fights applied forces |
| Surface detail | 0.80 | Kerbs and texture you can actually feel |
| Rumble → wheel | 0.71 | Game collisions come through the wheel, not overwhelming |
| Force curve | 25-point LUT | **Measured on real G29 hardware** |
| Gearbox | 6-speed + reverse | Shifter buttons already mapped (12–18) |

**What's portable, and what isn't:**

| | |
|:---|:---|
| ✅ Gains, ratios, gammas, bridge settings | Good starting point for **any** wheel |
| ✅ The `.lut` force curve | Accurate for **any G29** — same gear train, same stiction |
| ⚠️ Shifter button indices (12–18) | G29 + stock shifter. Re-learn them if yours differs |
| ⚠️ Axis offsets (0/4/20/24) | Hit **Re-detect automatically** on the Calibration tab for other wheels |

**On a different wheel?** Load the profile anyway for the tuning, then:
1. **Calibration** → **Re-detect automatically**
2. **Force Feedback** → **Start calibration** to measure *your* hardware (takes ~1 minute, the wheel moves on its own — let go of it)

---

## 🔧 Full setup walkthrough

Want it dialled in properly? Fifteen minutes, once.

<details>
<summary><b>1. Calibration tab — teach it your hardware</b></summary>

Roles are assigned automatically when you connect, but **capture pedal travel anyway.** Pedals rest at one end and almost never reach their electrical extremes — real travel matters far more than it sounds.

For each pedal: press it fully, release it fully, watch the live bar fill the whole range.

For steering: centre the wheel, click **Set centre**.

- **Deadzone** — removes jitter at rest. 0.02–0.03 is plenty.
- **Gamma** — `1.0` is linear. Below 1 is sharper off-centre (good for arcade games with lazy steering); above 1 is softer.

> 💡 Axis names come from the driver and are generic. On a G29 the clutch usually reports as `Y Axis`. Use the live bars to confirm before assigning — wiggle the thing and watch which bar moves.

</details>

<details>
<summary><b>2. Gearbox tab — make the shifter real</b></summary>

For each position: click **Learn**, then move the shifter into that gear. Do reverse too.

- **Gear ratios** — the fraction of top speed allowed in that gear. This is the knob that makes shifting feel meaningful; lower in 1st and 2nd, 1.00 in top.
- **Clutch bite** — where in the pedal travel drive engages. 0.45 is a natural feel.
- **Stalling** — on by default. Dump the clutch in 4th and you've stalled it.

</details>

<details>
<summary><b>3. Force Feedback tab — measure, then tune</b></summary>

**Run the calibration first.** Click **Start calibration**, then let go of the wheel. It moves on its own for about a minute while it measures your hardware's real force response, then writes `g29studio.lut`.

Then tune:

- **Master gain** — overall strength.
- **Self-aligning torque** — the main cornering force. The one that carries feel.
- **Surface detail** — kerbs, texture, road grain.
- **Minimum force** — lifts tiny forces above the G29's stiction. Raise it until small inputs are *felt*, then stop. (If you've run the calibration, the LUT mostly handles this.)
- **Centring spring** — handled by the wheel's own controller. Autocentre is switched off automatically on connect because it fights applied forces; this slider replaces it.
- **Damper** — takes the edge off oscillation.

</details>

<details>
<summary><b>4. Test Drive tab — feel it before you launch anything</b></summary>

Hit **Start** and drive. Real slip angle, real tyre load, real force feedback.

Push until the front tyres let go — you should feel the wheel go light just *before* it slides. If you can't feel that moment, raise self-aligning torque and surface detail.

When it feels right, **Save profile** and go play.

</details>

---

## ⌨️ Headless mode (no window)

Every path works from a terminal — handy for launching alongside a game, scripting, or diagnosing hardware.

```powershell
# Run the wheel-to-pad bridge with no UI, logs to g29studio_bridge.txt
g29studio.exe --bridge --seconds 300

# Measure force response, write an Assetto Corsa .lut
g29studio.exe --calibrate --steps 24 --reps 2 --out g29studio.lut

# Dump every axis and button the device reports
g29studio.exe --diag --seconds 20

# Self test: enumeration, profile round trip, physics stability, force range
g29studio.exe --selftest
```

`--bridge` is genuinely useful: start it, launch your game, drive. No window in the way.

---

## 🛠️ Build from source

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Output: `build\Release\g29studio.exe`

**Requirements:** Visual Studio 2022 with the Windows SDK. That's the whole list.

No vcpkg. No DirectX SDK. No `git submodule update`. Dear ImGui and ViGEmClient are vendored in `extern/`, DirectInput and D3D11 come from the Windows SDK. It compiles clean on a fresh machine.

---

## 🧠 How it works

```
 src/wheel.cpp      DirectInput: enumeration, polling, FFB effects
 src/profile.cpp    Calibration + tuning, plain key=value .ini
 src/pad.cpp        ViGEmBus virtual Xbox 360 pad, and rumble coming back
 src/ffbcal.cpp     Force-response measurement → inverted LUT
 src/testdrive.cpp  Vehicle physics with a saturating tyre model
 src/main.cpp       ImGui interface + the headless CLI modes
```

**Two details worth knowing**, because they cost real debugging time:

**The G29 driver reports the wrong axis offsets.** It claims `4/8/12/16` in `DIDEVICEOBJECTINSTANCE::dwOfs`, but the data actually lands in `lX`, `lY`, `lRz` and `slider[0]`. Trust `dwOfs` and you read three permanently-static fields and conclude the pedals are broken. G29 Studio derives offsets from the axis **type GUID** instead.

**Force calibration times a fixed distance, not a fixed time.** The obvious approach — apply force, measure displacement after 200 ms — fails completely on a geared wheel. A G29 leaves its acceleration phase almost immediately and hits a velocity limit, after which displacement stops tracking torque and the measured curve flattens into nonsense. Timing a *short, fixed* distance keeps you inside the acceleration phase where `torque ∝ 1/t²` actually holds.

---

## ⚖️ Honest limits

No overselling. Here's exactly what you get and what you don't.

**Force feedback in an unmodified game is limited to rumble.** XInput's entire feedback channel is two bytes: large motor and small motor. A game can tell us "something is shaking" and nothing more. That's enough for collisions and kerbs, and nowhere near enough for slip angle or tyre load. **No tool can get real telemetry out of a game that doesn't publish it** — and GTA V publishes none. The rich, physics-driven force feedback lives in the Test Drive tab, where we own the physics.

**You still need one driver.** ViGEmBus. Windows provides no user-mode API for creating a virtual gamepad; this is a hard OS limitation, not a design choice.

**Force feedback is exclusive-acquire on Windows.** Only one process can hold a device's effects at a time. If **Connect** fails, close whatever else has the wheel — a game, G HUB's tuning page, or a second copy of this app.

**Other gaps, honestly:**
- No synchro or rev-matching model. Select a gear miles off the right speed and it simply bogs or over-revs.
- Only tested on a G29 so far. It's built on generic DirectInput and *should* work on a G920, T300, Fanatec or anything else that enumerates — but nobody's confirmed it. **[Tell us if you try it.](#-lets-build-this-together)**

---

## 📊 Test status

| Path | Status |
|:---|:---|
| Build (MSVC 19.44, x64 Release) | ✅ passes |
| GUI launch, render, clean shutdown | ✅ verified |
| Device enumeration | ✅ `Logitech G HUB G29 Driving Force Racing Wheel USB` |
| Axis calibration + live input | ✅ verified on hardware |
| Force response calibration | ✅ 25-point curve measured, dead zone found at 16% |
| Virtual pad creation + teardown | ✅ created, driven, removed cleanly |
| Steering / throttle / brake → pad | ✅ full −1.000 to +1.000 sweep, triggers 0–255 |
| H-pattern shifter → simulated gears | ✅ gear changes tracked live |
| **Driving GTA V** | ✅ **works** |
| Physics stability | ✅ 54 km/h in 1st, 7196 rpm at the limiter, all values finite |
| Profile save / load round trip | ✅ verified |
| Wheels other than the G29 | ⬜ untested — help wanted |

---

## 🤝 Let's build this together

**This project is young, it's MIT licensed, and it needs people.** If you own a wheel and you've ever been annoyed that a game ignored it, you're exactly who this is for.

I built this to drive GTA V with a G29. The approach is general — **it should work with any wheel, in any game** — but one person with one wheel can only verify so much. That's where you come in.

### 🎁 The single most useful thing you can do

**Try it on your wheel and tell us what happened.** Working or broken, both are valuable.

```powershell
g29studio.exe --diag --seconds 20
```

[Open an issue](https://github.com/ayushmankumar7/g29studio/issues/new) with `g29studio_diag.txt` attached and your wheel's model. That file alone is often enough to add support for a device I'll never own. **G920, T300, T150, Fanatec, Thrustmaster, Moza, Simagic — all wanted.**

And if you tune a profile you like, **send it in.** A library of per-wheel, per-game profiles would make this instantly useful to people who don't want to calibrate anything.

### 🗺️ Where this is going

Pick anything. Issues are open, and "I'd like to try this" is a perfectly good comment to leave.

| | Idea | Difficulty |
|:---|:---|:---|
| 🎮 | **Per-game profiles** — auto-switch tuning when a game launches | 🟢 Approachable |
| 🖼️ | **Overlay** — gear, revs and FFB load on screen while you drive | 🟢 Approachable |
| 🎛️ | **DualSense / DS4 output** — not just Xbox 360 | 🟢 Approachable |
| 🔊 | **Engine audio from the simulated gearbox** — hear your gears | 🟡 Moderate |
| 📈 | **Telemetry adapters** — real FFB for games that *do* publish data (Assetto Corsa, BeamNG, iRacing, F1, Forza) | 🟡 Moderate |
| 🤖 | **Smarter gearbox** — synchro, rev-matching, heel-toe | 🟡 Moderate |
| 🧪 | **Speed estimation from screen or memory** — better gearbox behaviour in games with no telemetry | 🔴 Hard |
| 🔌 | **Plugin API** — let people write their own FFB models | 🔴 Hard |
| 🐧 | **Linux port** — evdev + uinput instead of DirectInput + ViGEm | 🔴 Hard |

### Contributing

1. Fork it, branch from `main`
2. Match the surrounding style — C++20, 4-space indent, comments that explain *why* rather than *what*
3. Run `g29studio.exe --selftest` and check `g29studio_selftest.txt` before opening a PR
4. If you touched a hardware path, say what you tested it on

**Issues, questions and "this didn't work on my wheel" reports are all genuinely welcome.** You don't need to write C++ to help — a diagnostic file, a working profile, or clearer instructions all move this forward.

⭐ **If this got your wheel working in a game that ignored it, star the repo.** It's how other wheel owners find this.

---

## 🙏 Built on

- **[Dear ImGui](https://github.com/ocornut/imgui)** `v1.91.5` by Omar Cornut — the interface. ([MIT](extern/imgui/LICENSE.txt))
- **[ViGEmClient / ViGEmBus](https://github.com/nefarius/ViGEmBus)** by Nefarius Software Solutions — virtual gamepads on Windows, and the reason any of this is possible. ([MIT](extern/ViGEmClient/LICENSE))

Both are vendored in [extern/](extern/) with their licenses intact, so a clone builds with no package manager and nothing to fetch.

---

## 📄 License

[MIT](LICENSE) — do what you like with it. Commercial use, modification, redistribution, all fine.

Not affiliated with Logitech, Rockstar Games, or anyone else. "G29" is used descriptively to say what hardware this was built and tested on.

<div align="center">

---

**Made because a £250 wheel shouldn't be a paperweight.**

[Report a bug](https://github.com/ayushmankumar7/g29studio/issues) · [Share your profile](https://github.com/ayushmankumar7/g29studio/issues/new) · [Star the repo ⭐](https://github.com/ayushmankumar7/g29studio)

</div>
