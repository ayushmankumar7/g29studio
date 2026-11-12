#include "wheel.h"

#include <algorithm>

namespace {

struct EnumCtx {
    std::vector<DeviceDesc>* out;
    bool markFFB;
};

BOOL CALLBACK enumDevCb(LPCDIDEVICEINSTANCEW inst, LPVOID ctxv) {
    auto* ctx = static_cast<EnumCtx*>(ctxv);
    if (ctx->markFFB) {
        for (auto& d : *ctx->out)
            if (d.instanceGuid == inst->guidInstance) d.hasFFB = true;
    } else {
        DeviceDesc d;
        d.instanceName = inst->tszInstanceName;
        d.productName  = inst->tszProductName;
        d.instanceGuid = inst->guidInstance;
        d.productGuid  = inst->guidProduct;
        ctx->out->push_back(std::move(d));
    }
    return DIENUM_CONTINUE;
}

struct AxisEnumCtx {
    std::vector<AxisInfo>* axes;
    int sliderIndex;
};

BOOL CALLBACK enumAxisCb(LPCDIDEVICEOBJECTINSTANCEW obj, LPVOID ctxv) {
    auto* ctx = static_cast<AxisEnumCtx*>(ctxv);

    AxisInfo a;
    a.name           = obj->tszName;
    a.type           = obj->dwType;
    a.reportedOffset = obj->dwOfs;

    // Map by axis type GUID, which is authoritative, rather than by dwOfs.
    if      (obj->guidType == GUID_XAxis)  a.offset = DIJOFS_X;
    else if (obj->guidType == GUID_YAxis)  a.offset = DIJOFS_Y;
    else if (obj->guidType == GUID_ZAxis)  a.offset = DIJOFS_Z;
    else if (obj->guidType == GUID_RxAxis) a.offset = DIJOFS_RX;
    else if (obj->guidType == GUID_RyAxis) a.offset = DIJOFS_RY;
    else if (obj->guidType == GUID_RzAxis) a.offset = DIJOFS_RZ;
    else if (obj->guidType == GUID_Slider)
        a.offset = (DWORD)DIJOFS_SLIDER(ctx->sliderIndex++);
    else
        return DIENUM_CONTINUE;   // not an axis we can address in DIJOYSTATE2

    ctx->axes->push_back(std::move(a));
    return DIENUM_CONTINUE;
}

IDirectInput8W* makeDI() {
    IDirectInput8W* di = nullptr;
    if (FAILED(DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION,
                                  IID_IDirectInput8W, (void**)&di, nullptr)))
        return nullptr;
    return di;
}

// DirectInput only grants an exclusive acquire against a top-level window owned
// by the calling process, so we always create our own rather than borrowing one.
HWND createOwnWindow() {
    static const wchar_t* kClass = L"g29studioDirectInputWnd";
    static bool registered = false;
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = DefWindowProcW;
        wc.hInstance     = hInst;
        wc.lpszClassName = kClass;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return nullptr;
        registered = true;
    }
    return CreateWindowExW(0, kClass, L"g29studio", WS_OVERLAPPED,
                           0, 0, 1, 1, nullptr, nullptr, hInst, nullptr);
}

} // namespace

std::vector<DeviceDesc> listDevices() {
    std::vector<DeviceDesc> out;
    IDirectInput8W* di = makeDI();
    if (!di) return out;

    EnumCtx ctx{&out, false};
    di->EnumDevices(DI8DEVCLASS_GAMECTRL, enumDevCb, &ctx, DIEDFL_ATTACHEDONLY);
    ctx.markFFB = true;
    di->EnumDevices(DI8DEVCLASS_GAMECTRL, enumDevCb, &ctx,
                    DIEDFL_ATTACHEDONLY | DIEDFL_FORCEFEEDBACK);
    di->Release();
    return out;
}

Wheel::~Wheel() { close(); }

void Wheel::close() {
    ffbShutdown();
    if (m_dev) {
        if (m_acquired) { m_dev->Unacquire(); m_acquired = false; }
        m_dev->Release();
        m_dev = nullptr;
    }
    if (m_di)   { m_di->Release(); m_di = nullptr; }
    if (m_hwnd) { DestroyWindow(m_hwnd); m_hwnd = nullptr; }
    m_axes.clear();
    m_buttons = 0;
    m_hasFFB  = false;
    m_product.clear();
}

bool Wheel::openFirstFFB(std::string& err) {
    auto devs = listDevices();
    if (devs.empty()) {
        err = "No game controllers attached. Is the G29 plugged in, powered, "
              "and switched to PC mode?";
        return false;
    }
    for (const auto& d : devs)
        if (d.hasFFB) return open(d.instanceGuid, true, err);
    return open(devs[0].instanceGuid, true, err);
}

