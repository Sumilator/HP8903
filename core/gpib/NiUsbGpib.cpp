// SPDX-License-Identifier: GPL-2.0-only
//
// User-space NI GPIB-USB-HS / HS+ driver. Protocol ported from the Linux
// kernel driver drivers/gpib/ni_usb/ni_usb_gpib.c, (C) 2004 Frank Mori Hess.
// Function names in comments refer to that file so the two can be compared.

#include "NiUsbGpib.h"

#include <libusb.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace gpib {

namespace {

// ---- USB endpoints (ni_usb_gpib.h) ---------------------------------------
constexpr uint8_t kHsBulkOut = 0x02, kHsBulkIn = 0x84;
constexpr uint8_t kHsPlusBulkOut = 0x01, kHsPlusBulkIn = 0x82;

// ---- vendor control requests ---------------------------------------------
constexpr uint8_t kStopRequest = 0x20;
constexpr uint8_t kWaitRequest = 0x21;
constexpr uint8_t kPollReadyRequest = 0x40;
constexpr uint8_t kSerialNumberRequest = 0x41;
constexpr uint8_t kHsPlus48Request = 0x48;
constexpr uint8_t kHsPlusLedRequest = 0x4b;
constexpr uint8_t kHsPlusF8Request = 0xf8;

// ---- bulk program block ids ----------------------------------------------
constexpr uint8_t kIdIbcac = 0x01;       // take control
constexpr uint8_t kIdTerm = 0x04;        // end of program
constexpr uint8_t kIdIbgts = 0x06;       // go to standby
constexpr uint8_t kIdRegWrite = 0x09;
constexpr uint8_t kIdIbsic = 0x0f;       // interface clear
constexpr uint8_t kIdRead = 0x0a;
constexpr uint8_t kIdCommand = 0x0c;
constexpr uint8_t kIdWrite = 0x0d;
constexpr uint8_t kIdIbrdData = 0x36;
constexpr uint8_t kIdIbrdExtData = 0x37;
constexpr uint8_t kIdIbrdStatus = 0x38;

// ---- adapter error codes --------------------------------------------------
enum : uint8_t {
    kErrNone = 0, kErrAborted = 1, kErrAtnState = 2, kErrAddressing = 3,
    kErrEosMode = 4, kErrNoBus = 5, kErrNoListener = 8, kErrTimeout = 10,
};

// ---- register sub-devices -------------------------------------------------
constexpr uint8_t kTnt = 1;       // TNT4882 GPIB chip
constexpr uint8_t kUnknown2 = 2;
constexpr uint8_t kUnknown3 = 3;

// ---- TNT4882 / NEC7210 registers (tnt4882_registers.h, nec7210_registers.h)
// NEC7210 register n sits at TNT4882 offset 2n.
constexpr uint8_t nec(int r) { return uint8_t(2 * r); }
constexpr uint8_t R_IMR1 = nec(1), R_IMR2 = nec(2), R_ADMR = nec(4),
                  R_AUXMR = nec(5), R_ADR = nec(6);
constexpr uint8_t R_AUXCR = 0x06, R_HSSEL = 0x0d, R_IMR3 = 0x12,
                  R_KEYREG = 0x17, R_CMDR = 0x1c, R_IMR0 = 0x1d;

// CMDR
constexpr uint8_t CLRSC = 0x02, SETSC = 0x03, SOFT_RESET = 0x22;
// AUXMR commands / register selectors
constexpr uint8_t AUX_PON = 0x00, AUX_CPPF = 0x01, AUX_CR = 0x02,
                  AUX_DSC = 0x14, AUX_CIFC = 0x16, AUX_CREN = 0x17,
                  AUX_SREN = 0x1f, AUX_HLDI = 0x51, AUX_CLEAR_END = 0x55;
constexpr uint8_t AUXRA = 0x80, AUXRB = 0xa0, AUXRG = 0x40, AUXRI = 0xe0;
constexpr uint8_t HR_HLDA = 0x01, HR_TRI = 0x04, NTNL_BIT = 0x08,
                  SISB = 0x01, USTD = 0x08, MSTD = 0x20;
constexpr uint8_t TNT_ONE_CHIP_BIT = 0x01, TNT_IMR0_ALWAYS_BITS = 0x80;
// ADR / ADMR bits (secondary addressing disabled)
constexpr uint8_t HR_DL = 0x20, HR_DT = 0x40, HR_ARS = 0x80;
constexpr uint8_t HR_ADM0 = 0x01, HR_TRM0 = 0x10, HR_TRM1 = 0x20;

// ---- IEEE-488 interface messages ------------------------------------------
constexpr uint8_t GTL = 0x01, SDC = 0x04;
constexpr uint8_t UNL = 0x3f, UNT = 0x5f;
constexpr uint8_t MLA(int a) { return uint8_t(0x20 | (a & 0x1f)); }
constexpr uint8_t MTA(int a) { return uint8_t(0x40 | (a & 0x1f)); }

constexpr int kBoardAddress = 0;  // our own primary address on the bus

std::string usbErr(int code) {
    return std::string(libusb_error_name(code));
}

}  // namespace

