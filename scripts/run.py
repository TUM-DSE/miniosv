#!/usr/bin/env python3

# Launcher for the slim miniosv UEFI unikernel under QEMU.
#
# This script boots the GPT/ESP disk image build/<mode>/loader.img 
# through UEFI firmware (OVMF on x86_64, AAVMF on aarch64) 
# with the kernel embedded as the EFI application. 
# The kernel is diskless (the single app is linked in); the boot disk is attached as NVMe.
# The guest serial port is the console on your terminal.

import subprocess
import sys
import argparse
import os
import errno
import shutil
import tempfile

devnull = open('/dev/null', 'w')

osv_base = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
host_arch = os.uname().machine

stty_params = None

def stty_save():
    global stty_params
    p = subprocess.Popen(["stty", "-g"], stdout=subprocess.PIPE, stderr=devnull)
    out, _ = p.communicate()
    stty_params = out.strip()

def stty_restore():
    if stty_params:
        subprocess.call(["stty", stty_params], stderr=devnull)

def format_args(args):
    def format_arg(arg):
        if arg == '':
            return '""'
        elif ' ' in arg:
            return '"%s"' % arg
        elif arg[0] == '-':
            return '\\\n' + arg
        else:
            return arg
    return ' '.join(map(format_arg, args))

def pick(*candidates):
    for c in candidates:
        if c and os.path.exists(c):
            return c
    return None

def find_firmware(arch):
    "Return (code_path, vars_path) for the UEFI firmware, or exit with guidance."
    if arch == 'x86_64':
        code = os.environ.get('OVMF_CODE') or pick(
            '/usr/share/OVMF/OVMF_CODE_4M.fd',
            '/usr/share/OVMF/OVMF_CODE.fd',
            '/usr/share/ovmf/OVMF_CODE.fd')
        vars_ = os.environ.get('OVMF_VARS') or pick(
            '/usr/share/OVMF/OVMF_VARS_4M.fd',
            '/usr/share/OVMF/OVMF_VARS.fd',
            '/usr/share/ovmf/OVMF_VARS.fd')
        pkg, prefix = 'ovmf', 'OVMF'
    else:
        code = os.environ.get('AAVMF_CODE') or pick(
            '/usr/share/AAVMF/AAVMF_CODE.fd',
            '/usr/share/qemu-efi-aarch64/QEMU_EFI.fd',
            '/usr/share/edk2/aarch64/QEMU_EFI.fd',
            os.path.expanduser('~/.cache/aavmf/AAVMF_CODE.fd'))
        vars_ = os.environ.get('AAVMF_VARS') or pick(
            '/usr/share/AAVMF/AAVMF_VARS.fd',
            '/usr/share/qemu-efi-aarch64/QEMU_VARS.fd',
            os.path.expanduser('~/.cache/aavmf/AAVMF_VARS.fd'))
        pkg, prefix = 'qemu-efi-aarch64', 'AAVMF'
    if not code or not vars_:
        sys.exit("run: no UEFI firmware found for %s. Install '%s', or set the "
                 "%s_CODE and %s_VARS env vars to the firmware files."
                 % (arch, pkg, prefix, prefix))
    return code, vars_

def setup_pflash(arch, code, vars_, workdir):
    # pflash needs a writable copy of the variable store; the aarch64 virt
    # pflash also requires the firmware images to be exactly 64 MiB.
    code_copy = os.path.join(workdir, 'code.fd')
    vars_copy = os.path.join(workdir, 'vars.fd')
    shutil.copy(code, code_copy)
    shutil.copy(vars_, vars_copy)
    os.chmod(code_copy, 0o644)
    os.chmod(vars_copy, 0o644)
    if arch == 'aarch64':
        for f in (code_copy, vars_copy):
            with open(f, 'r+b') as fh:
                fh.truncate(64 * 1024 * 1024)
    return code_copy, vars_copy

def set_boot_args(image, args):
    """Store the application arguments in the boot image.

    The image carries them in a reserved sector, which is the only channel that
    works the same under QEMU and on the clouds -- see scripts/setargs.py. This
    rewrites the image in place, so it must happen before QEMU opens it.
    """
    setargs = os.path.join(os.path.dirname(os.path.abspath(__file__)), "setargs.py")
    subprocess.check_call([sys.executable, setargs, image, args])


