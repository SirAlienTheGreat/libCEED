// Copyright (c) 2017-2025, Lawrence Livermore National Security, LLC and other CEED contributors.
// All Rights Reserved. See the top-level LICENSE and NOTICE files for details.
//
// SPDX-License-Identifier: BSD-2-Clause
//
// This file is part of CEED:  http://github.com/ceed
#pragma once

CEED_INTERN int CeedOperatorBuildKernel_Cuda_gen(CeedOperator op, bool *is_good_build);
CEED_INTERN int CeedOperatorBuildKernelFullAssemblyAtPoints_Cuda_gen(CeedOperator op, bool *is_good_build);
CEED_INTERN int CeedOperatorBuildKernelDiagonalAssemblyAtPoints_Cuda_gen(CeedOperator op, bool *is_good_build);
CEED_INTERN int CeedOperatorBuildKernelLinearAssembleQFunction_Cuda_gen(CeedOperator op, bool *is_good_build);
