# Copyright (c) 2017-2025, Lawrence Livermore National Security, LLC and other CEED contributors.
# All Rights Reserved. See the top-level LICENSE and NOTICE files for details.
#
# SPDX-License-Identifier: BSD-2-Clause
#
# This file is part of CEED:  http://github.com/ceed

# ------------------------------------------------------------
# Configuration
# ------------------------------------------------------------

# config.mk stores cached configuration variables
CONFIG ?= config.mk
-include $(CONFIG)

# common.mk holds definitions used in various makefiles throughout the project
COMMON ?= common.mk
-include $(COMMON)

# Quiet, color output
quiet ?= $($(1))

# Cancel built-in and old-fashioned implicit rules which we don't use
.SUFFIXES:

.SECONDEXPANSION: # to expand $$(@D)/.DIR

%/.DIR :
	@mkdir -p $(@D)
	@touch $@

.PRECIOUS: %/.DIR


# ------------------------------------------------------------
# Root directories for backend dependencies
# ------------------------------------------------------------

# XSMM_DIR env variable should point to XSMM main (github.com/hfp/libxsmm)
XSMM_DIR ?= ../libxsmm

# Often /opt/cuda or /usr/local/cuda, but sometimes present on machines that don't support CUDA
CUDA_DIR  ?=
CUDA_ARCH ?=

# Often /opt/rocm, but sometimes present on machines that don't support HIP
ROCM_DIR ?=
HIP_ARCH ?=

# env variable MAGMA_DIR can be used too
MAGMA_DIR ?= ../magma

# OCCA_DIR env variable should point to OCCA main (github.com/libocca/occa)
OCCA_DIR ?= ../occa/install


# ------------------------------------------------------------
# Compiler flags
# ------------------------------------------------------------

# Detect user compiler options and set defaults
ifeq (,$(filter-out undefined default,$(origin CC)))
  CC = gcc
endif
ifeq (,$(filter-out undefined default,$(origin CXX)))
  CXX = g++
endif
ifeq (,$(filter-out undefined default,$(origin FC)))
  FC = gfortran
endif
ifeq (,$(filter-out undefined default,$(origin LINK)))
  LINK = $(CC)
endif
ifeq (,$(filter-out undefined default,$(origin AR)))
  AR = ar
endif
ifeq (,$(filter-out undefined default,$(origin ARFLAGS)))
  ARFLAGS = crD
endif
NVCC ?= $(CUDA_DIR)/bin/nvcc
NVCC_CXX ?= $(CXX)
HIPCC ?= $(ROCM_DIR)/bin/hipcc
SYCLCXX ?= $(CXX)
SED ?= sed
ifneq ($(EMSCRIPTEN),)
  STATIC = 1
  EXE_SUFFIX = .wasm
  EM_LDFLAGS = -s TOTAL_MEMORY=256MB
endif

# ASAN must be left empty if you don't want to use it
ASAN ?=

# These are the values automatically detected here in the makefile. They are
# augmented with LDFLAGS and LDLIBS from the environment/passed by command line,
# if any. If the user sets CEED_LDFLAGS or CEED_LDLIBS, they are used *instead
# of* what we populate here (thus that's advanced usage and not recommended).
CEED_LDFLAGS ?=
CEED_LDLIBS  ?=

UNDERSCORE ?= 1

# Verbose mode, V or VERBOSE
V ?= $(VERBOSE)

# Warning: SANTIZ options still don't run with /gpu/occa
AFLAGS ?= -fsanitize=address #-fsanitize=undefined -fno-omit-frame-pointer

# Note: Intel oneAPI C/C++ compiler is now icx/icpx
CC_VENDOR := $(firstword $(filter gcc (GCC) clang cc icc icc_orig oneAPI XL emcc,$(subst -, ,$(shell $(CC) --version))))
CC_VENDOR := $(subst (GCC),gcc,$(subst icc_orig,icc,$(CC_VENDOR)))
CC_VENDOR := $(if $(filter cc,$(CC_VENDOR)),gcc,$(CC_VENDOR))
FC_VENDOR := $(if $(FC),$(firstword $(filter GNU ifort ifx XL,$(shell $(FC) --version 2>&1 || $(FC) -qversion))))

# Default extra flags by vendor
MARCHFLAG.gcc           := -march=native
MARCHFLAG.clang         := $(MARCHFLAG.gcc)
MARCHFLAG.icc           :=
MARCHFLAG.oneAPI        := $(MARCHFLAG.clang)
OMP_SIMD_FLAG.gcc       := -fopenmp-simd
OMP_SIMD_FLAG.clang     := $(OMP_SIMD_FLAG.gcc)
OMP_SIMD_FLAG.icc       := -qopenmp-simd
OMP_SIMD_FLAG.oneAPI    := $(OMP_SIMD_FLAG.icc)
OMP_FLAG.gcc            := -fopenmp
OMP_FLAG.clang          := $(OMP_FLAG.gcc)
OMP_FLAG.icc            := -qopenmp
OMP_FLAG.oneAPI         := $(OMP_FLAG.icc)
SYCL_FLAG.gcc           :=
SYCL_FLAG.clang         := -fsycl
SYCL_FLAG.icc           :=
SYCL_FLAG.oneAPI        := -fsycl -fno-sycl-id-queries-fit-in-int
OPT.gcc                 := -g -ffp-contract=fast
OPT.clang               := $(OPT.gcc)
OPT.icc                 := $(OPT.gcc)
OPT.oneAPI              := $(OPT.clang)
OPT.emcc                :=
CFLAGS.gcc              := $(if $(STATIC),,-fPIC) -std=c11 -Wall -Wextra -Wno-unused-parameter -MMD -MP
CFLAGS.clang            := $(CFLAGS.gcc)
CFLAGS.icc              := $(CFLAGS.gcc)
CFLAGS.oneAPI           := $(CFLAGS.clang)
CFLAGS.XL               := $(if $(STATIC),,-qpic) -MMD
CFLAGS.emcc             := $(CFLAGS.clang)
CXXFLAGS.gcc            := $(if $(STATIC),,-fPIC) -std=c++11 -Wall -Wextra -Wno-unused-parameter -MMD -MP
CXXFLAGS.clang          := $(CXXFLAGS.gcc)
CXXFLAGS.icc            := $(CXXFLAGS.gcc)
CXXFLAGS.oneAPI         := $(CXXFLAGS.clang)
CXXFLAGS.XL             := $(if $(STATIC),,-qpic) -std=c++11 -MMD
CXXFLAGS.emcc           := $(CXXFLAGS.clang)
FFLAGS.GNU              := $(if $(STATIC),,-fPIC) -cpp -Wall -Wextra -Wno-unused-parameter -Wno-unused-dummy-argument -MMD -MP
FFLAGS.ifort            := $(if $(STATIC),,-fPIC) -cpp
FFLAGS.ifx              := $(FFLAGS.ifort)
FFLAGS.XL               := $(if $(STATIC),,-qpic) -ffree-form -qpreprocess -qextname -MMD

# This check works with compilers that use gcc and clang.  It fails with some
# compilers; e.g., xlc apparently ignores all options when -E is passed, thus
# succeeds with any flags.  Users can pass MARCHFLAG=... if desired.
cc_check_flag = $(shell $(CC) -E -Werror $(1) -x c /dev/null > /dev/null 2>&1 && echo 1)
MARCHFLAG := $(MARCHFLAG.$(CC_VENDOR))
MARCHFLAG := $(if $(call cc_check_flag,$(MARCHFLAG)),$(MARCHFLAG),-mcpu=native)
MARCHFLAG := $(if $(call cc_check_flag,$(MARCHFLAG)),$(MARCHFLAG))

