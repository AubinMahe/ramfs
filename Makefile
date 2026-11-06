CC_LINUX := gcc
CC_WIN   := x86_64-w64-mingw32-gcc
AR_WIN   := x86_64-w64-mingw32-ar
WINE     := WINEDEBUG=-all wine
MODE     ?= RELEASE
ifeq ($(MODE),DEBUG)
	BUILD_FLAGS := -O0 -g -ggdb
	SUFFIX      := _debug
else ifeq ($(MODE),RELEASE)
	BUILD_FLAGS := -g0 -O2 -DNDEBUG
	SUFFIX      :=
else
  $(error MODE must be RELEASE or DEBUG)
endif
SRC_LIB     := src/ramfs_core.c src/ramfs_vfs.c src/ramfs_posix.c src/ramfs_stdio.c src/ramfs_archive.c
HDR         := inc/ramfs.h src/ramfs_core.h src/ramfs_vfs.h src/ramfs_sync.h
TESTS       := test/main.c test/test_macros.c
MT_TESTS    := test/test_mt.c
PERF_SUITE  := test/perf_benchmark.c
EXTRA       ?=
MEASURES    ?=
GIT_VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo "v1.0.0")
WARN        := -std=c11 -Wall -Wextra -pedantic -Wformat=2 -Wformat-security -Wstrict-overflow -Wno-unused-result -Werror $(EXTRA)
POSIX_FLAGS := -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -D_XOPEN_SOURCE=700
CFLAGS      := $(WARN) $(BUILD_FLAGS) -I inc $(POSIX_FLAGS) -DRAMFS_VERSION=\"$(GIT_VERSION)\" $(if $(MEASURES),-DMEASURES=$(MEASURES))
CPPFLAGS    := -DBUILDING_RAMFS_DLL
BUILD_LINUX := build_linux$(SUFFIX)
BUILD_WIN   := build_win$(SUFFIX)
B_ASAN      := build_asan
B_TSAN      := build_tsan
B_DBG       := build_valgrind
B_PERF      := build_perf
B_COV       := build_coverage
ALL_DIRS    := build_linux build_linux_debug build_win build_win_debug $(B_ASAN) $(B_TSAN) $(B_DBG) $(B_PERF) $(B_COV)
PERF_BIN    := $(BUILD_LINUX)/perf_benchmark
PERF_WIN    := $(BUILD_WIN)/perf_benchmark.exe

.PHONY: all linux windows win clean help test test_linux test_mt test_win bench check asan tsan valgrind efence perf callgrind format-sources complexity metrics coverage lcov gcov

all: linux windows

clean:
	rm -rf $(ALL_DIRS)

