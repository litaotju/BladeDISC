/* Copyright 2021 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include <fstream>

#include "absl/strings/str_cat.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "mlir/Dialect/GPU/Passes.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"
#include "mlir/Target/LLVMIR/Dialect/LLVMIR/LLVMToLLVMIRTranslation.h"
#include "mlir/Target/LLVMIR/Dialect/NVVM/NVVMToLLVMIRTranslation.h"
#include "mlir/Target/LLVMIR/Export.h"
#include "mlir/Transforms/DialectConversion.h"
#include "tensorflow/compiler/mlir/disc/transforms/PassDetail.h"
#include "tensorflow/compiler/xla/debug_options_flags.h"
#include "tensorflow/compiler/xla/service/gpu/gpu_asm_opts_util.h"
#include "tensorflow/compiler/xla/service/gpu/llvm_gpu_backend/gpu_backend_lib.h"
#include "tensorflow/compiler/xla/service/gpu/llvm_gpu_backend/utils.h"
#include "tensorflow/compiler/xla/service/gpu/stream_executor_util.h"
#include "tensorflow/compiler/xla/service/gpu/target_constants.h"
#include "tensorflow/compiler/xla/service/hlo_module_config.h"
#include "tensorflow/compiler/xla/service/llvm_ir/llvm_type_conversion_util.h"
#include "tensorflow/compiler/xla/status.h"
#include "tensorflow/compiler/xla/statusor.h"
#include "tensorflow/core/platform/cuda_libdevice_path.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/path.h"
#include "tensorflow/core/platform/random.h"
#include "tensorflow/core/util/env_var.h"
#include "transforms/codegen_utils.h"

#if defined(GOOGLE_CUDA) || defined(TENSORFLOW_USE_ROCM)
#include "tensorflow/stream_executor/gpu/asm_compiler.h"
#endif

#if TENSORFLOW_USE_ROCM
#include "tensorflow/core/platform/rocm_rocdl_path.h"
#include "tensorflow/stream_executor/rocm/rocm_driver_wrapper.h"
#endif

#if TENSORFLOW_USE_ROCM
#define CUDA_SUCCESS hipSuccess
#endif

namespace mlir {
namespace disc_ral {
namespace {

using xla::InternalError;
using xla::llvm_ir::AsArrayRef;
using xla::llvm_ir::AsStringRef;

class GpuKernelToBlobPass
    : public GpuKernelToBlobPassBase<GpuKernelToBlobPass> {
 public:
  GpuKernelToBlobPass(int cc_major, int cc_minor, bool multi_cc_support,
                      bool multi_cc_support_dbg_ptx_only,
                      StringRef blob_annotation) {
    blob_annotation_ = blob_annotation.str();
    cc_major_ = cc_major;
    cc_minor_ = cc_minor;
    multi_cc_support_ = multi_cc_support;
    multi_cc_support_dbg_ptx_only_ = multi_cc_support_dbg_ptx_only;
  }

  void runOnOperation() override {
    mlir::gpu::GPUModuleOp gpu_module = getOperation();
    if (multi_cc_support_) {
      VLOG(2) << "Multi compute capability support";
      for (auto item : c_MULTI_SM_CONFIG) {
        const std::string& name = item.first;
        if (multi_cc_support_dbg_ptx_only_) {
          if (name.find("compute") == std::string::npos) continue;
          VLOG(2) << "Multi compute capability support with PTX only";
        }

        auto blob_or = GetGpuBinaryBlob(gpu_module, std::get<0>(item.second),
                                        std::get<1>(item.second),
                                        std::get<2>(item.second));
        if (!blob_or.ok()) {
          gpu_module.emitError(blob_or.status().error_message());
          return signalPassFailure();
        }
        const auto& blob = blob_or.ValueOrDie();
        std::string blob_string(blob.begin(), blob.end());
        std::string attr_str = std::string(kGpuBinaryAttrName) + "_" + name;
        gpu_module->setAttr(attr_str,
                            mlir::StringAttr::get(&getContext(), blob_string));
      }
    } else {
      VLOG(2) << "JIT mode";
      auto blob_or = GetGpuBinaryBlob(gpu_module, cc_major_, cc_minor_);
      if (!blob_or.ok()) {
        gpu_module.emitError(blob_or.status().error_message());
        return signalPassFailure();
      }
      const auto& blob = blob_or.ValueOrDie();
      std::string blob_string(blob.begin(), blob.end());
      gpu_module->setAttr(blob_annotation_,
                          mlir::StringAttr::get(&getContext(), blob_string));
    }
  }

  static xla::Status ExecuteProgram(const std::string& program,
                                    const std::vector<llvm::StringRef>& args) {
    std::string error_message;
    int result = llvm::sys::ExecuteAndWait(
        program, AsArrayRef(args), llvm::None, {}, 0, 0, &error_message);
    if (result) {
      return xla::InternalError("llc execute fail: %s, error code %d",
                                error_message, result);
    }
    return xla::Status::OK();
  }

  xla::StatusOr<std::vector<uint8_t>> GetGpuBinaryBlob(
      mlir::gpu::GPUModuleOp gpu_module, int cc_major, int cc_minor,
      bool virtual_compute_arch = false) {
    llvm::LLVMContext llvmContext;
    auto llvmModule = mlir::translateModuleToLLVMIR(gpu_module, llvmContext);

#if TENSORFLOW_USE_DCU
    const std::string arch_str = "gfx906";
    std::string libdevice_dir = tensorflow::RocdlRoot();
    TF_RETURN_IF_ERROR(xla::gpu::LinkWithBitcodeVector(
        llvmModule.get(), xla::gpu::GetROCDLPaths(arch_str, libdevice_dir)));

    std::string llvm_path_1;
    tensorflow::ReadStringFromEnvVar("DISC_DCU_BACKEND_PATH",
                                     "/opt/dtk-21.10/llvm/bin/", &llvm_path_1);
    std::string llvm_path_2 = tensorflow::io::JoinPath("/opt/rocm", "hcc/bin");
    std::string llvm_path_3 = tensorflow::io::JoinPath("/opt/rocm", "llvm/bin");
    auto llc_program = llvm::sys::findProgramByName(
        "llc", {llvm_path_1, llvm_path_2, llvm_path_3});
    auto lld_program = llvm::sys::findProgramByName(
        "ld.lld", {llvm_path_1, llvm_path_2, llvm_path_3});
    if (!llc_program || !lld_program) {
      return xla::InternalError("unable to find llc or ld.lld in PATH: %s, %s",
                                llc_program.getError().message(),
                                lld_program.getError().message());
    }
    VLOG(2) << "llc found in path: " << *llc_program
            << ", ld.lld found in path: " << *lld_program;

    std::string random_number = std::to_string(tensorflow::random::New64());
    std::string tmp_path = "/tmp/";
    std::string ll_path = tmp_path + random_number + ".ll";
    std::string isabin_path = tmp_path + random_number + ".o";
    std::string hsaco_path = tmp_path + random_number + ".hsaco";

    std::error_code ec;
    std::unique_ptr<llvm::raw_fd_ostream> ll_fs(
        new llvm::raw_fd_ostream(ll_path, ec, llvm::sys::fs::OF_None));
    llvmModule->print(*ll_fs, nullptr);
    ll_fs->flush();

    std::string opt_level;
    tensorflow::ReadStringFromEnvVar("DISC_DCU_BACKEND_OPT_LEVEL", "3",
                                     &opt_level);
    if (VLOG_IS_ON(2)) {
      // Dump asm file
      std::string asm_path = tmp_path + random_number + ".asm";
      std::vector<llvm::StringRef> llc_args{
          AsStringRef("llc"),
          AsStringRef("-O"),
          AsStringRef(opt_level),
          AsStringRef("-mtriple"),
          AsStringRef("amdgcn-amd-amdhsa"),
          AsStringRef("-mcpu"),
          AsStringRef(arch_str),
          AsStringRef("-amdhsa-code-object-version"),
          AsStringRef("3"),
          AsStringRef("-filetype"),
          AsStringRef("asm"),
          AsStringRef("-o"),
          AsStringRef(asm_path),
          AsStringRef(ll_path)};
      TF_RETURN_IF_ERROR(ExecuteProgram(*llc_program, llc_args));
      VLOG(0) << "Asm file is dumped to: " << asm_path;
    }
    std::vector<llvm::StringRef> llc_args{
        AsStringRef("llc"),
        AsStringRef("-O"),
        AsStringRef(opt_level),
        AsStringRef("-mtriple"),
        AsStringRef("amdgcn-amd-amdhsa"),
        AsStringRef("-mcpu"),
        AsStringRef(arch_str),
        AsStringRef("-amdhsa-code-object-version"),
        AsStringRef("3"),
        AsStringRef("-filetype"),
        AsStringRef("obj"),
        AsStringRef("-o"),
        AsStringRef(isabin_path),
        AsStringRef(ll_path)};
    TF_RETURN_IF_ERROR(ExecuteProgram(*llc_program, llc_args));

    std::vector<llvm::StringRef> lld_args{
        AsStringRef("ld.lld"),  AsStringRef("-flavor"),   AsStringRef("gnu"),
        AsStringRef("-shared"), AsStringRef(isabin_path), AsStringRef("-o"),
        AsStringRef(hsaco_path)};
    TF_RETURN_IF_ERROR(ExecuteProgram(*lld_program, lld_args));

    // Read HSACO.
    std::ifstream hsaco_file(hsaco_path, std::ios::binary | std::ios::ate);
    std::ifstream::pos_type hsaco_file_size = hsaco_file.tellg();

    std::vector<uint8_t> hsaco(hsaco_file_size);
    hsaco_file.seekg(0, std::ios::beg);
    hsaco_file.read(reinterpret_cast<char*>(&hsaco[0]), hsaco_file_size);
    hsaco_file.close();
    bool keep_tempfiles = false;
    TF_CHECK_OK(tensorflow::ReadBoolFromEnvVar("DISC_ROCM_KEEP_TEMPFILES",
                                               /*default_val=*/false,
                                               &keep_tempfiles));
    if (!keep_tempfiles) {
      remove(ll_path.c_str());
      remove(isabin_path.c_str());
      remove(hsaco_path.c_str());
    }
    return hsaco;

