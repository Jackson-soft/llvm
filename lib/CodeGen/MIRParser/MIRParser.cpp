//===- MIRParser.cpp - MIR serialization format parser implementation -----===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the class that parses the optional LLVM IR and machine
// functions that are stored in MIR files.
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/MIRParser/MIRParser.h"
#include "MIParser.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/AsmParser/SlotMapping.h"
#include "llvm/CodeGen/GlobalISel/RegisterBank.h"
#include "llvm/CodeGen/GlobalISel/RegisterBankInfo.h"
#include "llvm/CodeGen/MIRYamlMapping.h"
#include "llvm/CodeGen/MachineConstantPool.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/ValueSymbolTable.h"
#include "llvm/Support/LineIterator.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SMLoc.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/YAMLTraits.h"
#include <memory>

using namespace llvm;

namespace llvm {

/// This class implements the parsing of LLVM IR that's embedded inside a MIR
/// file.
class MIRParserImpl {
  SourceMgr SM;
  StringRef Filename;
  LLVMContext &Context;
  StringMap<std::unique_ptr<yaml::MachineFunction>> Functions;
  SlotMapping IRSlots;
  /// Maps from register class names to register classes.
  StringMap<const TargetRegisterClass *> Names2RegClasses;
  /// Maps from register bank names to register banks.
  StringMap<const RegisterBank *> Names2RegBanks;

public:
  MIRParserImpl(std::unique_ptr<MemoryBuffer> Contents, StringRef Filename,
                LLVMContext &Context);

  void reportDiagnostic(const SMDiagnostic &Diag);

  /// Report an error with the given message at unknown location.
  ///
  /// Always returns true.
  bool error(const Twine &Message);

  /// Report an error with the given message at the given location.
  ///
  /// Always returns true.
  bool error(SMLoc Loc, const Twine &Message);

  /// Report a given error with the location translated from the location in an
  /// embedded string literal to a location in the MIR file.
  ///
  /// Always returns true.
  bool error(const SMDiagnostic &Error, SMRange SourceRange);

  /// Try to parse the optional LLVM module and the machine functions in the MIR
  /// file.
  ///
  /// Return null if an error occurred.
  std::unique_ptr<Module> parse();

  /// Parse the machine function in the current YAML document.
  ///
  /// \param NoLLVMIR - set to true when the MIR file doesn't have LLVM IR.
  /// A dummy IR function is created and inserted into the given module when
  /// this parameter is true.
  ///
  /// Return true if an error occurred.
  bool parseMachineFunction(yaml::Input &In, Module &M, bool NoLLVMIR);

  /// Initialize the machine function to the state that's described in the MIR
  /// file.
  ///
  /// Return true if error occurred.
  bool initializeMachineFunction(MachineFunction &MF);

  bool initializeRegisterInfo(PerFunctionMIParsingState &PFS,
                              const yaml::MachineFunction &YamlMF);

  void inferRegisterInfo(const PerFunctionMIParsingState &PFS,
                         const yaml::MachineFunction &YamlMF);

  bool initializeFrameInfo(PerFunctionMIParsingState &PFS,
                           const yaml::MachineFunction &YamlMF);

  bool parseCalleeSavedRegister(PerFunctionMIParsingState &PFS,
                                std::vector<CalleeSavedInfo> &CSIInfo,
                                const yaml::StringValue &RegisterSource,
                                int FrameIdx);

  bool parseStackObjectsDebugInfo(PerFunctionMIParsingState &PFS,
                                  const yaml::MachineStackObject &Object,
                                  int FrameIdx);

  bool initializeConstantPool(PerFunctionMIParsingState &PFS,
                              MachineConstantPool &ConstantPool,
                              const yaml::MachineFunction &YamlMF);

  bool initializeJumpTableInfo(PerFunctionMIParsingState &PFS,
                               const yaml::MachineJumpTable &YamlJTI);

private:
  bool parseMDNode(const PerFunctionMIParsingState &PFS, MDNode *&Node,
                   const yaml::StringValue &Source);

  bool parseMBBReference(const PerFunctionMIParsingState &PFS,
                         MachineBasicBlock *&MBB,
                         const yaml::StringValue &Source);

  /// Return a MIR diagnostic converted from an MI string diagnostic.
  SMDiagnostic diagFromMIStringDiag(const SMDiagnostic &Error,
                                    SMRange SourceRange);