format-sources:
	clang-format -i --verbose inc/*.h src/*.c src/*.h test/*.c

linux: $(BUILD_LINUX)/libramfs.so $(BUILD_LINUX)/libramfs_mt.so $(BUILD_LINUX)/test_ramfs $(BUILD_LINUX)/test_ramfs_mt $(PERF_BIN)

$(BUILD_LINUX)/libramfs.so: $(SRC_LIB) $(HDR)
	@mkdir -p $(@D)
	$(CC_LINUX) $(CFLAGS) $(CPPFLAGS) -fPIC -fvisibility=hidden -shared -MMD -MT $@ -MF $@.d -o $@ $(SRC_LIB) -lz

$(BUILD_LINUX)/libramfs_mt.so: $(SRC_LIB) $(HDR)
	@mkdir -p $(@D)
	$(CC_LINUX) $(CFLAGS) $(CPPFLAGS) -DRAMFS_THREAD_SAFE -pthread -fPIC -fvisibility=hidden -shared -MMD -MT $@ -MF $@.d -o $@ $(SRC_LIB) -lz

$(BUILD_LINUX)/test_ramfs: $(TESTS) $(HDR) $(BUILD_LINUX)/libramfs.so
	$(CC_LINUX) $(CFLAGS) -MMD -MT $@ -MF $@.d -o $@ $(TESTS) -L$(BUILD_LINUX) -lramfs -Wl,-rpath,'$$ORIGIN'

$(BUILD_LINUX)/test_ramfs_mt: $(MT_TESTS) $(HDR) $(BUILD_LINUX)/libramfs_mt.so
	$(CC_LINUX) $(CFLAGS) -pthread -MMD -MT $@ -MF $@.d -o $@ $(MT_TESTS) -L$(BUILD_LINUX) -lramfs_mt -Wl,-rpath,'$$ORIGIN'

$(PERF_BIN): $(PERF_SUITE) $(HDR) $(BUILD_LINUX)/libramfs.so
	$(CC_LINUX) $(CFLAGS) -MMD -MT $@ -MF $@.d -o $@ $(PERF_SUITE) -L$(BUILD_LINUX) -lramfs -Wl,-rpath,'$$ORIGIN'

test_linux: $(BUILD_LINUX)/test_ramfs
	./$<

test_mt: $(BUILD_LINUX)/test_ramfs_mt
	./$<

bench: $(PERF_BIN)
	./$<

windows: $(BUILD_WIN)/ramfs.dll $(BUILD_WIN)/ramfs_mt.dll $(BUILD_WIN)/test_ramfs.exe $(BUILD_WIN)/test_ramfs_mt.exe $(PERF_WIN)

$(BUILD_WIN)/ramfs.dll: $(SRC_LIB) $(HDR)
	@mkdir -p $(@D)
	$(CC_WIN) $(CFLAGS) $(CPPFLAGS) -shared -static-libgcc -MMD -MT $@ -MF $@.d -o $@ $(SRC_LIB) -lz -Wl,--out-implib,$(BUILD_WIN)/libramfs.a

$(BUILD_WIN)/ramfs_mt.dll: $(SRC_LIB) $(HDR)
	@mkdir -p $(@D)
	$(CC_WIN) $(CFLAGS) $(CPPFLAGS) -DRAMFS_THREAD_SAFE -shared -static-libgcc -MMD -MT $@ -MF $@.d -o $@ $(SRC_LIB) -lz -Wl,-Bstatic -lpthread -Wl,-Bdynamic -Wl,--out-implib,$(BUILD_WIN)/libramfs_mt.a

$(BUILD_WIN)/test_ramfs.exe: $(TESTS) $(BUILD_WIN)/ramfs.dll
	$(CC_WIN) $(CFLAGS) -static-libgcc -MMD -MT $@ -MF $@.d -o $@ $(TESTS) -L$(BUILD_WIN) -lramfs

$(BUILD_WIN)/test_ramfs_mt.exe: $(MT_TESTS) $(BUILD_WIN)/ramfs_mt.dll
	$(CC_WIN) $(CFLAGS) -static-libgcc -MMD -MT $@ -MF $@.d -o $@ $(MT_TESTS) -L$(BUILD_WIN) -lramfs_mt -Wl,-Bstatic -lpthread -Wl,-Bdynamic

$(PERF_WIN): $(PERF_SUITE) $(BUILD_WIN)/ramfs.dll
	$(CC_WIN) $(CFLAGS) -static-libgcc -MMD -MT $@ -MF $@.d -o $@ $(PERF_SUITE) -L$(BUILD_WIN) -lramfs

test_win: $(BUILD_WIN)/test_ramfs.exe $(BUILD_WIN)/test_ramfs_mt.exe
	WINEPATH=$$(pwd)/$(BUILD_WIN) $(WINE) $(BUILD_WIN)/test_ramfs.exe
	WINEPATH=$$(pwd)/$(BUILD_WIN) $(WINE) $(BUILD_WIN)/test_ramfs_mt.exe

test: test_linux test_mt test_win

QA_CFLAGS := $(WARN) -I inc $(POSIX_FLAGS) -DRAMFS_VERSION=\"$(GIT_VERSION)\"
QA_LDLIBS := -lz

$(B_ASAN)/test_ramfs: $(SRC_LIB) $(TESTS) $(HDR)
	@mkdir -p $(@D)
	$(CC_LINUX) $(QA_CFLAGS) -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=undefined -o $@ $(SRC_LIB) $(TESTS) $(QA_LDLIBS)

asan: $(B_ASAN)/test_ramfs
	./$<

$(B_TSAN)/test_ramfs_mt: $(SRC_LIB) $(MT_TESTS) $(HDR)
	@mkdir -p $(@D)
	$(CC_LINUX) $(QA_CFLAGS) -DRAMFS_THREAD_SAFE -pthread -g -O1 -fno-omit-frame-pointer -fsanitize=thread -o $@ $(SRC_LIB) $(MT_TESTS) $(QA_LDLIBS)

tsan: $(B_TSAN)/test_ramfs_mt
	setarch $$(uname -m) -R ./$<

$(B_DBG)/test_ramfs: $(SRC_LIB) $(TESTS) $(HDR)
	@mkdir -p $(@D)
	$(CC_LINUX) $(QA_CFLAGS) -g -O0 -o $@ $(SRC_LIB) $(TESTS) $(QA_LDLIBS)

valgrind: $(B_DBG)/test_ramfs
	valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes --error-exitcode=1 -q ./$<

$(B_DBG)/test_ramfs_ef: $(SRC_LIB) $(TESTS) $(HDR)
	@mkdir -p $(@D)
	$(CC_LINUX) $(QA_CFLAGS) -g -O0 -o $@ $(SRC_LIB) $(TESTS) -lefence $(QA_LDLIBS)

efence: $(B_DBG)/test_ramfs_ef
	EF_PROTECT_BELOW=0 ./$<
	EF_PROTECT_BELOW=1 ./$<

check: test_linux test_mt asan tsan valgrind efence

$(B_PERF)/perf_benchmark: $(SRC_LIB) $(PERF_SUITE) $(HDR)
	@mkdir -p $(@D)
	$(CC_LINUX) $(QA_CFLAGS) $(if $(MEASURES),-DMEASURES=$(MEASURES)) -O2 -DNDEBUG -g -fno-omit-frame-pointer -o $@ $(PERF_SUITE) $(SRC_LIB) $(QA_LDLIBS)

perf: $(B_PERF)/perf_benchmark
	perf stat -e task-clock,page-faults,context-switches ./$<
	perf record -F 2999 --call-graph fp -o $(B_PERF)/perf.data -- ./$<
	perf report -i $(B_PERF)/perf.data --stdio --no-children --percent-limit 2 | head -70

callgrind: $(B_PERF)/perf_benchmark
	valgrind --tool=callgrind --callgrind-out-file=$(B_PERF)/callgrind.out ./$<
	callgrind_annotate $(B_PERF)/callgrind.out | head -40

COMPLEXITY_LIMIT ?= 25

metrics: complexity

complexity:
	@which pmccabe >/dev/null 2>&1 || (echo "Error: pmccabe is not installed (sudo apt install pmccabe)" && exit 1)
	@echo "=== Most complex functions (CCN >= $(COMPLEXITY_LIMIT)) ==="
	@printf "%-5s %-5s %-7s %-6s %s\n" "CCN" "Trad" "Stmts" "Lines" "Function"
	@echo "----------------------------------------------------------------------"
	@pmccabe -c -r $(COMPLEXITY_LIMIT) $(SRC_LIB) | sort -k1,1nr | awk -F'\t' '{ printf "%-5s %-5s %-7s %-6s %s\n", $$1, $$2, $$3, $$5, $$6 }'
	@echo "----------------------------------------------------------------------"
	@echo "=== Summary by module ==="
	@printf "%-6s %-7s %-6s %s\n" "CCN" "Stmts" "Lines" "File"
	@echo "----------------------------------------------------------------------"
	@pmccabe -c -F $(SRC_LIB) | awk -F'\t' '{ printf "%-6s %-7s %-6s %s\n", $$1, $$3, $$5, $$6 }'
	@echo "----------------------------------------------------------------------"
	@printf "TOTAL PROJECT : "
	@pmccabe -c -T $(SRC_LIB) | awk -F'\t' '{ printf "CCN=%s, Stmts=%s, LoC=%s\n", $$1, $$3, $$5 }'

$(B_COV)/test_ramfs: $(SRC_LIB) $(TESTS) $(HDR)
	@mkdir -p $(@D)
	$(CC_LINUX) $(QA_CFLAGS) -g -O0 --coverage -o $@ $(SRC_LIB) $(TESTS) $(QA_LDLIBS)

gcov: $(B_COV)/test_ramfs
	@which gcov >/dev/null 2>&1 || (echo "Error: gcov is not installed" && exit 1)
	@rm -f $(B_COV)/*.gcda
	@./$< >/dev/null
	@echo "=== Code coverage by module (gcov) ==="
	@cd $(B_COV) && gcov -n $(patsubst src/%.c,test_ramfs-%.gcda,$(SRC_LIB))

lcov: coverage

coverage: $(B_COV)/test_ramfs
	@which lcov >/dev/null 2>&1 || (echo "Error: lcov is not installed (sudo apt install lcov)" && exit 1)
	@which genhtml >/dev/null 2>&1 || (echo "Error: genhtml is not installed (sudo apt install lcov)" && exit 1)
	@rm -f $(B_COV)/*.gcda $(B_COV)/*.info
	@rm -rf $(B_COV)/html
	@./$< >/dev/null
	@lcov --capture --directory $(B_COV) --output-file $(B_COV)/coverage.info --rc branch_coverage=1 --ignore-errors gcov,gcov >/dev/null 2>&1
	@lcov --extract $(B_COV)/coverage.info "*/src/*" --output-file $(B_COV)/coverage_src.info --rc branch_coverage=1 --ignore-errors gcov,gcov >/dev/null 2>&1
	@genhtml $(B_COV)/coverage_src.info --output-directory $(B_COV)/html --title "ramfs test coverage" --branch-coverage
	@echo ""
	@echo "=== Code coverage summary (lcov) ==="
	@lcov --summary $(B_COV)/coverage_src.info --rc branch_coverage=1
	@echo ""
	@xdg-open "$(B_COV)/html/index.html"

-include $(wildcard $(addsuffix /*.d,$(ALL_DIRS)))