#elif TENSORFLOW_USE_DCU_WITH_LLVM_ROCM_BACKEND
    xla::HloModuleConfig config;
    xla::DebugOptions options = xla::GetDebugOptionsFromFlags();
    config.set_debug_options(options);

    std::string arch_str = "gfx906";
    // Parse ROCm architecture.
    absl::string_view consumable_arch(arch_str);
    if (!absl::ConsumePrefix(&consumable_arch, "gfx")) {
      return tensorflow::errors::Internal(
          "Could not parse ROCm architecture prefix (expected gfx)");
    }
    std::string libdevice_dir = tensorflow::RocdlRoot();
    auto llvm_module_copy = llvm::CloneModule(*llvmModule);
    xla::gpu::GpuVersion gpu_version{arch_str};
    auto hsaco_or = xla::gpu::amdgpu::CompileToHsaco(
        llvm_module_copy.get(), gpu_version, config, libdevice_dir);
    return hsaco_or;

#elif TENSORFLOW_USE_ROCM
    return InternalError("ROCM devices except for DCU is not implemented yet");

#elif GOOGLE_CUDA
    if (!llvmModule) {
      return InternalError("Could not translate MLIR module to NVVM");
    }

    llvmModule->setModuleIdentifier("acme");
    llvmModule->setDataLayout(xla::gpu::nvptx::DataLayout());

    xla::HloModuleConfig config;
    xla::DebugOptions options = xla::GetDebugOptionsFromFlags();
    // Make sure we use full precision division operations.
    // We need this to pass the ut for `tf.FloorDiv`.
    (*options.mutable_xla_backend_extra_options())["-nvptx-prec-divf32"] = "2";
    config.set_debug_options(options);

    auto enable_fusion = [](llvm::TargetMachine* target) {
      target->Options.AllowFPOpFusion = llvm::FPOpFusion::FPOpFusionMode::Fast;
    };

    // Compile and collect requested fatbin and PTX images.
    std::vector<tensorflow::se::CubinOrPTXImage> images;
    TF_ASSIGN_OR_RETURN(std::string libdevice_dir, GetLibdeviceDir(config));
    auto gpu_asm_opts =
        xla::gpu::PtxOptsFromDebugOptions(config.debug_options());

    TF_ASSIGN_OR_RETURN(
        std::string ptx,
        xla::gpu::nvptx::CompileToPtx(
            llvmModule.get(),
            tensorflow::se::CudaComputeCapability{cc_major, cc_minor}, config,
            libdevice_dir, enable_fusion));

    VLOG(1) << "PTX code: \n" << ptx;

    std::vector<uint8_t> gpu_asm;
    if (virtual_compute_arch) {
      std::vector<uint8_t> ptx_bytes;
      std::copy(ptx.begin(), ptx.end(), std::back_inserter(ptx_bytes));
      tensorflow::se::CubinOrPTXImage image{
          absl::StrCat("compute_", cc_major, cc_minor), std::move(ptx_bytes)};
      TF_ASSIGN_OR_RETURN(gpu_asm,
                          tensorflow::se::BundleGpuAsm({image}, gpu_asm_opts));
    } else {
      TF_ASSIGN_OR_RETURN(std::vector<uint8_t> cubin,
                          tensorflow::se::CompileGpuAsm(
                              cc_major, cc_minor, ptx.c_str(), gpu_asm_opts));
      tensorflow::se::CubinOrPTXImage image{
          absl::StrCat("sm_", cc_major, cc_minor), std::move(cubin)};
      TF_ASSIGN_OR_RETURN(gpu_asm,
                          tensorflow::se::BundleGpuAsm({image}, gpu_asm_opts));
    }
    return gpu_asm;

