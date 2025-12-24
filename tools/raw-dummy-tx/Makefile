# Simple build for the raw_dummy_tx synthetic sender

SRC_LEVEL=2
ROOT=../..

include $(ROOT)/make/host.mak
include $(ROOT)/make/target.mak

CFLAGS+= -I$(ROOT)/include
LIBS+= -lpthread

TARG=raw_dummy_tx$(ESUFFIX)

all: $(TARG)

OBJS=raw_dummy_tx.o

$(TARG): $(OBJS)
	$(CC) $(CFLAGS) $(DEFINES) $(INCLUDES) $(OBJS) -o $@ $(LIBS)

clean:
	rm -f $(TARG) *.o *.ot *.dis .depend *~ .*~
