//===--- SerializeSIL.cpp - Read and write SIL ----------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "SILFormat.h"
#include "Serialization.h"
#include "swift/AST/Module.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILModule.h"

// This is a template-only header; eventually it should move to llvm/Support.
#include "clang/Basic/OnDiskHashTable.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"

using namespace swift;
using namespace swift::serialization;
using namespace swift::serialization::sil_block;

namespace {
    /// Used to serialize the on-disk func hash table.
  class FuncTableInfo {
  public:
    using key_type = Identifier;
    using key_type_ref = key_type;
    using data_type = DeclID;
    using data_type_ref = const data_type &;

    uint32_t ComputeHash(key_type_ref key) {
      assert(!key.empty());
      return llvm::HashString(key.str());
    }

    std::pair<unsigned, unsigned> EmitKeyDataLength(raw_ostream &out,
                                                    key_type_ref key,
                                                    data_type_ref data) {
      using namespace clang::io;
      uint32_t keyLength = key.str().size();
      uint32_t dataLength = sizeof(DeclID);
      Emit16(out, keyLength);
      Emit16(out, dataLength);
      return { keyLength, dataLength };
    }

    void EmitKey(raw_ostream &out, key_type_ref key, unsigned len) {
      out << key.str();
    }

    void EmitData(raw_ostream &out, key_type_ref key, data_type_ref data,
                  unsigned len) {
      static_assert(sizeof(DeclID) <= 32, "DeclID too large");
      using namespace clang::io;
      Emit32(out, data);
    }
  };

  class SILSerializer {
    Serializer &S;
    ASTContext &Ctx;

    llvm::BitstreamWriter &Out;

    /// A reusable buffer for emitting records.
    SmallVector<uint64_t, 64> ScratchRecord;

    /// In case we want to encode the relative of InstID vs ValueID.
    ValueID InstID = 0;

    llvm::DenseMap<const ValueBase*, ValueID> ValueIDs;
    ValueID LastValueID = 0;
    ValueID addValueRef(SILValue SV) {
      return addValueRef(SV.getDef());
    }
    ValueID addValueRef(const ValueBase *Val);

    using TableData = FuncTableInfo::data_type;
    using Table = llvm::DenseMap<FuncTableInfo::key_type, TableData>;
    Table FuncTable;
    std::vector<BitOffset> Funcs;
    DeclID FuncID;

    std::array<unsigned, 256> SILAbbrCodes;
    template <typename Layout>
    void registerSILAbbr() {
      using AbbrArrayTy = decltype(SILAbbrCodes);
      static_assert(Layout::Code <= std::tuple_size<AbbrArrayTy>::value,
                    "layout has invalid record code");
      SILAbbrCodes[Layout::Code] = Layout::emitAbbrev(Out);
    }

    void writeSILFunction(const SILFunction &F);
    void writeSILBasicBlock(const SILBasicBlock &BB);
    void writeSILInstruction(const SILInstruction &SI);
    void writeFuncTable();

  public:
    SILSerializer(Serializer &S, ASTContext &Ctx,
                  llvm::BitstreamWriter &Out);

    void writeAllSILFunctions(const SILModule *M);
  };
} // end anonymous namespace

SILSerializer::SILSerializer(Serializer &S, ASTContext &Ctx,
                             llvm::BitstreamWriter &Out) :
                            S(S), Ctx(Ctx), Out(Out), FuncID(1) {
}

/// We enumerate all values to update ValueIDs in a separate pass
/// to correctly handle forward reference of a value.
ValueID SILSerializer::addValueRef(const ValueBase *Val) {
  if (!Val)
    return 0;

  ValueID &id = ValueIDs[Val];
  if (id != 0)
    return id;

  id = ++LastValueID;
  return id;
}

void SILSerializer::writeSILFunction(const SILFunction &F) {
  DEBUG(llvm::dbgs() << "Serialize SIL:\n";
        F.dump());
  LastValueID = 0;
  FuncTable[Ctx.getIdentifier(F.getName())] = FuncID++;
  Funcs.push_back(Out.GetCurrentBitNo());
  InstID = 0;
  unsigned abbrCode = SILAbbrCodes[SILFunctionLayout::Code];
  TypeID FnID = S.addTypeRef(F.getLoweredType().getSwiftType());
  SILFunctionLayout::emitRecord(Out, ScratchRecord, abbrCode,
                       (unsigned)F.getLinkage(), FnID);
  for (const SILBasicBlock &BB : F)
    writeSILBasicBlock(BB);
}