  /// Return a MIR diagnostic converted from a diagnostic located in a YAML
  /// block scalar string.
  SMDiagnostic diagFromBlockStringDiag(const SMDiagnostic &Error,
                                       SMRange SourceRange);

  /// Create an empty function with the given name.
  void createDummyFunction(StringRef Name, Module &M);

  void initNames2RegClasses(const MachineFunction &MF);
  void initNames2RegBanks(const MachineFunction &MF);

  /// Check if the given identifier is a name of a register class.
  ///
  /// Return null if the name isn't a register class.
  const TargetRegisterClass *getRegClass(const MachineFunction &MF,
                                         StringRef Name);

  /// Check if the given identifier is a name of a register bank.
  ///
  /// Return null if the name isn't a register bank.
  const RegisterBank *getRegBank(const MachineFunction &MF, StringRef Name);

  void computeFunctionProperties(MachineFunction &MF);
};

} // end namespace llvm

MIRParserImpl::MIRParserImpl(std::unique_ptr<MemoryBuffer> Contents,
                             StringRef Filename, LLVMContext &Context)
    : SM(), Filename(Filename), Context(Context) {
  SM.AddNewSourceBuffer(std::move(Contents), SMLoc());
}

bool MIRParserImpl::error(const Twine &Message) {
  Context.diagnose(DiagnosticInfoMIRParser(
      DS_Error, SMDiagnostic(Filename, SourceMgr::DK_Error, Message.str())));
  return true;
}

bool MIRParserImpl::error(SMLoc Loc, const Twine &Message) {
  Context.diagnose(DiagnosticInfoMIRParser(
      DS_Error, SM.GetMessage(Loc, SourceMgr::DK_Error, Message)));
  return true;
}

bool MIRParserImpl::error(const SMDiagnostic &Error, SMRange SourceRange) {
  assert(Error.getKind() == SourceMgr::DK_Error && "Expected an error");
  reportDiagnostic(diagFromMIStringDiag(Error, SourceRange));
  return true;
}

void MIRParserImpl::reportDiagnostic(const SMDiagnostic &Diag) {
  DiagnosticSeverity Kind;
  switch (Diag.getKind()) {
  case SourceMgr::DK_Error:
    Kind = DS_Error;
    break;
  case SourceMgr::DK_Warning:
    Kind = DS_Warning;
    break;
  case SourceMgr::DK_Note:
    Kind = DS_Note;
    break;
  }
  Context.diagnose(DiagnosticInfoMIRParser(Kind, Diag));
}

static void handleYAMLDiag(const SMDiagnostic &Diag, void *Context) {
  reinterpret_cast<MIRParserImpl *>(Context)->reportDiagnostic(Diag);
}

std::unique_ptr<Module> MIRParserImpl::parse() {
  yaml::Input In(SM.getMemoryBuffer(SM.getMainFileID())->getBuffer(),
                 /*Ctxt=*/nullptr, handleYAMLDiag, this);
  In.setContext(&In);

  if (!In.setCurrentDocument()) {
    if (In.error())
      return nullptr;
    // Create an empty module when the MIR file is empty.
    return llvm::make_unique<Module>(Filename, Context);
  }

  std::unique_ptr<Module> M;
  bool NoLLVMIR = false;
  // Parse the block scalar manually so that we can return unique pointer
  // without having to go trough YAML traits.
  if (const auto *BSN =
          dyn_cast_or_null<yaml::BlockScalarNode>(In.getCurrentNode())) {
    SMDiagnostic Error;
    M = parseAssembly(MemoryBufferRef(BSN->getValue(), Filename), Error,
                      Context, &IRSlots);
    if (!M) {
      reportDiagnostic(diagFromBlockStringDiag(Error, BSN->getSourceRange()));
      return nullptr;
    }
    In.nextDocument();
    if (!In.setCurrentDocument())
      return M;
  } else {
    // Create an new, empty module.
    M = llvm::make_unique<Module>(Filename, Context);
    NoLLVMIR = true;
  }

  // Parse the machine functions.
  do {
    if (parseMachineFunction(In, *M, NoLLVMIR))
      return nullptr;
    In.nextDocument();
  } while (In.setCurrentDocument());

  return M;
}