// ===========================================================================
// Packet builders / parsers (pure, unit tested)
// ===========================================================================

namespace niproto {

namespace {
void pad4(std::vector<uint8_t>& v) { while (v.size() % 4) v.push_back(0); }
void term(std::vector<uint8_t>& v) { v.insert(v.end(), {kIdTerm, 0, 0, 0}); }
}  // namespace

uint8_t timeoutCode(double us) {
    // ni_usb_timeout_code: NI's T10us..T1000s table.
    if (us <= 0) return 0xf0;
    static const struct { double lim; uint8_t code; } t[] = {
        {10, 0xf1}, {30, 0xf2}, {100, 0xf3}, {300, 0xf4}, {1e3, 0xf5},
        {3e3, 0xf6}, {1e4, 0xf7}, {3e4, 0xf8}, {1e5, 0xf9}, {3e5, 0xfa},
        {1e6, 0xfb}, {3e6, 0xfc}, {1e7, 0xfd}, {3e7, 0xfe}, {1e8, 0xff},
        {3e8, 0x01}, {1e9, 0x02}};
    for (auto& e : t)
        if (us <= e.lim) return e.code;
    return 0x02;
}

Status status(const uint8_t* p) {
    // ni_usb_parse_status_block: the count is sent as a two's complement.
    Status s{};
    s.id = p[0];
    s.ibsta = uint16_t((p[1] << 8) | p[2]);
    s.error = p[3];
    uint16_t c = uint16_t(p[4] | (p[5] << 8));
    s.count = uint16_t(~c + 1);
    return s;
}

std::vector<uint8_t> regWrite(const std::vector<Reg>& regs) {
    // ni_usb_write_registers
    std::vector<uint8_t> out = {kIdRegWrite, uint8_t(regs.size()), 0};
    for (auto& r : regs) out.insert(out.end(), {r.device, r.address, r.value});
    pad4(out);
    term(out);
    return out;
}

std::vector<uint8_t> command(const uint8_t* bytes, size_t len, uint8_t tcode) {
    // ni_usb_command_chunk (len <= 16)
    std::vector<uint8_t> out = {kIdCommand, uint8_t(~(len - 1)), 0, tcode};
    out.insert(out.end(), bytes, bytes + len);
    pad4(out);
    term(out);
    return out;
}

std::vector<uint8_t> write(const uint8_t* data, size_t len, bool eoi, uint8_t tcode) {
    // ni_usb_write
    uint16_t cc = uint16_t(~(len - 1));
    std::vector<uint8_t> out = {kIdWrite, uint8_t(cc & 0xff), uint8_t(cc >> 8),
                                tcode, 0, 0, uint8_t(eoi ? 0x08 : 0), 0};
    out.insert(out.end(), data, data + len);
    pad4(out);
    term(out);
    return out;
}

std::vector<uint8_t> read(size_t len, uint8_t tcode) {
    // ni_usb_read: EOS disabled; then holdoff + clear END register writes.
    uint16_t cc = uint16_t(~(len - 1));
    std::vector<uint8_t> out = {kIdRead, 0, 0, tcode, uint8_t(cc & 0xff),
                                uint8_t(cc >> 8), 0, 0,
                                kIdRegWrite, 2, 0,
                                kTnt, R_AUXMR, AUX_HLDI,
                                kTnt, R_AUXMR, AUX_CLEAR_END};
    pad4(out);
    term(out);
    return out;
}

std::vector<uint8_t> simple(uint8_t id, uint8_t arg) {
    std::vector<uint8_t> out = {id, arg, 0, 0};
    term(out);
    return out;
}

bool parseRead(const uint8_t* in, size_t got, uint8_t* buf, size_t len,
               size_t* bytes, Status* st) {
    // parse_board_ibrd_readback: data in 16-byte blocks (0x36 + 15 bytes) or
    // 32-byte blocks (0x37, 0x00 + 30 bytes), then a 12-byte ibrd status.
    size_t i = 0, j = 0, blockLen = 0, blocks = 0;
    while (i < got && (in[i] == kIdIbrdData || in[i] == kIdIbrdExtData)) {
        if (in[i] == kIdIbrdData) { blockLen = 15; i += 1; }
        else { blockLen = 30; i += 2; }
        if (i + blockLen > got) return false;
        for (size_t k = 0; k < blockLen; ++k, ++i)
            if (j < len) buf[j++] = in[i];
        ++blocks;
    }
    if (i + 12 > got) return false;
    *st = status(&in[i]);
    if (st->id != kIdIbrdStatus) return false;
    i += 9;   // 8-byte status block + 1 unused byte
    size_t actual = blocks ? (blocks - 1) * blockLen + in[i] : 0;
    *bytes = std::min(actual, j);
    return true;
}

}  // namespace niproto
// ===========================================================================
// Enumeration
// ===========================================================================

