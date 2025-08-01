// Copyright (c) 2017-2025, Lawrence Livermore National Security, LLC and other CEED contributors.
// All Rights Reserved. See the top-level LICENSE and NOTICE files for details.
//
// SPDX-License-Identifier: BSD-2-Clause
//
// This file is part of CEED:  http://github.com/ceed

#include <ceed.h>
#include <ceed/backend.h>
#include <ceed/jit-source/cuda/cuda-types.h>
#include <cuda.h>
#include <cuda_runtime.h>
#include <stddef.h>
#include <string.h>

#include "../cuda/ceed-cuda-common.h"
#include "../cuda/ceed-cuda-compile.h"
#include "ceed-cuda-gen-operator-build.h"
#include "ceed-cuda-gen.h"

//------------------------------------------------------------------------------
// Destroy operator
//------------------------------------------------------------------------------
static int CeedOperatorDestroy_Cuda_gen(CeedOperator op) {
  Ceed                   ceed;
  CeedOperator_Cuda_gen *impl;

  CeedCallBackend(CeedOperatorGetCeed(op, &ceed));
  CeedCallBackend(CeedOperatorGetData(op, &impl));
  if (impl->module) CeedCallCuda(ceed, cuModuleUnload(impl->module));
  if (impl->module_assemble_full) CeedCallCuda(ceed, cuModuleUnload(impl->module_assemble_full));
  if (impl->module_assemble_diagonal) CeedCallCuda(ceed, cuModuleUnload(impl->module_assemble_diagonal));
  if (impl->module_assemble_qfunction) CeedCallCuda(ceed, cuModuleUnload(impl->module_assemble_qfunction));
  if (impl->points.num_per_elem) CeedCallCuda(ceed, cudaFree((void **)impl->points.num_per_elem));
  CeedCallBackend(CeedFree(&impl));
  CeedCallBackend(CeedDestroy(&ceed));
  return CEED_ERROR_SUCCESS;
}

static int Waste(int threads_per_sm, int warp_size, int threads_per_elem, int elems_per_block) {
  int useful_threads_per_block = threads_per_elem * elems_per_block;
  // round up to nearest multiple of warp_size
  int block_size    = CeedDivUpInt(useful_threads_per_block, warp_size) * warp_size;
  int blocks_per_sm = threads_per_sm / block_size;
  return threads_per_sm - useful_threads_per_block * blocks_per_sm;
}

// Choose the least wasteful block size constrained by blocks_per_sm of max_threads_per_block.
//
// The x and y part of block[] contains per-element sizes (specified on input) while the z part is number of elements.
//
// Problem setting: we'd like to make occupancy high with relatively few inactive threads. CUDA (cuOccupancyMaxPotentialBlockSize) can tell us how
// many threads can run.
//
// Note that full occupancy sometimes can't be achieved by one thread block.
// For example, an SM might support 1536 threads in total, but only 1024 within a single thread block.
// So cuOccupancyMaxPotentialBlockSize may suggest a block size of 768 so that two blocks can run, versus one block of 1024 will prevent a second
// block from running. The cuda-gen kernels are pretty heavy with lots of instruction-level parallelism (ILP) so we'll generally be okay with
// relatively low occupancy and smaller thread blocks, but we solve a reasonably general problem here. Empirically, we find that blocks bigger than
// about 256 have higher latency and worse load balancing when the number of elements is modest.
//
// cuda-gen can't choose block sizes arbitrarily; they need to be a multiple of the number of quadrature points (or number of basis functions).
// They also have a lot of __syncthreads(), which is another point against excessively large thread blocks.
// Suppose I have elements with 7x7x7 quadrature points.
// This will loop over the last dimension, so we have 7*7=49 threads per element.
// Suppose we have two elements = 2*49=98 useful threads.
// CUDA schedules in units of full warps (32 threads), so 128 CUDA hardware threads are effectively committed to that block.
// Now suppose cuOccupancyMaxPotentialBlockSize returned 352.
// We can schedule 2 blocks of size 98 (196 useful threads using 256 hardware threads), but not a third block (which would need a total of 384
// hardware threads).
//
// If instead, we had packed 3 elements, we'd have 3*49=147 useful threads occupying 160 slots, and could schedule two blocks.
// Alternatively, we could pack a single block of 7 elements (2*49=343 useful threads) into the 354 slots.
// The latter has the least "waste", but __syncthreads() over-synchronizes and it might not pay off relative to smaller blocks.
static int BlockGridCalculate(CeedInt num_elem, int blocks_per_sm, int max_threads_per_block, int max_threads_z, int warp_size, int block[3],
                              int *grid) {
  const int threads_per_sm   = blocks_per_sm * max_threads_per_block;
  const int threads_per_elem = block[0] * block[1];
  int       elems_per_block  = 1;
  int       waste            = Waste(threads_per_sm, warp_size, threads_per_elem, 1);

  for (int i = 2; i <= CeedIntMin(max_threads_per_block / threads_per_elem, num_elem); i++) {
    int i_waste = Waste(threads_per_sm, warp_size, threads_per_elem, i);

    // We want to minimize waste, but smaller kernels have lower latency and less __syncthreads() overhead so when a larger block size has the same
    // waste as a smaller one, go ahead and prefer the smaller block.
    if (i_waste < waste || (i_waste == waste && threads_per_elem * i <= 128)) {
      elems_per_block = i;
      waste           = i_waste;
    }
  }
  // In low-order elements, threads_per_elem may be sufficiently low to give an elems_per_block greater than allowable for the device, so we must
  // check before setting the z-dimension size of the block.
  block[2] = CeedIntMin(elems_per_block, max_threads_z);
  *grid    = CeedDivUpInt(num_elem, elems_per_block);
  return CEED_ERROR_SUCCESS;
}

// callback for cuOccupancyMaxPotentialBlockSize, providing the amount of dynamic shared memory required for a thread block of size threads.
static size_t dynamicSMemSize(int threads) { return threads * sizeof(CeedScalar); }

