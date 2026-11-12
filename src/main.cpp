// g29studio -- detect, calibrate, tune and test a Logitech G29 (and any other
// DirectInput wheel) using nothing but what ships with Windows.
//
// No drivers are installed. DirectInput and Direct3D 11 are part of the
// Windows SDK, and Dear ImGui is compiled straight into this executable.

#include "wheel.h"
#include "profile.h"
#include "testdrive.h"
#include "ffbcal.h"
#include "pad.h"

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

#include <d3d11.h>
#include <shellapi.h>   // CommandLineToArgvW
#include <tchar.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using clk = std::chrono::steady_clock;

static long long msSince(clk::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(clk::now() - t0).count();
}
static void sleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// ------------------------------------------------------------- D3D ----------

static ID3D11Device*           g_pd3dDevice        = nullptr;
static ID3D11DeviceContext*    g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*         g_pSwapChain        = nullptr;
static ID3D11RenderTargetView* g_mainRTV           = nullptr;
static bool                    g_swapChainOccluded = false;
static UINT                    g_resizeWidth = 0, g_resizeHeight = 0;

static void CreateRenderTarget() {
    ID3D11Texture2D* back = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) {
        g_pd3dDevice->CreateRenderTargetView(back, nullptr, &g_mainRTV);
        back->Release();
    }
}

static void CleanupRenderTarget() {
    if (g_mainRTV) { g_mainRTV->Release(); g_mainRTV = nullptr; }
}

static bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount       = 2;
    sd.BufferDesc.Width  = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator   = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags        = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage  = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed     = TRUE;
    sd.SwapEffect   = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
        D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &got,
        &g_pd3dDeviceContext);
    if (hr == DXGI_ERROR_UNSUPPORTED)
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
            D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &got,
            &g_pd3dDeviceContext);
    if (FAILED(hr)) return false;

    CreateRenderTarget();
    return true;
}

static void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain)        { g_pSwapChain->Release();        g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice)        { g_pd3dDevice->Release();        g_pd3dDevice = nullptr; }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) return 0;
            g_resizeWidth  = (UINT)LOWORD(lParam);
            g_resizeHeight = (UINT)HIWORD(lParam);
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ------------------------------------------------------- app state ----------

struct App {
    Wheel     wheel;
    Profile   profile;
    TestDrive sim;

    std::vector<DeviceDesc> devices;
    int         selected = -1;
    std::string status   = "Scan for devices to begin.";

    bool ffbEnabled = false;
    bool simRunning = false;

    // Calibration capture
    int  capturing  = -1;      // index into axisSlots
    int  learnGear  = -1;      // gear position awaiting a button press
    bool prevButtons[128]{};

    float manualForce = 0.0f;  // FFB test slider

    FfbCalibrator        cal;
    FfbCalibrator::Params calParams;

    VirtualPad pad;
    float      rumblePhase = 0.0f;
    // Last values actually sent to the game, for the UI readout.
    float outSteer = 0.0f;
    int   outLT = 0, outRT = 0, outGear = 0;
    unsigned short outButtons = 0;

    std::string profilePath = "g29studio_profile.ini";
};

static const char* kAxisSlotNames[4] = { "Steering", "Throttle", "Brake", "Clutch" };

static AxisCal* axisSlot(Profile& p, int i) {
    switch (i) {
        case 0: return &p.steering;
        case 1: return &p.throttle;
        case 2: return &p.brake;
        case 3: return &p.clutch;
    }
    return nullptr;
}

static std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                      nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n,
                        nullptr, nullptr);
    return s;
}

// Assigns the four roles straight from the driver's axis names, and works out
// which way each pedal travels from where it sits at rest. Logitech pedals read
// maximum when released and fall as they are pressed, so they need inverting --
// detecting that is more reliable than asking someone to guess.
static void autoAssign(Wheel& wheel, Profile& p) {
    if (!wheel.isOpen()) return;

    // DirectInput hands back a centred placeholder (every axis at 32768) until
    // the device sends its first real report. Deciding pedal direction from
    // that placeholder reads exactly 0.5 and concludes "not inverted", which
    // leaves every pedal backwards. Let the device settle first.
    for (int i = 0; i < 15; ++i) {
        wheel.poll();
        sleepMs(20);
    }
    const DIJOYSTATE2& st = wheel.state();

    auto lower = [](std::wstring s) {
        for (auto& c : s) if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
        return s;
    };
    auto has = [](const std::wstring& h, const wchar_t* n) {
        return h.find(n) != std::wstring::npos;
    };

    auto bind = [&](AxisCal* c, const AxisInfo& a, bool pedal) {
        c->offset    = a.offset;
        c->rawMin    = a.min;
        c->rawMax    = a.max;
        c->rawCenter = (a.min + a.max) / 2;
        if (pedal) {
            const LONG raw = Wheel::axisRaw(st, a.offset);
            const float frac = (float)(raw - a.min) /
                               std::max(1.0f, (float)(a.max - a.min));
            // Resting high means pressing drives the value down. An idle device
            // reports a placeholder half-scale value and never updates until
            // something moves, so an ambiguous reading falls back to the
            // Logitech convention (pedals rest at maximum). Capturing travel
            // settles it for certain.
            if      (frac > 0.6f) c->invert = true;
            else if (frac < 0.4f) c->invert = false;
            else                  c->invert = true;
            c->deadzone = 0.03f;
        } else {
            c->invert    = false;
            c->deadzone  = 0.02f;
            c->rawCenter = Wheel::axisRaw(st, a.offset);  // assume centred now
        }
    };

    p.steering.offset = p.throttle.offset = kUnassigned;
    p.brake.offset    = p.clutch.offset   = kUnassigned;

    const AxisInfo* generic = nullptr;
    for (const auto& a : wheel.axes()) {
        const std::wstring n = lower(a.name);
        if      (has(n, L"wheel") || has(n, L"steer"))   bind(&p.steering, a, false);
        else if (has(n, L"accel") || has(n, L"throttle") ||
                 has(n, L"gas"))                         bind(&p.throttle, a, true);
        else if (has(n, L"brake"))                       bind(&p.brake,    a, true);
        else if (has(n, L"clutch"))                      bind(&p.clutch,   a, true);
        else if (!generic)                               generic = &a;
    }
    // The G29 reports its clutch under a generic name ("Y Axis"); if the clutch
    // is still unassigned, the leftover unnamed axis is it.
    if (!p.clutch.assigned() && generic) bind(&p.clutch, *generic, true);
}

// Current gear from the shifter buttons, or 0 (neutral) when none are held.
static int readGear(const GearboxCfg& gb, const DIJOYSTATE2& st) {
    for (int i = 0; i <= gb.gearCount; ++i) {
        const int b = gb.gearButton[i];
        if (b >= 0 && b < 128 && (st.rgbButtons[b] & 0x80))
            return (i == 0) ? -1 : i;
    }
    return 0;
}

// ----------------------------------------------------------- UI -------------