void SILSerializer::writeSILBasicBlock(const SILBasicBlock &BB) {
  SmallVector<DeclID, 4> Args;
  for (auto I = BB.bbarg_begin(), E = BB.bbarg_end(); I != E; ++I) {
    SILArgument *SA = *I;
    DeclID tId = S.addTypeRef(SA->getType().getSwiftType());
    DeclID vId = addValueRef(static_cast<const ValueBase*>(SA));
    Args.push_back(tId);
    Args.push_back(vId);
  }

  unsigned abbrCode = SILAbbrCodes[SILBasicBlockLayout::Code];
  SILBasicBlockLayout::emitRecord(Out, ScratchRecord, abbrCode, Args);

  for (const SILInstruction &SI : BB)
    writeSILInstruction(SI);
}

void SILSerializer::writeSILInstruction(const SILInstruction &SI) {
  switch (SI.getKind()) {
  default: {
    unsigned abbrCode = SILAbbrCodes[SILInstTodoLayout::Code];
    SILInstTodoLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                  (unsigned)SI.getKind());
    break;
  }
  case ValueKind::AllocArrayInst: {
    const AllocArrayInst *AAI = cast<AllocArrayInst>(&SI);
    unsigned abbrCode = SILAbbrCodes[SILOneTypeOneOperandLayout::Code];
    SILOneTypeOneOperandLayout::emitRecord(Out, ScratchRecord, abbrCode,
        (unsigned)SI.getKind(),
        S.addTypeRef(AAI->getElementType().getSwiftRValueType()),
        (unsigned)AAI->getElementType().getCategory(),
        S.addTypeRef(AAI->getNumElements().getType().getSwiftRValueType()),
        (unsigned)AAI->getNumElements().getType().getCategory(),
        addValueRef(AAI->getNumElements()),
        AAI->getNumElements().getResultNumber());
    break;
  }
  case ValueKind::AllocBoxInst: {
    const AllocBoxInst *ABI = cast<AllocBoxInst>(&SI);
    unsigned abbrCode = SILAbbrCodes[SILOneTypeLayout::Code];
    SILOneTypeLayout::emitRecord(Out, ScratchRecord, abbrCode,
                      (unsigned)SI.getKind(),
                      S.addTypeRef(ABI->getElementType().getSwiftRValueType()),
                      (unsigned)ABI->getElementType().getCategory());
    break;
  }
  case ValueKind::AllocStackInst: {
    const AllocStackInst *ASI = cast<AllocStackInst>(&SI);
    unsigned abbrCode = SILAbbrCodes[SILOneTypeLayout::Code];
    SILOneTypeLayout::emitRecord(Out, ScratchRecord, abbrCode,
                      (unsigned)SI.getKind(),
                      S.addTypeRef(ASI->getElementType().getSwiftRValueType()),
                      (unsigned)ASI->getElementType().getCategory());
    break;
  }
  case ValueKind::ApplyInst: {
    // Format: attributes such as transparent, the callee's type, a value for
    // the callee and a list of values for the arguments. Each value in the list
    // is represented with 2 IDs: ValueID and ValueResultNumber.
    const ApplyInst *AI = cast<ApplyInst>(&SI);
    SmallVector<ValueID, 4> Args;
    for (auto Arg: AI->getArguments()) {
      Args.push_back(addValueRef(Arg));
      Args.push_back(Arg.getResultNumber());
    }
    SILInstApplyLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILInstApplyLayout::Code],
        (unsigned)AI->isTransparent(),
        S.addTypeRef(AI->getCallee().getType().getSwiftRValueType()),
        (unsigned)AI->getCallee().getType().getCategory(),
        addValueRef(AI->getCallee()), AI->getCallee().getResultNumber(),
        Args);
    break;
  }
  case ValueKind::DeallocStackInst:
  case ValueKind::ReturnInst: {
    unsigned abbrCode = SILAbbrCodes[SILOneOperandLayout::Code];
    SILOneOperandLayout::emitRecord(Out, ScratchRecord, abbrCode,
                 (unsigned)SI.getKind(),
                 S.addTypeRef(SI.getOperand(0).getType().getSwiftRValueType()),
                 (unsigned)SI.getOperand(0).getType().getCategory(),
                 addValueRef(SI.getOperand(0)),
                 SI.getOperand(0).getResultNumber());
    break;
  }
  case ValueKind::FunctionRefInst: {
    // Use SILOneOperandLayout to specify the function type and the function
    // name (IdentifierID).
    const FunctionRefInst *FRI = cast<FunctionRefInst>(&SI);
    unsigned abbrCode = SILAbbrCodes[SILOneOperandLayout::Code];
    SILOneOperandLayout::emitRecord(Out, ScratchRecord, abbrCode,
        (unsigned)SI.getKind(),
        S.addTypeRef(FRI->getType().getSwiftRValueType()),
        (unsigned)FRI->getType().getCategory(),
        S.addIdentifierRef(Ctx.getIdentifier(FRI->getFunction()->getName())),
        0);
    break;
  }
  case ValueKind::IntegerLiteralInst: {
    // Use SILOneOperandLayout to specify the type and the literal.
    const IntegerLiteralInst *ILI = cast<IntegerLiteralInst>(&SI);
    APInt value = ILI->getValue();
    unsigned abbrCode = SILAbbrCodes[SILOneOperandLayout::Code];
    SILOneOperandLayout::emitRecord(Out, ScratchRecord, abbrCode,
        (unsigned)SI.getKind(),
        S.addTypeRef(ILI->getType().getSwiftRValueType()),
        (unsigned)ILI->getType().getCategory(),
        S.addIdentifierRef(Ctx.getIdentifier(value.toString(10, true))),
        0);
    break;
  }
  case ValueKind::MetatypeInst: {
    const MetatypeInst *MI = cast<MetatypeInst>(&SI);
    unsigned abbrCode = SILAbbrCodes[SILOneTypeLayout::Code];
    SILOneTypeLayout::emitRecord(Out, ScratchRecord, abbrCode,
                      (unsigned)SI.getKind(),
                      S.addTypeRef(MI->getType().getSwiftRValueType()),
                      (unsigned)MI->getType().getCategory());
    break;
  }
  case ValueKind::StoreInst: {
    const StoreInst *StI = cast<StoreInst>(&SI);
    unsigned abbrCode = SILAbbrCodes[SILOneValueOneOperandLayout::Code];
    SILOneValueOneOperandLayout::emitRecord(Out, ScratchRecord, abbrCode,
                  (unsigned)SI.getKind(), addValueRef(StI->getSrc()),
                  StI->getSrc().getResultNumber(),
                  S.addTypeRef(StI->getDest().getType().getSwiftRValueType()),
                  (unsigned)StI->getDest().getType().getCategory(),
                  addValueRef(StI->getDest()),
                  StI->getDest().getResultNumber());
    break;
  }
  case ValueKind::StructExtractInst: {
    // Has a typed valueref and a field decl. We use SILOneValueOneOperandLayout
    // where the field decl is streamed as a ValueID.
    const StructExtractInst *SEI = cast<StructExtractInst>(&SI);
    SILOneValueOneOperandLayout::emitRecord(Out, ScratchRecord, 
        SILAbbrCodes[SILOneValueOneOperandLayout::Code],
        (unsigned)SI.getKind(), S.addDeclRef(SEI->getField()), 0,
        S.addTypeRef(SEI->getOperand().getType().getSwiftRValueType()),
        (unsigned)SEI->getOperand().getType().getCategory(),
        addValueRef(SEI->getOperand()), SEI->getOperand().getResultNumber());
    break;
  }
  case ValueKind::StructInst: {
    // Format: a type followed by a list of typed values. A typed value is
    // expressed by 4 IDs: TypeID, TypeCategory, ValueID, ValueResultNumber.
    const StructInst *StrI = cast<StructInst>(&SI);
    SmallVector<ValueID, 4> ListOfValues;
    for (auto Elt : StrI->getElements()) {
      ListOfValues.push_back(S.addTypeRef(Elt.getType().getSwiftRValueType()));
      ListOfValues.push_back((unsigned)Elt.getType().getCategory());
      ListOfValues.push_back(addValueRef(Elt));
      ListOfValues.push_back(Elt.getResultNumber());
    }

    SILOneTypeValuesLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILOneTypeValuesLayout::Code],
        (unsigned)SI.getKind(),
        S.addTypeRef(StrI->getType().getSwiftRValueType()),
        (unsigned)StrI->getType().getCategory(), ListOfValues);
    break;
  }
  case ValueKind::TupleInst: {
    // Format: a type followed by a list of values. A value is expressed by
    // 2 IDs: ValueID, ValueResultNumber.
    const TupleInst *TI = cast<TupleInst>(&SI);
    SmallVector<ValueID, 4> ListOfValues;
    for (auto Elt : TI->getElements()) {
      ListOfValues.push_back(addValueRef(Elt));
      ListOfValues.push_back(Elt.getResultNumber());
    }

    unsigned abbrCode = SILAbbrCodes[SILOneTypeValuesLayout::Code];
    SILOneTypeValuesLayout::emitRecord(Out, ScratchRecord, abbrCode,
        (unsigned)SI.getKind(),
        S.addTypeRef(TI->getType().getSwiftRValueType()), 
        (unsigned)TI->getType().getCategory(),
        ListOfValues);
    break;
  }
  }
  // Non-void values get registered in the value table.
  if (SI.hasValue()) {
    addValueRef(&SI);
    ++InstID;
  }
}

