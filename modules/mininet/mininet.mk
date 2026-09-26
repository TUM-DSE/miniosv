# modules/mininet: $(mininet-objects) for a Rust app that has the crate, $(mininet-lib-objects) for everyone else.

ifneq ($(conf_net_mininet),1)
$(error this app needs conf_net_mininet=1)
endif

mininet-dir := modules/mininet

$(out)/$(mininet-dir)/shim/shim.o: CXXFLAGS += -Iinclude/api/minidpdk

mininet-objects = $(mininet-dir)/shim/shim.o

ring-cflags = -isystem $(CURDIR)/include/api -isystem $(CURDIR)/include/api/$(arch) \
              -isystem $(CURDIR)/$(out)/gen/include
export CFLAGS_x86_64_unknown_linux_gnu = $(ring-cflags)
export CFLAGS_aarch64_unknown_linux_gnu = $(ring-cflags)


# Cargo features; test/Makefile adds selftest.
mininet-features ?=

mininet_cargo_dir = $(out)/mininet-objs/cargo
mininet_lib = $(mininet_cargo_dir)/release/libmininet.a

.PHONY: $(mininet_lib)
$(mininet_lib):
	$(call quiet, cargo build --release --manifest-path $(mininet-dir)/rust/Cargo.toml \
		--features "$(mininet-features)" --target-dir $(mininet_cargo_dir), CARGO $(mininet-dir)/rust)

$(out)/mininet-objs/mininet.o: $(mininet_lib)
	$(makedir)
	$(call quiet, cmp -s $< $@ || cp $< $@, CP libmininet.a)

mininet-lib-objects = mininet-objs/mininet.o $(mininet-dir)/mininet.o $(mininet-objects)