static void drawDeviceTab(App& app) {
    if (ImGui::Button("Scan for devices")) {
        app.devices = listDevices();
        app.selected = -1;
        for (size_t i = 0; i < app.devices.size(); ++i)
            if (app.devices[i].hasFFB) { app.selected = (int)i; break; }
        app.status = app.devices.empty()
            ? "No controllers found. Check the wheel is plugged in, powered, and set to PC mode."
            : "Found " + std::to_string(app.devices.size()) + " device(s).";
    }
    ImGui::SameLine();
    if (ImGui::Button("Connect") && app.selected >= 0 &&
        app.selected < (int)app.devices.size()) {
        std::string err;
        if (app.wheel.open(app.devices[app.selected].instanceGuid, true, err)) {
            autoAssign(app.wheel, app.profile);
            app.status = "Connected to " + wideToUtf8(app.wheel.productName()) +
                         " - axes assigned automatically.";
            std::string ferr;
            if (app.wheel.hasFFB() && !app.wheel.ffbInit(ferr))
                app.status += "  (force feedback unavailable: " + ferr + ")";
        } else {
            app.status = "Connect failed: " + err;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Disconnect")) {
        app.wheel.close();
        app.ffbEnabled = false;
        app.status = "Disconnected.";
    }

    ImGui::Separator();
    for (size_t i = 0; i < app.devices.size(); ++i) {
        const auto& d = app.devices[i];
        const bool sel = ((int)i == app.selected);
        std::string label = wideToUtf8(d.productName) +
                            (d.hasFFB ? "   [force feedback]" : "   [no FFB]");
        if (ImGui::Selectable(label.c_str(), sel)) app.selected = (int)i;
    }

    if (app.wheel.isOpen()) {
        ImGui::SeparatorText("Connected device");
        ImGui::Text("%s", wideToUtf8(app.wheel.productName()).c_str());
        ImGui::Text("Buttons: %d     Axes: %d     Force feedback: %s",
                    app.wheel.buttonCount(), (int)app.wheel.axes().size(),
                    app.wheel.hasFFB() ? "yes" : "no");

        ImGui::SeparatorText("Live input");
        const auto& st = app.wheel.state();
        for (const auto& a : app.wheel.axes()) {
            const LONG raw = Wheel::axisRaw(st, a.offset);
            const float n = (float)(raw - a.min) / std::max(1.0f, (float)(a.max - a.min));
            ImGui::ProgressBar(n, ImVec2(220, 0));
            ImGui::SameLine();
            ImGui::Text("%-12s raw %5ld", wideToUtf8(a.name).c_str(), raw);
        }
        ImGui::Spacing();
        ImGui::Text("Buttons held:");
        ImGui::SameLine();
        bool any = false;
        for (int b = 0; b < app.wheel.buttonCount() && b < 64; ++b)
            if (st.rgbButtons[b] & 0x80) { ImGui::Text("%d ", b); ImGui::SameLine(); any = true; }
        if (!any) ImGui::TextDisabled("none");
        ImGui::NewLine();
    }
}

static void drawCalibrationTab(App& app) {
    if (!app.wheel.isOpen()) {
        ImGui::TextDisabled("Connect a device first.");
        return;
    }
    const auto& st = app.wheel.state();
    const auto& axes = app.wheel.axes();

    ImGui::TextWrapped(
        "Roles are assigned automatically on connect. Capture travel for each "
        "pedal anyway: they rest at one end and rarely reach the extremes, so "
        "real travel matters more than it sounds.");
    if (ImGui::Button("Re-detect automatically")) {
        autoAssign(app.wheel, app.profile);
        app.status = "Axes reassigned from the device's own axis names.";
    }
    ImGui::Separator();

    for (int i = 0; i < 4; ++i) {
        AxisCal* cal = axisSlot(app.profile, i);
        ImGui::PushID(i);
        ImGui::SeparatorText(kAxisSlotNames[i]);

        // Axis chooser
        std::string preview = "(unassigned)";
        for (const auto& a : axes)
            if (a.offset == cal->offset) preview = wideToUtf8(a.name);
        if (ImGui::BeginCombo("Axis", preview.c_str())) {
            if (ImGui::Selectable("(unassigned)", !cal->assigned()))
                cal->offset = kUnassigned;
            for (const auto& a : axes) {
                const bool sel = (a.offset == cal->offset);
                if (ImGui::Selectable(wideToUtf8(a.name).c_str(), sel)) {
                    cal->offset = a.offset;
                    cal->rawMin = a.min;
                    cal->rawMax = a.max;
                    cal->rawCenter = (a.min + a.max) / 2;
                }
            }
            ImGui::EndCombo();
        }

        if (cal->assigned()) {
            const LONG raw = Wheel::axisRaw(st, cal->offset);
            const float shown = (i == 0) ? cal->bipolar(raw) : cal->unipolar(raw);

            if (i == 0) {
                ImGui::ProgressBar(shown * 0.5f + 0.5f, ImVec2(260, 0));
                ImGui::SameLine();
                ImGui::Text("%+.3f   raw %ld", shown, raw);
            } else {
                ImGui::ProgressBar(shown, ImVec2(260, 0));
                ImGui::SameLine();
                ImGui::Text("%.3f   raw %ld", shown, raw);
            }

            const bool busy = (app.capturing == i);
            if (ImGui::Button(busy ? "Stop capture" : "Capture travel")) {
                if (busy) {
                    app.capturing = -1;
                    // The pedal has just been released, so where it sits now
                    // tells us which way it travels -- far more reliable than
                    // guessing at connect time.
                    if (i != 0 && cal->rawMax > cal->rawMin) {
                        const float frac = (float)(raw - cal->rawMin) /
                                           (float)(cal->rawMax - cal->rawMin);
                        cal->invert = frac > 0.5f;
                        app.status = std::string(kAxisSlotNames[i]) + " captured: " +
                                     std::to_string(cal->rawMin) + ".." +
                                     std::to_string(cal->rawMax) +
                                     (cal->invert ? ", inverted." : ".");
                    } else {
                        app.status = "Capture finished.";
                    }
                } else {
                    app.capturing = i;
                    cal->rawMin = raw;
                    cal->rawMax = raw;
                    app.status = std::string("Press ") + kAxisSlotNames[i] +
                                 " fully a few times, release it, then press Stop capture.";
                }
            }
            if (i == 0) {
                ImGui::SameLine();
                if (ImGui::Button("Set centre")) {
                    cal->rawCenter = raw;
                    app.status = "Steering centre captured.";
                }
            }
            ImGui::SameLine();
            ImGui::Checkbox("Invert", &cal->invert);

            ImGui::SliderFloat("Deadzone", &cal->deadzone, 0.0f, 0.30f, "%.3f");
            ImGui::SliderFloat("Curve (gamma)", &cal->gamma, 0.40f, 2.50f, "%.2f");
            ImGui::Text("Range: %ld .. %ld", cal->rawMin, cal->rawMax);
        }
        ImGui::PopID();
    }

    ImGui::Separator();
    ImGui::SliderInt("Steering range (degrees)", &app.profile.steeringRangeDeg, 180, 1080);
}

static void drawGearboxTab(App& app) {
    auto& gb = app.profile.gearbox;
    ImGui::TextWrapped(
        "Most games have no gearbox at all, so this one is simulated here: the "
        "clutch and gear selection shape the throttle that leaves this app. "
        "Nothing is asked of the game.");
    ImGui::Separator();

    ImGui::Checkbox("Enable simulated gearbox", &gb.enabled);
    ImGui::SliderInt("Forward gears", &gb.gearCount, 4, 6);
    ImGui::Checkbox("Engine can stall", &gb.stallEnabled);
    ImGui::SameLine();
    ImGui::Checkbox("Neutral cuts drive", &gb.neutralCutsThrottle);
    ImGui::SliderFloat("Clutch bite point", &gb.clutchBite, 0.05f, 0.95f, "%.2f");

    ImGui::SeparatorText("Shifter buttons");
    ImGui::TextDisabled("Press Learn, then move the shifter into that position.");

    const auto& st = app.wheel.state();
    for (int i = 0; i <= gb.gearCount; ++i) {
        ImGui::PushID(100 + i);
        const char* label = (i == 0) ? "Reverse" : nullptr;
        char buf[16];
        if (!label) { snprintf(buf, sizeof(buf), "Gear %d", i); label = buf; }

        ImGui::Text("%-9s", label);
        ImGui::SameLine();
        if (gb.gearButton[i] >= 0) ImGui::Text("button %2d", gb.gearButton[i]);
        else                       ImGui::TextDisabled("unassigned");
        ImGui::SameLine();
        if (ImGui::Button(app.learnGear == i ? "Waiting..." : "Learn"))
            app.learnGear = (app.learnGear == i) ? -1 : i;
        ImGui::SameLine();
        if (ImGui::Button("Clear")) gb.gearButton[i] = -1;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160);
        ImGui::SliderFloat("ratio", &gb.gearRatio[i], 0.08f, 1.0f, "%.2f");
        ImGui::PopID();
    }

    ImGui::Separator();
    ImGui::Text("Current gear: %d", readGear(gb, st));
}