#endif

    return InternalError(
        "Neither TENSORFLOW_USE_ROCM nor GOOGLE_CUDA are defined."
        " Did you specify either --config=rocm or --config=cuda ?");
  }

 protected:
  void getDependentDialects(DialectRegistry& registry) const override {
    registerLLVMDialectTranslation(registry);
    registerNVVMDialectTranslation(registry);
    OperationPass<gpu::GPUModuleOp>::getDependentDialects(registry);
  }

 private:
  xla::StatusOr<std::string> GetLibdeviceDir(
      const xla::HloModuleConfig& hlo_module_config) {
    for (const std::string& cuda_root : tensorflow::CandidateCudaRoots(
             hlo_module_config.debug_options().xla_gpu_cuda_data_dir())) {
      std::string libdevice_dir =
          tensorflow::io::JoinPath(cuda_root, "nvvm", "libdevice");
      VLOG(2) << "Looking for libdevice at " << libdevice_dir;
      if (tensorflow::Env::Default()->IsDirectory(libdevice_dir).ok()) {
        VLOG(2) << "Found libdevice dir " << libdevice_dir;
        return libdevice_dir;
      }
    }
    return InternalError(
        "Can't find libdevice directory ${CUDA_DIR}/nvvm/libdevice");
  }
};

}  // namespace

std::unique_ptr<OperationPass<gpu::GPUModuleOp>> CreateDiscGpuKernelToBlobPass(
    int cc_major, int cc_minor, bool multi_cc_support,
    bool multi_cc_support_dbg_ptx_only, mlir::StringRef blob_annotation) {
  return std::make_unique<GpuKernelToBlobPass>(
      cc_major, cc_minor, multi_cc_support, multi_cc_support_dbg_ptx_only,
      blob_annotation);
}

}  // namespace disc_ral
}  // namespace mlir
