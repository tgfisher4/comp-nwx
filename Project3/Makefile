TARGETS= mpwordle/mpwordle mpwordleserver/mpwordleserver

all: $(TARGETS)

mpwordle/mpwordle:
	$(MAKE) -C mpwordle

mpwordleserver/mpwordleserver:
	$(MAKE) -C mpwordleserver

clean:
	$(MAKE) -C mpwordle clean
	$(MAKE) -C mpwordleserver clean


.PHONY: all clean