//------------------------------------------------------------------------------
// Apply and add to output
//------------------------------------------------------------------------------
static int CeedOperatorApplyAddCore_Cuda_gen(CeedOperator op, CUstream stream, const CeedScalar *input_arr, CeedScalar *output_arr, bool *is_run_good,
                                             CeedRequest *request) {
  bool                    is_at_points, is_tensor;
  Ceed                    ceed;
  Ceed_Cuda              *cuda_data;
  CeedInt                 num_elem, num_input_fields, num_output_fields;
  CeedEvalMode            eval_mode;
  CeedQFunctionField     *qf_input_fields, *qf_output_fields;
  CeedQFunction_Cuda_gen *qf_data;
  CeedQFunction           qf;
  CeedOperatorField      *op_input_fields, *op_output_fields;
  CeedOperator_Cuda_gen  *data;

  // Build the operator kernel
  CeedCallBackend(CeedOperatorBuildKernel_Cuda_gen(op, is_run_good));
  if (!(*is_run_good)) return CEED_ERROR_SUCCESS;

  CeedCallBackend(CeedOperatorGetCeed(op, &ceed));
  CeedCallBackend(CeedGetData(ceed, &cuda_data));
  CeedCallBackend(CeedOperatorGetData(op, &data));
  CeedCallBackend(CeedOperatorGetQFunction(op, &qf));
  CeedCallBackend(CeedQFunctionGetData(qf, &qf_data));
  CeedCallBackend(CeedOperatorGetNumElements(op, &num_elem));
  CeedCallBackend(CeedOperatorGetFields(op, &num_input_fields, &op_input_fields, &num_output_fields, &op_output_fields));
  CeedCallBackend(CeedQFunctionGetFields(qf, NULL, &qf_input_fields, NULL, &qf_output_fields));

  // Input vectors
  for (CeedInt i = 0; i < num_input_fields; i++) {
    CeedCallBackend(CeedQFunctionFieldGetEvalMode(qf_input_fields[i], &eval_mode));
    if (eval_mode == CEED_EVAL_WEIGHT) {  // Skip
      data->fields.inputs[i] = NULL;
    } else {
      bool       is_active;
      CeedVector vec;

      // Get input vector
      CeedCallBackend(CeedOperatorFieldGetVector(op_input_fields[i], &vec));
      is_active = vec == CEED_VECTOR_ACTIVE;
      if (is_active) data->fields.inputs[i] = input_arr;
      else CeedCallBackend(CeedVectorGetArrayRead(vec, CEED_MEM_DEVICE, &data->fields.inputs[i]));
      CeedCallBackend(CeedVectorDestroy(&vec));
    }
  }

  // Output vectors
  for (CeedInt i = 0; i < num_output_fields; i++) {
    CeedCallBackend(CeedQFunctionFieldGetEvalMode(qf_output_fields[i], &eval_mode));
    if (eval_mode == CEED_EVAL_WEIGHT) {  // Skip
      data->fields.outputs[i] = NULL;
    } else {
      bool       is_active;
      CeedVector vec;

      // Get output vector
      CeedCallBackend(CeedOperatorFieldGetVector(op_output_fields[i], &vec));
      is_active = vec == CEED_VECTOR_ACTIVE;
      if (is_active) data->fields.outputs[i] = output_arr;
      else CeedCallBackend(CeedVectorGetArray(vec, CEED_MEM_DEVICE, &data->fields.outputs[i]));
      CeedCallBackend(CeedVectorDestroy(&vec));
    }
  }

  // Point coordinates, if needed
  CeedCallBackend(CeedOperatorIsAtPoints(op, &is_at_points));
  if (is_at_points) {
    // Coords
    CeedVector vec;

    CeedCallBackend(CeedOperatorAtPointsGetPoints(op, NULL, &vec));
    CeedCallBackend(CeedVectorGetArrayRead(vec, CEED_MEM_DEVICE, &data->points.coords));
    CeedCallBackend(CeedVectorDestroy(&vec));

    // Points per elem
    if (num_elem != data->points.num_elem) {
      CeedInt            *points_per_elem;
      const CeedInt       num_bytes   = num_elem * sizeof(CeedInt);
      CeedElemRestriction rstr_points = NULL;

      data->points.num_elem = num_elem;
      CeedCallBackend(CeedOperatorAtPointsGetPoints(op, &rstr_points, NULL));
      CeedCallBackend(CeedCalloc(num_elem, &points_per_elem));
      for (CeedInt e = 0; e < num_elem; e++) {
        CeedInt num_points_elem;

        CeedCallBackend(CeedElemRestrictionGetNumPointsInElement(rstr_points, e, &num_points_elem));
        points_per_elem[e] = num_points_elem;
      }
      if (data->points.num_per_elem) CeedCallCuda(ceed, cudaFree((void **)data->points.num_per_elem));
      CeedCallCuda(ceed, cudaMalloc((void **)&data->points.num_per_elem, num_bytes));
      CeedCallCuda(ceed, cudaMemcpy((void *)data->points.num_per_elem, points_per_elem, num_bytes, cudaMemcpyHostToDevice));
      CeedCallBackend(CeedElemRestrictionDestroy(&rstr_points));
      CeedCallBackend(CeedFree(&points_per_elem));
    }
  }

  // Get context data
  CeedCallBackend(CeedQFunctionGetInnerContextData(qf, CEED_MEM_DEVICE, &qf_data->d_c));

  // Apply operator
  void *opargs[] = {(void *)&num_elem, &qf_data->d_c, &data->indices, &data->fields, &data->B, &data->G, &data->W, &data->points};
  int   max_threads_per_block, min_grid_size, grid;

  CeedCallBackend(CeedOperatorHasTensorBases(op, &is_tensor));
  CeedCallCuda(ceed, cuOccupancyMaxPotentialBlockSize(&min_grid_size, &max_threads_per_block, data->op, dynamicSMemSize, 0, 0x10000));
  int block[3] = {data->thread_1d, ((!is_tensor || data->dim == 1) ? 1 : data->thread_1d), -1};

  if (is_tensor) {
    CeedCallBackend(BlockGridCalculate(num_elem, min_grid_size / cuda_data->device_prop.multiProcessorCount, max_threads_per_block,
                                       cuda_data->device_prop.maxThreadsDim[2], cuda_data->device_prop.warpSize, block, &grid));
  } else {
    CeedInt elems_per_block = CeedIntMin(cuda_data->device_prop.maxThreadsDim[2], CeedIntMax(512 / data->thread_1d, 1));

    grid     = num_elem / elems_per_block + (num_elem % elems_per_block > 0);
    block[2] = elems_per_block;
  }
  CeedInt shared_mem = block[0] * block[1] * block[2] * sizeof(CeedScalar);

  CeedCallBackend(CeedTryRunKernelDimShared_Cuda(ceed, data->op, stream, grid, block[0], block[1], block[2], shared_mem, is_run_good, opargs));

  // Restore input arrays
  for (CeedInt i = 0; i < num_input_fields; i++) {
    CeedCallBackend(CeedQFunctionFieldGetEvalMode(qf_input_fields[i], &eval_mode));
    if (eval_mode == CEED_EVAL_WEIGHT) {  // Skip
    } else {
      bool       is_active;
      CeedVector vec;

      CeedCallBackend(CeedOperatorFieldGetVector(op_input_fields[i], &vec));
      is_active = vec == CEED_VECTOR_ACTIVE;
      if (!is_active) CeedCallBackend(CeedVectorRestoreArrayRead(vec, &data->fields.inputs[i]));
      CeedCallBackend(CeedVectorDestroy(&vec));
    }
  }

  // Restore output arrays
  for (CeedInt i = 0; i < num_output_fields; i++) {
    CeedCallBackend(CeedQFunctionFieldGetEvalMode(qf_output_fields[i], &eval_mode));
    if (eval_mode == CEED_EVAL_WEIGHT) {  // Skip
    } else {
      bool       is_active;
      CeedVector vec;

      CeedCallBackend(CeedOperatorFieldGetVector(op_output_fields[i], &vec));
      is_active = vec == CEED_VECTOR_ACTIVE;
      if (!is_active) CeedCallBackend(CeedVectorRestoreArray(vec, &data->fields.outputs[i]));
      CeedCallBackend(CeedVectorDestroy(&vec));
    }
  }

  // Restore point coordinates, if needed
  if (is_at_points) {
    CeedVector vec;

    CeedCallBackend(CeedOperatorAtPointsGetPoints(op, NULL, &vec));
    CeedCallBackend(CeedVectorRestoreArrayRead(vec, &data->points.coords));
    CeedCallBackend(CeedVectorDestroy(&vec));
  }

  // Restore context data
  CeedCallBackend(CeedQFunctionRestoreInnerContextData(qf, &qf_data->d_c));

  // Cleanup
  CeedCallBackend(CeedDestroy(&ceed));
  CeedCallBackend(CeedQFunctionDestroy(&qf));
  if (!(*is_run_good)) data->use_fallback = true;
  return CEED_ERROR_SUCCESS;
}

