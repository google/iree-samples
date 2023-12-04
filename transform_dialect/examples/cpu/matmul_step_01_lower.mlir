// Instructions; TL;DR
// ===================
//
// This script shows a minimal example lowering through IREE with custom transforms:
//   - one level of tiling with `num_threads = 1` (required to connect to IREE's
//     threadpool and abstractions).
//   - late bufferization (i.e. convert from SSA + subsets semantics to 
//     side-effects semantics).
//   - IREE-specific cleanups and concrete lowering to connect to the runtime.
//
// ```
//   export IREE_DIR=${HOME}/github/iree; \
//   export IREE_SAMPLES_DIR=${HOME}/github/iree-samples; \
//   cat ${IREE_SAMPLES_DIR}/transform_dialect/examples/matmul.mlir |\
//   sed "s/\${M}/11/g" | sed "s/\${K}/12/g" | sed "s/\${N}/13/g" | \
//   sed "s/private @matmul_static(/@matmul_static(/g" | \
//   ${IREE_DIR}/build/tools/iree-opt \
//     --iree-hal-target-backends=llvm-cpu \
//     --iree-abi-transformation-pipeline \
//     --iree-flow-transformation-pipeline \
//     --iree-stream-transformation-pipeline \
//     --iree-hal-configuration-pipeline | \
//   ${IREE_DIR}/build/tools/iree-opt \
//      --pass-pipeline='builtin.module(hal.executable(hal.executable.variant(iree-llvmcpu-lower-executable-target)))' \
//      --iree-codegen-llvmcpu-use-transform-dialect=${IREE_SAMPLES_DIR}/transform_dialect/examples/cpu/matmul_step_01_lower.mlir
// ```
//
// To execute:
// ```
//   export IREE_DIR=${HOME}/github/iree; \
//   export IREE_SAMPLES_DIR=${HOME}/github/iree-samples; \
//   cat ${IREE_SAMPLES_DIR}/transform_dialect/examples/matmul.mlir |\
//   sed "s/\${M}/11/g" | sed "s/\${K}/12/g" | sed "s/\${N}/13/g" | \
//   sed "s/private @matmul_static(/@matmul_static(/g" | \
//   iree-compile - --iree-hal-target-backends=llvm-cpu \
//     --iree-codegen-llvmcpu-use-transform-dialect=${IREE_SAMPLES_DIR}/transform_dialect/examples/cpu/matmul_step_01_lower.mlir | \
//   iree-run-module --function=matmul_static \
//     --input="11x12xf32=1" \
//     --input="12x13xf32=1" \
//     --input="11x13xf32=33" | \
//   FileCheck ${IREE_SAMPLES_DIR}/transform_dialect/examples/cpu/matmul_step_01_lower.mlir
// ```

// CHECK: EXEC @matmul_static
// CHECK: result[0]: hal.buffer_view
// CHECK: 11x13xf32=[45 45 45 45 45 45 45 45 45 45 45 45 45]
// etc

// Comment parts of the IR below starting from the bottom-up and rerun the command
// to see the effects of step 1. ; step 1. + step 2.; etc..
transform.sequence failures(propagate) {
^bb1(%variant_op: !pdl.operation):
  %original_matmul = transform.structured.match ops{["linalg.matmul"]} in %variant_op
    : (!pdl.operation) -> !pdl.operation

  // IREE-specific connection to the runtime and threadpool, required to run e2e.
  // ============================================================================
  %forall, %matmul =
    transform.structured.tile_to_forall_op %original_matmul
      num_threads [1]
      // TODO: IREE needs own workgroup mapping attribute independent of GPU.
      ( mapping = [#gpu.block<x>] )
  transform.iree.populate_workgroup_count_region_using_num_threads_slice
    %forall : (!pdl.operation) -> ()
  // Post-tiling canonicalizations, in particular to ensure num_threads == 1 in 
  // the IR and prepare for bufferization.
  transform.iree.apply_patterns %variant_op 
    {canonicalization, cse, licm, tiling_canonicalization} : (!pdl.operation) -> ()

  // IREE-specific bufferization.
  // ============================================================================
  %variant_op_2 = transform.iree.bufferize %variant_op
   : (!pdl.operation) -> (!pdl.operation)

  // IREE-specific cleanup and connection to the runtime and threadpool, 
  // required to run e2e.
  // ============================================================================
  %func = transform.structured.match ops{["func.func"]} in %variant_op_2
    : (!pdl.operation) -> !pdl.operation
  transform.iree.erase_hal_descriptor_type_from_memref %func
    : (!pdl.operation) -> ()
  transform.iree.forall_to_workgroup %func
    : (!pdl.operation) -> ()

  // TODO: maybe control transform.lower_to_llvm from here.

  // Late canonicalizations and cleanups.
  transform.iree.apply_patterns %variant_op_2 
    {canonicalization, cse, licm, tiling_canonicalization}
      : (!pdl.operation) -> ()

  // ============================================================================
  // Note: Ideally, we would only want the following 2 transforms for the most
  // basic e2e connection
  //
  // IREE-specific bufferization and cleanup
  // ============================================================================
  // %variant_op_2 = transform.iree.bufferize %variant_op
  // %func = transform.structured.match ops{["func.func"]} in %variant_op_2
  //   : (!pdl.operation) -> !pdl.operation
  // %func_2 = transform.iree.erase_hal_descriptor_type_from_memref %func
  //
  // Unfortunately, this is not possible because we end up with a conversion error
  // very late in the IREE pass pipeline:
  // ```
  //   failed to materialize conversion for result #0 of operation 'arith.constant'
  //   that remained live after conversion
  // ```
  //
  // This error is due to the fact that the "workgroup_count" region does not get
  // lowered and gives us problems.
  // As a consequence, we pay the cost of an extra 
  //   `transform.iree.tile_to_forall_and_workgroup_count_region` and 
  //   `transform.iree.forall_to_workgroup %func_2` 
  // to lower e2e without headaches.
  // ============================================================================
}
