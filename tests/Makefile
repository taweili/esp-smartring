# Host-side unit tests for ringkit's pure-logic modules.
#
# These compile the REAL ringkit sources against minimal ESP-IDF / FreeRTOS
# stubs and run on the development machine — no hardware, no idf.py.
#
#   make          # build the test binaries
#   make check    # build and run them
#   make clean
#
# The stubs live in stubs/ and are deliberately dumb: malloc backs the
# heap-caps API, the FreeRTOS mutex is a dummy handle (tests are
# single-threaded), and the HTTP client is a no-op (no network on the host).

CC       ?= cc
CFLAGS   ?= -std=gnu11 -Wall -Wextra -Werror -Wno-sign-compare -O2 -g
RINGKIT   = $(abspath ../components/ringkit)
INC       = -I$(RINGKIT)/include -I$(RINGKIT)/src -Istubs

STUB_SRC  = stub_esp.c
JSON_SRC  = $(RINGKIT)/src/ring_json.c
CHAT_SRCS = $(RINGKIT)/src/ring_chat.c $(JSON_SRC)

TESTS = test_ring_json test_ring_resample test_ring_byte_ring test_ring_chat_messages

all: $(TESTS)

check: $(TESTS)
	@failed=0; for t in $(TESTS); do echo "=== $$t ==="; ./$$t || failed=1; done; exit $$failed

test_ring_json: test_ring_json.c $(JSON_SRC) $(STUB_SRC)
	$(CC) $(CFLAGS) $(INC) $^ -o $@

test_ring_resample: test_ring_resample.c $(RINGKIT)/src/ring_resample.c $(RINGKIT)/src/ring_resample_coeffs.c $(STUB_SRC)
	$(CC) $(CFLAGS) $(INC) $^ -o $@

test_ring_byte_ring: test_ring_byte_ring.c $(RINGKIT)/src/ring_byte_ring.c $(STUB_SRC)
	$(CC) $(CFLAGS) $(INC) $^ -o $@

test_ring_chat_messages: test_ring_chat_messages.c $(CHAT_SRCS) $(STUB_SRC)
	$(CC) $(CFLAGS) $(INC) $^ -o $@

clean:
	rm -f $(TESTS)

.PHONY: all check clean
