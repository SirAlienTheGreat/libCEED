// Copyright (c) 2017-2025, Lawrence Livermore National Security, LLC and other CEED contributors.
// All Rights Reserved. See the top-level LICENSE and NOTICE files for details.
//
// SPDX-License-Identifier: BSD-2-Clause
//
// This file is part of CEED:  http://github.com/ceed

#define CEED_DEBUG_COLOR 12

#include <ceed.h>
#include <ceed/backend.h>
#include <ceed/gen-tools.h>
#include <ceed/jit-tools.h>

#include <iostream>
#include <sstream>
#include <string>

#include "../hip-ref/ceed-hip-ref.h"
#include "../hip-shared/ceed-hip-shared.h"
#include "../hip/ceed-hip-common.h"
#include "../hip/ceed-hip-compile.h"
#include "ceed-hip-gen.h"

struct FieldReuse_Hip {
  CeedInt      index;
  bool         is_input;
  CeedEvalMode eval_mode;
};

//------------------------------------------------------------------------------
// Calculate the block size used for launching the operator kernel
//------------------------------------------------------------------------------
extern "C" int BlockGridCalculate_Hip_gen(const CeedInt dim, const CeedInt num_elem, const CeedInt P_1d, const CeedInt Q_1d, CeedInt *block_sizes) {
  const CeedInt thread_1d = CeedIntMax(Q_1d, P_1d);
  if (dim == 1) {
    CeedInt elems_per_block = 64 * thread_1d > 256 ? 256 / thread_1d : 64;

    elems_per_block = elems_per_block > 0 ? elems_per_block : 1;
    block_sizes[0]  = thread_1d;
    block_sizes[1]  = 1;
    block_sizes[2]  = elems_per_block;
  } else if (dim == 2) {
    const CeedInt elems_per_block = thread_1d < 4 ? 16 : 2;

    block_sizes[0] = thread_1d;
    block_sizes[1] = thread_1d;
    block_sizes[2] = elems_per_block;
  } else if (dim == 3) {
    const CeedInt elems_per_block = thread_1d < 6 ? 4 : (thread_1d < 8 ? 2 : 1);

    block_sizes[0] = thread_1d;
    block_sizes[1] = thread_1d;
    block_sizes[2] = elems_per_block;
  }
  return CEED_ERROR_SUCCESS;
}

//------------------------------------------------------------------------------
// Determine type of operator
//------------------------------------------------------------------------------
static int CeedOperatorBuildKernelData_Hip_gen(Ceed ceed, CeedInt num_input_fields, CeedOperatorField *op_input_fields,
                                               CeedQFunctionField *qf_input_fields, CeedInt num_output_fields, CeedOperatorField *op_output_fields,
                                               CeedQFunctionField *qf_output_fields, CeedInt *max_P, CeedInt *max_P_1d, CeedInt *Q, CeedInt *Q_1d,
                                               CeedInt *max_dim, bool *is_all_tensor, bool *use_3d_slices) {
  // Check if all are tensor
  *is_all_tensor = true;
  for (CeedInt i = 0; i < num_input_fields; i++) {
    CeedBasis basis;

    CeedCallBackend(CeedOperatorFieldGetBasis(op_input_fields[i], &basis));
    if (basis != CEED_BASIS_NONE) {
      bool is_field_tensor;

      CeedCallBackend(CeedBasisIsTensor(basis, &is_field_tensor));
      *is_all_tensor = *is_all_tensor && is_field_tensor;
    }
    CeedCallBackend(CeedBasisDestroy(&basis));
  }
  for (CeedInt i = 0; i < num_output_fields; i++) {
    CeedBasis basis;

    CeedCallBackend(CeedOperatorFieldGetBasis(op_output_fields[i], &basis));
    if (basis != CEED_BASIS_NONE) {
      bool is_field_tensor;

      CeedCallBackend(CeedBasisIsTensor(basis, &is_field_tensor));
      *is_all_tensor = *is_all_tensor && is_field_tensor;
    }
    CeedCallBackend(CeedBasisDestroy(&basis));
  }

  // Find max_P, max_P_1d, Q, and Q_1d
  bool is_all_3d = true;

  *max_P    = 0;
  *max_P_1d = 0;
  *Q        = 0;
  *Q_1d     = 0;
  for (CeedInt i = 0; i < num_input_fields; i++) {
    CeedBasis basis;

    CeedCallBackend(CeedOperatorFieldGetBasis(op_input_fields[i], &basis));
    if (basis != CEED_BASIS_NONE) {
      bool    is_field_tensor;
      CeedInt field_dim = 0, field_P = 0, field_P_1d = 0, field_Q = 0, field_Q_1d = 0;

      // Check if 3D
      CeedCallBackend(CeedBasisGetDimension(basis, &field_dim));
      is_all_3d = is_all_3d && (field_dim == 3);
      *max_dim  = CeedIntMax(*max_dim, field_dim);

      // Collect P, P_1d, Q, and Q_1d
      CeedCallBackend(CeedBasisGetNumNodes(basis, &field_P));
      *max_P = CeedIntMax(*max_P, field_P);
      CeedCallBackend(CeedBasisIsTensor(basis, &is_field_tensor));
      if (is_field_tensor) {
        CeedCallBackend(CeedBasisGetNumNodes1D(basis, &field_P_1d));
        *max_P_1d = CeedIntMax(*max_P_1d, field_P_1d);
      }
      CeedCallBackend(CeedBasisGetNumQuadraturePoints(basis, &field_Q));
      CeedCheck(*Q == 0 || field_Q == *Q, ceed, CEED_ERROR_BACKEND, "Quadrature spaces must be compatible");
      *Q = field_Q;
      if (is_field_tensor) {
        CeedCallBackend(CeedBasisGetNumQuadraturePoints1D(basis, &field_Q_1d));
        CeedCheck(*Q_1d == 0 || field_Q_1d == *Q_1d, ceed, CEED_ERROR_BACKEND, "Quadrature spaces must be compatible");
        *Q_1d = field_Q_1d;
      }
    }
    CeedCallBackend(CeedBasisDestroy(&basis));
  }
  for (CeedInt i = 0; i < num_output_fields; i++) {
    CeedBasis basis;

    CeedCallBackend(CeedOperatorFieldGetBasis(op_output_fields[i], &basis));
    if (basis != CEED_BASIS_NONE) {
      bool    is_field_tensor;
      CeedInt field_dim = 0, field_P = 0, field_P_1d = 0, field_Q = 0, field_Q_1d = 0;

      // Check if 3D
      CeedCallBackend(CeedBasisGetDimension(basis, &field_dim));
      is_all_3d = is_all_3d && (field_dim == 3);
      *max_dim  = CeedIntMax(*max_dim, field_dim);

      // Collect P, P_1d, Q, and Q_1d
      CeedCallBackend(CeedBasisGetNumNodes(basis, &field_P));
      *max_P = CeedIntMax(*max_P, field_P);
      CeedCallBackend(CeedBasisIsTensor(basis, &is_field_tensor));
      if (is_field_tensor) {
        CeedCallBackend(CeedBasisGetNumNodes1D(basis, &field_P_1d));
        *max_P_1d = CeedIntMax(*max_P_1d, field_P_1d);
      }
      CeedCallBackend(CeedBasisGetNumQuadraturePoints(basis, &field_Q));
      CeedCheck(*Q == 0 || field_Q == *Q, ceed, CEED_ERROR_BACKEND, "Quadrature spaces must be compatible");
      *Q = field_Q;
      if (is_field_tensor) {
        CeedCallBackend(CeedBasisGetNumQuadraturePoints1D(basis, &field_Q_1d));
        CeedCheck(*Q_1d == 0 || field_Q_1d == *Q_1d, ceed, CEED_ERROR_BACKEND, "Quadrature spaces must be compatible");
        *Q_1d = field_Q_1d;
      }
    }
    CeedCallBackend(CeedBasisDestroy(&basis));
  }

  // Only use 3D collocated gradient parallelization strategy when gradient is computed
  *use_3d_slices = false;
  if (is_all_3d && *is_all_tensor) {
    bool was_grad_found = false;

    for (CeedInt i = 0; i < num_input_fields; i++) {
      CeedEvalMode eval_mode;

      CeedCallBackend(CeedQFunctionFieldGetEvalMode(qf_input_fields[i], &eval_mode));
      if (eval_mode == CEED_EVAL_GRAD) {
        CeedBasis_Hip_shared *basis_data;
        CeedBasis             basis;

        CeedCallBackend(CeedOperatorFieldGetBasis(op_input_fields[i], &basis));
        CeedCallBackend(CeedBasisGetData(basis, &basis_data));
        *use_3d_slices = basis_data->d_collo_grad_1d && (was_grad_found ? *use_3d_slices : true);
        was_grad_found = true;
        CeedCallBackend(CeedBasisDestroy(&basis));
      }
    }
    for (CeedInt i = 0; i < num_output_fields; i++) {
      CeedEvalMode eval_mode;

      CeedCallBackend(CeedQFunctionFieldGetEvalMode(qf_output_fields[i], &eval_mode));
      if (eval_mode == CEED_EVAL_GRAD) {
        CeedBasis_Hip_shared *basis_data;
        CeedBasis             basis;

        CeedCallBackend(CeedOperatorFieldGetBasis(op_output_fields[i], &basis));
        CeedCallBackend(CeedBasisGetData(basis, &basis_data));
        *use_3d_slices = basis_data->d_collo_grad_1d && (was_grad_found ? *use_3d_slices : true);
        was_grad_found = true;
        CeedCallBackend(CeedBasisDestroy(&basis));
      }
    }
  }
  return CEED_ERROR_SUCCESS;
}

//------------------------------------------------------------------------------
// Setup fields
//------------------------------------------------------------------------------
static int CeedOperatorBuildKernelFieldData_Hip_gen(std::ostringstream &code, CeedOperator_Hip_gen *data, Tab &tab, CeedInt i,
                                                    CeedOperatorField op_field, CeedQFunctionField qf_field, FieldReuse_Hip field_reuse,
                                                    CeedInt max_dim, CeedInt Q, CeedInt Q_1d, bool is_input, bool is_all_tensor, bool is_at_points,
                                                    bool use_3d_slices) {
  bool      is_tensor = true;
  CeedBasis basis;
  CeedCallBackend(CeedOperatorFieldGetBasis(op_field, &basis));
  if (basis != CEED_BASIS_NONE) CeedCallBackend(CeedBasisIsTensor(basis, &is_tensor));

  const char           *field_name;
  std::string           var_suffix = (is_input ? "_in_" : "_out_") + std::to_string(i);
  std::string           P_name = (is_tensor ? "P_1d" : "P") + var_suffix, Q_name = is_tensor ? "Q_1d" : "Q";
  std::string           option_name = (is_input ? "inputs" : "outputs");
  CeedEvalMode          eval_mode   = CEED_EVAL_NONE;
  CeedInt               elem_size = 0, num_comp = 0, dim = max_dim, P_1d = 0;
  CeedElemRestriction   elem_rstr;
  CeedBasis_Hip_shared *basis_data;

  // Field reuse info
  bool use_previous_field = field_reuse.index != -1;

  CeedCallBackend(CeedOperatorFieldGetName(op_field, &field_name));
  code << tab << "// -- " << (is_input ? "Input" : "Output") << " field " << i << ": " << field_name << "\n";

  // Get field data
  CeedCallBackend(CeedOperatorFieldGetElemRestriction(op_field, &elem_rstr));
  if (elem_rstr != CEED_ELEMRESTRICTION_NONE) {
    CeedCallBackend(CeedElemRestrictionGetElementSize(elem_rstr, &elem_size));
    CeedCallBackend(CeedElemRestrictionGetNumComponents(elem_rstr, &num_comp));
  }
  CeedCallBackend(CeedElemRestrictionDestroy(&elem_rstr));
  if (basis != CEED_BASIS_NONE) {
    CeedCallBackend(CeedBasisGetData(basis, &basis_data));
    CeedCallBackend(CeedBasisGetDimension(basis, &dim));
    if (is_tensor) CeedCallBackend(CeedBasisGetNumNodes1D(basis, &P_1d));
    else CeedCallBackend(CeedBasisGetNumNodes(basis, &P_1d));
  }
  CeedCallBackend(CeedQFunctionFieldGetEvalMode(qf_field, &eval_mode));

  // Set field constants
  code << tab << "const CeedInt dim" << var_suffix << " = " << dim << ";\n";
  if (is_tensor && !is_all_tensor) {
    CeedInt P = 0;

    CeedCallBackend(CeedBasisGetNumNodes(basis, &P));
    code << tab << "const CeedInt P" << var_suffix << " = " << (basis == CEED_BASIS_NONE ? Q : P) << ";\n";
  }
  code << tab << "const CeedInt " << P_name << " = " << (basis == CEED_BASIS_NONE ? Q_1d : P_1d) << ";\n";
  if (eval_mode != CEED_EVAL_WEIGHT) {
    code << tab << "const CeedInt num_comp" << var_suffix << " = " << num_comp << ";\n";
  }

  // Load basis data
  code << tab << "// EvalMode: " << CeedEvalModes[eval_mode] << "\n";
  switch (eval_mode) {
    case CEED_EVAL_NONE:
      break;
    case CEED_EVAL_INTERP:
      if (is_at_points) {
        // AtPoints
        if (!basis_data->d_chebyshev_interp_1d) {
          CeedSize    interp_bytes;
          CeedScalar *chebyshev_interp_1d;

          interp_bytes = P_1d * Q_1d * sizeof(CeedScalar);
          CeedCallBackend(CeedCalloc(P_1d * Q_1d, &chebyshev_interp_1d));
          CeedCallBackend(CeedBasisGetChebyshevInterp1D(basis, chebyshev_interp_1d));
          CeedCallHip(CeedBasisReturnCeed(basis), hipMalloc((void **)&basis_data->d_chebyshev_interp_1d, interp_bytes));
          CeedCallHip(CeedBasisReturnCeed(basis),
                      hipMemcpy(basis_data->d_chebyshev_interp_1d, chebyshev_interp_1d, interp_bytes, hipMemcpyHostToDevice));
          CeedCallBackend(CeedFree(&chebyshev_interp_1d));
        }
        if (is_input) data->B.inputs[i] = basis_data->d_chebyshev_interp_1d;
        else data->B.outputs[i] = basis_data->d_chebyshev_interp_1d;
      } else {
        // Standard quadrature
        if (is_input) data->B.inputs[i] = basis_data->d_interp_1d;
        else data->B.outputs[i] = basis_data->d_interp_1d;
      }
      if (use_previous_field) {
        std::string reuse_var = "s_B" + ((field_reuse.is_input ? "_in_" : "_out_") + std::to_string(field_reuse.index));

        code << tab << "CeedScalar *s_B" << var_suffix << " = " << reuse_var << ";\n";
      } else {
        code << tab << "__shared__ CeedScalar s_B" << var_suffix << "[" << P_name << "*" << Q_name << "];\n";
        code << tab << "LoadMatrix<" << P_name << ", " << Q_name << ">(data, B." << option_name << "[" << i << "], s_B" << var_suffix << ");\n";
      }
      break;
    case CEED_EVAL_GRAD:
      if (is_at_points) {
        // AtPoints
        if (!basis_data->d_chebyshev_interp_1d) {
          CeedSize    interp_bytes;
          CeedScalar *chebyshev_interp_1d;

          interp_bytes = P_1d * Q_1d * sizeof(CeedScalar);
          CeedCallBackend(CeedCalloc(P_1d * Q_1d, &chebyshev_interp_1d));
          CeedCallBackend(CeedBasisGetChebyshevInterp1D(basis, chebyshev_interp_1d));
          CeedCallHip(CeedBasisReturnCeed(basis), hipMalloc((void **)&basis_data->d_chebyshev_interp_1d, interp_bytes));
          CeedCallHip(CeedBasisReturnCeed(basis),
                      hipMemcpy(basis_data->d_chebyshev_interp_1d, chebyshev_interp_1d, interp_bytes, hipMemcpyHostToDevice));
          CeedCallBackend(CeedFree(&chebyshev_interp_1d));
        }
        if (is_input) data->B.inputs[i] = basis_data->d_chebyshev_interp_1d;
        else data->B.outputs[i] = basis_data->d_chebyshev_interp_1d;
      } else {
        // Standard quadrature
        if (is_input) data->B.inputs[i] = basis_data->d_interp_1d;
        else data->B.outputs[i] = basis_data->d_interp_1d;
      }
      if (is_tensor) {
        if (use_previous_field) {
          std::string reuse_var = "s_B" + ((field_reuse.is_input ? "_in_" : "_out_") + std::to_string(field_reuse.index));

          code << tab << "CeedScalar *s_B" << var_suffix << " = " << reuse_var << ";\n";
        } else {
          code << tab << "__shared__ CeedScalar s_B" << var_suffix << "[" << P_name << "*" << Q_name << "];\n";
          code << tab << "LoadMatrix<" << P_name << ", " << Q_name << ">(data, B." << option_name << "[" << i << "], s_B" << var_suffix << ");\n";
        }
      }
      if (is_at_points) break;  // No G mat for AtPoints
      if (use_3d_slices) {
        if (is_input) data->G.inputs[i] = basis_data->d_collo_grad_1d;
        else data->G.outputs[i] = basis_data->d_collo_grad_1d;
        if (use_previous_field && field_reuse.eval_mode == CEED_EVAL_GRAD) {
          std::string reuse_var = "s_G" + ((field_reuse.is_input ? "_in_" : "_out_") + std::to_string(field_reuse.index));

          code << tab << "CeedScalar *s_G" << var_suffix << " = " << reuse_var << ";\n";
        } else {
          code << tab << "__shared__ CeedScalar s_G" << var_suffix << "[" << Q_name << "*" << Q_name << "];\n";
          code << tab << "LoadMatrix<" << Q_name << ", " << Q_name << ">(data, G." << option_name << "[" << i << "], s_G" << var_suffix << ");\n";
        }
      } else {
        bool has_collo_grad = basis_data->d_collo_grad_1d;

        if (is_input) data->G.inputs[i] = has_collo_grad ? basis_data->d_collo_grad_1d : basis_data->d_grad_1d;
        else data->G.outputs[i] = has_collo_grad ? basis_data->d_collo_grad_1d : basis_data->d_grad_1d;
        if (has_collo_grad) {
          if (use_previous_field && field_reuse.eval_mode == CEED_EVAL_GRAD) {
            std::string reuse_var = "s_G" + ((field_reuse.is_input ? "_in_" : "_out_") + std::to_string(field_reuse.index));

            code << tab << "CeedScalar *s_G" << var_suffix << " = " << reuse_var << ";\n";
          } else {
            code << tab << "__shared__ CeedScalar s_G" << var_suffix << "[" << Q_name << "*" << Q_name << "];\n";
            code << tab << "LoadMatrix<" << Q_name << ", " << Q_name << ">(data, G." << option_name << "[" << i << "], s_G" << var_suffix << ");\n";
          }
        } else {
          if (use_previous_field && field_reuse.eval_mode == CEED_EVAL_GRAD) {
            std::string reuse_var = "s_G" + ((field_reuse.is_input ? "_in_" : "_out_") + std::to_string(field_reuse.index));

            code << tab << "CeedScalar *s_G" << var_suffix << " = " << reuse_var << ";\n";
          } else {
            code << tab << "__shared__ CeedScalar s_G" << var_suffix << "[" << P_name << "*" << Q_name << (is_tensor ? "" : "*dim")
                 << (is_tensor ? "" : var_suffix) << "];\n";
            code << tab << "LoadMatrix<" << P_name << ", " << Q_name << (is_tensor ? "" : "*dim") << (is_tensor ? "" : var_suffix) << ">(data, G."
                 << option_name << "[" << i << "], s_G" << var_suffix << ");\n";
          }
        }
      }
      break;
    case CEED_EVAL_WEIGHT:
      break;  // No action
      // LCOV_EXCL_START
    case CEED_EVAL_DIV:
    case CEED_EVAL_CURL:
      break;  // TODO: Not implemented
              // LCOV_EXCL_STOP
  }
  CeedCallBackend(CeedBasisDestroy(&basis));
  return CEED_ERROR_SUCCESS;
}

