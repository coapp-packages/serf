# this is just for testing... we'll get a "real" build system at some point
#
# add apr-config and apu-config to your PATH before invoking this Makefile
#

OBJECTS = buckets/aggregate_buckets.o buckets/request_buckets.o context.o \
          buckets/buckets.o buckets/simple_buckets.o buckets/file_buckets.o \
          buckets/mmap_buckets.o buckets/socket_buckets.o \
          buckets/response_buckets.o buckets/headers_buckets.o \
          buckets/allocator.o buckets/dechunk_buckets.o \
          buckets/deflate_buckets.o buckets/limit_buckets.o \
          buckets/ssl_buckets.o buckets/barrier_buckets.o

PROGRAMS = test/serf_get test/serf_response test/serf_request
TESTCASES = test/testcases/simple.response \
  test/testcases/chunked-empty.response test/testcases/chunked.response \
  test/testcases/chunked-trailers.response \
  test/testcases/deflate.response

# Place apr-config and apu-config in your PATH.
APR_CONFIG=apr-1-config
APU_CONFIG=apu-1-config

CC = `$(APR_CONFIG) --cc`
CFLAGS = `$(APR_CONFIG) --cflags`
CPPFLAGS = `$(APR_CONFIG) --cppflags`
INCLUDES = -I`pwd` `$(APR_CONFIG) --includes`

LDFLAGS = `$(APR_CONFIG) --ldflags` `$(APU_CONFIG) --ldflags`
LIBS = `$(APR_CONFIG) --link-ld --libs` `$(APU_CONFIG) --link-ld --libs` -lz -lssl -lcrypto

all: $(OBJECTS) $(PROGRAMS)

context.o: context.c
buckets/aggregate_buckets.o: buckets/aggregate_buckets.c
buckets/request_buckets.o: buckets/request_buckets.c
buckets/buckets.o: buckets/buckets.c
buckets/simple_buckets.o: buckets/simple_buckets.c

test/serf_get: $(OBJECTS) test/serf_get.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

test/serf_response: $(OBJECTS) test/serf_response.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

test/serf_request: $(OBJECTS) test/serf_request.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

check: test/serf_response
	@for i in $(TESTCASES); \
		 do echo "== Testing $$i =="; \
		 ./test/serf_response $$i; \
	done;

clean:
	rm -f $(OBJECTS) $(PROGRAMS)

.c.o:
	$(CC) $(CFLAGS) $(CPPFLAGS) $(INCLUDES) -c -o $@ $<
