// SPDX-License-Identifier: GPL-2.0-only
#include "Transport.h"

#include "MockTransport.h"
#include "NiUsbGpib.h"

#include <cstdio>

namespace gpib {

std::vector<AdapterInfo> listAdapters() {
    std::vector<AdapterInfo> out;
    for (auto& f : NiUsbBoard::enumerate()) {
        AdapterInfo a;
        a.kind = "ni-usb";
        switch (f.product) {
            case NiUsbBoard::kProductHS: a.name = "NI GPIB-USB-HS"; break;
            case NiUsbBoard::kProductHSPlus: a.name = "NI GPIB-USB-HS+"; break;
            default: a.name = "NI GPIB-USB-B (not supported yet)"; break;
        }
        if (!f.serial.empty()) a.name += "  [" + f.serial + "]";
        a.id = "ni-usb" + (f.serial.empty() ? "" : ":" + f.serial);
        out.push_back(a);
    }
    out.push_back({"mock", "Simulated HP 8903 (no hardware)", "mock"});
    return out;
}

std::unique_ptr<Transport> makeTransport(const std::string& spec, int address) {
    std::string kind = spec, arg;
    if (auto p = spec.find(':'); p != std::string::npos) {
        kind = spec.substr(0, p);
        arg = spec.substr(p + 1);
    }
    if (kind == "ni-usb" || kind == "ni" || kind.empty())
        return std::make_unique<NiUsbTransport>(arg, address);
    if (kind == "mock") return std::make_unique<MockTransport>(address);
    throw Error("Unknown adapter '" + spec + "' (use ni-usb or mock)");
}

}  // namespace gpib