bool MIRParserImpl::parseMachineFunction(yaml::Input &In, Module &M,
                                         bool NoLLVMIR) {
  auto MF = llvm::make_unique<yaml::MachineFunction>();
  yaml::EmptyContext Ctx;
  yaml::yamlize(In, *MF, false, Ctx);
  if (In.error())
    return true;
  auto FunctionName = MF->Name;
  if (Functions.find(FunctionName) != Functions.end())
    return error(Twine("redefinition of machine function '") + FunctionName +
                 "'");
  Functions.insert(std::make_pair(FunctionName, std::move(MF)));
  if (NoLLVMIR)
    createDummyFunction(FunctionName, M);
  else if (!M.getFunction(FunctionName))
    return error(Twine("function '") + FunctionName +
                 "' isn't defined in the provided LLVM IR");
  return false;
}

void MIRParserImpl::createDummyFunction(StringRef Name, Module &M) {
  auto &Context = M.getContext();
  Function *F = cast<Function>(M.getOrInsertFunction(
      Name, FunctionType::get(Type::getVoidTy(Context), false)));
  BasicBlock *BB = BasicBlock::Create(Context, "entry", F);
  new UnreachableInst(Context, BB);
}

static bool isSSA(const MachineFunction &MF) {
  const MachineRegisterInfo &MRI = MF.getRegInfo();
  for (unsigned I = 0, E = MRI.getNumVirtRegs(); I != E; ++I) {
    unsigned Reg = TargetRegisterInfo::index2VirtReg(I);
    if (!MRI.hasOneDef(Reg) && !MRI.def_empty(Reg))
      return false;
  }
  return true;
}

void MIRParserImpl::computeFunctionProperties(MachineFunction &MF) {
  MachineFunctionProperties &Properties = MF.getProperties();

  bool HasPHI = false;
  bool HasInlineAsm = false;
  for (const MachineBasicBlock &MBB : MF) {
    for (const MachineInstr &MI : MBB) {
      if (MI.isPHI())
        HasPHI = true;
      if (MI.isInlineAsm())
        HasInlineAsm = true;
    }
  }
  if (!HasPHI)
    Properties.set(MachineFunctionProperties::Property::NoPHIs);
  MF.setHasInlineAsm(HasInlineAsm);

  if (isSSA(MF))
    Properties.set(MachineFunctionProperties::Property::IsSSA);
  else
    Properties.reset(MachineFunctionProperties::Property::IsSSA);

  const MachineRegisterInfo &MRI = MF.getRegInfo();
  if (MRI.getNumVirtRegs() == 0)
    Properties.set(MachineFunctionProperties::Property::NoVRegs);
}

bool MIRParserImpl::initializeMachineFunction(MachineFunction &MF) {
  auto It = Functions.find(MF.getName());
  if (It == Functions.end())
    return error(Twine("no machine function information for function '") +
                 MF.getName() + "' in the MIR file");
  // TODO: Recreate the machine function.
  const yaml::MachineFunction &YamlMF = *It->getValue();
  if (YamlMF.Alignment)
    MF.setAlignment(YamlMF.Alignment);
  MF.setExposesReturnsTwice(YamlMF.ExposesReturnsTwice);

  if (YamlMF.Legalized)
    MF.getProperties().set(MachineFunctionProperties::Property::Legalized);
  if (YamlMF.RegBankSelected)
    MF.getProperties().set(
        MachineFunctionProperties::Property::RegBankSelected);
  if (YamlMF.Selected)
    MF.getProperties().set(MachineFunctionProperties::Property::Selected);

  PerFunctionMIParsingState PFS(MF, SM, IRSlots);
  if (initializeRegisterInfo(PFS, YamlMF))
    return true;
  if (!YamlMF.Constants.empty()) {
    auto *ConstantPool = MF.getConstantPool();
    assert(ConstantPool && "Constant pool must be created");
    if (initializeConstantPool(PFS, *ConstantPool, YamlMF))
      return true;
  }

  StringRef BlockStr = YamlMF.Body.Value.Value;
  SMDiagnostic Error;
  SourceMgr BlockSM;
  BlockSM.AddNewSourceBuffer(
      MemoryBuffer::getMemBuffer(BlockStr, "",/*RequiresNullTerminator=*/false),
      SMLoc());
  PFS.SM = &BlockSM;
  if (parseMachineBasicBlockDefinitions(PFS, BlockStr, Error)) {
    reportDiagnostic(
        diagFromBlockStringDiag(Error, YamlMF.Body.Value.SourceRange));
    return true;
  }
  PFS.SM = &SM;

  if (MF.empty())
    return error(Twine("machine function '") + Twine(MF.getName()) +
                 "' requires at least one machine basic block in its body");
  // Initialize the frame information after creating all the MBBs so that the
  // MBB references in the frame information can be resolved.
  if (initializeFrameInfo(PFS, YamlMF))
    return true;
  // Initialize the jump table after creating all the MBBs so that the MBB
  // references can be resolved.
  if (!YamlMF.JumpTableInfo.Entries.empty() &&
      initializeJumpTableInfo(PFS, YamlMF.JumpTableInfo))
    return true;
  // Parse the machine instructions after creating all of the MBBs so that the
  // parser can resolve the MBB references.
  StringRef InsnStr = YamlMF.Body.Value.Value;
  SourceMgr InsnSM;
  InsnSM.AddNewSourceBuffer(
      MemoryBuffer::getMemBuffer(InsnStr, "", /*RequiresNullTerminator=*/false),
      SMLoc());
  PFS.SM = &InsnSM;
  if (parseMachineInstructions(PFS, InsnStr, Error)) {
    reportDiagnostic(
        diagFromBlockStringDiag(Error, YamlMF.Body.Value.SourceRange));
    return true;
  }
  PFS.SM = &SM;

  inferRegisterInfo(PFS, YamlMF);

  computeFunctionProperties(MF);

  // FIXME: This is a temporary workaround until the reserved registers can be
  // serialized.
  MF.getRegInfo().freezeReservedRegs(MF);
  MF.verify();
  return false;
}

