/*
 * virtio-accel: offloading an operation to an accelerator on the host.
 *
 * The host side is QEMU's virtio-accel device (TUM-DSE/lros-qemu,
 * subprojects/vaccel), backed by libvaccel, which dispatches to a plugin for
 * whatever accelerator is actually present -- an RKNN NPU on the Orange Pi, a
 * CUDA GPU elsewhere. What the guest sends is not accelerator-specific: an
 * opcode and a list of argument buffers.
 *
 * This is the transport only. The operation vocabulary -- sessions, matmul,
 * tensor memory -- lives in modules/vaccel, which calls the three entry points
 * below directly. There is no /dev/accel and no ioctl in between; miniOSv has
 * neither, and the Unikraft port's devfs layer exists only to cross a
 * kernel/user boundary that does not exist here.
 */

#ifndef VIRTIO_ACCEL_DRIVER_H
#define VIRTIO_ACCEL_DRIVER_H

#include <atomic>
#include <cstdint>

#include <osv/mutex.h>
#include <osv/sched.hh>
#include <osv/waitqueue.hh>

#include "drivers/device.hh"
#include "drivers/virtio.hh"

namespace virtio {

// --- wire format ---------------------------------------------------------
//
// Field for field what the device expects (virtio_accel.h in the QEMU
// subproject). Only `len` is read out of an argument: the device allocates its
// own buffer and copies from the descriptor, so the pointers below are never
// dereferenced by the host and are carried only because they are part of the
// struct it reads.
//
// Deliberately NOT packed. The device's copies are plain C structs at natural
// alignment, and it advances through the descriptor stream by
// `n * sizeof(struct virtio_accel_arg)` -- 48 bytes, not the 37 the fields add
// up to. Packing these makes the device read the argument arrays at the wrong
// stride and report "gop_arg[0] too short".

enum accel_op_type : uint32_t {
    ACCEL_NO_OP           = 0,
    ACCEL_CREATE_SESSION  = 1,
    ACCEL_DESTROY_SESSION = 2,
    ACCEL_DO_OP           = 3,
};

enum accel_status : uint32_t {
    ACCEL_S_OK      = 0,
    ACCEL_S_ERR     = 1,
    ACCEL_S_BADMSG  = 2,
    ACCEL_S_NOTSUPP = 3,
    ACCEL_S_INVSESS = 4,
};

struct accel_wire_arg {
    uint32_t len;
    uint8_t *buf;
    uint8_t *usr_buf;
    uint8_t *usr_pages;
    uint32_t usr_npages;
    uint8_t padding[5];
};

struct accel_wire_op {
    uint32_t in_nr;
    uint32_t out_nr;
    accel_wire_arg *in;
    accel_wire_arg *out;
};

struct accel_wire_hdr {
    uint32_t sess_id;
    uint32_t op_type;
    accel_wire_op op;
};

// --- driver --------------------------------------------------------------

// The device advances through the stream by these sizes; if they drift, the
// symptom is a mid-stream misparse rather than anything that names the cause.
static_assert(sizeof(accel_wire_arg) == 48, "virtio_accel_arg layout");
static_assert(sizeof(accel_wire_op) == 24, "virtio_accel_op layout");
static_assert(sizeof(accel_wire_hdr) == 32, "virtio_accel_hdr layout");

class accel : public virtio_driver {
public:
    // One argument of an operation. `buf` is the caller's memory; the driver
    // stages it through DMA-able memory itself, so it may live anywhere.
    struct arg {
        void *buf;
        uint32_t len;
    };

    explicit accel(virtio_device &dev);
    virtual ~accel();

    virtual std::string get_name() const override { return "virtio-accel"; }

    static hw_driver *probe(hw_device *dev);

    //! The device, or nullptr if the guest was booted without one.
    static accel *instance() { return _instance; }

    // Each returns 0, or -errno. A device-reported failure other than a
    // transport error comes back as -EIO with the status logged.
    int create_session(uint32_t *sess_id);
    int destroy_session(uint32_t sess_id);

    //! `out` arguments are sent to the host, `in` arguments receive results.
    //! Argument order is the operation's own; modules/vaccel defines it.
    int do_op(uint32_t sess_id, const arg *out, uint32_t out_nr,
              const arg *in, uint32_t in_nr);

private:
    void handle_irq();
    bool ack_irq();

    // The one place a request is built and completed. op_type picks which of
    // the three shapes to send; sess_id_out is only used by CREATE_SESSION.
    int request(uint32_t op_type, uint32_t sess_id,
                const arg *out, uint32_t out_nr,
                const arg *in, uint32_t in_nr,
                uint32_t *sess_id_out);

    // Takes finished requests off the used ring and wakes their callers.
    void complete_loop();

    static accel *_instance;

    vring *_queue;

    // Several requests may be in flight, so that a graph submitted by one task
    // does not hold back another's: the host can then run them in priority
    // order. Each caller waits on its own entry; the completion thread matches
    // used-ring cookies to entries.
    struct pending {
        sched::thread *caller;
        std::atomic<bool> done{false};
    };
    sched::thread *_completer = nullptr;

    // The ring. Held only while descriptors are added or taken back, never
    // across a request.
    mutex _lock;
    // Woken, under _lock, when descriptors are given back.
    waitqueue _room;
};

}

#endif /* VIRTIO_ACCEL_DRIVER_H */