//------------------------------------------------------------------------------
// Restriction
//------------------------------------------------------------------------------
static int CeedOperatorBuildKernelRestriction_Hip_gen(std::ostringstream &code, CeedOperator_Hip_gen *data, Tab &tab, CeedInt i,
                                                      CeedInt field_input_buffer[], CeedOperatorField op_field, CeedQFunctionField qf_field,
                                                      CeedInt max_dim, CeedInt Q_1d, bool is_input, bool is_all_tensor, bool is_at_points,
                                                      bool use_3d_slices) {
  std::string              var_suffix = (is_input ? "_in_" : "_out_") + std::to_string(i);
  std::string              P_name     = (is_all_tensor ? "P_1d" : "P") + var_suffix;
  CeedEvalMode             eval_mode  = CEED_EVAL_NONE;
  CeedInt                  elem_size = 0, num_comp = 0;
  CeedSize                 l_size;
  CeedRestrictionType      rstr_type = CEED_RESTRICTION_STANDARD;
  CeedElemRestriction_Hip *rstr_data;
  CeedElemRestriction      elem_rstr;

  // Get field data
  CeedCallBackend(CeedOperatorFieldGetElemRestriction(op_field, &elem_rstr));
  if (elem_rstr != CEED_ELEMRESTRICTION_NONE) {
    CeedCallBackend(CeedElemRestrictionGetType(elem_rstr, &rstr_type));
    CeedCallBackend(CeedElemRestrictionGetElementSize(elem_rstr, &elem_size));
    CeedCallBackend(CeedElemRestrictionGetNumComponents(elem_rstr, &num_comp));
    CeedCallBackend(CeedElemRestrictionGetData(elem_rstr, &rstr_data));
  }
  CeedCallBackend(CeedQFunctionFieldGetEvalMode(qf_field, &eval_mode));

  // Restriction
  if (is_input) {
    // Input
    if (field_input_buffer[i] != i) {
      std::string buffer_name = "r_e_in_" + std::to_string(field_input_buffer[i]);

      // Restriction was already done for previous input
      code << tab << "CeedScalar *r_e" << var_suffix << " = " << buffer_name << ";\n";
    } else if (eval_mode != CEED_EVAL_WEIGHT && !((eval_mode == CEED_EVAL_NONE) && use_3d_slices && is_at_points)) {
      if (eval_mode == CEED_EVAL_NONE && rstr_type != CEED_RESTRICTION_POINTS) {
        // No basis action, so r_e_in_* in also r_q_in_* and needs to be allocated
        code << tab << "CeedScalar r_e" << var_suffix << "[num_comp" << var_suffix << "*" << P_name << "];\n";
      } else if (rstr_type != CEED_RESTRICTION_POINTS) {
        // Otherwise we're using the scratch space
        code << tab << "CeedScalar *r_e" << var_suffix << " = r_e_scratch;\n";
      }
      switch (rstr_type) {
        case CEED_RESTRICTION_STANDARD: {
          CeedInt comp_stride;

          CeedCallBackend(CeedElemRestrictionGetLVectorSize(elem_rstr, &l_size));
          code << tab << "const CeedInt l_size" << var_suffix << " = " << l_size << ";\n";
          CeedCallBackend(CeedElemRestrictionGetCompStride(elem_rstr, &comp_stride));
          code << tab << "const CeedInt comp_stride" << var_suffix << " = " << comp_stride << ";\n";
          data->indices.inputs[i] = (CeedInt *)rstr_data->d_offsets;
          code << tab << "ReadLVecStandard" << (is_all_tensor ? max_dim : 1) << "d<num_comp" << var_suffix << ", comp_stride" << var_suffix << ", "
               << P_name << ">(data, l_size" << var_suffix << ", elem, indices.inputs[" << i << "], d" << var_suffix << ", r_e" << var_suffix
               << ");\n";
          break;
        }
        case CEED_RESTRICTION_STRIDED: {
          bool    has_backend_strides;
          CeedInt num_elem;

          CeedCallBackend(CeedElemRestrictionHasBackendStrides(elem_rstr, &has_backend_strides));
          CeedCallBackend(CeedElemRestrictionGetNumElements(elem_rstr, &num_elem));
          CeedInt strides[3] = {1, elem_size * num_elem, elem_size};

          if (!has_backend_strides) {
            CeedCallBackend(CeedElemRestrictionGetStrides(elem_rstr, strides));
          }
          code << tab << "const CeedInt strides" << var_suffix << "_0 = " << strides[0] << ", strides" << var_suffix << "_1 = " << strides[1]
               << ", strides" << var_suffix << "_2 = " << strides[2] << ";\n";
          code << tab << "ReadLVecStrided" << (is_all_tensor ? max_dim : 1) << "d<num_comp" << var_suffix << ", " << P_name << ", strides"
               << var_suffix << "_0, strides" << var_suffix << "_1, strides" << var_suffix << "_2>(data, elem, d" << var_suffix << ", r_e"
               << var_suffix << ");\n";
          break;
        }
        case CEED_RESTRICTION_POINTS: {
          CeedInt comp_stride;

          CeedCallBackend(CeedElemRestrictionGetCompStride(elem_rstr, &comp_stride));
          code << tab << "const CeedInt comp_stride" << var_suffix << " = " << comp_stride << ";\n";
          data->indices.inputs[i] = (CeedInt *)rstr_data->d_offsets;
          break;
        }
        // LCOV_EXCL_START
        case CEED_RESTRICTION_ORIENTED:
        case CEED_RESTRICTION_CURL_ORIENTED:
          break;  // TODO: Not implemented
                  // LCOV_EXCL_STOP
      }
    }
  } else {
    // Output
    switch (rstr_type) {
      case CEED_RESTRICTION_STANDARD: {
        CeedInt comp_stride;

        CeedCallBackend(CeedElemRestrictionGetLVectorSize(elem_rstr, &l_size));
        code << tab << "const CeedInt l_size" << var_suffix << " = " << l_size << ";\n";
        CeedCallBackend(CeedElemRestrictionGetCompStride(elem_rstr, &comp_stride));
        code << tab << "const CeedInt comp_stride" << var_suffix << " = " << comp_stride << ";\n";
        data->indices.outputs[i] = (CeedInt *)rstr_data->d_offsets;
        code << tab << "WriteLVecStandard" << (is_all_tensor ? max_dim : 1) << "d<num_comp" << var_suffix << ", comp_stride" << var_suffix << ", "
             << P_name << ">(data, l_size" << var_suffix << ", elem, indices.outputs[" << i << "], r_e" << var_suffix << ", d" << var_suffix
             << ");\n";
        break;
      }
      case CEED_RESTRICTION_STRIDED: {
        bool    has_backend_strides;
        CeedInt num_elem;

        CeedCallBackend(CeedElemRestrictionHasBackendStrides(elem_rstr, &has_backend_strides));
        CeedCallBackend(CeedElemRestrictionGetNumElements(elem_rstr, &num_elem));
        CeedInt strides[3] = {1, elem_size * num_elem, elem_size};

        if (!has_backend_strides) {
          CeedCallBackend(CeedElemRestrictionGetStrides(elem_rstr, strides));
        }
        code << tab << "const CeedInt strides" << var_suffix << "_0 = " << strides[0] << ", strides" << var_suffix << "_1 = " << strides[1]
             << ", strides" << var_suffix << "_2 = " << strides[2] << ";\n";
        code << tab << "WriteLVecStrided" << (is_all_tensor ? max_dim : 1) << "d<num_comp" << var_suffix << ", " << P_name << ", strides"
             << var_suffix << "_0, strides" << var_suffix << "_1, strides" << var_suffix << "_2>(data, elem, r_e" << var_suffix << ", d" << var_suffix
             << ");\n";
        break;
      }
      case CEED_RESTRICTION_POINTS:
        data->indices.outputs[i] = (CeedInt *)rstr_data->d_offsets;
        break;
      // LCOV_EXCL_START
      case CEED_RESTRICTION_ORIENTED:
      case CEED_RESTRICTION_CURL_ORIENTED:
        break;  // TODO: Not implemented
                // LCOV_EXCL_STOP
    }
  }
  CeedCallBackend(CeedElemRestrictionDestroy(&elem_rstr));
  return CEED_ERROR_SUCCESS;
}

//------------------------------------------------------------------------------
// Basis
//------------------------------------------------------------------------------
static int CeedOperatorBuildKernelBasis_Hip_gen(std::ostringstream &code, CeedOperator_Hip_gen *data, Tab &tab, CeedInt i, CeedOperatorField op_field,
                                                CeedQFunctionField qf_field, CeedInt max_dim, CeedInt Q_1d, bool is_input, bool is_all_tensor,
                                                bool is_at_points, bool use_3d_slices) {
  bool      is_tensor = true;
  CeedBasis basis;
  CeedCallBackend(CeedOperatorFieldGetBasis(op_field, &basis));
  CeedCallBackend(CeedBasisIsTensor(basis, &is_tensor));

  std::string         var_suffix = (is_input ? "_in_" : "_out_") + std::to_string(i);
  std::string         P_name = (is_tensor ? "P_1d" : "P") + var_suffix, Q_name = is_tensor ? "Q_1d" : "Q";
  CeedEvalMode        eval_mode = CEED_EVAL_NONE;
  CeedInt             dim = max_dim, elem_size = 0, num_comp = 0, P_1d = 0;
  CeedElemRestriction elem_rstr;

  // Get field data
  CeedCallBackend(CeedOperatorFieldGetElemRestriction(op_field, &elem_rstr));
  if (elem_rstr != CEED_ELEMRESTRICTION_NONE) {
    CeedCallBackend(CeedElemRestrictionGetElementSize(elem_rstr, &elem_size));
    CeedCallBackend(CeedElemRestrictionGetNumComponents(elem_rstr, &num_comp));
  }
  CeedCallBackend(CeedElemRestrictionDestroy(&elem_rstr));
  if (basis != CEED_BASIS_NONE) {
    CeedCallBackend(CeedBasisGetDimension(basis, &dim));
    if (is_tensor) CeedCallBackend(CeedBasisGetNumNodes1D(basis, &P_1d));
    else CeedCallBackend(CeedBasisGetNumNodes(basis, &P_1d));
  }
  CeedCallBackend(CeedQFunctionFieldGetEvalMode(qf_field, &eval_mode));

  // Basis
  code << tab << "// EvalMode: " << CeedEvalModes[eval_mode] << "\n";
  if (is_input) {
    switch (eval_mode) {
      case CEED_EVAL_NONE:
        if (!use_3d_slices && !is_at_points) {
          code << tab << "CeedScalar *r_q" << var_suffix << " = r_e" << var_suffix << ";\n";
        }
        break;
      case CEED_EVAL_INTERP:
        if (is_at_points) {
          std::string function_name = (dim == 1 ? "Interp" : "InterpTensor") + std::to_string(dim) + "d";

          code << tab << "CeedScalar r_c" << var_suffix << "[num_comp" << var_suffix << "*" << (dim >= 3 ? Q_name : "1") << "];\n";
          code << tab << function_name << "<num_comp" << var_suffix << ", " << P_name << ", " << Q_name << ", OP_T_1D>(data, r_e" << var_suffix
               << ", s_B" << var_suffix << ", r_c" << var_suffix << ");\n";
        } else {
          std::string function_name = is_tensor
                                          ? ((dim == 1 ? "Interp" : "InterpTensor") + std::to_string(dim) + "d" + (is_all_tensor ? "" : "Flattened"))
                                          : "InterpNonTensor";
          std::string op_t_1d_name  = (is_all_tensor || !is_tensor) ? "OP_T_1D" : (P_1d > Q_1d ? P_name : Q_name);

          code << tab << "CeedScalar r_q" << var_suffix << "[num_comp" << var_suffix << "*" << (is_all_tensor && (dim >= 3) ? Q_name : "1") << "];\n";
          code << tab << function_name << "<num_comp" << var_suffix << ", " << P_name << ", " << Q_name << ", " << op_t_1d_name << ">(data, r_e"
               << var_suffix << ", s_B" << var_suffix << ", r_q" << var_suffix << ");\n";
        }
        break;
      case CEED_EVAL_GRAD:
        if (is_at_points) {
          std::string function_name = (dim == 1 ? "Interp" : "InterpTensor") + std::to_string(dim) + "d";

          code << tab << "CeedScalar r_c" << var_suffix << "[num_comp" << var_suffix << "*" << (dim >= 3 ? Q_name : "1") << "];\n";
          code << tab << function_name << "<num_comp" << var_suffix << ", " << P_name << ", " << Q_name << ", OP_T_1D>(data, r_e" << var_suffix
               << ", s_B" << var_suffix << ", r_c" << var_suffix << ");\n";
        } else if (use_3d_slices) {
          std::string function_name = (dim > 1 ? "InterpTensor" : "Interp") + std::to_string(dim) + "d";

          code << tab << "CeedScalar r_q" << var_suffix << "[num_comp" << var_suffix << "*" << Q_name << "];\n";
          code << tab << function_name << "<num_comp" << var_suffix << ", " << P_name << ", " << Q_name << ", OP_T_1D>(data, r_e" << var_suffix
               << ", s_B" << var_suffix << ", r_q" << var_suffix << ");\n";
        } else if (is_tensor) {
          bool        is_collocated = dim == 3 && Q_1d >= P_1d;
          std::string function_name = (dim == 1 ? "Grad" : (is_collocated ? "GradTensorCollocated" : "GradTensor")) + std::to_string(dim) + "d" +
                                      (is_all_tensor ? "" : "Flattened");
          std::string op_t_1d_name = is_all_tensor ? "OP_T_1D" : (P_1d > Q_1d ? P_name : Q_name);

          code << tab << "CeedScalar r_q" << var_suffix << "[num_comp" << var_suffix << "*dim" << var_suffix << "*"
               << (is_all_tensor && dim >= 3 ? Q_name : "1") << "];\n";
          code << tab << function_name << "<num_comp" << var_suffix << ", " << P_name << ", " << Q_name << ", " << op_t_1d_name << ">(data, r_e"
               << var_suffix << ", s_B" << var_suffix << ", s_G" << var_suffix << ", r_q" << var_suffix << ");\n";
        } else {
          std::string function_name = "GradNonTensor";

          code << tab << "CeedScalar r_q" << var_suffix << "[num_comp" << var_suffix << "*dim" << var_suffix << "];\n";
          code << tab << function_name << "<num_comp" << var_suffix << ", dim" << var_suffix << ", " << P_name << ", " << Q_name
               << ", OP_T_1D>(data, r_e" << var_suffix << ", s_G" << var_suffix << ", r_q" << var_suffix << ");\n";
        }
        break;
      case CEED_EVAL_WEIGHT: {
        if (is_at_points) {
          code << tab << "// Nothing to do AtPoints\n";
        } else {
          CeedBasis_Hip_shared *basis_data;
          std::string           function_name = is_tensor
                                                    ? ((dim == 1 ? "Weight" : "WeightTensor") + std::to_string(dim) + "d" + (is_all_tensor ? "" : "Flattened"))
                                                    : "WeightNonTensor";

          code << tab << "CeedScalar r_q" << var_suffix << "[" << (is_all_tensor && (dim >= 3) ? Q_name : "1") << "];\n";
          CeedCallBackend(CeedBasisGetData(basis, &basis_data));
          data->W = basis_data->d_q_weight_1d;
          code << tab << function_name << "<" << P_name << ", " << Q_name << ">(data, W, r_q" << var_suffix << ");\n";
        }
        break;
      }
      // LCOV_EXCL_START
      case CEED_EVAL_DIV:
      case CEED_EVAL_CURL:
        break;  // TODO: Not implemented
                // LCOV_EXCL_STOP
    }
  } else {
    switch (eval_mode) {
      case CEED_EVAL_NONE:
        code << tab << "CeedScalar *r_e" << var_suffix << " = r_q" << var_suffix << ";\n";
        break;  // No action
      case CEED_EVAL_INTERP:
        code << tab << "CeedScalar *r_e" << var_suffix << " = r_e_scratch;\n";
        if (is_at_points) {
          std::string function_name = (dim == 1 ? "InterpTranspose" : "InterpTransposeTensor") + std::to_string(dim) + "d";

          code << tab << function_name << "<num_comp" << var_suffix << ", " << P_name << ", " << Q_name << ", OP_T_1D>(data, r_c" << var_suffix
               << ", s_B" << var_suffix << ", r_e" << var_suffix << ");\n";
        } else {
          std::string function_name =
              is_tensor ? ((dim == 1 ? "InterpTranspose" : "InterpTransposeTensor") + std::to_string(dim) + "d" + (is_all_tensor ? "" : "Flattened"))
                        : "InterpTransposeNonTensor";
          std::string op_t_1d_name = (is_all_tensor || !is_tensor) ? "OP_T_1D" : (P_1d > Q_1d ? P_name : Q_name);

          code << tab << function_name << "<num_comp" << var_suffix << ", " << P_name << ", " << Q_name << ", " << op_t_1d_name << ">(data, r_q"
               << var_suffix << ", s_B" << var_suffix << ", r_e" << var_suffix << ");\n";
        }
        break;
      case CEED_EVAL_GRAD:
        code << tab << "CeedScalar *r_e" << var_suffix << " = r_e_scratch;\n";
        if (is_at_points) {
          std::string function_name = (dim == 1 ? "InterpTranspose" : "InterpTransposeTensor") + std::to_string(dim) + "d";

          code << tab << function_name << "<num_comp" << var_suffix << ", " << P_name << ", " << Q_name << ", OP_T_1D>(data, r_c" << var_suffix
               << ", s_B" << var_suffix << ", r_e" << var_suffix << ");\n";
        } else if (use_3d_slices) {
          std::string function_name = (dim == 1 ? "InterpTranspose" : "InterpTransposeTensor") + std::to_string(dim) + "d";

          code << tab << function_name << "<num_comp" << var_suffix << ", " << P_name << ", " << Q_name << ", OP_T_1D>(data, r_q" << var_suffix
               << ", s_B" << var_suffix << ", r_e" << var_suffix << ");\n";
        } else if (is_tensor) {
          bool        is_collocated = dim == 3 && Q_1d >= P_1d;
          std::string function_name = (dim == 1 ? "GradTranspose" : (is_collocated ? "GradTransposeTensorCollocated" : "GradTransposeTensor")) +
                                      std::to_string(dim) + "d" + (is_all_tensor ? "" : "Flattened");
          std::string op_t_1d_name = is_all_tensor ? "OP_T_1D" : (P_1d > Q_1d ? P_name : Q_name);

          code << tab << function_name << "<num_comp" << var_suffix << ", " << P_name << ", " << Q_name << ", " << op_t_1d_name << ">(data, r_q"
               << var_suffix << ", s_B" << var_suffix << ", s_G" << var_suffix << ", r_e" << var_suffix << ");\n";
        } else {
          std::string function_name = "GradTransposeNonTensor";

          code << tab << function_name << "<num_comp" << var_suffix << ", dim" << var_suffix << ", " << P_name << ", " << Q_name
               << ", OP_T_1D>(data, r_q" << var_suffix << ", s_G" << var_suffix << ", r_e" << var_suffix << ");\n";
        }
        break;
      // LCOV_EXCL_START
      case CEED_EVAL_WEIGHT:
        break;  // Should not occur
      case CEED_EVAL_DIV:
      case CEED_EVAL_CURL:
        break;  // TODO: Not implemented
                // LCOV_EXCL_STOP
    }
  }
  CeedCallBackend(CeedBasisDestroy(&basis));
  return CEED_ERROR_SUCCESS;
}

