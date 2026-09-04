# hsdbg test workspace
#
# Targets:
#   hello         a trivial program, for a first launch and single-stepping
#   macro_demo    layered #define macros, for the macro unwrapper
#   trace_demo    nested calls, for the flame-chart timeline
#   sample_demo   plain busywork, for the sampling profiler
#   memory_demo   allocation waves, for the resident-memory graph
#   instr_demo    -finstrument-functions + the trace runtime, auto-traced

UNAME := $(shell uname)

CC  ?= clang
CXX ?= clang++

CFLAGS   := -g -O0 -Wall -Wextra
CXXFLAGS := -g -O0 -Wall -Wextra -std=c++23

OBJ           := .obj
TRACE_RUNTIME := ../ext/hsdbg_trace/hsdbg_trace.c

TARGETS := hello macro_demo trace_demo sample_demo memory_demo instr_demo

ifeq ($(UNAME),Darwin)
    DSYMUTIL := dsymutil
else
    DSYMUTIL := true
endif

all: $(TARGETS)

$(OBJ):
	mkdir -p $(OBJ)

$(OBJ)/%.o: %.cpp | $(OBJ)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJ)/%.o: %.c | $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@

hello: $(OBJ)/hello.o
	$(CXX) $(CXXFLAGS) $^ -o $@
	$(DSYMUTIL) $@

macro_demo: $(OBJ)/macro_demo.o
	$(CC) $(CFLAGS) $^ -o $@
	$(DSYMUTIL) $@

trace_demo: $(OBJ)/trace_demo.o
	$(CXX) $(CXXFLAGS) $^ -o $@
	$(DSYMUTIL) $@

sample_demo: $(OBJ)/sample_demo.o
	$(CXX) $(CXXFLAGS) $^ -o $@
	$(DSYMUTIL) $@

memory_demo: $(OBJ)/memory_demo.o
	$(CXX) $(CXXFLAGS) $^ -o $@
	$(DSYMUTIL) $@

# instr_demo.c is compiled with -finstrument-functions so every function calls
# the enter/exit hooks; the trace runtime that records them is compiled WITHOUT
# the flag (instrumenting the hooks themselves would recurse forever)
$(OBJ)/instr_demo.o: instr_demo.c | $(OBJ)
	$(CC) $(CFLAGS) -finstrument-functions -c $< -o $@

$(OBJ)/hsdbg_trace.o: $(TRACE_RUNTIME) | $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@

instr_demo: $(OBJ)/instr_demo.o $(OBJ)/hsdbg_trace.o
	$(CC) $(CFLAGS) $^ -o $@
	$(DSYMUTIL) $@

clean:
	rm -rf $(TARGETS) *.dSYM $(OBJ)

.PHONY: all clean
