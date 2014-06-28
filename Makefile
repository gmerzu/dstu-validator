OS=$(shell uname -s)

ifeq ($(OS),Linux)
RE_INC=/usr/include/re
RE_LIB=/usr/lib
LFLAGS+=-Wl,-rpath=$(SSL)/lib
else
RE_INC=../libre/include
RE_LIB=../libre
endif

SSL=/usr/local/ssl

INCS=-I$(SSL)/include -I$(RE_INC)
LIBS=\
	-L$(SSL)/lib -lcrypto \
	-L$(RE_LIB) -lre

CFLAGS=-DHAVE_INET6

OBJS=app.o daemon.o asn1.o

%.o: %.c
	cc $< -o $@ -c $(INCS) $(CFLAGS)

authd: $(OBJS)
	cc $(OBJS) -o $@ $(LIBS) $(LFLAGS)