bool MIRParserImpl::initializeRegisterInfo(PerFunctionMIParsingState &PFS,
    const yaml::MachineFunction &YamlMF) {
  MachineFunction &MF = PFS.MF;
  MachineRegisterInfo &RegInfo = MF.getRegInfo();
  assert(RegInfo.tracksLiveness());
  if (!YamlMF.TracksRegLiveness)
    RegInfo.invalidateLiveness();

  SMDiagnostic Error;
  // Parse the virtual register information.
  for (const auto &VReg : YamlMF.VirtualRegisters) {
    unsigned Reg;
    if (StringRef(VReg.Class.Value).equals("_")) {
      // This is a generic virtual register.
      // The size will be set appropriately when we reach the definition.
      Reg = RegInfo.createGenericVirtualRegister(LLT{});
      PFS.GenericVRegs.insert(Reg);
    } else {
      const auto *RC = getRegClass(MF, VReg.Class.Value);
      if (RC) {
        Reg = RegInfo.createVirtualRegister(RC);
      } else {
        const auto *RegBank = getRegBank(MF, VReg.Class.Value);
        if (!RegBank)
          return error(
              VReg.Class.SourceRange.Start,
              Twine("use of undefined register class or register bank '") +
                  VReg.Class.Value + "'");
        Reg = RegInfo.createGenericVirtualRegister(LLT{});
        RegInfo.setRegBank(Reg, *RegBank);
        PFS.GenericVRegs.insert(Reg);
      }
    }
    if (!PFS.VirtualRegisterSlots.insert(std::make_pair(VReg.ID.Value, Reg))
             .second)
      return error(VReg.ID.SourceRange.Start,
                   Twine("redefinition of virtual register '%") +
                       Twine(VReg.ID.Value) + "'");
    if (!VReg.PreferredRegister.Value.empty()) {
      unsigned PreferredReg = 0;
      if (parseNamedRegisterReference(PFS, PreferredReg,
                                      VReg.PreferredRegister.Value, Error))
        return error(Error, VReg.PreferredRegister.SourceRange);
      RegInfo.setSimpleHint(Reg, PreferredReg);
    }
  }

  // Parse the liveins.
  for (const auto &LiveIn : YamlMF.LiveIns) {
    unsigned Reg = 0;
    if (parseNamedRegisterReference(PFS, Reg, LiveIn.Register.Value, Error))
      return error(Error, LiveIn.Register.SourceRange);
    unsigned VReg = 0;
    if (!LiveIn.VirtualRegister.Value.empty()) {
      if (parseVirtualRegisterReference(PFS, VReg, LiveIn.VirtualRegister.Value,
                                        Error))
        return error(Error, LiveIn.VirtualRegister.SourceRange);
    }
    RegInfo.addLiveIn(Reg, VReg);
  }

  // Parse the callee saved register mask.
  BitVector CalleeSavedRegisterMask(RegInfo.getUsedPhysRegsMask().size());
  if (!YamlMF.CalleeSavedRegisters)
    return false;
  for (const auto &RegSource : YamlMF.CalleeSavedRegisters.getValue()) {
    unsigned Reg = 0;
    if (parseNamedRegisterReference(PFS, Reg, RegSource.Value, Error))
      return error(Error, RegSource.SourceRange);
    CalleeSavedRegisterMask[Reg] = true;
  }
  RegInfo.setUsedPhysRegMask(CalleeSavedRegisterMask.flip());
  return false;
}

