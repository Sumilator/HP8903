// SPDX-License-Identifier: GPL-2.0-only
//
// User-space driver for National Instruments GPIB-USB-HS / HS+ adapters.
//
// No kernel extension and no NI software are needed: the adapter is driven
// directly over USB with libusb. The wire protocol is ported from the Linux
// kernel driver drivers/gpib/ni_usb/ni_usb_gpib.c
// (C) 2004 Frank Mori Hess, GPL-2.0.
//
// The adapter contains a TNT4882 GPIB controller chip (NEC 7210 compatible).
// Everything is done by sending small "programs" over the bulk OUT endpoint
// (register writes, data writes, command bytes, reads) and parsing the status
// blocks that come back on the bulk IN endpoint.
#pragma once

#include "Transport.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct libusb_context;
struct libusb_device_handle;

namespace gpib {

// Pure packet builders / parsers for the adapter's bulk protocol, kept free
// of USB so they can be unit tested.
namespace niproto {
struct Reg { uint8_t device; uint8_t address; uint8_t value; };
struct Status { uint8_t id; uint16_t ibsta; uint8_t error; uint16_t count; };
uint8_t timeoutCode(double microseconds);
std::vector<uint8_t> regWrite(const std::vector<Reg>& regs);
std::vector<uint8_t> command(const uint8_t* bytes, size_t len, uint8_t tcode);
std::vector<uint8_t> write(const uint8_t* data, size_t len, bool eoi, uint8_t tcode);
std::vector<uint8_t> read(size_t len, uint8_t tcode);
std::vector<uint8_t> simple(uint8_t id, uint8_t arg = 0);   // ibcac, ibgts, ibsic
Status status(const uint8_t* p);
// Parses an ibrd reply; copies data into buf (up to len); returns false on
// malformed input.
bool parseRead(const uint8_t* in, size_t got, uint8_t* buf, size_t len,
               size_t* bytes, Status* st);
}  // namespace niproto

// Low-level board: one adapter, acting as system controller at address 0.
class NiUsbBoard {
public:
    static constexpr uint16_t kVendorNI = 0x3923;
    static constexpr uint16_t kProductHS = 0x709b;
    static constexpr uint16_t kProductHSPlus = 0x7618;
    static constexpr uint16_t kProductB = 0x702a;         // needs firmware
    static constexpr uint16_t kProductBPreInit = 0x702b;  // blank USB-B

    struct Found {
        uint16_t product;
        std::string serial;   // USB serial string, may be empty
        uint8_t bus, port;
    };
    static std::vector<Found> enumerate();

    NiUsbBoard();
    ~NiUsbBoard();
    NiUsbBoard(const NiUsbBoard&) = delete;
    NiUsbBoard& operator=(const NiUsbBoard&) = delete;

    // serial: empty = first adapter found.
    void open(const std::string& serial = "");
    void close();
    bool isOpen() const { return handle_ != nullptr; }

    uint16_t product() const { return product_; }
    uint32_t boardSerial() const { return boardSerial_; }
    std::string productName() const;

    // --- IEEE-488 controller operations -----------------------------------
    void interfaceClear();                 // pulse IFC, become CIC
    void remoteEnable(bool on);            // REN line
    void takeControl(bool synchronous);    // assert ATN
    void goToStandby();                    // release ATN
    // Send interface command bytes with ATN asserted (UNL, MLA, SDC ...)
    void command(const std::vector<uint8_t>& bytes);
    // Write data bytes (ATN released). Returns bytes written.
    size_t writeData(const uint8_t* data, size_t len, bool eoi);
    // Read data until EOI or len. Returns bytes read; *gotEnd set on EOI.
    size_t readData(uint8_t* buf, size_t len, bool* gotEnd);
    // Ask the adapter to stop an operation in progress (thread-safe).
    void stop();
    // Read the adapter's GPIB status word (ibsta bits, see Status below).
    unsigned status();

    void setTimeout(double seconds) { timeoutUs_ = seconds * 1e6; }
    double timeout() const { return timeoutUs_ / 1e6; }

    enum Status : unsigned {
        DCAS = 1u << 0, DTAS = 1u << 1, LACS = 1u << 2, TACS = 1u << 3,
        ATN = 1u << 4, CIC = 1u << 5, REM = 1u << 6, LOK = 1u << 7,
        CMPL = 1u << 8, SRQI = 1u << 12, END = 1u << 13, TIMO = 1u << 14,
        ERR = 1u << 15,
    };

private:
    using Reg = niproto::Reg;
    using StatusBlock = niproto::Status;

    void writeRegisters(const std::vector<Reg>& regs, unsigned* ibsta = nullptr);
    void bulkOut(const std::vector<uint8_t>& data, unsigned timeoutMs);
    size_t bulkIn(uint8_t* buf, size_t len, unsigned timeoutMs);
    int controlIn(uint8_t request, uint16_t value, uint16_t index,
                  uint8_t* buf, uint16_t len, unsigned timeoutMs,
                  bool recipientInterface = false);
    void waitForReady();
    void hsPlusExtraInit();
    void initChip();
    void setInterruptMonitor(unsigned mask);
    uint8_t timeoutCode() const { return niproto::timeoutCode(timeoutUs_); }
    unsigned usbTimeoutMs() const;
    [[noreturn]] void fail(const std::string& what, int code = 0) const;

    libusb_context* ctx_ = nullptr;
    libusb_device_handle* handle_ = nullptr;
    uint16_t product_ = 0;
    uint32_t boardSerial_ = 0;
    uint8_t epOut_ = 0, epIn_ = 0;
    double timeoutUs_ = 3e6;
    unsigned lastStatus_ = 0;
    std::mutex transfer_;   // one bulk program at a time
};

// Transport for one instrument on an NI GPIB-USB adapter.
class NiUsbTransport : public Transport {
public:
    explicit NiUsbTransport(std::string serial = "", int address = 28);
    ~NiUsbTransport() override;

    void open() override;
    void close() override;
    bool isOpen() const override { return board_.isOpen(); }
    void clear() override;
    void write(const std::string& cmd) override;
    std::string read(double timeoutSeconds) override;
    void goToLocal() override;
    void abort() override { board_.stop(); }
    std::string describe() const override;

    NiUsbBoard& board() { return board_; }

private:
    void addressAsListener();
    void addressAsTalker();
    std::string serial_;
    NiUsbBoard board_;
};

}  // namespace gpib
