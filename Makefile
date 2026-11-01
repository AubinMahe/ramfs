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
  $(error MODE doit valoir RELEASE ou DEBUG)
endif
SRC_LIB     := src/ramfile.c
HDR         := inc/ramfile.h
TESTS       := test/main.c test/test_macros.c
BENCH       := test/benchmark.c
BENCH_RAM   := test/benchmark-ramfile-only.c
EXTRA       ?=
MEASURES    ?=
WARN        := -std=c11 -Wall -Wextra -pedantic $(if $(WERROR),-Werror) $(EXTRA)
CFLAGS      := $(WARN) $(BUILD_FLAGS) -I inc -D_POSIX_C_SOURCE=200809L $(if $(MEASURES),-DMEASURES=$(MEASURES))
CPPFLAGS    := -DBUILDING_RAMFILE_DLL
BUILD_LINUX := build_linux$(SUFFIX)
BUILD_WIN   := build_win$(SUFFIX)
B_ASAN      := build_asan
B_DBG       := build_valgrind
B_PERF      := build_perf
ALL_DIRS    := build_linux build_linux_debug build_win build_win_debug $(B_ASAN) $(B_DBG) $(B_PERF)
BENCH_LINUX := $(BUILD_LINUX)/benchmark
BENCH_WIN   := $(BUILD_WIN)/benchmark.exe

.PHONY: all linux windows win clean help test test_linux test_win bench_linux bench_win check asan valgrind efence perf callgrind format-sources

all: linux windows

clean:
	rm -rf $(ALL_DIRS)

format-sources:
	clang-format -i --verbose inc/*.h src/*.c test/*.c

linux: $(BUILD_LINUX)/libramfile.so $(BUILD_LINUX)/test_ramfile $(BENCH_LINUX)

$(BUILD_LINUX)/libramfile.so: $(SRC_LIB) $(HDR)
	@mkdir -p $(@D)
	$(CC_LINUX) $(CFLAGS) $(CPPFLAGS) -fPIC -fvisibility=hidden -shared -MMD -MT $@ -MF $@.d -o $@ $(SRC_LIB) -lz

$(BUILD_LINUX)/test_ramfile: $(TESTS) $(HDR) $(BUILD_LINUX)/libramfile.so
	$(CC_LINUX) $(CFLAGS) -MMD -MT $@ -MF $@.d -o $@ $(TESTS) -L$(BUILD_LINUX) -lramfile -Wl,-rpath,'$$ORIGIN'

$(BENCH_LINUX): $(BENCH) $(HDR) $(BUILD_LINUX)/libramfile.so
	$(CC_LINUX) $(CFLAGS) -MMD -MT $@ -MF $@.d -o $@ $(BENCH) -L$(BUILD_LINUX) -lramfile -Wl,-rpath,'$$ORIGIN'

test_linux: $(BUILD_LINUX)/test_ramfile
	./$<

bench_linux: $(BENCH_LINUX)
	./$<

windows: $(BUILD_WIN)/ramfile.dll $(BUILD_WIN)/test_ramfile.exe $(BENCH_WIN)

$(BUILD_WIN)/ramfile.dll: $(SRC_LIB) $(HDR)
	@mkdir -p $(@D)
	$(CC_WIN) $(CFLAGS) $(CPPFLAGS) -shared -static-libgcc -MMD -MT $@ -MF $@.d -o $@ $(SRC_LIB) -lz -Wl,--out-implib,$(BUILD_WIN)/libramfile.a

$(BUILD_WIN)/test_ramfile.exe: $(TESTS) $(BUILD_WIN)/ramfile.dll
	$(call say,WIN-CC,$@)
	$(CC_WIN) $(CFLAGS) -static-libgcc -MMD -MT $@ -MF $@.d -o $@ $(TESTS) -L$(BUILD_WIN) -lramfile

$(BENCH_WIN): $(BENCH) $(BUILD_WIN)/ramfile.dll
	$(call say,WIN-CC,$@)
	$(CC_WIN) $(CFLAGS) -static-libgcc -MMD -MT $@ -MF $@.d -o $@ $(BENCH) -L$(BUILD_WIN) -lramfile

test_win: $(BUILD_WIN)/test_ramfile.exe
	cd $(BUILD_WIN) && $(WINE) test_ramfile.exe

test: test_linux test_win

QA_CFLAGS := $(WARN) -I inc -D_POSIX_C_SOURCE=200809L
QA_LDLIBS := -lz

$(B_ASAN)/test_ramfile: $(SRC_LIB) $(TESTS) $(HDR)
	@mkdir -p $(@D)
	$(CC_LINUX) $(QA_CFLAGS) -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=undefined -o $@ $(SRC_LIB) $(TESTS) $(QA_LDLIBS)

asan: $(B_ASAN)/test_ramfile
	./$<

$(B_DBG)/test_ramfile: $(SRC_LIB) $(TESTS) $(HDR)
	@mkdir -p $(@D)
	$(CC_LINUX) $(QA_CFLAGS) -g -O0 -o $@ $(SRC_LIB) $(TESTS) $(QA_LDLIBS)

valgrind: $(B_DBG)/test_ramfile
	valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes --error-exitcode=1 -q ./$<

$(B_DBG)/test_ramfile_ef: $(SRC_LIB) $(TESTS) $(HDR)
	@mkdir -p $(@D)
	$(CC_LINUX) $(QA_CFLAGS) -g -O0 -o $@ $(SRC_LIB) $(TESTS) -lefence $(QA_LDLIBS)

efence: $(B_DBG)/test_ramfile_ef
	EF_PROTECT_BELOW=0 ./$<
	EF_PROTECT_BELOW=1 ./$<

check: test_linux asan valgrind efence

$(B_PERF)/benchmark: $(SRC_LIB) $(BENCH) $(HDR)
	@mkdir -p $(@D)
	$(CC_LINUX) $(QA_CFLAGS) $(if $(MEASURES),-DMEASURES=$(MEASURES)) -O2 -DNDEBUG -g -fno-omit-frame-pointer -o $@ $(BENCH) $(SRC_LIB) $(QA_LDLIBS)

perf: $(B_PERF)/benchmark
	perf stat -e task-clock,page-faults,context-switches ./$<
	perf record -F 2999 --call-graph fp -o $(B_PERF)/perf.data -- ./$<
	perf report -i $(B_PERF)/perf.data --stdio --no-children --percent-limit 2 | head -70

callgrind: $(B_PERF)/benchmark
	valgrind --tool=callgrind --callgrind-out-file=$(B_PERF)/callgrind.out ./$<
	callgrind_annotate $(B_PERF)/callgrind.out | head -40

$(B_PERF)/benchmark-ramfile-only: $(SRC_LIB) $(BENCH_RAM) $(HDR)
	@mkdir -p $(@D)
	$(CC_LINUX) $(QA_CFLAGS) $(if $(MEASURES),-DMEASURES=$(MEASURES)) -O2 -DNDEBUG -g -fno-omit-frame-pointer -o $@ $(BENCH_RAM) $(SRC_LIB) $(QA_LDLIBS)

perf-ramfile-only: $(B_PERF)/benchmark-ramfile-only
	perf stat -e task-clock,page-faults,context-switches ./$<
	perf record -F 2999 --call-graph fp -o $(B_PERF)/perf.data -- ./$<
	perf report -i $(B_PERF)/perf.data --stdio --no-children --percent-limit 2 | head -70

-include $(wildcard $(addsuffix /*.d,$(ALL_DIRS)))