void SILSerializer::writeFuncTable() {
  using clang::OnDiskChainedHashTableGenerator;

  if (FuncTable.empty())
    return;

  SmallVector<uint64_t, 8> scratch;
  llvm::SmallString<4096> hashTableBlob;
  uint32_t tableOffset;
  {
    OnDiskChainedHashTableGenerator<FuncTableInfo> generator;
    for (auto &entry : FuncTable)
      generator.insert(entry.first, entry.second);

    llvm::raw_svector_ostream blobStream(hashTableBlob);
    // Make sure that no bucket is at offset 0
    clang::io::Emit32(blobStream, 0);
    tableOffset = generator.Emit(blobStream);
  }

  unsigned abbrCode = SILAbbrCodes[FuncListLayout::Code];
  FuncListLayout::emitRecord(Out, ScratchRecord, abbrCode, tableOffset,
                             hashTableBlob);

  abbrCode = SILAbbrCodes[FuncOffsetLayout::Code];
  FuncOffsetLayout::emitRecord(Out, ScratchRecord, abbrCode, Funcs);
}

void SILSerializer::writeAllSILFunctions(const SILModule *M) {
  {
    BCBlockRAII subBlock(Out, SIL_BLOCK_ID, 4);
    registerSILAbbr<SILFunctionLayout>();
    registerSILAbbr<SILBasicBlockLayout>();
    registerSILAbbr<SILOneValueOneOperandLayout>();
    registerSILAbbr<SILOneTypeLayout>();
    registerSILAbbr<SILOneOperandLayout>();
    registerSILAbbr<SILOneTypeOneOperandLayout>();
    registerSILAbbr<SILOneTypeValuesLayout>();
    registerSILAbbr<SILInstApplyLayout>();
    registerSILAbbr<SILInstTodoLayout>();

    // Go through all SILFunctions in M, and if it is transparent,
    // write out the SILFunction.
    for (const SILFunction &F : *M) {
      if (F.isTransparent() && !F.empty())
        writeSILFunction(F);
    }
  }
  {
    BCBlockRAII restoreBlock(Out, SIL_INDEX_BLOCK_ID, 4);
    registerSILAbbr<FuncListLayout>();
    registerSILAbbr<FuncOffsetLayout>();
    writeFuncTable();
  }
}

void Serializer::writeSILFunctions(const SILModule *M) {
  if (!M)
    return;

  SILSerializer SILSer(*this, TU->Ctx, Out);
  SILSer.writeAllSILFunctions(M);

}
