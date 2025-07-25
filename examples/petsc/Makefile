# Copyright (c) 2017-2025, Lawrence Livermore National Security, LLC and other CEED contributors
# All Rights Reserved. See the top-level LICENSE and NOTICE files for details.
#
# SPDX-License-Identifier: BSD-2-Clause
#
# This file is part of CEED:  http://github.com/ceed

COMMON ?= ../../common.mk
-include $(COMMON)

# Note: PETSC_ARCH can be undefined or empty for installations which do not use
#       PETSC_ARCH - for example when using PETSc installed through Spack.
PETSc.pc := $(PETSC_DIR)/$(PETSC_ARCH)/lib/pkgconfig/PETSc.pc
CEED_DIR ?= ../..
ceed.pc := $(CEED_DIR)/lib/pkgconfig/ceed.pc

CC = $(call pkgconf, --variable=ccompiler $(PETSc.pc) $(ceed.pc))
CFLAGS = -std=c11 \
  $(call pkgconf, --variable=cflags_extra $(PETSc.pc)) \
  $(call pkgconf, --cflags-only-other $(PETSc.pc)) \
  $(OPT)
CPPFLAGS = $(call pkgconf, --cflags-only-I $(PETSc.pc) $(ceed.pc)) \
  $(call pkgconf, --variable=cflags_dep $(PETSc.pc))
LDFLAGS = $(call pkgconf, --libs-only-L --libs-only-other $(PETSc.pc) $(ceed.pc))
LDFLAGS += $(patsubst -L%, $(call pkgconf, --variable=ldflag_rpath $(PETSc.pc))%, $(call pkgconf, --libs-only-L $(PETSc.pc) $(ceed.pc)))
LDLIBS = $(call pkgconf, --libs-only-l $(PETSc.pc) $(ceed.pc)) -lm

OBJDIR := build
SRCDIR := src

all: area bps bpsraw bpssphere dmswarm multigrid

utils.c := $(sort $(wildcard $(SRCDIR)/*.c))
utils.o = $(utils.c:%.c=$(OBJDIR)/%.o)
libutils.a: $(utils.o)
	$(call quiet,AR) $(ARFLAGS) $@ $^

area.c := area.c
area.o = $(area.c:%.c=$(OBJDIR)/%.o)
area: $(area.o) libutils.a | $(PETSc.pc) $(ceed.pc)
	$(call quiet,LINK.o) $(CEED_LDFLAGS) $^ $(LOADLIBES) $(LDLIBS) -o $@

bps.c := bps.c
bps.o = $(bps.c:%.c=$(OBJDIR)/%.o)
bps: $(bps.o) libutils.a | $(PETSc.pc) $(ceed.pc)
	$(call quiet,LINK.o) $(CEED_LDFLAGS) $^ $(LOADLIBES) $(LDLIBS) -o $@

bpsraw.c := bpsraw.c
bpsraw.o = $(bpsraw.c:%.c=$(OBJDIR)/%.o)
bpsraw: $(bpsraw.o) libutils.a | $(PETSc.pc) $(ceed.pc)
	$(call quiet,LINK.o) $(CEED_LDFLAGS) $^ $(LOADLIBES) $(LDLIBS) -o $@

bpssphere.c := bpssphere.c
bpssphere.o = $(bpssphere.c:%.c=$(OBJDIR)/%.o)
bpssphere: $(bpssphere.o) libutils.a | $(PETSc.pc) $(ceed.pc)
	$(call quiet,LINK.o) $(CEED_LDFLAGS) $^ $(LOADLIBES) $(LDLIBS) -o $@

bpsswarm.c := bpsswarm.c
bpsswarm.o = $(bpsswarm.c:%.c=$(OBJDIR)/%.o)
bpsswarm: $(bpsswarm.o) libutils.a | $(PETSc.pc) $(ceed.pc)
	$(call quiet,LINK.o) $(CEED_LDFLAGS) $^ $(LOADLIBES) $(LDLIBS) -o $@


dmswarm.c := dmswarm.c
dmswarm.o = $(dmswarm.c:%.c=$(OBJDIR)/%.o)
dmswarm: $(dmswarm.o) libutils.a | $(PETSc.pc) $(ceed.pc)
	$(call quiet,LINK.o) $(CEED_LDFLAGS) $^ $(LOADLIBES) $(LDLIBS) -o $@

multigrid.c := multigrid.c
multigrid.o = $(multigrid.c:%.c=$(OBJDIR)/%.o)
multigrid: $(multigrid.o) libutils.a | $(PETSc.pc) $(ceed.pc)
	$(call quiet,LINK.o) $(CEED_LDFLAGS) $^ $(LOADLIBES) $(LDLIBS) -o $@

.SECONDEXPANSION: # to expand $$(@D)/.DIR
%/.DIR :
	@mkdir -p $(@D)
	@touch $@

# Quiet, color output
quiet ?= $($(1))

$(OBJDIR)/%.o : %.c | $$(@D)/.DIR
	$(call quiet,CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $(abspath $<)

# Rules for building the examples
#%: %.c

print: $(PETSc.pc) $(ceed.pc)
	$(info CC      : $(CC))
	$(info CFLAGS  : $(CFLAGS))
	$(info CPPFLAGS: $(CPPFLAGS))
	$(info LDFLAGS : $(LDFLAGS))
	$(info LDLIBS  : $(LDLIBS))
	@true

clean:
	$(RM) -r $(OBJDIR) *.vtu area bps bpsraw bpssphere dmswarm multigrid libutils.a

$(PETSc.pc):
	$(if $(wildcard $@),,$(error \
	  PETSc config not found at $@. Please set PETSC_DIR and PETSC_ARCH))

.PHONY: all print clean

pkgconf = $(shell pkg-config $1 | sed -e 's/^"//g' -e 's/"$$//g')

-include $(src.o:%.o=%.d)
