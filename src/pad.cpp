#include "pad.h"

#include <ViGEm/Client.h>

std::atomic<unsigned char> VirtualPad::s_large{0};
std::atomic<unsigned char> VirtualPad::s_small{0};

// Called by ViGEmBus on its own thread whenever the game updates rumble.
void CALLBACK padNotify(void* /*client*/, void* /*target*/,
                        unsigned char largeMotor, unsigned char smallMotor,
                        unsigned char /*led*/, void* /*user*/) {
    VirtualPad::s_large.store(largeMotor, std::memory_order_relaxed);
    VirtualPad::s_small.store(smallMotor, std::memory_order_relaxed);
}

VirtualPad::~VirtualPad() { close(); }

bool VirtualPad::open(std::string& err) {
    close();

    auto client = vigem_alloc();
    if (!client) { err = "vigem_alloc failed (out of memory)."; return false; }
    m_client = client;

    VIGEM_ERROR e = vigem_connect(client);
    if (!VIGEM_SUCCESS(e)) {
        err = (e == VIGEM_ERROR_BUS_NOT_FOUND)
            ? "ViGEmBus driver not found. Install it with: "
              "winget install --id ViGEm.ViGEmBus"
            : "vigem_connect failed (code " + std::to_string((int)e) + ").";
        close();
        return false;
    }

    auto target = vigem_target_x360_alloc();
    if (!target) { err = "vigem_target_x360_alloc failed."; close(); return false; }
    m_target = target;

    e = vigem_target_add(client, target);
    if (!VIGEM_SUCCESS(e)) {
        err = "vigem_target_add failed (code " + std::to_string((int)e) + ").";
        close();
        return false;
    }
    m_added = true;

    // Rumble sent by the game arrives through this callback.
    vigem_target_x360_register_notification(
        client, target,
        reinterpret_cast<PFN_VIGEM_X360_NOTIFICATION>(&padNotify), nullptr);

    s_large.store(0);
    s_small.store(0);
    return true;
}

void VirtualPad::close() {
    auto client = static_cast<PVIGEM_CLIENT>(m_client);
    auto target = static_cast<PVIGEM_TARGET>(m_target);

    if (client && target && m_added) {
        vigem_target_x360_unregister_notification(target);
        vigem_target_remove(client, target);
        m_added = false;
    }
    if (target) { vigem_target_free(target); m_target = nullptr; }
    if (client) {
        vigem_disconnect(client);
        vigem_free(client);
        m_client = nullptr;
    }
    s_large.store(0);
    s_small.store(0);
}

void VirtualPad::setButtons(unsigned short f)                    { m_buttons = f; }
void VirtualPad::setTriggers(unsigned char lt, unsigned char rt) { m_lt = lt; m_rt = rt; }
void VirtualPad::setLeftStick(short x, short y)                  { m_lx = x; m_ly = y; }
void VirtualPad::setRightStick(short x, short y)                 { m_rx = x; m_ry = y; }

bool VirtualPad::submit() {
    if (!m_client || !m_target || !m_added) return false;
    XUSB_REPORT r{};
    r.wButtons      = m_buttons;
    r.bLeftTrigger  = m_lt;
    r.bRightTrigger = m_rt;
    r.sThumbLX      = m_lx;
    r.sThumbLY      = m_ly;
    r.sThumbRX      = m_rx;
    r.sThumbRY      = m_ry;
    return VIGEM_SUCCESS(vigem_target_x360_update(
        static_cast<PVIGEM_CLIENT>(m_client),
        static_cast<PVIGEM_TARGET>(m_target), r));
}

void VirtualPad::rumble(unsigned char& largeMotor, unsigned char& smallMotor) const {
    largeMotor = s_large.load(std::memory_order_relaxed);
    smallMotor = s_small.load(std::memory_order_relaxed);
}