static int CeedOperatorApplyAdd_Cuda_gen(CeedOperator op, CeedVector input_vec, CeedVector output_vec, CeedRequest *request) {
  bool              is_run_good = false;
  const CeedScalar *input_arr   = NULL;
  CeedScalar       *output_arr  = NULL;

  // Try to run kernel
  if (input_vec != CEED_VECTOR_NONE) CeedCallBackend(CeedVectorGetArrayRead(input_vec, CEED_MEM_DEVICE, &input_arr));
  if (output_vec != CEED_VECTOR_NONE) CeedCallBackend(CeedVectorGetArray(output_vec, CEED_MEM_DEVICE, &output_arr));
  CeedCallBackend(CeedOperatorApplyAddCore_Cuda_gen(op, NULL, input_arr, output_arr, &is_run_good, request));
  if (input_vec != CEED_VECTOR_NONE) CeedCallBackend(CeedVectorRestoreArrayRead(input_vec, &input_arr));
  if (output_vec != CEED_VECTOR_NONE) CeedCallBackend(CeedVectorRestoreArray(output_vec, &output_arr));

  // Fallback on unsuccessful run
  if (!is_run_good) {
    CeedOperator op_fallback;

    CeedDebug(CeedOperatorReturnCeed(op), "\nFalling back to /gpu/cuda/ref CeedOperator for ApplyAdd\n");
    CeedCallBackend(CeedOperatorGetFallback(op, &op_fallback));
    CeedCallBackend(CeedOperatorApplyAdd(op_fallback, input_vec, output_vec, request));
  }
  return CEED_ERROR_SUCCESS;
}

static int CeedOperatorApplyAddComposite_Cuda_gen(CeedOperator op, CeedVector input_vec, CeedVector output_vec, CeedRequest *request) {
  bool              is_run_good[CEED_COMPOSITE_MAX] = {false};
  CeedInt           num_suboperators;
  const CeedScalar *input_arr  = NULL;
  CeedScalar       *output_arr = NULL;
  Ceed              ceed;
  CeedOperator     *sub_operators;

  CeedCallBackend(CeedOperatorGetCeed(op, &ceed));
  CeedCall(CeedCompositeOperatorGetNumSub(op, &num_suboperators));
  CeedCall(CeedCompositeOperatorGetSubList(op, &sub_operators));
  if (input_vec != CEED_VECTOR_NONE) CeedCallBackend(CeedVectorGetArrayRead(input_vec, CEED_MEM_DEVICE, &input_arr));
  if (output_vec != CEED_VECTOR_NONE) CeedCallBackend(CeedVectorGetArray(output_vec, CEED_MEM_DEVICE, &output_arr));
  for (CeedInt i = 0; i < num_suboperators; i++) {
    CeedInt num_elem = 0;

    CeedCall(CeedOperatorGetNumElements(sub_operators[i], &num_elem));
    if (num_elem > 0) {
      cudaStream_t stream = NULL;

      CeedCallCuda(ceed, cudaStreamCreate(&stream));
      CeedCallBackend(CeedOperatorApplyAddCore_Cuda_gen(sub_operators[i], stream, input_arr, output_arr, &is_run_good[i], request));
      CeedCallCuda(ceed, cudaStreamDestroy(stream));
    }
  }
  if (input_vec != CEED_VECTOR_NONE) CeedCallBackend(CeedVectorRestoreArrayRead(input_vec, &input_arr));
  if (output_vec != CEED_VECTOR_NONE) CeedCallBackend(CeedVectorRestoreArray(output_vec, &output_arr));
  CeedCallCuda(ceed, cudaDeviceSynchronize());

  // Fallback on unsuccessful run
  for (CeedInt i = 0; i < num_suboperators; i++) {
    if (!is_run_good[i]) {
      CeedOperator op_fallback;

      CeedDebug(ceed, "\nFalling back to /gpu/cuda/ref CeedOperator for ApplyAdd\n");
      CeedCallBackend(CeedOperatorGetFallback(sub_operators[i], &op_fallback));
      CeedCallBackend(CeedOperatorApplyAdd(op_fallback, input_vec, output_vec, request));
    }
  }
  CeedCallBackend(CeedDestroy(&ceed));
  return CEED_ERROR_SUCCESS;
}

