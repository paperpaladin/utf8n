# utf8n -- SPDX-License-Identifier: GPL-3.0-or-later
#
# t/run.sh compiles the test programs, so test-asan is a CFLAGS override and
# nothing more.  -ffile-prefix-map keeps the build reproducible: without it -g
# writes $(CURDIR) into DWARF .debug_line_str.

CC      ?= gcc
CFLAGS  ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -g -ffile-prefix-map=$(CURDIR)=.
ASAN     = -std=c11 -Wall -Wextra -Wpedantic -O1 -g \
	   -fsanitize=address,undefined -fno-omit-frame-pointer \
	   -ffile-prefix-map=$(CURDIR)=.

.PHONY: all test test-asan clean

all: utf8n.o

utf8n.o: utf8n.c utf8n.h
	$(CC) $(CFLAGS) -c -o $@ utf8n.c

test: all
	CC='$(CC)' CFLAGS='$(CFLAGS)' ./t/run.sh

# We decode untrusted UTF-8, so the sanitizers are not optional diligence.
test-asan:
	CC='$(CC)' CFLAGS='$(ASAN)' UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
		./t/run.sh

clean:
	rm -f utf8n.o t/unit t/conform t/nfx
	rm -rf scratch