std::vector<NiUsbBoard::Found> NiUsbBoard::enumerate() {
    std::vector<Found> out;
    libusb_context* ctx = nullptr;
    if (libusb_init(&ctx) != 0) return out;
    libusb_device** list = nullptr;
    ssize_t n = libusb_get_device_list(ctx, &list);
    for (ssize_t i = 0; i < n; ++i) {
        libusb_device_descriptor d{};
        if (libusb_get_device_descriptor(list[i], &d) != 0) continue;
        if (d.idVendor != kVendorNI) continue;
        if (d.idProduct != kProductHS && d.idProduct != kProductHSPlus &&
            d.idProduct != kProductB && d.idProduct != kProductBPreInit)
            continue;
        Found f{d.idProduct, "", libusb_get_bus_number(list[i]),
                libusb_get_port_number(list[i])};
        libusb_device_handle* h = nullptr;
        if (d.iSerialNumber && libusb_open(list[i], &h) == 0) {
            unsigned char s[128] = {0};
            if (libusb_get_string_descriptor_ascii(h, d.iSerialNumber, s,
                                                   sizeof s) > 0)
                f.serial = reinterpret_cast<char*>(s);
            libusb_close(h);
        }
        out.push_back(f);
    }
    if (list) libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    return out;
}

// ===========================================================================
// Open / close
// ===========================================================================

NiUsbBoard::NiUsbBoard() = default;
NiUsbBoard::~NiUsbBoard() { close(); }

std::string NiUsbBoard::productName() const {
    switch (product_) {
        case kProductHS: return "NI GPIB-USB-HS";
        case kProductHSPlus: return "NI GPIB-USB-HS+";
        case kProductB: return "NI GPIB-USB-B";
        default: return "NI GPIB-USB (unknown)";
    }
}

void NiUsbBoard::fail(const std::string& what, int code) const {
    std::string msg = "NI GPIB-USB: " + what;
    if (code) msg += " (" + usbErr(code) + ")";
    if (code == LIBUSB_ERROR_TIMEOUT) throw Timeout(msg);
    throw Error(msg);
}