//------------------------------------------------------------------------------
// QFunction assembly
//------------------------------------------------------------------------------
static int CeedOperatorLinearAssembleQFunctionCore_Cuda_gen(CeedOperator op, bool build_objects, CeedVector *assembled, CeedElemRestriction *rstr,
                                                            CeedRequest *request) {
  Ceed                   ceed;
  CeedOperator_Cuda_gen *data;

  CeedCallBackend(CeedOperatorGetCeed(op, &ceed));
  CeedCallBackend(CeedOperatorGetData(op, &data));

  // Build the assembly kernel
  if (!data->assemble_qfunction && !data->use_assembly_fallback) {
    bool is_build_good = false;

    CeedCallBackend(CeedOperatorBuildKernel_Cuda_gen(op, &is_build_good));
    if (is_build_good) CeedCallBackend(CeedOperatorBuildKernelLinearAssembleQFunction_Cuda_gen(op, &is_build_good));
    if (!is_build_good) data->use_assembly_fallback = true;
  }

  // Try assembly
  if (!data->use_assembly_fallback) {
    bool                    is_run_good = true;
    Ceed_Cuda              *cuda_data;
    CeedInt                 num_elem, num_input_fields, num_output_fields;
    CeedEvalMode            eval_mode;
    CeedScalar             *assembled_array;
    CeedQFunctionField     *qf_input_fields, *qf_output_fields;
    CeedQFunction_Cuda_gen *qf_data;
    CeedQFunction           qf;
    CeedOperatorField      *op_input_fields, *op_output_fields;

    CeedCallBackend(CeedGetData(ceed, &cuda_data));
    CeedCallBackend(CeedOperatorGetQFunction(op, &qf));
    CeedCallBackend(CeedQFunctionGetData(qf, &qf_data));
    CeedCallBackend(CeedOperatorGetNumElements(op, &num_elem));
    CeedCallBackend(CeedOperatorGetFields(op, &num_input_fields, &op_input_fields, &num_output_fields, &op_output_fields));
    CeedCallBackend(CeedQFunctionGetFields(qf, NULL, &qf_input_fields, NULL, &qf_output_fields));

    // Input vectors
    for (CeedInt i = 0; i < num_input_fields; i++) {
      CeedCallBackend(CeedQFunctionFieldGetEvalMode(qf_input_fields[i], &eval_mode));
      if (eval_mode == CEED_EVAL_WEIGHT) {  // Skip
        data->fields.inputs[i] = NULL;
      } else {
        bool       is_active;
        CeedVector vec;

        // Get input vector
        CeedCallBackend(CeedOperatorFieldGetVector(op_input_fields[i], &vec));
        is_active = vec == CEED_VECTOR_ACTIVE;
        if (is_active) data->fields.inputs[i] = NULL;
        else CeedCallBackend(CeedVectorGetArrayRead(vec, CEED_MEM_DEVICE, &data->fields.inputs[i]));
        CeedCallBackend(CeedVectorDestroy(&vec));
      }
    }

    // Get context data
    CeedCallBackend(CeedQFunctionGetInnerContextData(qf, CEED_MEM_DEVICE, &qf_data->d_c));

    // Build objects if needed
    if (build_objects) {
      CeedInt qf_size_in = 0, qf_size_out = 0, Q;

      // Count number of active input fields
      {
        for (CeedInt i = 0; i < num_input_fields; i++) {
          CeedInt    field_size;
          CeedVector vec;

          // Get input vector
          CeedCallBackend(CeedOperatorFieldGetVector(op_input_fields[i], &vec));
          // Check if active input
          if (vec == CEED_VECTOR_ACTIVE) {
            CeedCallBackend(CeedQFunctionFieldGetSize(qf_input_fields[i], &field_size));
            qf_size_in += field_size;
          }
          CeedCallBackend(CeedVectorDestroy(&vec));
        }
        CeedCheck(qf_size_in > 0, ceed, CEED_ERROR_BACKEND, "Cannot assemble QFunction without active inputs and outputs");
      }

      // Count number of active output fields
      {
        for (CeedInt i = 0; i < num_output_fields; i++) {
          CeedInt    field_size;
          CeedVector vec;

          // Get output vector
          CeedCallBackend(CeedOperatorFieldGetVector(op_output_fields[i], &vec));
          // Check if active output
          if (vec == CEED_VECTOR_ACTIVE) {
            CeedCallBackend(CeedQFunctionFieldGetSize(qf_output_fields[i], &field_size));
            qf_size_out += field_size;
          }
          CeedCallBackend(CeedVectorDestroy(&vec));
        }
        CeedCheck(qf_size_out > 0, ceed, CEED_ERROR_BACKEND, "Cannot assemble QFunction without active inputs and outputs");
      }
      CeedCallBackend(CeedOperatorGetNumQuadraturePoints(op, &Q));

      // Actually build objects now
      const CeedSize l_size     = (CeedSize)num_elem * Q * qf_size_in * qf_size_out;
      CeedInt        strides[3] = {1, num_elem * Q, Q}; /* *NOPAD* */

      // Create output restriction
      CeedCallBackend(CeedElemRestrictionCreateStrided(ceed, num_elem, Q, qf_size_in * qf_size_out,
                                                       (CeedSize)qf_size_in * (CeedSize)qf_size_out * (CeedSize)num_elem * (CeedSize)Q, strides,
                                                       rstr));
      // Create assembled vector
      CeedCallBackend(CeedVectorCreate(ceed, l_size, assembled));
    }

    // Assembly array
    CeedCallBackend(CeedVectorGetArrayWrite(*assembled, CEED_MEM_DEVICE, &assembled_array));

    // Assemble QFunction
    void *opargs[] = {(void *)&num_elem, &qf_data->d_c, &data->indices, &data->fields, &data->B, &data->G, &data->W, &data->points, &assembled_array};
    bool  is_tensor = false;
    int   max_threads_per_block, min_grid_size, grid;

    CeedCallBackend(CeedOperatorHasTensorBases(op, &is_tensor));
    CeedCallCuda(ceed, cuOccupancyMaxPotentialBlockSize(&min_grid_size, &max_threads_per_block, data->op, dynamicSMemSize, 0, 0x10000));
    int block[3] = {data->thread_1d, ((!is_tensor || data->dim == 1) ? 1 : data->thread_1d), -1};

    if (is_tensor) {
      CeedCallBackend(BlockGridCalculate(num_elem, min_grid_size / cuda_data->device_prop.multiProcessorCount, max_threads_per_block,
                                         cuda_data->device_prop.maxThreadsDim[2], cuda_data->device_prop.warpSize, block, &grid));
    } else {
      CeedInt elems_per_block = CeedIntMin(cuda_data->device_prop.maxThreadsDim[2], CeedIntMax(512 / data->thread_1d, 1));

      grid     = num_elem / elems_per_block + (num_elem % elems_per_block > 0);
      block[2] = elems_per_block;
    }
    CeedInt shared_mem = block[0] * block[1] * block[2] * sizeof(CeedScalar);

    CeedCallBackend(
        CeedTryRunKernelDimShared_Cuda(ceed, data->assemble_qfunction, NULL, grid, block[0], block[1], block[2], shared_mem, &is_run_good, opargs));
    CeedCallCuda(ceed, cudaDeviceSynchronize());

    // Restore input arrays
    for (CeedInt i = 0; i < num_input_fields; i++) {
      CeedCallBackend(CeedQFunctionFieldGetEvalMode(qf_input_fields[i], &eval_mode));
      if (eval_mode == CEED_EVAL_WEIGHT) {  // Skip
      } else {
        bool       is_active;
        CeedVector vec;

        CeedCallBackend(CeedOperatorFieldGetVector(op_input_fields[i], &vec));
        is_active = vec == CEED_VECTOR_ACTIVE;
        if (!is_active) CeedCallBackend(CeedVectorRestoreArrayRead(vec, &data->fields.inputs[i]));
        CeedCallBackend(CeedVectorDestroy(&vec));
      }
    }

    // Restore context data
    CeedCallBackend(CeedQFunctionRestoreInnerContextData(qf, &qf_data->d_c));

    // Restore assembly array
    CeedCallBackend(CeedVectorRestoreArray(*assembled, &assembled_array));

    // Cleanup
    CeedCallBackend(CeedQFunctionDestroy(&qf));
    if (!is_run_good) {
      data->use_assembly_fallback = true;
      if (build_objects) {
        CeedCallBackend(CeedVectorDestroy(assembled));
        CeedCallBackend(CeedElemRestrictionDestroy(rstr));
      }
    }
  }
  CeedCallBackend(CeedDestroy(&ceed));

  // Fallback, if needed
  if (data->use_assembly_fallback) {
    CeedOperator op_fallback;

    CeedDebug(CeedOperatorReturnCeed(op), "\nFalling back to /gpu/cuda/ref CeedOperator for LinearAssemblyQFunction\n");
    CeedCallBackend(CeedOperatorGetFallback(op, &op_fallback));
    CeedCallBackend(CeedOperatorFallbackLinearAssembleQFunctionBuildOrUpdate(op_fallback, assembled, rstr, request));
    return CEED_ERROR_SUCCESS;
  }
  return CEED_ERROR_SUCCESS;
}