static void drawFfbTab(App& app) {
    auto& f = app.profile.ffb;
    if (!app.wheel.isOpen() || !app.wheel.ffbReady()) {
        ImGui::TextDisabled("Connect a force-feedback device first.");
        return;
    }

    if (ImGui::Checkbox("Force feedback active", &app.ffbEnabled)) {
        if (!app.ffbEnabled) app.wheel.stopAllForces();
    }
    ImGui::Separator();

    ImGui::SliderFloat("Master gain",   &f.masterGain, 0.0f, 2.0f, "%.2f");
    ImGui::SliderFloat("Self-aligning torque", &f.satScale, 0.0f, 2.0f, "%.2f");
    ImGui::SliderFloat("Surface detail", &f.detailGain, 0.0f, 2.0f, "%.2f");
    ImGui::SliderFloat("Minimum force",  &f.minForce, 0.0f, 0.35f, "%.3f");
    ImGui::TextDisabled("Minimum force lifts small detail over the gear train's stiction.");

    ImGui::SeparatorText("Device-side effects");
    if (ImGui::SliderFloat("Centring spring", &f.springGain, 0.0f, 1.0f, "%.2f"))
        app.wheel.setSpring(f.springGain, f.springSaturation);
    if (ImGui::SliderFloat("Damper", &f.damperGain, 0.0f, 1.0f, "%.2f"))
        app.wheel.setDamper(f.damperGain);
    ImGui::Checkbox("Invert force direction", &f.invertForce);

    ImGui::SeparatorText("Manual test");
    ImGui::TextDisabled("Only active when the test drive is stopped.");
    ImGui::SliderFloat("Constant force", &app.manualForce, -1.0f, 1.0f, "%.2f");
    if (ImGui::Button("Release")) app.manualForce = 0.0f;

    // ---- force response calibration ---------------------------------------
    ImGui::SeparatorText("Force response calibration");
    ImGui::TextWrapped(
        "Measures how much torque the wheel really produces for each force it "
        "is asked for, then inverts that into a correction curve. A G29 has a "
        "dead zone around centre where small forces move nothing at all, so "
        "without this the finest detail is lost entirely.");

    if (app.cal.running()) {
        ImGui::ProgressBar(app.cal.progress(), ImVec2(-1, 0));
        ImGui::TextWrapped("%s", app.cal.message().c_str());
        if (ImGui::Button("Cancel")) app.cal.cancel();
        ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1),
                           "Keep hands clear - the wheel is turning itself.");
    } else {
        ImGui::SetNextItemWidth(140);
        ImGui::SliderInt("Steps", &app.calParams.steps, 8, 40);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140);
        ImGui::SliderInt("Repeats", &app.calParams.reps, 1, 4);
        ImGui::TextDisabled("More steps and repeats give a smoother curve and take longer.");

        if (ImGui::Button("Start calibration")) {
            std::string err;
            if (!app.cal.start(&app.wheel, app.calParams, err)) app.status = err;
            else {
                app.simRunning = false;
                app.status = "Calibrating - keep hands off the wheel.";
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Load .lut")) {
            std::string err;
            if (loadLutFile(f.lutFile, f.lut, err)) {
                f.useLut = true;
                app.status = "Loaded " + f.lutFile;
            } else {
                app.status = err;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Save .lut")) {
            std::string err;
            app.status = saveLutFile(f.lutFile, f.lut, err)
                       ? ("Saved " + f.lutFile) : err;
        }

        const std::string msg = app.cal.message();
        if (!msg.empty()) ImGui::TextWrapped("%s", msg.c_str());
    }

    if (!f.lut.empty()) {
        ImGui::Checkbox("Apply correction curve", &f.useLut);
        ImGui::SameLine();
        ImGui::TextDisabled("(%d points)", (int)f.lut.size());

        // Measured response, and the correction derived from it.
        auto pts = app.cal.points();
        if (!pts.empty()) {
            std::vector<float> resp;
            resp.reserve(pts.size());
            for (const auto& q : pts) resp.push_back(q.response);
            ImGui::PlotLines("Measured", resp.data(), (int)resp.size(), 0,
                             "torque vs requested force", 0.0f, 1.0f, ImVec2(-1, 70));
        }
        std::vector<float> corr;
        corr.reserve(f.lut.size());
        for (const auto& q : f.lut) corr.push_back(q.second);
        ImGui::PlotLines("Correction", corr.data(), (int)corr.size(), 0,
                         "output vs requested", 0.0f, 1.0f, ImVec2(-1, 70));
        if (f.lut.size() > 1) {
            ImGui::Text("A request of %.0f%% now sends %.0f%% - that jump is the "
                        "dead zone being skipped.",
                        f.lut[1].first * 100.0f, f.lut[1].second * 100.0f);
        }
    }
}

// Wheel button index -> Xbox 360 button. Shifter positions are skipped, so a
// gear selection never also fires a face button.
struct BtnMap { int wheelBtn; unsigned short xusb; };
static const BtnMap kBtnMap[] = {
    {0, XUSB_GAMEPAD_A},             {1, XUSB_GAMEPAD_B},
    {2, XUSB_GAMEPAD_X},             {3, XUSB_GAMEPAD_Y},
    {4, XUSB_GAMEPAD_RIGHT_SHOULDER},{5, XUSB_GAMEPAD_LEFT_SHOULDER},
    {6, XUSB_GAMEPAD_BACK},          {7, XUSB_GAMEPAD_START},
    {8, XUSB_GAMEPAD_LEFT_THUMB},    {9, XUSB_GAMEPAD_RIGHT_THUMB},
};

