# Copyright (c) 2017-2025, Lawrence Livermore National Security, LLC and other CEED contributors
# All Rights Reserved. See the top-level LICENSE and NOTICE files for details.
#
# SPDX-License-Identifier: BSD-2-Clause
#
# This file is part of CEED:  http://github.com/ceed

OPT ?= -O -g

# Ceed directory
CEED_DIR ?= ../..
CEED_FLAGS ?= -I$(CEED_DIR)/include -std=c11  $(OPT)
CEED_LIBS ?= -Wl,-rpath,$(abspath $(CEED_DIR)/lib) -L$(CEED_DIR)/lib -lceed -L$(CEED_DIR)/examples/ceed -lm

EXAMPLES.c = $(wildcard ex*.c)
EXAMPLES = $(EXAMPLES.c:%.c=%)

.SUFFIXES:
.SUFFIXES: .c
.PHONY: all clean

all: $(EXAMPLES)

# Remove built-in rules
%: %.c

# Special build rule for example 4 (rust)
ex4-volume-rust: ex4-volume-rust.c
	cargo +nightly build --release --manifest-path ex4-volume-rs/Cargo.toml --config ex4-volume-rs/.cargo/config.toml
	$(LINK.c) $(CEED_FLAGS) $(CEED_LDFLAGS) $< -o $@ $(CEED_LIBS) -L$(CEED_DIR)/examples/ceed/ex4-volume-rs/target/release -lex4_volume_rs

# Rules for building the examples
%: %.c
	$(LINK.c) $(CEED_FLAGS) $(CEED_LDFLAGS) $< -o $@ $(CEED_LIBS)

clean:
	rm -f *~ $(EXAMPLES)
	rm -rf *.dSYM *.TVD.*breakpoints