static int CeedOperatorLinearAssembleQFunction_Cuda_gen(CeedOperator op, CeedVector *assembled, CeedElemRestriction *rstr, CeedRequest *request) {
  return CeedOperatorLinearAssembleQFunctionCore_Cuda_gen(op, true, assembled, rstr, request);
}

static int CeedOperatorLinearAssembleQFunctionUpdate_Cuda_gen(CeedOperator op, CeedVector assembled, CeedElemRestriction rstr, CeedRequest *request) {
  return CeedOperatorLinearAssembleQFunctionCore_Cuda_gen(op, false, &assembled, &rstr, request);
}

//------------------------------------------------------------------------------
// AtPoints diagonal assembly
//------------------------------------------------------------------------------
static int CeedOperatorLinearAssembleAddDiagonalAtPoints_Cuda_gen(CeedOperator op, CeedVector assembled, CeedRequest *request) {
  Ceed                   ceed;
  CeedOperator_Cuda_gen *data;

  CeedCallBackend(CeedOperatorGetCeed(op, &ceed));
  CeedCallBackend(CeedOperatorGetData(op, &data));

  // Build the assembly kernel
  if (!data->assemble_diagonal && !data->use_assembly_fallback) {
    bool                     is_build_good = false;
    CeedInt                  num_active_bases_in, num_active_bases_out;
    CeedOperatorAssemblyData assembly_data;

    CeedCallBackend(CeedOperatorGetOperatorAssemblyData(op, &assembly_data));
    CeedCallBackend(
        CeedOperatorAssemblyDataGetEvalModes(assembly_data, &num_active_bases_in, NULL, NULL, NULL, &num_active_bases_out, NULL, NULL, NULL, NULL));
    if (num_active_bases_in == num_active_bases_out) {
      CeedCallBackend(CeedOperatorBuildKernel_Cuda_gen(op, &is_build_good));
      if (is_build_good) CeedCallBackend(CeedOperatorBuildKernelDiagonalAssemblyAtPoints_Cuda_gen(op, &is_build_good));
    }
    if (!is_build_good) data->use_assembly_fallback = true;
  }

  // Try assembly
  if (!data->use_assembly_fallback) {
    bool                    is_run_good = true;
    Ceed_Cuda              *cuda_data;
    CeedInt                 num_elem, num_input_fields, num_output_fields;
    CeedEvalMode            eval_mode;
    CeedScalar             *assembled_array;
    CeedQFunctionField     *qf_input_fields, *qf_output_fields;
    CeedQFunction_Cuda_gen *qf_data;
    CeedQFunction           qf;
    CeedOperatorField      *op_input_fields, *op_output_fields;

    CeedCallBackend(CeedGetData(ceed, &cuda_data));
    CeedCallBackend(CeedOperatorGetQFunction(op, &qf));
    CeedCallBackend(CeedQFunctionGetData(qf, &qf_data));
    CeedCallBackend(CeedOperatorGetNumElements(op, &num_elem));
    CeedCallBackend(CeedOperatorGetFields(op, &num_input_fields, &op_input_fields, &num_output_fields, &op_output_fields));
    CeedCallBackend(CeedQFunctionGetFields(qf, NULL, &qf_input_fields, NULL, &qf_output_fields));

    // Input vectors
    for (CeedInt i = 0; i < num_input_fields; i++) {
      CeedCallBackend(CeedQFunctionFieldGetEvalMode(qf_input_fields[i], &eval_mode));
      if (eval_mode == CEED_EVAL_WEIGHT) {  // Skip
        data->fields.inputs[i] = NULL;
      } else {
        bool       is_active;
        CeedVector vec;

        // Get input vector
        CeedCallBackend(CeedOperatorFieldGetVector(op_input_fields[i], &vec));
        is_active = vec == CEED_VECTOR_ACTIVE;
        if (is_active) data->fields.inputs[i] = NULL;
        else CeedCallBackend(CeedVectorGetArrayRead(vec, CEED_MEM_DEVICE, &data->fields.inputs[i]));
        CeedCallBackend(CeedVectorDestroy(&vec));
      }
    }

    // Point coordinates
    {
      CeedVector vec;

      CeedCallBackend(CeedOperatorAtPointsGetPoints(op, NULL, &vec));
      CeedCallBackend(CeedVectorGetArrayRead(vec, CEED_MEM_DEVICE, &data->points.coords));
      CeedCallBackend(CeedVectorDestroy(&vec));

      // Points per elem
      if (num_elem != data->points.num_elem) {
        CeedInt            *points_per_elem;
        const CeedInt       num_bytes   = num_elem * sizeof(CeedInt);
        CeedElemRestriction rstr_points = NULL;

        data->points.num_elem = num_elem;
        CeedCallBackend(CeedOperatorAtPointsGetPoints(op, &rstr_points, NULL));
        CeedCallBackend(CeedCalloc(num_elem, &points_per_elem));
        for (CeedInt e = 0; e < num_elem; e++) {
          CeedInt num_points_elem;

          CeedCallBackend(CeedElemRestrictionGetNumPointsInElement(rstr_points, e, &num_points_elem));
          points_per_elem[e] = num_points_elem;
        }
        if (data->points.num_per_elem) CeedCallCuda(ceed, cudaFree((void **)data->points.num_per_elem));
        CeedCallCuda(ceed, cudaMalloc((void **)&data->points.num_per_elem, num_bytes));
        CeedCallCuda(ceed, cudaMemcpy((void *)data->points.num_per_elem, points_per_elem, num_bytes, cudaMemcpyHostToDevice));
        CeedCallBackend(CeedElemRestrictionDestroy(&rstr_points));
        CeedCallBackend(CeedFree(&points_per_elem));
      }
    }

    // Get context data
    CeedCallBackend(CeedQFunctionGetInnerContextData(qf, CEED_MEM_DEVICE, &qf_data->d_c));

    // Assembly array
    CeedCallBackend(CeedVectorGetArray(assembled, CEED_MEM_DEVICE, &assembled_array));

    // Assemble diagonal
    void *opargs[] = {(void *)&num_elem, &qf_data->d_c, &data->indices, &data->fields, &data->B, &data->G, &data->W, &data->points, &assembled_array};
    int   max_threads_per_block, min_grid_size, grid;

    CeedCallCuda(ceed, cuOccupancyMaxPotentialBlockSize(&min_grid_size, &max_threads_per_block, data->op, dynamicSMemSize, 0, 0x10000));
    int block[3] = {data->thread_1d, (data->dim == 1 ? 1 : data->thread_1d), -1};

    CeedCallBackend(BlockGridCalculate(num_elem, min_grid_size / cuda_data->device_prop.multiProcessorCount, 1,
                                       cuda_data->device_prop.maxThreadsDim[2], cuda_data->device_prop.warpSize, block, &grid));
    CeedInt shared_mem = block[0] * block[1] * block[2] * sizeof(CeedScalar);

    CeedCallBackend(
        CeedTryRunKernelDimShared_Cuda(ceed, data->assemble_diagonal, NULL, grid, block[0], block[1], block[2], shared_mem, &is_run_good, opargs));
    CeedCallCuda(ceed, cudaDeviceSynchronize());

    // Restore input arrays
    for (CeedInt i = 0; i < num_input_fields; i++) {
      CeedCallBackend(CeedQFunctionFieldGetEvalMode(qf_input_fields[i], &eval_mode));
      if (eval_mode == CEED_EVAL_WEIGHT) {  // Skip
      } else {
        bool       is_active;
        CeedVector vec;

        CeedCallBackend(CeedOperatorFieldGetVector(op_input_fields[i], &vec));
        is_active = vec == CEED_VECTOR_ACTIVE;
        if (!is_active) CeedCallBackend(CeedVectorRestoreArrayRead(vec, &data->fields.inputs[i]));
        CeedCallBackend(CeedVectorDestroy(&vec));
      }
    }

    // Restore point coordinates
    {
      CeedVector vec;

      CeedCallBackend(CeedOperatorAtPointsGetPoints(op, NULL, &vec));
      CeedCallBackend(CeedVectorRestoreArrayRead(vec, &data->points.coords));
      CeedCallBackend(CeedVectorDestroy(&vec));
    }

    // Restore context data
    CeedCallBackend(CeedQFunctionRestoreInnerContextData(qf, &qf_data->d_c));

    // Restore assembly array
    CeedCallBackend(CeedVectorRestoreArray(assembled, &assembled_array));

    // Cleanup
    CeedCallBackend(CeedQFunctionDestroy(&qf));
    if (!is_run_good) data->use_assembly_fallback = true;
  }
  CeedCallBackend(CeedDestroy(&ceed));

  // Fallback, if needed
  if (data->use_assembly_fallback) {
    CeedOperator op_fallback;

    CeedDebug(CeedOperatorReturnCeed(op), "\nFalling back to /gpu/cuda/ref CeedOperator for AtPoints LinearAssembleAddDiagonal\n");
    CeedCallBackend(CeedOperatorGetFallback(op, &op_fallback));
    CeedCallBackend(CeedOperatorLinearAssembleAddDiagonal(op_fallback, assembled, request));
    return CEED_ERROR_SUCCESS;
  }
  return CEED_ERROR_SUCCESS;
}