// Turns calibrated wheel input into what the game will see on the pad.
//
// The gearbox lives entirely here. No game is asked for anything: the clutch
// and the selected gear simply shape the throttle we emit, which is why the
// H-pattern works in titles that have no gearbox at all.
struct PadOut {
    float steer = 0.0f;
    int   lt = 0, rt = 0, gear = 0;
    unsigned short buttons = 0;
};

static PadOut computeOutput(Wheel& wheel, const Profile& p, VirtualPad& pad) {
    const DIJOYSTATE2& st = wheel.state();

    float steer = p.steering.assigned()
        ? p.steering.bipolar(Wheel::axisRaw(st, p.steering.offset)) : 0.0f;
    float thr = p.throttle.assigned()
        ? p.throttle.unipolar(Wheel::axisRaw(st, p.throttle.offset)) : 0.0f;
    float brk = p.brake.assigned()
        ? p.brake.unipolar(Wheel::axisRaw(st, p.brake.offset)) : 0.0f;
    const float clu = p.clutch.assigned()
        ? p.clutch.unipolar(Wheel::axisRaw(st, p.clutch.offset)) : 0.0f;

    const int gear = readGear(p.gearbox, st);

    if (p.gearbox.enabled && p.bridge.shapeThrottle) {
        const bool clutchIn = clu > p.gearbox.clutchBite;
        if (clutchIn) {
            thr = 0.0f;                                   // drive disconnected
        } else if (gear == 0) {
            if (p.gearbox.neutralCutsThrottle) thr = 0.0f;
        } else {
            // Each gear caps how much throttle reaches the game, which is the
            // closest a pad can get to a gear ratio.
            const int idx = std::clamp(std::abs(gear), 0, 6);
            thr *= std::clamp(p.gearbox.gearRatio[idx], 0.05f, 1.0f);
        }
        // Reverse: the accelerator should drive backwards. Games reverse on the
        // brake trigger, so the two pedals swap roles.
        if (gear < 0) std::swap(thr, brk);
    }

    steer = std::clamp(steer * p.bridge.steerScale, -1.0f, 1.0f);

    unsigned short flags = 0;
    if (p.bridge.mapButtons) {
        for (const auto& m : kBtnMap) {
            bool isGearButton = false;
            for (int g = 0; g <= p.gearbox.gearCount; ++g)
                if (p.gearbox.gearButton[g] == m.wheelBtn) { isGearButton = true; break; }
            if (isGearButton) continue;
            if (m.wheelBtn < wheel.buttonCount() &&
                (st.rgbButtons[m.wheelBtn] & 0x80))
                flags |= m.xusb;
        }
        const DWORD pov = st.rgdwPOV[0];
        if (pov != 0xFFFFFFFF) {
            const DWORD d = pov / 100;
            if (d > 315 || d <= 45)  flags |= XUSB_GAMEPAD_DPAD_UP;
            if (d > 45  && d <= 135) flags |= XUSB_GAMEPAD_DPAD_RIGHT;
            if (d > 135 && d <= 225) flags |= XUSB_GAMEPAD_DPAD_DOWN;
            if (d > 225 && d <= 315) flags |= XUSB_GAMEPAD_DPAD_LEFT;
        }
    }

    PadOut o;
    o.steer   = steer;
    o.rt      = (int)std::lround(std::clamp(thr, 0.0f, 1.0f) * 255.0f);
    o.lt      = (int)std::lround(std::clamp(brk, 0.0f, 1.0f) * 255.0f);
    o.gear    = gear;
    o.buttons = flags;

    pad.setLeftStick((short)std::clamp(steer * 32767.0f, -32767.0f, 32767.0f), 0);
    pad.setRightStick(0, 0);
    pad.setTriggers((unsigned char)o.lt, (unsigned char)o.rt);
    pad.setButtons(flags);
    pad.submit();
    return o;
}

static void drawGameOutputTab(App& app) {
    auto& b = app.profile.bridge;

    ImGui::TextWrapped(
        "Presents the G29 to Windows as a standard Xbox 360 controller, so any "
        "game can use it. Nothing is injected into any game and no game file is "
        "touched -- the game simply sees a controller.");
    ImGui::Separator();

    if (!app.pad.isOpen()) {
        if (ImGui::Button("Start game output")) {
            std::string err;
            if (app.pad.open(err)) {
                b.enabled = true;
                app.status = "Virtual Xbox 360 pad created. Games can see it now.";
            } else {
                app.status = err;
            }
        }
    } else {
        if (ImGui::Button("Stop game output")) {
            app.pad.close();
            b.enabled = false;
            app.status = "Virtual pad removed.";
        }
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "ACTIVE");
    }

    ImGui::Separator();
    ImGui::SliderFloat("Steering scale", &b.steerScale, 0.3f, 2.0f, "%.2f");
    ImGui::TextDisabled("Raise if the game's own steering assist feels too lazy.");
    ImGui::Checkbox("Gearbox shapes throttle", &b.shapeThrottle);
    ImGui::SameLine();
    ImGui::Checkbox("Map wheel buttons", &b.mapButtons);
    ImGui::SliderFloat("Game rumble to wheel", &b.rumbleToWheel, 0.0f, 1.5f, "%.2f");

    ImGui::SeparatorText("What the game is receiving");
    ImGui::Text("Steering  %+.3f", app.outSteer);
    ImGui::ProgressBar(app.outSteer * 0.5f + 0.5f, ImVec2(-1, 0));
    ImGui::Text("Throttle (RT)  %3d", app.outRT);
    ImGui::ProgressBar(app.outRT / 255.0f, ImVec2(-1, 0));
    ImGui::Text("Brake (LT)     %3d", app.outLT);
    ImGui::ProgressBar(app.outLT / 255.0f, ImVec2(-1, 0));
    ImGui::Text("Gear %s   buttons 0x%04X",
                app.outGear < 0 ? "R" : (app.outGear == 0 ? "N" :
                    std::to_string(app.outGear).c_str()), app.outButtons);

    unsigned char lg = 0, sm = 0;
    app.pad.rumble(lg, sm);
    ImGui::Text("Rumble from game:  large %3u   small %3u", lg, sm);
    ImGui::TextDisabled(
        "Two bytes is the whole of XInput's feedback channel. It carries "
        "collisions and kerbs; slip angle and tyre load are not available from "
        "a game that is not modified.");

    ImGui::SeparatorText("Before launching a game");
    ImGui::TextWrapped(
        "Leave this app running - it owns the wheel and feeds the pad. If a game "
        "also reads the real wheel directly you may get doubled steering; set the "
        "game to use a controller rather than a wheel.");
}