void MIRParserImpl::inferRegisterInfo(const PerFunctionMIParsingState &PFS,
                                      const yaml::MachineFunction &YamlMF) {
  if (YamlMF.CalleeSavedRegisters)
    return;
  MachineRegisterInfo &MRI = PFS.MF.getRegInfo();
  for (const MachineBasicBlock &MBB : PFS.MF) {
    for (const MachineInstr &MI : MBB) {
      for (const MachineOperand &MO : MI.operands()) {
        if (!MO.isRegMask())
          continue;
        MRI.addPhysRegsUsedFromRegMask(MO.getRegMask());
      }
    }
  }
}

bool MIRParserImpl::initializeFrameInfo(PerFunctionMIParsingState &PFS,
                                        const yaml::MachineFunction &YamlMF) {
  MachineFunction &MF = PFS.MF;
  MachineFrameInfo &MFI = MF.getFrameInfo();
  const Function &F = *MF.getFunction();
  const yaml::MachineFrameInfo &YamlMFI = YamlMF.FrameInfo;
  MFI.setFrameAddressIsTaken(YamlMFI.IsFrameAddressTaken);
  MFI.setReturnAddressIsTaken(YamlMFI.IsReturnAddressTaken);
  MFI.setHasStackMap(YamlMFI.HasStackMap);
  MFI.setHasPatchPoint(YamlMFI.HasPatchPoint);
  MFI.setStackSize(YamlMFI.StackSize);
  MFI.setOffsetAdjustment(YamlMFI.OffsetAdjustment);
  if (YamlMFI.MaxAlignment)
    MFI.ensureMaxAlignment(YamlMFI.MaxAlignment);
  MFI.setAdjustsStack(YamlMFI.AdjustsStack);
  MFI.setHasCalls(YamlMFI.HasCalls);
  MFI.setMaxCallFrameSize(YamlMFI.MaxCallFrameSize);
  MFI.setHasOpaqueSPAdjustment(YamlMFI.HasOpaqueSPAdjustment);
  MFI.setHasVAStart(YamlMFI.HasVAStart);
  MFI.setHasMustTailInVarArgFunc(YamlMFI.HasMustTailInVarArgFunc);
  if (!YamlMFI.SavePoint.Value.empty()) {
    MachineBasicBlock *MBB = nullptr;
    if (parseMBBReference(PFS, MBB, YamlMFI.SavePoint))
      return true;
    MFI.setSavePoint(MBB);
  }
  if (!YamlMFI.RestorePoint.Value.empty()) {
    MachineBasicBlock *MBB = nullptr;
    if (parseMBBReference(PFS, MBB, YamlMFI.RestorePoint))
      return true;
    MFI.setRestorePoint(MBB);
  }

  std::vector<CalleeSavedInfo> CSIInfo;
  // Initialize the fixed frame objects.
  for (const auto &Object : YamlMF.FixedStackObjects) {
    int ObjectIdx;
    if (Object.Type != yaml::FixedMachineStackObject::SpillSlot)
      ObjectIdx = MFI.CreateFixedObject(Object.Size, Object.Offset,
                                        Object.IsImmutable, Object.IsAliased);
    else
      ObjectIdx = MFI.CreateFixedSpillStackObject(Object.Size, Object.Offset);
    MFI.setObjectAlignment(ObjectIdx, Object.Alignment);
    if (!PFS.FixedStackObjectSlots.insert(std::make_pair(Object.ID.Value,
                                                         ObjectIdx))
             .second)
      return error(Object.ID.SourceRange.Start,
                   Twine("redefinition of fixed stack object '%fixed-stack.") +
                       Twine(Object.ID.Value) + "'");
    if (parseCalleeSavedRegister(PFS, CSIInfo, Object.CalleeSavedRegister,
                                 ObjectIdx))
      return true;
  }

  // Initialize the ordinary frame objects.
  for (const auto &Object : YamlMF.StackObjects) {
    int ObjectIdx;
    const AllocaInst *Alloca = nullptr;
    const yaml::StringValue &Name = Object.Name;
    if (!Name.Value.empty()) {
      Alloca = dyn_cast_or_null<AllocaInst>(
          F.getValueSymbolTable()->lookup(Name.Value));
      if (!Alloca)
        return error(Name.SourceRange.Start,
                     "alloca instruction named '" + Name.Value +
                         "' isn't defined in the function '" + F.getName() +
                         "'");
    }
    if (Object.Type == yaml::MachineStackObject::VariableSized)
      ObjectIdx = MFI.CreateVariableSizedObject(Object.Alignment, Alloca);
    else
      ObjectIdx = MFI.CreateStackObject(
          Object.Size, Object.Alignment,
          Object.Type == yaml::MachineStackObject::SpillSlot, Alloca);
    MFI.setObjectOffset(ObjectIdx, Object.Offset);
    if (!PFS.StackObjectSlots.insert(std::make_pair(Object.ID.Value, ObjectIdx))
             .second)
      return error(Object.ID.SourceRange.Start,
                   Twine("redefinition of stack object '%stack.") +
                       Twine(Object.ID.Value) + "'");
    if (parseCalleeSavedRegister(PFS, CSIInfo, Object.CalleeSavedRegister,
                                 ObjectIdx))
      return true;
    if (Object.LocalOffset)
      MFI.mapLocalFrameObject(ObjectIdx, Object.LocalOffset.getValue());
    if (parseStackObjectsDebugInfo(PFS, Object, ObjectIdx))
      return true;
  }
  MFI.setCalleeSavedInfo(CSIInfo);
  if (!CSIInfo.empty())
    MFI.setCalleeSavedInfoValid(true);

  // Initialize the various stack object references after initializing the
  // stack objects.
  if (!YamlMFI.StackProtector.Value.empty()) {
    SMDiagnostic Error;
    int FI;
    if (parseStackObjectReference(PFS, FI, YamlMFI.StackProtector.Value, Error))
      return error(Error, YamlMFI.StackProtector.SourceRange);
    MFI.setStackProtectorIndex(FI);
  }
  return false;
}

