# =============================================================================
# Makefile -- NAND Flash Translation Layer simulator
# =============================================================================
#
# Build discipline is part of the project, not an afterthought:
#
#   -std=c99            no compiler extensions, so the code ports
#   -Wall -Wextra       every warning the compiler is willing to give
#   -Werror             warnings are build failures, because in firmware a
#                       warning is usually a bug that has not bitten yet
#   -Wshadow            catches the classic "inner variable hides outer" bug
#   -Wpedantic          rejects anything the standard does not guarantee
#
# Targets:
#   make            build the simulator and the test binary
#   make test       build and run the unit tests
#   make bench      run the standard benchmark sweep into bench/results.csv
#   make asan       rebuild with AddressSanitizer + UBSan and run the tests
#   make memcheck   run the tests under valgrind
#   make clean
# =============================================================================

CC       ?= gcc
CSTD      = -std=c99
WARN      = -Wall -Wextra -Werror -Wshadow -Wpedantic -Wstrict-prototypes \
            -Wmissing-prototypes -Wold-style-definition
OPT      ?= -O2
DEBUG     = -g
INC       = -Iinclude
DEFS     ?=
CFLAGS    = $(CSTD) $(WARN) $(OPT) $(DEBUG) $(INC) $(DEFS) $(CFLAGS_EXTRA)
LDLIBS    = -lm

BUILD     = build
BIN       = ftlsim
TESTBIN   = ftltest

# The FTL library: everything except the two main() functions.
LIB_SRC   = src/nand.c     \
            src/blockmgr.c \
            src/badblock.c \
            src/mapping.c  \
            src/gc.c       \
            src/wear.c     \
            src/journal.c  \
            src/ftl.c

TEST_SRC  = tests/test_main.c     \
            tests/test_nand.c     \
            tests/test_mapping.c  \
            tests/test_gc.c       \
            tests/test_wear.c     \
            tests/test_badblock.c \
            tests/test_journal.c  \
            tests/test_ftl.c

LIB_OBJ   = $(LIB_SRC:%.c=$(BUILD)/%.o)
TEST_OBJ  = $(TEST_SRC:%.c=$(BUILD)/%.o)
MAIN_OBJ  = $(BUILD)/src/main.o

.PHONY: all test bench sweep asan memcheck clean help

all: $(BIN) $(TESTBIN)

$(BIN): $(LIB_OBJ) $(MAIN_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(TESTBIN): $(LIB_OBJ) $(TEST_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

# Pattern rule with automatic dependency generation, so editing a header
# rebuilds every .c that includes it. Getting this wrong is how you spend an
# afternoon debugging a stale object file.
$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

-include $(LIB_OBJ:.o=.d) $(TEST_OBJ:.o=.d) $(MAIN_OBJ:.o=.d)

test: $(TESTBIN)
	./$(TESTBIN)

# The standard sweep that produces the numbers in DOCS/RESULTS.md.
bench: $(BIN)
	@mkdir -p bench
	python3 tools/run_experiments.py --binary ./$(BIN) --out bench/results.csv

sweep: bench

# Sanitizers. Firmware bugs are overwhelmingly memory bugs; running the whole
# test suite under ASan and UBSan costs seconds and is worth doing every time.
asan:
	$(MAKE) clean
	$(MAKE) OPT="-O1" CFLAGS_EXTRA="-fsanitize=address,undefined -fno-omit-frame-pointer" $(TESTBIN)
	./$(TESTBIN)

memcheck: $(TESTBIN)
	valgrind --error-exitcode=1 --leak-check=full ./$(TESTBIN)

clean:
	rm -rf $(BUILD) $(BIN) $(TESTBIN)

help:
	@echo "make            build ftlsim and ftltest"
	@echo "make test       run the unit tests"
	@echo "make bench      run the benchmark sweep -> bench/results.csv"
	@echo "make asan       run the tests under AddressSanitizer + UBSan"
	@echo "make memcheck   run the tests under valgrind"
	@echo "make clean"
	@echo ""
	@echo "Override geometry at build time, e.g.:"
	@echo "  make CFLAGS_EXTRA='-DNAND_NUM_BLOCKS=512 -DFTL_OP_PERCENT=7'"