static void drawTestDriveTab(App& app) {
    ImGui::TextWrapped(
        "A small vehicle model with a saturating tyre curve. Because the physics "
        "run here, the force feedback is driven by real slip angle, load and "
        "surface -- the detail a controller bridge can never get out of a game "
        "it does not modify.");

    if (ImGui::Button(app.simRunning ? "Stop" : "Start")) {
        app.simRunning = !app.simRunning;
        if (app.simRunning) app.sim.reset();
        else if (app.wheel.isOpen()) app.wheel.setConstantForce(0.0f);
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset car")) app.sim.reset();

    const CarState& s = app.sim.state();
    ImGui::SameLine();
    ImGui::Text("   %5.1f km/h   %6.0f rpm   gear %d   %s",
                s.speedKph(), s.engineRpm,
                readGear(app.profile.gearbox, app.wheel.state()),
                s.stalled ? "STALLED" : "");

    // ---- track + car -------------------------------------------------------
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 avail  = ImGui::GetContentRegionAvail();
    float h = std::max(240.0f, avail.y - 120.0f);
    ImVec2 size(avail.x, h);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y),
                      IM_COL32(26, 32, 26, 255));

    const float scale = std::min(size.x / 420.0f, size.y / 300.0f);
    auto toScreen = [&](float wx, float wy) {
        return ImVec2(origin.x + size.x * 0.5f + wx * scale,
                      origin.y + size.y * 0.5f + wy * scale);
    };

    // Track band
    const int N = 160;
    ImVec2 inner[N + 1], outer[N + 1];
    for (int i = 0; i <= N; ++i) {
        const float t = (float)i / N * 6.2831853f;
        const float cx = std::cos(t), sy = std::sin(t);
        const float rx = TestDrive::kTrackRadiusX, ry = TestDrive::kTrackRadiusY;
        const float avg = 0.5f * (rx + ry);
        const float fi = (avg - TestDrive::kHalfWidth) / avg;
        const float fo = (avg + TestDrive::kHalfWidth) / avg;
        inner[i] = toScreen(rx * cx * fi, ry * sy * fi);
        outer[i] = toScreen(rx * cx * fo, ry * sy * fo);
    }
    dl->AddPolyline(outer, N + 1, IM_COL32(70, 70, 76, 255), 0, 3.0f);
    dl->AddPolyline(inner, N + 1, IM_COL32(70, 70, 76, 255), 0, 3.0f);
    for (int i = 0; i < N; i += 2) {
        dl->AddQuadFilled(inner[i], outer[i], outer[i + 1], inner[i + 1],
                          IM_COL32(48, 48, 52, 255));
    }

    // Car
    const float ch = s.heading;
    const float cs = std::cos(ch), sn = std::sin(ch);
    auto body = [&](float lx, float ly) {
        return toScreen(s.x + lx * cs - ly * sn, s.y + lx * sn + ly * cs);
    };
    ImVec2 p0 = body(2.2f, -1.0f), p1 = body(2.2f, 1.0f),
           p2 = body(-2.2f, 1.0f), p3 = body(-2.2f, -1.0f);
    const ImU32 carCol = s.surface == SURF_ASPHALT ? IM_COL32(90, 190, 255, 255)
                       : s.surface == SURF_KERB    ? IM_COL32(255, 200, 80, 255)
                                                   : IM_COL32(140, 230, 140, 255);
    dl->AddQuadFilled(p0, p1, p2, p3, carCol);
    dl->AddLine(body(2.2f, 0.0f), body(4.0f, 0.0f), IM_COL32(255, 255, 255, 200), 2.0f);

    ImGui::Dummy(size);

    // ---- telemetry ---------------------------------------------------------
    ImGui::Separator();
    const char* surfName = s.surface == SURF_ASPHALT ? "asphalt"
                         : s.surface == SURF_KERB    ? "kerb" : "grass";
    ImGui::Text("Surface %-8s grip %.2f   slip front %+6.2f deg   force %+.3f",
                surfName, s.grip, s.slipFront * 57.2958f,
                app.sim.steeringForce(app.profile.ffb));
    const float fnorm = app.sim.steeringForce(app.profile.ffb) * 0.5f + 0.5f;
    ImGui::ProgressBar(fnorm, ImVec2(-1, 0), "steering force");
    if (!app.simRunning)
        ImGui::TextDisabled("Press Start. Keyboard fallback: W / S steer, Up / Down pedals.");
}

// ------------------------------------------------------- self test ----------

static int runSelfTest() {
    std::ofstream log("g29studio_selftest.txt");
    log << "g29studio self test\n";

    auto devs = listDevices();
    log << "devices found: " << devs.size() << "\n";
    for (const auto& d : devs)
        log << "  " << wideToUtf8(d.productName)
            << (d.hasFFB ? "  [FFB]" : "  [no FFB]") << "\n";

    Profile p;
    std::string err;
    if (!p.save("g29studio_selftest_profile.ini", err)) log << "profile save FAILED: " << err << "\n";
    else {
        Profile q;
        if (!q.load("g29studio_selftest_profile.ini", err)) log << "profile load FAILED: " << err << "\n";
        else log << "profile round trip OK (gamma=" << q.steering.gamma
                 << ", springGain=" << q.ffb.springGain << ")\n";
    }

    // Physics must stay finite and the car must actually move.
    TestDrive sim;
    sim.reset();
    CarInput in;
    in.throttle = 1.0f;
    in.gear = 1;
    GearboxCfg gb;
    for (int i = 0; i < 600; ++i) sim.step(1.0f / 60.0f, in, gb);
    const CarState& s = sim.state();
    const bool finite = std::isfinite(s.x) && std::isfinite(s.y) &&
                        std::isfinite(s.vx) && std::isfinite(s.engineRpm);
    log << "sim after 10s: speed " << s.speedKph() << " km/h, rpm " << s.engineRpm
        << ", finite=" << (finite ? "yes" : "NO") << "\n";

    FfbCfg f;
    log << "steering force in range: "
        << (std::fabs(sim.steeringForce(f)) <= 1.0f ? "yes" : "NO") << "\n";

    // LUT: a curve that skips a 10% dead zone, like a real G29 measures.
    std::vector<std::pair<float, float>> lut = {
        {0.00f, 0.00f}, {0.05f, 0.17f}, {0.25f, 0.27f},
        {0.50f, 0.36f}, {0.75f, 0.63f}, {1.00f, 1.00f}
    };
    std::string lerr;
    bool lutOk = saveLutFile("g29studio_selftest.lut", lut, lerr);
    std::vector<std::pair<float, float>> back;
    if (lutOk) lutOk = loadLutFile("g29studio_selftest.lut", back, lerr);
    log << "lut file round trip: " << (lutOk ? "OK" : ("FAILED: " + lerr))
        << " (" << back.size() << " points)\n";

    FfbCfg g;
    g.lut = back;
    g.useLut = true;
    const float atZero  = g.applyLut(0.0f);
    const float atSmall = g.applyLut(0.05f);
    const float atFull  = g.applyLut(1.0f);
    const float negative= g.applyLut(-0.05f);
    log << "lut applyLut: 0 -> " << atZero
        << ", 0.05 -> " << atSmall
        << ", 1.0 -> " << atFull
        << ", -0.05 -> " << negative << "\n";

    const bool lutSane =
        atZero == 0.0f &&                       // silence stays silence
        atSmall > 0.10f &&                      // dead zone actually skipped
        std::fabs(atFull - 1.0f) < 1e-3f &&     // full force still full
        std::fabs(negative + atSmall) < 1e-5f;  // direction preserved
    log << "lut behaviour sane: " << (lutSane ? "yes" : "NO") << "\n";

    // Monotonic check: output must never decrease as the request rises.
    bool mono = true;
    float prev = -1.0f;
    for (float r = 0.0f; r <= 1.0f; r += 0.02f) {
        const float v = g.applyLut(r);
        if (v < prev - 1e-4f) mono = false;
        prev = v;
    }
    log << "lut monotonic: " << (mono ? "yes" : "NO") << "\n";

    // Disabled LUT must be a no-op.
    g.useLut = false;
    const bool passthrough = std::fabs(g.applyLut(0.42f) - 0.42f) < 1e-6f;
    log << "lut disabled is passthrough: " << (passthrough ? "yes" : "NO") << "\n";

    log << "done\n";
    return (finite && lutOk && lutSane && mono && passthrough) ? 0 : 1;
}