bool MIRParserImpl::parseCalleeSavedRegister(PerFunctionMIParsingState &PFS,
    std::vector<CalleeSavedInfo> &CSIInfo,
    const yaml::StringValue &RegisterSource, int FrameIdx) {
  if (RegisterSource.Value.empty())
    return false;
  unsigned Reg = 0;
  SMDiagnostic Error;
  if (parseNamedRegisterReference(PFS, Reg, RegisterSource.Value, Error))
    return error(Error, RegisterSource.SourceRange);
  CSIInfo.push_back(CalleeSavedInfo(Reg, FrameIdx));
  return false;
}

/// Verify that given node is of a certain type. Return true on error.
template <typename T>
static bool typecheckMDNode(T *&Result, MDNode *Node,
                            const yaml::StringValue &Source,
                            StringRef TypeString, MIRParserImpl &Parser) {
  if (!Node)
    return false;
  Result = dyn_cast<T>(Node);
  if (!Result)
    return Parser.error(Source.SourceRange.Start,
                        "expected a reference to a '" + TypeString +
                            "' metadata node");
  return false;
}

bool MIRParserImpl::parseStackObjectsDebugInfo(PerFunctionMIParsingState &PFS,
    const yaml::MachineStackObject &Object, int FrameIdx) {
  // Debug information can only be attached to stack objects; Fixed stack
  // objects aren't supported.
  assert(FrameIdx >= 0 && "Expected a stack object frame index");
  MDNode *Var = nullptr, *Expr = nullptr, *Loc = nullptr;
  if (parseMDNode(PFS, Var, Object.DebugVar) ||
      parseMDNode(PFS, Expr, Object.DebugExpr) ||
      parseMDNode(PFS, Loc, Object.DebugLoc))
    return true;
  if (!Var && !Expr && !Loc)
    return false;
  DILocalVariable *DIVar = nullptr;
  DIExpression *DIExpr = nullptr;
  DILocation *DILoc = nullptr;
  if (typecheckMDNode(DIVar, Var, Object.DebugVar, "DILocalVariable", *this) ||
      typecheckMDNode(DIExpr, Expr, Object.DebugExpr, "DIExpression", *this) ||
      typecheckMDNode(DILoc, Loc, Object.DebugLoc, "DILocation", *this))
    return true;
  PFS.MF.getMMI().setVariableDbgInfo(DIVar, DIExpr, unsigned(FrameIdx), DILoc);
  return false;
}