bool Wheel::open(const GUID& instanceGuid, bool exclusive, std::string& err) {
    close();

    m_di = makeDI();
    if (!m_di) { err = "DirectInput8Create failed."; return false; }

    if (FAILED(m_di->CreateDevice(instanceGuid, &m_dev, nullptr)) || !m_dev) {
        err = "Could not open that device.";
        return false;
    }
    if (FAILED(m_dev->SetDataFormat(&c_dfDIJoystick2))) {
        err = "SetDataFormat failed.";
        return false;
    }

    m_hwnd = createOwnWindow();
    if (!m_hwnd) { err = "Could not create a window for DirectInput."; return false; }

    DWORD coop = exclusive ? (DISCL_EXCLUSIVE | DISCL_BACKGROUND)
                           : (DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);
    if (FAILED(m_dev->SetCooperativeLevel(m_hwnd, coop))) {
        err = "SetCooperativeLevel failed.";
        return false;
    }

    DIDEVICEINSTANCEW inst{};
    inst.dwSize = sizeof(inst);
    if (SUCCEEDED(m_dev->GetDeviceInfo(&inst))) m_product = inst.tszProductName;

    DIDEVCAPS caps{};
    caps.dwSize = sizeof(caps);
    if (SUCCEEDED(m_dev->GetCapabilities(&caps))) {
        m_buttons = (int)caps.dwButtons;
        m_hasFFB  = (caps.dwFlags & DIDC_FORCEFEEDBACK) != 0;
    }

    AxisEnumCtx actx{&m_axes, 0};
    m_dev->EnumObjects(enumAxisCb, &actx, DIDFT_AXIS);

    // Normalise every axis to 0..65535 so calibration maths is predictable.
    // Addressed by object ID, not by offset, for the same reason as above.
    for (auto& a : m_axes) {
        DIPROPRANGE pr{};
        pr.diph.dwSize       = sizeof(DIPROPRANGE);
        pr.diph.dwHeaderSize = sizeof(DIPROPHEADER);
        pr.diph.dwHow        = DIPH_BYID;
        pr.diph.dwObj        = a.type;
        pr.lMin = 0;
        pr.lMax = 65535;
        m_dev->SetProperty(DIPROP_RANGE, &pr.diph);

        DIPROPRANGE got = pr;
        if (SUCCEEDED(m_dev->GetProperty(DIPROP_RANGE, &got.diph))) {
            a.min = got.lMin;
            a.max = got.lMax;
        }
    }

    if (exclusive && m_hasFFB) setAutoCenter(false);

    if (FAILED(m_dev->Acquire())) {
        err = "Could not acquire the wheel. Another program may be holding it "
              "(a game, G HUB, or another copy of this app).";
        return false;
    }
    m_acquired = true;
    return true;
}

bool Wheel::poll() {
    if (!m_dev) return false;
    HRESULT hr = m_dev->Poll();
    if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
        if (FAILED(m_dev->Acquire())) return false;
        m_dev->Poll();
    }
    m_state = DIJOYSTATE2{};
    return SUCCEEDED(m_dev->GetDeviceState(sizeof(DIJOYSTATE2), &m_state));
}

LONG Wheel::axisRaw(const DIJOYSTATE2& st, DWORD offset) {
    switch (offset) {
        case DIJOFS_X:  return st.lX;
        case DIJOFS_Y:  return st.lY;
        case DIJOFS_Z:  return st.lZ;
        case DIJOFS_RX: return st.lRx;
        case DIJOFS_RY: return st.lRy;
        case DIJOFS_RZ: return st.lRz;
        default: break;
    }
    if (offset == (DWORD)DIJOFS_SLIDER(0)) return st.rglSlider[0];
    if (offset == (DWORD)DIJOFS_SLIDER(1)) return st.rglSlider[1];
    return 0;
}

// ---------------------------------------------------------------- FFB --------

bool Wheel::createCondition(const GUID& guid, IDirectInputEffect** out) {
    DICONDITION cond{};
    cond.lOffset              = 0;
    cond.lPositiveCoefficient = 0;
    cond.lNegativeCoefficient = 0;
    cond.dwPositiveSaturation = DI_FFNOMINALMAX;
    cond.dwNegativeSaturation = DI_FFNOMINALMAX;
    cond.lDeadBand            = 0;

    DIEFFECT eff{};
    eff.dwSize                  = sizeof(DIEFFECT);
    eff.dwFlags                 = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
    eff.dwDuration              = INFINITE;
    eff.dwGain                  = DI_FFNOMINALMAX;
    eff.dwTriggerButton         = DIEB_NOTRIGGER;
    eff.cAxes                   = 1;
    eff.rgdwAxes                = m_axesOfs;
    eff.rglDirection            = m_dir;
    eff.cbTypeSpecificParams    = sizeof(DICONDITION);
    eff.lpvTypeSpecificParams   = &cond;

    if (FAILED(m_dev->CreateEffect(guid, &eff, out, nullptr)) || !*out) return false;
    (*out)->Start(1, 0);
    return true;
}

