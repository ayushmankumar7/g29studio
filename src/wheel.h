#pragma once
#define DIRECTINPUT_VERSION 0x0800

#include <windows.h>
#include <dinput.h>

#include <string>
#include <vector>

// A game controller found on the system.
struct DeviceDesc {
    std::wstring instanceName;
    std::wstring productName;
    GUID         instanceGuid{};
    GUID         productGuid{};
    bool         hasFFB = false;
};

// One axis physically present on the opened device.
struct AxisInfo {
    // Offset into DIJOYSTATE2, derived from the axis TYPE GUID.
    //
    // It is deliberately not taken from DIDEVICEOBJECTINSTANCE::dwOfs: the G29
    // driver reports 4/8/12/16 there while the data actually lands in lX, lY,
    // lRz and slider0, so trusting dwOfs reads three permanently-static fields.
    DWORD        offset = 0;
    DWORD        type   = 0;    // dwType, for DIPH_BYID property access
    DWORD        reportedOffset = 0;  // what the driver claimed, for diagnostics
    std::wstring name;
    LONG         min = 0;
    LONG         max = 65535;
};

std::vector<DeviceDesc> listDevices();

// Wraps one DirectInput device: polling plus the force-feedback effects we use.
//
// Force feedback on Windows is exclusive-acquire, so only one process may own
// the wheel's effects at a time. That is why this app takes the wheel directly
// rather than sitting behind anything else.
class Wheel {
public:
    Wheel() = default;
    ~Wheel();
    Wheel(const Wheel&) = delete;
    Wheel& operator=(const Wheel&) = delete;

    bool open(const GUID& instanceGuid, bool exclusive, std::string& err);
    bool openFirstFFB(std::string& err);
    void close();
    bool isOpen() const { return m_dev != nullptr; }

    bool poll();
    const DIJOYSTATE2& state() const { return m_state; }

    const std::wstring&          productName() const { return m_product; }
    const std::vector<AxisInfo>& axes()        const { return m_axes; }
    int  buttonCount() const { return m_buttons; }
    bool hasFFB()      const { return m_hasFFB; }

    static LONG axisRaw(const DIJOYSTATE2& st, DWORD offset);
    LONG axisRaw(DWORD offset) const { return axisRaw(m_state, offset); }

    // --- Force feedback -----------------------------------------------------
    bool ffbInit(std::string& err);
    void ffbShutdown();
    bool ffbReady() const { return m_constant != nullptr; }

    // Main steering force, -1..+1. This carries self-aligning torque, kerbs,
    // surface texture -- everything that makes a wheel feel like something.
    void setConstantForce(float n);

    // Device-side centring spring. coefficient 0..1, saturation 0..1.
    void setSpring(float coefficient, float saturation);

    // Device-side damping, 0..1. Raises stability, kills oscillation.
    void setDamper(float coefficient);

    void setAutoCenter(bool on);
    void stopAllForces();

private:
    bool createCondition(const GUID& guid, IDirectInputEffect** out);
    void updateCondition(IDirectInputEffect* eff, LONG coeff, DWORD saturation);

    IDirectInput8W*       m_di   = nullptr;
    IDirectInputDevice8W* m_dev  = nullptr;
    HWND                  m_hwnd = nullptr;

    IDirectInputEffect* m_constant = nullptr;
    IDirectInputEffect* m_spring   = nullptr;
    IDirectInputEffect* m_damper   = nullptr;

    // Kept alive between SetParameters() calls.
    DWORD           m_axesOfs[1]{};
    LONG            m_dir[1]{};
    DICONSTANTFORCE m_cf{};
    DIEFFECT        m_cfEffect{};

    std::wstring          m_product;
    std::vector<AxisInfo> m_axes;
    DIJOYSTATE2           m_state{};
    int  m_buttons  = 0;
    bool m_hasFFB   = false;
    bool m_acquired = false;
};