bool MIRParserImpl::parseMDNode(const PerFunctionMIParsingState &PFS,
    MDNode *&Node, const yaml::StringValue &Source) {
  if (Source.Value.empty())
    return false;
  SMDiagnostic Error;
  if (llvm::parseMDNode(PFS, Node, Source.Value, Error))
    return error(Error, Source.SourceRange);
  return false;
}

bool MIRParserImpl::initializeConstantPool(PerFunctionMIParsingState &PFS,
    MachineConstantPool &ConstantPool, const yaml::MachineFunction &YamlMF) {
  DenseMap<unsigned, unsigned> &ConstantPoolSlots = PFS.ConstantPoolSlots;
  const MachineFunction &MF = PFS.MF;
  const auto &M = *MF.getFunction()->getParent();
  SMDiagnostic Error;
  for (const auto &YamlConstant : YamlMF.Constants) {
    const Constant *Value = dyn_cast_or_null<Constant>(
        parseConstantValue(YamlConstant.Value.Value, Error, M));
    if (!Value)
      return error(Error, YamlConstant.Value.SourceRange);
    unsigned Alignment =
        YamlConstant.Alignment
            ? YamlConstant.Alignment
            : M.getDataLayout().getPrefTypeAlignment(Value->getType());
    unsigned Index = ConstantPool.getConstantPoolIndex(Value, Alignment);
    if (!ConstantPoolSlots.insert(std::make_pair(YamlConstant.ID.Value, Index))
             .second)
      return error(YamlConstant.ID.SourceRange.Start,
                   Twine("redefinition of constant pool item '%const.") +
                       Twine(YamlConstant.ID.Value) + "'");
  }
  return false;
}

bool MIRParserImpl::initializeJumpTableInfo(PerFunctionMIParsingState &PFS,
    const yaml::MachineJumpTable &YamlJTI) {
  MachineJumpTableInfo *JTI = PFS.MF.getOrCreateJumpTableInfo(YamlJTI.Kind);
  for (const auto &Entry : YamlJTI.Entries) {
    std::vector<MachineBasicBlock *> Blocks;
    for (const auto &MBBSource : Entry.Blocks) {
      MachineBasicBlock *MBB = nullptr;
      if (parseMBBReference(PFS, MBB, MBBSource.Value))
        return true;
      Blocks.push_back(MBB);
    }
    unsigned Index = JTI->createJumpTableIndex(Blocks);
    if (!PFS.JumpTableSlots.insert(std::make_pair(Entry.ID.Value, Index))
             .second)
      return error(Entry.ID.SourceRange.Start,
                   Twine("redefinition of jump table entry '%jump-table.") +
                       Twine(Entry.ID.Value) + "'");
  }
  return false;
}

bool MIRParserImpl::parseMBBReference(const PerFunctionMIParsingState &PFS,
                                      MachineBasicBlock *&MBB,
                                      const yaml::StringValue &Source) {
  SMDiagnostic Error;
  if (llvm::parseMBBReference(PFS, MBB, Source.Value, Error))
    return error(Error, Source.SourceRange);
  return false;
}

SMDiagnostic MIRParserImpl::diagFromMIStringDiag(const SMDiagnostic &Error,
                                                 SMRange SourceRange) {
  assert(SourceRange.isValid() && "Invalid source range");
  SMLoc Loc = SourceRange.Start;
  bool HasQuote = Loc.getPointer() < SourceRange.End.getPointer() &&
                  *Loc.getPointer() == '\'';
  // Translate the location of the error from the location in the MI string to
  // the corresponding location in the MIR file.
  Loc = Loc.getFromPointer(Loc.getPointer() + Error.getColumnNo() +
                           (HasQuote ? 1 : 0));

  // TODO: Translate any source ranges as well.
  return SM.GetMessage(Loc, Error.getKind(), Error.getMessage(), None,
                       Error.getFixIts());
}