// --------------------------------------------------------- diagnostics ------

// Dumps what the device actually reports, reading the DIJOYSTATE2 fields
// directly rather than through our offset mapping, so the mapping itself can
// be checked rather than assumed.
static int runDiag(int seconds) {
    std::ofstream log("g29studio_diag.txt");
    auto say = [&](const std::string& s) { log << s << "\n"; log.flush(); };

    Wheel w;
    std::string err;
    // Non-exclusive: we only need to read, and this avoids fighting anything else.
    auto devs = listDevices();
    if (devs.empty()) { say("no devices attached"); return 1; }
    int pick = 0;
    for (size_t i = 0; i < devs.size(); ++i) if (devs[i].hasFFB) { pick = (int)i; break; }
    if (!w.open(devs[pick].instanceGuid, false, err)) { say("open failed: " + err); return 1; }

    say("device : " + wideToUtf8(w.productName()));
    say("buttons: " + std::to_string(w.buttonCount()));
    say("");
    say("enumerated axes (name, offset, range as the driver accepted it):");
    for (const auto& a : w.axes()) {
        char b[160];
        snprintf(b, sizeof(b), "  ofs=%-3lu  min=%-6ld max=%-6ld  %s",
                 (unsigned long)a.offset, a.min, a.max, wideToUtf8(a.name).c_str());
        say(b);
    }

    struct Field { const char* name; LONG mn, mx, last; bool seen; };
    Field f[8] = {
        {"lX", 0, 0, 0, false}, {"lY", 0, 0, 0, false},
        {"lZ", 0, 0, 0, false}, {"lRx", 0, 0, 0, false},
        {"lRy", 0, 0, 0, false}, {"lRz", 0, 0, 0, false},
        {"slider0", 0, 0, 0, false}, {"slider1", 0, 0, 0, false},
    };
    bool buttonSeen[128] = {};

    // Run the same auto-assignment the UI uses, so the roles below are an
    // end-to-end check of the mapping rather than of the raw fields alone.
    Profile prof;
    autoAssign(w, prof);
    auto roleName = [&](const AxisCal& c) {
        for (const auto& a : w.axes())
            if (a.offset == c.offset) return wideToUtf8(a.name);
        return std::string("(unassigned)");
    };
    say("");
    say("auto-assigned roles:");
    say("  steering -> " + roleName(prof.steering) +
        (prof.steering.invert ? "  [inverted]" : ""));
    say("  throttle -> " + roleName(prof.throttle) +
        (prof.throttle.invert ? "  [inverted]" : ""));
    say("  brake    -> " + roleName(prof.brake) +
        (prof.brake.invert ? "  [inverted]" : ""));
    say("  clutch   -> " + roleName(prof.clutch) +
        (prof.clutch.invert ? "  [inverted]" : ""));

    say("");
    say("sampling for " + std::to_string(seconds) + " s ...");
    say("");
    say("  time    lX     lY     lZ    lRx    lRy    lRz   sl0    sl1 | steer  thr  brk  clu | buttons");

    const auto t0 = clk::now();
    int lastPrint = -1;
    while (msSince(t0) < (long long)seconds * 1000) {
        if (!w.poll()) { sleepMs(10); continue; }
        const DIJOYSTATE2& s = w.state();
        LONG v[8] = { s.lX, s.lY, s.lZ, s.lRx, s.lRy, s.lRz,
                      s.rglSlider[0], s.rglSlider[1] };
        for (int i = 0; i < 8; ++i) {
            if (!f[i].seen) { f[i].mn = f[i].mx = v[i]; f[i].seen = true; }
            f[i].mn = std::min(f[i].mn, v[i]);
            f[i].mx = std::max(f[i].mx, v[i]);
            f[i].last = v[i];
        }
        for (int b = 0; b < w.buttonCount() && b < 128; ++b)
            if (s.rgbButtons[b] & 0x80) buttonSeen[b] = true;

        const int tenth = (int)(msSince(t0) / 250);
        if (tenth != lastPrint) {
            lastPrint = tenth;
            std::string btns;
            for (int b = 0; b < w.buttonCount() && b < 128; ++b)
                if (s.rgbButtons[b] & 0x80) btns += std::to_string(b) + " ";
            const float rs = prof.steering.assigned()
                ? prof.steering.bipolar(Wheel::axisRaw(s, prof.steering.offset)) : 0.0f;
            const float rt = prof.throttle.assigned()
                ? prof.throttle.unipolar(Wheel::axisRaw(s, prof.throttle.offset)) : 0.0f;
            const float rb = prof.brake.assigned()
                ? prof.brake.unipolar(Wheel::axisRaw(s, prof.brake.offset)) : 0.0f;
            const float rc = prof.clutch.assigned()
                ? prof.clutch.unipolar(Wheel::axisRaw(s, prof.clutch.offset)) : 0.0f;

            char line[320];
            snprintf(line, sizeof(line),
                     "  %5.1fs %6ld %6ld %6ld %6ld %6ld %6ld %6ld %6ld |%+5.2f %4.2f %4.2f %4.2f | %s",
                     msSince(t0) / 1000.0, v[0], v[1], v[2], v[3], v[4], v[5],
                     v[6], v[7], rs, rt, rb, rc, btns.c_str());
            say(line);
        }
        sleepMs(20);
    }

    say("");
    say("SUMMARY - which fields actually moved:");
    for (int i = 0; i < 8; ++i) {
        const LONG span = f[i].mx - f[i].mn;
        char b[160];
        snprintf(b, sizeof(b), "  %-8s min=%-6ld max=%-6ld span=%-6ld  %s",
                 f[i].name, f[i].mn, f[i].mx, span,
                 span > 1000 ? "<-- MOVED" : "(static)");
        say(b);
    }
    std::string pressed;
    for (int b = 0; b < 128; ++b) if (buttonSeen[b]) pressed += std::to_string(b) + " ";
    say("");
    say("buttons seen pressed: " + (pressed.empty() ? std::string("none") : pressed));

    w.close();
    return 0;
}

// ------------------------------------------------------ headless bridge -----