OMP_SIMD_FLAG := $(OMP_SIMD_FLAG.$(CC_VENDOR))
OMP_SIMD_FLAG := $(if $(call cc_check_flag,$(OMP_SIMD_FLAG)),$(OMP_SIMD_FLAG))

# Error checking flags
PEDANTIC      ?=
PEDANTICFLAGS ?= -Werror -pedantic

# Compiler flags
OPT    ?= -O $(MARCHFLAG) $(OPT.$(CC_VENDOR)) $(OMP_SIMD_FLAG)
CFLAGS ?= $(OPT) $(CFLAGS.$(CC_VENDOR)) $(if $(PEDANTIC),$(PEDANTICFLAGS))
CXXFLAGS ?= $(OPT) $(CXXFLAGS.$(CC_VENDOR)) $(if $(PEDANTIC),$(PEDANTICFLAGS))
FFLAGS ?= $(OPT) $(FFLAGS.$(FC_VENDOR))
LIBCXX ?= -lstdc++
NVCCFLAGS ?= -ccbin $(CXX) -Xcompiler '$(OPT)' -Xcompiler -fPIC
ifneq ($(CUDA_ARCH),)
  NVCCFLAGS += -arch=$(CUDA_ARCH)
endif
HIPCCFLAGS ?= $(filter-out $(OMP_SIMD_FLAG),$(OPT)) -fPIC -munsafe-fp-atomics
ifneq ($(HIP_ARCH),)
  HIPCCFLAGS += --offload-arch=$(HIP_ARCH)
endif
SYCL_FLAG := $(SYCL_FLAG.$(CC_VENDOR))
SYCLFLAGS ?= $(SYCL_FLAG) -fPIC -std=c++17 $(filter-out -std=c++11,$(CXXFLAGS)) $(filter-out $(OMP_SIMD_FLAG),$(OPT))

OPENMP ?=
ifneq ($(OPENMP),)
  OMP_FLAG := $(OMP_FLAG.$(CC_VENDOR))
  OMP_FLAG := $(if $(call cc_check_flag,$(OMP_FLAG)),$(OMP_FLAG))
  CFLAGS += $(OMP_FLAG)
  CEED_LDFLAGS += $(OMP_FLAG)
endif

ifeq ($(COVERAGE), 1)
  CFLAGS += --coverage
  CXXFLAGS += --coverage
  CEED_LDFLAGS += --coverage
endif

CFLAGS += $(if $(ASAN),$(AFLAGS))
FFLAGS += $(if $(ASAN),$(AFLAGS))
CEED_LDFLAGS += $(if $(ASAN),$(AFLAGS))
CPPFLAGS += -I./include
CEED_LDLIBS = -lm
OBJDIR := build
for_install := $(filter install,$(MAKECMDGOALS))
LIBDIR := $(if $(for_install),$(OBJDIR),lib)

# Installation variables
prefix ?= /usr/local
bindir = $(prefix)/bin
libdir = $(prefix)/lib
includedir = $(prefix)/include
pkgconfigdir = $(libdir)/pkgconfig
INSTALL = install
INSTALL_PROGRAM = $(INSTALL)
INSTALL_DATA = $(INSTALL) -m644

# Get number of processors of the machine
NPROCS := $(shell getconf _NPROCESSORS_ONLN)
# prepare make options to run in parallel
MFLAGS := -j $(NPROCS) --warn-undefined-variables \
                       --no-print-directory --no-keep-going

PYTHON ?= python3
PROVE ?= prove
PROVE_OPTS ?= -j $(NPROCS)
DARWIN := $(filter Darwin,$(shell uname -s))
SO_EXT := $(if $(DARWIN),dylib,so)

ceed.pc := $(LIBDIR)/pkgconfig/ceed.pc
libceed.so := $(LIBDIR)/libceed.$(SO_EXT)
libceed.a := $(LIBDIR)/libceed.a
libceed := $(if $(STATIC),$(libceed.a),$(libceed.so))
CEED_LIBS = -lceed
libceeds = $(libceed)
BACKENDS_BUILTIN := /cpu/self/ref/serial /cpu/self/ref/blocked /cpu/self/opt/serial /cpu/self/opt/blocked
BACKENDS_MAKE := $(BACKENDS_BUILTIN)


# ------------------------------------------------------------
# Root directories for examples using external libraries
# ------------------------------------------------------------

# DEAL_II_DIR env variable should point to sibling directory
ifneq ($(wildcard ../dealii/install/lib/libdeal_II.*),)
  DEAL_II_DIR ?= ../dealii/install
endif
# Export for deal.II testing
export DEAL_II_DIR

# MFEM_DIR env variable should point to sibling directory
ifneq ($(wildcard ../mfem/libmfem.*),)
  MFEM_DIR ?= ../mfem
endif