//------------------------------------------------------------------------------
// QFunction
//------------------------------------------------------------------------------
static int CeedOperatorBuildKernelQFunction_Hip_gen(std::ostringstream &code, CeedOperator_Hip_gen *data, Tab &tab, CeedInt max_dim,
                                                    CeedInt max_num_points, CeedInt num_input_fields, CeedOperatorField *op_input_fields,
                                                    CeedQFunctionField *qf_input_fields, CeedInt num_output_fields,
                                                    CeedOperatorField *op_output_fields, CeedQFunctionField *qf_output_fields,
                                                    std::string qfunction_name, CeedInt Q_1d, bool is_all_tensor, bool is_at_points,
                                                    bool use_3d_slices) {
  std::string         Q_name    = is_all_tensor ? "Q_1d" : "Q";
  CeedEvalMode        eval_mode = CEED_EVAL_NONE;
  CeedElemRestriction elem_rstr;

  // Setup output arrays
  code << "\n";
  code << tab << "// -- Output field setup\n";
  for (CeedInt i = 0; i < num_output_fields; i++) {
    const char *field_name;
    std::string var_suffix = "_out_" + std::to_string(i);

    CeedCallBackend(CeedOperatorFieldGetName(op_output_fields[i], &field_name));
    code << tab << "// ---- Output field " << i << ": " << field_name << "\n";
    CeedCallBackend(CeedQFunctionFieldGetEvalMode(qf_output_fields[i], &eval_mode));
    switch (eval_mode) {
      case CEED_EVAL_NONE:
        if (is_at_points) {
          code << tab << "CeedScalar r_q" << var_suffix << "[num_comp" << var_suffix << "];\n";
        } else {
          code << tab << "CeedScalar r_q" << var_suffix << "[num_comp" << var_suffix << "*" << (is_all_tensor && (max_dim >= 3) ? Q_name : "1")
               << "];\n";
        }
        break;
      case CEED_EVAL_INTERP:
        if (is_at_points) {
          // Accumulator for point data
          code << tab << "CeedScalar r_c" << var_suffix << "[num_comp" << var_suffix << "*" << (max_dim >= 3 ? Q_name : "1") << "];\n";
          code << tab << "for (CeedInt i = 0; i < num_comp" << var_suffix << "*" << (max_dim >= 3 ? Q_name : "1") << "; i++) r_c" << var_suffix
               << "[i] = 0.0;\n";
        } else {
          code << tab << "CeedScalar r_q" << var_suffix << "[num_comp" << var_suffix << "*" << (is_all_tensor && (max_dim >= 3) ? Q_name : "1")
               << "];\n";
        }
        break;
      case CEED_EVAL_GRAD:
        if (is_at_points) {
          // Accumulator for point data
          code << tab << "CeedScalar r_c" << var_suffix << "[num_comp" << var_suffix << "*" << (max_dim >= 3 ? Q_name : "1") << "];\n";
          code << tab << "for (CeedInt i = 0; i < num_comp" << var_suffix << "*" << (max_dim >= 3 ? Q_name : "1") << "; i++) r_c" << var_suffix
               << "[i] = 0.0;\n";
        } else if (use_3d_slices) {
          // Accumulator for gradient slices
          code << tab << "CeedScalar r_q" << var_suffix << "[num_comp" << var_suffix << "*" << Q_name << "];\n";
          code << tab << "for (CeedInt i = 0; i < num_comp" << var_suffix << "*" << Q_name << "; i++) r_q" << var_suffix << "[i] = 0.0;\n";
        } else {
          code << tab << "CeedScalar r_q" << var_suffix << "[num_comp" << var_suffix << "*dim" << var_suffix << "*"
               << (is_all_tensor && (max_dim >= 3) ? Q_name : "1") << "];\n";
        }
        break;
      case CEED_EVAL_WEIGHT:
        break;
        // LCOV_EXCL_START
      case CEED_EVAL_DIV:
      case CEED_EVAL_CURL:
        break;  // TODO: Not implemented
                // LCOV_EXCL_STOP
    }
  }

  if (is_at_points) {
    // We need to handle batches of points
    code << "\n";
    code << tab << "// Note: Using batches of points\n";
    code << tab << "const CeedInt point_loop_bound = (blockDim.x*blockDim.y) * ceil((1.0*max_num_points) / (blockDim.x*blockDim.y));\n\n";
    code << tab << "#pragma unroll\n";
    code << tab << "for (CeedInt i = threadIdx.x + threadIdx.y*blockDim.x; i < point_loop_bound; i += blockDim.x*blockDim.y) {\n";
    tab.push();
    code << tab << "const CeedInt p = i % max_num_points;\n\n";

    code << tab << "// -- Coordinates\n";
    code << tab << "CeedScalar r_x[max_dim];\n";
    code << tab << "ReadPoint<max_dim, coords_comp_stride, max_num_points>(data, elem, p, max_num_points, points.indices, points.coords, r_x);\n\n";

    code << tab << "// -- Input fields\n";
    for (CeedInt i = 0; i < num_input_fields; i++) {
      const char *field_name;
      std::string var_suffix = "_in_" + std::to_string(i);
      std::string P_name     = "P_1d" + var_suffix;

      CeedCallBackend(CeedOperatorFieldGetName(op_input_fields[i], &field_name));
      code << tab << "// ---- Input field " << i << ": " << field_name << "\n";
      CeedCallBackend(CeedQFunctionFieldGetEvalMode(qf_input_fields[i], &eval_mode));
      // Basis action
      code << tab << "// EvalMode: " << CeedEvalModes[eval_mode] << "\n";
      switch (eval_mode) {
        case CEED_EVAL_NONE:
          code << tab << "CeedScalar r_s" << var_suffix << "[num_comp" << var_suffix << "];\n";
          code << tab << "ReadPoint<num_comp" << var_suffix << ", comp_stride" << var_suffix
               << ", max_num_points>(data, elem, p, max_num_points, indices.inputs[" << i << "], d" << var_suffix << ", r_s" << var_suffix << ");\n";
          break;
        case CEED_EVAL_INTERP:
          code << tab << "CeedScalar r_s" << var_suffix << "[num_comp" << var_suffix << "];\n";
          code << tab << "InterpAtPoints" << max_dim << "d<num_comp" << var_suffix << ", max_num_points, " << P_name << ", " << Q_name
               << ">(data, i, r_c" << var_suffix << ", r_x, r_s" << var_suffix << ");\n";
          break;
        case CEED_EVAL_GRAD:
          code << tab << "CeedScalar r_s" << var_suffix << "[num_comp" << var_suffix << "*dim" << var_suffix << "];\n";
          code << tab << "GradAtPoints" << max_dim << "d<num_comp" << var_suffix << ", max_num_points, " << P_name << ", " << Q_name
               << ">(data, i, r_c" << var_suffix << ", r_x, r_s" << var_suffix << ");\n";
          break;
        case CEED_EVAL_WEIGHT:
          code << tab << "CeedScalar r_s" << var_suffix << "[1];\n";
          code << tab << "r_s" << var_suffix << "[0] = 1.0;\n";
          break;
          // LCOV_EXCL_START
        case CEED_EVAL_DIV:
        case CEED_EVAL_CURL:
          break;  // TODO: Not implemented
                  // LCOV_EXCL_STOP
      }
    }
    code << "\n";
    code << tab << "// -- Output fields\n";
    for (CeedInt i = 0; i < num_output_fields; i++) {
      const char *field_name;
      std::string var_suffix = "_out_" + std::to_string(i);

      CeedCallBackend(CeedOperatorFieldGetName(op_output_fields[i], &field_name));
      code << tab << "// ---- Output field " << i << ": " << field_name << "\n";
      CeedCallBackend(CeedQFunctionFieldGetEvalMode(qf_output_fields[i], &eval_mode));
      // Basis action
      switch (eval_mode) {
        case CEED_EVAL_NONE:
          code << tab << "CeedScalar r_s" << var_suffix << "[num_comp" << var_suffix << "];\n";
          break;
        case CEED_EVAL_INTERP:
          code << tab << "CeedScalar r_s" << var_suffix << "[num_comp" << var_suffix << "];\n";
          break;
        case CEED_EVAL_GRAD:
          code << tab << "CeedScalar r_s" << var_suffix << "[num_comp" << var_suffix << "*dim" << var_suffix << "];\n";
          break;
          // LCOV_EXCL_START
        case CEED_EVAL_WEIGHT:
          break;  // Should not occur
        case CEED_EVAL_DIV:
        case CEED_EVAL_CURL:
          break;  // TODO: Not implemented
                  // LCOV_EXCL_STOP
      }
    }

  } else if (use_3d_slices) {
    // We treat quadrature points per slice in 3d to save registers
    code << "\n";
    code << tab << "// Note: Using planes of 3D elements\n";
    code << tab << "#pragma unroll\n";
    code << tab << "for (CeedInt q = 0; q < " << Q_name << "; q++) {\n";
    tab.push();
    code << tab << "// -- Input fields\n";
    for (CeedInt i = 0; i < num_input_fields; i++) {
      const char *field_name;
      std::string var_suffix = "_in_" + std::to_string(i);

      CeedCallBackend(CeedOperatorFieldGetName(op_input_fields[i], &field_name));
      code << tab << "// ---- Input field " << i << ": " << field_name << "\n";
      CeedCallBackend(CeedQFunctionFieldGetEvalMode(qf_input_fields[i], &eval_mode));
      // Basis action
      code << tab << "// EvalMode: " << CeedEvalModes[eval_mode] << "\n";
      switch (eval_mode) {
        case CEED_EVAL_NONE:
          bool is_strided;

          code << tab << "CeedScalar r_s" << var_suffix << "[num_comp" << var_suffix << "];\n";

          CeedCallBackend(CeedOperatorFieldGetElemRestriction(op_input_fields[i], &elem_rstr));
          CeedCallBackend(CeedElemRestrictionIsStrided(elem_rstr, &is_strided));
          if (is_strided) {
            bool    has_backend_strides;
            CeedInt num_elem, elem_size;

            CeedCallBackend(CeedElemRestrictionGetElementSize(elem_rstr, &elem_size));
            CeedCallBackend(CeedElemRestrictionHasBackendStrides(elem_rstr, &has_backend_strides));
            CeedCallBackend(CeedElemRestrictionGetNumElements(elem_rstr, &num_elem));
            CeedInt strides[3] = {1, elem_size * num_elem, elem_size};

            if (!has_backend_strides) {
              CeedCallBackend(CeedElemRestrictionGetStrides(elem_rstr, strides));
            }
            code << tab << "const CeedInt strides" << var_suffix << "_0 = " << strides[0] << ", strides" << var_suffix << "_1 = " << strides[1]
                 << ", strides" << var_suffix << "_2 = " << strides[2] << ";\n";
            code << tab << "ReadEVecSliceStrided3d<num_comp" << var_suffix << ", " << Q_name << ", strides" << var_suffix << "_0, strides"
                 << var_suffix << "_1, strides" << var_suffix << "_2>(data, elem, q, d" << var_suffix << ", r_s" << var_suffix << ");\n";
          } else {
            CeedSize                 l_size = 0;
            CeedInt                  comp_stride;
            CeedElemRestriction_Hip *rstr_data;

            CeedCallBackend(CeedElemRestrictionGetLVectorSize(elem_rstr, &l_size));
            code << tab << "const CeedInt l_size" << var_suffix << " = " << l_size << ";\n";
            CeedCallBackend(CeedElemRestrictionGetCompStride(elem_rstr, &comp_stride));
            code << tab << "const CeedInt comp_stride" << var_suffix << " = " << comp_stride << ";\n";
            CeedCallBackend(CeedElemRestrictionGetData(elem_rstr, &rstr_data));
            data->indices.inputs[i] = (CeedInt *)rstr_data->d_offsets;
            code << tab << "ReadEVecSliceStandard3d<num_comp" << var_suffix << ", comp_stride" << var_suffix << ", " << Q_name << ">(data, l_size"
                 << var_suffix << ", elem, q, indices.inputs[" << i << "], d" << var_suffix << ", r_s" << var_suffix << ");\n";
          }
          CeedCallBackend(CeedElemRestrictionDestroy(&elem_rstr));
          break;
        case CEED_EVAL_INTERP:
          code << tab << "CeedScalar r_s" << var_suffix << "[num_comp" << var_suffix << "];\n";
          code << tab << "for (CeedInt j = 0; j < num_comp" << var_suffix << "; j++) {\n";
          tab.push();
          code << tab << "r_s" << var_suffix << "[j] = r_q" << var_suffix << "[q + j*" << Q_name << "];\n";
          tab.pop();
          code << tab << "}\n";
          break;
        case CEED_EVAL_GRAD:
          code << tab << "CeedScalar r_s" << var_suffix << "[num_comp" << var_suffix << "*dim" << var_suffix << "];\n";
          code << tab << "GradColloSlice3d<num_comp" << var_suffix << ", " << Q_name << ", OP_T_1D>(data, q, r_q" << var_suffix << ", s_G"
               << var_suffix << ", r_s" << var_suffix << ");\n";
          break;
        case CEED_EVAL_WEIGHT:
          code << tab << "CeedScalar r_s" << var_suffix << "[1];\n";
          code << tab << "r_s" << var_suffix << "[0] = r_q" << var_suffix << "[q];\n";
          break;
          // LCOV_EXCL_START
        case CEED_EVAL_DIV:
        case CEED_EVAL_CURL:
          break;  // TODO: Not implemented
                  // LCOV_EXCL_STOP
      }
    }
    code << "\n";
    code << tab << "// -- Output fields\n";
    for (CeedInt i = 0; i < num_output_fields; i++) {
      const char *field_name;
      std::string var_suffix = "_out_" + std::to_string(i);

      CeedCallBackend(CeedOperatorFieldGetName(op_output_fields[i], &field_name));
      code << tab << "// ---- Output field " << i << ": " << field_name << "\n";
      CeedCallBackend(CeedQFunctionFieldGetEvalMode(qf_output_fields[i], &eval_mode));
      // Basis action
      switch (eval_mode) {
        case CEED_EVAL_NONE:
          code << tab << "CeedScalar r_s" << var_suffix << "[num_comp" << var_suffix << "];\n";
          break;
        case CEED_EVAL_INTERP:
          code << tab << "CeedScalar r_s" << var_suffix << "[num_comp" << var_suffix << "];\n";
          break;
        case CEED_EVAL_GRAD:
          code << tab << "CeedScalar r_s" << var_suffix << "[num_comp" << var_suffix << "*dim" << var_suffix << "];\n";
          break;
          // LCOV_EXCL_START
        case CEED_EVAL_WEIGHT:
          break;  // Should not occur
        case CEED_EVAL_DIV:
        case CEED_EVAL_CURL:
          break;  // TODO: Not implemented
                  // LCOV_EXCL_STOP
      }
    }
  } else {
    code << "\n";
    code << tab << "// Note: Using full elements\n";
    code << tab << "{\n";
    tab.push();
    code << tab << "// -- Input fields\n";
    for (CeedInt i = 0; i < num_input_fields; i++) {
      const char *field_name;

      CeedCallBackend(CeedOperatorFieldGetName(op_input_fields[i], &field_name));
      code << tab << "// ---- Input field " << i << ": " << field_name << "\n";
      code << tab << "CeedScalar *r_s_in_" << i << " = r_q_in_" << i << ";\n";
    }
    code << tab << "// -- Output fields\n";
    for (CeedInt i = 0; i < num_output_fields; i++) {
      const char *field_name;

      CeedCallBackend(CeedOperatorFieldGetName(op_output_fields[i], &field_name));
      code << tab << "// ---- Output field " << i << ": " << field_name << "\n";
      code << tab << "CeedScalar *r_s_out_" << i << " = r_q_out_" << i << ";\n";
    }
  }

  // Input and output buffers
  code << "\n";
  code << tab << "// -- QFunction inputs and outputs\n";
  code << tab << "// ---- Inputs\n";
  code << tab << "CeedScalar *inputs[" << CeedIntMax(num_input_fields, 1) << "];\n";
  for (CeedInt i = 0; i < num_input_fields; i++) {
    const char *field_name;

    CeedCallBackend(CeedOperatorFieldGetName(op_input_fields[i], &field_name));
    code << tab << "// ------ Input field " << i << ": " << field_name << "\n";
    code << tab << "inputs[" << i << "] = r_s_in_" << i << ";\n";
  }
  code << tab << "// ---- Outputs\n";
  code << tab << "CeedScalar *outputs[" << CeedIntMax(num_output_fields, 1) << "];\n";
  for (CeedInt i = 0; i < num_output_fields; i++) {
    const char *field_name;

    CeedCallBackend(CeedOperatorFieldGetName(op_output_fields[i], &field_name));
    code << tab << "// ------ Output field " << i << ": " << field_name << "\n";
    code << tab << "outputs[" << i << "] = r_s_out_" << i << ";\n";
  }

  // Apply QFunction
  code << "\n";
  code << tab << "// -- Apply QFunction\n";
  code << tab << "" << qfunction_name << "(ctx, ";
  if (max_dim != 3 || is_at_points || use_3d_slices || !is_all_tensor) {
    code << "1";
  } else {
    code << Q_name;
  }
  code << ", inputs, outputs);\n";

  if (is_at_points) {
    // Map back to coefficients
    code << "\n";
    code << tab << "// -- Output fields\n";
    for (CeedInt i = 0; i < num_output_fields; i++) {
      const char *field_name;
      std::string var_suffix = "_out_" + std::to_string(i);
      std::string P_name     = "P_1d" + var_suffix;

      CeedCallBackend(CeedOperatorFieldGetName(op_output_fields[i], &field_name));
      code << tab << "// ---- Output field " << i << ": " << field_name << "\n";
      CeedCallBackend(CeedQFunctionFieldGetEvalMode(qf_output_fields[i], &eval_mode));
      // Basis action
      code << tab << "// EvalMode: " << CeedEvalModes[eval_mode] << "\n";
      switch (eval_mode) {
        case CEED_EVAL_NONE: {
          CeedInt             comp_stride;
          CeedElemRestriction elem_rstr;

          CeedCallBackend(CeedOperatorFieldGetElemRestriction(op_output_fields[i], &elem_rstr));
          CeedCallBackend(CeedElemRestrictionGetCompStride(elem_rstr, &comp_stride));
          CeedCallBackend(CeedElemRestrictionDestroy(&elem_rstr));
          code << tab << "const CeedInt comp_stride" << var_suffix << " = " << comp_stride << ";\n";
          code << tab << "WritePoint<num_comp" << var_suffix << ", comp_stride" << var_suffix
               << ", max_num_points>(data, elem, i, points.num_per_elem[elem], indices.outputs[" << i << "]"
               << ", r_s" << var_suffix << ", d" << var_suffix << ");\n";
          break;
        }
        case CEED_EVAL_INTERP:
          code << tab << "if (i >= points.num_per_elem[elem]) {\n";
          tab.push();
          code << tab << "for (CeedInt j = 0; j < num_comp" << var_suffix << "; j++) r_s" << var_suffix << "[j] = 0.0;\n";
          tab.pop();
          code << tab << "}\n";
          code << tab << "InterpTransposeAtPoints" << max_dim << "d<num_comp" << var_suffix << ", max_num_points, " << P_name << ", " << Q_name
               << ">(data, i, r_s" << var_suffix << ", r_x, r_c" << var_suffix << ");\n";
          break;
        case CEED_EVAL_GRAD:
          code << tab << "if (i >= points.num_per_elem[elem]) {\n";
          tab.push();
          code << tab << "for (CeedInt j = 0; j < num_comp" << var_suffix << "*dim" << var_suffix << "; j++) r_s" << var_suffix << "[j] = 0.0;\n";
          tab.pop();
          code << tab << "}\n";
          code << tab << "GradTransposeAtPoints" << max_dim << "d<num_comp" << var_suffix << ", max_num_points, " << P_name << ", " << Q_name
               << ">(data, i, r_s" << var_suffix << ", r_x, r_c" << var_suffix << ");\n";
          break;
          // LCOV_EXCL_START
        case CEED_EVAL_WEIGHT:
          break;  // Should not occur
        case CEED_EVAL_DIV:
        case CEED_EVAL_CURL:
          break;  // TODO: Not implemented
                  // LCOV_EXCL_STOP
      }
    }
  } else if (use_3d_slices) {
    // Copy or apply transpose grad, if needed
    code << "\n";
    code << tab << "// -- Output fields\n";
    for (CeedInt i = 0; i < num_output_fields; i++) {
      const char *field_name;
      std::string var_suffix = "_out_" + std::to_string(i);
      std::string P_name     = "P_1d" + var_suffix;

      CeedCallBackend(CeedOperatorFieldGetName(op_output_fields[i], &field_name));
      code << tab << "// ---- Output field " << i << ": " << field_name << "\n";
      CeedCallBackend(CeedQFunctionFieldGetEvalMode(qf_output_fields[i], &eval_mode));
      // Basis action
      code << tab << "// EvalMode: " << CeedEvalModes[eval_mode] << "\n";
      switch (eval_mode) {
        case CEED_EVAL_NONE:
          code << tab << "for (CeedInt j = 0; j < num_comp" << var_suffix << " ; j++) {\n";
          tab.push();
          code << tab << "r_q" << var_suffix << "[q + j*" << Q_name << "] = r_s" << var_suffix << "[j];\n";
          tab.pop();
          code << tab << "}\n";
          break;
        case CEED_EVAL_INTERP:
          code << tab << "for (CeedInt j = 0; j < num_comp" << var_suffix << " ; j++) {\n";
          tab.push();
          code << tab << "r_q" << var_suffix << "[q + j*" << Q_name << "] = r_s" << var_suffix << "[j];\n";
          tab.pop();
          code << tab << "}\n";
          break;
        case CEED_EVAL_GRAD:
          code << tab << "GradColloSliceTranspose3d<num_comp" << var_suffix << ", " << Q_name << ", OP_T_1D>(data, q, r_s" << var_suffix << ", s_G"
               << var_suffix << ", r_q" << var_suffix << ");\n";
          break;
          // LCOV_EXCL_START
        case CEED_EVAL_WEIGHT:
          break;  // Should not occur
        case CEED_EVAL_DIV:
        case CEED_EVAL_CURL:
          break;  // TODO: Not implemented
                  // LCOV_EXCL_STOP
      }
    }
  }
  tab.pop();
  code << tab << "}\n";
  return CEED_ERROR_SUCCESS;
}

