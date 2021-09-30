# Lint as: python3
# Copyright 2021 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
"""Test architecture for a set of tflite tests."""

import absl
import absl.testing as testing
import iree.compiler.tflite as iree_tflite_compile
import iree.runtime as iree_rt
import numpy as np
import os
import shutil
import sys
import tensorflow.compat.v2 as tf
import urllib.request

FLAGS = absl.flags.FLAGS

absl.flags.DEFINE_boolean("bypass_compare_results", False,
                          "Bypass result comparison with TFLite Interpreter.")

class TFLiteModelTest(testing.absltest.TestCase):
  def __init__(self, model_path, *args, **kwargs):
    super(TFLiteModelTest, self).__init__(*args, **kwargs)
    self.model_path = model_path

  def setUp(self):
    exe_basename = os.path.basename(sys.argv[0])
    self.workdir = os.path.join(os.path.dirname(__file__), "tmp", exe_basename)
    print(f"TMP_DIR = {self.workdir}")
    os.makedirs(self.workdir, exist_ok=True)
    self.tflite_file = '/'.join([self.workdir, 'model.tflite'])
    self.tflite_ir = '/'.join([self.workdir, 'tflite.mlir'])
    self.iree_ir = '/'.join([self.workdir, 'tosa.mlir'])
    if os.path.exists(self.model_path):
      shutil.copy(self.model_path, self.tflite_file)
    else:
      urllib.request.urlretrieve(self.model_path, self.tflite_file)
    self.binary = '/'.join([self.workdir, 'module.bytecode'])

  def generate_inputs(self, input_details):
    args = []
    for input in input_details:
      absl.logging.info("\t%s, %s", str(input["shape"]), input["dtype"].__name__)
      args.append(np.zeros(shape=input["shape"], dtype=input["dtype"]))
    return args

  def generate_inputs(self):
    args = np.zeros([1, 1], dtype=np.float)
    return args

  def compare_results(self, iree_results, tflite_results, details):
    self.assertEqual(
      len(iree_results), len(tflite_results), "Number of results do not match")

    for i in range(len(details)):
      iree_result = iree_results[i]
      tflite_result = tflite_results[i]
      dtype = details[i]["dtype"]
      iree_result = iree_result.astype(dtype)
      tflite_result = tflite_result.astype(dtype)
      self.assertEqual(iree_result.shape, tflite_result.shape)
      maxError = np.max(np.abs(iree_result - tflite_result))
      absl.logging.info("Max error (%d): %f", i, maxError)

  def compile_and_execute(self):
    self.assertIsNotNone(self.model_path)

    absl.logging.info("Setting up for IREE")
    iree_tflite_compile.compile_file(
      self.tflite_file, input_type="tosa",
      output_file=self.binary,
      save_temp_tfl_input=self.tflite_ir,
      save_temp_iree_input=self.iree_ir,
      target_backends=iree_tflite_compile.DEFAULT_TESTING_BACKENDS,
      import_only=False)

    # Some models may not be TFLite Python API compatible.
    # Bypass the comparison check.
    if not FLAGS.bypass_compare_results:
      absl.logging.info("Setting up tflite interpreter")
      tflite_interpreter = tf.lite.Interpreter(model_path=self.tflite_file)
      tflite_interpreter.allocate_tensors()
      input_details = tflite_interpreter.get_input_details()
      output_details = tflite_interpreter.get_output_details()

      absl.logging.info("Setting up test inputs")
      args = self.generate_inputs(input_details)

      absl.logging.info("Invoking TFLite")
      for i, input in enumerate(args):
        tflite_interpreter.set_tensor(input_details[i]['index'], input)
      tflite_interpreter.invoke()
      tflite_results = []
      for output_detail in output_details:
        tflite_results.append(np.array(tflite_interpreter.get_tensor(
          output_detail['index'])))
    else:
      # Still generate inputs so we can run IREE invocation.
      # The model test class needs to provide the right input.
      args = self.generate_inputs()


    absl.logging.info("Invoke IREE")
    iree_results = None
    with open(self.binary, 'rb') as f:
      config = iree_rt.Config("dylib")
      ctx = iree_rt.SystemContext(config=config)
      vm_module = iree_rt.VmModule.from_flatbuffer(f.read())
      ctx.add_vm_module(vm_module)
      invoke = ctx.modules.module["main"]
      iree_results = invoke(*args)
      if not isinstance(iree_results, tuple):
        iree_results = (iree_results,)

    if not FLAGS.bypass_compare_results:
      # Fix type information for unsigned cases.
      iree_results = list(iree_results)
      tflite_results = list(tflite_results)
      for i in range(len(output_details)):
        dtype = output_details[i]["dtype"]
        iree_results[i] = iree_results[i].astype(dtype)
        tflite_results[i] = tflite_results[i].astype(dtype)

      self.compare_results(iree_results, tflite_results, output_details)
