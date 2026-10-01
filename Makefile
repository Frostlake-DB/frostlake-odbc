# Frostlake ODBC driver
#
#   make            build build/libfrostlakeodbc.so
#   make test       build + register (user-space) + run the smoke test
#                   against a Frostlake server on FROSTLAKE_TEST_PORT (default 18095);
#                   start one with test/run-server.sh or point at your own. With
#                   FL_CORPUS naming frostlake's engine/src/test/resources/testkit,
#                   the testkit corpus replays against that server too
#   make install    register the driver + a "Frostlake" DSN for this user
#   make clean
#
# Only unixODBC is required (headers + libodbcinst for reading DSN attributes).

CC       ?= gcc
CFLAGS   ?= -O2
CFLAGS   += -std=c11 -fPIC -Wall -Wextra -D_GNU_SOURCE
# -Bsymbolic is load-bearing: the driver manager (libodbc) exports the same
# SQL* names this driver does, and it sits earlier in the global symbol scope.
# Without it, an intra-driver call like SQLAllocStmt -> SQLAllocHandle goes
# through the PLT, gets interposed by the DM's copy, and the DM then rejects a
# handle it never issued.
LDFLAGS  += -shared -Wl,-Bsymbolic
LDLIBS   += -lodbcinst

BUILD    := build
LIB      := $(BUILD)/libfrostlakeodbc.so
SRC      := src/json.c src/http.c src/util.c src/proto.c src/session.c src/handles.c \
            src/connect.c src/execute.c src/results.c src/catalog.c
OBJ      := $(SRC:src/%.c=$(BUILD)/%.o)

.PHONY: all test install clean

all: $(LIB)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c src/fl_odbc.h src/json.h src/http.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIB): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

$(BUILD)/smoke: test/smoke.c | $(BUILD)
	$(CC) $(CFLAGS) -Wno-unused-parameter test/smoke.c -o $@ -lodbc

# The driver on its own, with no driver manager to answer misuse first.
$(BUILD)/direct: test/direct.c | $(BUILD)
	$(CC) $(CFLAGS) -Wno-unused-parameter test/direct.c -o $@ -ldl

# The session handling, against a scripted server in the test's own process.
$(BUILD)/session: test/session.c | $(BUILD)
	$(CC) $(CFLAGS) -Wno-unused-parameter test/session.c -o $@ -ldl -lpthread

# The engine's testkit corpus through the driver manager; test/smoke.sh runs it when FL_CORPUS is set.
$(BUILD)/suites: test/suites.c src/json.c src/json.h | $(BUILD)
	$(CC) $(CFLAGS) -Isrc test/suites.c src/json.c -o $@ -lodbc

test: $(LIB) $(BUILD)/smoke $(BUILD)/direct $(BUILD)/session $(BUILD)/suites
	test/smoke.sh

install: $(LIB)
	install/register.sh

clean:
	rm -rf $(BUILD)