def start_osv_qemu(options):
    workdir = tempfile.mkdtemp(prefix='miniosv-run-')
    try:
        code, vars_ = find_firmware(options.arch)
        code_copy, vars_copy = setup_pflash(options.arch, code, vars_, workdir)

        use_kvm = options.hypervisor == 'kvm'

        args = ["-m", options.memsize, "-smp", options.vcpus, "-no-reboot"]

        # Machine, CPU and acceleration.
        if options.arch == 'x86_64':
            args += ["-machine", "q35"]
        else:
            gic = "max" if use_kvm else options.gic_version
            args += ["-machine", "virt,gic-version=%s" % gic]
        if use_kvm:
            args += ["-enable-kvm", "-cpu", "host"]
        else:
            args += ["-cpu", "max"]

        # UEFI firmware on pflash.
        args += [
            "-drive", "if=pflash,format=raw,readonly=on,file=%s" % code_copy,
            "-drive", "if=pflash,format=raw,file=%s" % vars_copy]

        # Skip the firmware's boot-menu countdown.
        args += ["-boot", "menu=on,splash-time=0"]

        # Boot disk: the GPT/ESP image as an NVMe drive. The firmware finds
        # \EFI\BOOT\BOOT{X64,AA64}.EFI on it, exactly as on AWS Nitro.
        args += [
            "-drive", "id=bootdisk,format=raw,if=none,file=%s" % options.image_file,
            "-device", "nvme,serial=miniosv,drive=bootdisk,bootindex=0"]

        # Extra emulated NVMe drives, in the order given: the guest sees them as
        # controller 1, 2, ... (0 is the boot disk). A drive is either a
        # filesystem image to mount, or a single file the application reads as a
        # raw namespace -- llama.cpp takes its model that way, with no image to
        # build and no filesystem in between.
        for i, image in enumerate(options.emulated_nvme or [], start=1):
            dev = "nvme,serial=deadbeef%d,drive=nvm%d,bootindex=%d" % (i, i, i)
            if options.nvme_mdts is not None:
                dev += ",mdts=%d" % options.nvme_mdts
            drive = "file=%s,if=none,id=nvm%d,format=raw" % (image, i)
            if options.nvme_cache is not None:
                drive += ",cache=%s" % options.nvme_cache
            args += ["-drive", drive, "-device", dev]

        # vAccel offload: the guest reaches a host accelerator through this
        # device.
        if options.vaccel:
            args += [
                "-object", "acceldev-backend-vaccel,id=gen0",
                "-device", "virtio-accel-pci,id=accl0,runtime=gen0,"
                           "disable-legacy=on,event_idx=off"]

        # PCI passthrough: one -device per address. Devices must be bound to
        # vfio-pci on the host, and QEMU must run with enough privilege (sudo).
        for pci in options.pass_pci or []:
            args += ["-device", "vfio-pci,host=%s" % pci]

        # No networking, no graphics: the console is the serial port on this
        # terminal (Ctrl-A C for the QEMU monitor, Ctrl-A X to quit).
        args += ["-nic", "none", "-display", "none", "-serial", "mon:stdio"]

        if not options.nogdb:
            args += ["-gdb", "tcp::%s,server,nowait" % options.gdb]
        if options.wait:
            args += ["-S"]
        if options.no_shutdown:
            args += ["-no-shutdown"]

        for a in options.pass_args or []:
            args += a.split()

        qemu_path = options.qemu_path
        if not qemu_path and options.vaccel:
            qemu_path = os.environ.get('QEMU_VACCEL')
            if not qemu_path:
                sys.exit("run: --vaccel needs the QEMU that carries virtio-accel. "
                         "Enter the dev shell (which sets QEMU_VACCEL), or pass "
                         "--qemu-path.")
        qemu_path = qemu_path or ('qemu-system-%s' % options.arch)
        cmdline = [qemu_path] + args

        if options.dry_run:
            print(format_args(cmdline))
            return

        # --preload scopes the library to qemu itself. Putting LD_PRELOAD on
        # this script instead would load it into every helper subprocess too
        # (stty, setargs.py); the boot-time benchmark preloads a library whose
        # exit-time stop must catch qemu, not whichever helper exits first.
        env = os.environ.copy()
        if options.preload:
            env["LD_PRELOAD"] = options.preload

        try:
            stty_save()
            ret = subprocess.call(cmdline, env=env)
            if ret != 0:
                sys.exit("qemu failed.")
        except OSError as e:
            if e.errno == errno.ENOENT:
                print("'%s' binary not found. Please install the "
                      "qemu-system-%s package." % (qemu_path, options.arch),
                      file=sys.stderr)
            else:
                print("OS error(%d): \"%s\" while running %s" %
                      (e.errno, e.strerror, " ".join(cmdline)), file=sys.stderr)
        finally:
            stty_restore()
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

def choose_hypervisor(arch):
    # KVM only when the guest arch matches the host and /dev/kvm is usable.
    # Note: x86 miniosv's only KVM clock is kvmclock, so an x86 guest really
    # wants KVM (under pure TCG it has no time source); aarch64 is fine on TCG
    # (its clock is the always-present ARM generic timer).
    if os.path.exists('/dev/kvm') and os.access('/dev/kvm', os.W_OK) and arch == host_arch:
        return 'kvm'
    return 'tcg'

