//===- HandshakeVerilatorWrapper.cpp - Handshake Verilator wrapper --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains the implementation of the HandshakeVerilatorWrapper class,
// an HLT wrapper for wrapping handshake.funcOp based kernels, simulated by
// Verilator.
//
//===----------------------------------------------------------------------===//

#include "circt-hls/Tools/hlt/WrapGen/handshake/HandshakeVerilatorWrapper.h"
#include "circt-hls/Tools/hlt/WrapGen/VerilatorEmitterUtils.h"

using namespace llvm;
using namespace mlir;
using namespace circt;

namespace circt_hls {

LogicalResult HandshakeVerilatorWrapper::init(Operation *refOp,
                                              Operation *kernelOp) {
  if (!(refOp && kernelOp))
    return refOp->emitError()
           << "Expected both a reference and a kernel operation for wrapping a "
              "handshake simulator.";

  auto hsRefOp = dyn_cast<handshake::FuncOp>(refOp);
  if (!hsRefOp)
    return refOp->emitOpError()
           << "expected reference operation to be a handshake.func operation.";
  auto firrtlLikeOp = dyn_cast<firrtl::FModuleLike>(kernelOp);
  if (!firrtlLikeOp)
    return firrtlLikeOp->emitOpError() << "expected reference operation to be "
                                          "a firrtl FModuleLike operation.";
  hsOp = hsRefOp;
  firrtlOp = firrtlLikeOp;
  return success();
}

SmallVector<std::string> HandshakeVerilatorWrapper::getIncludes() {
  SmallVector<std::string> includes;
  includes.push_back(("V" + funcName() + ".h").str());
  includes.push_back("circt-hls/Tools/hlt/Simulator/HandshakeSimInterface.h");
  includes.push_back("circt-hls/Tools/hlt/Simulator/SimDriver.h");
  includes.push_back("cstdint");
  return includes;
}

LogicalResult HandshakeVerilatorWrapper::emitPreamble(Operation *kernelOp) {
  auto handshakeFirMod = dyn_cast<firrtl::FModuleLike>(kernelOp);
  if (!handshakeFirMod)
    return kernelOp->emitOpError() << "Expected a FIRRTL module of the "
                                      "handshake kernel that is to be wrapped";

  if (emitIOTypes(emitVerilatorType).failed())
    return failure();

  // Emit model type.
  osi() << "using TModel = V" << funcName() << ";\n";
  osi() << "using " << funcName()
        << "SimInterface = HandshakeSimInterface<TInput, TOutput, "
           "TModel>;\n\n";

  // Emit simulator.
  firrtlOp = handshakeFirMod;
  if (emitSimulator().failed())
    return failure();

  // Emit simulator driver type.
  osi() << "using TSim = " << funcName() << "Sim;\n";
  return success();
}

std::string HandshakeVerilatorWrapper::getResName(unsigned idx) {
  return firrtlOp.getPortName(idx + inCtrlIdx() + 1).str();
}
std::string HandshakeVerilatorWrapper::getInputName(unsigned idx) {
  return firrtlOp.getPortName(idx).str();
}

unsigned
HandshakeVerilatorWrapper::getBundleDataWidth(firrtl::BundleType bundleType) {
  auto dataSig = bundleType.getElement("data");
  assert(dataSig.hasValue() && "Expected bundle to have a data signal");
  return dataSig.getValue().type.getBitWidthOrSentinel();
}

unsigned HandshakeVerilatorWrapper::inCtrlIdx() {
  return funcOp.getNumArguments();
}

LogicalResult HandshakeVerilatorWrapper::emitSimulator() {
  osi() << "class " << funcName() << "Sim : public " << funcName()
        << "SimInterface {\n";
  osi() << "public:\n";
  osi().indent();

  osi() << funcName() << "Sim() : " << funcName() << "SimInterface() {\n";
  osi().indent();

  osi() << "// --- Generic Verilator interface\n";
  osi() << "interface.clock = &dut->clock;\n";
  osi() << "interface.reset = &dut->reset;\n\n";

  auto inCtrlName = getInputName(inCtrlIdx());
  auto outCtrlName = getResName(funcOp.getNumResults());
  osi() << "// --- Handshake interface\n";
  osi() << "inCtrl = std::make_unique<HandshakeInPort>(&dut->" << inCtrlName
        << "_ready, &dut->" << inCtrlName << "_valid);\n";
  osi() << "outCtrl = std::make_unique<HandshakeOutPort>(&dut->" << outCtrlName
        << "_ready, &dut->" << outCtrlName << "_valid);\n";
  osi() << "\n";

  // We expect equivalence between the order of function arguments and the ports
  // of the FIRRTL module. Additionally, the handshake layer has then added
  // one more argument and return port for the control signals.

  osi() << "// --- Software interface\n";
  auto funcType = funcOp.getFunctionType();
  osi() << "// - Input ports\n";
  for (auto &input : enumerate(funcType.getInputs())) {
    if (emitInputPort(input.value(), input.index()).failed())
      return failure();
  }
  osi() << "\n// - Output ports\n";
  for (auto &res : enumerate(funcType.getResults())) {
    if (emitOutputPort(res.value(), res.index()).failed())
      return failure();
  }
  osi().unindent();
  osi() << "};\n";

  osi().unindent();
  osi() << "};\n\n";
  return success();
}

static raw_indented_ostream &emitHSPortCtor(raw_indented_ostream &os,
                                            StringRef prefix,
                                            bool hasData = true) {
  os << "(\"" << prefix << "\", ";
  os << "&dut->" << prefix << "_ready, ";
  os << "&dut->" << prefix << "_valid";
  if (hasData)
    os << ", &dut->" << prefix << "_data";
  os << ")";
  return os;
}

static raw_indented_ostream &emitHSTypeAssert(raw_indented_ostream &os,
                                              StringRef type, StringRef value,
                                              StringRef err) {
  os << "static_assert(std::is_same<" << type << ", typeof(" << value
     << ")>::value, \"" << err << "\");\n";
  return os;
}

LogicalResult HandshakeVerilatorWrapper::emitExtMemPort(MemRefType memref,
                                                        unsigned idx) {
  auto shape = memref.getShape();
  assert(shape.size() == 1 && "Only support unidimensional memories");
  std::string name = getInputName(idx);

  // Find any ldAddr#/stAddr# bundle within the memory port
  auto bundleType = firrtlOp.getPortType(idx).cast<firrtl::BundleType>();
  unsigned addrWidth = 0;
  for (auto sig : bundleType.getElements()) {
    if (sig.name.strref().startswith("ldAddr") ||
        sig.name.strref().startswith("stAddr"))
      addrWidth = getBundleDataWidth(sig.type.cast<firrtl::BundleType>());
  }
  assert(addrWidth > 0 && "Found no address signal in memory bundle!");

  osi() << "auto " << name << " = addInputPort<HandshakeMemoryInterface<";
  if (emitVerilatorType(osi(), hsOp.getLoc(), memref.getElementType()).failed())
    return failure();
  osi() << ", ";
  if (emitVerilatorTypeFromWidth(osi(), hsOp.getLoc(), addrWidth).failed())
    return failure();
  osi() << ">>(/*size=*/" << shape.front() << ");\n";

  // Locate the external memory operation referencing the input
  auto extMemUsers = hsOp.getArgument(idx).getUsers();
  assert(std::distance(extMemUsers.begin(), extMemUsers.end()) == 1 &&
         "expected exactly 1 user of a memref input argument");
  handshake::ExternalMemoryOp extMemOp =
      cast<handshake::ExternalMemoryOp>(*extMemUsers.begin());

  // Load ports
  for (unsigned i = 0; i < extMemOp.getLdCount(); ++i) {
    osi() << name << "->addLoadPort(\n";
    osi().indent();

    // Data port
    osi() << "std::make_shared<HandshakeDataInPort<";
    if (emitVerilatorType(os(), hsOp.getLoc(), memref.getElementType())
            .failed())
      return failure();
    osi() << ">>";
    emitHSPortCtor(osi(), name + "_ldData" + std::to_string(i));
    osi() << ",\n";

    // Address port
    osi() << "std::make_shared<HandshakeDataOutPort<";
    if (emitVerilatorTypeFromWidth(os(), hsOp.getLoc(), addrWidth).failed())
      return failure();
    osi() << ">>";
    emitHSPortCtor(osi(), name + "_ldAddr" + std::to_string(i));
    osi() << ",\n";

    // done port
    osi() << "std::make_shared<HandshakeInPort>";
    emitHSPortCtor(osi(), name + "_ldDone" + std::to_string(i),
                   /*hasData=*/false);
    osi() << ");\n";
    osi().unindent();
  }

  // Store ports
  for (unsigned i = 0; i < extMemOp.getStCount(); ++i) {
    osi() << name << "->addStorePort(\n";
    osi().indent();

    // Data port
    osi() << "std::make_shared<HandshakeDataOutPort<";
    if (emitVerilatorType(os(), hsOp.getLoc(), memref.getElementType())
            .failed())
      return failure();
    osi() << ">>";
    emitHSPortCtor(osi(), name + "_stData" + std::to_string(i));
    osi() << ",\n";

    // Address port
    osi() << "std::make_shared<HandshakeDataOutPort<";
    if (emitVerilatorTypeFromWidth(os(), hsOp.getLoc(), addrWidth).failed())
      return failure();
    osi() << ">>";
    emitHSPortCtor(osi(), name + "_stAddr" + std::to_string(i));
    osi() << ",\n";

    // done port
    osi() << "std::make_shared<HandshakeInPort>";
    emitHSPortCtor(osi(), name + "_stDone" + std::to_string(i),
                   /*hasData=*/false);
    osi() << ");\n";
    osi().unindent();
  }
  return success();
}

LogicalResult HandshakeVerilatorWrapper::emitInputPort(Type t, unsigned idx) {
  if (auto memref = t.dyn_cast<MemRefType>(); memref) {
    return emitExtMemPort(memref, idx);
  } else {
    auto arg = getInputName(idx);
    auto dataType = "TArg" + std::to_string(idx);

    // Emit a static assert on the data type and the expected result type to
    // avoid funky template errors.
    emitHSTypeAssert(
        osi(), dataType, "dut->" + arg + "_data",
        "Type mismatch between handshake data port type and actual "
        "port type. This might be a verilator version issue");

    osi() << "addInputPort<HandshakeDataInPort<" << dataType << ">>";
    emitHSPortCtor(osi(), arg) << ";\n";
  }
  return success();
}
LogicalResult HandshakeVerilatorWrapper::emitOutputPort(Type t, unsigned idx) {
  auto arg = getResName(idx);
  auto dataType = "TRes" + std::to_string(idx);

  // Emit a static assert on the data type and the expected result type to avoid
  // funky template errors.
  emitHSTypeAssert(osi(), dataType, "dut->" + arg + "_data",
                   "Type mismatch between handshake data port type and actual "
                   "port type. This might be a verilator version issue");

  osi() << "addOutputPort<HandshakeDataOutPort<" << dataType << ">>";
  emitHSPortCtor(osi(), arg) << ";\n";
  return success();
}

} // namespace circt_hls
