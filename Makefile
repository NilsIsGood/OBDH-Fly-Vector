# R7023E OBDH model - build all three processes.
#
#   make          build pf, pl and gnd_stub
#   make demo     build, then run the scripted demonstration
#   make clean    remove binaries and generated logs

CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -O2 -D_DEFAULT_SOURCE
TARGETS = pf pl gnd_stub

all: $(TARGETS)

pf: pf.c obdh.c obdh.h
	$(CC) $(CFLAGS) -o $@ pf.c obdh.c

pl: pl.c obdh.c obdh.h
	$(CC) $(CFLAGS) -o $@ pl.c obdh.c

gnd_stub: gnd_stub.c obdh.c obdh.h
	$(CC) $(CFLAGS) -o $@ gnd_stub.c obdh.c

demo: all
	./demo.sh

clean:
	rm -f $(TARGETS) pl_images.log gnd_tm.log

.PHONY: all demo clean