void NiUsbBoard::open(const std::string& serial) {
    if (handle_) return;
    int r = libusb_init(&ctx_);
    if (r != 0) fail("could not start USB access (libusb_init)", r);

    libusb_device** list = nullptr;
    ssize_t n = libusb_get_device_list(ctx_, &list);
    libusb_device* chosen = nullptr;
    uint16_t pid = 0;
    bool sawBlankUsbB = false, sawUsbB = false;
    for (ssize_t i = 0; i < n && !chosen; ++i) {
        libusb_device_descriptor d{};
        if (libusb_get_device_descriptor(list[i], &d) != 0) continue;
        if (d.idVendor != kVendorNI) continue;
        if (d.idProduct == kProductBPreInit) { sawBlankUsbB = true; continue; }
        if (d.idProduct == kProductB) { sawUsbB = true; continue; }
        if (d.idProduct != kProductHS && d.idProduct != kProductHSPlus) continue;
        if (!serial.empty()) {
            libusb_device_handle* h = nullptr;
            bool match = false;
            if (d.iSerialNumber && libusb_open(list[i], &h) == 0) {
                unsigned char s[128] = {0};
                if (libusb_get_string_descriptor_ascii(h, d.iSerialNumber, s,
                                                       sizeof s) > 0)
                    match = serial == reinterpret_cast<char*>(s);
                libusb_close(h);
            }
            if (!match) continue;
        }
        chosen = list[i];
        pid = d.idProduct;
    }
    if (chosen) r = libusb_open(chosen, &handle_);
    if (list) libusb_free_device_list(list, 1);

    if (!chosen) {
        libusb_exit(ctx_);
        ctx_ = nullptr;
        if (sawBlankUsbB || sawUsbB)
            throw Error("Found an NI GPIB-USB-B. This model needs NI's "
                        "firmware loaded at every power-up; it is not "
                        "supported yet (GPIB-USB-HS and HS+ are).");
        throw Error("No NI GPIB-USB-HS adapter found. Check the USB cable, "
                    "and on a Mac make sure NI-488.2 is not installed "
                    "(its kernel extension would claim the adapter).");
    }
    if (r != 0) {
        libusb_exit(ctx_);
        ctx_ = nullptr;
        fail("could not open the USB device"
#ifdef _WIN32
             " - is the WinUSB driver installed for it?"
#endif
             , r);
    }
    product_ = pid;
    if (pid == kProductHSPlus) { epOut_ = kHsPlusBulkOut; epIn_ = kHsPlusBulkIn; }
    else { epOut_ = kHsBulkOut; epIn_ = kHsBulkIn; }

    libusb_set_auto_detach_kernel_driver(handle_, 1);  // Linux only, harmless
    int cfg = 0;
    if (libusb_get_configuration(handle_, &cfg) == 0 && cfg != 1)
        libusb_set_configuration(handle_, 1);
    r = libusb_claim_interface(handle_, 0);
    if (r != 0) {
        libusb_close(handle_);
        handle_ = nullptr;
        libusb_exit(ctx_);
        ctx_ = nullptr;
        fail("could not claim the adapter's USB interface - is another "
             "program (or NI-488.2) using it?", r);
    }

    try {
        waitForReady();                          // ni_usb_hs_wait_for_ready
        if (pid == kProductHSPlus) hsPlusExtraInit();
        setInterruptMonitor(0);                  // ni_usb_set_interrupt_monitor
        initChip();                              // ni_usb_init
    } catch (...) {
        close();
        throw;
    }
}

void NiUsbBoard::close() {
    if (handle_) {
        // ni_usb_shutdown_hardware: chip reset, release the bus.
        try {
            writeRegisters({{kTnt, R_AUXMR, AUX_CR}, {kUnknown3, 0x10, 0x00}});
        } catch (...) {}
        libusb_release_interface(handle_, 0);
        libusb_close(handle_);
        handle_ = nullptr;
    }
    if (ctx_) {
        libusb_exit(ctx_);
        ctx_ = nullptr;
    }
}

// ===========================================================================
// USB primitives
// ===========================================================================

void NiUsbBoard::bulkOut(const std::vector<uint8_t>& data, unsigned timeoutMs) {
    int done = 0;
    int r = libusb_bulk_transfer(handle_, epOut_,
                                 const_cast<uint8_t*>(data.data()),
                                 int(data.size()), &done, timeoutMs);
    if (r != 0) fail("bulk write failed", r);
    if (done != int(data.size())) fail("short bulk write");
}

size_t NiUsbBoard::bulkIn(uint8_t* buf, size_t len, unsigned timeoutMs) {
    int done = 0;
    int r = libusb_bulk_transfer(handle_, epIn_, buf, int(len), &done, timeoutMs);
    if (r != 0) fail("bulk read failed", r);
    return size_t(done);
}

int NiUsbBoard::controlIn(uint8_t request, uint16_t value, uint16_t index,
                          uint8_t* buf, uint16_t len, unsigned timeoutMs,
                          bool recipientInterface) {
    uint8_t type = LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR |
                   (recipientInterface ? LIBUSB_RECIPIENT_INTERFACE
                                       : LIBUSB_RECIPIENT_DEVICE);
    int r = libusb_control_transfer(handle_, type, request, value, index, buf,
                                    len, timeoutMs);
    if (r < 0) fail("control request 0x" + [&] {
        char s[8]; std::snprintf(s, sizeof s, "%02x", request); return std::string(s);
    }() + " failed", r);
    return r;
}

