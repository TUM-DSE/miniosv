# modules/mininet: the network stack.
#
# There are two ways to link it, because there are two kinds of caller.
#
#   $(mininet-objects)      A Rust application that already depends on the
#                           crate -- the smoltcp-s3 benchmark does -- and so
#                           has the stack inside its own archive. All it needs
#                           from here is the shim, which is C++ and has to be
#                           compiled by the kernel.
#
#   $(mininet-lib-objects)  Everything else. Builds the crate as a staticlib
#                           and links it, so a C++ application talks to the
#                           stack through mininet.hh and never sees Rust.
#
# Include from an application's Makefile fragment and add one of them:
#
#     include modules/mininet/mininet.mk
#     app-objects += $(mininet-lib-objects)

mininet-dir := modules/mininet

# The shim is the only thing in the tree that includes the minidpdk headers
# directly; everything above it goes through the API.
$(out)/$(mininet-dir)/shim/shim.o: CXXFLAGS += -Iinclude/api/minidpdk

mininet-objects = $(mininet-dir)/shim/shim.o

# --- the crate, for callers that are not Rust -------------------------------

mininet_cargo_dir = $(out)/mininet-objs/cargo
mininet_lib = $(mininet_cargo_dir)/release/libmininet.a

# Phony because cargo decides what needs rebuilding; make cannot know the
# crate's inputs without duplicating them here and getting it wrong.
.PHONY: $(mininet_lib)
$(mininet_lib):
	$(call quiet, cargo build --release --manifest-path $(mininet-dir)/rust/Cargo.toml \
		--target-dir $(mininet_cargo_dir), CARGO $(mininet-dir)/rust)

# The archive is handed to the linker under a .o name, as the app fragments do:
# the link line takes objects, and ld is happy to be given an archive there.
$(out)/mininet-objs/mininet.o: $(mininet_lib)
	$(makedir)
	$(call quiet, cmp -s $< $@ || cp $< $@, CP libmininet.a)

mininet-lib-objects = mininet-objs/mininet.o $(mininet-dir)/mininet.o $(mininet-objects)