// Runs the wheel-to-Xbox-pad bridge with no window. Useful for launching
// alongside a game, and it is how the pad path gets verified.
static int runBridge(int seconds) {
    std::ofstream log("g29studio_bridge.txt");
    auto say = [&](const std::string& s) { log << s << "\n"; log.flush(); };

    Wheel w;
    std::string err;
    if (!w.openFirstFFB(err)) { say("wheel open failed: " + err); return 1; }
    say("wheel : " + wideToUtf8(w.productName()));

    Profile p;
    std::string e;
    p.load("g29studio_profile.ini", e);
    autoAssign(w, p);
    if (!p.ffb.lutFile.empty() && loadLutFile(p.ffb.lutFile, p.ffb.lut, e)) {
        p.ffb.useLut = true;
        say("force curve: " + p.ffb.lutFile + " (" +
            std::to_string(p.ffb.lut.size()) + " points)");
    }

    std::string ferr;
    const bool ffb = w.ffbInit(ferr);
    if (ffb) {
        w.setSpring(p.ffb.springGain, p.ffb.springSaturation);
        w.setDamper(p.ffb.damperGain);
    } else {
        say("force feedback unavailable: " + ferr);
    }

    VirtualPad pad;
    if (!pad.open(err)) { say("virtual pad failed: " + err); return 1; }
    say("virtual Xbox 360 pad created - games can see it now");
    say("");
    say("  time   steer    LT    RT  gear  buttons  rumble");

    float phase = 0.0f;
    const auto t0 = clk::now();
    auto prev = clk::now();
    int lastPrint = -1;
    while (msSince(t0) < (long long)seconds * 1000) {
        const auto now = clk::now();
        const float dt = std::clamp(
            std::chrono::duration<float>(now - prev).count(), 0.001f, 0.1f);
        prev = now;

        if (!w.poll()) { sleepMs(5); continue; }
        const PadOut o = computeOutput(w, p, pad);

        unsigned char lg = 0, sm = 0;
        pad.rumble(lg, sm);
        if (ffb) {
            const float r = (std::max(lg, sm) / 255.0f) * p.bridge.rumbleToWheel;
            float f = 0.0f;
            if (r > 0.002f) {
                phase += dt * 28.0f * 6.2831853f;
                f = std::sin(phase) * r;
            }
            w.setConstantForce(p.ffb.applyLut(std::clamp(f, -1.0f, 1.0f)));
        }

        const int half = (int)(msSince(t0) / 500);
        if (half != lastPrint) {
            lastPrint = half;
            char b[200];
            snprintf(b, sizeof(b),
                     "  %5.1fs %+6.3f %5d %5d %5s   0x%04X   %3u/%3u",
                     msSince(t0) / 1000.0, o.steer, o.lt, o.rt,
                     o.gear < 0 ? "R" : (o.gear == 0 ? "N" :
                        std::to_string(o.gear).c_str()),
                     o.buttons, lg, sm);
            say(b);
        }
        sleepMs(4);
    }

    if (ffb) w.stopAllForces();
    pad.close();
    w.close();
    say("");
    say("bridge stopped, pad removed cleanly");
    return 0;
}

// ----------------------------------------------------- headless cal ---------

// Runs a full force-response calibration without opening the UI, so it can be
// scripted or driven from a terminal. Writes an Assetto Corsa format .lut.
static int runCalibrate(int steps, int reps, const std::string& outPath) {
    std::ofstream log("g29studio_calibrate.txt");
    auto say = [&](const std::string& s) { log << s << "\n"; log.flush(); };

    Wheel w;
    std::string err;
    if (!w.openFirstFFB(err)) { say("open failed: " + err); return 1; }
    say("device: " + wideToUtf8(w.productName()));

    if (!w.ffbInit(err)) { say("force feedback init failed: " + err); return 1; }

    // Condition effects would fight the measurement.
    w.setSpring(0.0f, 1.0f);
    w.setDamper(0.0f);

    FfbCalibrator cal;
    FfbCalibrator::Params p;
    p.steps = steps;
    p.reps  = reps;
    if (!cal.start(&w, p, err)) { say("could not start: " + err); return 1; }

    say("measuring: " + std::to_string(steps) + " steps x " +
        std::to_string(reps) + " repeats, both directions");
    while (cal.running())
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    cal.join();

    say("");
    say("measured response (requested force -> relative torque):");
    for (const auto& q : cal.points()) {
        char b[96];
        snprintf(b, sizeof(b), "  %5.2f  ->  %.4f", q.force, q.response);
        say(b);
    }

    auto lut = cal.lut();
    if (lut.empty()) { say(""); say("FAILED: " + cal.message()); w.close(); return 1; }

    if (!saveLutFile(outPath, lut, err)) { say("could not save: " + err); w.close(); return 1; }

    say("");
    say(cal.message());
    say("wrote " + outPath + " (" + std::to_string(lut.size()) + " points)");
    say("");
    say("correction curve (requested -> sent):");
    for (const auto& [in, out] : lut) {
        char b[64];
        snprintf(b, sizeof(b), "  %.4f|%.4f", in, out);
        say(b);
    }

    w.stopAllForces();
    w.close();
    return 0;
}

