#pragma once

#include <windows.h>
#include <ViGEm/Common.h>   // XUSB_GAMEPAD_* button flags

#include <atomic>
#include <string>

// <rpcndr.h> defines `small` as `char`, which wrecks any parameter by that
// name. Drop it so this header is safe to include anywhere.
#ifdef small
#  undef small
#endif

// A virtual Xbox 360 pad backed by the ViGEmBus driver.
//
// This is what lets the G29 drive games that have no idea a wheel exists:
// Windows and the game see a standard Xbox controller, nothing is injected
// into the game, and no game file is touched.
//
// The feedback channel runs the other way. XInput offers exactly two bytes --
// large and small rumble motor -- so a game can tell us "something is shaking"
// and nothing more. That is enough for collisions and kerbs, and nowhere near
// enough for slip angle or tyre load.
class VirtualPad {
public:
    VirtualPad() = default;
    ~VirtualPad();
    VirtualPad(const VirtualPad&) = delete;
    VirtualPad& operator=(const VirtualPad&) = delete;

    bool open(std::string& err);
    void close();
    bool isOpen() const { return m_added; }

    void setButtons(unsigned short xusbButtonFlags);
    void setTriggers(unsigned char leftTrigger, unsigned char rightTrigger);
    void setLeftStick(short x, short y);
    void setRightStick(short x, short y);
    bool submit();

    // Most recent rumble the game asked for, 0..255 each.
    void rumble(unsigned char& largeMotor, unsigned char& smallMotor) const;

private:
    void* m_client = nullptr;   // PVIGEM_CLIENT
    void* m_target = nullptr;   // PVIGEM_TARGET
    bool  m_added  = false;

    unsigned short m_buttons = 0;
    unsigned char  m_lt = 0, m_rt = 0;
    short m_lx = 0, m_ly = 0, m_rx = 0, m_ry = 0;

    static std::atomic<unsigned char> s_large;
    static std::atomic<unsigned char> s_small;

    friend void CALLBACK padNotify(void*, void*, unsigned char, unsigned char,
                                   unsigned char, void*);
};