# NEK5K_DIR env variable should point to sibling directory
ifneq ($(wildcard ../Nek5000/*),)
  NEK5K_DIR ?= $(abspath ../Nek5000)
endif
# Exports for NEK5K testing
export CEED_DIR = $(abspath .)
export NEK5K_DIR
MPI ?= 1

# Check for PETSc in ../petsc
ifneq ($(wildcard ../petsc/lib/libpetsc.*),)
  PETSC_DIR ?= ../petsc
endif

# ------------------------------------------------------------
# Build the library (default target)
# ------------------------------------------------------------

lib: $(libceed) $(ceed.pc)
# run 'lib' target in parallel
par:;@$(MAKE) $(MFLAGS) V=$(V) lib

$(libceed.so) : CEED_LDFLAGS += $(if $(DARWIN), -install_name @rpath/$(notdir $(libceed.so)))

# ------------------------------------------------------------
# Source files
# ------------------------------------------------------------

# Interface and gallery
libceed.c := $(filter-out interface/ceed-cuda.c interface/ceed-hip.c interface/ceed-jit-source-root-$(if $(for_install),default,install).c, $(wildcard interface/ceed*.c backends/*.c gallery/*.c))
gallery.c := $(wildcard gallery/*/ceed*.c)
libceed.c += $(gallery.c)

# Backends
# - CPU
ref.c          := $(sort $(wildcard backends/ref/*.c))
blocked.c      := $(sort $(wildcard backends/blocked/*.c))
ceedmemcheck.c := $(sort $(wildcard backends/memcheck/*.c))
opt.c          := $(sort $(wildcard backends/opt/*.c))
avx.c          := $(sort $(wildcard backends/avx/*.c))
xsmm.c         := $(sort $(wildcard backends/xsmm/*.c))
# - GPU
cuda.c         := $(sort $(wildcard backends/cuda/*.c))
cuda.cpp       := $(sort $(wildcard backends/cuda/*.cpp))
cuda-ref.c     := $(sort $(wildcard backends/cuda-ref/*.c))
cuda-ref.cpp   := $(sort $(wildcard backends/cuda-ref/*.cpp))
cuda-ref.cu    := $(sort $(wildcard backends/cuda-ref/kernels/*.cu))
cuda-shared.c  := $(sort $(wildcard backends/cuda-shared/*.c))
cuda-gen.c     := $(sort $(wildcard backends/cuda-gen/*.c))
cuda-gen.cpp   := $(sort $(wildcard backends/cuda-gen/*.cpp))
cuda-all.c     := interface/ceed-cuda.c $(cuda.c) $(cuda-ref.c) $(cuda-shared.c) $(cuda-gen.c)
cuda-all.cpp   := $(cuda.cpp) $(cuda-ref.cpp) $(cuda-gen.cpp)
cuda-all.cu    := $(cuda-ref.cu)
hip.c          := $(sort $(wildcard backends/hip/*.c))
hip.cpp        := $(sort $(wildcard backends/hip/*.cpp))
hip-ref.c      := $(sort $(wildcard backends/hip-ref/*.c))
hip-ref.cpp    := $(sort $(wildcard backends/hip-ref/*.cpp))
hip-ref.hip    := $(sort $(wildcard backends/hip-ref/kernels/*.hip.cpp))
hip-shared.c   := $(sort $(wildcard backends/hip-shared/*.c))
hip-gen.c      := $(sort $(wildcard backends/hip-gen/*.c))
hip-gen.cpp    := $(sort $(wildcard backends/hip-gen/*.cpp))
hip-all.c      := interface/ceed-hip.c $(hip.c) $(hip-ref.c) $(hip-shared.c) $(hip-gen.c)
hip-all.cpp    := $(hip.cpp) $(hip-ref.cpp) $(hip-gen.cpp)
hip-all.hip    := $(hip-ref.hip)
sycl-core.cpp  := $(sort $(wildcard backends/sycl/*.sycl.cpp))
sycl-ref.cpp   := $(sort $(wildcard backends/sycl-ref/*.sycl.cpp))
sycl-shared.cpp:= $(sort $(wildcard backends/sycl-shared/*.sycl.cpp))
sycl-gen.cpp   := $(sort $(wildcard backends/sycl-gen/*.sycl.cpp))
magma.c        := $(sort $(wildcard backends/magma/*.c))
magma.cpp      := $(sort $(wildcard backends/magma/*.cpp))
occa.cpp       := $(sort $(shell find backends/occa -type f -name *.cpp))

# Tests
tests.c := $(sort $(wildcard tests/t[0-9][0-9][0-9]-*.c))
tests.f := $(if $(FC),$(sort $(wildcard tests/t[0-9][0-9][0-9]-*.f90)))
tests   := $(tests.c:tests/%.c=$(OBJDIR)/%$(EXE_SUFFIX))
ctests  := $(tests)
tests   += $(tests.f:tests/%.f90=$(OBJDIR)/%$(EXE_SUFFIX))

# Examples
examples.c := $(sort $(wildcard examples/ceed/*.c))
examples.f := $(if $(FC),$(sort $(wildcard examples/ceed/*.f)))
examples   := $(examples.c:examples/ceed/%.c=$(OBJDIR)/%$(EXE_SUFFIX))
examples   += $(examples.f:examples/ceed/%.f=$(OBJDIR)/%$(EXE_SUFFIX))

# deal.II Examples
dealiiexamples := $(OBJDIR)/dealii-bps

# MFEM Examples
mfemexamples.cpp := $(sort $(wildcard examples/mfem/*.cpp))
mfemexamples     := $(mfemexamples.cpp:examples/mfem/%.cpp=$(OBJDIR)/mfem-%)

# Nek5K Examples
nekexamples := $(OBJDIR)/nek-bps

# Rust QFunction Examples
rustqfunctions.c       := $(sort $(wildcard examples/rust-qfunctions/*.c))
rustqfunctionsexamples := $(rustqfunctions.c:examples/rust-qfunctions/%.c=$(OBJDIR)/rustqfunctions-%)

# PETSc Examples
petscexamples.c := $(wildcard examples/petsc/*.c)
petscexamples   := $(petscexamples.c:examples/petsc/%.c=$(OBJDIR)/petsc-%)

# Fluid Dynamics Example
fluidsexamples.c := $(sort $(wildcard examples/fluids/*.c))
fluidsexamples   := $(fluidsexamples.c:examples/fluids/%.c=$(OBJDIR)/fluids-%)

# Solid Mechanics Example
solidsexamples.c := $(sort $(wildcard examples/solids/*.c))
solidsexamples   := $(solidsexamples.c:examples/solids/%.c=$(OBJDIR)/solids-%)


# ------------------------------------------------------------
# View configuration options
# ------------------------------------------------------------

backend_status = $(if $(filter $1,$(BACKENDS_MAKE)), [backends: $1], [not found])

info-basic:
	$(info -----------------------------------------)
	$(info |     ___ __    ______________________  |)
	$(info |    / (_) /_  / ____/ ____/ ____/ __ \ |)
	$(info |   / / / __ \/ /   / __/ / __/ / / / / |)
	$(info |  / / / /_/ / /___/ /___/ /___/ /_/ /  |)
	$(info | /_/_/_.___/\____/_____/_____/_____/   |)
	$(info -----------------------------------------)
	$(info )
	$(info -----------------------------------------)
	$(info )
	$(info Built-in Backends:)
	$(info   $(BACKENDS_BUILTIN))
	$(info )
	$(info Additional Backends:)
	$(info   $(filter-out $(BACKENDS_BUILTIN),$(BACKENDS)))
	$(info )
	$(info -----------------------------------------)
	$(info )
	@true

info:
	$(info -----------------------------------------)
	$(info |     ___ __    ______________________  |)
	$(info |    / (_) /_  / ____/ ____/ ____/ __ \ |)
	$(info |   / / / __ \/ /   / __/ / __/ / / / / |)
	$(info |  / / / /_/ / /___/ /___/ /___/ /_/ /  |)
	$(info | /_/_/_.___/\____/_____/_____/_____/   |)
	$(info -----------------------------------------)
	$(info )
	$(info -----------------------------------------)
	$(info )
	$(info Built-in Backends:)
	$(info   $(BACKENDS_BUILTIN))
	$(info )
	$(info Additional Backends:)
	$(info   $(filter-out $(BACKENDS_BUILTIN),$(BACKENDS)))
	$(info )
	$(info -----------------------------------------)
	$(info )
	$(info Compiler Flags:)
	$(info CC            = $(CC))
	$(info CXX           = $(CXX))
	$(info FC            = $(FC))
	$(info CPPFLAGS      = $(CPPFLAGS))
	$(info CFLAGS        = $(CFLAGS))
	$(info CXXFLAGS      = $(CXXFLAGS))
	$(info FFLAGS        = $(FFLAGS))
	$(info NVCCFLAGS     = $(NVCCFLAGS))
	$(info HIPCCFLAGS    = $(HIPCCFLAGS))
	$(info SYCLFLAGS     = $(SYCLFLAGS))
	$(info CEED_LDFLAGS  = $(CEED_LDFLAGS))
	$(info CEED_LDLIBS   = $(CEED_LDLIBS))
	$(info AR            = $(AR))
	$(info ARFLAGS       = $(ARFLAGS))
	$(info OPT           = $(OPT))
	$(info AFLAGS        = $(AFLAGS))
	$(info ASAN          = $(or $(ASAN),(empty)))
	$(info VERBOSE       = $(or $(V),(empty)) [verbose=$(if $(V),on,off)])
	$(info )
	$(info -----------------------------------------)
	$(info )
	$(info Backend Dependencies:)
	$(info MEMCHK_STATUS = $(MEMCHK_STATUS)$(call backend_status,$(MEMCHK_BACKENDS)))
	$(info AVX_STATUS    = $(AVX_STATUS)$(call backend_status,$(AVX_BACKENDS)))
	$(info XSMM_DIR      = $(XSMM_DIR)$(call backend_status,$(XSMM_BACKENDS)))
	$(info CUDA_DIR      = $(CUDA_DIR)$(call backend_status,$(CUDA_BACKENDS)))
	$(info ROCM_DIR      = $(ROCM_DIR)$(call backend_status,$(HIP_BACKENDS)))
	$(info SYCL_DIR      = $(SYCL_DIR)$(call backend_status,$(SYCL_BACKENDS)))
	$(info MAGMA_DIR     = $(MAGMA_DIR)$(call backend_status,$(MAGMA_BACKENDS)))
	$(info OCCA_DIR      = $(OCCA_DIR)$(call backend_status,$(OCCA_BACKENDS)))
	$(info )
	$(info -----------------------------------------)
	$(info )
	$(info Example Dependencies:)
	$(info MFEM_DIR      = $(MFEM_DIR))
	$(info NEK5K_DIR     = $(NEK5K_DIR))
	$(info PETSC_DIR     = $(PETSC_DIR))
	$(info DEAL_II_DIR   = $(DEAL_II_DIR))
	$(info )
	$(info -----------------------------------------)
	$(info )
	$(info Install Options:)
	$(info prefix        = $(prefix))
	$(info includedir    = $(value includedir))
	$(info libdir        = $(value libdir))
	$(info pkgconfigdir  = $(value pkgconfigdir))
	$(info )
	$(info -----------------------------------------)
	$(info )
	$(info Git:)
	$(info describe      = $(GIT_DESCRIBE))
	$(info )
	$(info -----------------------------------------)
	@true

info-backends:
	$(info make: 'lib' with optional backends: $(filter-out $(BACKENDS_BUILTIN),$(BACKENDS)))
	@true

info-backends-all:
	$(info make: 'lib' with backends: $(BACKENDS))
	@true


# ------------------------------------------------------------
# Backends
# ------------------------------------------------------------

# Standard Backends
libceed.c += $(ref.c)
libceed.c += $(blocked.c)
libceed.c += $(opt.c)

# Memcheck Backends
MEMCHK_STATUS   = Disabled
MEMCHK         := $(shell echo "$(HASH)include <valgrind/memcheck.h>" | $(CC) $(CPPFLAGS) -E - >/dev/null 2>&1 && echo 1)
MEMCHK_BACKENDS = /cpu/self/memcheck/serial /cpu/self/memcheck/blocked
ifeq ($(MEMCHK),1)
  MEMCHK_STATUS = Enabled
  libceed.c += $(ceedmemcheck.c)
  BACKENDS_MAKE += $(MEMCHK_BACKENDS)
endif

# AVX Backeds
AVX_STATUS   = Disabled
AVX_FLAG    := $(if $(filter clang,$(CC_VENDOR)),+avx,-mavx)
AVX         := $(filter $(AVX_FLAG),$(shell $(CC) $(CFLAGS:-M%=) -v -E -x c /dev/null 2>&1))
AVX_BACKENDS = /cpu/self/avx/serial /cpu/self/avx/blocked
ifneq ($(AVX),)
  AVX_STATUS = Enabled
  libceed.c += $(avx.c)
  BACKENDS_MAKE += $(AVX_BACKENDS)
endif

# Collect list of libraries and paths for use in linking and pkg-config
PKG_LIBS =
# Stubs that will not be RPATH'd
PKG_STUBS_LIBS =

# libXSMM Backends
XSMM_BACKENDS = /cpu/self/xsmm/serial /cpu/self/xsmm/blocked
ifneq ($(wildcard $(XSMM_DIR)/lib/libxsmm.*),)
  PKG_LIBS += -L$(abspath $(XSMM_DIR))/lib -lxsmm
  MKL ?=
  ifeq (,$(MKL)$(MKLROOT))
    BLAS_LIB ?= -lblas -ldl
  else
    ifneq ($(MKLROOT),)
      # Some installs put everything inside an intel64 subdirectory, others not
      MKL_LIBDIR = $(dir $(firstword $(wildcard $(MKLROOT)/lib/intel64/libmkl_sequential.* $(MKLROOT)/lib/libmkl_sequential.*)))
      MKL_LINK = -L$(MKL_LIBDIR)
    endif
    BLAS_LIB ?= $(MKL_LINK) -Wl,--push-state,--no-as-needed -lmkl_intel_lp64 -lmkl_sequential -lmkl_core -lpthread -lm -ldl -Wl,--pop-state
  endif
  PKG_LIBS += $(BLAS_LIB)
  libceed.c += $(xsmm.c)
  $(xsmm.c:%.c=$(OBJDIR)/%.o) $(xsmm.c:%=%.tidy) : CPPFLAGS += -I$(XSMM_DIR)/include
  BACKENDS_MAKE += $(XSMM_BACKENDS)
endif

# CUDA Backends
ifneq ($(CUDA_DIR),)
  CUDA_LIB_DIR := $(wildcard $(foreach d,lib lib64 lib/x86_64-linux-gnu,$(CUDA_DIR)/$d/libcudart.${SO_EXT}))
  CUDA_LIB_DIR := $(patsubst %/,%,$(dir $(firstword $(CUDA_LIB_DIR))))
endif
CUDA_LIB_DIR_STUBS := $(CUDA_LIB_DIR)/stubs
CUDA_BACKENDS = /gpu/cuda/ref /gpu/cuda/shared /gpu/cuda/gen
ifneq ($(CUDA_LIB_DIR),)
  $(libceeds) : CPPFLAGS += -I$(CUDA_DIR)/include
  PKG_LIBS += -L$(abspath $(CUDA_LIB_DIR)) -lcudart -lnvrtc -lcuda -lcublas
  PKG_STUBS_LIBS += -L$(CUDA_LIB_DIR_STUBS)
  LIBCEED_CONTAINS_CXX = 1
  libceed.c     += interface/ceed-cuda.c
  libceed.c     += $(cuda-all.c)
  libceed.cpp   += $(cuda-all.cpp)
  libceed.cu    += $(cuda-all.cu)
  BACKENDS_MAKE += $(CUDA_BACKENDS)
endif

# HIP Backends
HIP_LIB_DIR := $(wildcard $(foreach d,lib lib64,$(ROCM_DIR)/$d/libamdhip64.${SO_EXT}))
HIP_LIB_DIR := $(patsubst %/,%,$(dir $(firstword $(HIP_LIB_DIR))))
HIP_BACKENDS = /gpu/hip/ref /gpu/hip/shared /gpu/hip/gen
ifneq ($(HIP_LIB_DIR),)
  HIPCONFIG_CPPFLAGS := $(subst =,,$(shell $(ROCM_DIR)/bin/hipconfig -C))
  $(hip-all.c:%.c=$(OBJDIR)/%.o) $(hip-all.c:%=%.tidy): CPPFLAGS += $(HIPCONFIG_CPPFLAGS)
  ifneq ($(CXX), $(HIPCC))
    $(hip-all.cpp:%.cpp=$(OBJDIR)/%.o) $(hip-all.cpp:%=%.tidy): CPPFLAGS += $(HIPCONFIG_CPPFLAGS)
  endif
  PKG_LIBS += -L$(abspath $(HIP_LIB_DIR)) -lamdhip64 -lhipblas
  LIBCEED_CONTAINS_CXX = 1
  libceed.c     += $(hip-all.c)
  libceed.cpp   += $(hip-all.cpp)
  libceed.hip   += $(hip-all.hip)
  BACKENDS_MAKE += $(HIP_BACKENDS)
endif

# SYCL Backends
SYCL_BACKENDS = /gpu/sycl/ref /gpu/sycl/shared /gpu/sycl/gen
ifneq ($(SYCL_DIR),)
  SYCL_LIB_DIR := $(wildcard $(foreach d,lib lib64,$(SYCL_DIR)/$d/libsycl.${SO_EXT}))
  SYCL_LIB_DIR := $(patsubst %/,%,$(dir $(firstword $(SYCL_LIB_DIR))))
endif
ifneq ($(SYCL_LIB_DIR),)
  PKG_LIBS += $(SYCL_FLAG) -lze_loader
  LIBCEED_CONTAINS_CXX = 1
  libceed.sycl  += $(sycl-core.cpp) $(sycl-ref.cpp) $(sycl-shared.cpp) $(sycl-gen.cpp)
  BACKENDS_MAKE += $(SYCL_BACKENDS)
endif

# MAGMA Backends
ifneq ($(wildcard $(MAGMA_DIR)/lib/libmagma.*),)
  MAGMA_ARCH=$(shell nm -g $(MAGMA_DIR)/lib/libmagma.* | grep -c "hipblas")
  ifeq ($(MAGMA_ARCH), 0)  # CUDA MAGMA
    ifneq ($(CUDA_LIB_DIR),)
      cuda_link = $(if $(STATIC),,-Wl,-rpath,$(CUDA_LIB_DIR)) -L$(CUDA_LIB_DIR) -lcublas -lcusparse -lcudart
      omp_link = -fopenmp
      magma_link_static = -L$(MAGMA_DIR)/lib -lmagma $(cuda_link) $(omp_link)
      magma_link_shared = -L$(MAGMA_DIR)/lib $(if $(STATIC),,-Wl,-rpath,$(abspath $(MAGMA_DIR)/lib)) -lmagma
      magma_link := $(if $(wildcard $(MAGMA_DIR)/lib/libmagma.${SO_EXT}),$(magma_link_shared),$(magma_link_static))
      PKG_LIBS += $(magma_link)
      libceed.c   += $(magma.c)
      libceed.cpp += $(magma.cpp)
      $(magma.c:%.c=$(OBJDIR)/%.o) $(magma.c:%=%.tidy) : CPPFLAGS += -DADD_ -I$(MAGMA_DIR)/include -I$(CUDA_DIR)/include
      $(magma.cpp:%.cpp=$(OBJDIR)/%.o) $(magma.cpp:%=%.tidy) : CPPFLAGS += -DADD_ -I$(MAGMA_DIR)/include -I$(CUDA_DIR)/include
      MAGMA_BACKENDS = /gpu/cuda/magma /gpu/cuda/magma/det
    endif
  else  # HIP MAGMA
    ifneq ($(HIP_LIB_DIR),)
      omp_link = -fopenmp
      hip_link = $(if $(STATIC),,-Wl,-rpath,$(HIP_LIB_DIR)) -L$(HIP_LIB_DIR) -lhipblas -lhipsparse -lamdhip64
      magma_link_static = -L$(MAGMA_DIR)/lib -lmagma $(hip_link) $(omp_link)
      magma_link_shared = -L$(MAGMA_DIR)/lib $(hip_link) $(omp_link) $(if $(STATIC),,-Wl,-rpath,$(abspath $(MAGMA_DIR)/lib)) -lmagma
      magma_link := $(if $(wildcard $(MAGMA_DIR)/lib/libmagma.${SO_EXT}),$(magma_link_shared),$(magma_link_static))
      PKG_LIBS += $(magma_link)
      libceed.c   += $(magma.c)
      libceed.cpp += $(magma.cpp)
      $(magma.c:%.c=$(OBJDIR)/%.o) $(magma.c:%=%.tidy) : CPPFLAGS += $(HIPCONFIG_CPPFLAGS) -I$(MAGMA_DIR)/include -I$(ROCM_DIR)/include -DCEED_MAGMA_USE_HIP -DADD_
      $(magma.cpp:%.cpp=$(OBJDIR)/%.o) $(magma.cpp:%=%.tidy) : CPPFLAGS += $(HIPCONFIG_CPPFLAGS) -I$(MAGMA_DIR)/include -I$(ROCM_DIR)/include -DCEED_MAGMA_USE_HIP -DADD_
      MAGMA_BACKENDS = /gpu/hip/magma /gpu/hip/magma/det
    endif
  endif
  LIBCEED_CONTAINS_CXX = 1
  BACKENDS_MAKE += $(MAGMA_BACKENDS)
endif

# OCCA Backends
OCCA_BACKENDS = /cpu/self/occa
ifneq ($(wildcard $(OCCA_DIR)/lib/libocca.*),)
  OCCA_MODES := $(shell LD_LIBRARY_PATH=$(OCCA_DIR)/lib $(OCCA_DIR)/bin/occa modes)
  OCCA_BACKENDS += $(if $(filter OpenMP,$(OCCA_MODES)),/cpu/openmp/occa)
  OCCA_BACKENDS += $(if $(filter dpcpp,$(OCCA_MODES)),/gpu/dpcpp/occa)
  OCCA_BACKENDS += $(if $(filter OpenCL,$(OCCA_MODES)),/gpu/opencl/occa)
  OCCA_BACKENDS += $(if $(filter HIP,$(OCCA_MODES)),/gpu/hip/occa)
  OCCA_BACKENDS += $(if $(filter CUDA,$(OCCA_MODES)),/gpu/cuda/occa)
  $(libceeds) : CPPFLAGS += -I$(OCCA_DIR)/include
  PKG_LIBS += -L$(abspath $(OCCA_DIR))/lib -locca
  LIBCEED_CONTAINS_CXX = 1
  libceed.cpp += $(occa.cpp)
  BACKENDS_MAKE += $(OCCA_BACKENDS)
endif

BACKENDS ?= $(BACKENDS_MAKE)
export BACKENDS


# ------------------------------------------------------------
# Linker Flags
# ------------------------------------------------------------

_pkg_ldflags = $(filter -L%,$(PKG_LIBS))
_pkg_ldlibs = $(filter-out -L%,$(PKG_LIBS))
$(libceeds) : CEED_LDFLAGS += $(_pkg_ldflags) $(if $(STATIC),,$(_pkg_ldflags:-L%=-Wl,-rpath,%)) $(PKG_STUBS_LIBS)
$(libceeds) : CEED_LDLIBS += $(_pkg_ldlibs)
ifeq ($(STATIC),1)
  $(examples) $(tests) : CEED_LDFLAGS += $(EM_LDFLAGS) $(_pkg_ldflags) $(if $(STATIC),,$(_pkg_ldflags:-L%=-Wl,-rpath,%)) $(PKG_STUBS_LIBS)
  $(examples) $(tests) : CEED_LDLIBS += $(_pkg_ldlibs)
endif

pkgconfig-libs-private = $(PKG_LIBS)
ifeq ($(LIBCEED_CONTAINS_CXX),1)
  $(libceeds) : LINK = $(CXX)
  ifeq ($(STATIC),1)
    $(examples) $(tests) : CEED_LDLIBS += $(LIBCXX)
    pkgconfig-libs-private += $(LIBCXX)
  endif
endif


# ------------------------------------------------------------
# Building core library components
# ------------------------------------------------------------

# File names *-weak.c contain weak symbol definitions, which must be listed last
# when creating shared or static libraries.
weak_last = $(filter-out %-weak.o,$(1)) $(filter %-weak.o,$(1))

libceed.o = $(libceed.c:%.c=$(OBJDIR)/%.o) $(libceed.cpp:%.cpp=$(OBJDIR)/%.o) $(libceed.cu:%.cu=$(OBJDIR)/%.o) $(libceed.hip:%.hip.cpp=$(OBJDIR)/%.o) $(libceed.sycl:%.sycl.cpp=$(OBJDIR)/%.o)
$(filter %fortran.o,$(libceed.o)) : CPPFLAGS += $(if $(filter 1,$(UNDERSCORE)),-DUNDERSCORE)
$(libceed.o): | info-backends
$(libceed.so) : $(call weak_last,$(libceed.o)) | $$(@D)/.DIR
	$(call quiet,LINK) $(LDFLAGS) $(CEED_LDFLAGS) -shared -o $@ $^ $(CEED_LDLIBS) $(LDLIBS)

$(libceed.a) : $(call weak_last,$(libceed.o)) | $$(@D)/.DIR
	$(call quiet,AR) $(ARFLAGS) $@ $^

$(OBJDIR)/%.o : $(CURDIR)/%.c | $$(@D)/.DIR
	$(call quiet,CC) $(CPPFLAGS) $(CFLAGS) $(CONFIGFLAGS) -c -o $@ $(abspath $<)

$(OBJDIR)/%.o : $(CURDIR)/%.cpp | $$(@D)/.DIR
	$(call quiet,CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $(abspath $<)

$(OBJDIR)/%.o : $(CURDIR)/%.cu | $$(@D)/.DIR
	$(call quiet,NVCC) $(filter-out -Wno-unused-function, $(CPPFLAGS)) $(NVCCFLAGS) -c -o $@ $(abspath $<)

$(OBJDIR)/%.o : $(CURDIR)/%.hip.cpp | $$(@D)/.DIR
	$(call quiet,HIPCC) $(CPPFLAGS) $(HIPCCFLAGS) -c -o $@ $(abspath $<)

$(OBJDIR)/%.o : $(CURDIR)/%.sycl.cpp | $$(@D)/.DIR
	$(call quiet,SYCLCXX) $(SYCLFLAGS) $(CPPFLAGS) -c -o $@ $(abspath $<)

$(OBJDIR)/%.o : $(CURDIR)/%.sycl.cpp | $$(@D)/.DIR
	$(call quiet,SYCLCXX) $(SYCLFLAGS) $(CPPFLAGS) -c -o $@ $(abspath $<)

$(OBJDIR)/%$(EXE_SUFFIX) : tests/%.c | $$(@D)/.DIR
	$(call quiet,LINK.c) $(CEED_LDFLAGS) -o $@ $(abspath $<) $(CEED_LIBS) $(CEED_LDLIBS) $(LDLIBS) -I./tests/test-include

$(OBJDIR)/%$(EXE_SUFFIX) : tests/%.f90 | $$(@D)/.DIR
	$(call quiet,LINK.F) -DSOURCE_DIR='"$(abspath $(<D))/"' $(CEED_LDFLAGS) -o $@ $(abspath $<) $(CEED_LIBS) $(CEED_LDLIBS) $(LDLIBS)

$(OBJDIR)/%$(EXE_SUFFIX) : examples/ceed/%.c | $$(@D)/.DIR
	$(call quiet,LINK.c) $(CEED_LDFLAGS) -o $@ $(abspath $<) $(CEED_LIBS) $(CEED_LDLIBS) $(LDLIBS)

$(OBJDIR)/%$(EXE_SUFFIX) : examples/ceed/%.f | $$(@D)/.DIR
	$(call quiet,LINK.F) -DSOURCE_DIR='"$(abspath $(<D))/"' $(CEED_LDFLAGS) -o $@ $(abspath $<) $(CEED_LIBS) $(CEED_LDLIBS) $(LDLIBS)


# ------------------------------------------------------------
# Building examples
# ------------------------------------------------------------

# deal.II
# Note: Invoking deal.II's CMAKE build system here
$(OBJDIR)/dealii-bps : examples/deal.II/*.cc examples/deal.II/*.h $(libceed) | $$(@D)/.DIR
	mkdir -p examples/deal.II/build
	cmake -B examples/deal.II/build -S examples/deal.II -DDEAL_II_DIR=$(DEAL_II_DIR) -DCEED_DIR=$(PWD)
	+$(call quiet,MAKE) -C examples/deal.II/build
	cp examples/deal.II/build/bps $(OBJDIR)/dealii-bps

# MFEM
$(OBJDIR)/mfem-% : examples/mfem/%.cpp $(libceed) | $$(@D)/.DIR
	+$(MAKE) -C examples/mfem CEED_DIR=`pwd` \
	  MFEM_DIR="$(abspath $(MFEM_DIR))" CXX=$(CXX) $*
	cp examples/mfem/$* $@

# Nek5000
# Note: Multiple Nek files cannot be built in parallel. The '+' here enables
#       this single Nek bps file to be built in parallel with other examples,
#       such as when calling `make prove-all -j2`.
$(OBJDIR)/nek-bps : examples/nek/bps/bps.usr examples/nek/nek-examples.sh $(libceed) | $$(@D)/.DIR
	+$(MAKE) -C examples MPI=$(MPI) CEED_DIR=`pwd` NEK5K_DIR="$(abspath $(NEK5K_DIR))" nek
	mv examples/nek/build/bps $(OBJDIR)/bps
	cp examples/nek/nek-examples.sh $(OBJDIR)/nek-bps

# Rust QFunctions
$(OBJDIR)/rustqfunctions-% : examples/rust-qfunctions/%.c $(libceed) | $$(@D)/.DIR
	+$(MAKE) -C examples/rust-qfunctions CEED_DIR=`pwd`
	cp examples/rust-qfunctions/$* $@

# PETSc
# Several executables have common utilities, but we can't build the utilities
# from separate submake invocations because they'll compete with each
# other/corrupt output. So we put it in this utility library, but we don't want
# to manually list source dependencies up at this level, so we'll just always
# call recursive make to check that this utility is up to date.
examples/petsc/libutils.a.PHONY: $(libceed) $(ceed.pc)
	+$(call quiet,MAKE) -C examples/petsc CEED_DIR=`pwd` AR=$(AR) ARFLAGS=$(ARFLAGS) \
	  PETSC_DIR="$(abspath $(PETSC_DIR))" OPT="$(OPT)" $(basename $(@F))

$(OBJDIR)/petsc-% : examples/petsc/%.c examples/petsc/libutils.a.PHONY $(libceed) $(ceed.pc) | $$(@D)/.DIR
	+$(call quiet,MAKE) -C examples/petsc CEED_DIR=`pwd` \
	  PETSC_DIR="$(abspath $(PETSC_DIR))" OPT="$(OPT)" $*
	cp examples/petsc/$* $@

# Fluid dynamics proxy application
$(OBJDIR)/fluids-% : examples/fluids/%.c examples/fluids/src/*.c examples/fluids/*.h examples/fluids/include/*.h examples/fluids/problems/*.c examples/fluids/qfunctions/*.h $(libceed) $(ceed.pc) examples/fluids/Makefile | $$(@D)/.DIR
	+$(call quiet,MAKE) -C examples/fluids CEED_DIR=`pwd` \
	  PETSC_DIR="$(abspath $(PETSC_DIR))" OPT="$(OPT)" $*
	cp examples/fluids/$* $@

# Solid mechanics proxy application
$(OBJDIR)/solids-% : examples/solids/%.c examples/solids/%.h \
    examples/solids/problems/*.c examples/solids/src/*.c \
    examples/solids/include/*.h examples/solids/problems/*.h examples/solids/qfunctions/*.h \
    $(libceed) $(ceed.pc) | $$(@D)/.DIR
	+$(call quiet,MAKE) -C examples/solids CEED_DIR=`pwd` \
	  PETSC_DIR="$(abspath $(PETSC_DIR))" OPT="$(OPT)" $*
	cp examples/solids/$* $@

examples      : $(allexamples)
ceedexamples  : $(examples)
nekexamples   : $(nekexamples)
mfemexamples  : $(mfemexamples)
petscexamples : $(petscexamples)

rustqfunctionsexamples : $(rustqfunctionsexamples)

external_examples := \
	$(if $(MFEM_DIR),$(mfemexamples)) \
	$(if $(PETSC_DIR),$(petscexamples)) \
	$(if $(NEK5K_DIR),$(nekexamples)) \
	$(if $(DEAL_II_DIR),$(dealiiexamples)) \
	$(if $(PETSC_DIR),$(fluidsexamples)) \
	$(if $(PETSC_DIR),$(solidsexamples)) \
	$(rustqfunctionsexamples)

allexamples = $(examples) $(external_examples)

$(examples) : $(libceed)
$(tests) : $(libceed)
$(tests) $(examples) : override LDFLAGS += $(if $(STATIC),,-Wl,-rpath,$(abspath $(LIBDIR))) -L$(LIBDIR)


# ------------------------------------------------------------
# Testing
# ------------------------------------------------------------

# Set number processes for testing
NPROC_TEST ?= 1
export NPROC_TEST

# Set pool size for testing
NPROC_POOL ?= 1
export NPROC_POOL

run-% : $(OBJDIR)/%
	@$(PYTHON) tests/junit.py --mode tap --ceed-backends $(BACKENDS) --nproc $(NPROC_TEST) --pool-size $(NPROC_POOL) --search '$(subsearch)' $(<:$(OBJDIR)/%=%)

# The test and prove targets can be controlled via pattern searches.  The
# default is to run tests and those examples that have no external dependencies.
# Examples of finer grained control:
#
#   make test search='petsc mfem'      # PETSc and MFEM examples
#   make prove search='t3'             # t3xx series tests
#   make junit search='ex petsc'       # core ex* and PETSc tests
search ?= t ex
realsearch = $(search:%=%%)
matched = $(foreach pattern,$(realsearch),$(filter $(OBJDIR)/$(pattern),$(tests) $(allexamples)))
subsearch ?= .*
JUNIT_BATCH ?= ''

# Test core libCEED
test : $(matched:$(OBJDIR)/%=run-%)

# Run test target in parallel
tst : ;@$(MAKE) $(MFLAGS) V=$(V) test
# CPU C tests only for backend %
ctc-% : $(ctests);@$(foreach tst,$(ctests),$(tst) /cpu/$*;)

# Testing with TAP format
# https://testanything.org/tap-specification.html
prove : $(matched)
	$(info Testing backends: $(BACKENDS))
	$(PROVE) $(PROVE_OPTS) --exec '$(PYTHON) tests/junit.py' $(matched:$(OBJDIR)/%=%) :: --mode tap --ceed-backends $(BACKENDS) --nproc $(NPROC_TEST) --pool-size $(NPROC_POOL) --search '$(subsearch)'
# Run prove target in parallel
prv : ;@$(MAKE) $(MFLAGS) V=$(V) prove

prove-all :
	+$(MAKE) prove realsearch=%

junit-% : $(OBJDIR)/%
	@printf "  %10s %s\n" TEST $(<:$(OBJDIR)/%=%); $(PYTHON) tests/junit.py --ceed-backends $(BACKENDS) --nproc $(NPROC_TEST) --pool-size $(NPROC_POOL) --search '$(subsearch)' --junit-batch $(JUNIT_BATCH) $(<:$(OBJDIR)/%=%)

junit : $(matched:$(OBJDIR)/%=junit-%)

all: $(alltests)

# Benchmarks
allbenchmarks = petsc-bps
bench_targets = $(addprefix bench-,$(allbenchmarks))
.PHONY: $(bench_targets) benchmarks
$(bench_targets): bench-%: $(OBJDIR)/%
	cd benchmarks && ./benchmark.sh --ceed "$(BACKENDS)" -r $(*).sh
benchmarks: $(bench_targets)

$(ceed.pc) : pkgconfig-prefix = $(abspath .)
$(OBJDIR)/ceed.pc : pkgconfig-prefix = $(prefix)
.INTERMEDIATE : $(OBJDIR)/ceed.pc
%/ceed.pc : ceed.pc.template | $$(@D)/.DIR
	@$(SED) \
	    -e "s:%prefix%:$(pkgconfig-prefix):" \
	    -e "s:%opt%:$(OPT):" \
	    -e "s:%libs_private%:$(pkgconfig-libs-private):" $< > $@

GIT_DESCRIBE = $(shell git -c safe.directory=$PWD describe --always --dirty 2>/dev/null || printf "unknown\n")

$(OBJDIR)/interface/ceed-config.o: Makefile
$(OBJDIR)/interface/ceed-config.o: CONFIGFLAGS += -DCEED_GIT_VERSION="\"$(GIT_DESCRIBE)\""
$(OBJDIR)/interface/ceed-config.o: CONFIGFLAGS += -DCEED_BUILD_CONFIGURATION="\"// Build Configuration:$(foreach v,$(CONFIG_VARS),\n$(v) = $($(v)))\""

$(OBJDIR)/interface/ceed-jit-source-root-default.o : CPPFLAGS += -DCEED_JIT_SOURCE_ROOT_DEFAULT="\"$(abspath ./include)/\""
$(OBJDIR)/interface/ceed-jit-source-root-install.o : CPPFLAGS += -DCEED_JIT_SOURCE_ROOT_DEFAULT="\"$(abspath $(includedir))/\""


# ------------------------------------------------------------
# Installation
# ------------------------------------------------------------

install : $(libceed) $(OBJDIR)/ceed.pc
	$(INSTALL) -d $(addprefix $(if $(DESTDIR),"$(DESTDIR)"),"$(includedir)"\
	  "$(includedir)/ceed/" "$(includedir)/ceed/jit-source/"\
	  "$(includedir)/ceed/jit-source/cuda/" "$(includedir)/ceed/jit-source/hip/"\
	  "$(includedir)/ceed/jit-source/gallery/" "$(includedir)/ceed/jit-source/magma/"\
	  "$(includedir)/ceed/jit-source/sycl/" "$(libdir)" "$(pkgconfigdir)")
	$(INSTALL_DATA) include/ceed/ceed.h "$(DESTDIR)$(includedir)/ceed/"
	$(INSTALL_DATA) include/ceed/types.h "$(DESTDIR)$(includedir)/ceed/"
	$(INSTALL_DATA) include/ceed/ceed-f32.h "$(DESTDIR)$(includedir)/ceed/"
	$(INSTALL_DATA) include/ceed/ceed-f64.h "$(DESTDIR)$(includedir)/ceed/"
	$(INSTALL_DATA) include/ceed/fortran.h "$(DESTDIR)$(includedir)/ceed/"
	$(INSTALL_DATA) include/ceed/backend.h "$(DESTDIR)$(includedir)/ceed/"
	$(INSTALL_DATA) include/ceed/cuda.h "$(DESTDIR)$(includedir)/ceed/"
	$(INSTALL_DATA) include/ceed/hip.h "$(DESTDIR)$(includedir)/ceed/"
	$(INSTALL_DATA) $(libceed) "$(DESTDIR)$(libdir)/"
	$(INSTALL_DATA) $(OBJDIR)/ceed.pc "$(DESTDIR)$(pkgconfigdir)/"
	$(INSTALL_DATA) include/ceed.h "$(DESTDIR)$(includedir)/"
	$(INSTALL_DATA) include/ceedf.h "$(DESTDIR)$(includedir)/"
	$(INSTALL_DATA) $(wildcard include/ceed/jit-source/cuda/*.h) "$(DESTDIR)$(includedir)/ceed/jit-source/cuda/"
	$(INSTALL_DATA) $(wildcard include/ceed/jit-source/hip/*.h) "$(DESTDIR)$(includedir)/ceed/jit-source/hip/"
	$(INSTALL_DATA) $(wildcard include/ceed/jit-source/gallery/*.h) "$(DESTDIR)$(includedir)/ceed/jit-source/gallery/"
	$(INSTALL_DATA) $(wildcard include/ceed/jit-source/magma/*.h) "$(DESTDIR)$(includedir)/ceed/jit-source/magma/"
	$(INSTALL_DATA) $(wildcard include/ceed/jit-source/sycl/*.h) "$(DESTDIR)$(includedir)/ceed/jit-source/sycl/"


# ------------------------------------------------------------
# Cleaning
# ------------------------------------------------------------

cln clean :
	$(RM) -r $(OBJDIR) $(LIBDIR) dist *egg* .pytest_cache *cffi*
	$(call quiet,MAKE) -C examples clean NEK5K_DIR="$(abspath $(NEK5K_DIR))"
	$(call quiet,MAKE) -C python/tests clean
	$(RM) benchmarks/*output.txt
	$(RM) -f temp_*

distclean : clean
	$(RM) -r doc/html doc/sphinx/build $(CONFIG)


# ------------------------------------------------------------
# Documentation
# ------------------------------------------------------------

DOXYGEN ?= doxygen

doxygen :
	$(DOXYGEN) -q Doxyfile

doc-html doc-latexpdf doc-epub doc-livehtml : doc-% : doxygen
	make -C doc/sphinx $*

doc : doc-html


# ------------------------------------------------------------
# Linting utilities
# ------------------------------------------------------------

# Style/Format
CLANG_FORMAT      ?= clang-format
CLANG_FORMAT_OPTS += -style=file -i
AUTOPEP8          ?= autopep8
AUTOPEP8_OPTS     += --in-place --aggressive --max-line-length 120

format.ch := $(filter-out include/ceedf.h $(wildcard tests/t*-f.h), $(shell git ls-files '*.[ch]pp' '*.[ch]' '*.cu'))
format.py := $(filter-out tests/junit-xml/junit_xml/__init__.py, $(shell git ls-files '*.py'))
format.ot := $(filter-out doc/sphinx/source/CODE_OF_CONDUCT.md doc/sphinx/source/CONTRIBUTING.md, $(shell git ls-files '*.md' '*.f90'))

format-c  :
	$(CLANG_FORMAT) $(CLANG_FORMAT_OPTS) $(format.ch)

format-py :
	$(AUTOPEP8) $(AUTOPEP8_OPTS) $(format.py)

format-ot:
	@$(SED) -r 's/\s+$$//' -i $(format.ot)

format    : format-c format-py format-ot

# Vermin - python version requirements
VERMIN            ?= vermin
VERMIN_OPTS       += -t=3.8- --violations

vermin    :
	$(VERMIN) $(VERMIN_OPTS) $(format.py)

# Tidy
CLANG_TIDY ?= clang-tidy

%.c.tidy : %.c
	$(CLANG_TIDY) $(TIDY_OPTS) $^ -- $(CPPFLAGS) --std=c11 -I$(CUDA_DIR)/include -I$(ROCM_DIR)/include -DCEED_JIT_SOURCE_ROOT_DEFAULT="\"$(abspath ./include)/\"" -DCEED_GIT_VERSION="\"$(GIT_DESCRIBE)\"" -DCEED_BUILD_CONFIGURATION="\"// Build Configuration:$(foreach v,$(CONFIG_VARS),\n$(v) = $($(v)))\""

%.cpp.tidy : %.cpp
	$(CLANG_TIDY) $(TIDY_OPTS) $^ -- $(CPPFLAGS) --std=c++11 -I$(CUDA_DIR)/include -I$(OCCA_DIR)/include -I$(ROCM_DIR)/include

tidy-c   : $(libceed.c:%=%.tidy)
tidy-cpp : $(libceed.cpp:%=%.tidy)

tidy : tidy-c tidy-cpp

# Include-What-You-Use
ifneq ($(wildcard ../iwyu/*),)
  IWYU_DIR ?= ../iwyu
  IWYU_CC  ?= $(IWYU_DIR)/build/bin/include-what-you-use
endif
iwyu :
	$(MAKE) -B CC=$(IWYU_CC)


# ------------------------------------------------------------
# Variable printing for debugging
# ------------------------------------------------------------

print :
	@echo $(VAR)=$($(VAR))

print-% :
	$(info [ variable name]: $*)
	$(info [        origin]: $(origin $*))
	$(info [        flavor]: $(flavor $*))
	$(info [         value]: $(value $*))
	$(info [expanded value]: $($*))
	$(info )
	@true


# ------------------------------------------------------------
# Configuration caching
# ------------------------------------------------------------

# "make configure" detects any variables passed on the command line or
# previously set in config.mk, caching them in config.mk as simple
# (:=) variables.  Variables set in config.mk or on the command line
# take precedence over the defaults provided in the file.  Typical
# usage:
#
#   make configure CC=/path/to/my/cc CUDA_DIR=/opt/cuda
#   make
#   make prove
#
# The values in the file can be updated by passing them on the command
# line, e.g.,
#
#   make configure CC=/path/to/other/clang

# All variables to consider for caching
CONFIG_VARS = CC CXX FC NVCC NVCC_CXX HIPCC \
  OPT CFLAGS CPPFLAGS CXXFLAGS FFLAGS NVCCFLAGS HIPCCFLAGS SYCLFLAGS \
  AR ARFLAGS LDFLAGS LDLIBS LIBCXX SED \
  MAGMA_DIR OCCA_DIR XSMM_DIR CUDA_DIR CUDA_ARCH MFEM_DIR PETSC_DIR NEK5K_DIR ROCM_DIR HIP_ARCH SYCL_DIR

# $(call needs_save,CFLAGS) returns true (a nonempty string) if CFLAGS
# was set on the command line or in config.mk (where it will appear as
# a simple variable).
needs_save = $(or $(filter command line,$(origin $(1))),$(filter simple,$(flavor $(1))))

configure :
	$(file > $(CONFIG))
	$(foreach v,$(CONFIG_VARS),$(if $(call needs_save,$(v)),$(file >> $(CONFIG),$(v) := $($(v)))))
	@echo "Configuration cached in $(CONFIG):"
	@cat $(CONFIG)


# ------------------------------------------------------------
# Building Python wheels for deployment
# ------------------------------------------------------------

wheel : export MARCHFLAG = -march=generic
wheel : export WHEEL_PLAT = manylinux2010_x86_64
wheel :
	docker run -it --user $(shell id -u):$(shell id -g) --rm -v $(PWD):/io -w /io \
	  -e MARCHFLAG -e WHEEL_PLAT \
	  quay.io/pypa/$(WHEEL_PLAT) python/make-wheels.sh

# ------------------------------------------------------------
# Phony targets
# ------------------------------------------------------------

# These targets are not files but rather commands to run
.PHONY : all cln clean doxygen doc format lib install par print test tst prove prv prove-all junit examples tidy iwyu info info-backends info-backends-all configure wheel


# Include *.d deps when not -B = --always-make: useful if the paths are wonky in a container
-include $(if $(filter B,$(MAKEFLAGS)),,$(libceed.c:%.c=$(OBJDIR)/%.d) $(tests.c:tests/%.c=$(OBJDIR)/%.d))
