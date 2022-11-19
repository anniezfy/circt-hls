//===- WrapGen.cpp - HLT wrapper generation tool --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the hlt-wrapgen tool. Based on an input reference
// function, a target kernel operation is wrapped to generate a .cpp file
// suitable for interaction with the HLT simulation library.
//
//===----------------------------------------------------------------------===//

#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/Parser/Parser.h"

#include "llvm/ADT/TypeSwitch.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SourceMgr.h"

#include "circt/Dialect/FIRRTL/FIRRTLDialect.h"
#include "circt/Dialect/Handshake/HandshakeDialect.h"
#include "circt/Dialect/Handshake/HandshakeOps.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/IR/SCF.h"

#include "circt-hls/Tools/hlt/WrapGen/BaseWrapper.h"
#include "circt-hls/Tools/hlt/WrapGen/calyx/CalyxVerilatorWrapper.h"
#include "circt-hls/Tools/hlt/WrapGen/handshake/HandshakeVerilatorWrapper.h"
#include "circt-hls/Tools/hlt/WrapGen/std/StdWrapper.h"

using namespace llvm;
using namespace mlir;
using namespace circt;
using namespace circt_hls;

// Used for the software interface.
static cl::opt<std::string>
    inputFunctionFilename("func", cl::Required,
                          cl::desc("<builtin function filename>"),
                          cl::init("-"));

// Used for the transactional interface.
static cl::opt<std::string>
    inputReferenceFilename("ref", cl::Optional,
                           cl::desc("<Reference function filename>"),
                           cl::init("-"));

// Used for the hardware interface.
static cl::opt<std::string> inputKernelFilename("kernel", cl::Optional,
                                                cl::desc("<Kernel filename>"),
                                                cl::init("-"));

static cl::opt<std::string> outputDirectory("o", cl::Required,
                                            cl::desc("<output directory>"),
                                            cl::init("-"));

static cl::opt<std::string>
    functionName("name", cl::Required,
                 cl::desc("The name of the function to wrap"), cl::init("-"));

enum class KernelType { HandshakeFIRRTL, Calyx, Standard };

static cl::opt<KernelType> kernelType(
    "type", cl::Required,
    cl::desc("The type of the kernel to wrap. This will guide "
             "which wrapper is used, and what operation type the "
             "source kernel operation should be."),
    cl::init(KernelType::HandshakeFIRRTL),
    llvm::cl::values(
        clEnumValN(KernelType::HandshakeFIRRTL, "handshakeFIRRTL",
                   "Use the Handshake wrapper"),
        clEnumValN(KernelType::Calyx, "calyx", "Use the Calyx wrapper"),
        clEnumValN(KernelType::Standard, "std", "Use the standard wrapper")));