void Wheel::updateCondition(IDirectInputEffect* eff, LONG coeff, DWORD saturation) {
    if (!eff) return;
    DICONDITION cond{};
    cond.lOffset              = 0;
    cond.lPositiveCoefficient = coeff;
    cond.lNegativeCoefficient = coeff;
    cond.dwPositiveSaturation = saturation;
    cond.dwNegativeSaturation = saturation;
    cond.lDeadBand            = 0;

    DIEFFECT eff2{};
    eff2.dwSize                = sizeof(DIEFFECT);
    eff2.dwFlags               = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
    eff2.cAxes                 = 1;
    eff2.rgdwAxes              = m_axesOfs;
    eff2.rglDirection          = m_dir;
    eff2.cbTypeSpecificParams  = sizeof(DICONDITION);
    eff2.lpvTypeSpecificParams = &cond;
    eff->SetParameters(&eff2, DIEP_TYPESPECIFICPARAMS | DIEP_START);
}

bool Wheel::ffbInit(std::string& err) {
    if (!m_dev)   { err = "No device open.";                       return false; }
    if (!m_hasFFB){ err = "This device reports no force feedback."; return false; }
    if (m_constant) return true;

    m_dev->SendForceFeedbackCommand(DISFFC_RESET);
    m_dev->SendForceFeedbackCommand(DISFFC_SETACTUATORSON);

    m_axesOfs[0] = DIJOFS_X;
    m_dir[0]     = 0;
    m_cf.lMagnitude = 0;

    m_cfEffect = DIEFFECT{};
    m_cfEffect.dwSize                = sizeof(DIEFFECT);
    m_cfEffect.dwFlags               = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
    m_cfEffect.dwDuration            = INFINITE;
    m_cfEffect.dwGain                = DI_FFNOMINALMAX;
    m_cfEffect.dwTriggerButton       = DIEB_NOTRIGGER;
    m_cfEffect.cAxes                 = 1;
    m_cfEffect.rgdwAxes              = m_axesOfs;
    m_cfEffect.rglDirection          = m_dir;
    m_cfEffect.cbTypeSpecificParams  = sizeof(DICONSTANTFORCE);
    m_cfEffect.lpvTypeSpecificParams = &m_cf;

    if (FAILED(m_dev->CreateEffect(GUID_ConstantForce, &m_cfEffect, &m_constant, nullptr))
        || !m_constant) {
        err = "Could not create the constant-force effect.";
        return false;
    }
    m_constant->Start(1, 0);

    // Optional: some wheels expose only a subset of condition effects.
    createCondition(GUID_Spring, &m_spring);
    createCondition(GUID_Damper, &m_damper);
    return true;
}

void Wheel::ffbShutdown() {
    stopAllForces();
    if (m_constant) { m_constant->Release(); m_constant = nullptr; }
    if (m_spring)   { m_spring->Release();   m_spring   = nullptr; }
    if (m_damper)   { m_damper->Release();   m_damper   = nullptr; }
    if (m_dev) m_dev->SendForceFeedbackCommand(DISFFC_STOPALL);
}

void Wheel::setConstantForce(float n) {
    if (!m_constant) return;
    n = std::clamp(n, -1.0f, 1.0f);
    m_cf.lMagnitude = (LONG)(n * DI_FFNOMINALMAX);
    m_cfEffect.cbTypeSpecificParams  = sizeof(DICONSTANTFORCE);
    m_cfEffect.lpvTypeSpecificParams = &m_cf;
    m_constant->SetParameters(&m_cfEffect, DIEP_TYPESPECIFICPARAMS | DIEP_START);
}

void Wheel::setSpring(float coefficient, float saturation) {
    updateCondition(m_spring,
                    (LONG)(std::clamp(coefficient, 0.0f, 1.0f) * DI_FFNOMINALMAX),
                    (DWORD)(std::clamp(saturation, 0.0f, 1.0f) * DI_FFNOMINALMAX));
}

void Wheel::setDamper(float coefficient) {
    updateCondition(m_damper,
                    (LONG)(std::clamp(coefficient, 0.0f, 1.0f) * DI_FFNOMINALMAX),
                    DI_FFNOMINALMAX);
}

void Wheel::setAutoCenter(bool on) {
    if (!m_dev) return;
    const bool wasAcquired = m_acquired;
    if (wasAcquired) { m_dev->Unacquire(); m_acquired = false; }

    DIPROPDWORD dw{};
    dw.diph.dwSize       = sizeof(DIPROPDWORD);
    dw.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    dw.diph.dwHow        = DIPH_DEVICE;
    dw.diph.dwObj        = 0;
    dw.dwData            = on ? DIPROPAUTOCENTER_ON : DIPROPAUTOCENTER_OFF;
    m_dev->SetProperty(DIPROP_AUTOCENTER, &dw.diph);

    if (wasAcquired && SUCCEEDED(m_dev->Acquire())) m_acquired = true;
}

void Wheel::stopAllForces() {
    setConstantForce(0.0f);
    if (m_spring) updateCondition(m_spring, 0, DI_FFNOMINALMAX);
    if (m_damper) updateCondition(m_damper, 0, DI_FFNOMINALMAX);
}
