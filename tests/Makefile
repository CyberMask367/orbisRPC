CC ?= cc
CFLAGS += -std=c11 -Wall -Wextra -Werror -DHEALTH_TESTABLE -I../orbisrpc -I../third_party/mbedtls/include

TARGET := test_utils
OBJDIR := .objs
ASAN_OBJDIR := .objs-asan
MBEDTLS_DIR := ../third_party/mbedtls/library
# Same exclusion set as scripts/build.sh (POSIX-only modules).
MBEDTLS_SRCS := $(filter-out $(MBEDTLS_DIR)/net_sockets.c $(MBEDTLS_DIR)/timing.c $(MBEDTLS_DIR)/entropy_poll.c,$(wildcard $(MBEDTLS_DIR)/*.c))

ORBIS_SRCS := ../orbisrpc/jsonlite.c ../orbisrpc/b64.c ../orbisrpc/sfo.c ../orbisrpc/tmdb_crypto.c ../orbisrpc/updater_util.c ../orbisrpc/art.c ../orbisrpc/log.c ../orbisrpc/health.c ../orbisrpc/manifest.c ../orbisrpc/compat.c ../orbisrpc/cfg.c

ORBIS_OBJS := $(patsubst ../orbisrpc/%.c,$(OBJDIR)/orbis_%.o,$(ORBIS_SRCS))
MBEDTLS_OBJS := $(patsubst $(MBEDTLS_DIR)/%.c,$(OBJDIR)/mbed_%.o,$(MBEDTLS_SRCS))
TEST_OBJS := $(OBJDIR)/test_utils.o $(ORBIS_OBJS) $(MBEDTLS_OBJS)

ASAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer
ASAN_ORBIS_OBJS := $(patsubst ../orbisrpc/%.c,$(ASAN_OBJDIR)/orbis_%.o,$(ORBIS_SRCS))
ASAN_MBEDTLS_OBJS := $(patsubst $(MBEDTLS_DIR)/%.c,$(ASAN_OBJDIR)/mbed_%.o,$(MBEDTLS_SRCS))
ASAN_OBJS := $(ASAN_OBJDIR)/test_utils.o $(ASAN_ORBIS_OBJS) $(ASAN_MBEDTLS_OBJS)

.PHONY: all clean test asan

all: $(TARGET)

test: $(TARGET)
	./$(TARGET)

$(TARGET): $(TEST_OBJS)
	$(CC) $(CFLAGS) -o $@ $(TEST_OBJS)

asan: $(ASAN_OBJS)
	$(CC) $(CFLAGS) $(ASAN_FLAGS) -o test_utils_asan $(ASAN_OBJS)
	./test_utils_asan

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(ASAN_OBJDIR):
	mkdir -p $(ASAN_OBJDIR)

$(OBJDIR)/test_utils.o: test_utils.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ test_utils.c

$(OBJDIR)/orbis_%.o: ../orbisrpc/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/mbed_%.o: $(MBEDTLS_DIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -Wno-unused-parameter -c -o $@ $<

$(ASAN_OBJDIR)/test_utils.o: test_utils.c | $(ASAN_OBJDIR)
	$(CC) $(CFLAGS) $(ASAN_FLAGS) -c -o $@ test_utils.c

$(ASAN_OBJDIR)/orbis_%.o: ../orbisrpc/%.c | $(ASAN_OBJDIR)
	$(CC) $(CFLAGS) $(ASAN_FLAGS) -c -o $@ $<

$(ASAN_OBJDIR)/mbed_%.o: $(MBEDTLS_DIR)/%.c | $(ASAN_OBJDIR)
	$(CC) $(CFLAGS) $(ASAN_FLAGS) -Wno-unused-parameter -c -o $@ $<

clean:
	rm -rf $(TARGET) test_utils_asan $(OBJDIR) $(ASAN_OBJDIR) *.o