SMDiagnostic MIRParserImpl::diagFromBlockStringDiag(const SMDiagnostic &Error,
                                                    SMRange SourceRange) {
  assert(SourceRange.isValid());

  // Translate the location of the error from the location in the llvm IR string
  // to the corresponding location in the MIR file.
  auto LineAndColumn = SM.getLineAndColumn(SourceRange.Start);
  unsigned Line = LineAndColumn.first + Error.getLineNo() - 1;
  unsigned Column = Error.getColumnNo();
  StringRef LineStr = Error.getLineContents();
  SMLoc Loc = Error.getLoc();

  // Get the full line and adjust the column number by taking the indentation of
  // LLVM IR into account.
  for (line_iterator L(*SM.getMemoryBuffer(SM.getMainFileID()), false), E;
       L != E; ++L) {
    if (L.line_number() == Line) {
      LineStr = *L;
      Loc = SMLoc::getFromPointer(LineStr.data());
      auto Indent = LineStr.find(Error.getLineContents());
      if (Indent != StringRef::npos)
        Column += Indent;
      break;
    }
  }

  return SMDiagnostic(SM, Loc, Filename, Line, Column, Error.getKind(),
                      Error.getMessage(), LineStr, Error.getRanges(),
                      Error.getFixIts());
}

void MIRParserImpl::initNames2RegClasses(const MachineFunction &MF) {
  if (!Names2RegClasses.empty())
    return;
  const TargetRegisterInfo *TRI = MF.getSubtarget().getRegisterInfo();
  for (unsigned I = 0, E = TRI->getNumRegClasses(); I < E; ++I) {
    const auto *RC = TRI->getRegClass(I);
    Names2RegClasses.insert(
        std::make_pair(StringRef(TRI->getRegClassName(RC)).lower(), RC));
  }
}

void MIRParserImpl::initNames2RegBanks(const MachineFunction &MF) {
  if (!Names2RegBanks.empty())
    return;
  const RegisterBankInfo *RBI = MF.getSubtarget().getRegBankInfo();
  // If the target does not support GlobalISel, we may not have a
  // register bank info.
  if (!RBI)
    return;
  for (unsigned I = 0, E = RBI->getNumRegBanks(); I < E; ++I) {
    const auto &RegBank = RBI->getRegBank(I);
    Names2RegBanks.insert(
        std::make_pair(StringRef(RegBank.getName()).lower(), &RegBank));
  }
}

const TargetRegisterClass *MIRParserImpl::getRegClass(const MachineFunction &MF,
                                                      StringRef Name) {
  initNames2RegClasses(MF);
  auto RegClassInfo = Names2RegClasses.find(Name);
  if (RegClassInfo == Names2RegClasses.end())
    return nullptr;
  return RegClassInfo->getValue();
}

const RegisterBank *MIRParserImpl::getRegBank(const MachineFunction &MF,
                                              StringRef Name) {
  initNames2RegBanks(MF);
  auto RegBankInfo = Names2RegBanks.find(Name);
  if (RegBankInfo == Names2RegBanks.end())
    return nullptr;
  return RegBankInfo->getValue();
}

MIRParser::MIRParser(std::unique_ptr<MIRParserImpl> Impl)
    : Impl(std::move(Impl)) {}

MIRParser::~MIRParser() {}

std::unique_ptr<Module> MIRParser::parseLLVMModule() { return Impl->parse(); }

bool MIRParser::initializeMachineFunction(MachineFunction &MF) {
  return Impl->initializeMachineFunction(MF);
}

std::unique_ptr<MIRParser> llvm::createMIRParserFromFile(StringRef Filename,
                                                         SMDiagnostic &Error,
                                                         LLVMContext &Context) {
  auto FileOrErr = MemoryBuffer::getFile(Filename);
  if (std::error_code EC = FileOrErr.getError()) {
    Error = SMDiagnostic(Filename, SourceMgr::DK_Error,
                         "Could not open input file: " + EC.message());
    return nullptr;
  }
  return createMIRParser(std::move(FileOrErr.get()), Context);
}

std::unique_ptr<MIRParser>
llvm::createMIRParser(std::unique_ptr<MemoryBuffer> Contents,
                      LLVMContext &Context) {
  auto Filename = Contents->getBufferIdentifier();
  if (Context.shouldDiscardValueNames()) {
    Context.diagnose(DiagnosticInfoMIRParser(
        DS_Error,
        SMDiagnostic(
            Filename, SourceMgr::DK_Error,
            "Can't read MIR with a Context that discards named Values")));
    return nullptr;
  }
  return llvm::make_unique<MIRParser>(
      llvm::make_unique<MIRParserImpl>(std::move(Contents), Filename, Context));
}
