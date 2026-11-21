// SPDX-License-Identifier: GPL-2.0-only
//
// Transport: how bytes reach one GPIB instrument.
//
// The instrument layer (HP8903) only ever talks to this interface, so the
// same measurement code runs over our own NI GPIB-USB-HS driver, a Prologix
// / AR488 serial adapter, or the built-in simulator.
#pragma once

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace gpib {

class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Timeout : public Error {
public:
    using Error::Error;
};

class Transport {
public:
    virtual ~Transport() = default;

    // Open the adapter and become controller-in-charge.
    virtual void open() = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;

    // Selected Device Clear to the instrument.
    virtual void clear() = 0;

    // Send a program string. A terminating LF is appended and EOI is
    // asserted on it, which is what the HP 8903 expects.
    virtual void write(const std::string& cmd) = 0;

    // Read one response (terminated by EOI). Throws Timeout.
    virtual std::string read(double timeoutSeconds) = 0;

    // Return the instrument to front-panel (local) control.
    virtual void goToLocal() = 0;

    // Abort a read in progress from another thread (best effort).
    virtual void abort() {}

    virtual std::string describe() const = 0;

    int address() const { return address_; }
    void setAddress(int a) { address_ = a; }

protected:
    int address_ = 28;  // HP 8903 factory default
};

struct AdapterInfo {
    std::string kind;     // "ni-usb", "prologix", "mock"
    std::string name;     // human readable
    std::string id;       // opaque id used to open it
};

// Lists every adapter this build can drive that is currently attached.
std::vector<AdapterInfo> listAdapters();

// Spec strings: "ni-usb", "ni-usb:<serial>", "mock"
std::unique_ptr<Transport> makeTransport(const std::string& spec, int address);

}  // namespace gpib
