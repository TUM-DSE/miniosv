/*
 * virtio-accel transport. See drivers/virtio-accel.hh.
 *
 * The descriptor layout is fixed by the device (virtio_accel_handle_request in
 * the QEMU subproject), and it is stricter than "some out buffers, some in
 * buffers":
 *
 *   out (device-readable), in this order
 *     1. the header, alone in its descriptor -- the device consumes exactly
 *        sizeof(hdr) and then takes the next descriptor's base address
 *     2. the out argument array, one contiguous descriptor, if out_nr > 0
 *     3. the in argument array, one contiguous descriptor, if in_nr > 0
 *        (device-readable: the device needs the lengths, not the data)
 *     4. each out argument's data, one descriptor each
 *
 *   in (device-writable), in this order
 *     5. each in argument's data, one descriptor each
 *     6. for CREATE_SESSION, the session id
 *     7. the status word, last, and exactly 4 bytes -- the device checks this
 *
 * Everything is staged through one physically contiguous allocation. The
 * caller's buffers come from wherever the application allocates (ggml uses
 * mmap), and a vring descriptor has to name a physical address, so they would
 * otherwise have to be linearly mapped. Copying also keeps the argument arrays
 * contiguous, which the device requires.
 */

#include "drivers/virtio-accel.hh"

#include <cerrno>
#include <cstring>
#include <vector>

#include <cstdio>

#include <osv/debug.hh>
#include <osv/sched.hh>
#include <osv/mem/frames.hh>
#include <osv/mem/phys.hh>

#include "drivers/virtio-device.hh"

namespace virtio {

accel *accel::_instance = nullptr;

namespace {

// Above every real-time priority an lros worker or arrival thread takes.
constexpr unsigned completer_rt_priority = 200;

// Every descriptor starts here, so keep them apart by the widest alignment the
// structures need. The device reads the argument arrays as packed structs.
const size_t ARG_ALIGN = 16;

size_t align_up(size_t n, size_t a)
{
    return (n + a - 1) & ~(a - 1);
}

// A staging area for one request: physically contiguous, so each piece can be
// handed to the ring by address.
class staging {
public:
    explicit staging(size_t size)
        : _size(size)
        , _pa(mem::frames::alloc(size, ARG_ALIGN))
    {
        _p = _pa ? static_cast<uint8_t *>(mem::map_phys(_pa, size)) : nullptr;
        if (_p) {
            memset(_p, 0, size);
        }
    }
    ~staging()
    {
        if (_p) {
            mem::frames::free(_pa, _size);
        }
    }
    staging(const staging &) = delete;
    staging &operator=(const staging &) = delete;

    explicit operator bool() const { return _p != nullptr; }

