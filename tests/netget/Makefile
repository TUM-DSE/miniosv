# netget: mininet against a real object server, over whatever NIC the drivers found.
#
#     make app=tests/netget
#
# It fetches one object by HTTP range GETs: a small one first, then a run whose bytes it hashes so the host can check
# them, then a timed run for throughput. The server, object and load are the NETGET_* defines in netget.cc.
ifneq ($(conf_net_mininet),1)
$(error netget needs conf_net_mininet=1)
endif
# The test server's CA (kite_expes nic/tls.sh), trusted besides the public roots for tls=1.
NETGET_CA ?= /mnt/nvme1n1/kite/tls/ca.der
ifneq ($(wildcard $(NETGET_CA)),)
mininet-features := extra-root
mininet-extra-root := $(NETGET_CA)
endif
include modules/mininet/mininet.mk
app-objects = tests/netget/netget.o $(mininet-lib-objects)