namespace circt_hls {

/// Instantiates a wrapper based on the type of the kernel operation.
static std::unique_ptr<BaseWrapper> getWrapper() {
  switch (kernelType) {
  case KernelType::HandshakeFIRRTL:
    return std::make_unique<HandshakeVerilatorWrapper>(outputDirectory);
  case KernelType::Standard:
    return std::make_unique<StdWrapper>(outputDirectory);
  case KernelType::Calyx:
    return std::make_unique<CalyxVerilatorWrapper>(outputDirectory);
  }
}

/// Container for the current set of loaded modules.
static SmallVector<mlir::OwningOpRef<mlir::ModuleOp>> modules;

/// Load a module from the argument file fn into the modules vector.
static ModuleOp getModule(MLIRContext *ctx, StringRef fn) {
  auto file_or_err = MemoryBuffer::getFileOrSTDIN(fn);
  if (std::error_code error = file_or_err.getError()) {
    errs() << "Error: Could not open input file '" << fn
           << "': " << error.message() << "\n";
    return nullptr;
  }

  // Load the MLIR module.
  SourceMgr source_mgr;
  source_mgr.AddNewSourceBuffer(std::move(*file_or_err), SMLoc());
  modules.emplace_back(mlir::parseSourceFile<ModuleOp>(source_mgr, ctx));
  if (!modules.back()) {
    errs() << "Error: Found no modules in input file '" << fn << "'\n";
    return nullptr;
  }
  return modules.back().get();
}

/// Locates the operation defining the provided symbol within the set of loaded
/// modules.
static mlir::Operation *getOpToWrap(mlir::MLIRContext *ctx, StringRef fn,
                                    StringRef symbol) {
  auto file_or_err = MemoryBuffer::getFileOrSTDIN(fn);
  if (std::error_code error = file_or_err.getError()) {
    errs() << ": could not open input file '" << fn << "': " << error.message()
           << "\n";
    return nullptr;
  }

  auto mod = getModule(ctx, fn);
  if (!mod) {
    errs() << "No module in file: " << fn << "\n";
    return nullptr;
  }

  Operation *targetOp = nullptr;
  SymbolTable::walkSymbolTables(
      mod, /*unused*/ false, [&](Operation *symOp, bool) {
        Operation *op = SymbolTable::lookupSymbolIn(symOp, symbol);
        if (op) {
          assert(targetOp == nullptr);
          targetOp = op;
        }
      });

  if (!targetOp) {
    errs() << "Found no definitions of symbol '" << symbol << "' in '" << fn
           << "'\n";
    return nullptr;
  }
  return targetOp;
}

static void registerDialects(mlir::DialectRegistry &registry) {
  registry.insert<mlir::memref::MemRefDialect>();
  registry.insert<mlir::cf::ControlFlowDialect>();
  registry.insert<arith::ArithDialect>();
  registry.insert<scf::SCFDialect>();
  registry.insert<handshake::HandshakeDialect>();
  registry.insert<firrtl::FIRRTLDialect>();
  registry.insert<LLVM::LLVMDialect>();
  registry.insert<calyx::CalyxDialect>();
  registry.insert<hw::HWDialect>();
  registry.insert<cf::ControlFlowDialect>();
  registry.insert<func::FuncDialect>();
}

} // namespace circt_hls

static Operation *getOpToWrapErroring(MLIRContext *ctx, StringRef fileName,
                                      StringRef symbolName) {
  Operation *op = nullptr;
  if (!fileName.empty()) {
    op = circt_hls::getOpToWrap(ctx, fileName, symbolName);
    if (!op) {
      errs() << "No symbol named '" << symbolName << "' found in '" << fileName
             << "\n";
    }
  }
  return op;
}

int main(int argc, char **argv) {
  InitLLVM y(argc, argv);
  cl::ParseCommandLineOptions(argc, argv, "hlt test wrapper generator\n\n");

  if (!sys::fs::exists(outputDirectory)) {
    errs() << "Output directory '" << outputDirectory << "' does not exist!\n";
    return 1;
  }

  mlir::DialectRegistry registry;
  circt_hls::registerDialects(registry);
  mlir::MLIRContext context(registry);
  context.allowUnregisteredDialects();

  Operation *funcOpPtr =
      getOpToWrapErroring(&context, inputFunctionFilename, functionName);
  if (!funcOpPtr)
    return 1;

  auto funcOp = dyn_cast<mlir::func::FuncOp>(funcOpPtr);
  if (!funcOp) {
    errs() << "Expected --func to be a builtin.func\n";
    return 1;
  }

  Operation *refOp =
      getOpToWrapErroring(&context, inputReferenceFilename, functionName);
  if (!refOp)
    return 1;

  Operation *kernelOp = nullptr;
  if (inputKernelFilename.getNumOccurrences() != 0) {
    kernelOp = getOpToWrapErroring(&context, inputKernelFilename, functionName);
    if (!kernelOp)
      return 1;
  }

  /// Locate wrapping handler for the operation.
  auto wrapper = circt_hls::getWrapper();
  if (!wrapper)
    return 1;

  /// Go wrap!
  if (wrapper->wrap(funcOp, refOp, kernelOp).failed())
    return 1;
}
