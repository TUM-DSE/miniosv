/* Defaults for the conf_drivers_* options; make passes the chosen values. */
#ifndef OSV_DRIVERS_CONFIG_H
#define OSV_DRIVERS_CONFIG_H

#ifndef CONF_drivers_acpi
#define CONF_drivers_acpi 1
#endif
#ifndef CONF_drivers_pci
#define CONF_drivers_pci 1
#endif
#ifndef CONF_drivers_ena
#define CONF_drivers_ena 1
#endif
#ifndef CONF_drivers_nvme
#define CONF_drivers_nvme 1
#endif
#ifndef CONF_drivers_virtio
#define CONF_drivers_virtio 1
#endif
#ifndef CONF_drivers_virtio_accel
#define CONF_drivers_virtio_accel 1
#endif

#endif