    //! Carve `len` bytes off the front, aligned. Returns nullptr if the
    //! reservation was short, which would be a bug in the size calculation.
    uint8_t *take(size_t len)
    {
        const size_t at = align_up(_used, ARG_ALIGN);
        if (at + len > _size) {
            return nullptr;
        }
        _used = at + len;
        return _p + at;
    }

private:
    size_t _size;
    mem::frames::phys_addr _pa;
    size_t _used = 0;
    uint8_t *_p;
};

} // namespace

accel::accel(virtio_device &dev)
    : virtio_driver(dev)
{
    setup_features();
    probe_virt_queues();

    _queue = get_virt_queue(0);

    // Pinned where the driver is set up, like every other thread: nothing
    // moves a thread between cpus here.
    _completer = sched::thread::make([this] { complete_loop(); },
                                     sched::thread::attr().name("virtio-accel")
                                         .pin(sched::cpu::current()));

    interrupt_factory int_factory;
    int_factory.register_msi_bindings = [this](interrupt_manager &msi) {
        msi.easy_register({{0, [this] {
                                this->_queue->disable_interrupts();
                                this->_completer->wake_with_irq_disabled();
                            },
                            nullptr}});
    };
    _dev.register_interrupt(int_factory);
    // Above any thread that waits on it: a waiter's siblings may spin on this
    // cpu at a real-time priority until the reply comes.
    _completer->set_realtime_priority(completer_rt_priority);
    _completer->start();

    add_dev_status(VIRTIO_CONFIG_S_DRIVER_OK);

    _instance = this;
    // printf, not debug(): debug() only reaches the console when the global
    // verbose flag is set, and the other drivers here announce themselves.
    printf("virtio-accel: attached, one queue of %d\n",
           (int)_queue->size());
}

accel::~accel()
{
    if (_instance == this) {
        _instance = nullptr;
    }
}

hw_driver *accel::probe(hw_device *dev)
{
    return virtio::probe<accel, VIRTIO_ID_ACCEL>(dev);
}

int accel::create_session(uint32_t *sess_id)
{
    if (!sess_id) {
        return -EINVAL;
    }
    return request(ACCEL_CREATE_SESSION, 0, nullptr, 0, nullptr, 0, sess_id);
}

int accel::destroy_session(uint32_t sess_id)
{
    return request(ACCEL_DESTROY_SESSION, sess_id, nullptr, 0, nullptr, 0,
                   nullptr);
}

int accel::do_op(uint32_t sess_id, const arg *out, uint32_t out_nr,
                 const arg *in, uint32_t in_nr)
{
    return request(ACCEL_DO_OP, sess_id, out, out_nr, in, in_nr, nullptr);
}

int accel::request(uint32_t op_type, uint32_t sess_id,
                   const arg *out, uint32_t out_nr,
                   const arg *in, uint32_t in_nr,
                   uint32_t *sess_id_out)
{
    const bool want_sess_id = (sess_id_out != nullptr);

    // Size the staging area: header, the two argument arrays, every argument's
    // data, the session id and the status, each aligned.
    size_t need = align_up(sizeof(accel_wire_hdr), ARG_ALIGN);
    if (out_nr) {
        need += align_up(out_nr * sizeof(accel_wire_arg), ARG_ALIGN);
    }
    if (in_nr) {
        need += align_up(in_nr * sizeof(accel_wire_arg), ARG_ALIGN);
    }
    for (uint32_t i = 0; i < out_nr; i++) {
        need += align_up(out[i].len, ARG_ALIGN);
    }
    for (uint32_t i = 0; i < in_nr; i++) {
        need += align_up(in[i].len, ARG_ALIGN);
    }
    // The device writes the session id with a 64-bit store even though the
    // value is 32 bits, so reserve eight bytes for it rather than let it run
    // into whatever follows.
    need += align_up(sizeof(uint64_t), ARG_ALIGN);
    need += align_up(sizeof(uint32_t), ARG_ALIGN);

    staging buf(need);
    if (!buf) {
        return -ENOMEM;
    }

    auto *hdr = reinterpret_cast<accel_wire_hdr *>(
        buf.take(sizeof(accel_wire_hdr)));
    accel_wire_arg *out_args = nullptr;
    accel_wire_arg *in_args = nullptr;
    if (out_nr) {
        out_args = reinterpret_cast<accel_wire_arg *>(
            buf.take(out_nr * sizeof(accel_wire_arg)));
    }
    if (in_nr) {
        in_args = reinterpret_cast<accel_wire_arg *>(
            buf.take(in_nr * sizeof(accel_wire_arg)));
    }

    std::vector<uint8_t *> out_data(out_nr);
    std::vector<uint8_t *> in_data(in_nr);
    for (uint32_t i = 0; i < out_nr; i++) {
        out_data[i] = buf.take(out[i].len);
        out_args[i].len = out[i].len;
        memcpy(out_data[i], out[i].buf, out[i].len);
    }
    for (uint32_t i = 0; i < in_nr; i++) {
        in_data[i] = buf.take(in[i].len);
        in_args[i].len = in[i].len;
    }

    auto *sid = reinterpret_cast<uint64_t *>(buf.take(sizeof(uint64_t)));
    auto *status = reinterpret_cast<uint32_t *>(buf.take(sizeof(uint32_t)));
    if (!hdr || !sid || !status) {
        return -ENOMEM;  // the reservation above was wrong
    }

    hdr->sess_id = sess_id;
    hdr->op_type = op_type;
    hdr->op.out_nr = out_nr;
    hdr->op.in_nr = in_nr;
    hdr->op.out = out_args;
    hdr->op.in = in_args;
    *status = ACCEL_S_ERR;

    pending p;
    p.caller = sched::thread::current();
    WITH_LOCK(_lock) {
        for (;;) {
            _queue->init_sg();

            _queue->add_out_sg(hdr, sizeof(*hdr));
            if (out_nr) {
                _queue->add_out_sg(out_args, out_nr * sizeof(accel_wire_arg));
            }
            if (in_nr) {
                _queue->add_out_sg(in_args, in_nr * sizeof(accel_wire_arg));
            }
            for (uint32_t i = 0; i < out_nr; i++) {
                _queue->add_out_sg(out_data[i], out[i].len);
            }
            for (uint32_t i = 0; i < in_nr; i++) {
                _queue->add_in_sg(in_data[i], in[i].len);
            }
            if (want_sess_id) {
                _queue->add_in_sg(sid, sizeof(*sid));
            }
            _queue->add_in_sg(status, sizeof(*status));

            if (_queue->add_buf(&p)) {
                break;
            }
            _room.wait(_lock);   // the ring is full of other requests
        }
        _queue->kick();
    }
    sched::thread::wait_until([&p] { return p.done.load(std::memory_order_acquire); });

    if (*status != ACCEL_S_OK) {
        printf("virtio-accel: op %d failed with status %d\n", (int)op_type,
               (int)*status);
        return -EIO;
    }

    // Results land in the staging area; hand them back to the caller.
    for (uint32_t i = 0; i < in_nr; i++) {
        memcpy(in[i].buf, in_data[i], in[i].len);
    }
    if (want_sess_id) {
        *sess_id_out = static_cast<uint32_t>(*sid);
    }
    return 0;
}

void accel::complete_loop()
{
    for (;;) {
        wait_for_queue(_queue, &vring::used_ring_not_empty);
        WITH_LOCK(_lock) {
            u32 len = 0;
            while (auto *p = static_cast<pending *>(_queue->get_buf_elem(&len))) {
                _queue->get_buf_finalize();
                // The entry lives on the caller's stack and is gone as soon as
                // the caller sees it done, so the thread is read first.
                sched::thread *caller = p->caller;
                caller->wake_with([p] { p->done.store(true, std::memory_order_release); });
            }
            _room.wake_all(_lock);
        }
    }
}

void accel::handle_irq()
{
}

bool accel::ack_irq()
{
    return _dev.read_and_ack_isr();
}

}
