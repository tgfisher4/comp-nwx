CC= 		gcc
CFLAGS=		-g -Wall -std=gnu99 -Iinclude
LD=			gcc
LDFLAGS=	-Llib
LIBS=		-ljansson -lm -lpthread
TARGETS=	mycalserver

all:		$(TARGETS)

lib/libjansson.a:
	@echo "Installing Jansson json library..."
	@bash install-jansson.sh
	@rm -rf jansson-2.13
	@rm -f  jansson-2.13.tar.bz2

mycalserver.o: mycalserver.c lib/libjansson.a
	@echo "Compiling $@..."
	@$(CC) $(CFLAGS) -o $@ -c $<
	
mycalserver: mycalserver.o
	@echo "Linking $@..."
	@$(LD) $(LDFLAGS) -o $@ $^ $(LIBS)

clean:
	@echo Cleaning...
	@rm -rf lib include
	@rm $(TARGETS) *.o

.PHONY: all clean
