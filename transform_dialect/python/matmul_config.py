
fill_matmul_f32 = """ 
!input_tensor_t = tensor<${M}x${K}xf32>
!weight_tensor_t = tensor<${K}x${N}xf32>
!output_tensor_t = tensor<${M}x${N}xf32>
func.func @${FN_NAME}(%in: !input_tensor_t, %wei: !weight_tensor_t) -> !output_tensor_t {
  %cst_0 = arith.constant 0.0 : f32 
  %empty = tensor.empty() : !output_tensor_t
  %out = linalg.fill ins(%cst_0 : f32) outs(%empty : !output_tensor_t) -> !output_tensor_t
  %res = linalg.matmul
     ins(%in, %wei: !input_tensor_t, !weight_tensor_t)
    outs(%out: !output_tensor_t) -> !output_tensor_t
  return %res : !output_tensor_t
}
"""

def make_fill_matmul_f32_problem(M, N, K, td_config=None):
  fn_name = f"mm_{M}_{N}_{K}"
  fn_name = fn_name if td_config is None else \
    fn_name  + "_" + "_".join([f"{k}_{v}" for k, v in td_config.items()])
  fn_name = fn_name.replace(',', '_')
  return fill_matmul_f32.replace(
    "${M}", str(M)).replace(
    "${K}", str(K)).replace(
    "${N}", str(N)).replace(
    "${FN_NAME}", str(fn_name)), \
    fn_name

# Some extra flags that may be useful to uncomment, but not to remember and type...
# "--mlir-print-ir-after-all",
# "--iree-hal-dump-executable-benchmarks-to=/tmp/iree-executables",
# Dump generated binary files (i.e. PTX).
# "--iree-hal-dump-executable-binaries-to=/tmp/iree-executables",
# Uncomment the following to see generated bitcode files, on which llvm-dis
# can be used to get to LLVM IR.
# "--iree-hal-dump-executable-intermediates-to=/tmp/iree-executables",
# "--iree-hal-dump-executable-sources-to=/tmp/iree-executables",
def append_td_repro_options(options, td_repro=False):
  return options + [
    "--debug-only=transform-dialect-save-repro",
    "--mlir-disable-threading",

  ] if td_repro else options

def make_iree_baseline_options(td_repro=False):
  res = [
    # Despite best efforts, can't seem to start IREE from GPU-resident tensors 
    # without copies accounted in the cost atm...
    "--iree-hal-benchmark-dispatch-repeat-count=2",
    "--iree-stream-resource-index-bits=64",
    "--iree-vm-target-index-bits=64",
    "--iree-hal-cuda-llvm-target-arch=sm_80",
    "--iree-codegen-llvmgpu-enable-transform-dialect-jit=false",
  ]
  return append_td_repro_options(res, td_repro)


# Some extra flags that may be useful to uncomment, but not to remember and type...
# Uncomment some options manually
# 
# Options for debugging mapping to mma operations.
# "--debug-only=vector-unroll",
# "--debug-only=iree-codegen-gpu-pipelining",
# "--debug-only=llvm-gpu-utils",
#
# Options for debugging GPU barrier removal.
# "--debug-only=transform-llvmgpu-extensions-alias",
#
# Options for debugging transform strategies.
# "--debug-only=iree-transform-strategy-builder",
# "--debug-only=transform-dialect-save-repro",
# "--mlir-disable-threading",
def make_iree_td_options(config, td_repro=False, benchmark=False):
  res = [
    # Despite best efforts, can't seem to start IREE from GPU-resident tensors 
    # without copies accounted in the cost atm...
    "--iree-hal-benchmark-dispatch-repeat-count=2",
    "--iree-stream-resource-index-bits=64",
    "--iree-vm-target-index-bits=64",
    "--iree-hal-cuda-llvm-target-arch=sm_80",
    f"--td-matmul-strategy-blk-sizes={config['blk']}",
    f"--td-matmul-strategy-num-threads={config['tds']}",
    f"--td-matmul-strategy-num-warps={config['wps']}",
    f"--td-matmul-strategy-pipeline-depth={config['p']}",
    f"--td-matmul-strategy-reduc-size={config['r']}",
    f"--td-matmul-strategy-use-async-copies={config['acp']}",
    f"--td-matmul-strategy-use-mma-sync={config['mma']}",
    "--iree-codegen-llvmgpu-enable-transform-dialect-aligned-matmul",
    f'--iree-flow-enable-pad-handling',
    f'--iree-codegen-llvmgpu-enable-transform-dialect-pad-strategy'
  ]
  return append_td_repro_options(res, td_repro)

def append_td_graph_script(l, filename=None):
  return l + [
    # TODO: when openxla builds python bindings properly we can just add
    # f'--openxla-transform-preprocessing={filename}',
    # and later just omit it altogether because the strategy is embedded into 
    # the C++ plugin.
    # In the meantime we need to use IREE.
    f'--iree-flow-dispatch-use-transform-dialect={filename}',
  ] if filename is not None else l

# For now assume type if TF32.
def compute_precision(K, *tensors):
  max_value = 0.0
  for t in tensors:
      max_value = max(float(t.abs().max()), max_value)
  # Relative precision for TF32 is 1e-4, for FP32 it is 1e-7.
  rtol = 1e-4 * K
  rtol = 1e-4
  atol = rtol * max_value * K
  # print(f"rtol={rtol} atol={atol}")
  return rtol, atol