unsigned NiUsbBoard::usbTimeoutMs() const {
    // Give the adapter time to report its own GPIB timeout first.
    return unsigned(timeoutUs_ / 1000.0) + 2000;
}

// ===========================================================================
// Bring-up
// ===========================================================================

void NiUsbBoard::waitForReady() {
    uint8_t buf[16] = {0};
    int n = controlIn(kSerialNumberRequest, 0, 0, buf, sizeof buf, 1000);
    if (n >= 5 && buf[0] == kSerialNumberRequest)
        boardSerial_ = uint32_t(buf[1]) | (uint32_t(buf[2]) << 8) |
                       (uint32_t(buf[3]) << 16) | (uint32_t(buf[4]) << 24);

    // Poll until the adapter reports ready: any of bytes 6, 7 or 10 non-zero.
    for (int i = 0; i < 50; ++i) {
        std::memset(buf, 0, sizeof buf);
        n = controlIn(kPollReadyRequest, 0, 0, buf, sizeof buf, 100);
        if (n >= 11 && (buf[6] || buf[7] || buf[10])) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    throw Error("NI GPIB-USB: adapter did not become ready");
}

void NiUsbBoard::hsPlusExtraInit() {
    // As observed from NI's Windows driver; stops the LED blinking.
    uint8_t buf[16];
    controlIn(kHsPlus48Request, 0, 0, buf, 16, 1000);
    controlIn(kHsPlusLedRequest, 1, 0, buf, 2, 1000);
    controlIn(kHsPlusF8Request, 0, 1, buf, 9, 1000, true);
}

void NiUsbBoard::setInterruptMonitor(unsigned mask) {
    uint8_t buf[8];
    controlIn(kWaitRequest, 0x300, uint16_t(mask), buf, sizeof buf, 1000);
}

void NiUsbBoard::initChip() {
    // ni_usb_setup_init, for: system controller, primary address 0,
    // no secondary address, 500 ns T1 delay, EOS disabled.
    std::vector<Reg> w = {
        {kUnknown3, 0x10, 0x00},
        {kTnt, R_CMDR, SOFT_RESET},
        {kTnt, R_AUXMR, uint8_t(AUXRA | HR_HLDA)},
        {kTnt, R_AUXCR, uint8_t(AUXRA | HR_HLDA)},
        {kTnt, R_HSSEL, TNT_ONE_CHIP_BIT},
        {kTnt, R_AUXMR, AUX_CR},
        {kTnt, R_IMR0, TNT_IMR0_ALWAYS_BITS},
        {kTnt, R_IMR1, 0x00},
        {kTnt, R_IMR2, 0x00},
        {kTnt, R_IMR3, 0x00},
        {kTnt, R_AUXMR, AUX_HLDI},
        // ni_usb_setup_t1_delay(500 ns)
        {kTnt, R_AUXMR, uint8_t(AUXRI | USTD | SISB)},
        {kTnt, R_AUXMR, uint8_t(AUXRB | HR_TRI)},
        {kTnt, R_KEYREG, 0x00},
        {kTnt, R_AUXMR, uint8_t(AUXRG | NTNL_BIT)},
        {kTnt, R_CMDR, SETSC},
        {kTnt, R_AUXMR, AUX_CIFC},
        {kTnt, R_ADR, kBoardAddress},
        {kUnknown2, 0x00, kBoardAddress},
        // ni_usb_write_sad(disabled)
        {kTnt, R_ADR, uint8_t(HR_ARS | HR_DT | HR_DL)},
        {kTnt, R_ADMR, uint8_t(HR_TRM0 | HR_TRM1 | HR_ADM0)},
        {kUnknown2, 0x01, 0x00},
        {kUnknown2, 0x02, 0xfd},
        {kTnt, 0x0f, 0x11},
        {kTnt, R_AUXMR, AUX_PON},
        {kTnt, R_AUXMR, AUX_CPPF},
    };
    (void)MSTD;
    (void)CLRSC; (void)AUX_DSC;
    writeRegisters(w, &lastStatus_);
}

// ===========================================================================
// Programs
// ===========================================================================

void NiUsbBoard::writeRegisters(const std::vector<Reg>& regs, unsigned* ibsta) {
    std::lock_guard<std::mutex> lk(transfer_);
    bulkOut(niproto::regWrite(regs), 1000);
    uint8_t in[32] = {0};
    size_t n = bulkIn(in, sizeof in, 1000);
    if (n != 16) fail("unexpected register-write reply length " + std::to_string(n));
    StatusBlock s = niproto::status(in);
    if (s.id != kIdRegWrite) fail("register-write reply has wrong id");
    if (s.error) fail("register write error " + std::to_string(s.error));
    if (in[8] != regs.size()) fail("not all register writes completed");
    if (ibsta) *ibsta = s.ibsta;
}

void NiUsbBoard::interfaceClear() {
    // ni_usb_request_system_control(1) then ni_usb_interface_clear
    writeRegisters({{kTnt, R_CMDR, SETSC}, {kTnt, R_AUXMR, AUX_CIFC}});
    std::lock_guard<std::mutex> lk(transfer_);
    bulkOut(niproto::simple(kIdIbsic), 1000);
    uint8_t in[16];
    if (bulkIn(in, sizeof in, 1000) != 12) fail("bad IFC reply");
    lastStatus_ = niproto::status(in).ibsta;
}

void NiUsbBoard::remoteEnable(bool on) {
    writeRegisters({{kTnt, R_AUXMR, on ? AUX_SREN : AUX_CREN}}, &lastStatus_);
}

void NiUsbBoard::takeControl(bool synchronous) {
    std::lock_guard<std::mutex> lk(transfer_);
    bulkOut(niproto::simple(kIdIbcac, synchronous ? 1 : 0), 1000);
    uint8_t in[16];
    if (bulkIn(in, sizeof in, 1000) != 12) fail("bad take-control reply");
    lastStatus_ = niproto::status(in).ibsta;
}

void NiUsbBoard::goToStandby() {
    std::lock_guard<std::mutex> lk(transfer_);
    bulkOut(niproto::simple(kIdIbgts), 1000);
    uint8_t in[32];
    if (bulkIn(in, sizeof in, 1000) != 12) fail("bad standby reply");
    lastStatus_ = niproto::status(in).ibsta;
}

void NiUsbBoard::command(const std::vector<uint8_t>& bytes) {
    // ni_usb_command: at most 16 bytes per program.
    size_t pos = 0;
    while (pos < bytes.size()) {
        size_t len = std::min<size_t>(16, bytes.size() - pos);
        StatusBlock s;
        {
            std::lock_guard<std::mutex> lk(transfer_);
            bulkOut(niproto::command(bytes.data() + pos, len, timeoutCode()), usbTimeoutMs());
            uint8_t in[16];
            if (bulkIn(in, sizeof in, usbTimeoutMs()) != 12) fail("bad command reply");
            s = niproto::status(in);
        }
        lastStatus_ = s.ibsta;
        switch (s.error) {
            case kErrNone: break;
            case kErrNoBus:
                throw Error("GPIB: no instrument is responding on the bus - "
                            "is the HP 8903 switched on and the cable fitted?");
            case kErrTimeout: throw Timeout("GPIB: command timed out");
            default: fail("command error " + std::to_string(s.error));
        }
        if (s.count >= len) fail("command made no progress");
        pos += len - s.count;
    }
}

size_t NiUsbBoard::writeData(const uint8_t* data, size_t len, bool eoi) {
    if (len == 0 || len > 0xffff) throw Error("GPIB: bad write length");
    StatusBlock s;
    {
        std::lock_guard<std::mutex> lk(transfer_);
        bulkOut(niproto::write(data, len, eoi, timeoutCode()), usbTimeoutMs());
        uint8_t in[16];
        if (bulkIn(in, sizeof in, usbTimeoutMs()) != 12) fail("bad write reply");
        s = niproto::status(in);
    }
    lastStatus_ = s.ibsta;
    switch (s.error) {
        case kErrNone: break;
        case kErrNoListener:
            throw Error("GPIB: no listener - check the instrument's GPIB address");
        case kErrAddressing: throw Error("GPIB: addressing error on write");
        case kErrTimeout: throw Timeout("GPIB: write timed out");
        case kErrAborted: throw Error("GPIB: write aborted");
        default: fail("write error " + std::to_string(s.error));
    }
    return len - s.count;
}

size_t NiUsbBoard::readData(uint8_t* buf, size_t len, bool* gotEnd) {
    if (len == 0 || len > 0xffff) throw Error("GPIB: bad read length");
    std::vector<uint8_t> in((len / 30 + 1) * 0x20 + 0x20);
    size_t got;
    {
        std::lock_guard<std::mutex> lk(transfer_);
        bulkOut(niproto::read(len, timeoutCode()), 1000);
        got = bulkIn(in.data(), in.size(), usbTimeoutMs());
    }
    size_t actual = 0;
    StatusBlock s{};
    if (!niproto::parseRead(in.data(), got, buf, len, &actual, &s))
        fail("malformed read reply (" + std::to_string(got) + " bytes)");
    lastStatus_ = s.ibsta;
    switch (s.error) {
        case kErrNone: break;
        case kErrTimeout:
            throw Timeout("GPIB: no reply from the instrument within " +
                          std::to_string(int(timeout() + 0.5)) + " s");
        case kErrAborted: throw Error("GPIB: read aborted");
        case kErrAddressing: throw Error("GPIB: addressing error on read");
        case kErrAtnState: throw Error("GPIB: read attempted with ATN asserted");
        default: fail("read error " + std::to_string(s.error));
    }
    if (gotEnd) *gotEnd = (s.ibsta & END) != 0;
    return actual;
}
void NiUsbBoard::stop() {
    if (!handle_) return;
    uint8_t buf[8];
    // Deliberately not taking transfer_: this interrupts a pending read.
    libusb_control_transfer(handle_,
                            LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR |
                                LIBUSB_RECIPIENT_DEVICE,
                            kStopRequest, 0, 0, buf, sizeof buf, 1000);
}

unsigned NiUsbBoard::status() {
    uint8_t buf[8];
    if (controlIn(kWaitRequest, 0x200, 0, buf, sizeof buf, 1000) == 8)
        lastStatus_ = niproto::status(buf).ibsta;
    return lastStatus_;
}

// ===========================================================================
// NiUsbTransport: device-level operations, the way linux-gpib's ibwrt /
// ibrd / ibclr / ibloc address a device.
// ===========================================================================

NiUsbTransport::NiUsbTransport(std::string serial, int address)
    : serial_(std::move(serial)) {
    address_ = address;
}

NiUsbTransport::~NiUsbTransport() { close(); }

void NiUsbTransport::open() {
    board_.open(serial_);
    board_.interfaceClear();      // become controller-in-charge
    board_.remoteEnable(true);    // assert REN
    board_.setTimeout(3.0);
}

void NiUsbTransport::close() {
    if (!board_.isOpen()) return;
    try { goToLocal(); } catch (...) {}
    try { board_.remoteEnable(false); } catch (...) {}
    board_.close();
}

void NiUsbTransport::addressAsListener() {
    board_.takeControl(true);
    board_.command({UNL, MLA(address_), MTA(kBoardAddress)});
    board_.goToStandby();
}

void NiUsbTransport::addressAsTalker() {
    board_.takeControl(true);
    board_.command({UNT, UNL, MLA(kBoardAddress), MTA(address_)});
    board_.goToStandby();
}

void NiUsbTransport::clear() {
    board_.takeControl(true);
    board_.command({UNL, MLA(address_), SDC, UNL});
}

void NiUsbTransport::goToLocal() {
    board_.takeControl(true);
    board_.command({UNL, MLA(address_), GTL, UNL});
}

void NiUsbTransport::write(const std::string& cmd) {
    board_.setTimeout(3.0);
    addressAsListener();
    std::string s = cmd + "\n";
    board_.writeData(reinterpret_cast<const uint8_t*>(s.data()), s.size(), true);
}

std::string NiUsbTransport::read(double timeoutSeconds) {
    board_.setTimeout(timeoutSeconds);
    addressAsTalker();
    std::string result;
    uint8_t buf[64];
    for (int guard = 0; guard < 64; ++guard) {
        bool end = false;
        size_t n = board_.readData(buf, sizeof buf, &end);
        result.append(reinterpret_cast<char*>(buf), n);
        if (end || n < sizeof buf) break;
    }
    return result;
}

std::string NiUsbTransport::describe() const {
    char s[96];
    std::snprintf(s, sizeof s, "%s (serial %08X), GPIB address %d",
                  board_.isOpen() ? board_.productName().c_str() : "NI GPIB-USB",
                  board_.boardSerial(), address_);
    return s;
}

}  // namespace gpib