//------------------------------------------------------------------------------
// AtPoints full assembly
//------------------------------------------------------------------------------
static int CeedSingleOperatorAssembleAtPoints_Cuda_gen(CeedOperator op, CeedInt offset, CeedVector assembled) {
  Ceed                   ceed;
  CeedOperator_Cuda_gen *data;

  CeedCallBackend(CeedOperatorGetCeed(op, &ceed));
  CeedCallBackend(CeedOperatorGetData(op, &data));

  // Build the assembly kernel
  if (!data->assemble_full && !data->use_assembly_fallback) {
    bool                     is_build_good = false;
    CeedInt                  num_active_bases_in, num_active_bases_out;
    CeedOperatorAssemblyData assembly_data;

    CeedCallBackend(CeedOperatorGetOperatorAssemblyData(op, &assembly_data));
    CeedCallBackend(
        CeedOperatorAssemblyDataGetEvalModes(assembly_data, &num_active_bases_in, NULL, NULL, NULL, &num_active_bases_out, NULL, NULL, NULL, NULL));
    if (num_active_bases_in == num_active_bases_out) {
      CeedCallBackend(CeedOperatorBuildKernel_Cuda_gen(op, &is_build_good));
      if (is_build_good) CeedCallBackend(CeedOperatorBuildKernelFullAssemblyAtPoints_Cuda_gen(op, &is_build_good));
    }
    if (!is_build_good) data->use_assembly_fallback = true;
  }

  // Try assembly
  if (!data->use_assembly_fallback) {
    bool                    is_run_good = true;
    Ceed_Cuda              *cuda_data;
    CeedInt                 num_elem, num_input_fields, num_output_fields;
    CeedEvalMode            eval_mode;
    CeedScalar             *assembled_array;
    CeedQFunctionField     *qf_input_fields, *qf_output_fields;
    CeedQFunction_Cuda_gen *qf_data;
    CeedQFunction           qf;
    CeedOperatorField      *op_input_fields, *op_output_fields;

    CeedCallBackend(CeedGetData(ceed, &cuda_data));
    CeedCallBackend(CeedOperatorGetQFunction(op, &qf));
    CeedCallBackend(CeedQFunctionGetData(qf, &qf_data));
    CeedCallBackend(CeedOperatorGetNumElements(op, &num_elem));
    CeedCallBackend(CeedOperatorGetFields(op, &num_input_fields, &op_input_fields, &num_output_fields, &op_output_fields));
    CeedCallBackend(CeedQFunctionGetFields(qf, NULL, &qf_input_fields, NULL, &qf_output_fields));

    // Input vectors
    for (CeedInt i = 0; i < num_input_fields; i++) {
      CeedCallBackend(CeedQFunctionFieldGetEvalMode(qf_input_fields[i], &eval_mode));
      if (eval_mode == CEED_EVAL_WEIGHT) {  // Skip
        data->fields.inputs[i] = NULL;
      } else {
        bool       is_active;
        CeedVector vec;

        // Get input vector
        CeedCallBackend(CeedOperatorFieldGetVector(op_input_fields[i], &vec));
        is_active = vec == CEED_VECTOR_ACTIVE;
        if (is_active) data->fields.inputs[i] = NULL;
        else CeedCallBackend(CeedVectorGetArrayRead(vec, CEED_MEM_DEVICE, &data->fields.inputs[i]));
        CeedCallBackend(CeedVectorDestroy(&vec));
      }
    }

    // Point coordinates
    {
      CeedVector vec;

      CeedCallBackend(CeedOperatorAtPointsGetPoints(op, NULL, &vec));
      CeedCallBackend(CeedVectorGetArrayRead(vec, CEED_MEM_DEVICE, &data->points.coords));
      CeedCallBackend(CeedVectorDestroy(&vec));

      // Points per elem
      if (num_elem != data->points.num_elem) {
        CeedInt            *points_per_elem;
        const CeedInt       num_bytes   = num_elem * sizeof(CeedInt);
        CeedElemRestriction rstr_points = NULL;

        data->points.num_elem = num_elem;
        CeedCallBackend(CeedOperatorAtPointsGetPoints(op, &rstr_points, NULL));
        CeedCallBackend(CeedCalloc(num_elem, &points_per_elem));
        for (CeedInt e = 0; e < num_elem; e++) {
          CeedInt num_points_elem;

          CeedCallBackend(CeedElemRestrictionGetNumPointsInElement(rstr_points, e, &num_points_elem));
          points_per_elem[e] = num_points_elem;
        }
        if (data->points.num_per_elem) CeedCallCuda(ceed, cudaFree((void **)data->points.num_per_elem));
        CeedCallCuda(ceed, cudaMalloc((void **)&data->points.num_per_elem, num_bytes));
        CeedCallCuda(ceed, cudaMemcpy((void *)data->points.num_per_elem, points_per_elem, num_bytes, cudaMemcpyHostToDevice));
        CeedCallBackend(CeedElemRestrictionDestroy(&rstr_points));
        CeedCallBackend(CeedFree(&points_per_elem));
      }
    }

    // Get context data
    CeedCallBackend(CeedQFunctionGetInnerContextData(qf, CEED_MEM_DEVICE, &qf_data->d_c));

    // Assembly array
    CeedCallBackend(CeedVectorGetArray(assembled, CEED_MEM_DEVICE, &assembled_array));
    CeedScalar *assembled_offset_array = &assembled_array[offset];

    // Assemble diagonal
    void *opargs[] = {(void *)&num_elem, &qf_data->d_c, &data->indices, &data->fields,          &data->B,
                      &data->G,          &data->W,      &data->points,  &assembled_offset_array};
    int   max_threads_per_block, min_grid_size, grid;

    CeedCallCuda(ceed, cuOccupancyMaxPotentialBlockSize(&min_grid_size, &max_threads_per_block, data->op, dynamicSMemSize, 0, 0x10000));
    int block[3] = {data->thread_1d, (data->dim == 1 ? 1 : data->thread_1d), -1};

    CeedCallBackend(BlockGridCalculate(num_elem, min_grid_size / cuda_data->device_prop.multiProcessorCount, 1,
                                       cuda_data->device_prop.maxThreadsDim[2], cuda_data->device_prop.warpSize, block, &grid));
    CeedInt shared_mem = block[0] * block[1] * block[2] * sizeof(CeedScalar);

    CeedCallBackend(
        CeedTryRunKernelDimShared_Cuda(ceed, data->assemble_full, NULL, grid, block[0], block[1], block[2], shared_mem, &is_run_good, opargs));
    CeedCallCuda(ceed, cudaDeviceSynchronize());

    // Restore input arrays
    for (CeedInt i = 0; i < num_input_fields; i++) {
      CeedCallBackend(CeedQFunctionFieldGetEvalMode(qf_input_fields[i], &eval_mode));
      if (eval_mode == CEED_EVAL_WEIGHT) {  // Skip
      } else {
        bool       is_active;
        CeedVector vec;

        CeedCallBackend(CeedOperatorFieldGetVector(op_input_fields[i], &vec));
        is_active = vec == CEED_VECTOR_ACTIVE;
        if (!is_active) CeedCallBackend(CeedVectorRestoreArrayRead(vec, &data->fields.inputs[i]));
        CeedCallBackend(CeedVectorDestroy(&vec));
      }
    }

    // Restore point coordinates
    {
      CeedVector vec;

      CeedCallBackend(CeedOperatorAtPointsGetPoints(op, NULL, &vec));
      CeedCallBackend(CeedVectorRestoreArrayRead(vec, &data->points.coords));
      CeedCallBackend(CeedVectorDestroy(&vec));
    }

    // Restore context data
    CeedCallBackend(CeedQFunctionRestoreInnerContextData(qf, &qf_data->d_c));

    // Restore assembly array
    CeedCallBackend(CeedVectorRestoreArray(assembled, &assembled_array));

    // Cleanup
    CeedCallBackend(CeedQFunctionDestroy(&qf));
    if (!is_run_good) data->use_assembly_fallback = true;
  }
  CeedCallBackend(CeedDestroy(&ceed));

  // Fallback, if needed
  if (data->use_assembly_fallback) {
    CeedOperator op_fallback;

    CeedDebug(CeedOperatorReturnCeed(op), "\nFalling back to /gpu/cuda/ref CeedOperator for AtPoints SingleOperatorAssemble\n");
    CeedCallBackend(CeedOperatorGetFallback(op, &op_fallback));
    CeedCallBackend(CeedSingleOperatorAssemble(op_fallback, offset, assembled));
    return CEED_ERROR_SUCCESS;
  }
  return CEED_ERROR_SUCCESS;
}

