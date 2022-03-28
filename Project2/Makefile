# Group: Graham Fisher, Tommy Gallagher, Jason Brown
TARGETS= mycalclient/mycal mycalserver/mycalserver

all: $(TARGETS)

mycalclient/mycal:
	cd mycalclient && chmod +x mycal

mycalserver/mycalserver:
	$(MAKE) -C mycalserver

clean:
	$(MAKE) -C mycalserver clean

.PHONY: all clean