// ------------------------------------------------------------ main ----------

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR lpCmdLine, int) {
    if (lpCmdLine && wcsstr(lpCmdLine, L"--selftest")) return runSelfTest();

    if (lpCmdLine && wcsstr(lpCmdLine, L"--bridge")) {
        int secs = 3600;
        int argc = 0;
        if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
            for (int i = 1; i < argc; ++i)
                if (!wcscmp(argv[i], L"--seconds") && i + 1 < argc) secs = _wtoi(argv[++i]);
            LocalFree(argv);
        }
        return runBridge(std::clamp(secs, 5, 86400));
    }

    if (lpCmdLine && wcsstr(lpCmdLine, L"--diag")) {
        int secs = 30;
        int argc = 0;
        if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
            for (int i = 1; i < argc; ++i)
                if (!wcscmp(argv[i], L"--seconds") && i + 1 < argc) secs = _wtoi(argv[++i]);
            LocalFree(argv);
        }
        return runDiag(std::clamp(secs, 5, 120));
    }

    if (lpCmdLine && wcsstr(lpCmdLine, L"--calibrate")) {
        int steps = 20, reps = 2;
        std::string out = "g29studio.lut";
        int argc = 0;
        if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
            for (int i = 1; i < argc; ++i) {
                if (!wcscmp(argv[i], L"--steps") && i + 1 < argc) steps = _wtoi(argv[++i]);
                else if (!wcscmp(argv[i], L"--reps") && i + 1 < argc) reps = _wtoi(argv[++i]);
                else if (!wcscmp(argv[i], L"--out")  && i + 1 < argc) out = wideToUtf8(argv[++i]);
            }
            LocalFree(argv);
        }
        return runCalibrate(steps, reps, out);
    }

    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L,
                       hInstance, nullptr, nullptr, nullptr, nullptr,
                       L"g29studioMain", nullptr };
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"G29 Studio",
                                WS_OVERLAPPEDWINDOW, 100, 100, 1180, 820,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }
    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    App app;
    {
        std::string e;
        const bool hadProfile = app.profile.load(app.profilePath, e);
        // Bring back a previously measured correction curve, if there is one.
        if (!app.profile.ffb.lutFile.empty()) {
            std::string le;
            if (loadLutFile(app.profile.ffb.lutFile, app.profile.ffb.lut, le)) {
                // On a first run there is no saved preference, and a measured
                // curve sitting unapplied is useless, so switch it on.
                if (!hadProfile) app.profile.ffb.useLut = true;
            } else {
                app.profile.ffb.useLut = false;
            }
        }
    }
    app.devices = listDevices();
    if (!app.devices.empty()) {
        app.selected = 0;
        for (size_t i = 0; i < app.devices.size(); ++i)
            if (app.devices[i].hasFFB) { app.selected = (int)i; break; }
    }

    auto last = std::chrono::steady_clock::now();
    bool done = false;
    while (!done) {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        if (g_swapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            ::Sleep(10);
            continue;
        }
        g_swapChainOccluded = false;

        if (g_resizeWidth != 0 && g_resizeHeight != 0) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_resizeWidth, g_resizeHeight,
                                        DXGI_FORMAT_UNKNOWN, 0);
            g_resizeWidth = g_resizeHeight = 0;
            CreateRenderTarget();
        }

        const auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        dt = std::clamp(dt, 1.0f / 480.0f, 1.0f / 15.0f);

        // ---- calibration hand-off -------------------------------------------
        // While the calibrator runs it owns the wheel outright: DirectInput
        // calls are not safe to make from two threads at once.
        if (app.cal.finished() && !app.cal.running()) {
            app.cal.acknowledge();
            app.cal.join();
            auto produced = app.cal.lut();
            if (!produced.empty()) {
                app.profile.ffb.lut    = produced;
                app.profile.ffb.useLut = true;
                std::string e;
                if (saveLutFile(app.profile.ffb.lutFile, produced, e))
                    app.status = "Calibration complete. Saved " + app.profile.ffb.lutFile;
                else
                    app.status = "Calibration complete, but could not save: " + e;
            } else if (app.cal.failed()) {
                app.status = app.cal.message();
            }
            // Restore the user's device-side effects, which calibration muted.
            if (app.wheel.ffbReady()) {
                app.wheel.setSpring(app.profile.ffb.springGain,
                                    app.profile.ffb.springSaturation);
                app.wheel.setDamper(app.profile.ffb.damperGain);
            }
        }

        // ---- input ---------------------------------------------------------
        if (app.wheel.isOpen() && !app.cal.running()) {
            app.wheel.poll();
            const auto& st = app.wheel.state();

            if (app.capturing >= 0) {
                AxisCal* c = axisSlot(app.profile, app.capturing);
                if (c && c->assigned()) {
                    const LONG raw = Wheel::axisRaw(st, c->offset);
                    c->rawMin = std::min(c->rawMin, raw);
                    c->rawMax = std::max(c->rawMax, raw);
                }
            }
            if (app.learnGear >= 0) {
                for (int b = 0; b < app.wheel.buttonCount() && b < 128; ++b) {
                    const bool down = (st.rgbButtons[b] & 0x80) != 0;
                    if (down && !app.prevButtons[b]) {
                        app.profile.gearbox.gearButton[app.learnGear] = b;
                        app.status = "Assigned button " + std::to_string(b);
                        app.learnGear = -1;
                        break;
                    }
                }
            }
            for (int b = 0; b < 128; ++b)
                app.prevButtons[b] = (b < app.wheel.buttonCount()) &&
                                     ((st.rgbButtons[b] & 0x80) != 0);
        }

        // ---- output to games -----------------------------------------------
        if (app.pad.isOpen() && app.wheel.isOpen() && !app.cal.running()) {
            const PadOut o = computeOutput(app.wheel, app.profile, app.pad);
            app.outSteer = o.steer;
            app.outLT = o.lt; app.outRT = o.rt;
            app.outGear = o.gear; app.outButtons = o.buttons;
        }

        // ---- simulation ----------------------------------------------------
        if (app.cal.running()) {
            // Wheel is busy being measured; leave it entirely alone.
        } else if (app.simRunning) {
            CarInput in;
            if (app.wheel.isOpen()) {
                const auto& st = app.wheel.state();
                const Profile& p = app.profile;
                if (p.steering.assigned()) in.steer    = p.steering.bipolar(Wheel::axisRaw(st, p.steering.offset));
                if (p.throttle.assigned()) in.throttle = p.throttle.unipolar(Wheel::axisRaw(st, p.throttle.offset));
                if (p.brake.assigned())    in.brake    = p.brake.unipolar(Wheel::axisRaw(st, p.brake.offset));
                if (p.clutch.assigned())   in.clutch   = p.clutch.unipolar(Wheel::axisRaw(st, p.clutch.offset));
                in.gear = readGear(app.profile.gearbox, st);
            }
            // Keyboard fallback so the bench is usable with no wheel attached.
            if (!app.wheel.isOpen()) {
                if (ImGui::IsKeyDown(ImGuiKey_A)) in.steer = -1.0f;
                if (ImGui::IsKeyDown(ImGuiKey_D)) in.steer = 1.0f;
                if (ImGui::IsKeyDown(ImGuiKey_W)) in.throttle = 1.0f;
                if (ImGui::IsKeyDown(ImGuiKey_S)) in.brake = 1.0f;
                in.gear = 1;
            }
            app.sim.step(dt, in, app.profile.gearbox);

            if (app.ffbEnabled && app.wheel.ffbReady()) {
                const float raw = app.sim.steeringForce(app.profile.ffb);
                app.wheel.setConstantForce(app.profile.ffb.applyLut(raw));
            }
        } else if (app.ffbEnabled && app.wheel.ffbReady()) {
            float f = app.manualForce;
            // Convert the game's rumble into something the wheel can express.
            // It is only two bytes, so this gives collisions and kerbs -- not
            // road feel, which no unmodified game can tell us about.
            if (app.pad.isOpen()) {
                unsigned char lg = 0, sm = 0;
                app.pad.rumble(lg, sm);
                const float r = (std::max(lg, sm) / 255.0f) *
                                app.profile.bridge.rumbleToWheel;
                if (r > 0.002f) {
                    app.rumblePhase += dt * 28.0f * 6.2831853f;
                    f += std::sin(app.rumblePhase) * r;
                }
            }
            app.wheel.setConstantForce(
                app.profile.ffb.applyLut(std::clamp(f, -1.0f, 1.0f)));
        }

        // ---- UI ------------------------------------------------------------
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("G29 Studio", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

        if (ImGui::BeginTabBar("tabs")) {
            if (ImGui::BeginTabItem("Device"))      { drawDeviceTab(app);      ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Calibration")) { drawCalibrationTab(app); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Gearbox"))     { drawGearboxTab(app);     ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Force Feedback")) { drawFfbTab(app);      ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Game Output"))   { drawGameOutputTab(app);ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Test Drive"))  { drawTestDriveTab(app);   ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }

        ImGui::Separator();
        if (ImGui::Button("Save profile")) {
            std::string e;
            app.status = app.profile.save(app.profilePath, e) ? "Profile saved." : e;
        }
        ImGui::SameLine();
        if (ImGui::Button("Load profile")) {
            std::string e;
            app.status = app.profile.load(app.profilePath, e) ? "Profile loaded." : e;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", app.status.c_str());
        ImGui::End();

        ImGui::Render();
        const float clear[4] = { 0.09f, 0.09f, 0.11f, 1.0f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRTV, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRTV, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        HRESULT hr = g_pSwapChain->Present(1, 0);
        g_swapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
    }

    app.pad.close();
    app.wheel.close();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}