//------------------------------------------------------------------------------
// Build single operator kernel
//------------------------------------------------------------------------------
extern "C" int CeedOperatorBuildKernel_Hip_gen(CeedOperator op, bool *is_good_build) {
  bool                   is_all_tensor = true, is_all_nontensor = true, is_at_points = false, use_3d_slices = false;
  Ceed                   ceed;
  CeedInt                Q = 0, Q_1d = 0, num_input_fields, num_output_fields, max_dim = 1, max_num_points = 0, coords_comp_stride = 0;
  CeedQFunctionField    *qf_input_fields, *qf_output_fields;
  CeedQFunction_Hip_gen *qf_data;
  CeedQFunction          qf;
  CeedOperatorField     *op_input_fields, *op_output_fields;
  CeedOperator_Hip_gen  *data;
  std::ostringstream     code;
  Tab                    tab;

  CeedCallBackend(CeedOperatorGetData(op, &data));
  {
    bool is_setup_done;

    CeedCallBackend(CeedOperatorIsSetupDone(op, &is_setup_done));
    if (is_setup_done) {
      *is_good_build = !data->use_fallback;
      return CEED_ERROR_SUCCESS;
    }
  }

  // Check field compatibility
  CeedCallBackend(CeedOperatorGetFields(op, &num_input_fields, &op_input_fields, &num_output_fields, &op_output_fields));
  {
    bool has_shared_bases = true;

    for (CeedInt i = 0; i < num_input_fields; i++) {
      CeedBasis basis;

      CeedCallBackend(CeedOperatorFieldGetBasis(op_input_fields[i], &basis));
      if (basis != CEED_BASIS_NONE) {
        bool        is_tensor = true;
        const char *resource;
        char       *resource_root;
        Ceed        basis_ceed;

        CeedCallBackend(CeedBasisIsTensor(basis, &is_tensor));
        is_all_tensor    = is_all_tensor && is_tensor;
        is_all_nontensor = is_all_nontensor && !is_tensor;
        CeedCallBackend(CeedBasisGetCeed(basis, &basis_ceed));
        CeedCallBackend(CeedGetResource(basis_ceed, &resource));
        CeedCallBackend(CeedGetResourceRoot(basis_ceed, resource, ":", &resource_root));
        has_shared_bases = has_shared_bases && !strcmp(resource_root, "/gpu/hip/shared");
        CeedCallBackend(CeedFree(&resource_root));
        CeedCallBackend(CeedDestroy(&basis_ceed));
      }
      CeedCallBackend(CeedBasisDestroy(&basis));
    }

    for (CeedInt i = 0; i < num_output_fields; i++) {
      CeedBasis basis;

      CeedCallBackend(CeedOperatorFieldGetBasis(op_output_fields[i], &basis));
      if (basis != CEED_BASIS_NONE) {
        bool        is_tensor = true;
        const char *resource;
        char       *resource_root;
        Ceed        basis_ceed;

        CeedCallBackend(CeedBasisIsTensor(basis, &is_tensor));
        is_all_tensor    = is_all_tensor && is_tensor;
        is_all_nontensor = is_all_nontensor && !is_tensor;

        CeedCallBackend(CeedBasisGetCeed(basis, &basis_ceed));
        CeedCallBackend(CeedGetResource(basis_ceed, &resource));
        CeedCallBackend(CeedGetResourceRoot(basis_ceed, resource, ":", &resource_root));
        has_shared_bases = has_shared_bases && !strcmp(resource_root, "/gpu/hip/shared");
        CeedCallBackend(CeedFree(&resource_root));
        CeedCallBackend(CeedDestroy(&basis_ceed));
      }
      CeedCallBackend(CeedBasisDestroy(&basis));
    }
    // -- Fallback to ref if not all bases are shared
    if (!has_shared_bases) {
      *is_good_build = false;
      return CEED_ERROR_SUCCESS;
    }
  }
  CeedCallBackend(CeedOperatorGetCeed(op, &ceed));
  CeedCallBackend(CeedOperatorGetQFunction(op, &qf));
  CeedCallBackend(CeedQFunctionGetData(qf, &qf_data));
  CeedCallBackend(CeedQFunctionGetFields(qf, NULL, &qf_input_fields, NULL, &qf_output_fields));

  // Get operator data
  CeedCallBackend(CeedOperatorIsAtPoints(op, &is_at_points));
  {
    CeedInt max_P = 0, max_P_1d = 0;

    CeedCallBackend(CeedOperatorBuildKernelData_Hip_gen(ceed, num_input_fields, op_input_fields, qf_input_fields, num_output_fields, op_output_fields,
                                                        qf_output_fields, &max_P, &max_P_1d, &Q, &Q_1d, &max_dim, &is_all_tensor, &use_3d_slices));
    data->max_P_1d = is_all_tensor ? max_P_1d : max_P;
  }
  if (max_dim == 0) max_dim = 1;
  data->dim = max_dim;
  if (is_at_points) {
    CeedElemRestriction_Hip *rstr_data;
    CeedElemRestriction      rstr_points = NULL;

    CeedCallBackend(CeedOperatorAtPointsGetPoints(op, &rstr_points, NULL));
    CeedCallBackend(CeedElemRestrictionGetMaxPointsInElement(rstr_points, &max_num_points));
    CeedCallBackend(CeedElemRestrictionGetCompStride(rstr_points, &coords_comp_stride));
    CeedCallBackend(CeedElemRestrictionGetData(rstr_points, &rstr_data));
    data->points.indices = (CeedInt *)rstr_data->d_offsets;
    CeedCallBackend(CeedElemRestrictionDestroy(&rstr_points));
  }
  if (is_at_points) use_3d_slices = false;
  if (Q_1d == 0) {
    if (is_at_points) Q_1d = max_num_points;
    else CeedCallBackend(CeedOperatorGetNumQuadraturePoints(op, &Q_1d));
  }
  if (Q == 0) Q = Q_1d;
  data->Q    = Q;
  data->Q_1d = Q_1d;

  // Check for restriction only identity operator
  {
    bool is_identity_qf;

    CeedCallBackend(CeedQFunctionIsIdentity(qf, &is_identity_qf));
    if (is_identity_qf) {
      CeedEvalMode eval_mode_in, eval_mode_out;

      CeedCallBackend(CeedQFunctionFieldGetEvalMode(qf_input_fields[0], &eval_mode_in));
      CeedCallBackend(CeedQFunctionFieldGetEvalMode(qf_output_fields[0], &eval_mode_out));
      CeedCheck(eval_mode_in != CEED_EVAL_NONE || eval_mode_out != CEED_EVAL_NONE, ceed, CEED_ERROR_BACKEND,
                "Backend does not implement restriction only identity operators");
    }
  }

  // Load basis source files
  if (!is_all_nontensor) {
    code << tab << "// Tensor basis source\n";
    code << tab << "#include <ceed/jit-source/hip/hip-shared-basis-tensor-templates.h>\n\n";
  }
  if (!is_all_tensor) {
    code << tab << "// Non-tensor basis source\n";
    code << tab << "#include <ceed/jit-source/hip/hip-shared-basis-nontensor-templates.h>\n\n";
  }
  if (is_at_points) {
    code << tab << "// AtPoints basis source\n";
    code << tab << "#include <ceed/jit-source/hip/hip-shared-basis-tensor-at-points-templates.h>\n\n";
  }
  if (!is_all_tensor && !is_all_nontensor) {
    code << tab << "// Tensor basis source\n";
    code << tab << "#include <ceed/jit-source/hip/hip-shared-basis-tensor-flattened-templates.h>\n\n";
  }
  code << tab << "// CodeGen operator source\n";
  code << tab << "#include <ceed/jit-source/hip/hip-gen-templates.h>\n\n";

  // Get QFunction name
  std::string qfunction_name(qf_data->qfunction_name);
  std::string operator_name;

  operator_name = "CeedKernelHipGenOperator_" + qfunction_name;

  // Define CEED_Q_VLA
  code << "\n" << tab << "#undef CEED_Q_VLA\n";
  if (max_dim != 3 || is_at_points || use_3d_slices || !is_all_tensor) {
    code << tab << "#define CEED_Q_VLA 1\n\n";
  } else {
    code << tab << "#define CEED_Q_VLA " << Q_1d << "\n\n";
  }

  // Add user QFunction source
  {
    const char *source_path;

    CeedCallBackend(CeedQFunctionGetSourcePath(qf, &source_path));
    CeedCheck(source_path, ceed, CEED_ERROR_UNSUPPORTED, "/gpu/hip/gen backend requires QFunction source code file");

    code << tab << "// User QFunction source\n";
    code << tab << "#include \"" << source_path << "\"\n\n";
  }

  // Setup
  code << "\n" << tab << "// -----------------------------------------------------------------------------\n";
  code << tab << "// Operator Kernel\n";
  code << tab << "// \n";
  code << tab << "// d_[in,out]_i:   CeedVector device array\n";
  code << tab << "// r_[in,out]_e_i: Element vector register\n";
  code << tab << "// r_[in,out]_q_i: Quadrature space vector register\n";
  code << tab << "// r_[in,out]_c_i: AtPoints Chebyshev coefficients register\n";
  code << tab << "// r_[in,out]_s_i: Quadrature space slice vector register\n";
  code << tab << "// \n";
  code << tab << "// s_B_[in,out]_i: Interpolation matrix, shared memory\n";
  code << tab << "// s_G_[in,out]_i: Gradient matrix, shared memory\n";
  code << tab << "// -----------------------------------------------------------------------------\n";
  code << tab << "extern \"C\" __launch_bounds__(BLOCK_SIZE)\n";
  code << "__global__ void " << operator_name
       << "(CeedInt num_elem, void* ctx, FieldsInt_Hip indices, Fields_Hip fields, Fields_Hip B, Fields_Hip G, CeedScalar* W, Points_Hip points) {\n";
  tab.push();

  // Scratch buffers
  for (CeedInt i = 0; i < num_input_fields; i++) {
    CeedEvalMode eval_mode;

    CeedCallBackend(CeedQFunctionFieldGetEvalMode(qf_input_fields[i], &eval_mode));
    if (eval_mode != CEED_EVAL_WEIGHT) {  // Skip CEED_EVAL_WEIGHT
      code << tab << "const CeedScalar *__restrict__ d_in_" << i << " = fields.inputs[" << i << "];\n";
    }
  }
  for (CeedInt i = 0; i < num_output_fields; i++) {
    code << tab << "CeedScalar *__restrict__ d_out_" << i << " = fields.outputs[" << i << "];\n";
  }

  code << tab << "const CeedInt max_dim = " << max_dim << ";\n";
  if (!is_all_tensor) {
    code << tab << "const CeedInt Q = " << Q << ";\n";
  }
  if (!is_all_nontensor) {
    code << tab << "const CeedInt Q_1d = " << Q_1d << ";\n";
  }
  if (is_at_points) {
    code << tab << "const CeedInt max_num_points = " << max_num_points << ";\n";
    code << tab << "const CeedInt coords_comp_stride = " << coords_comp_stride << ";\n";
  }

  // Shared data
  code << tab << "extern __shared__ CeedScalar slice[];\n";
  code << tab << "SharedData_Hip data;\n";
  code << tab << "data.t_id_x = threadIdx.x;\n";
  code << tab << "data.t_id_y = threadIdx.y;\n";
  code << tab << "data.t_id_z = threadIdx.z;\n";
  code << tab << "data.t_id   = threadIdx.x + threadIdx.y*blockDim.x + threadIdx.z*blockDim.y*blockDim.x;\n";
  code << tab << "data.slice  = slice + data.t_id_z*OP_T_1D" << ((!is_all_tensor || max_dim == 1) ? "" : "*OP_T_1D") << ";\n";

  // -- Determine input mat reuse
  FieldReuse_Hip input_matrix_reuse[CEED_FIELD_MAX];

  for (CeedInt i = 0; i < num_input_fields; i++) {
    input_matrix_reuse[i].index = -1;
  }
  for (CeedInt i = 0; i < num_input_fields; i++) {
    bool         is_tensor = true;
    CeedEvalMode eval_mode_i;
    CeedBasis    basis_i;

    CeedCallBackend(CeedQFunctionFieldGetEvalMode(qf_input_fields[i], &eval_mode_i));
    if (eval_mode_i == CEED_EVAL_WEIGHT) continue;
    CeedCallBackend(CeedOperatorFieldGetBasis(op_input_fields[i], &basis_i));
    CeedCallBackend(CeedBasisIsTensor(basis_i, &is_tensor));
    for (CeedInt j = 0; (input_matrix_reuse[i].index == -1) && (j < i); j++) {
      CeedEvalMode eval_mode_j;
      CeedBasis    basis_j;

      CeedCallBackend(CeedQFunctionFieldGetEvalMode(qf_input_fields[j], &eval_mode_j));
      if (eval_mode_j == CEED_EVAL_WEIGHT) continue;
      CeedCallBackend(CeedOperatorFieldGetBasis(op_input_fields[j], &basis_j));
      if (basis_i == basis_j) {
        if (is_tensor) {
          input_matrix_reuse[i].index     = j;
          input_matrix_reuse[i].is_input  = true;
          input_matrix_reuse[i].eval_mode = eval_mode_j;
        } else {
          // For non-tensor can only re-use with the same eval mode
          if (eval_mode_i == eval_mode_j) {
            input_matrix_reuse[i].index     = j;
            input_matrix_reuse[i].is_input  = true;
            input_matrix_reuse[i].eval_mode = eval_mode_j;
          }
        }
      }
      CeedCallBackend(CeedBasisDestroy(&basis_j));
    }
    CeedCallBackend(CeedBasisDestroy(&basis_i));
  }

  // -- Determine output mat reuse
  FieldReuse_Hip output_matrix_reuse[CEED_FIELD_MAX];

  for (CeedInt i = 0; i < num_output_fields; i++) {
    output_matrix_reuse[i].index = -1;
  }
  for (CeedInt i = 0; i < num_output_fields; i++) {
    bool         is_tensor = true;
    CeedEvalMode eval_mode_i;
    CeedBasis    basis_i;

    CeedCallBackend(CeedQFunctionFieldGetEvalMode(qf_output_fields[i], &eval_mode_i));
    CeedCallBackend(CeedOperatorFieldGetBasis(op_output_fields[i], &basis_i));
    for (CeedInt j = 0; (output_matrix_reuse[i].index == -1) && (j < num_input_fields); j++) {
      CeedEvalMode eval_mode_j;
      CeedBasis    basis_j;

      CeedCallBackend(CeedQFunctionFieldGetEvalMode(qf_input_fields[j], &eval_mode_j));
      if (eval_mode_j == CEED_EVAL_WEIGHT) continue;
      CeedCallBackend(CeedOperatorFieldGetBasis(op_input_fields[j], &basis_j));
      if (basis_i == basis_j) {
        if (is_tensor) {
          output_matrix_reuse[i].index     = j;
          output_matrix_reuse[i].is_input  = true;
          output_matrix_reuse[i].eval_mode = eval_mode_j;
        } else {
          // For non-tensor can only re-use with the same eval mode
          if (eval_mode_i == eval_mode_j) {
            output_matrix_reuse[i].index     = j;
            output_matrix_reuse[i].is_input  = true;
            output_matrix_reuse[i].eval_mode = eval_mode_j;
          }
        }
      }
      CeedCallBackend(CeedBasisDestroy(&basis_j));
    }
    for (CeedInt j = 0; (output_matrix_reuse[i].index == -1) && (j < i); j++) {
      CeedEvalMode eval_mode_j;
      CeedBasis    basis_j;

      CeedCallBackend(CeedQFunctionFieldGetEvalMode(qf_output_fields[j], &eval_mode_j));
      if (eval_mode_j == CEED_EVAL_WEIGHT) continue;
      CeedCallBackend(CeedOperatorFieldGetBasis(op_output_fields[j], &basis_j));
      CeedCallBackend(CeedBasisIsTensor(basis_i, &is_tensor));
      if (basis_i == basis_j) {
        if (is_tensor) {
          output_matrix_reuse[i].index     = j;
          output_matrix_reuse[i].is_input  = false;
          output_matrix_reuse[i].eval_mode = eval_mode_j;
        } else {
          // For non-tensor can only re-use with the same eval mode
          if (eval_mode_i == eval_mode_j) {
            output_matrix_reuse[i].index     = j;
            output_matrix_reuse[i].is_input  = false;
            output_matrix_reuse[i].eval_mode = eval_mode_j;
          }
        }
      }
      CeedCallBackend(CeedBasisDestroy(&basis_j));
    }
    CeedCallBackend(CeedBasisDestroy(&basis_i));
  }

  // Initialize constants, and matrices B and G
  code << "\n" << tab << "// Input field constants and basis data\n";
  for (CeedInt i = 0; i < num_input_fields; i++) {
    CeedCallBackend(CeedOperatorBuildKernelFieldData_Hip_gen(code, data, tab, i, op_input_fields[i], qf_input_fields[i], input_matrix_reuse[i],
                                                             max_dim, Q, Q_1d, true, is_all_tensor, is_at_points, use_3d_slices));
  }
  code << "\n" << tab << "// Output field constants and basis data\n";
  for (CeedInt i = 0; i < num_output_fields; i++) {
    CeedCallBackend(CeedOperatorBuildKernelFieldData_Hip_gen(code, data, tab, i, op_output_fields[i], qf_output_fields[i], output_matrix_reuse[i],
                                                             max_dim, Q, Q_1d, false, is_all_tensor, is_at_points, use_3d_slices));
  }

  // Loop over all elements
  code << "\n" << tab << "// Element loop\n";
  code << tab << "__syncthreads();\n";
  code << tab << "for (CeedInt elem = blockIdx.x*blockDim.z + threadIdx.z; elem < num_elem; elem += gridDim.x*blockDim.z) {\n";
  tab.push();

  // -- Compute minimum buffer space needed
  CeedInt max_rstr_buffer_size = 1;

  for (CeedInt i = 0; i < num_input_fields; i++) {
    CeedEvalMode eval_mode;

    CeedCallBackend(CeedQFunctionFieldGetEvalMode(qf_input_fields[i], &eval_mode));
    if (eval_mode != CEED_EVAL_NONE && eval_mode != CEED_EVAL_WEIGHT) {
      CeedInt             num_comp;
      CeedElemRestriction elem_rstr;

      CeedCallBackend(CeedOperatorFieldGetElemRestriction(op_input_fields[i], &elem_rstr));
      CeedCallBackend(CeedElemRestrictionGetNumComponents(elem_rstr, &num_comp));
      max_rstr_buffer_size = CeedIntMax(max_rstr_buffer_size, num_comp * (is_all_tensor && (max_dim >= 3) ? Q_1d : 1));
      CeedCallBackend(CeedElemRestrictionDestroy(&elem_rstr));
    }
  }
  for (CeedInt i = 0; i < num_output_fields; i++) {
    CeedEvalMode eval_mode;

    CeedCallBackend(CeedQFunctionFieldGetEvalMode(qf_output_fields[i], &eval_mode));
    if (eval_mode != CEED_EVAL_NONE) {
      CeedInt             num_comp;
      CeedElemRestriction elem_rstr;

      CeedCallBackend(CeedOperatorFieldGetElemRestriction(op_output_fields[i], &elem_rstr));
      CeedCallBackend(CeedElemRestrictionGetNumComponents(elem_rstr, &num_comp));
      max_rstr_buffer_size = CeedIntMax(max_rstr_buffer_size, num_comp * (is_all_tensor && (max_dim >= 3) ? Q_1d : 1));
      CeedCallBackend(CeedElemRestrictionDestroy(&elem_rstr));
    }
  }
  code << tab << "// Scratch restriction buffer space\n";
  code << tab << "CeedScalar r_e_scratch[" << max_rstr_buffer_size << "];\n";

  // -- Determine best input field processing order
  CeedInt field_rstr_in_buffer[CEED_FIELD_MAX], input_field_order[CEED_FIELD_MAX];

  for (CeedInt i = 0; i < num_input_fields; i++) {
    field_rstr_in_buffer[i] = -1;
    input_field_order[i]    = -1;
  }
  {
    bool    is_ordered[CEED_FIELD_MAX];
    CeedInt curr_index = 0;

    for (CeedInt i = 0; i < num_input_fields; i++) is_ordered[i] = false;
    for (CeedInt i = 0; i < num_input_fields; i++) {
      CeedVector          vec_i;
      CeedElemRestriction rstr_i;

      if (is_ordered[i]) continue;
      field_rstr_in_buffer[i]       = i;
      is_ordered[i]                 = true;
      input_field_order[curr_index] = i;
      curr_index++;
      CeedCallBackend(CeedOperatorFieldGetVector(op_input_fields[i], &vec_i));
      if (vec_i == CEED_VECTOR_NONE) continue;  // CEED_EVAL_WEIGHT
      CeedCallBackend(CeedOperatorFieldGetElemRestriction(op_input_fields[i], &rstr_i));
      for (CeedInt j = i + 1; j < num_input_fields; j++) {
        CeedVector          vec_j;
        CeedElemRestriction rstr_j;

        CeedCallBackend(CeedOperatorFieldGetVector(op_input_fields[j], &vec_j));
        CeedCallBackend(CeedOperatorFieldGetElemRestriction(op_input_fields[j], &rstr_j));
        if (rstr_i == rstr_j && vec_i == vec_j) {
          field_rstr_in_buffer[j]       = i;
          is_ordered[j]                 = true;
          input_field_order[curr_index] = j;
          curr_index++;
        }
        CeedCallBackend(CeedVectorDestroy(&vec_j));
        CeedCallBackend(CeedElemRestrictionDestroy(&rstr_j));
      }
      CeedCallBackend(CeedVectorDestroy(&vec_i));
      CeedCallBackend(CeedElemRestrictionDestroy(&rstr_i));
    }
  }

  // -- Input restriction and basis
  code << "\n" << tab << "// -- Input field restrictions and basis actions\n";
  for (CeedInt i = 0; i < num_input_fields; i++) {
    const char   *field_name;
    const CeedInt f = input_field_order[i];

    CeedCallBackend(CeedOperatorFieldGetName(op_input_fields[f], &field_name));
    code << tab << "// ---- Input field " << f << ": " << field_name << "\n";

    // ---- Restriction
    CeedCallBackend(CeedOperatorBuildKernelRestriction_Hip_gen(code, data, tab, f, field_rstr_in_buffer, op_input_fields[f], qf_input_fields[f],
                                                               max_dim, Q_1d, true, is_all_tensor, is_at_points, use_3d_slices));

    // ---- Basis action
    CeedCallBackend(CeedOperatorBuildKernelBasis_Hip_gen(code, data, tab, f, op_input_fields[f], qf_input_fields[f], max_dim, Q_1d, true,
                                                         is_all_tensor, is_at_points, use_3d_slices));
  }

  // -- Q function
  CeedCallBackend(CeedOperatorBuildKernelQFunction_Hip_gen(code, data, tab, max_dim, max_num_points, num_input_fields, op_input_fields,
                                                           qf_input_fields, num_output_fields, op_output_fields, qf_output_fields, qfunction_name,
                                                           Q_1d, is_all_tensor, is_at_points, use_3d_slices));

  // -- Output basis and restriction
  code << "\n" << tab << "// -- Output field basis action and restrictions\n";
  for (CeedInt i = 0; i < num_output_fields; i++) {
    const char *field_name;

    CeedCallBackend(CeedOperatorFieldGetName(op_output_fields[i], &field_name));
    code << tab << "// ---- Output field " << i << ": " << field_name << "\n";

    // ---- Basis action
    CeedCallBackend(CeedOperatorBuildKernelBasis_Hip_gen(code, data, tab, i, op_output_fields[i], qf_output_fields[i], max_dim, Q_1d, false,
                                                         is_all_tensor, is_at_points, use_3d_slices));

    // ---- Restriction
    CeedCallBackend(CeedOperatorBuildKernelRestriction_Hip_gen(code, data, tab, i, NULL, op_output_fields[i], qf_output_fields[i], max_dim, Q_1d,
                                                               false, is_all_tensor, is_at_points, use_3d_slices));
  }

  // Close loop and function
  tab.pop();
  code << tab << "}\n";
  tab.pop();
  code << tab << "}\n";
  code << tab << "// -----------------------------------------------------------------------------\n\n";

  CeedInt block_sizes[3] = {0, 0, 0};
  CeedInt num_elem;

  // Compile
  CeedCallBackend(CeedOperatorGetNumElements(op, &num_elem));
  CeedCallBackend(BlockGridCalculate_Hip_gen(is_all_tensor ? max_dim : 1, num_elem, data->max_P_1d, is_all_tensor ? Q_1d : Q, block_sizes));
  {
    bool is_compile_good = false;

    data->thread_1d = block_sizes[0];
    CeedCallBackend(CeedTryCompile_Hip(ceed, code.str().c_str(), &is_compile_good, &data->module, 2, "OP_T_1D", block_sizes[0], "BLOCK_SIZE",
                                       block_sizes[0] * block_sizes[1] * block_sizes[2]));
    if (is_compile_good) {
      *is_good_build = true;
      CeedCallBackend(CeedGetKernel_Hip(ceed, data->module, operator_name.c_str(), &data->op));
    } else {
      *is_good_build     = false;
      data->use_fallback = true;
    }
  }
  CeedCallBackend(CeedOperatorSetSetupDone(op));
  CeedCallBackend(CeedDestroy(&ceed));
  CeedCallBackend(CeedQFunctionDestroy(&qf));
  return CEED_ERROR_SUCCESS;
}