//------------------------------------------------------------------------------
// Create operator
//------------------------------------------------------------------------------
int CeedOperatorCreate_Cuda_gen(CeedOperator op) {
  bool                   is_composite, is_at_points;
  Ceed                   ceed;
  CeedOperator_Cuda_gen *impl;

  CeedCallBackend(CeedOperatorGetCeed(op, &ceed));
  CeedCallBackend(CeedCalloc(1, &impl));
  CeedCallBackend(CeedOperatorSetData(op, impl));
  CeedCall(CeedOperatorIsComposite(op, &is_composite));
  if (is_composite) {
    CeedCallBackend(CeedSetBackendFunction(ceed, "Operator", op, "ApplyAddComposite", CeedOperatorApplyAddComposite_Cuda_gen));
  } else {
    CeedCallBackend(CeedSetBackendFunction(ceed, "Operator", op, "ApplyAdd", CeedOperatorApplyAdd_Cuda_gen));
  }
  CeedCall(CeedOperatorIsAtPoints(op, &is_at_points));
  if (is_at_points) {
    CeedCallBackend(
        CeedSetBackendFunction(ceed, "Operator", op, "LinearAssembleAddDiagonal", CeedOperatorLinearAssembleAddDiagonalAtPoints_Cuda_gen));
    CeedCallBackend(CeedSetBackendFunction(ceed, "Operator", op, "LinearAssembleSingle", CeedSingleOperatorAssembleAtPoints_Cuda_gen));
  }
  if (!is_at_points) {
    CeedCallBackend(CeedSetBackendFunction(ceed, "Operator", op, "LinearAssembleQFunction", CeedOperatorLinearAssembleQFunction_Cuda_gen));
    CeedCallBackend(
        CeedSetBackendFunction(ceed, "Operator", op, "LinearAssembleQFunctionUpdate", CeedOperatorLinearAssembleQFunctionUpdate_Cuda_gen));
  }
  CeedCallBackend(CeedSetBackendFunction(ceed, "Operator", op, "Destroy", CeedOperatorDestroy_Cuda_gen));
  CeedCallBackend(CeedDestroy(&ceed));
  return CEED_ERROR_SUCCESS;
}

//------------------------------------------------------------------------------
