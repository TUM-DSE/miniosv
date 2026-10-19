/*
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <stdexcept>

#include <osv/aligned_new.hh>
#include <osv/debug.h>

#include "drivers/iavf/iavf.hh"
#include "drivers/iavf/iavf_ethdev.hh"

extern bool opt_maxnic;
extern int maxnic;

namespace intel {

iavf::iavf(pci::device& dev)
    : _dev(dev)
{
    if (iavf_attach(&_dev, &_edev) != 0) {
        throw std::runtime_error("Failed to attach iavf device");
    }
}

void iavf::dump_config(void)
{
    u8 B, D, F;
    _dev.get_bdf(B, D, F);

    _dev.dump_config();
    tprintf_d("iavf", "%s [%x:%x.%x] vid:id= %x:%x", get_name().c_str(),
        (u16)B, (u16)D, (u16)F, _dev.get_vendor_id(), _dev.get_device_id());
}

hw_driver* iavf::probe(hw_device* dev)
{
    try {
        if (auto pci_dev = dynamic_cast<pci::device*>(dev)) {
            if (iavf_probe(pci_dev)) {
                if (opt_maxnic && maxnic-- <= 0) {
                    return nullptr;
                }
                return aligned_new<iavf>(*pci_dev);
            }
        }
    } catch (std::exception& e) {
        tprintf_e("iavf", "Exception on device construction: %s", e.what());
    }
    return nullptr;
}

}