//------------------------------------------------------------------------------
// Build AtPoints assembly operator kernel
//------------------------------------------------------------------------------
static int CeedOperatorBuildKernelAssemblyAtPoints_Hip_gen(CeedOperator op, bool is_full, bool *is_good_build) {
  bool                   is_all_tensor = true, is_at_points = false, use_3d_slices = false;
  Ceed                   ceed;
  CeedInt                Q, Q_1d, num_input_fields, num_output_fields, max_dim = 1, max_num_points = 0, coords_comp_stride = 0;
  CeedQFunctionField    *qf_input_fields, *qf_output_fields;
  CeedQFunction_Hip_gen *qf_data;
  CeedQFunction          qf;
  CeedOperatorField     *op_input_fields, *op_output_fields;
  CeedOperator_Hip_gen  *data;
  std::ostringstream     code;
  Tab                    tab;

  // Check compatibility
  CeedCallBackend(CeedOperatorGetCeed(op, &ceed));
  CeedCallBackend(CeedOperatorIsAtPoints(op, &is_at_points));
  CeedCheck(is_at_points, ceed, CEED_ERROR_BACKEND, "Only AtPoints operator assembly supported");

  // Retrieve operator data
  CeedCallBackend(CeedOperatorGetData(op, &data));
  Q       = data->Q;
  Q_1d    = data->Q_1d;
  max_dim = data->dim;
  {
    CeedElemRestriction rstr_points = NULL;

    CeedCallBackend(CeedOperatorAtPointsGetPoints(op, &rstr_points, NULL));
    CeedCallBackend(CeedElemRestrictionGetMaxPointsInElement(rstr_points, &max_num_points));
    CeedCallBackend(CeedElemRestrictionGetCompStride(rstr_points, &coords_comp_stride));
    CeedCallBackend(CeedElemRestrictionDestroy(&rstr_points));
  }
  CeedCallBackend(CeedOperatorGetQFunction(op, &qf));
  CeedCallBackend(CeedQFunctionGetData(qf, &qf_data));
  CeedCallBackend(CeedQFunctionGetFields(qf, NULL, &qf_input_fields, NULL, &qf_output_fields));
  CeedCallBackend(CeedOperatorGetFields(op, &num_input_fields, &op_input_fields, &num_output_fields, &op_output_fields));

  // Load basis source files
  code << tab << "// Tensor basis source\n";
  code << tab << "#include <ceed/jit-source/hip/hip-shared-basis-tensor-templates.h>\n\n";
  code << tab << "// AtPoints basis source\n";
  code << tab << "#include <ceed/jit-source/hip/hip-shared-basis-tensor-at-points-templates.h>\n\n";
  code << tab << "// CodeGen operator source\n";
  code << tab << "#include <ceed/jit-source/hip/hip-gen-templates.h>\n\n";

  // Get QFunction name
  std::string qfunction_name(qf_data->qfunction_name);
  std::string operator_name;

  if (is_full) {
    operator_name = "CeedKernelHipGenOperatorFullAssembly_" + qfunction_name;
  } else {
    operator_name = "CeedKernelHipGenOperatorDiagonalAssembly_" + qfunction_name;
  }

  // Define CEED_Q_VLA
  code << "\n" << tab << "#undef CEED_Q_VLA\n";
  code << tab << "#define CEED_Q_VLA 1\n\n";

  // Add user QFunction source
  {
    const char *source_path;

    CeedCallBackend(CeedQFunctionGetSourcePath(qf, &source_path));
    CeedCheck(source_path, ceed, CEED_ERROR_UNSUPPORTED, "/gpu/hip/gen backend requires QFunction source code file");

    code << tab << "// User QFunction source\n";
    code << tab << "#include \"" << source_path << "\"\n\n";
  }

  // Setup
  code << "\n" << tab << "// -----------------------------------------------------------------------------\n";
  code << tab << "// Operator Assembly Kernel\n";
  code << tab << "// \n";
  code << tab << "// d_[in,out]_i:   CeedVector device array\n";
  code << tab << "// r_[in,out]_e_i: Element vector register\n";
  code << tab << "// r_[in,out]_q_i: Quadrature space vector register\n";
  code << tab << "// r_[in,out]_c_i: AtPoints Chebyshev coefficients register\n";
  code << tab << "// r_[in,out]_s_i: Quadrature space slice vector register\n";
  code << tab << "// \n";
  code << tab << "// s_B_[in,out]_i: Interpolation matrix, shared memory\n";
  code << tab << "// s_G_[in,out]_i: Gradient matrix, shared memory\n";
  code << tab << "// -----------------------------------------------------------------------------\n";
  code << tab << "extern \"C\" __global__ void " << operator_name
       << "(CeedInt num_elem, void* ctx, FieldsInt_Hip indices, Fields_Hip fields, Fields_Hip B, Fields_Hip G, CeedScalar *W, Points_Hip "
          "points, CeedScalar *__restrict__ values_array) {\n";
  tab.push();

  // Scratch buffers
  for (CeedInt i = 0; i < num_input_fields; i++) {
    CeedEvalMode eval_mode;

    CeedCallBackend(CeedQFunctionFieldGetEvalMode(qf_input_fields[i], &eval_mode));
    if (eval_mode != CEED_EVAL_WEIGHT) {  // Skip CEED_EVAL_WEIGHT
      code << tab << "const CeedScalar *__restrict__ d_in_" << i << " = fields.inputs[" << i << "];\n";
    }
  }
  for (CeedInt i = 0; i < num_output_fields; i++) {
    code << tab << "CeedScalar *__restrict__ d_out_" << i << " = fields.outputs[" << i << "];\n";
  }

  code << tab << "const CeedInt max_dim = " << max_dim << ";\n";
  code << tab << "const CeedInt Q_1d = " << Q_1d << ";\n";
  code << tab << "const CeedInt max_num_points = " << max_num_points << ";\n";
  code << tab << "const CeedInt coords_comp_stride = " << coords_comp_stride << ";\n";

  // Shared data
  code << tab << "extern __shared__ CeedScalar slice[];\n";
  code << tab << "SharedData_Hip data;\n";
  code << tab << "data.t_id_x = threadIdx.x;\n";
  code << tab << "data.t_id_y = threadIdx.y;\n";
  code << tab << "data.t_id_z = threadIdx.z;\n";
  code << tab << "data.t_id   = threadIdx.x + threadIdx.y*blockDim.x + threadIdx.z*blockDim.y*blockDim.x;\n";
  code << tab << "data.slice  = slice + data.t_id_z*OP_T_1D" << ((!is_all_tensor || max_dim == 1) ? "" : "*OP_T_1D") << ";\n";

  // -- Determine input mat reuse
  FieldReuse_Hip input_matrix_reuse[CEED_FIELD_MAX];

  for (CeedInt i = 0; i < num_input_fields; i++) {
    input_matrix_reuse[i].index = -1;
  }
  for (CeedInt i = 0; i < num_input_fields; i++) {
    CeedEvalMode eval_mode_i;
    CeedBasis    basis_i;

    CeedCallBackend(CeedQFunctionFieldGetEvalMode(qf_input_fields[i], &eval_mode_i));
    if (eval_mode_i == CEED_EVAL_WEIGHT) continue;
    CeedCallBackend(CeedOperatorFieldGetBasis(op_input_fields[i], &basis_i));
    for (CeedInt j = 0; (input_matrix_reuse[i].index == -1) && (j < i); j++) {
      CeedEvalMode eval_mode_j;
      CeedBasis    basis_j;

      CeedCallBackend(CeedQFunctionFieldGetEvalMode(qf_input_fields[j], &eval_mode_j));
      if (eval_mode_j == CEED_EVAL_WEIGHT) continue;
      CeedCallBackend(CeedOperatorFieldGetBasis(op_input_fields[j], &basis_j));
      if (basis_i == basis_j) {
        input_matrix_reuse[i].index     = j;
        input_matrix_reuse[i].is_input  = true;
        input_matrix_reuse[i].eval_mode = eval_mode_j;
      }
      CeedCallBackend(CeedBasisDestroy(&basis_j));
    }
    CeedCallBackend(CeedBasisDestroy(&basis_i));
  }

  // -- Determine output mat reuse
  FieldReuse_Hip output_matrix_reuse[CEED_FIELD_MAX];

  for (CeedInt i = 0; i < num_output_fields; i++) {
    output_matrix_reuse[i].index = -1;
  }
  for (CeedInt i = 0; i < num_output_fields; i++) {
    CeedEvalMode eval_mode_i;
    CeedBasis    basis_i;

    CeedCallBackend(CeedQFunctionFieldGetEvalMode(qf_output_fields[i], &eval_mode_i));
    CeedCallBackend(CeedOperatorFieldGetBasis(op_output_fields[i], &basis_i));
    for (CeedInt j = 0; (output_matrix_reuse[i].index == -1) && (j < num_input_fields); j++) {
      CeedEvalMode eval_mode_j;
      CeedBasis    basis_j;

      CeedCallBackend(CeedQFunctionFieldGetEvalMode(qf_input_fields[j], &eval_mode_j));
      if (eval_mode_j == CEED_EVAL_WEIGHT) continue;
      CeedCallBackend(CeedOperatorFieldGetBasis(op_input_fields[j], &basis_j));
      if (basis_i == basis_j) {
        output_matrix_reuse[i].index     = j;
        output_matrix_reuse[i].is_input  = true;
        output_matrix_reuse[i].eval_mode = eval_mode_j;
      }
      CeedCallBackend(CeedBasisDestroy(&basis_j));
    }
    for (CeedInt j = 0; (output_matrix_reuse[i].index == -1) && (j < i); j++) {
      CeedEvalMode eval_mode_j;
      CeedBasis    basis_j;

      CeedCallBackend(CeedQFunctionFieldGetEvalMode(qf_output_fields[j], &eval_mode_j));
      if (eval_mode_j == CEED_EVAL_WEIGHT) continue;
      CeedCallBackend(CeedOperatorFieldGetBasis(op_output_fields[j], &basis_j));
      if (basis_i == basis_j) {
        output_matrix_reuse[i].index     = j;
        output_matrix_reuse[i].is_input  = false;
        output_matrix_reuse[i].eval_mode = eval_mode_j;
      }
      CeedCallBackend(CeedBasisDestroy(&basis_j));
    }
    CeedCallBackend(CeedBasisDestroy(&basis_i));
  }

  // Initialize constants, and matrices B and G
  code << "\n" << tab << "// Input field constants and basis data\n";
  for (CeedInt i = 0; i < num_input_fields; i++) {
    CeedCallBackend(CeedOperatorBuildKernelFieldData_Hip_gen(code, data, tab, i, op_input_fields[i], qf_input_fields[i], input_matrix_reuse[i],
                                                             max_dim, Q, Q_1d, true, is_all_tensor, is_at_points, use_3d_slices));
  }
  code << "\n" << tab << "// Output field constants and basis data\n";
  for (CeedInt i = 0; i < num_output_fields; i++) {
    CeedCallBackend(CeedOperatorBuildKernelFieldData_Hip_gen(code, data, tab, i, op_output_fields[i], qf_output_fields[i], output_matrix_reuse[i],
                                                             max_dim, Q, Q_1d, false, is_all_tensor, is_at_points, use_3d_slices));
  }

  // Loop over all elements
  code << "\n" << tab << "// Element loop\n";
  code << tab << "__syncthreads();\n";
  code << tab << "for (CeedInt elem = blockIdx.x*blockDim.z + threadIdx.z; elem < num_elem; elem += gridDim.x*blockDim.z) {\n";
  tab.push();

  // -- Compute minimum buffer space needed
  CeedInt max_rstr_buffer_size = 1;

  for (CeedInt i = 0; i < num_input_fields; i++) {
    CeedEvalMode eval_mode;

    CeedCallBackend(CeedQFunctionFieldGetEvalMode(qf_input_fields[i], &eval_mode));
    if (eval_mode != CEED_EVAL_NONE && eval_mode != CEED_EVAL_WEIGHT) {
      CeedInt             num_comp;
      CeedElemRestriction elem_rstr;

      CeedCallBackend(CeedOperatorFieldGetElemRestriction(op_input_fields[i], &elem_rstr));
      CeedCallBackend(CeedElemRestrictionGetNumComponents(elem_rstr, &num_comp));
      max_rstr_buffer_size = CeedIntMax(max_rstr_buffer_size, num_comp * (is_all_tensor && (max_dim >= 3) ? Q_1d : 1));
      CeedCallBackend(CeedElemRestrictionDestroy(&elem_rstr));
    }
  }
  for (CeedInt i = 0; i < num_output_fields; i++) {
    CeedEvalMode eval_mode;

    CeedCallBackend(CeedQFunctionFieldGetEvalMode(qf_output_fields[i], &eval_mode));
    if (eval_mode != CEED_EVAL_NONE) {
      CeedInt             num_comp;
      CeedElemRestriction elem_rstr;

      CeedCallBackend(CeedOperatorFieldGetElemRestriction(op_output_fields[i], &elem_rstr));
      CeedCallBackend(CeedElemRestrictionGetNumComponents(elem_rstr, &num_comp));
      max_rstr_buffer_size = CeedIntMax(max_rstr_buffer_size, num_comp * (is_all_tensor && (max_dim >= 3) ? Q_1d : 1));
      CeedCallBackend(CeedElemRestrictionDestroy(&elem_rstr));
    }
  }
  code << tab << "// Scratch restriction buffer space\n";
  code << tab << "CeedScalar r_e_scratch[" << max_rstr_buffer_size << "];\n";

  // -- Determine best input field processing order
  CeedInt field_rstr_in_buffer[CEED_FIELD_MAX], input_field_order[CEED_FIELD_MAX];

  for (CeedInt i = 0; i < num_input_fields; i++) {
    field_rstr_in_buffer[i] = -1;
    input_field_order[i]    = -1;
  }
  {
    bool    is_ordered[CEED_FIELD_MAX];
    CeedInt curr_index = 0;

    for (CeedInt i = 0; i < num_input_fields; i++) is_ordered[i] = false;
    for (CeedInt i = 0; i < num_input_fields; i++) {
      CeedVector          vec_i;
      CeedElemRestriction rstr_i;

      if (is_ordered[i]) continue;
      field_rstr_in_buffer[i]       = i;
      is_ordered[i]                 = true;
      input_field_order[curr_index] = i;
      curr_index++;
      CeedCallBackend(CeedOperatorFieldGetVector(op_input_fields[i], &vec_i));
      if (vec_i == CEED_VECTOR_NONE) continue;  // CEED_EVAL_WEIGHT
      CeedCallBackend(CeedOperatorFieldGetElemRestriction(op_input_fields[i], &rstr_i));
      for (CeedInt j = i + 1; j < num_input_fields; j++) {
        CeedVector          vec_j;
        CeedElemRestriction rstr_j;

        CeedCallBackend(CeedOperatorFieldGetVector(op_input_fields[j], &vec_j));
        CeedCallBackend(CeedOperatorFieldGetElemRestriction(op_input_fields[j], &rstr_j));
        if (rstr_i == rstr_j && vec_i == vec_j) {
          field_rstr_in_buffer[j]       = i;
          is_ordered[j]                 = true;
          input_field_order[curr_index] = j;
          curr_index++;
        }
        CeedCallBackend(CeedVectorDestroy(&vec_j));
        CeedCallBackend(CeedElemRestrictionDestroy(&rstr_j));
      }
      CeedCallBackend(CeedVectorDestroy(&vec_i));
      CeedCallBackend(CeedElemRestrictionDestroy(&rstr_i));
    }
  }

  // -- Input restriction and basis
  code << "\n" << tab << "// -- Input field restrictions and basis actions\n";
  CeedInt active_field_index = -1;

  for (CeedInt i = 0; i < num_input_fields; i++) {
    bool          is_active = false;
    const char   *field_name;
    const CeedInt f = input_field_order[i];

    {
      CeedVector vec;

      CeedCallBackend(CeedOperatorFieldGetVector(op_input_fields[f], &vec));
      is_active = vec == CEED_VECTOR_ACTIVE;
      CeedCallBackend(CeedVectorDestroy(&vec));
    }

    CeedCallBackend(CeedOperatorFieldGetName(op_input_fields[f], &field_name));
    code << tab << "// ---- Input field " << f << ": " << field_name << "\n";

    if (is_active) {
      std::string var_suffix = "_in_" + std::to_string(f);

      code << tab << "// Active field - no restriction or basis action here\n";
      if (active_field_index == -1) {
        active_field_index = f;
        code << tab << "CeedScalar r_e" << var_suffix << "[num_comp" << var_suffix << "*" << (max_dim >= 3 ? "P_1d" + var_suffix : "1")
             << "] = {0.0};\n";
      } else {
        code << tab << "CeedScalar *r_e" << var_suffix << " = r_e_in_" << active_field_index << ";\n";
      }
    } else {
      // ---- Restriction
      CeedCallBackend(CeedOperatorBuildKernelRestriction_Hip_gen(code, data, tab, f, field_rstr_in_buffer, op_input_fields[f], qf_input_fields[f],
                                                                 max_dim, Q_1d, true, is_all_tensor, is_at_points, use_3d_slices));

      // ---- Basis action
      CeedCallBackend(CeedOperatorBuildKernelBasis_Hip_gen(code, data, tab, f, op_input_fields[f], qf_input_fields[f], max_dim, Q_1d, true,
                                                           is_all_tensor, is_at_points, use_3d_slices));
    }
  }

  // -- Loop over active field
  std::string active_var_suffix = "_in_" + std::to_string(active_field_index);

  code << "\n" << tab << "// Loop over nodes in active field\n";
  code << tab << "for (CeedInt n = 0; n < num_comp" << active_var_suffix << "*P_1d" << active_var_suffix
       << (max_dim > 1 ? "*P_1d" + active_var_suffix : "") << (max_dim > 2 ? "*P_1d" + active_var_suffix : "") << "; n++) {\n";
  tab.push();

  // -- Set current active node and component to 1
  code << tab << "// Set current active node and component to 1.0\n";
  code << tab << "SetEVecStandard" << max_dim << "d_Single<num_comp" << active_var_suffix << ", P_1d" << active_var_suffix << ">(data, n, 1.0, r_e"
       << active_var_suffix << ");\n\n";

  for (CeedInt i = 0; i < num_input_fields; i++) {
    bool          is_active = false;
    const char   *field_name;
    const CeedInt f = input_field_order[i];

    {
      CeedVector vec;

      CeedCallBackend(CeedOperatorFieldGetVector(op_input_fields[f], &vec));
      is_active = vec == CEED_VECTOR_ACTIVE;
      CeedCallBackend(CeedVectorDestroy(&vec));
    }
    if (!is_active) continue;

    CeedCallBackend(CeedOperatorFieldGetName(op_input_fields[f], &field_name));
    code << tab << "// ---- Input field " << f << ": " << field_name << "\n";

    // ---- Basis action
    CeedCallBackend(CeedOperatorBuildKernelBasis_Hip_gen(code, data, tab, f, op_input_fields[f], qf_input_fields[f], max_dim, Q_1d, true,
                                                         is_all_tensor, is_at_points, use_3d_slices));
  }

  // -- Q function
  CeedCallBackend(CeedOperatorBuildKernelQFunction_Hip_gen(code, data, tab, max_dim, max_num_points, num_input_fields, op_input_fields,
                                                           qf_input_fields, num_output_fields, op_output_fields, qf_output_fields, qfunction_name,
                                                           Q_1d, is_all_tensor, is_at_points, use_3d_slices));

  // -- Output basis and restriction
  code << "\n" << tab << "// -- Output field basis action and restrictions\n";
  for (CeedInt i = 0; i < num_output_fields; i++) {
    bool        is_active = false;
    const char *field_name;

    {
      CeedVector vec;

      CeedCallBackend(CeedOperatorFieldGetVector(op_output_fields[i], &vec));
      is_active = vec == CEED_VECTOR_ACTIVE;
      CeedCallBackend(CeedVectorDestroy(&vec));
    }
    if (!is_active) continue;

    CeedCallBackend(CeedOperatorFieldGetName(op_output_fields[i], &field_name));
    code << tab << "// ---- Output field " << i << ": " << field_name << "\n";

    // ---- Basis action
    CeedCallBackend(CeedOperatorBuildKernelBasis_Hip_gen(code, data, tab, i, op_output_fields[i], qf_output_fields[i], max_dim, Q_1d, false,
                                                         is_all_tensor, is_at_points, use_3d_slices));

    // ---- Restriction
    if (is_full) {
      std::string         var_suffix = "_out_" + std::to_string(i);
      CeedInt             comp_stride;
      CeedSize            l_size;
      CeedElemRestriction elem_rstr;

      CeedCallBackend(CeedOperatorFieldGetElemRestriction(op_output_fields[i], &elem_rstr));
      CeedCallBackend(CeedElemRestrictionGetLVectorSize(elem_rstr, &l_size));
      code << tab << "const CeedInt l_size" << var_suffix << " = " << l_size << ";\n";
      CeedCallBackend(CeedElemRestrictionGetCompStride(elem_rstr, &comp_stride));
      code << tab << "const CeedInt comp_stride" << var_suffix << " = " << comp_stride << ";\n";
      code << tab << "WriteLVecStandard" << max_dim << "d_Assembly<num_comp" << var_suffix << ", comp_stride" << var_suffix << ", P_1d" + var_suffix
           << ">(data, l_size" << var_suffix << ", elem, n, r_e" << var_suffix << ", values_array);\n";
      CeedCallBackend(CeedElemRestrictionDestroy(&elem_rstr));
    } else {
      std::string         var_suffix = "_out_" + std::to_string(i);
      CeedInt             comp_stride;
      CeedSize            l_size;
      CeedElemRestriction elem_rstr;

      CeedCallBackend(CeedOperatorFieldGetElemRestriction(op_output_fields[i], &elem_rstr));
      CeedCallBackend(CeedElemRestrictionGetLVectorSize(elem_rstr, &l_size));
      code << tab << "const CeedInt l_size" << var_suffix << " = " << l_size << ";\n";
      CeedCallBackend(CeedElemRestrictionGetCompStride(elem_rstr, &comp_stride));
      code << tab << "const CeedInt comp_stride" << var_suffix << " = " << comp_stride << ";\n";
      code << tab << "WriteLVecStandard" << max_dim << "d_Single<num_comp" << var_suffix << ", comp_stride" << var_suffix << ", P_1d" + var_suffix
           << ">(data, l_size" << var_suffix << ", elem, n, indices.outputs[" << i << "], r_e" << var_suffix << ", values_array);\n";
      CeedCallBackend(CeedElemRestrictionDestroy(&elem_rstr));
    }
  }

  // -- Reset current active node and component
  code << "\n" << tab << "// Reset current active node and component to 0.0\n";
  code << tab << "SetEVecStandard" << max_dim << "d_Single<num_comp" << active_var_suffix << ", P_1d" << active_var_suffix << ">(data, n, 0.0, r_e"
       << active_var_suffix << ");\n";

  // -- End of loop over active field
  tab.pop();
  code << tab << "}\n";

  // Close loop and function
  tab.pop();
  code << tab << "}\n";
  tab.pop();
  code << tab << "}\n";
  code << tab << "// -----------------------------------------------------------------------------\n\n";

  CeedInt block_sizes[3] = {0, 0, 0};
  CeedInt num_elem;

  // Compile
  CeedCallBackend(CeedOperatorGetNumElements(op, &num_elem));
  CeedCallBackend(BlockGridCalculate_Hip_gen(max_dim, num_elem, data->max_P_1d, Q_1d, block_sizes));
  {
    bool is_compile_good = false;

    data->thread_1d = block_sizes[0];
    CeedCallBackend(CeedTryCompile_Hip(ceed, code.str().c_str(), &is_compile_good,
                                       is_full ? &data->module_assemble_full : &data->module_assemble_diagonal, 2, "OP_T_1D", block_sizes[0],
                                       "BLOCK_SIZE", block_sizes[0] * block_sizes[1] * block_sizes[2]));
    if (is_compile_good) {
      *is_good_build = true;
      CeedCallBackend(CeedGetKernel_Hip(ceed, is_full ? data->module_assemble_full : data->module_assemble_diagonal, operator_name.c_str(),
                                        is_full ? &data->assemble_full : &data->assemble_diagonal));
    } else {
      *is_good_build              = false;
      data->use_assembly_fallback = true;
    }
  }
  CeedCallBackend(CeedDestroy(&ceed));
  CeedCallBackend(CeedQFunctionDestroy(&qf));
  return CEED_ERROR_SUCCESS;
}

