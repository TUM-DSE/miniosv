# The test application: a small driver (osv_tests.cc) plus the OS/libc++
# correctness and stress suites. Linked into the kernel instead of the normal
# application with:
#
#     make app=test
#
# This file is included by the top-level Makefile, which compiles these sources
# with the kernel's own flags and links them into the kernel image.
#
#   - osv_tests.cc : the osv_app_main() driver that runs the suites below
#   - os-features  : OS facility conformance checks
#   - os-libc      : C libc surface conformance checks (string/stdlib/math/...)
#   - os-iostream  : C++ iostreams / localization checks (std::cout etc.)
#   - os-memmove   : memmove() overlap-correctness regression test
#   - os-memory    : the memory clients: early, heap, page cache, plus malloc/mmap
#   - os-memory-primitives : vspace, frames and mapping, one header per function
#   - os-stress    : concurrency / allocator / TLS stress tests
#   - os-miniext   : miniext filesystem checks, only when conf_fs_miniext=1
#                    (needs --emulated-nvme at run time; skips without)
#   - os-mininet   : network stack unit tests. Needs no NIC: the parser, the
#                    body sink and the ABI, not a transfer.

app-objects =  test/osv_tests.o
app-objects += test/os-features.o
app-objects += test/os-libc.o
app-objects += test/os-iostream.o
app-objects += test/os-memmove.o
app-objects += test/os-memory.o
app-objects += test/os-memory-primitives.o
app-objects += test/os-stress.o

# The stack is linked as a library so the suite reaches it through mininet.hh
# and the raw ABI, the same way any C++ caller does.
include modules/mininet/mininet.mk
app-objects += test/os-mininet.o
app-objects += $(mininet-lib-objects)
ifeq ($(conf_fs_miniext),1)
app-objects += test/os-miniext.o
endif