def main(options):
    if options.hypervisor not in ("tcg", "kvm"):
        sys.exit("Unrecognized hypervisor '%s' (expected kvm or tcg)."
                 % options.hypervisor)
    start_osv_qemu(options)

if __name__ == "__main__":
    parser = argparse.ArgumentParser(prog='run',
        description='Boot the miniosv UEFI image under QEMU.')
    parser.add_argument("-d", "--debug", action="store_true",
                        help="run the debug build (build/debug)")
    parser.add_argument("-r", "--release", action="store_true",
                        help="run the release build (build/release)")
    parser.add_argument("-w", "--wait", action="store_true",
                        help="freeze the guest at startup (resume from the monitor or gdb)")
    parser.add_argument("-m", "--memsize", action="store", default="2G",
                        help="guest memory, e.g. 1G, 2G (default 2G; >=2G recommended)")
    parser.add_argument("-c", "--vcpus", action="store", default="4",
                        help="number of vcpus (default 4)")
    parser.add_argument("-p", "--hypervisor", action="store", default="auto",
                        help="acceleration: kvm, tcg, or auto (default)")
    parser.add_argument("-H", "--no-shutdown", action="store_true",
                        help="keep qemu alive on guest shutdown/reset (for the debugger)")
    parser.add_argument("--dry-run", action="store_true",
                        help="print the QEMU command line and exit")
    parser.add_argument("--nogdb", action="store_true",
                        help="do not expose the gdb stub")
    parser.add_argument("--gdb", action="store", default="1234",
                        help="gdb stub port (default 1234)")
    parser.add_argument("--pass-args", action="append",
                        help="append raw arguments to the QEMU command line")
    parser.add_argument("--qemu-path", action="store",
                        help="path to the qemu binary (default qemu-system-<arch>)")
    parser.add_argument("--image-path", action="store",
                        help="path to the boot image (default build/<mode>/loader.img)")
    parser.add_argument("--arch", action="store", choices=["x86_64", "aarch64"],
                        default=host_arch,
                        help="guest architecture (default: host arch)")
    parser.add_argument("--emulated-nvme", action="append", metavar="IMAGE",
                        help="attach a file as an extra emulated NVMe device; repeatable, "
                             "and the guest numbers them 1, 2, ... in the order given")
    parser.add_argument("--nvme-mdts", type=int, default=None,
                        help="MDTS for the emulated data drives: one command "
                             "carries 4 KiB << this, so 7 is 512 KiB and 9 is "
                             "2 MiB. QEMU's own default is 7.")
    parser.add_argument("--nvme-cache", action="store", default=None,
                        choices=["none", "writeback", "writethrough", "directsync", "unsafe"],
                        help="QEMU cache mode for the emulated data drives. The "
                             "default (writeback) serves repeated reads from the "
                             "host page cache; 'none' opens the file O_DIRECT so "
                             "every guest read really hits the disk.")
    parser.add_argument("--vaccel", action="store_true",
                        help="attach the virtio-accel device; needs the QEMU from the "
                             "lros-qemu flake (uses $QEMU_VACCEL unless --qemu-path is given)")
    parser.add_argument("--pass-pci", action="store", nargs='+', metavar="ADDR",
                        help="passthrough PCI device(s) bound to vfio-pci, e.g. 0000:01:00.0")
    parser.add_argument("--gic-version", action="store", default="3",
                        help="aarch64 GIC version under TCG (default 3)")
    parser.add_argument("--preload", action="store", metavar="LIB",
                        help="LD_PRELOAD this library into qemu (only qemu, "
                             "not this script's helper subprocesses)")
    parser.add_argument("--args", action="store", metavar="STRING",
                        help="application arguments; written into the boot "
                             "image before starting (see scripts/setargs.py). "
                             "The first word names the executable to run.")
    cmdargs = parser.parse_args()

    # The build output dir is build/<mode>.<arch> (arch as x64 / aarch64), so
    # derive the image from --arch rather than the arch-ambiguous build/last.
    mode = "debug" if cmdargs.debug else "release"
    archdir = "x64" if cmdargs.arch == "x86_64" else "aarch64"
    cmdargs.image_file = os.path.abspath(cmdargs.image_path or
        os.path.join(osv_base, "build/%s.%s/loader.img" % (mode, archdir)))
    if not os.path.exists(cmdargs.image_file):
        sys.exit("Boot image %s does not exist. Build it first, e.g.:\n"
                 "  make arch=%s" % (cmdargs.image_file, archdir))

    if cmdargs.hypervisor == "auto":
        cmdargs.hypervisor = choose_hypervisor(cmdargs.arch)

    # Rewrite the image before QEMU opens it.
    if cmdargs.args is not None:
        set_boot_args(cmdargs.image_file, cmdargs.args)

    main(cmdargs)
