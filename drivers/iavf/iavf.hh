/*
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

// Intel's adaptive virtual function, as a hw_driver: probe matches the VF device ids, and the constructor brings the
// port up and registers it with minidpdk.

#ifndef IAVF_HH
#define IAVF_HH

#include "drivers/driver.hh"
#include "drivers/pci-device.hh"

class iavf_eth_dev;

namespace intel {

class iavf : public hw_driver {
public:
    explicit iavf(pci::device& dev);
    virtual ~iavf() = default;

    virtual std::string get_name() const { return "iavf"; }

    virtual void dump_config(void);

    static hw_driver* probe(hw_device* dev);

private:
    pci::device& _dev;
    iavf_eth_dev* _edev = nullptr;
};

}

#endif