extern "C" int CeedOperatorBuildKernelDiagonalAssemblyAtPoints_Hip_gen(CeedOperator op, bool *is_good_build) {
  return CeedOperatorBuildKernelAssemblyAtPoints_Hip_gen(op, false, is_good_build);
}

extern "C" int CeedOperatorBuildKernelFullAssemblyAtPoints_Hip_gen(CeedOperator op, bool *is_good_build) {
  return CeedOperatorBuildKernelAssemblyAtPoints_Hip_gen(op, true, is_good_build);
}
//------------------------------------------------------------------------------
// Build QFunction assembly operator kernel
//------------------------------------------------------------------------------
extern "C" int CeedOperatorBuildKernelLinearAssembleQFunction_Hip_gen(CeedOperator op, bool *is_good_build) {
  bool                   is_all_tensor = true, is_all_nontensor = true, is_at_points = false, use_3d_slices = false;
  Ceed                   ceed;
  CeedInt                Q, Q_1d, num_input_fields, num_output_fields, max_dim = 1, max_num_points = 0;
  CeedQFunctionField    *qf_input_fields, *qf_output_fields;
  CeedQFunction_Hip_gen *qf_data;
  CeedQFunction          qf;
  CeedOperatorField     *op_input_fields, *op_output_fields;
  CeedOperator_Hip_gen  *data;
  std::ostringstream     code;
  Tab                    tab;

  // Check compatibility
  CeedCallBackend(CeedOperatorGetCeed(op, &ceed));
  CeedCallBackend(CeedOperatorIsAtPoints(op, &is_at_points));
  CeedCheck(!is_at_points, ceed, CEED_ERROR_BACKEND, "AtPoints QFunction assembly is not supported");

  // Check field compatibility
  CeedCallBackend(CeedOperatorGetFields(op, &num_input_fields, &op_input_fields, &num_output_fields, &op_output_fields));
  {
    bool has_shared_bases = true;

    for (CeedInt i = 0; i < num_input_fields; i++) {
      CeedBasis basis;

      CeedCallBackend(CeedOperatorFieldGetBasis(op_input_fields[i], &basis));
      if (basis != CEED_BASIS_NONE) {
        bool        is_tensor = true;
        const char *resource;
        char       *resource_root;
        Ceed        basis_ceed;

        CeedCallBackend(CeedBasisIsTensor(basis, &is_tensor));
        is_all_tensor    = is_all_tensor && is_tensor;
        is_all_nontensor = is_all_nontensor && !is_tensor;
        CeedCallBackend(CeedBasisGetCeed(basis, &basis_ceed));
        CeedCallBackend(CeedGetResource(basis_ceed, &resource));
        CeedCallBackend(CeedGetResourceRoot(basis_ceed, resource, ":", &resource_root));
        has_shared_bases = has_shared_bases && !strcmp(resource_root, "/gpu/hip/shared");
        CeedCallBackend(CeedFree(&resource_root));
        CeedCallBackend(CeedDestroy(&basis_ceed));
      }
      CeedCallBackend(CeedBasisDestroy(&basis));
    }

    for (CeedInt i = 0; i < num_output_fields; i++) {
      CeedBasis basis;

      CeedCallBackend(CeedOperatorFieldGetBasis(op_output_fields[i], &basis));
      if (basis != CEED_BASIS_NONE) {
        bool        is_tensor = true;
        const char *resource;
        char       *resource_root;
        Ceed        basis_ceed;

        CeedCallBackend(CeedBasisIsTensor(basis, &is_tensor));
        is_all_tensor    = is_all_tensor && is_tensor;
        is_all_nontensor = is_all_nontensor && !is_tensor;

        CeedCallBackend(CeedBasisGetCeed(basis, &basis_ceed));
        CeedCallBackend(CeedGetResource(basis_ceed, &resource));
        CeedCallBackend(CeedGetResourceRoot(basis_ceed, resource, ":", &resource_root));
        has_shared_bases = has_shared_bases && !strcmp(resource_root, "/gpu/hip/shared");
        CeedCallBackend(CeedFree(&resource_root));
        CeedCallBackend(CeedDestroy(&basis_ceed));
      }
      CeedCallBackend(CeedBasisDestroy(&basis));
    }
  }

  // Retrieve operator data
  CeedCallBackend(CeedOperatorGetData(op, &data));
  Q       = data->Q;
  Q_1d    = data->Q_1d;
  max_dim = data->dim;
  CeedCallBackend(CeedOperatorGetQFunction(op, &qf));
  CeedCallBackend(CeedQFunctionGetData(qf, &qf_data));
  CeedCallBackend(CeedQFunctionGetFields(qf, NULL, &qf_input_fields, NULL, &qf_output_fields));

  // Load basis source files
  if (!is_all_nontensor) {
    code << tab << "// Tensor basis source\n";
    code << tab << "#include <ceed/jit-source/hip/hip-shared-basis-tensor-templates.h>\n\n";
  }
  if (!is_all_tensor) {
    code << tab << "// Non-tensor basis source\n";
    code << tab << "#include <ceed/jit-source/hip/hip-shared-basis-nontensor-templates.h>\n\n";
  }
  if (!is_all_tensor && !is_all_nontensor) {
    code << "// Tensor basis source\n";
    code << "#include <ceed/jit-source/hip/hip-shared-basis-tensor-flattened-templates.h>\n\n";
  }
  code << "// CodeGen operator source\n";
  code << "#include <ceed/jit-source/hip/hip-gen-templates.h>\n\n";

  // Get QFunction name
  std::string qfunction_name(qf_data->qfunction_name);
  std::string operator_name;

  operator_name = "CeedKernelHipGenQFunctionAssembly_" + qfunction_name;

  // Define CEED_Q_VLA
  code << "\n" << tab << "#undef CEED_Q_VLA\n";
  if (max_dim != 3 || is_at_points || use_3d_slices || !is_all_tensor) {
    code << tab << "#define CEED_Q_VLA 1\n\n";
  } else {
    code << tab << "#define CEED_Q_VLA " << Q_1d << "\n\n";
  }

  // Add user QFunction source
  {
    const char *source_path;

    CeedCallBackend(CeedQFunctionGetSourcePath(qf, &source_path));
    CeedCheck(source_path, ceed, CEED_ERROR_UNSUPPORTED, "/gpu/hip/gen backend requires QFunction source code file");

    code << tab << "// User QFunction source\n";
    code << tab << "#include \"" << source_path << "\"\n\n";
  }

  // Setup
  code << "\n" << tab << "// -----------------------------------------------------------------------------\n";
  code << tab << "// Operator Assembly Kernel\n";
  code << tab << "// \n";
  code << tab << "// d_[in,out]_i:   CeedVector device array\n";
  code << tab << "// r_[in,out]_e_i: Element vector register\n";
  code << tab << "// r_[in,out]_q_i: Quadrature space vector register\n";
  code << tab << "// r_[in,out]_c_i: AtPoints Chebyshev coefficients register\n";
  code << tab << "// r_[in,out]_s_i: Quadrature space slice vector register\n";
  code << tab << "// \n";
  code << tab << "// s_B_[in,out]_i: Interpolation matrix, shared memory\n";
  code << tab << "// s_G_[in,out]_i: Gradient matrix, shared memory\n";
  code << tab << "// -----------------------------------------------------------------------------\n";
  code << tab << "extern \"C\" __global__ void " << operator_name
       << "(CeedInt num_elem, void* ctx, FieldsInt_Hip indices, Fields_Hip fields, Fields_Hip B, Fields_Hip G, CeedScalar *W, Points_Hip "
          "points, CeedScalar *__restrict__ values_array) {\n";
  tab.push();

  // Scratch buffers
  for (CeedInt i = 0; i < num_input_fields; i++) {
    CeedEvalMode eval_mode;

    CeedCallBackend(CeedQFunctionFieldGetEvalMode(qf_input_fields[i], &eval_mode));
    if (eval_mode != CEED_EVAL_WEIGHT) {  // Skip CEED_EVAL_WEIGHT
      code << tab << "const CeedScalar *__restrict__ d_in_" << i << " = fields.inputs[" << i << "];\n";
    }
  }
  for (CeedInt i = 0; i < num_output_fields; i++) {
    code << tab << "CeedScalar *__restrict__ d_out_" << i << " = fields.outputs[" << i << "];\n";
  }

  code << tab << "const CeedInt max_dim = " << max_dim << ";\n";
  if (!is_all_tensor) {
    code << tab << "const CeedInt Q = " << Q << ";\n";
  }
  if (!is_all_nontensor) {
    code << tab << "const CeedInt Q_1d = " << Q_1d << ";\n";
  }

  // Shared data
  code << tab << "extern __shared__ CeedScalar slice[];\n";
  code << tab << "SharedData_Hip data;\n";
  code << tab << "data.t_id_x = threadIdx.x;\n";
  code << tab << "data.t_id_y = threadIdx.y;\n";
  code << tab << "data.t_id_z = threadIdx.z;\n";
  code << tab << "data.t_id   = threadIdx.x + threadIdx.y*blockDim.x + threadIdx.z*blockDim.y*blockDim.x;\n";
  code << tab << "data.slice  = slice + data.t_id_z*OP_T_1D" << ((!is_all_tensor || max_dim == 1) ? "" : "*OP_T_1D") << ";\n";

  // -- Determine input mat reuse
  FieldReuse_Hip input_matrix_reuse[CEED_FIELD_MAX];

  for (CeedInt i = 0; i < num_input_fields; i++) {
    input_matrix_reuse[i].index = -1;
  }
  for (CeedInt i = 0; i < num_input_fields; i++) {
    bool         is_tensor = true;
    CeedEvalMode eval_mode_i;
    CeedBasis    basis_i;

    CeedCallBackend(CeedQFunctionFieldGetEvalMode(qf_input_fields[i], &eval_mode_i));
    if (eval_mode_i == CEED_EVAL_WEIGHT) continue;
    CeedCallBackend(CeedOperatorFieldGetBasis(op_input_fields[i], &basis_i));
    CeedCallBackend(CeedBasisIsTensor(basis_i, &is_tensor));
    for (CeedInt j = 0; (input_matrix_reuse[i].index == -1) && (j < i); j++) {
      CeedEvalMode eval_mode_j;
      CeedBasis    basis_j;

      CeedCallBackend(CeedQFunctionFieldGetEvalMode(qf_input_fields[j], &eval_mode_j));
      if (eval_mode_j == CEED_EVAL_WEIGHT) continue;
      CeedCallBackend(CeedOperatorFieldGetBasis(op_input_fields[j], &basis_j));
      if (basis_i == basis_j) {
        if (is_tensor) {
          input_matrix_reuse[i].index     = j;
          input_matrix_reuse[i].is_input  = true;
          input_matrix_reuse[i].eval_mode = eval_mode_j;
        } else {
          // For non-tensor can only re-use with the same eval mode
          if (eval_mode_i == eval_mode_j) {
            input_matrix_reuse[i].index     = j;
            input_matrix_reuse[i].is_input  = true;
            input_matrix_reuse[i].eval_mode = eval_mode_j;
          }
        }
      }
      CeedCallBackend(CeedBasisDestroy(&basis_j));
    }
    CeedCallBackend(CeedBasisDestroy(&basis_i));
  }

  // -- Determine output mat reuse
  FieldReuse_Hip output_matrix_reuse[CEED_FIELD_MAX];

  for (CeedInt i = 0; i < num_output_fields; i++) {
    output_matrix_reuse[i].index = -1;
  }
  for (CeedInt i = 0; i < num_output_fields; i++) {
    bool         is_tensor = true;
    CeedEvalMode eval_mode_i;
    CeedBasis    basis_i;

    CeedCallBackend(CeedQFunctionFieldGetEvalMode(qf_output_fields[i], &eval_mode_i));
    CeedCallBackend(CeedOperatorFieldGetBasis(op_output_fields[i], &basis_i));
    CeedCallBackend(CeedBasisIsTensor(basis_i, &is_tensor));
    for (CeedInt j = 0; (output_matrix_reuse[i].index == -1) && (j < num_input_fields); j++) {
      CeedEvalMode eval_mode_j;
      CeedBasis    basis_j;

      CeedCallBackend(CeedQFunctionFieldGetEvalMode(qf_input_fields[j], &eval_mode_j));
      if (eval_mode_j == CEED_EVAL_WEIGHT) continue;
      CeedCallBackend(CeedOperatorFieldGetBasis(op_input_fields[j], &basis_j));
      if (basis_i == basis_j) {
        if (is_tensor) {
          output_matrix_reuse[i].index     = j;
          output_matrix_reuse[i].is_input  = true;
          output_matrix_reuse[i].eval_mode = eval_mode_j;
        } else {
          // For non-tensor can only re-use with the same eval mode
          if (eval_mode_i == eval_mode_j) {
            output_matrix_reuse[i].index     = j;
            output_matrix_reuse[i].is_input  = true;
            output_matrix_reuse[i].eval_mode = eval_mode_j;
          }
        }
      }
      CeedCallBackend(CeedBasisDestroy(&basis_j));
    }
    for (CeedInt j = 0; (output_matrix_reuse[i].index == -1) && (j < i); j++) {
      CeedEvalMode eval_mode_j;
      CeedBasis    basis_j;

      CeedCallBackend(CeedQFunctionFieldGetEvalMode(qf_output_fields[j], &eval_mode_j));
      if (eval_mode_j == CEED_EVAL_WEIGHT) continue;
      CeedCallBackend(CeedOperatorFieldGetBasis(op_output_fields[j], &basis_j));
      if (basis_i == basis_j) {
        if (is_tensor) {
          output_matrix_reuse[i].index     = j;
          output_matrix_reuse[i].is_input  = false;
          output_matrix_reuse[i].eval_mode = eval_mode_j;
        } else {
          // For non-tensor can only re-use with the same eval mode
          if (eval_mode_i == eval_mode_j) {
            output_matrix_reuse[i].index     = j;
            output_matrix_reuse[i].is_input  = false;
            output_matrix_reuse[i].eval_mode = eval_mode_j;
          }
        }
      }
      CeedCallBackend(CeedBasisDestroy(&basis_j));
    }
    CeedCallBackend(CeedBasisDestroy(&basis_i));
  }

  // Initialize constants, and matrices B and G
  code << "\n" << tab << "// Input field constants and basis data\n";
  for (CeedInt i = 0; i < num_input_fields; i++) {
    CeedCallBackend(CeedOperatorBuildKernelFieldData_Hip_gen(code, data, tab, i, op_input_fields[i], qf_input_fields[i], input_matrix_reuse[i],
                                                             max_dim, Q, Q_1d, true, is_all_tensor, is_at_points, use_3d_slices));
  }
  code << "\n" << tab << "// Output field constants and basis data\n";
  for (CeedInt i = 0; i < num_output_fields; i++) {
    CeedCallBackend(CeedOperatorBuildKernelFieldData_Hip_gen(code, data, tab, i, op_output_fields[i], qf_output_fields[i], output_matrix_reuse[i],
                                                             max_dim, Q, Q_1d, false, is_all_tensor, is_at_points, use_3d_slices));
  }

  // Loop over all elements
  code << "\n" << tab << "// Element loop\n";
  code << tab << "__syncthreads();\n";
  code << tab << "for (CeedInt elem = blockIdx.x*blockDim.z + threadIdx.z; elem < num_elem; elem += gridDim.x*blockDim.z) {\n";
  tab.push();

  // -- Compute minimum buffer space needed
  CeedInt max_rstr_buffer_size = 1;

  for (CeedInt i = 0; i < num_input_fields; i++) {
    CeedEvalMode eval_mode;

    CeedCallBackend(CeedQFunctionFieldGetEvalMode(qf_input_fields[i], &eval_mode));
    if (eval_mode != CEED_EVAL_NONE && eval_mode != CEED_EVAL_WEIGHT) {
      CeedInt             num_comp;
      CeedElemRestriction elem_rstr;

      CeedCallBackend(CeedOperatorFieldGetElemRestriction(op_input_fields[i], &elem_rstr));
      CeedCallBackend(CeedElemRestrictionGetNumComponents(elem_rstr, &num_comp));
      max_rstr_buffer_size = CeedIntMax(max_rstr_buffer_size, num_comp * (is_all_tensor && (max_dim >= 3) ? Q_1d : 1));
      CeedCallBackend(CeedElemRestrictionDestroy(&elem_rstr));
    }
  }
  for (CeedInt i = 0; i < num_output_fields; i++) {
    CeedEvalMode eval_mode;

    CeedCallBackend(CeedQFunctionFieldGetEvalMode(qf_output_fields[i], &eval_mode));
    if (eval_mode != CEED_EVAL_NONE) {
      CeedInt             num_comp;
      CeedElemRestriction elem_rstr;

      CeedCallBackend(CeedOperatorFieldGetElemRestriction(op_output_fields[i], &elem_rstr));
      CeedCallBackend(CeedElemRestrictionGetNumComponents(elem_rstr, &num_comp));
      max_rstr_buffer_size = CeedIntMax(max_rstr_buffer_size, num_comp * (is_all_tensor && (max_dim >= 3) ? Q_1d : 1));
      CeedCallBackend(CeedElemRestrictionDestroy(&elem_rstr));
    }
  }
  code << tab << "// Scratch restriction buffer space\n";
  code << tab << "CeedScalar r_e_scratch[" << max_rstr_buffer_size << "];\n";

  // -- Determine best input field processing order
  CeedInt field_rstr_in_buffer[CEED_FIELD_MAX], input_field_order[CEED_FIELD_MAX];

  for (CeedInt i = 0; i < num_input_fields; i++) {
    field_rstr_in_buffer[i] = -1;
    input_field_order[i]    = -1;
  }
  {
    bool    is_ordered[CEED_FIELD_MAX];
    CeedInt curr_index = 0;

    for (CeedInt i = 0; i < num_input_fields; i++) is_ordered[i] = false;
    for (CeedInt i = 0; i < num_input_fields; i++) {
      CeedVector          vec_i;
      CeedElemRestriction rstr_i;

      if (is_ordered[i]) continue;
      field_rstr_in_buffer[i]       = i;
      is_ordered[i]                 = true;
      input_field_order[curr_index] = i;
      curr_index++;
      CeedCallBackend(CeedOperatorFieldGetVector(op_input_fields[i], &vec_i));
      if (vec_i == CEED_VECTOR_NONE) continue;  // CEED_EVAL_WEIGHT
      CeedCallBackend(CeedOperatorFieldGetElemRestriction(op_input_fields[i], &rstr_i));
      for (CeedInt j = i + 1; j < num_input_fields; j++) {
        CeedVector          vec_j;
        CeedElemRestriction rstr_j;

        CeedCallBackend(CeedOperatorFieldGetVector(op_input_fields[j], &vec_j));
        CeedCallBackend(CeedOperatorFieldGetElemRestriction(op_input_fields[j], &rstr_j));
        if (rstr_i == rstr_j && vec_i == vec_j) {
          field_rstr_in_buffer[j]       = i;
          is_ordered[j]                 = true;
          input_field_order[curr_index] = j;
          curr_index++;
        }
        CeedCallBackend(CeedVectorDestroy(&vec_j));
        CeedCallBackend(CeedElemRestrictionDestroy(&rstr_j));
      }
      CeedCallBackend(CeedVectorDestroy(&vec_i));
      CeedCallBackend(CeedElemRestrictionDestroy(&rstr_i));
    }
  }

  // -- Input restriction and basis
  code << "\n" << tab << "// -- Input field restrictions and basis actions\n";
  CeedInt num_active_in = 0, num_active_out = 0, qf_assembly_size_out = 0;
  CeedInt active_fields_in[CEED_FIELD_MAX], active_fields_out[CEED_FIELD_MAX];

  for (CeedInt i = 0; i < num_input_fields; i++) {
    bool          is_active = false;
    const char   *field_name;
    const CeedInt f = input_field_order[i];

    {
      CeedVector vec;

      CeedCallBackend(CeedOperatorFieldGetVector(op_input_fields[f], &vec));
      is_active = vec == CEED_VECTOR_ACTIVE;
      CeedCallBackend(CeedVectorDestroy(&vec));
    }

    CeedCallBackend(CeedOperatorFieldGetName(op_input_fields[f], &field_name));
    code << tab << "// ---- Input field " << f << ": " << field_name << "\n";

    if (is_active) {
      CeedEvalMode eval_mode;
      CeedInt      field_size;

      active_fields_in[num_active_in] = f;
      num_active_in++;
      CeedCallBackend(CeedQFunctionFieldGetSize(qf_input_fields[f], &field_size));
      CeedCallBackend(CeedQFunctionFieldGetEvalMode(qf_input_fields[f], &eval_mode));
      if (eval_mode == CEED_EVAL_GRAD) {
        code << tab << "CeedScalar r_q_in_" << f << "[num_comp_in_" << f << "*" << "dim_in_" << f << "*"
             << (is_all_tensor && (max_dim >= 3) ? "Q_1d" : "1") << "] = {0.};\n";
      } else {
        code << tab << "CeedScalar r_q_in_" << f << "[num_comp_in_" << f << "*" << (is_all_tensor && (max_dim >= 3) ? "Q_1d" : "1") << "] = {0.};\n";
      }
      code << tab << "const CeedInt field_size_in_" << f << " = " << field_size << ";\n";
    } else {
      // ---- Restriction
      CeedCallBackend(CeedOperatorBuildKernelRestriction_Hip_gen(code, data, tab, f, field_rstr_in_buffer, op_input_fields[f], qf_input_fields[f],
                                                                 max_dim, Q_1d, true, is_all_tensor, is_at_points, use_3d_slices));

      // ---- Basis action
      CeedCallBackend(CeedOperatorBuildKernelBasis_Hip_gen(code, data, tab, f, op_input_fields[f], qf_input_fields[f], max_dim, Q_1d, true,
                                                           is_all_tensor, is_at_points, use_3d_slices));
    }
  }
  code << tab << "const CeedInt field_sizes_in[" << num_active_in << "] = {";
  for (CeedInt i = 0; i < num_active_in; i++) {
    code << "field_size_in_" << active_fields_in[i] << (i < num_active_in - 1 ? ", " : "");
  }
  code << "};\n";
  code << tab << "CeedScalar * r_q_in[" << num_active_in << "] = {";
  for (CeedInt i = 0; i < num_active_in; i++) {
    code << "r_q_in_" << active_fields_in[i] << (i < num_active_in - 1 ? ", " : "");
  }
  code << "};\n";

  for (CeedInt i = 0; i < num_output_fields; i++) {
    bool is_active = false;

    {
      CeedVector vec;

      CeedCallBackend(CeedOperatorFieldGetVector(op_output_fields[i], &vec));
      is_active = vec == CEED_VECTOR_ACTIVE;
      CeedCallBackend(CeedVectorDestroy(&vec));
    }
    if (is_active) {
      const char *field_name;
      CeedInt     field_size;

      active_fields_out[num_active_out] = i;
      num_active_out++;
      CeedCallBackend(CeedQFunctionFieldGetSize(qf_output_fields[i], &field_size));
      qf_assembly_size_out += field_size;
      CeedCallBackend(CeedOperatorFieldGetName(op_output_fields[i], &field_name));
      code << tab << "// ---- Output field " << i << ": " << field_name << "\n";
      code << tab << "const CeedInt field_size_out_" << i << " = " << field_size << ";\n";
    }
  }
  code << tab << "const CeedInt field_sizes_out[" << num_active_out << "] = {";
  for (CeedInt i = 0; i < num_active_out; i++) {
    code << "field_size_out_" << active_fields_out[i] << (i < num_active_out - 1 ? ", " : "");
  }
  code << "};\n";
  code << tab << "const CeedInt total_size_out = " << qf_assembly_size_out << ";\n";

  // -- Loop over active field
  code << "\n" << tab << "CeedInt input_offset = 0;\n";
  code << tab << "// Loop over active QFunction input fields\n";
  code << tab << "const CeedInt num_active_in = " << num_active_in << ";\n";
  code << tab << "for (CeedInt a = 0; a < num_active_in; a++) {\n";
  tab.push();

  // -- Loop over size of active field
  code << "\n" << tab << "// Loop over current active input field size\n";
  code << tab << "const CeedInt field_size_in = field_sizes_in[a];\n";
  code << tab << "for (CeedInt s = 0; s < field_size_in; s++) {\n";
  tab.push();

  // -- Set current active point and component to 1
  code << tab << "// Set current active point and component to 1.0\n";
  if (is_all_tensor && (max_dim >= 3)) {
    code << tab << "for (CeedInt i = 0; i < Q_1d; i++) r_q_in[a][i + s * Q_1d] = 1.0;\n";
  } else {
    code << tab << "r_q_in[a][s] = 1.0;\n";
  }

  // -- Q function
  CeedCallBackend(CeedOperatorBuildKernelQFunction_Hip_gen(code, data, tab, max_dim, max_num_points, num_input_fields, op_input_fields,
                                                           qf_input_fields, num_output_fields, op_output_fields, qf_output_fields, qfunction_name,
                                                           Q_1d, is_all_tensor, is_at_points, use_3d_slices));

  // -- Output basis and restriction
  code << "\n" << tab << "// -- Output field basis action and restrictions\n";
  CeedScalar offset = 0;

  for (CeedInt i = 0; i < num_output_fields; i++) {
    bool        is_active = false;
    const char *field_name;

    {
      CeedVector vec;

      CeedCallBackend(CeedOperatorFieldGetVector(op_output_fields[i], &vec));
      is_active = vec == CEED_VECTOR_ACTIVE;
      CeedCallBackend(CeedVectorDestroy(&vec));
    }
    if (!is_active) continue;

    CeedCallBackend(CeedOperatorFieldGetName(op_output_fields[i], &field_name));
    code << tab << "// ---- Output field " << i << ": " << field_name << "\n";

    // ---- Restriction
    CeedInt field_size;

    code << tab << "WriteLVecStandard" << (is_all_tensor ? max_dim : 1) << "d_QFAssembly<total_size_out, field_size_out_" << i << ", "
         << (is_all_tensor ? "Q_1d" : "Q") << ">(data, num_elem, elem, input_offset + s, " << offset << ", r_q_out_" << i << ", values_array);\n";
    CeedCallBackend(CeedQFunctionFieldGetSize(qf_output_fields[i], &field_size));
    offset += field_size;
  }

  // -- Reset current active node and component
  code << "\n" << tab << "// Reset current active node and component to 0.0\n";
  if (is_all_tensor && (max_dim >= 3)) {
    code << tab << "for (CeedInt i = 0; i < Q_1d; i++) r_q_in[a][i + s * Q_1d] = 0.0;\n";
  } else {
    code << tab << "r_q_in[a][s] = 0.0;\n";
  }

  // -- End of loop over size of active field
  tab.pop();
  code << tab << "}\n";
  code << tab << "input_offset += field_size_in;\n";

  // -- End of loop over active field
  tab.pop();
  code << tab << "}\n";

  // Close loop and function
  tab.pop();
  code << tab << "}\n";
  tab.pop();
  code << tab << "}\n";
  code << tab << "// -----------------------------------------------------------------------------\n\n";

  CeedInt block_sizes[3] = {0, 0, 0};
  CeedInt num_elem;

  // Compile
  CeedCallBackend(CeedOperatorGetNumElements(op, &num_elem));
  CeedCallBackend(BlockGridCalculate_Hip_gen(max_dim, num_elem, data->max_P_1d, Q_1d, block_sizes));
  {
    bool is_compile_good = false;

    data->thread_1d = block_sizes[0];
    CeedCallBackend(CeedTryCompile_Hip(ceed, code.str().c_str(), &is_compile_good, &data->module_assemble_qfunction, 2, "OP_T_1D", block_sizes[0],
                                       "BLOCK_SIZE", block_sizes[0] * block_sizes[1] * block_sizes[2]));
    if (is_compile_good) {
      *is_good_build = true;
      CeedCallBackend(CeedGetKernel_Hip(ceed, data->module_assemble_qfunction, operator_name.c_str(), &data->assemble_qfunction));
    } else {
      *is_good_build              = false;
      data->use_assembly_fallback = true;
    }
  }
  CeedCallBackend(CeedDestroy(&ceed));
  CeedCallBackend(CeedQFunctionDestroy(&qf));
  return CEED_ERROR_SUCCESS;
}

//------------------------------------------------------------------------------
