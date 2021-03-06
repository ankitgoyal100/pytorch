/**
 * Copyright (c) 2016-present, Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <string>

#include "binaries/benchmark_helper.h"
#include "caffe2/core/blob_serialization.h"
#ifdef __CUDA_ARCH__
#include "caffe2/core/context_gpu.h"
#endif
#include "caffe2/core/init.h"
#include "caffe2/core/logging.h"
#include "caffe2/core/net.h"
#include "caffe2/core/operator.h"
#include "caffe2/utils/string_utils.h"
#include "observers/net_observer_reporter_print.h"
#include "observers/observer_config.h"
#include "observers/perf_observer.h"

using std::shared_ptr;
using std::string;
using std::unique_ptr;
using std::vector;

void observerConfig() {
  caffe2::ClearGlobalNetObservers();
  caffe2::AddGlobalNetObserverCreator([](caffe2::NetBase* subject) {
    return caffe2::make_unique<caffe2::PerfNetObserver>(subject);
  });
  caffe2::ObserverConfig::setReporter(
      caffe2::make_unique<caffe2::NetObserverReporterPrint>());
}

bool backendCudaSet(const string& backend) {
  bool run_on_gpu = false;
  if (backend == "cuda") {
#ifdef __CUDA_ARCH__
    if (caffe2::HasCudaGPU()) {
      run_on_gpu = true;
    } else {
      CAFFE_THROW("NO GPU support on this host machine");
    }
#else
    CAFFE_THROW("NO GPU support");
#endif
  }
  return run_on_gpu;
}

void setDeviceType(caffe2::NetDef* net_def, caffe2::DeviceType& run_dev) {
  for (int j = 0; j < net_def->op_size(); j++) {
    caffe2::OperatorDef* op = net_def->mutable_op(j);
    op->mutable_device_option()->set_device_type(run_dev);
  }
}

void setOperatorEngine(caffe2::NetDef* net_def, const string& backend) {
  if (backend != "builtin") {
    string engine = backend == "nnpack" ? "NNPACK"
                                        : backend == "eigen" ? "EIGEN"
                                                             : backend == "mkl"
                ? "MKLDNN"
                : backend == "cuda" ? "CUDA"
                                    : backend == "default" ? "" : "NONE";
    CAFFE_ENFORCE(engine != "NONE", "Backend is not supported");
    for (int i = 0; i < net_def->op_size(); i++) {
      caffe2::OperatorDef* op_def = net_def->mutable_op(i);
      op_def->set_engine(engine);
    }
  }
}

void loadInput(
    shared_ptr<caffe2::Workspace> workspace,
    const bool run_on_gpu,
    const string& input,
    const string& input_file,
    const string& input_dims,
    const string& input_type) {
  // Load input.
  if (input.size()) {
    vector<string> input_names = caffe2::split(',', input);
    if (input_file.size()) {
      vector<string> input_files = caffe2::split(',', input_file);
      CAFFE_ENFORCE_EQ(
          input_names.size(),
          input_files.size(),
          "Input name and file should have the same number.");
      for (int i = 0; i < input_names.size(); ++i) {
        caffe2::BlobProto blob_proto;
        CAFFE_ENFORCE(caffe2::ReadProtoFromFile(input_files[i], &blob_proto));
        workspace->CreateBlob(input_names[i])->Deserialize(blob_proto);
      }
    } else if (input_dims.size() || input_type.size()) {
      CAFFE_ENFORCE_GE(
          input_dims.size(),
          0,
          "Input dims must be specified when input tensors are used.");
      CAFFE_ENFORCE_GE(
          input_type.size(),
          0,
          "Input type must be specified when input tensors are used.");

      vector<string> input_dims_list = caffe2::split(';', input_dims);
      CAFFE_ENFORCE_EQ(
          input_names.size(),
          input_dims_list.size(),
          "Input name and dims should have the same number of items.");
      vector<string> input_type_list = caffe2::split(';', input_type);
      CAFFE_ENFORCE_EQ(
          input_names.size(),
          input_type_list.size(),
          "Input name and type should have the same number of items.");
      for (size_t i = 0; i < input_names.size(); ++i) {
        vector<string> input_dims_str = caffe2::split(',', input_dims_list[i]);
        vector<int> input_dims;
        for (const string& s : input_dims_str) {
          input_dims.push_back(caffe2::stoi(s));
        }
        caffe2::Blob* blob = workspace->GetBlob(input_names[i]);
        if (blob == nullptr) {
          blob = workspace->CreateBlob(input_names[i]);
        }
        if (run_on_gpu) {
          LOG(INFO) << "Running on GPU.";
#ifdef __CUDA_ARCH__
          caffe2::TensorCUDA* tensor = blob->GetMutable<caffe2::TensorCUDA>();
          CHECK_NOTNULL(tensor);
          tensor->Resize(input_dims);
          if (input_type_list[i] == "uint8_t") {
            tensor->mutable_data<uint8_t>();
          } else if (input_type_list[i] == "float") {
            tensor->mutable_data<float>();
          } else {
            CAFFE_THROW("Unsupported input type: ", input_type_list[i]);
          }
#else
          CAFFE_THROW("Not support GPU on mobile.");
#endif
        } else {
          caffe2::TensorCPU* tensor = blob->GetMutable<caffe2::TensorCPU>();
          CHECK_NOTNULL(tensor);
          tensor->Resize(input_dims);
          if (input_type_list[i] == "uint8_t") {
            tensor->mutable_data<uint8_t>();
          } else if (input_type_list[i] == "float") {
            tensor->mutable_data<float>();
          } else {
            CAFFE_THROW("Unsupported input type: ", input_type_list[i]);
          }
        }
      }
    } else {
      CAFFE_THROW(
          "You requested input tensors, but neither input_file nor "
          "input_dims is set.");
    }
  }
}

void runNetwork(
    shared_ptr<caffe2::Workspace> workspace,
    caffe2::NetDef& net_def,
    const bool run_individual,
    const int warmup,
    const int iter) {
  if (!net_def.has_name()) {
    net_def.set_name("benchmark");
  }

  caffe2::NetBase* net = workspace->CreateNet(net_def);
  CHECK_NOTNULL(net);

  LOG(INFO) << "Starting benchmark.";
  caffe2::ObserverConfig::initSampleRate(1, 1, 1, run_individual, warmup);
  LOG(INFO) << "Running warmup runs.";
  for (int i = 0; i < warmup; ++i) {
    CAFFE_ENFORCE(net->Run(), "Warmup run ", i, " has failed.");
  }

  LOG(INFO) << "Main runs.";
  CAFFE_ENFORCE(
      iter >= 0,
      "Number of main runs should be non negative, provided ",
      iter,
      ".");
  for (int i = 0; i < iter; ++i) {
    caffe2::ObserverConfig::initSampleRate(1, 1, 1, 0, warmup);
    CAFFE_ENFORCE(net->Run(), "Main run ", i, " has failed.");
    if (run_individual) {
      caffe2::ObserverConfig::initSampleRate(1, 1, 1, 1, warmup);
      CAFFE_ENFORCE(net->Run(), "Main run ", i, " with operator has failed.");
    }
  }
}

void writeOutput(
    shared_ptr<caffe2::Workspace> workspace,
    const bool run_on_gpu,
    const string& output,
    const string& output_folder,
    const bool text_output) {
  string output_prefix = output_folder.size() ? output_folder + "/" : "";
  if (output.size()) {
    vector<string> output_names = caffe2::split(',', output);
    if (output == "*") {
      output_names = workspace->Blobs();
    }
    for (const string& name : output_names) {
      CAFFE_ENFORCE(
          workspace->HasBlob(name),
          "You requested a non-existing blob: ",
          name);
      if (text_output) {
        if (run_on_gpu) {
#ifdef __CUDA_ARCH__
          writeTextOutput<caffe2::CUDAContext, caffe2::TensorCUDA>(
              workspace->GetBlob(name)->GetMutable<caffe2::TensorCUDA>(),
              output_prefix,
              name);
#else
          CAFFE_THROW("Not support GPU.");
#endif
        } else {
          writeTextOutput<caffe2::CPUContext, caffe2::TensorCPU>(
              workspace->GetBlob(name)->GetMutable<caffe2::TensorCPU>(),
              output_prefix,
              name);
        }
      } else {
        string serialized = workspace->GetBlob(name)->Serialize(name);
        string output_filename = output_prefix + name;
        caffe2::WriteStringToFile(serialized, output_filename.c_str());
      }
    }
  }
}
