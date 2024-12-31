//===- FilPizlonator.cpp - coolest fugcing pass ever written --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file is a part of Fil-C.
//
// See here:
// https://github.com/pizlonator/llvm-project-deluge/blob/deluge/Manifesto.md
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Instrumentation/FilPizlonator.h"

#include <llvm/Analysis/CFG.h>
#include <llvm/Demangle/Demangle.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/TypedPointerType.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/IntrinsicsX86.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include <llvm/Transforms/Utils/ModuleUtils.h>
#include <llvm/Transforms/Utils/PromoteMemToReg.h>
#include <llvm/TargetParser/Triple.h>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <sstream>

using namespace llvm;

namespace {

static cl::opt<bool> verbose(
  "filc-verbose", cl::desc("Make FilC verbose"),
  cl::Hidden, cl::init(false));
static cl::opt<bool> ultraVerbose(
  "filc-ultra-verbose", cl::desc("Make FilC ultra verbose"),
  cl::Hidden, cl::init(false));
static cl::opt<bool> logAllocations(
  "filc-log-allocations", cl::desc("Make FilC emit code to log every allocation"),
  cl::Hidden, cl::init(false));
static cl::opt<bool> optimizeChecks(
  "filc-optimize-checks", cl::desc("Pick an optimal schedule for checks"),
  cl::Hidden, cl::init(true));
static cl::opt<bool> propagateChecksBackward(
  "filc-propagate-checks-backward", cl::desc("Perform backward propagation of checks"),
  cl::Hidden, cl::init(true));
static cl::opt<bool> useAsmForOffsets(
  "filc-use-asm-for-offset", cl::desc("Use inline assembly to compute offsets"),
  cl::Hidden, cl::init(false));

// This has to match the FilC runtime.

static constexpr size_t GCMinAlign = 16;
static constexpr size_t FlightPtrAlign = 16;
static constexpr size_t WordSize = 8;
static constexpr size_t WordSizeShift = 3;

static constexpr uintptr_t ObjectAuxPtrMask = 0xfffffffffffflu;
static constexpr uintptr_t ObjectAuxFlagsShift = 48;

static constexpr size_t ObjectSize = 16;

static constexpr uint16_t SpecialTypeFunction = 1;
static constexpr uint16_t SpecialTypeMask = 15;

static constexpr uint16_t ObjectFlagGlobal = 1;
static constexpr uint16_t ObjectFlagReadonly = 2;
static constexpr uint16_t ObjectFlagFree = 4;
static constexpr uint16_t ObjectFlagGlobalAux = 16;
static constexpr uint16_t ObjectFlagsSpecialShift = 6;
static constexpr uint16_t ObjectFlagsAlignShift = 10;

static constexpr uintptr_t AtomicBoxBit = 1;

static constexpr unsigned NumUnwindRegisters = 2;

static constexpr size_t CCAlignment = 64;
static constexpr size_t CCInlineSize = 256;

static constexpr uint8_t ThreadStateCheckRequested = 2;
static constexpr uint8_t ThreadStateStopRequested = 4;
static constexpr uint8_t ThreadStateDeferredSignal = 8;

enum class AccessKind {
  Read,
  Write
};

static inline const char* accessKindString(AccessKind AK) {
  switch (AK) {
  case AccessKind::Read:
    return "Read";
  case AccessKind::Write:
    return "Write";
  }
  llvm_unreachable("Bad access kind");
  return nullptr;
}

enum class ConstantKind {
  Global,
  Expr
};

enum class JmpBufKind {
  setjmp,
  _setjmp,
  sigsetjmp
};

struct ConstantTarget {
  ConstantTarget() { }

  ConstantTarget(ConstantKind Kind, GlobalValue* Target)
    : Kind(Kind)
    , Target(Target) {
  }

  explicit operator bool() const { return !!Target; }
  
  ConstantKind Kind { ConstantKind::Global };
  GlobalValue* Target { nullptr }; // Getter for globals (including functions), constexpr node for constexprs
};

struct ConstantRelocation {
  ConstantRelocation() { }

  ConstantRelocation(size_t Offset, ConstantKind Kind, GlobalValue* Target)
    : Offset(Offset)
    , Kind(Kind)
    , Target(Target) {
  }
  
  size_t Offset { 0 };
  ConstantKind Kind { ConstantKind::Global };
  GlobalValue* Target { nullptr }; // Getter for globals (including functions), constexpr node for constexprs
};

enum class ConstexprOpcode {
  AddPtrImmediate
};

enum class MemoryKind {
  CC,
  GlobalInit,
  Heap
};

struct ValuePtr {
  ValuePtr() { }

  ValuePtr(Value* V, size_t PtrIndex)
    : V(V), PtrIndex(PtrIndex) {
  }

  Value* V { nullptr };
  size_t PtrIndex { 0 };

  bool operator==(const ValuePtr& Other) const {
    return V == Other.V && PtrIndex == Other.PtrIndex;
  }

  size_t hash() const {
    return std::hash<Value*>()(V) + PtrIndex;
  }
};

} // anonymous namespace

template<> struct std::hash<ValuePtr> {
  size_t operator()(const ValuePtr& Key) const {
    return Key.hash();
  }
};

namespace {

struct FunctionOriginKey {
  FunctionOriginKey() = default;

  FunctionOriginKey(Function* OldF, bool CanCatch)
    : OldF(OldF), CanCatch(CanCatch) {
  }

  Function* OldF { nullptr };
  bool CanCatch { false };

  bool operator==(const FunctionOriginKey& Other) const {
    return OldF == Other.OldF && CanCatch == Other.CanCatch;
  }

  size_t hash() const {
    return std::hash<Function*>()(OldF) + static_cast<size_t>(CanCatch);
  }
};

struct EHDataKey {
  EHDataKey() = default;
  EHDataKey(LandingPadInst* LPI) : LPI(LPI) { }

  LandingPadInst* LPI { nullptr };

  bool operator==(const EHDataKey& Other) const {
    if (!LPI)
      return !Other.LPI;

    if (!Other.LPI)
      return false;

    if (LPI->isCleanup() != Other.LPI->isCleanup())
      return false;

    if (LPI->getNumClauses() != Other.LPI->getNumClauses())
      return false;

    for (unsigned Idx = LPI->getNumClauses(); Idx--;) {
      if (LPI->getClause(Idx) != Other.LPI->getClause(Idx))
        return false;
    }

    return true;
  }

  size_t hash() const {
    if (!LPI)
      return 0;

    size_t Result = 0;
    Result += LPI->isCleanup();
    Result += LPI->getNumClauses();
    for (unsigned Idx = LPI->getNumClauses(); Idx--;)
      Result += std::hash<Constant*>()(LPI->getClause(Idx));
    return Result;
  }
};

struct OriginKey {
  OriginKey() = default;

  OriginKey(Function* OldF, DILocation* DI, bool CanCatch, LandingPadInst* LPI)
    : OldF(OldF), DI(DI), CanCatch(CanCatch), LPI(LPI) {
    if (LPI)
      assert(CanCatch);
  }

  Function* OldF { nullptr };
  DILocation* DI { nullptr };
  bool CanCatch { false };
  LandingPadInst* LPI { nullptr };

  bool operator==(const OriginKey& Other) const {
    return OldF == Other.OldF && DI == Other.DI && CanCatch == Other.CanCatch
      && EHDataKey(LPI) == EHDataKey(Other.LPI);
  }

  size_t hash() const {
    return std::hash<Function*>()(OldF) + std::hash<DILocation*>()(DI) + static_cast<size_t>(CanCatch)
      + EHDataKey(LPI).hash();
  }
};

struct InlineFrameKey {
  InlineFrameKey() = default;

  InlineFrameKey(Function* OldF, DISubprogram* Subprogram, DILocation* InlinedAt, bool CanCatch)
    : OldF(OldF), Subprogram(Subprogram), InlinedAt(InlinedAt), CanCatch(CanCatch) {
  }

  Function* OldF { nullptr };
  DISubprogram* Subprogram { nullptr };
  DILocation* InlinedAt { nullptr };
  bool CanCatch { false };

  bool operator==(const InlineFrameKey& Other) const {
    return OldF == Other.OldF && Subprogram == Other.Subprogram && InlinedAt == Other.InlinedAt
      && CanCatch == Other.CanCatch;
  }

  size_t hash() const {
    return std::hash<Function*>()(OldF) + std::hash<DISubprogram*>()(Subprogram)
      + std::hash<DILocation*>()(InlinedAt) + static_cast<size_t>(CanCatch);
  }
};

} // anonymous namespace

template<> struct std::hash<FunctionOriginKey> {
  size_t operator()(const FunctionOriginKey& Key) const {
    return Key.hash();
  }
};

template<> struct std::hash<EHDataKey> {
  size_t operator()(const EHDataKey& Key) const {
    return Key.hash();
  }
};

template<> struct std::hash<OriginKey> {
  size_t operator()(const OriginKey& Key) const {
    return Key.hash();
  }
};

template<> struct std::hash<InlineFrameKey> {
  size_t operator()(const InlineFrameKey& Key) const {
    return Key.hash();
  }
};

namespace {

static constexpr size_t NumSpecialFrameObjects = 0;

enum class MATokenKind {
  None,
  Error,
  Directive,
  Identifier,
  Comma,
  EndLine
};

inline const char* MATokenKindString(MATokenKind Kind) {
  switch (Kind) {
  case MATokenKind::None:
    return "None";
  case MATokenKind::Error:
    return "Error";
  case MATokenKind::Directive:
    return "Directive";
  case MATokenKind::Identifier:
    return "Identifier";
  case MATokenKind::Comma:
    return "Comma";
  case MATokenKind::EndLine:
    return "EndLine";
  }
  llvm_unreachable("Bad MATokenKind");
  return nullptr;
}

struct MAToken {
  MAToken() = default;

  MAToken(MATokenKind Kind, const std::string& Str): Kind(Kind), Str(Str) {}

  MATokenKind Kind { MATokenKind::None };
  std::string Str;
};

class MATokenizer {
  std::string MA;
  size_t Idx { 0 };

  void skipWhitespace() {
    while (Idx < MA.size() && MA[Idx] != '\n' && isspace(MA[Idx]))
      Idx++;
  }

  void skipID() {
    while (Idx < MA.size() && (isalnum(MA[Idx]) ||
                               MA[Idx] == '_' ||
                               MA[Idx] == '$' ||
                               MA[Idx] == '@' ||
                               MA[Idx] == '.'))
      Idx++;
  }
  
public:
  MATokenizer(const std::string& MA): MA(MA) {}

  bool isAtEnd() const { return Idx >= MA.size(); }

  MAToken getNext() {
    skipWhitespace();
    if (isAtEnd())
      return MAToken();
    if (MA[Idx] == '\n') {
      Idx++;
      return MAToken(MATokenKind::EndLine, "\n");
    }
    if (MA[Idx] == ',') {
      Idx++;
      return MAToken(MATokenKind::Comma, "\n");
    }
    if (MA[Idx] == '.') {
      size_t Start = Idx;
      skipID();
      return MAToken(MATokenKind::Directive, MA.substr(Start, Idx - Start));
    }
    if (isalpha(MA[Idx]) || MA[Idx] == '_') {
      size_t Start = Idx;
      skipID();
      return MAToken(MATokenKind::Identifier, MA.substr(Start, Idx - Start));
    }
    return MAToken(MATokenKind::Error, MA.substr(Idx, MA.size() - Idx));
  }

  MAToken getNextSpecific(MATokenKind Kind) {
    MAToken Tok = getNext();
    if (Tok.Kind != Kind) {
      errs() << "Expected " << MATokenKindString(Kind) << " but got: " << Tok.Str << "\n";
      llvm_unreachable("Error parsing module asm");
    }
    return Tok;
  }
};

enum class AIState {
  Unknown,
  Uninitialized,
  Initialized,
  MaybeInitialized
};

AIState mergeAIState(AIState A, AIState B) {
  if (A == B)
    return A;
  if (A == AIState::Unknown)
    return B;
  if (B == AIState::Unknown)
    return A;
  return AIState::MaybeInitialized;
}

__attribute__((used)) const char* aiStateString(AIState State) {
  switch (State) {
  case AIState::Unknown:
    return "Unknown";
  case AIState::Uninitialized:
    return "Uninitialized";
  case AIState::Initialized:
    return "Initialized";
  case AIState::MaybeInitialized:
    return "MaybeInitialized";
  }
  llvm_unreachable("bad AIState");
  return nullptr;
}

enum class CheckKind {
  // The order of CheckKinds is important for sorting AccessChecks!

  // Check if the object is valid (not NULL). This check has to happen before any of the other
  // checks.
  ValidObject,

  // Indicates that we already know that CanonicalPtr + Offset is aligned to Size, and there's no
  // need to check, but we can rely on it. This kind should only appear in the final ChecksForInst
  // list but never during backward or forward propagation. Not all CheckKinds have a Known variant.
  // We need a Known variant for Alignment because it influences how the TypeIsInt and UpperBound
  // check works. For CheckKinds that have no Known variant, the CheckKind will simply be excluded
  // from ChecksForInst if it's known already.
  KnownAlignment,

  // Check if the CanonicalPtr + Offset is aligned to Size.
  Alignment,

  // Check if the object is not readonly.
  CanWrite,

  // Indicates that we already know that CanonicalPtr + Offset is greaterequal object->lower, and
  // there's no need to check, but we can rely on it. This kind should only appear in the final
  // ChecksForInst list but never during backward or forward propagation.
  KnownLowerBound,

  // Check that CanonicalPtr + Offset is greaterequal object->lower.
  LowerBound,

  // Check that CanonicalPtr + Offset is lessequal object->upper.
  UpperBound,

  // Get the aux ptr. If it's null, just return the null.
  //
  // FIXME: It would be great if stuff like this was tracked on something broader than the canonical
  // ptr. We should identify the "root ptr", i.e. the common one ignoring all arithmetic. The trick is
  // how to then canonicalize those.
  //
  // And the same thing can be said for ValidObject, EnsureAuxPtr, and maybe even Alignment (though
  // that one is harder).
  //
  // FIXME #2: If we've previously ensured aux ptr, then loads don't have to null check the aux.
  GetAuxPtr,

  // Ensure the aux ptr. If it's null, create one. This doesn't load the aux ptr at all; it assumes
  // that already happened with a GetAuxPtr. It just checks if that ptr is null, and if it is, it does
  // the ensure. However, doing the ensure does reload it first to make sure it didn't become nonnull.
  EnsureAuxPtr,

  // Check that the object isn't free. Checking any TypeIsInt or TypeIsPtr automatically checks that
  // the object isn't free.
  NotFree
};

bool IsKnownCheckKind(CheckKind CK) {
  switch (CK) {
  case CheckKind::KnownAlignment:
  case CheckKind::KnownLowerBound:
    return true;
  case CheckKind::ValidObject:
  case CheckKind::CanWrite:
  case CheckKind::Alignment:
  case CheckKind::UpperBound:
  case CheckKind::LowerBound:
  case CheckKind::NotFree:
  case CheckKind::GetAuxPtr:
  case CheckKind::EnsureAuxPtr:
    return false;
  }
  llvm_unreachable("Bad CheckKind.");
  return false;
}

CheckKind FundamentalCheckKind(CheckKind CK) {
  switch (CK) {
  case CheckKind::KnownAlignment:
    return CheckKind::Alignment;
  case CheckKind::KnownLowerBound:
    return CheckKind::LowerBound;
  case CheckKind::ValidObject:
  case CheckKind::CanWrite:
  case CheckKind::Alignment:
  case CheckKind::UpperBound:
  case CheckKind::LowerBound:
  case CheckKind::NotFree:
  case CheckKind::GetAuxPtr:
  case CheckKind::EnsureAuxPtr:
    return CK;
  }
  llvm_unreachable("Bad CheckKind.");
  return CheckKind::ValidObject;
}

raw_ostream& operator<<(raw_ostream& OS, CheckKind CK) {
  switch (CK) {
  case CheckKind::ValidObject:
    OS << "ValidObject";
    return OS;
  case CheckKind::KnownAlignment:
    OS << "KnownAlignment";
    return OS;
  case CheckKind::Alignment:
    OS << "Alignment";
    return OS;
  case CheckKind::CanWrite:
    OS << "CanWrite";
    return OS;
  case CheckKind::UpperBound:
    OS << "UpperBound";
    return OS;
  case CheckKind::KnownLowerBound:
    OS << "KnownLowerBound";
    return OS;
  case CheckKind::LowerBound:
    OS << "LowerBound";
    return OS;
  case CheckKind::GetAuxPtr:
    OS << "GetAuxPtr";
    return OS;
  case CheckKind::EnsureAuxPtr:
    OS << "EnsureAuxPtr";
    return OS;
  case CheckKind::NotFree:
    OS << "NotFree";
    return OS;
  }
  llvm_unreachable("Bad CheckKind");
  return OS;
}

enum class AIDirection {
  Forward,
  Backward
};

struct CombinedDI {
  std::unordered_set<DILocation*> Locations; // This set may contain null.

  CombinedDI() = default;
  
  CombinedDI(DILocation* Location) {
    Locations.insert(Location);
  }

  bool operator==(const CombinedDI& Other) const {
    return Locations == Other.Locations;
  }

  size_t hash() const {
    size_t Result = Locations.size();
    for (DILocation* Location : Locations)
      Result += std::hash<DILocation*>()(Location);
    return Result;
  }
};

} // anonymous namespace

template<> struct std::hash<CombinedDI> {
  size_t operator()(const CombinedDI& Key) const {
    return Key.hash();
  }
};

template<> struct std::hash<std::pair<const CombinedDI*, const CombinedDI*>> {
  size_t operator()(const std::pair<const CombinedDI*, const CombinedDI*>& Key) const {
    return std::hash<const CombinedDI*>()(Key.first) + 3 * std::hash<const CombinedDI*>()(Key.second);
  }
};

namespace {

int64_t PositiveModulo(int64_t Offset, int64_t Alignment) {
  int64_t Result = Offset % Alignment;
  if (Result < 0)
    Result += Alignment;
  assert(Result >= 0);
  assert(Result < Alignment);
  return Result;
}

struct AccessCheck {
  Value* CanonicalPtr { nullptr };
  int64_t Offset { 0 };
  int64_t Size { 0 };
  CheckKind CK { CheckKind::ValidObject };

  AccessCheck() = default;
  
  AccessCheck(Value* CanonicalPtr, int64_t Offset, int64_t Size, CheckKind CK):
    CanonicalPtr(CanonicalPtr), Offset(Offset), Size(Size), CK(CK) {
    assert(CanonicalPtr);
    assert((int32_t)Offset == Offset);
    assert(Size >= 0);
    switch (CK) {
    case CheckKind::ValidObject:
    case CheckKind::CanWrite:
    case CheckKind::NotFree:
    case CheckKind::GetAuxPtr:
    case CheckKind::EnsureAuxPtr:
      assert(!Size);
      assert(!Offset);
      return;
    case CheckKind::Alignment:
    case CheckKind::KnownAlignment:
      assert(Size >= 1);
      assert(Size <= static_cast<int64_t>(WordSize));
      assert(!((Size - 1) & Size));
      assert(Offset >= 0);
      assert(Offset < Size);
      return;
    case CheckKind::UpperBound:
    case CheckKind::LowerBound:
    case CheckKind::KnownLowerBound:
      assert(!Size);
      return;
    }
    llvm_unreachable("Bad CheckKind.");
  }

  explicit operator bool() const {
    return !!CanonicalPtr;
  }

  bool operator<(const AccessCheck& Other) const {
    if (CanonicalPtr != Other.CanonicalPtr)
      return CanonicalPtr < Other.CanonicalPtr;

    // This is important: we require that the CheckKinds fit into the list in ascending order,
    // according to the order they are declared in CheckKind, ignoring whether they are known/merged
    // or not.
    if (FundamentalCheckKind(CK) != FundamentalCheckKind(Other.CK))
      return FundamentalCheckKind(CK) < FundamentalCheckKind(Other.CK);

    if (FundamentalCheckKind(CK) == CheckKind::Alignment && Size != Other.Size)
      return Size > Other.Size;

    if (Offset != Other.Offset) {
      if (CK == CheckKind::UpperBound)
        return Offset > Other.Offset;
      else
        return Offset < Other.Offset;
    }

    if (Size != Other.Size)
      return Size > Other.Size;

    // If everything else is equal, then the order is such that known comes before unknown.
    return CK < Other.CK;
  }

  // Note: we say that two AccessChecks are equal even if their DI's are different.
  bool operator==(const AccessCheck& Other) const {
    return Size == Other.Size && CanonicalPtr == Other.CanonicalPtr && Offset == Other.Offset
      && CK == Other.CK;
  }

  bool operator!=(const AccessCheck& Other) const {
    return !(*this == Other);
  }

  void print(raw_ostream& OS) const {
    OS << CK << "(" << CanonicalPtr << ":" << CanonicalPtr->getName();
    switch (CK) {
    case CheckKind::ValidObject:
    case CheckKind::CanWrite:
    case CheckKind::NotFree:
    case CheckKind::GetAuxPtr:
    case CheckKind::EnsureAuxPtr:
      break;
    case CheckKind::Alignment:
    case CheckKind::KnownAlignment:
      OS << ", Size = " << Size << ", Offset = " << Offset;
      break;
    case CheckKind::LowerBound:
    case CheckKind::KnownLowerBound:
    case CheckKind::UpperBound:
      OS << ", Offset = " << Offset;
      break;
    }
    OS << ")";
  }
};

raw_ostream& operator<<(raw_ostream& OS, const AccessCheck& AC) {
  AC.print(OS);
  return OS;
}

template<typename T>
raw_ostream& PrintVector(raw_ostream& OS, const std::vector<T>& V) {
  for (size_t Index = 0; Index < V.size(); ++Index) {
    if (Index)
      OS << ", ";
    OS << V[Index];
  }
  return OS;
}

raw_ostream& operator<<(raw_ostream& OS, const std::vector<AccessCheck>& Checks) {
  return PrintVector(OS, Checks);
}

struct AccessCheckWithDI : AccessCheck {
  const CombinedDI* DI { nullptr };

  AccessCheckWithDI() = default;

  AccessCheckWithDI(Value* CanonicalPtr, int64_t Offset, int64_t Size, CheckKind CK,
                    const CombinedDI* DI):
    AccessCheck(CanonicalPtr, Offset, Size, CK), DI(DI) {
  }

  AccessCheckWithDI(const AccessCheck& AC):
    AccessCheck(AC) {
  }

  AccessCheckWithDI& operator=(const AccessCheck& AC) {
    *(AccessCheck*)this = AC;
    DI = nullptr;
    return *this;
  }
};

raw_ostream& operator<<(raw_ostream& OS, const std::vector<AccessCheckWithDI>& Checks) {
  return PrintVector(OS, Checks);
}

struct ChecksOrBottom {
  bool Bottom;
  std::vector<AccessCheck> Checks;

  ChecksOrBottom(): Bottom(true) {}
};

struct ChecksWithDIOrBottom {
  bool Bottom;
  std::vector<AccessCheckWithDI> Checks;

  ChecksWithDIOrBottom(): Bottom(true) {}
};

struct PtrAndOffset {
  Value* HighP { nullptr };
  int64_t Offset { 0 };

  PtrAndOffset() = default;

  PtrAndOffset(Value* HighP, int64_t Offset): HighP(HighP), Offset(Offset) {}
};

struct GEPKey {
  Type* SourceTy { nullptr };
  Value* Pointer { nullptr };
  std::vector<Value*> Indices;

  GEPKey() = default;

  GEPKey(GetElementPtrInst* GEP):
    SourceTy(GEP->getSourceElementType()), Pointer(GEP->getPointerOperand()) {
    for (Value* Index : GEP->indices())
      Indices.push_back(Index);
  }

  bool operator==(const GEPKey& Other) const {
    return SourceTy == Other.SourceTy && Pointer == Other.Pointer && Indices == Other.Indices;
  }

  size_t hash() const {
    size_t Result = std::hash<Type*>()(SourceTy) + std::hash<Value*>()(Pointer) + Indices.size();
    for (Value* Index : Indices)
      Result += std::hash<Value*>()(Index);
    return Result;
  }
};

struct OptimizedAccessCheckOriginKey {
  uint32_t Size { 0 };
  uint8_t Alignment { 0 };
  uint8_t AlignmentOffset { 0 };
  bool NeedsWrite { false };
  DILocation* ScheduledDI { nullptr };
  const CombinedDI* SemanticDI { nullptr };

  OptimizedAccessCheckOriginKey() = default;

  OptimizedAccessCheckOriginKey(
    uint32_t Size, uint8_t Alignment, uint8_t AlignmentOffset, bool NeedsWrite,
    DILocation* ScheduledDI, const CombinedDI* SemanticDI):
    Size(Size), Alignment(Alignment), AlignmentOffset(AlignmentOffset),
    NeedsWrite(NeedsWrite), ScheduledDI(ScheduledDI), SemanticDI(SemanticDI) {
  }

  bool operator==(const OptimizedAccessCheckOriginKey& Other) const {
    return Size == Other.Size && Alignment == Other.Alignment
      && AlignmentOffset == Other.AlignmentOffset
      && NeedsWrite == Other.NeedsWrite && ScheduledDI == Other.ScheduledDI
      && SemanticDI == Other.SemanticDI;
  }

  size_t hash() const {
    return static_cast<size_t>(Size) + static_cast<size_t>(Alignment)
      + static_cast<size_t>(AlignmentOffset)
      + static_cast<size_t>(NeedsWrite) + std::hash<DILocation*>()(ScheduledDI)
      + std::hash<const CombinedDI*>()(SemanticDI);
  }
};

struct AlignmentAndOffset {
  uint8_t Alignment { 0 };
  uint8_t AlignmentOffset { 0 };

  AlignmentAndOffset() = default;

  AlignmentAndOffset(uint8_t Alignment, uint8_t AlignmentOffset):
    Alignment(Alignment), AlignmentOffset(AlignmentOffset) {
  }

  bool operator==(const AlignmentAndOffset& Other) const {
    return Alignment == Other.Alignment && AlignmentOffset == Other.AlignmentOffset;
  }

  size_t hash() const {
    return std::hash<uint8_t>()(Alignment) + AlignmentOffset;
  }
};

struct OptimizedAlignmentContradictionOriginKey {
  std::vector<AlignmentAndOffset> Alignments;
  DILocation* ScheduledDI { nullptr };
  const CombinedDI* SemanticDI { nullptr };

  OptimizedAlignmentContradictionOriginKey() = default;

  OptimizedAlignmentContradictionOriginKey(const std::vector<AlignmentAndOffset>& Alignments,
                                           DILocation* ScheduledDI, const CombinedDI* SemanticDI):
    Alignments(Alignments), ScheduledDI(ScheduledDI), SemanticDI(SemanticDI) {
  }

  bool operator==(const OptimizedAlignmentContradictionOriginKey& Other) const {
    return Alignments == Other.Alignments && ScheduledDI == Other.ScheduledDI
      && SemanticDI == Other.SemanticDI;
  }

  size_t hash() const {
    size_t Result = Alignments.size();
    for (AlignmentAndOffset Alignment : Alignments)
      Result += Alignment.hash();
    Result += std::hash<DILocation*>()(ScheduledDI);
    Result += std::hash<const CombinedDI*>()(SemanticDI);
    return Result;
  }
};

} // anonymous namespace

template<> struct std::hash<GEPKey> {
  size_t operator()(const GEPKey& Key) const {
    return Key.hash();
  }
};

template<> struct std::hash<OptimizedAccessCheckOriginKey> {
  size_t operator()(const OptimizedAccessCheckOriginKey& Key) const {
    return Key.hash();
  }
};

template<> struct std::hash<OptimizedAlignmentContradictionOriginKey> {
  size_t operator()(const OptimizedAlignmentContradictionOriginKey& Key) const {
    return Key.hash();
  }
};

namespace {

template<typename VectorT, typename FuncT>
void EraseIf(VectorT& V, const FuncT& F) {
  size_t SrcIndex = 0;
  size_t DstIndex = 0;
  while (SrcIndex < V.size()) {
    auto Value = std::move(V[SrcIndex++]);
    if (!F(Value))
      V[DstIndex++] = std::move(Value);
  }
  V.resize(DstIndex);
}

struct AuxBaseAndPtr {
  Value* BaseP { nullptr };
  Value* P { nullptr };

  AuxBaseAndPtr() = default;
  AuxBaseAndPtr(Value* BaseP, Value* P): BaseP(BaseP), P(P) {}
};

enum class CapabilityInferenceState {
  Bottom,
  Definite,
  Top
};

struct InferredCapability {
  CapabilityInferenceState State { CapabilityInferenceState::Bottom };
  Value* Capability;

  InferredCapability() = default;

  InferredCapability(CapabilityInferenceState State, Value* Capability):
    State(State), Capability(Capability) {
    if (State != CapabilityInferenceState::Definite)
      assert(!Capability);
    else
      assert(Capability);
  }

  bool operator==(const InferredCapability& Other) const {
    return State == Other.State && Capability == Other.Capability;
  }

  bool merge(const InferredCapability& Other) {
    if (*this == Other)
      return false;
    
    switch (State) {
    case CapabilityInferenceState::Bottom:
      *this = Other;
      return true;

    case CapabilityInferenceState::Definite:
      if (Other.State == CapabilityInferenceState::Bottom)
        return false;
      State = CapabilityInferenceState::Top;
      Capability = nullptr;
      return true;

    case CapabilityInferenceState::Top:
      return false;
    }

    llvm_unreachable("Bad inference state");
    return false;
  }
};

class Pizlonator {
  static constexpr unsigned TargetAS = 0;
  
  LLVMContext& C;
  Module &M;
  const DataLayout DLBefore;
  const DataLayout DL;

  unsigned PtrBits;
  Type* VoidTy;
  IntegerType* Int1Ty;
  IntegerType* Int8Ty;
  IntegerType* Int16Ty;
  IntegerType* Int32Ty;
  IntegerType* IntPtrTy;
  IntegerType* Int128Ty;
  Type* FloatTy;
  Type* DoubleTy;
  PointerType* RawPtrTy;
  StructType* FlightPtrTy;
  StructType* OriginNodeTy;
  StructType* FunctionOriginTy;
  StructType* OriginTy;
  StructType* InlineFrameTy;
  StructType* OriginWithEHTy;
  StructType* ObjectTy;
  StructType* FrameTy;
  StructType* ThreadTy;
  StructType* ConstantRelocationTy;
  StructType* ConstexprNodeTy;
  StructType* AlignmentAndOffsetTy;
  StructType* PizlonatedReturnValueTy;
  StructType* PtrPairTy;
  FunctionType* PizlonatedFuncTy;
  FunctionType* GlobalGetterTy;
  FunctionType* CtorDtorTy;
  FunctionType* SetjmpTy;
  FunctionType* SigsetjmpTy;
  Constant* RawNull;
  Constant* FlightNull;
  BitCastInst* Dummy;

  // Low-level functions used by codegen.
  FunctionCallee PollcheckSlow;
  FunctionCallee StoreBarrierForLowerSlow;
  FunctionCallee StorePtrAtomicWithPtrPairOutline;
  FunctionCallee ObjectEnsureAuxPtrOutline;
  FunctionCallee ThreadEnsureCCOutlineBufferSlow;
  FunctionCallee StrongCasPtr;
  FunctionCallee XchgPtr;
  FunctionCallee GetNextPtrBytesForVAArg;
  FunctionCallee Allocate;
  FunctionCallee AllocateWithAlignment;
  FunctionCallee OptimizedAlignmentContradiction;
  FunctionCallee OptimizedAccessCheckFail;
  FunctionCallee CheckFunctionCallFail;
  FunctionCallee Memset;
  FunctionCallee Memmove;
  FunctionCallee GlobalInitializationContextCreate;
  FunctionCallee GlobalInitializationContextAdd;
  FunctionCallee GlobalInitializationContextDestroy;
  FunctionCallee ExecuteConstantRelocations;
  FunctionCallee DeferOrRunGlobalCtor;
  FunctionCallee RunGlobalDtor;
  FunctionCallee Error;
  FunctionCallee RealMemset;
  FunctionCallee RealMemcpy;
  FunctionCallee LandingPad;
  FunctionCallee ResumeUnwind;
  FunctionCallee JmpBufCreate;
  FunctionCallee PromoteArgsToHeap;
  FunctionCallee PrepareToReturnWithData;
  FunctionCallee CCArgsCheckFailure;
  FunctionCallee CCRetsCheckFailure;
  FunctionCallee _Setjmp;
  FunctionCallee ExpectI1;
  FunctionCallee LifetimeStart;
  FunctionCallee LifetimeEnd;
  FunctionCallee StackCheckAsm;
  FunctionCallee ThreadlocalAddress;

  Constant* IsMarking;

  std::unordered_set<CombinedDI> CombinedDIs;
  std::unordered_map<std::pair<const CombinedDI*, const CombinedDI*>,
                     const CombinedDI*> CombinedDIMaker;
  std::unordered_map<DILocation*, const CombinedDI*> BasicDIs;
  
  std::unordered_map<std::string, GlobalVariable*> Strings;
  std::unordered_map<FunctionOriginKey, GlobalVariable*> FunctionOrigins;
  std::unordered_map<OriginKey, GlobalVariable*> Origins;
  std::unordered_map<InlineFrameKey, GlobalVariable*> InlineFrames;
  std::unordered_map<EHDataKey, GlobalVariable*> EHDatas; /* the value is a high-level GV, need to
                                                             lookup the getter. */
  std::unordered_map<Constant*, int> EHTypeIDs;
  std::unordered_map<CallBase*, unsigned> Setjmps;
  std::unordered_map<Type*, Constant*> CCTypes;
  std::unordered_map<Instruction*, std::vector<AccessCheckWithDI>> ChecksForInst;
  std::unordered_map<OptimizedAccessCheckOriginKey, GlobalVariable*> OptimizedAccessCheckOrigins;
  std::unordered_map<OptimizedAlignmentContradictionOriginKey,
                     GlobalVariable*> OptimizedAlignmentContradictionOrigins;
  std::unordered_map<Value*, AllocaInst*> CanonicalPtrAuxBaseVars;
  std::unordered_map<Instruction*, std::vector<AllocaInst*>> AuxBaseVarOperands;

  std::vector<GlobalVariable*> Globals;
  std::vector<Function*> Functions;
  std::vector<GlobalAlias*> Aliases;

  std::unordered_map<Type*, Type*> FlightedTypes;

  std::unordered_map<GlobalValue*, Function*> GlobalToGetter;
  std::unordered_map<GlobalValue*, GlobalVariable*> GlobalToGlobal;
  std::unordered_set<Value*> Getters;
  std::unordered_map<Function*, Function*> FunctionToHiddenFunction;

  std::string FunctionName;
  Function* OldF;
  Function* NewF;

  std::unordered_map<Instruction*, Type*> InstTypes;
  std::unordered_map<Instruction*, std::vector<Type*>> InstTypeVectors;
  std::unordered_map<InvokeInst*, LandingPadInst*> LPIs;
  
  BasicBlock* FirstRealBlock;

  BasicBlock* ReturnB;
  PHINode* ReturnPhi;
  BasicBlock* ResumeB;
  PHINode* RetSizePhi;
  BasicBlock* ReallyReturnB;

  bool UsesVastartOrZargs;
  size_t SnapshottedArgsFrameIndex;
  Value* SnapshottedArgsPtrForVastart;
  Value* SnapshottedArgsPtrForZargs;
  std::vector<Value*> Args;

  Value* MyThread;

  std::unordered_map<ValuePtr, size_t> FrameIndexMap;
  size_t FrameSize;
  Value* Frame;

  std::vector<Instruction*> ToErase;

  BitCastInst* makeDummy(Type* T) {
    return new BitCastInst(UndefValue::get(T), T, "dummy");
  }

  GlobalVariable* getString(StringRef Str) {
    auto iter = Strings.find(Str.str());
    if (iter != Strings.end())
      return iter->second;

    Constant* C = ConstantDataArray::getString(this->C, Str);
    GlobalVariable* Result = new GlobalVariable(
      M, C->getType(), true, GlobalVariable::PrivateLinkage, C, "filc_string");
    Strings[Str.str()] = Result;
    return Result;
  }

  std::string getFunctionName(Function *F) {
    std::string FunctionName;
    if (char* DemangledName = itaniumDemangle(F->getName())) {
      FunctionName = DemangledName;
      free(DemangledName);
    } else
      FunctionName = F->getName();
    return FunctionName;
  }

  // What does "CanCatch" mean in this context? CanCatch=true means we're at an origin that is either:
  // - a CallInst in a function that is !doesNotThrow, or
  // - an InvokeInst.
  //
  // Lots of origins don't meet this definition!
  Constant* getFunctionOrigin(bool CanCatch) {
    assert(OldF);
    
    FunctionOriginKey FOK(OldF, CanCatch);
    auto iter = FunctionOrigins.find(FOK);
    if (iter != FunctionOrigins.end())
      return iter->second;

    Constant* Personality = RawNull;
    if (CanCatch && OldF->hasPersonalityFn()) {
      assert(GlobalToGetter.count(cast<Function>(OldF->getPersonalityFn())));
      Personality = GlobalToGetter[cast<Function>(OldF->getPersonalityFn())];
    }
    
    bool CanThrow = !OldF->doesNotThrow();
    unsigned NumSetjmps = Setjmps.size();

    assert(FrameSize < UINT_MAX);
    
    Constant* C = ConstantStruct::get(
      FunctionOriginTy,
      { ConstantStruct::get(
          OriginNodeTy,
          { getString(getFunctionName(OldF)),
            OldF->getSubprogram() ? getString(OldF->getSubprogram()->getFilename()) : RawNull,
            ConstantInt::get(Int32Ty, FrameSize) }),
        Personality, ConstantInt::get(Int8Ty, CanThrow), ConstantInt::get(Int8Ty, CanCatch),
        ConstantInt::get(Int32Ty, NumSetjmps) });
    GlobalVariable* Result = new GlobalVariable(
      M, FunctionOriginTy, true, GlobalVariable::PrivateLinkage, C, "filc_function_origin");
    FunctionOrigins[FOK] = Result;
    return Result;
  }

  Constant* getEHData(LandingPadInst* LPI) {
    if (!LPI)
      return RawNull;
    assert(EHDatas.count(LPI));
    assert(GlobalToGetter.count(EHDatas[LPI]));
    return GlobalToGetter[EHDatas[LPI]];
  }

  Constant* getInlineFrame(DISubprogram* Subprogram, DILocation* InlinedAt, bool CanCatch) {
    assert(OldF);
    assert(InlinedAt);

    InlineFrameKey IFK(OldF, Subprogram, InlinedAt, CanCatch);
    auto iter = InlineFrames.find(IFK);
    if (iter != InlineFrames.end())
      return iter->second;

    unsigned Line = InlinedAt->getLine();
    unsigned Col = InlinedAt->getColumn();

    Constant* C = ConstantStruct::get(
      InlineFrameTy,
      { ConstantStruct::get(
          OriginNodeTy,
          { getString(Subprogram->getName()), getString(Subprogram->getFilename()),
            ConstantInt::get(Int32Ty, UINT_MAX) }),
        ConstantStruct::get(
          OriginTy,
          { getOriginNode(InlinedAt->getScope()->getSubprogram(), InlinedAt->getInlinedAt(),
                          CanCatch),
            ConstantInt::get(Int32Ty, Line), ConstantInt::get(Int32Ty, Col) }) });
    GlobalVariable* Result = new GlobalVariable(
      M, InlineFrameTy, true, GlobalVariable::PrivateLinkage, C, "filc_inline_frame");
    InlineFrames[IFK] = Result;
    return Result;
  }

  Constant* getOriginNode(DISubprogram* Subprogram, DILocation* InlinedAt, bool CanCatch) {
    assert(Subprogram);
    if (InlinedAt)
      return getInlineFrame(Subprogram, InlinedAt, CanCatch);
    return getFunctionOrigin(CanCatch);
  }

  // See the definition of CanCatch, above.
  Constant* getOrigin(DebugLoc Loc, bool CanCatch = false, LandingPadInst* LPI = nullptr) {
    assert(OldF);
    if (LPI)
      assert(CanCatch);
    
    DILocation* Impl = Loc.get();
    OriginKey OK(OldF, Impl, CanCatch, LPI);
    auto iter = Origins.find(OK);
    if (iter != Origins.end())
      return iter->second;

    unsigned Line = 0;
    unsigned Col = 0;
    Constant* OriginNode;
    if (Loc) {
      Line = Loc.getLine();
      Col = Loc.getCol();
      OriginNode = getOriginNode(Loc->getScope()->getSubprogram(), Loc->getInlinedAt(), CanCatch);
    } else
      OriginNode = getFunctionOrigin(CanCatch);

    GlobalVariable* Result;
    if (CanCatch && OldF->hasPersonalityFn()) {
      Constant* C = ConstantStruct::get(
        OriginWithEHTy,
        { OriginNode, ConstantInt::get(Int32Ty, Line), ConstantInt::get(Int32Ty, Col),
          getEHData(LPI) });
      Result = new GlobalVariable(
        M, OriginWithEHTy, true, GlobalVariable::PrivateLinkage, C, "filc_origin_with_eh");
    } else {
      Constant* C = ConstantStruct::get(
        OriginTy,
        { OriginNode, ConstantInt::get(Int32Ty, Line), ConstantInt::get(Int32Ty, Col) });
      Result = new GlobalVariable(
        M, OriginTy, true, GlobalVariable::PrivateLinkage, C, "filc_origin");
    }
    Origins[OK] = Result;
    return Result;
  }

  // See definition of CanCatch=true, above.
  Value* getCatchOrigin(DebugLoc Loc, LandingPadInst* LPI = nullptr) {
    return getOrigin(Loc, true, LPI);
  }

  Type* toFlightTypeImpl(Type* T) {
    assert(T != FlightPtrTy);
    
    if (isa<FunctionType>(T))
      return PizlonatedFuncTy;

    if (isa<TypedPointerType>(T)) {
      llvm_unreachable("Shouldn't ever see typed pointers");
      return nullptr;
    }

    if (isa<PointerType>(T)) {
      if (T->getPointerAddressSpace() == TargetAS) {
        assert(T == RawPtrTy);
        return FlightPtrTy;
      }
      return T;
    }

    if (StructType* ST = dyn_cast<StructType>(T)) {
      if (ST->isOpaque())
        return ST;
      std::vector<Type*> Elements;
      for (Type* InnerT : ST->elements())
        Elements.push_back(toFlightType(InnerT));
      if (ST->isLiteral())
        return StructType::get(C, Elements, ST->isPacked());
      std::string NewName = ("pizlonated_" + ST->getName()).str();
      return StructType::create(C, Elements, NewName, ST->isPacked());
    }
      
    if (ArrayType* AT = dyn_cast<ArrayType>(T))
      return ArrayType::get(toFlightType(AT->getElementType()), AT->getNumElements());
      
    if (FixedVectorType* VT = dyn_cast<FixedVectorType>(T)) {
      return FixedVectorType::get(
        toFlightType(VT->getElementType()), VT->getElementCount().getFixedValue());
    }

    if (isa<ScalableVectorType>(T)) {
      llvm_unreachable("Shouldn't see scalable vector types");
      return nullptr;
    }
    
    return T;
  }

  Type* toFlightType(Type* T) {
    auto iter = FlightedTypes.find(T);
    if (iter != FlightedTypes.end())
      return iter->second;

    Type* FlightT = toFlightTypeImpl(T);
    assert(T->isSized() == FlightT->isSized());
    FlightedTypes[T] = FlightT;
    return FlightT;
  }

  Value* expectBool(Value* Predicate, bool Expected, Instruction* InsertBefore) {
    CallInst* Result = CallInst::Create(
      ExpectI1, { Predicate, ConstantInt::getBool(Int1Ty, Expected) },
      Expected ? "filc_expect_true" : "filc_expect_false",
      InsertBefore);
    Result->setDebugLoc(InsertBefore->getDebugLoc());
    return Result;
  }

  Value* expectTrue(Value* Predicate, Instruction* InsertBefore) {
    return expectBool(Predicate, true, InsertBefore);
  }
  
  Value* expectFalse(Value* Predicate, Instruction* InsertBefore) {
    return expectBool(Predicate, false, InsertBefore);
  }

  Value* threadFieldPtr(Value* Thread, unsigned FieldIndex, const char* Name,
                        Instruction* InsertBefore) {
    GetElementPtrInst* GEP = GetElementPtrInst::Create(
      ThreadTy, Thread, { ConstantInt::get(IntPtrTy, 0), ConstantInt::get(Int32Ty, FieldIndex) },
      Name, InsertBefore);
    GEP->setDebugLoc(InsertBefore->getDebugLoc());
    return GEP;
  }

  Value* threadStackLimitPtr(Value* Thread, Instruction* InsertBefore) {
    return threadFieldPtr(Thread, 0, "filc_thread_stack_limit_ptr", InsertBefore);
  }

  Value* threadStatePtr(Value* Thread, Instruction* InsertBefore) {
    return threadFieldPtr(Thread, 1, "filc_thread_state_ptr", InsertBefore);
  }

  Value* threadTIDPtr(Value* Thread, Instruction* InsertBefore) {
    return threadFieldPtr(Thread, 2, "filc_thread_tid_ptr", InsertBefore);
  }

  Value* threadTopFramePtr(Value* Thread, Instruction* InsertBefore) {
    return threadFieldPtr(Thread, 3, "filc_thread_top_frame_ptr", InsertBefore);
  }

  Value* threadUnwindRegistersPtr(Value* Thread, Instruction* InsertBefore) {
    return threadFieldPtr(Thread, 5, "filc_thread_unwind_registers_ptr", InsertBefore);
  }

  Value* threadUnwindRegisterPtr(Value* Thread, unsigned RegisterIndex, Instruction* InsertBefore) {
    assert(RegisterIndex < NumUnwindRegisters);
    GetElementPtrInst* GEP = GetElementPtrInst::Create(
      FlightPtrTy, threadUnwindRegistersPtr(Thread, InsertBefore),
      { ConstantInt::get(IntPtrTy, RegisterIndex) }, "filc_unwind_register_ptr", InsertBefore);
    GEP->setDebugLoc(InsertBefore->getDebugLoc());
    return GEP;
  }

  Value* threadCookiePtrPtr(Value* Thread, Instruction* InsertBefore) {
    return threadFieldPtr(Thread, 6, "filc_thread_cookie_ptr_ptr", InsertBefore);
  }

  Value* threadCCInlineBufferPtr(Value* Thread, Instruction* InsertBefore) {
    return threadFieldPtr(Thread, 8, "filc_thread_cc_inline_buffer_ptr", InsertBefore);
  }

  Value* threadCCInlineAuxBufferPtr(Value* Thread, Instruction* InsertBefore) {
    return threadFieldPtr(Thread, 9, "filc_thread_cc_inline_aux_buffer_ptr", InsertBefore);
  }

  Value* threadCCOutlineBufferPtr(Value* Thread, Instruction* InsertBefore) {
    return threadFieldPtr(Thread, 10, "filc_thread_cc_outline_buffer_ptr", InsertBefore);
  }

  Value* threadCCOutlineAuxBufferPtr(Value* Thread, Instruction* InsertBefore) {
    return threadFieldPtr(Thread, 11, "filc_thread_cc_outline_aux_buffer_ptr", InsertBefore);
  }

  Value* threadCCOutlineSizePtr(Value* Thread, Instruction* InsertBefore) {
    return threadFieldPtr(Thread, 12, "filc_thread_cc_outline_size_ptr", InsertBefore);
  }

  Value* flightPtrPtr(Value* P, Instruction* InsertBefore) {
    if (isa<ConstantAggregateZero>(P))
      return RawNull;
    Instruction* Result = ExtractValueInst::Create(RawPtrTy, P, { 0 }, "filc_ptr_ptr", InsertBefore);
    Result->setDebugLoc(InsertBefore->getDebugLoc());
    return Result;
  }

  Value* flightPtrLower(Value* P, Instruction* InsertBefore) {
    if (isa<ConstantAggregateZero>(P))
      return RawNull;
    Instruction* Result = ExtractValueInst::Create(RawPtrTy, P, { 1 }, "filc_ptr_ptr", InsertBefore);
    Result->setDebugLoc(InsertBefore->getDebugLoc());
    return Result;
  }

  Value* flightPtrPtrAsInt(Value* P, Instruction* InsertBefore) {
    Instruction* PtrAsInt = new PtrToIntInst(
      flightPtrPtr(P, InsertBefore), IntPtrTy, "filc_ptr_ptr_as_int", InsertBefore);
    PtrAsInt->setDebugLoc(InsertBefore->getDebugLoc());
    return PtrAsInt;
  }

  Value* flightPtrOffset(Value* P, Instruction* InsertBefore) {
    Instruction* LowerAsInt = new PtrToIntInst(
      flightPtrLower(P, InsertBefore), IntPtrTy, "filc_ptr_lower_as_int", InsertBefore);
    LowerAsInt->setDebugLoc(InsertBefore->getDebugLoc());
    Instruction* Result = BinaryOperator::Create(
      Instruction::Sub, flightPtrPtrAsInt(P, InsertBefore), LowerAsInt, "filc_ptr_offset",
      InsertBefore);
    Result->setDebugLoc(InsertBefore->getDebugLoc());
    return Result;
  }

  Value* createFlightPtr(Value* Lower, Value* Ptr, Instruction* InsertBefore) {
    Instruction* Result = InsertValueInst::Create(
      UndefValue::get(FlightPtrTy), Ptr, { 0 }, "filc_insert_ptr", InsertBefore);
    Result->setDebugLoc(InsertBefore->getDebugLoc());
    Result = InsertValueInst::Create(Result, Lower, { 1 }, "filc_insert_lower", InsertBefore);
    Result->setDebugLoc(InsertBefore->getDebugLoc());
    return Result;
  }

  Value* flightPtrForPayload(Value* Payload, Instruction* InsertBefore) {
    return createFlightPtr(Payload, Payload, InsertBefore);
  }

  Value* lowerForObject(Value* Object, Instruction* InsertBefore) {
    Instruction* GEP = GetElementPtrInst::Create(
      ObjectTy, Object, { ConstantInt::get(IntPtrTy, 1) }, "filc_lower_for_object", InsertBefore);
    GEP->setDebugLoc(InsertBefore->getDebugLoc());
    return GEP;
  }

  Value* objectForLower(Value* Lower, Instruction* InsertBefore) {
    Instruction* GEP = GetElementPtrInst::Create(
      ObjectTy, Lower, { ConstantInt::get(IntPtrTy, -(intptr_t)1) }, "filc_object_for_lower",
      InsertBefore);
    GEP->setDebugLoc(InsertBefore->getDebugLoc());
    return GEP;
  }

  Value* flightPtrForObject(Value* Object, Instruction* InsertBefore) {
    return flightPtrForPayload(lowerForObject(Object, InsertBefore), InsertBefore);
  }

  Value* flightPtrWithPtr(Value* FlightPtr, Value* NewRawPtr, Instruction* InsertBefore) {
    return createFlightPtr(flightPtrLower(FlightPtr, InsertBefore), NewRawPtr, InsertBefore);
  }

  Value* flightPtrWithOffset(Value* FlightPtr, Value* Offset, Instruction* InsertBefore) {
    Instruction* GEP = GetElementPtrInst::Create(
      Int8Ty, flightPtrPtr(FlightPtr, InsertBefore), { Offset }, "filc_ptr_with_offset",
      InsertBefore);
    GEP->setDebugLoc(InsertBefore->getDebugLoc());
    return flightPtrWithPtr(FlightPtr, GEP, InsertBefore);
  }

  Value* ptrToUpperForLower(Value* Lower, Instruction* InsertBefore) {
    Instruction* UpperPtr = GetElementPtrInst::Create(
      ObjectTy, Lower,
      { ConstantInt::get(IntPtrTy, -(intptr_t)1), ConstantInt::get(Int32Ty, 0) },
      "filc_object_upper_ptr", InsertBefore); 
    UpperPtr->setDebugLoc(InsertBefore->getDebugLoc());
    return UpperPtr;
  }

  Value* ptrToAuxForLower(Value* Lower, Instruction* InsertBefore)
  {
    Instruction* AuxPtr = GetElementPtrInst::Create(
      ObjectTy, Lower,
      { ConstantInt::get(IntPtrTy, -(intptr_t)1), ConstantInt::get(Int32Ty, 1) },
      "filc_object_aux_ptr", InsertBefore);
    AuxPtr->setDebugLoc(InsertBefore->getDebugLoc());
    return AuxPtr;
  }

  Value* upperForLower(Value* Lower, Instruction* InsertBefore) {
    Instruction* Upper = new LoadInst(
      RawPtrTy, ptrToUpperForLower(Lower, InsertBefore), "filc_object_upper_load", InsertBefore);
    Upper->setDebugLoc(InsertBefore->getDebugLoc());
    return Upper;
  }

  Value* auxForLower(Value* Lower, Instruction* InsertBefore) {
    Instruction* Aux = new LoadInst(
      IntPtrTy, ptrToAuxForLower(Lower, InsertBefore), "filc_object_aux_load", InsertBefore);
    Aux->setDebugLoc(InsertBefore->getDebugLoc());
    return Aux;
  }

  Value* flagsForLower(Value* Lower, Instruction* InsertBefore) {
    Value* Aux = auxForLower(Lower, InsertBefore);
    Instruction* Flags = BinaryOperator::Create(
      Instruction::LShr, Aux, ConstantInt::get(IntPtrTy, ObjectAuxFlagsShift), "filc_object_flags",
      InsertBefore);
    Flags->setDebugLoc(InsertBefore->getDebugLoc());
    return Flags;
  }

  Value* auxPtrForLower(Value* Lower, Instruction* InsertBefore) {
    Value* Aux = auxForLower(Lower, InsertBefore);
    Instruction* AuxPtrAsInt = BinaryOperator::Create(
      Instruction::And, Aux, ConstantInt::get(IntPtrTy, ObjectAuxPtrMask), "filc_aux_ptr_as_int",
      InsertBefore);
    AuxPtrAsInt->setDebugLoc(InsertBefore->getDebugLoc());
    Instruction* AuxPtr = new IntToPtrInst(AuxPtrAsInt, RawPtrTy, "filc_aux_ptr", InsertBefore);
    AuxPtr->setDebugLoc(InsertBefore->getDebugLoc());
    return AuxPtr;
  }

  Value* badFlightPtr(Value* P, Instruction* InsertBefore) {
    return createFlightPtr(RawNull, P, InsertBefore);
  }

  Value* flightPtrPtrPtr(Value* P, Instruction* InsertBefore) {
    Instruction* GEP = GetElementPtrInst::Create(
      FlightPtrTy, P, { ConstantInt::get(IntPtrTy, 0), ConstantInt::get(Int32Ty, 0) },
      "filc_flight_ptr_ptr", InsertBefore);
    GEP->setDebugLoc(InsertBefore->getDebugLoc());
    return GEP;
  }

  Value* flightPtrLowerPtr(Value* P, Instruction* InsertBefore) {
    Instruction* GEP = GetElementPtrInst::Create(
      FlightPtrTy, P, { ConstantInt::get(IntPtrTy, 0), ConstantInt::get(Int32Ty, 1) },
      "filc_flight_ptr_ptr", InsertBefore);
    GEP->setDebugLoc(InsertBefore->getDebugLoc());
    return GEP;
  }

  Value* flightPtrForWord(Value* Word, Instruction* InsertBefore) {
    if (isa<ConstantAggregateZero>(Word))
      return FlightNull;
    Instruction* IntRawPtr = new TruncInst(
      Word, IntPtrTy, "filc_flight_word_raw_ptr_int", InsertBefore);
    IntRawPtr->setDebugLoc(InsertBefore->getDebugLoc());
    Instruction* RawPtr = new IntToPtrInst(IntRawPtr, RawPtrTy, "filc_flight_ptr_ptr", InsertBefore);
    RawPtr->setDebugLoc(InsertBefore->getDebugLoc());
    Instruction* Shifted = BinaryOperator::Create(
      Instruction::LShr, Word, ConstantInt::get(Int128Ty, 64), "filc_flight_word_lower_shift",
      InsertBefore);
    Shifted->setDebugLoc(InsertBefore->getDebugLoc());
    Instruction* IntLower = new TruncInst(Shifted, IntPtrTy, "filc_flight_word_lower_int",
                                          InsertBefore);
    IntLower->setDebugLoc(InsertBefore->getDebugLoc());
    Instruction* Lower = new IntToPtrInst(IntLower, RawPtrTy, "filc_flight_ptr_lower", InsertBefore);
    Lower->setDebugLoc(InsertBefore->getDebugLoc());
    return createFlightPtr(Lower, RawPtr, InsertBefore);
  }

  Value* wordForFlightPtr(Value* P, Instruction* InsertBefore) {
    Instruction* PtrAsInt = new PtrToIntInst(
      flightPtrPtr(P, InsertBefore), IntPtrTy, "filc_ptr_as_int", InsertBefore);
    PtrAsInt->setDebugLoc(InsertBefore->getDebugLoc());
    Instruction* LowerAsInt = new PtrToIntInst(
      flightPtrLower(P, InsertBefore), IntPtrTy, "filc_object_as_int", InsertBefore);
    LowerAsInt->setDebugLoc(InsertBefore->getDebugLoc());
    Instruction* CastedPtr = new ZExtInst(PtrAsInt, Int128Ty, "filc_ptr_zext", InsertBefore);
    CastedPtr->setDebugLoc(InsertBefore->getDebugLoc());
    Instruction* CastedLower = new ZExtInst(LowerAsInt, Int128Ty, "filc_lower_zext", InsertBefore);
    CastedLower->setDebugLoc(InsertBefore->getDebugLoc());
    Instruction* ShiftedLower = BinaryOperator::Create(
      Instruction::Shl, CastedLower, ConstantInt::get(Int128Ty, 64), "filc_lower_shifted",
      InsertBefore);
    ShiftedLower->setDebugLoc(InsertBefore->getDebugLoc());
    Instruction* Word = BinaryOperator::Create(
      Instruction::Or, CastedPtr, ShiftedLower, "filc_ptr_word", InsertBefore);
    Word->setDebugLoc(InsertBefore->getDebugLoc());
    return Word;
  }

  Value* loadFlightPtr(Value* P, Instruction* InsertBefore) {
    Instruction* Lower = new LoadInst(
      RawPtrTy, flightPtrLowerPtr(P, InsertBefore), "filc_flight_load_lower", false, Align(8),
      AtomicOrdering::Monotonic, SyncScope::System, InsertBefore);
    Lower->setDebugLoc(InsertBefore->getDebugLoc());
    Instruction* RawPtr = new LoadInst(
      RawPtrTy, flightPtrPtrPtr(P, InsertBefore), "filc_flight_load_raw_ptr", false, Align(8),
      AtomicOrdering::Monotonic, SyncScope::System, InsertBefore);
    RawPtr->setDebugLoc(InsertBefore->getDebugLoc());
    return createFlightPtr(Lower, RawPtr, InsertBefore);
  }

  Value* loadPtr(
    Value* P, Value* BaseAuxP, Value* AuxP, bool isVolatile, Align A, AtomicOrdering AO,
    MemoryKind MK, Instruction* InsertBefore) {
    if (MK != MemoryKind::Heap) {
      assert(!isVolatile);
      assert(AO == AtomicOrdering::NotAtomic);
    }
    Value* BaseAuxPIsNull;
    BasicBlock* OriginalB = InsertBefore->getParent();
    if (MK == MemoryKind::Heap) {
      Instruction* BaseAuxPIsNullInst = new ICmpInst(
        InsertBefore, ICmpInst::ICMP_EQ, BaseAuxP, RawNull, "filc_base_aux_ptr_is_null");
      BaseAuxPIsNullInst->setDebugLoc(InsertBefore->getDebugLoc());
      BaseAuxPIsNull = BaseAuxPIsNullInst;
    } else
      BaseAuxPIsNull = ConstantInt::getBool(Int1Ty, false);
    Instruction* NotNullCase = SplitBlockAndInsertIfElse(BaseAuxPIsNull, InsertBefore, false);
    Instruction* LowerAsIntLoad = new LoadInst(
      IntPtrTy, AuxP, "filc_load_lower", isVolatile, Align(WordSize),
      MK == MemoryKind::Heap ? getMergedAtomicOrdering(AtomicOrdering::Monotonic, AO) : AO,
      SyncScope::System, NotNullCase);
    LowerAsIntLoad->setDebugLoc(InsertBefore->getDebugLoc());
    PHINode* LowerAsInt = PHINode::Create(IntPtrTy, 2, "filc_lower_as_int_phi", InsertBefore);
    LowerAsInt->setDebugLoc(InsertBefore->getDebugLoc());
    LowerAsInt->addIncoming(ConstantInt::get(IntPtrTy, 0), OriginalB);
    LowerAsInt->addIncoming(LowerAsIntLoad, LowerAsIntLoad->getParent());

    auto FinishCreatingFlight = [&] (Instruction* Where) {
      Instruction* LowerToPtr = new IntToPtrInst(
        LowerAsInt, RawPtrTy, "filc_lower_as_ptr", Where);
      LowerToPtr->setDebugLoc(InsertBefore->getDebugLoc());
      Instruction* RawPtrLoad = new LoadInst(
        RawPtrTy, P, "filc_atomic_case_load_raw_ptr", isVolatile, std::max(A, Align(WordSize)), AO,
        SyncScope::System, Where);
      RawPtrLoad->setDebugLoc(InsertBefore->getDebugLoc());
      return createFlightPtr(LowerToPtr, RawPtrLoad, Where);
    };
    
    if (MK == MemoryKind::Heap) {
      Instruction* Masked = BinaryOperator::Create(
        Instruction::And, LowerAsInt, ConstantInt::get(IntPtrTy, AtomicBoxBit),
        "filc_lower_atomic_box_bit", InsertBefore);
      Masked->setDebugLoc(InsertBefore->getDebugLoc());
      Instruction* IsNotBox = new ICmpInst(
        InsertBefore, ICmpInst::ICMP_EQ, Masked, ConstantInt::get(IntPtrTy, 0),
        "filc_lower_is_not_box");
      IsNotBox->setDebugLoc(InsertBefore->getDebugLoc());
      Instruction* ThenTerm;
      Instruction* ElseTerm;
      SplitBlockAndInsertIfThenElse(expectTrue(IsNotBox, InsertBefore), InsertBefore,
                                    &ThenTerm, &ElseTerm);
      Value* AtomicCaseNotBoxPtr = nullptr;
      if (isVolatile || AO != AtomicOrdering::NotAtomic)
        AtomicCaseNotBoxPtr = FinishCreatingFlight(ThenTerm);
      Instruction* BoxAsInt = BinaryOperator::Create(
        Instruction::And, LowerAsInt, ConstantInt::get(IntPtrTy, ~AtomicBoxBit),
        "filc_box_as_int", ElseTerm);
      BoxAsInt->setDebugLoc(InsertBefore->getDebugLoc());
      Instruction* Box = new IntToPtrInst(BoxAsInt, RawPtrTy, "filc_box", ElseTerm);
      Box->setDebugLoc(InsertBefore->getDebugLoc());
      if (isVolatile || AO != AtomicOrdering::NotAtomic) {
        // FIXME: Maybe it would be better if this was a function call.
        Instruction* PtrWord = new LoadInst(
          Int128Ty, Box, "filc_ptr_word_from_box", isVolatile, std::max(A, Align(FlightPtrAlign)),
          getMergedAtomicOrdering(AtomicOrdering::Monotonic, AO), SyncScope::System, ElseTerm);
        PtrWord->setDebugLoc(InsertBefore->getDebugLoc());
        Value* AtomicCaseBoxPtr = flightPtrForWord(PtrWord, ElseTerm);
        PHINode* Result = PHINode::Create(FlightPtrTy, 2, "filc_ptr_load_phi", InsertBefore);
        Result->setDebugLoc(InsertBefore->getDebugLoc());
        Result->addIncoming(AtomicCaseNotBoxPtr, ThenTerm->getParent());
        Result->addIncoming(AtomicCaseBoxPtr, ElseTerm->getParent());
        return Result;
      }
      Instruction* LowerFromBoxLoadInt = new LoadInst(
        IntPtrTy, flightPtrLowerPtr(Box, ElseTerm), "filc_lower_from_box", isVolatile,
        std::max(A, Align(WordSize)), getMergedAtomicOrdering(AtomicOrdering::Monotonic, AO),
        SyncScope::System, ElseTerm);
      LowerFromBoxLoadInt->setDebugLoc(InsertBefore->getDebugLoc());
      PHINode* NewLowerAsInt = PHINode::Create(IntPtrTy, 2, "filc_lower_as_int_phi2", InsertBefore);
      NewLowerAsInt->setDebugLoc(InsertBefore->getDebugLoc());
      NewLowerAsInt->addIncoming(LowerAsInt, ThenTerm->getParent());
      NewLowerAsInt->addIncoming(LowerFromBoxLoadInt, ElseTerm->getParent());
      LowerAsInt = NewLowerAsInt;
    }
    return FinishCreatingFlight(InsertBefore);
  }

  Value* loadPtr(
    Value* P, AuxBaseAndPtr Aux, bool isVolatile, Align A, AtomicOrdering AO, MemoryKind MK,
    Instruction* InsertBefore) {
    return loadPtr(P, Aux.BaseP, Aux.P, isVolatile, A, AO, MK, InsertBefore);
  }

  Value* loadPtr(Value* P, Value* BaseAuxP, Value* AuxP, Instruction* InsertBefore) {
    return loadPtr(
      P, BaseAuxP, AuxP, false, Align(WordSize), AtomicOrdering::NotAtomic, MemoryKind::Heap,
      InsertBefore);
  }

  Value* loadPtr(Value* P, AuxBaseAndPtr Aux, Instruction* InsertBefore) {
    return loadPtr(P, Aux.BaseP, Aux.P, InsertBefore);
  }

  void storeBarrierForLower(Value* Lower, Instruction* InsertBefore) {
    assert(MyThread);
    DebugLoc DL = InsertBefore->getDebugLoc();
    ICmpInst* NullObject = new ICmpInst(
      InsertBefore, ICmpInst::ICMP_EQ, Lower, RawNull, "filc_barrier_null_object");
    NullObject->setDebugLoc(DL);
    Instruction* NotNullTerm = SplitBlockAndInsertIfElse(NullObject, InsertBefore, false);
    LoadInst* IsMarkingByte = new LoadInst(Int8Ty, IsMarking, "filc_is_marking_byte", NotNullTerm);
    IsMarkingByte->setDebugLoc(DL);
    ICmpInst* IsNotMarking = new ICmpInst(
      NotNullTerm, ICmpInst::ICMP_EQ, IsMarkingByte, ConstantInt::get(Int8Ty, 0),
      "filc_is_not_marking");
    IsNotMarking->setDebugLoc(DL);
    Instruction* IsMarkingTerm = SplitBlockAndInsertIfElse(
      expectTrue(IsNotMarking, NotNullTerm), NotNullTerm, false);
    CallInst::Create(StoreBarrierForLowerSlow, { MyThread, Lower }, "", IsMarkingTerm)
      ->setDebugLoc(DL);
  }
  
  void storeBarrierForValue(Value* V, Instruction* InsertBefore) {
    storeBarrierForLower(flightPtrLower(V, InsertBefore), InsertBefore);
  }
  
  void storePtr(
    Value* V, Value* P, Value* AuxP, bool isVolatile, Align A, AtomicOrdering AO, MemoryKind MK,
    Instruction* InsertBefore) {
    if (MK != MemoryKind::Heap) {
      assert(!isVolatile);
      assert(AO == AtomicOrdering::NotAtomic);
    }

    if (MK == MemoryKind::Heap && (AO != AtomicOrdering::NotAtomic || isVolatile)) {
      CallInst::Create(StorePtrAtomicWithPtrPairOutline, { MyThread, P, AuxP, V }, "", InsertBefore)
        ->setDebugLoc(InsertBefore->getDebugLoc());
      return;
    }
    
    if (MK == MemoryKind::Heap)
      storeBarrierForValue(V, InsertBefore);

    (new StoreInst(
      flightPtrLower(V, InsertBefore), AuxP, isVolatile, std::max(A, Align(WordSize)),
      MK == MemoryKind::Heap ? getMergedAtomicOrdering(AtomicOrdering::Monotonic, AO) : AO,
      SyncScope::System, InsertBefore))->setDebugLoc(InsertBefore->getDebugLoc());
    (new StoreInst(
      flightPtrPtr(V, InsertBefore), P, isVolatile, std::max(A, Align(WordSize)), AO,
      SyncScope::System, InsertBefore))->setDebugLoc(InsertBefore->getDebugLoc());
  }

  void storePtr(Value* V, Value* P, Value* AuxP, Instruction* InsertBefore) {
    storePtr(V, P, AuxP, false, Align(WordSize), AtomicOrdering::NotAtomic, MemoryKind::Heap,
             InsertBefore);
  }

  // This happens to work just as well for raw types and flight types, and that's important.
  bool hasPtrs(Type *T) {
    if (isa<FunctionType>(T)) {
      llvm_unreachable("shouldn't see function types in hasPtrs");
      return false;
    }

    if (isa<TypedPointerType>(T)) {
      llvm_unreachable("Shouldn't ever see typed pointers");
      return false;
    }

    if (isa<PointerType>(T)) {
      assert (!T->getPointerAddressSpace());
      return true;
    }

    if (T == FlightPtrTy)
      return true;

    if (StructType* ST = dyn_cast<StructType>(T)) {
      for (unsigned Index = ST->getNumElements(); Index--;) {
        Type* InnerT = ST->getElementType(Index);
        if (hasPtrs(InnerT))
          return true;
      }
      return false;
    }
      
    if (ArrayType* AT = dyn_cast<ArrayType>(T))
      return hasPtrs(AT->getElementType());

    if (FixedVectorType* VT = dyn_cast<FixedVectorType>(T))
      return hasPtrs(VT->getElementType());

    if (isa<ScalableVectorType>(T)) {
      llvm_unreachable("Shouldn't ever see scalable vectors in hasPtrs");
      return false;
    }
    
    return false;
  }

  Align lowAlign(Type* T, Align A, AtomicOrdering AO) {
    if (needToCheckAlignment(T) || AO != AtomicOrdering::NotAtomic)
      return A;
    return Align(1);
  }

  Value* loadValueRecurseAfterCheck(
    Type* T, Value* P, Value* BaseAuxP, Value* AuxP,
    bool isVolatile, Align A, AtomicOrdering AO, SyncScope::ID SS, MemoryKind MK,
    Instruction* InsertBefore) {
    A = std::min(DL.getABITypeAlign(T), A);
    
    if (!hasPtrs(T))
      return new LoadInst(T, P, "filc_load", isVolatile, lowAlign(T, A, AO), AO, SS, InsertBefore);
    
    if (isa<FunctionType>(T)) {
      llvm_unreachable("shouldn't see function types in loadValueRecurseAfterCheck");
      return nullptr;
    }

    if (isa<TypedPointerType>(T)) {
      llvm_unreachable("Shouldn't ever see typed pointers");
      return nullptr;
    }

    assert(T != FlightPtrTy);

    if (T == RawPtrTy)
      return loadPtr(P, BaseAuxP, AuxP, isVolatile, A, AO, MK, InsertBefore);

    assert(!isa<PointerType>(T));

    if (StructType* ST = dyn_cast<StructType>(T)) {
      Value* Result = UndefValue::get(toFlightType(T));
      for (unsigned Index = ST->getNumElements(); Index--;) {
        Type* InnerT = ST->getElementType(Index);
        Value *InnerP = GetElementPtrInst::Create(
          ST, P, { ConstantInt::get(Int32Ty, 0), ConstantInt::get(Int32Ty, Index) },
          "filc_InnerP_struct", InsertBefore);
        Value* InnerAuxP = GetElementPtrInst::Create(
          ST, AuxP, { ConstantInt::get(Int32Ty, 0), ConstantInt::get(Int32Ty, Index) },
          "filc_InnerAuxP_struct", InsertBefore);
        Value* V = loadValueRecurseAfterCheck(
          InnerT, InnerP, BaseAuxP, InnerAuxP, isVolatile, A, AO, SS, MK, InsertBefore);
        Result = InsertValueInst::Create(Result, V, Index, "filc_insert_struct", InsertBefore);
      }
      return Result;
    }
      
    if (ArrayType* AT = dyn_cast<ArrayType>(T)) {
      Value* Result = UndefValue::get(toFlightType(AT));
      assert(static_cast<unsigned>(AT->getNumElements()) == AT->getNumElements());
      for (unsigned Index = static_cast<unsigned>(AT->getNumElements()); Index--;) {
        Value *InnerP = GetElementPtrInst::Create(
          AT, P, { ConstantInt::get(IntPtrTy, 0), ConstantInt::get(IntPtrTy, Index) },
          "filc_InnerP_array", InsertBefore);
        Value *InnerAuxP = GetElementPtrInst::Create(
          AT, AuxP, { ConstantInt::get(IntPtrTy, 0), ConstantInt::get(IntPtrTy, Index) },
          "filc_InnerAuxP_array", InsertBefore);
        Value* V = loadValueRecurseAfterCheck(
          AT->getElementType(), InnerP, BaseAuxP, InnerAuxP, isVolatile, A, AO, SS, MK, InsertBefore);
        Result = InsertValueInst::Create(Result, V, Index, "filc_insert_array", InsertBefore);
      }
      return Result;
    }
      
    if (FixedVectorType* VT = dyn_cast<FixedVectorType>(T)) {
      Value* Result = UndefValue::get(toFlightType(VT));
      for (unsigned Index = VT->getElementCount().getFixedValue(); Index--;) {
        Value *InnerP = GetElementPtrInst::Create(
          VT, P, { ConstantInt::get(IntPtrTy, 0), ConstantInt::get(IntPtrTy, Index) },
          "filc_InnerP_vector", InsertBefore);
        Value *InnerAuxP = GetElementPtrInst::Create(
          VT, AuxP, { ConstantInt::get(IntPtrTy, 0), ConstantInt::get(IntPtrTy, Index) },
          "filc_InnerAuxP_vector", InsertBefore);
        Value* V = loadValueRecurseAfterCheck(
          VT->getElementType(), InnerP, BaseAuxP, InnerAuxP, isVolatile, A, AO, SS, MK, InsertBefore);
        Result = InsertElementInst::Create(
          Result, V, ConstantInt::get(IntPtrTy, Index), "filc_insert_vector", InsertBefore);
      }
      return Result;
    }

    if (isa<ScalableVectorType>(T)) {
      llvm_unreachable("Shouldn't see scalable vector types in loadValueRecurseAfterCheck");
      return nullptr;
    }

    llvm_unreachable("Should not get here.");
    return nullptr;
  }

  Value* loadValueRecurseAfterCheck(
    Type* T, Value* P, AuxBaseAndPtr Aux,
    bool isVolatile, Align A, AtomicOrdering AO, SyncScope::ID SS, MemoryKind MK,
    Instruction* InsertBefore) {
    return loadValueRecurseAfterCheck(
      T, P, Aux.BaseP, Aux.P, isVolatile, A, AO, SS, MK, InsertBefore);
  }
  
  void storeValueRecurseAfterCheck(
    Type* T, Value* V, Value* P, Value* AuxP,
    bool isVolatile, Align A, AtomicOrdering AO, SyncScope::ID SS, MemoryKind MK,
    Instruction* InsertBefore) {
    A = std::min(DL.getABITypeAlign(T), A);
    
    if (!hasPtrs(T)) {
      new StoreInst(V, P, isVolatile, lowAlign(T, A, AO), AO, SS, InsertBefore);
      return;
    }
    
    if (isa<FunctionType>(T)) {
      llvm_unreachable("shouldn't see function types in storeValueRecurseAfterCheck");
      return;
    }

    if (isa<TypedPointerType>(T)) {
      llvm_unreachable("Shouldn't ever see typed pointers");
      return;
    }

    assert(T != FlightPtrTy);

    if (T == RawPtrTy) {
      storePtr(V, P, AuxP, isVolatile, A, AO, MK, InsertBefore);
      return;
    }

    assert(!isa<PointerType>(T));

    if (StructType* ST = dyn_cast<StructType>(T)) {
      for (unsigned Index = ST->getNumElements(); Index--;) {
        Type* InnerT = ST->getElementType(Index);
        Value *InnerP = GetElementPtrInst::Create(
          ST, P, { ConstantInt::get(Int32Ty, 0), ConstantInt::get(Int32Ty, Index) },
          "filc_InnerP_struct", InsertBefore);
        Value *InnerAuxP = GetElementPtrInst::Create(
          ST, AuxP, { ConstantInt::get(Int32Ty, 0), ConstantInt::get(Int32Ty, Index) },
          "filc_InnerAuxP_struct", InsertBefore);
        Value* InnerV = ExtractValueInst::Create(
          toFlightType(InnerT), V, { Index }, "filc_extract_struct", InsertBefore);
        storeValueRecurseAfterCheck(
          InnerT, InnerV, InnerP, InnerAuxP, isVolatile, A, AO, SS, MK, InsertBefore);
      }
      return;
    }
      
    if (ArrayType* AT = dyn_cast<ArrayType>(T)) {
      assert(static_cast<unsigned>(AT->getNumElements()) == AT->getNumElements());
      for (unsigned Index = static_cast<unsigned>(AT->getNumElements()); Index--;) {
        Value *InnerP = GetElementPtrInst::Create(
          AT, P, { ConstantInt::get(IntPtrTy, 0), ConstantInt::get(IntPtrTy, Index) },
          "filc_InnerP_array", InsertBefore);
        Value *InnerAuxP = GetElementPtrInst::Create(
          AT, AuxP, { ConstantInt::get(IntPtrTy, 0), ConstantInt::get(IntPtrTy, Index) },
          "filc_InnerAuxP_array", InsertBefore);
        Value* InnerV = ExtractValueInst::Create(
          toFlightType(AT->getElementType()), V, { Index }, "filc_extract_array", InsertBefore);
        storeValueRecurseAfterCheck(
          AT->getElementType(), InnerV, InnerP, InnerAuxP, isVolatile, A, AO, SS, MK, InsertBefore);
      }
      return;
    }
      
    if (FixedVectorType* VT = dyn_cast<FixedVectorType>(T)) {
      for (unsigned Index = VT->getElementCount().getFixedValue(); Index--;) {
        Value *InnerP = GetElementPtrInst::Create(
          VT, P, { ConstantInt::get(IntPtrTy, 0), ConstantInt::get(IntPtrTy, Index) },
          "filc_InnerP_vector", InsertBefore);
        Value *InnerAuxP = GetElementPtrInst::Create(
          VT, AuxP, { ConstantInt::get(IntPtrTy, 0), ConstantInt::get(IntPtrTy, Index) },
          "filc_InnerAuxP_vector", InsertBefore);
        Value* InnerV = ExtractElementInst::Create(
          V, ConstantInt::get(IntPtrTy, Index), "filc_extract_vector", InsertBefore);
        storeValueRecurseAfterCheck(
          VT->getElementType(), InnerV, InnerP, InnerAuxP, isVolatile, A, AO, SS, MK, InsertBefore);
      }
      return;
    }

    if (isa<ScalableVectorType>(T)) {
      llvm_unreachable("Shouldn't see scalable vector types in storeValueRecurseAfterCheck");
      return;
    }

    llvm_unreachable("Should not get here.");
  }

  Value* allocateObject(Value* Size, size_t Alignment, Instruction* InsertBefore) {
    Instruction* Result;
    if (Alignment <= GCMinAlign) {
      Result = CallInst::Create(
        Allocate, { MyThread, Size }, "filc_allocate", InsertBefore);
    } else {
      Result = CallInst::Create(
        AllocateWithAlignment,
        { MyThread, Size, ConstantInt::get(IntPtrTy, Alignment) },
        "filc_allocate", InsertBefore);
    }
    Result->setDebugLoc(InsertBefore->getDebugLoc());
    return Result;
  }

  Value* allocate(Value* Size, size_t Alignment, Instruction* InsertBefore) {
    return flightPtrForObject(allocateObject(Size, Alignment, InsertBefore), InsertBefore);
  }

  size_t countPtrs(Type* T) {
    assert(!isa<FunctionType>(T));
    assert(!isa<TypedPointerType>(T));
    assert(T != FlightPtrTy);
    assert(!isa<ScalableVectorType>(T));

    if (isa<PointerType>(T))
      return 1;

    if (StructType* ST = dyn_cast<StructType>(T)) {
      size_t Result = 0;
      for (unsigned Index = ST->getNumElements(); Index--;)
        Result += countPtrs(ST->getElementType(Index));
      return Result;
    }

    if (ArrayType* AT = dyn_cast<ArrayType>(T))
      return AT->getNumElements() * countPtrs(AT->getElementType());

    if (FixedVectorType* VT = dyn_cast<FixedVectorType>(T))
      return VT->getElementCount().getFixedValue() * countPtrs(VT->getElementType());

    assert(!hasPtrs(T));
    return 0;
  }

  void computeFrameIndexMap(const std::vector<BasicBlock*>& Blocks) {
    FrameIndexMap.clear();
    FrameSize = NumSpecialFrameObjects;
    Setjmps.clear();
    UsesVastartOrZargs = false;

    assert(!Blocks.empty());

    auto LiveCast = [&] (Value* V) -> Value* {
      if (isa<Instruction>(V))
        return V;
      if (!isa<Argument>(V) && !isa<Constant>(V) && !isa<MetadataAsValue>(V) && !isa<InlineAsm>(V)
          && !isa<BasicBlock>(V)) {
        errs() << "V = " << *V << "\n";
        assert(isa<Constant>(V) || isa<MetadataAsValue>(V) || isa<InlineAsm>(V) || isa<BasicBlock>(V));
      }
      return nullptr;
    };

    std::unordered_map<BasicBlock*, std::unordered_set<Value*>> LiveAtTail;
    bool Changed = true;
    while (Changed) {
      Changed = false;

      for (size_t BlockIndex = Blocks.size(); BlockIndex--;) {
        BasicBlock* BB = Blocks[BlockIndex];
        std::unordered_set<Value*> Live = LiveAtTail[BB];

        for (auto It = BB->rbegin(); It != BB->rend(); ++It) {
          Instruction* I = &*It;
          if (verbose)
            errs() << "Liveness dealing with: " << *I << "\n";

          Live.erase(I);
          
          if (PHINode* Phi = dyn_cast<PHINode>(I)) {
            for (unsigned Index = Phi->getNumIncomingValues(); Index--;) {
              Value* V = Phi->getIncomingValue(Index);
              if (Value* LV = LiveCast(V)) {
                BasicBlock* PBB = Phi->getIncomingBlock(Index);
                Changed |= LiveAtTail[PBB].insert(LV).second;
              }
            }
            continue;
          }

          for (Value* V : I->operand_values()) {
            if (Value* LV = LiveCast(V))
              Live.insert(LV);
          }
        }

        // NOTE: We might be given IR with unreachable blocks. Those will have live-at-head. Like,
        // whatever. But if it's the root block and it has live-at-head then what the fugc.
        if (!BlockIndex) {
          for (Value* V : Live) {
            if (!isa<Argument>(V))
              errs() << "Unexpected live: " << *V << "\n";
            assert(isa<Argument>(V));
          }
        }

        for (BasicBlock* PBB : predecessors(BB)) {
          for (Value* LV : Live)
            Changed |= LiveAtTail[PBB].insert(LV).second;
        }
      }
    }

    std::unordered_map<ValuePtr, std::unordered_set<ValuePtr>> Interference;

    for (size_t BlockIndex = Blocks.size(); BlockIndex--;) {
      BasicBlock* BB = Blocks[BlockIndex];
      std::unordered_set<Value*> Live = LiveAtTail[BB];
      
      for (auto It = BB->rbegin(); It != BB->rend(); ++It) {
        Instruction* I = &*It;
        
        Live.erase(I);

        size_t NumIPtrs = countPtrs(I->getType());
        if (NumIPtrs) {
          for (Value* LV : Live) {
            size_t NumVIPtrs = countPtrs(LV->getType());
            for (size_t LVPtrIndex = NumVIPtrs; LVPtrIndex--;) {
              for (size_t IPtrIndex = NumIPtrs; IPtrIndex--;) {
                Interference[ValuePtr(I, IPtrIndex)].insert(ValuePtr(LV, LVPtrIndex));
                Interference[ValuePtr(LV, LVPtrIndex)].insert(ValuePtr(I, IPtrIndex));
              }
            }
          }
        }

        for (Value* V : I->operand_values()) {
          if (Value* LV = LiveCast(V))
            Live.insert(LV);
        }
      }
    }

    // The arguments interfere with one another.
    for (Argument& A1 : OldF->args()) {
      size_t NumA1Ptrs = countPtrs(A1.getType());
      if (!NumA1Ptrs)
        continue;
      for (Argument& A2 : OldF->args()) {
        size_t NumA2Ptrs = countPtrs(A2.getType());
        for (size_t A2PtrIndex = NumA2Ptrs; A2PtrIndex--;) {
          for (size_t A1PtrIndex = NumA1Ptrs; A1PtrIndex--;)
            Interference[ValuePtr(&A1, A1PtrIndex)].insert(ValuePtr(&A2, A2PtrIndex));
        }
      }
    }

    // Make this deterministic by having a known order in which we process stuff.
    std::vector<ValuePtr> Order;
    for (Argument& A : OldF->args()) {
      for (size_t PtrIndex = countPtrs(A.getType()); PtrIndex--;)
        Order.push_back(ValuePtr(&A, PtrIndex));
    }
    for (BasicBlock* BB : Blocks) {
      for (Instruction& I : *BB) {
        for (size_t PtrIndex = countPtrs(I.getType()); PtrIndex--;)
          Order.push_back(ValuePtr(&I, PtrIndex));
      }
    }

    for (ValuePtr IP : Order) {
      const std::unordered_set<ValuePtr>& Adjacency = Interference[IP];
      for (size_t FrameIndex = NumSpecialFrameObjects; ; FrameIndex++) {
        bool Ok = true;
        for (ValuePtr AIP : Adjacency) {
          if (FrameIndexMap.count(AIP) && FrameIndexMap[AIP] == FrameIndex) {
            Ok = false;
            break;
          }
        }
        if (Ok) {
          FrameIndexMap[IP] = FrameIndex;
          FrameSize = std::max(FrameSize, FrameIndex + 1);
          break;
        }
      }
    }

    for (BasicBlock* BB : Blocks) {
      for (Instruction& I : *BB) {
        if (CallBase* CI = dyn_cast<CallBase>(&I)) {
          if (Function* F = dyn_cast<Function>(CI->getCalledOperand())) {
            if (F->getName() == "zargs") {
              UsesVastartOrZargs = true;
              break;
            }
          }
        }
        if (IntrinsicInst* II = dyn_cast<IntrinsicInst>(&I)) {
          if (II->getIntrinsicID() == Intrinsic::vastart) {
            UsesVastartOrZargs = true;
            break;
          }
        }
      }
      if (UsesVastartOrZargs)
        break;
    }
    if (UsesVastartOrZargs)
      SnapshottedArgsFrameIndex = FrameSize++;

    for (BasicBlock* BB : Blocks) {
      for (Instruction& I : *BB) {
        if (CallBase* CI = dyn_cast<CallBase>(&I)) {
          if (Function* F = dyn_cast<Function>(CI->getCalledOperand())) {
            if (isSetjmp(F)) {
              assert(CI->hasFnAttr(Attribute::ReturnsTwice));
              assert(F->hasFnAttribute(Attribute::ReturnsTwice));
              Setjmps[CI] = FrameSize++;
            }
          }
        }
      }
    }
  }

  void recordLowerAtIndex(Value* Lower, size_t FrameIndex, Instruction* InsertBefore) {
    assert(FrameIndex < FrameSize);
    Instruction* LowersPtr = GetElementPtrInst::Create(
      FrameTy, Frame, { ConstantInt::get(Int32Ty, 0), ConstantInt::get(Int32Ty, 2) },
      "filc_frame_lowers", InsertBefore);
    LowersPtr->setDebugLoc(InsertBefore->getDebugLoc());
    Instruction* LowerPtr = GetElementPtrInst::Create(
      RawPtrTy, LowersPtr, { ConstantInt::get(IntPtrTy, FrameIndex) }, "filc_frame_lower",
      InsertBefore);
    LowerPtr->setDebugLoc(InsertBefore->getDebugLoc());
    (new StoreInst(Lower, LowerPtr, InsertBefore))->setDebugLoc(InsertBefore->getDebugLoc());
  }

  void recordLowersRecurse(
    Value* ValueKey, Type* T, Value* V, size_t& PtrIndex, Instruction* InsertBefore) {
    assert(!isa<FunctionType>(T));
    assert(!isa<TypedPointerType>(T));
    assert(!isa<ScalableVectorType>(T));
    assert(T != FlightPtrTy);

    if (T == RawPtrTy) {
      assert(FrameIndexMap.count(ValuePtr(ValueKey, PtrIndex)));
      size_t FrameIndex = FrameIndexMap[ValuePtr(ValueKey, PtrIndex)];
      assert(FrameIndex >= NumSpecialFrameObjects);
      recordLowerAtIndex(flightPtrLower(V, InsertBefore), FrameIndex, InsertBefore);
      PtrIndex++;
      return;
    }

    if (StructType* ST = dyn_cast<StructType>(T)) {
      for (unsigned Index = ST->getNumElements(); Index--;) {
        Type* InnerT = ST->getElementType(Index);
        Value* InnerV = ExtractValueInst::Create(
          toFlightType(InnerT), V, { Index }, "filc_extract_struct", InsertBefore);
        recordLowersRecurse(ValueKey, InnerT, InnerV, PtrIndex, InsertBefore);
      }
      return;
    }
      
    if (ArrayType* AT = dyn_cast<ArrayType>(T)) {
      assert(static_cast<unsigned>(AT->getNumElements()) == AT->getNumElements());
      for (unsigned Index = static_cast<unsigned>(AT->getNumElements()); Index--;) {
        Value* InnerV = ExtractValueInst::Create(
          toFlightType(AT->getElementType()), V, { Index }, "filc_extract_array", InsertBefore);
        recordLowersRecurse(ValueKey, AT->getElementType(), InnerV, PtrIndex, InsertBefore);
      }
      return;
    }
      
    if (FixedVectorType* VT = dyn_cast<FixedVectorType>(T)) {
      for (unsigned Index = VT->getElementCount().getFixedValue(); Index--;) {
        Value* InnerV = ExtractElementInst::Create(
          V, ConstantInt::get(IntPtrTy, Index), "filc_extract_vector", InsertBefore);
        recordLowersRecurse(ValueKey, VT->getElementType(), InnerV, PtrIndex, InsertBefore);
      }
      return;
    }

    assert(!hasPtrs(T));
  }

  void recordLowers(Value* ValueKey, Type* T, Value* V, Instruction* InsertBefore) {
    if (verbose) {
      errs() << "Recording objects for " << *ValueKey << ", T = " << *T << ", V = " << *V
             << "\n";
    }
    size_t PtrIndex = 0;
    recordLowersRecurse(ValueKey, T, V, PtrIndex, InsertBefore);
    assert(PtrIndex == countPtrs(ValueKey->getType()));
  }

  void recordLowers(Value* V, Instruction* InsertBefore) {
    recordLowers(V, V->getType(), V, InsertBefore);
  }

  Constant* optimizedAccessCheckOrigin(
    int64_t Size, int64_t Alignment, int64_t AlignmentOffset, bool NeedsWrite, DebugLoc ScheduledDI,
    const CombinedDI* SemanticDI) {
    if (verbose) {
      errs() << "Creating access check origin with size = " << Size << ", alignment = " << Alignment
             << ", alignment offset = " << AlignmentOffset << ", needs write = " << NeedsWrite
             << "\n";
    }
    
    assert(static_cast<uint32_t>(Size) == Size);
    assert(static_cast<uint8_t>(Alignment) == Alignment);
    assert(static_cast<uint8_t>(AlignmentOffset) == AlignmentOffset);

    OptimizedAccessCheckOriginKey OACOK(
      static_cast<uint32_t>(Size), static_cast<uint8_t>(Alignment),
      static_cast<uint8_t>(AlignmentOffset), NeedsWrite, ScheduledDI, SemanticDI);
    auto Iter = OptimizedAccessCheckOrigins.find(OACOK);
    if (Iter != OptimizedAccessCheckOrigins.end())
      return Iter->second;

    size_t SemanticDILength = SemanticDI ? SemanticDI->Locations.size() : 0;
    ArrayType* AT = ArrayType::get(RawPtrTy, SemanticDILength + 1);
    StructType* ST = StructType::get(C, { Int32Ty, Int8Ty, Int8Ty, Int8Ty, RawPtrTy, AT });
    std::vector<Constant*> SemanticDICs;
    if (SemanticDI) {
      for (DILocation* DI : SemanticDI->Locations)
        SemanticDICs.push_back(getOrigin(DI));
    }
    SemanticDICs.push_back(RawNull);
    Constant* CS = ConstantStruct::get(
      ST,
      { ConstantInt::get(Int32Ty, Size), ConstantInt::get(Int8Ty, Alignment),
        ConstantInt::get(Int8Ty, AlignmentOffset),
        ConstantInt::get(Int8Ty, static_cast<int>(NeedsWrite)), getOrigin(ScheduledDI),
        ConstantArray::get(AT, SemanticDICs) });
    GlobalVariable* Result = new GlobalVariable(
      M, ST, true, GlobalVariable::PrivateLinkage, CS, "filc_optimized_access_check_origin");
    OptimizedAccessCheckOrigins[OACOK] = Result;
    return Result;
  }

  Constant* optimizedAlignmentContradictionOrigin(
    const std::vector<AlignmentAndOffset>& Alignments, DebugLoc ScheduledDI,
    const CombinedDI* SemanticDI) {
    assert(Alignments.size() >= 2);

    OptimizedAlignmentContradictionOriginKey OACOK(Alignments, ScheduledDI, SemanticDI);
    auto Iter = OptimizedAlignmentContradictionOrigins.find(OACOK);
    if (Iter != OptimizedAlignmentContradictionOrigins.end())
      return Iter->second;

    std::vector<Constant*> AlignmentConsts;
    for (AlignmentAndOffset Alignment : Alignments) {
      AlignmentConsts.push_back(
        ConstantStruct::get(
          AlignmentAndOffsetTy,
          { ConstantInt::get(Int8Ty, Alignment.Alignment),
            ConstantInt::get(Int8Ty, Alignment.AlignmentOffset) }));
    }
    AlignmentConsts.push_back(
      ConstantStruct::get(
        AlignmentAndOffsetTy, { ConstantInt::get(Int8Ty, 0), ConstantInt::get(Int8Ty, 0) }));
    ArrayType* AT = ArrayType::get(AlignmentAndOffsetTy, AlignmentConsts.size());
    GlobalVariable* AlignmentsG = new GlobalVariable(
      M, AT, true, GlobalVariable::PrivateLinkage, ConstantArray::get(AT, AlignmentConsts),
      "filc_alignments_and_offsets");

    size_t SemanticDILength = SemanticDI ? SemanticDI->Locations.size() : 0;
    AT = ArrayType::get(RawPtrTy, SemanticDILength + 1);
    StructType* ST = StructType::get(C, { RawPtrTy, RawPtrTy, AT });
    std::vector<Constant*> SemanticDICs;
    if (SemanticDI) {
      for (DILocation* DI : SemanticDI->Locations)
        SemanticDICs.push_back(getOrigin(DI));
    }
    SemanticDICs.push_back(RawNull);
    Constant* CS = ConstantStruct::get(
      ST, { AlignmentsG, getOrigin(ScheduledDI), ConstantArray::get(AT, SemanticDICs) });
    GlobalVariable* Result = new GlobalVariable(
      M, ST, true, GlobalVariable::PrivateLinkage, CS,
      "filc_optimized_alignment_contradiction_origin");
    OptimizedAlignmentContradictionOrigins[OACOK] = Result;
    return Result;
  }

  template<typename CheckT>
  void checkCanonicalizedAccessChecks(const std::vector<CheckT>& Checks) {
    for (size_t Index = 0; Index < Checks.size();) {
      Value* CanonicalPtr = Checks[Index].CanonicalPtr;

      auto failAssert = [&] (const char* File, int Line, const char* Expression) {
        errs() << File << ":" << Line << ": assertion " << Expression << " failed for checks on "
               << CanonicalPtr << ":" << CanonicalPtr->getName() << ":\n"
               << "    " << Checks << "\n";
        llvm_unreachable("checkCanonicalizedAccessChecks found an issue with checks!");
      };

#define acAssert(exp) do { \
        if (!(exp)) \
          failAssert(__FILE__, __LINE__, #exp); \
      } while (false)
    
      int64_t Alignment = 0;
      int64_t AlignmentOffset = 0;
      bool HasLowerBound = false;
      bool HasUpperBound = false;
      
      size_t BeginIndex = Index;
      size_t EndIndex;
      for (EndIndex = BeginIndex;
           EndIndex < Checks.size() && Checks[EndIndex].CanonicalPtr == CanonicalPtr;
           ++EndIndex) {
        CheckT AC = Checks[EndIndex];
        switch (AC.CK) {
        case CheckKind::Alignment:
        case CheckKind::KnownAlignment:
          if (Alignment)
            acAssert((AlignmentOffset % AC.Size) != AC.Offset);
          else {
            Alignment = AC.Size;
            AlignmentOffset = AC.Offset;
            acAssert(AlignmentOffset >= 0);
            acAssert(AlignmentOffset < Alignment);
          }
          break;
        case CheckKind::KnownLowerBound:
        case CheckKind::LowerBound:
          HasLowerBound = true;
          break;
        case CheckKind::UpperBound:
          HasUpperBound = true;
          break;
        default:
          break;
        }
      }

      if (HasUpperBound)
        acAssert(HasLowerBound);

      Index = EndIndex;
    }
  }

  AllocaInst* canonicalPtrAuxBaseVar(Value* P) {
    auto Iter = CanonicalPtrAuxBaseVars.find(P);
    if (Iter != CanonicalPtrAuxBaseVars.end())
      return Iter->second;

    Instruction* InsertBefore = &NewF->getEntryBlock().front();
    AllocaInst* Result = new AllocaInst(RawPtrTy, 0, nullptr, "filc_aux_base_var", InsertBefore);
    new StoreInst(RawNull, Result, InsertBefore);
    
    CanonicalPtrAuxBaseVars[P] = Result;
    return Result;
  }

  void storeOrigin(Value* Origin, Instruction* InsertBefore) {
    Instruction* OriginPtr = GetElementPtrInst::Create(
      FrameTy, Frame, { ConstantInt::get(Int32Ty, 0), ConstantInt::get(Int32Ty, 1) },
      "filc_frame_origin_ptr", InsertBefore);
    OriginPtr->setDebugLoc(InsertBefore->getDebugLoc());
    (new StoreInst(Origin, OriginPtr, InsertBefore))->setDebugLoc(InsertBefore->getDebugLoc());
  }

  AuxBaseAndPtr auxPtrForOperand(Value* P, Instruction* I, unsigned OperandIdx,
                                 Instruction* InsertBefore) {
    assert(AuxBaseVarOperands.count(I));
    assert(OperandIdx < AuxBaseVarOperands[I].size());
    AllocaInst* AuxBaseVar = AuxBaseVarOperands[I][OperandIdx];
    assert(AuxBaseVar);
    if (verbose) {
      errs() << "For " << *I << " and operand index " << OperandIdx << " got AuxBaseVar = "
             << *AuxBaseVar << "\n";
    }
    Instruction* AuxBaseP = new LoadInst(RawPtrTy, AuxBaseVar, "filc_aux_base_ptr", InsertBefore);
    AuxBaseP->setDebugLoc(InsertBefore->getDebugLoc());
    Instruction* AuxP = GetElementPtrInst::Create(
      Int8Ty, AuxBaseP, flightPtrOffset(P, InsertBefore), "filc_aux_ptr", InsertBefore);
    AuxP->setDebugLoc(InsertBefore->getDebugLoc());
    return AuxBaseAndPtr(AuxBaseP, AuxP);
  }

  void emitChecks(std::vector<AccessCheckWithDI> Checks, Instruction* Inst) {
    if (verbose)
      errs() << "Raw checks: " << Checks << "\n";
    canonicalizeAccessChecks(Checks);
    if (verbose)
      errs() << "Canonicalized checks: " << Checks << "\n";

    for (size_t Index = 0; Index < Checks.size();) {
      Value* CanonicalPtr = Checks[Index].CanonicalPtr;
      Value* FlightPtr = lowerConstantValue(CanonicalPtr, Inst, RawNull);

      auto ptrWithOffset = [&] (int64_t Offset, Instruction* InsertBefore) {
        assert((int32_t)Offset == Offset);
        assert(FlightPtr);
        
        if (!useAsmForOffsets)
          return flightPtrWithOffset(FlightPtr, ConstantInt::get(IntPtrTy, Offset), InsertBefore);
        
        Value* FlightP = flightPtrPtr(FlightPtr, InsertBefore);
        std::ostringstream buf;
        buf << "leaq " << Offset << "($1), $0";
        FunctionCallee Asm = InlineAsm::get(
          FunctionType::get(RawPtrTy, { RawPtrTy }, false),
          buf.str(), "=r,r,~{dirflag},~{fpsr},~{flags}",
          /*hasSideEffects=*/false);
        Instruction* OffsetP = CallInst::Create(Asm, { FlightP }, "filc_lea", InsertBefore);
        OffsetP->setDebugLoc(Inst->getDebugLoc());
        return flightPtrWithPtr(FlightPtr, OffsetP, InsertBefore);
      };
      
      int64_t Alignment = 0;
      int64_t AlignmentOffset = 0;
      bool AlignmentContradiction = false;
      bool NeedsWritable = false;
      int64_t LowerBoundOffset = 0;
      bool HasLowerBound = false;
      int64_t UpperBoundOffset = 0;
      bool HasUpperBound = false;
      bool HasRangeCheck = false;
      const CombinedDI* RangeDI = nullptr;
      bool HasFreeCheck = false;

      size_t BeginIndex = Index;
      size_t EndIndex;
      for (EndIndex = BeginIndex;
           EndIndex < Checks.size() && Checks[EndIndex].CanonicalPtr == CanonicalPtr;
           ++EndIndex) {
        AccessCheckWithDI AC = Checks[EndIndex];
        switch (AC.CK) {
        case CheckKind::ValidObject:
          HasRangeCheck = true;
          RangeDI = combineDI(RangeDI, AC.DI);
          break;
        case CheckKind::Alignment:
        case CheckKind::KnownAlignment:
          if (Alignment)
            AlignmentContradiction = true;
          Alignment = AC.Size;
          AlignmentOffset = AC.Offset;
          HasRangeCheck = true;
          RangeDI = combineDI(RangeDI, AC.DI);
          break;
        case CheckKind::CanWrite:
          NeedsWritable = true;
          HasRangeCheck = true;
          RangeDI = combineDI(RangeDI, AC.DI);
          break;
        case CheckKind::KnownLowerBound:
        case CheckKind::LowerBound:
          LowerBoundOffset = AC.Offset;
          HasLowerBound = true;
          HasRangeCheck = true;
          RangeDI = combineDI(RangeDI, AC.DI);
          break;
        case CheckKind::UpperBound:
          UpperBoundOffset = AC.Offset;
          HasUpperBound = true;
          HasRangeCheck = true;
          RangeDI = combineDI(RangeDI, AC.DI);
          break;
        case CheckKind::NotFree:
          HasRangeCheck = true;
          HasFreeCheck = true;
          RangeDI = combineDI(RangeDI, AC.DI);
          break;
        case CheckKind::GetAuxPtr:
        case CheckKind::EnsureAuxPtr:
          break;
        }
      }
      assert(EndIndex > BeginIndex);
      assert(EndIndex <= Checks.size());

      Index = EndIndex;

      if (AlignmentContradiction) {
        std::vector<AlignmentAndOffset> Alignments;
        for (size_t SubIndex = BeginIndex; SubIndex < EndIndex; ++SubIndex) {
          AccessCheckWithDI AC = Checks[SubIndex];
          switch (AC.CK) {
          case CheckKind::Alignment:
          case CheckKind::KnownAlignment:
            assert(static_cast<uint8_t>(AC.Offset) == AC.Offset);
            assert(static_cast<uint8_t>(AC.Size) == AC.Size);
            Alignments.push_back(AlignmentAndOffset(AC.Size, AC.Offset));
            break;

          default:
            break;
          }
        }

        CallInst::Create(
          OptimizedAlignmentContradiction, {
            FlightPtr,
            optimizedAlignmentContradictionOrigin(Alignments, Inst->getDebugLoc(), RangeDI) }, "",
          Inst)->setDebugLoc(Inst->getDebugLoc());
        continue;
      }

      if (!Alignment) {
        assert(!AlignmentOffset);
        Alignment = 1;
      }
      assert(Alignment == 1 || Alignment == 2 || Alignment == 4 || Alignment == 8);
      assert(AlignmentOffset < Alignment);

      if (HasUpperBound) {
        // It's not clear if we're actually doing this right.
        //
        // Forward propagation by itself does the right thing: if it eliminates a lower bound check,
        // it replaces it with KnownLowerBound. That's fine.
        //
        // But does backward propagation do the right thing? In backward propagation, we convert the
        // known lower bound checks to just plain lower bound checks. Do they then survive correctly?
        //
        // I guess we'll find out!
        assert(HasLowerBound);
        assert(UpperBoundOffset > LowerBoundOffset);
      }

      BasicBlock* RangeFailB = nullptr;
      if (HasRangeCheck) {
        RangeFailB = BasicBlock::Create(C, "filc_range_fail_block", NewF);
        Instruction* RangeFailTerm = new UnreachableInst(C, RangeFailB);
        CallInst::Create(
          OptimizedAccessCheckFail,
          { ptrWithOffset(LowerBoundOffset, RangeFailTerm),
            optimizedAccessCheckOrigin(
              HasUpperBound ? UpperBoundOffset - LowerBoundOffset : 0, Alignment,
              PositiveModulo(AlignmentOffset - LowerBoundOffset, Alignment),
              NeedsWritable, Inst->getDebugLoc(), RangeDI) },
          "", RangeFailTerm)->setDebugLoc(Inst->getDebugLoc());
      }

      for (size_t SubIndex = BeginIndex; SubIndex < EndIndex; ++SubIndex) {
        AccessCheckWithDI AC = Checks[SubIndex];
        switch (AC.CK) {
        case CheckKind::ValidObject: {
          ICmpInst* NullObject = new ICmpInst(
            Inst, ICmpInst::ICMP_EQ, flightPtrLower(FlightPtr, Inst), RawNull,
            "filc_null_access_object");
          NullObject->setDebugLoc(Inst->getDebugLoc());
          SplitBlockAndInsertIfThen(
            expectFalse(NullObject, Inst), Inst, false, nullptr, nullptr, nullptr, RangeFailB);
          break;
        }

        case CheckKind::KnownAlignment:
          break;

        case CheckKind::Alignment: {
          assert(AC.Size == 1 || AC.Size == 2 || AC.Size == 4 || AC.Size == 8 || AC.Size == 16);
          if (AC.Size == 1)
            break;
          Instruction* PtrInt = new PtrToIntInst(
            flightPtrPtr(ptrWithOffset(AC.Offset, Inst), Inst), IntPtrTy, "filc_ptr_as_int", Inst);
          PtrInt->setDebugLoc(Inst->getDebugLoc());
          Instruction* Masked = BinaryOperator::Create(
            Instruction::And, PtrInt, ConstantInt::get(IntPtrTy, AC.Size - 1),
            "filc_ptr_alignment_masked", Inst);
          Masked->setDebugLoc(Inst->getDebugLoc());
          ICmpInst* IsAligned = new ICmpInst(
            Inst, ICmpInst::ICMP_EQ, Masked, ConstantInt::get(IntPtrTy, 0),
            "filc_ptr_is_aligned");
          IsAligned->setDebugLoc(Inst->getDebugLoc());
          SplitBlockAndInsertIfElse(
            expectTrue(IsAligned, Inst), Inst, false, nullptr, nullptr, nullptr, RangeFailB);
          break;
        }

        case CheckKind::CanWrite: {
          assert(NeedsWritable);
          BinaryOperator* Masked = BinaryOperator::Create(
            Instruction::And, flagsForLower(flightPtrLower(FlightPtr, Inst), Inst),
            ConstantInt::get(IntPtrTy, ObjectFlagReadonly | (HasFreeCheck ? ObjectFlagFree : 0)),
            "filc_flags_masked", Inst);
          Masked->setDebugLoc(Inst->getDebugLoc());
          ICmpInst* IsNotReadOnly = new ICmpInst(
            Inst, ICmpInst::ICMP_EQ, Masked, ConstantInt::get(IntPtrTy, 0),
            "filc_object_is_not_read_only");
          IsNotReadOnly->setDebugLoc(Inst->getDebugLoc());
          SplitBlockAndInsertIfElse(
            expectTrue(IsNotReadOnly, Inst), Inst, false, nullptr, nullptr, nullptr, RangeFailB);
          break;
        }

        case CheckKind::NotFree: {
          assert(HasFreeCheck);
          if ((HasLowerBound && HasUpperBound) || NeedsWritable)
            break;
          BinaryOperator* Masked = BinaryOperator::Create(
            Instruction::And, flagsForLower(flightPtrLower(FlightPtr, Inst), Inst),
            ConstantInt::get(IntPtrTy, ObjectFlagFree), "filc_flags_masked", Inst);
          Masked->setDebugLoc(Inst->getDebugLoc());
          ICmpInst* IsNotFree = new ICmpInst(
            Inst, ICmpInst::ICMP_EQ, Masked, ConstantInt::get(IntPtrTy, 0),
            "filc_object_is_not_free");
          IsNotFree->setDebugLoc(Inst->getDebugLoc());
          SplitBlockAndInsertIfElse(
            expectTrue(IsNotFree, Inst), Inst, false, nullptr, nullptr, nullptr, RangeFailB);
          break;
        }
          
        case CheckKind::UpperBound: {
          assert(HasLowerBound);
          assert(HasUpperBound);
          assert(UpperBoundOffset > LowerBoundOffset);
          Value* Upper = upperForLower(flightPtrLower(FlightPtr, Inst), Inst);
          Value* Ptr = flightPtrPtr(ptrWithOffset(LowerBoundOffset, Inst), Inst);
          Instruction* IsBelowUpper;
          if (UpperBoundOffset - LowerBoundOffset == Alignment
              && !AlignmentContradiction
              // It's possible for the distance between upper bound and lower bound to be 16, but
              // the KnownAlignment to be 16, but we're actually checking a 16-sized range of bytes
              // not aligned to 16 (that straddle two 16 byte words).
              && PositiveModulo(UpperBoundOffset, Alignment) == AlignmentOffset) {
            assert(PositiveModulo(LowerBoundOffset, Alignment) == AlignmentOffset);
            IsBelowUpper = new ICmpInst(
              Inst, ICmpInst::ICMP_ULT, Ptr, Upper, "filc_ptr_below_upper");
          } else {
            Instruction* UpperMinus = GetElementPtrInst::Create(
              Int8Ty, Upper, { ConstantInt::get(IntPtrTy, LowerBoundOffset - UpperBoundOffset) },
              "filc_upper_minus", Inst);
            UpperMinus->setDebugLoc(Inst->getDebugLoc());
            IsBelowUpper = new ICmpInst(
              Inst, ICmpInst::ICMP_ULE, Ptr, UpperMinus, "filc_ptr_below_equal_upper");
          }
          IsBelowUpper->setDebugLoc(Inst->getDebugLoc());
          SplitBlockAndInsertIfElse(
            expectTrue(IsBelowUpper, Inst), Inst, false, nullptr, nullptr, nullptr, RangeFailB);
          break;
        }

        case CheckKind::KnownLowerBound:
          assert(HasLowerBound);
          break;

        case CheckKind::LowerBound: {
          assert(HasLowerBound);
          Instruction* IsBelowLower = new ICmpInst(
            Inst, ICmpInst::ICMP_ULT, flightPtrPtr(ptrWithOffset(LowerBoundOffset, Inst), Inst),
            flightPtrLower(FlightPtr, Inst), "filc_ptr_below_lower");
          IsBelowLower->setDebugLoc(Inst->getDebugLoc());
          SplitBlockAndInsertIfThen(
            expectFalse(IsBelowLower, Inst), Inst, false, nullptr, nullptr, nullptr, RangeFailB);
          break;
        }

        case CheckKind::GetAuxPtr: {
          AllocaInst* AuxBaseVar = canonicalPtrAuxBaseVar(CanonicalPtr);
          (new StoreInst(auxPtrForLower(flightPtrLower(FlightPtr, Inst), Inst), AuxBaseVar,
                         Inst))->setDebugLoc(Inst->getDebugLoc());
          break;
        }

        case CheckKind::EnsureAuxPtr: {
          AllocaInst* AuxBaseVar = canonicalPtrAuxBaseVar(CanonicalPtr);
          Instruction* AuxBase = new LoadInst(RawPtrTy, AuxBaseVar, "filc_get_aux_ptr", Inst);
          AuxBase->setDebugLoc(Inst->getDebugLoc());
          Instruction* AuxBaseIsNull = new ICmpInst(
            Inst, ICmpInst::ICMP_EQ, AuxBase, RawNull, "filc_aux_ptr_is_null");
          AuxBaseIsNull->setDebugLoc(Inst->getDebugLoc());
          Instruction* ThenTerm =
            SplitBlockAndInsertIfThen(expectFalse(AuxBaseIsNull, Inst), Inst, false);
          // FIXME: Is this the right origin? I think it is, because we aren't hoisting these.
          storeOrigin(getOrigin(Inst->getDebugLoc()), ThenTerm);
          Instruction* CreatedAuxBase = CallInst::Create(
            ObjectEnsureAuxPtrOutline,
            { MyThread, objectForLower(flightPtrLower(FlightPtr, ThenTerm), ThenTerm) },
            "filc_object_ensure_aux_ptr_outline", ThenTerm);
          CreatedAuxBase->setDebugLoc(Inst->getDebugLoc());
          (new StoreInst(CreatedAuxBase, AuxBaseVar, ThenTerm))->setDebugLoc(Inst->getDebugLoc());
          break;
        } }
      }
    }
  }

  void emitChecksForInst(Instruction* Inst) {
    if (verbose)
      errs() << "Emitting checks for " << *Inst << "\n";
    auto Iter = ChecksForInst.find(Inst);
    if (Iter == ChecksForInst.end())
      return;
    emitChecks(Iter->second, Inst);
  }

  void buildCheck(int64_t Size, int64_t Alignment, Value* CanonicalPtr, int64_t Offset,
                  AccessKind AK, const CombinedDI* DI, std::vector<AccessCheckWithDI>& Checks) {
    if (verbose) {
      errs() << "Building check: Size = " << Size << ", Alignment = " << Alignment
             << ", CanonicalPtr = " << CanonicalPtr->getName() << ", Offset = " << Offset
             << ", AK = " << accessKindString(AK) << ", DI = " << DI << "\n";
    }
    assert(Size);
    assert(Alignment);
    assert(!((Alignment - 1) & Alignment));
    assert((int32_t)Offset == Offset);
    Alignment = std::min(Alignment, static_cast<int64_t>(WordSize));
    Checks.push_back(
      AccessCheckWithDI(CanonicalPtr, 0, 0, CheckKind::ValidObject, DI));
    Checks.push_back(
      AccessCheckWithDI(CanonicalPtr, PositiveModulo(Offset, Alignment), Alignment,
                        CheckKind::Alignment, DI));
    if (AK == AccessKind::Write) {
      Checks.push_back(
        AccessCheckWithDI(CanonicalPtr, 0, 0, CheckKind::CanWrite, DI));
    }
    Checks.push_back(
      AccessCheckWithDI(CanonicalPtr, Offset, 0, CheckKind::LowerBound, DI));
    assert(Offset + Size > Offset);
    Checks.push_back(
      AccessCheckWithDI(CanonicalPtr, Offset + Size, 0, CheckKind::UpperBound, DI));
    Checks.push_back(
      AccessCheckWithDI(CanonicalPtr, 0, 0, CheckKind::NotFree, DI));
  }

  bool needToCheckAlignment(Type* T) {
    // FIXME: This is very X86-specific.
    
    assert(T != FlightPtrTy);

    if (isa<FunctionType>(T))
      return false;

    if (isa<TypedPointerType>(T)) {
      llvm_unreachable("Shouldn't ever see typed pointers");
      return false;
    }

    if (isa<PointerType>(T))
      return true;

    if (T->isIntegerTy())
      return false;

    if (T->isFloatTy() || T->isDoubleTy())
      return false;

    if (StructType* ST = dyn_cast<StructType>(T)) {
      assert(!ST->isOpaque());
      for (Type* InnerT : ST->elements()) {
        if (needToCheckAlignment(InnerT))
          return true;
      }
      return false;
    }

    if (ArrayType* AT = dyn_cast<ArrayType>(T))
      return needToCheckAlignment(AT->getElementType());

    return true;
  }

  void buildChecksRecurse(Type* T, Value* HighP, int64_t Offset, size_t Alignment, AtomicOrdering AO,
                          AccessKind AK, const CombinedDI* DI,
                          std::vector<AccessCheckWithDI>& Checks) {
    if (!hasPtrs(T)) {
      size_t CheckedAlignment;
      if (needToCheckAlignment(T) || AO != AtomicOrdering::NotAtomic)
        CheckedAlignment = MinAlign(DL.getABITypeAlign(T).value(), Alignment);
      else
        CheckedAlignment = 1;
      buildCheck(DL.getTypeStoreSize(T), CheckedAlignment, HighP, Offset, AK, DI, Checks);
      return;
    }
    
    if (isa<FunctionType>(T)) {
      llvm_unreachable("shouldn't see function types in buildChecksRecurse");
      return;
    }

    if (isa<TypedPointerType>(T)) {
      llvm_unreachable("Shouldn't ever see typed pointers");
      return;
    }

    assert(T != FlightPtrTy);

    if (T == RawPtrTy) {
      buildCheck(WordSize, WordSize, HighP, Offset, AK, DI, Checks);
      Checks.push_back(AccessCheckWithDI(HighP, 0, 0, CheckKind::GetAuxPtr, DI));
      if (AK == AccessKind::Write)
        Checks.push_back(AccessCheckWithDI(HighP, 0, 0, CheckKind::EnsureAuxPtr, DI));
      return;
    }

    assert(!isa<PointerType>(T));

    if (StructType* ST = dyn_cast<StructType>(T)) {
      const StructLayout* SL = DL.getStructLayout(ST);
      for (unsigned Index = ST->getNumElements(); Index--;) {
        Type* InnerT = ST->getElementType(Index);
        buildChecksRecurse(
          InnerT, HighP, Offset + SL->getElementOffset(Index), Alignment, AO, AK, DI, Checks);
      }
      return;
    }
      
    if (ArrayType* AT = dyn_cast<ArrayType>(T)) {
      Type* ET = AT->getElementType();
      size_t ESize = DL.getTypeAllocSize(ET);
      for (int64_t Index = AT->getNumElements(); Index--;)
        buildChecksRecurse(ET, HighP, Offset + Index * ESize, Alignment, AO, AK, DI, Checks);
      return;
    }
      
    if (FixedVectorType* VT = dyn_cast<FixedVectorType>(T)) {
      Type* ET = VT->getElementType();
      size_t ESize = DL.getTypeAllocSize(ET);
      for (int64_t Index = VT->getElementCount().getFixedValue(); Index--;)
        buildChecksRecurse(ET, HighP, Offset + Index * ESize, Alignment, AO, AK, DI, Checks);
      return;
    }

    if (isa<ScalableVectorType>(T)) {
      llvm_unreachable("Shouldn't see scalable vector types in buildChecksRecurse");
      return;
    }

    llvm_unreachable("Should not get here.");
  }

  PtrAndOffset canonicalizePtr(Value* HighP) {
    Value* OriginalHighP = HighP;
    int64_t Offset = 0;

    if (GetElementPtrInst* GEP = dyn_cast<GetElementPtrInst>(HighP)) {
      APInt OffsetAP(64, 0, false);
      if (GEP->accumulateConstantOffset(DLBefore, OffsetAP)) {
        Offset += OffsetAP.getZExtValue();
        HighP = GEP->getPointerOperand();
      }
    }

    if (ConstantExpr* CE = dyn_cast<ConstantExpr>(HighP)) {
      if (CE->getOpcode() == Instruction::GetElementPtr) {
        GetElementPtrInst* GEP = cast<GetElementPtrInst>(CE->getAsInstruction());
        APInt OffsetAP(64, 0, false);
        if (GEP->accumulateConstantOffset(DLBefore, OffsetAP)) {
          Offset += OffsetAP.getZExtValue();
          HighP = GEP->getPointerOperand();
        }
        GEP->deleteValue();
      }
    }

    // As a way of preventing any overflow shenanigans, we carry around offsets in int64's but give
    // up if the offset can't fit in a int32. This is more conservative than necessary, but also
    // likely harmless - it's not clear that we need to worry at all about "optimizations" for
    // programs that use ginormous field offsets or ginormous constant array indices.
    if ((int32_t)Offset == Offset)
      return PtrAndOffset(HighP, Offset);
    
    return PtrAndOffset(OriginalHighP, 0);
  }

  void buildChecks(Instruction* I, Type* T, Value* HighP, Align Alignment, AtomicOrdering AO,
                   AccessKind AK, std::vector<AccessCheckWithDI>& Checks) {
    if (verbose) {
      errs() << "Building checks for " << *I << " with T = " << *T << ", HighP = " << *HighP
             << ", Alignment = " << Alignment.value() << ", AK = " << accessKindString(AK) << "\n";
    }

    PtrAndOffset PAO = canonicalizePtr(HighP);
    if (verbose)
      errs() << "PAO = " << *PAO.HighP << ", offset = " << PAO.Offset << "\n";
    buildChecksRecurse(T, PAO.HighP, PAO.Offset, Alignment.value(), AO, AK, basicDI(I->getDebugLoc()),
                       Checks);
    canonicalizeAccessChecks(Checks);
  }

  template<typename FuncT>
  void forEachCheck(Instruction* I, const FuncT& Func) {
    if (LoadInst* LI = dyn_cast<LoadInst>(I)) {
      Type* T = LI->getType();
      Value* HighP = LI->getPointerOperand();
      Func(LI, T, HighP, LI->getAlign(), LI->getOrdering(), AccessKind::Read);
      return;
    }
    
    if (StoreInst* SI = dyn_cast<StoreInst>(I)) {
      Type* T = SI->getValueOperand()->getType();
      Value* HighP = SI->getPointerOperand();
      Func(SI, T, HighP, SI->getAlign(), SI->getOrdering(), AccessKind::Write);
      return;
    }
    
    if (AtomicCmpXchgInst* AI = dyn_cast<AtomicCmpXchgInst>(I)) {
      assert(AI->getMergedOrdering() != AtomicOrdering::NotAtomic);
      Type* T = AI->getNewValOperand()->getType();
      Value* HighP = AI->getPointerOperand();
      Func(AI, T, HighP, AI->getAlign(), AI->getMergedOrdering(), AccessKind::Write);
      return;
    }
    
    if (AtomicRMWInst* AI = dyn_cast<AtomicRMWInst>(I)) {
      assert(AI->getOrdering() != AtomicOrdering::NotAtomic);
      Type* T = AI->getValOperand()->getType();
      Value* HighP = AI->getPointerOperand();
      Func(AI, T, HighP, AI->getAlign(), AI->getOrdering(), AccessKind::Write);
      return;
    }

    if (IntrinsicInst* II = dyn_cast<IntrinsicInst>(I)) {
      switch (II->getIntrinsicID()) {
      case Intrinsic::vastart:
        Func(II, RawPtrTy, II->getArgOperand(0), Align(WordSize), AtomicOrdering::NotAtomic,
             AccessKind::Write);
        return;
      case Intrinsic::vacopy:
        Func(II, RawPtrTy, II->getArgOperand(0), Align(WordSize), AtomicOrdering::NotAtomic,
             AccessKind::Write);
        Func(II, RawPtrTy, II->getArgOperand(1), Align(WordSize), AtomicOrdering::NotAtomic,
             AccessKind::Read);
        return;
      default:
        return;
      }
    }

    if (CallBase* CI = dyn_cast<CallBase>(I)) {
      if (Function* F = dyn_cast<Function>(CI->getCalledOperand())) {
        if (isSetjmp(F)) {
          Func(CI, RawPtrTy, CI->getArgOperand(0), Align(WordSize), AtomicOrdering::NotAtomic,
               AccessKind::Write);
          return;
        }
      }
    }
  }

  void buildChecks(Instruction* I, std::vector<AccessCheckWithDI>& Checks) {
    forEachCheck(I, [&] (Instruction* I, Type* T, Value* HighP, Align Alignment, AtomicOrdering AO,
                         AccessKind AK) {
      buildChecks(I, T, HighP, Alignment, AO, AK, Checks);
    });
  }

  template<typename FuncT>
  void forEachCanonicalPtrOperand(Instruction* I, const FuncT& Func) {
    forEachCheck(I, [&] (Instruction*, Type*, Value* HighP, Align, AtomicOrdering, AccessKind) {
      Func(canonicalizePtr(HighP));
    });
  }

  void captureAuxBaseVars(const std::vector<Instruction*>& Instructions) {
    for (Instruction* I : Instructions) {
      forEachCanonicalPtrOperand(I, [&] (PtrAndOffset PAO) {
        AuxBaseVarOperands[I].push_back(canonicalPtrAuxBaseVar(PAO.HighP));
      });
    }
  }

  const CombinedDI* hashConsDI(const CombinedDI& DI) {
    return &*CombinedDIs.insert(DI).first;
  }

  const CombinedDI* combineDI(const CombinedDI* A, const CombinedDI* B) {
    if (A == B)
      return A;
    if (!A)
      return B;
    if (!B)
      return A;
    const CombinedDI* First = std::min(A, B);
    const CombinedDI* Second = std::max(A, B);
    auto Iter = CombinedDIMaker.find(std::make_pair(First, Second));
    if (Iter != CombinedDIMaker.end())
      return Iter->second;
    CombinedDI NewDI;
    NewDI.Locations.insert(A->Locations.begin(), A->Locations.end());
    NewDI.Locations.insert(B->Locations.begin(), B->Locations.end());
    const CombinedDI* Result = hashConsDI(NewDI);
    CombinedDIMaker[std::make_pair(First, Second)] = Result;
    return Result;
  }

  const CombinedDI* basicDI(DILocation* Loc) {
    auto Iter = BasicDIs.find(Loc);
    if (Iter != BasicDIs.end())
      return Iter->second;
    const CombinedDI* Result = hashConsDI(CombinedDI(Loc));
    BasicDIs[Loc] = Result;
    return Result;
  }

  // Notes about backwards propagation.
  //
  // Some examples of how this works. Here's an example where we haven't split critical edges. Note
  // that this analysis is sound if we don't, but in fact we do (see prepare()). However, it's
  // possible for some edges to be unsplittable in LLVM IR, and we design this pass so that it works
  // soundly (albeit more conservatively) if we ever choose not to split some edges as a matter of
  // policy.
  //
  //     a:
  //         abody
  //         br c
  //     b:
  //         bbody
  //         br c, d
  //     c:
  //         cbody
  //         br e
  //     d:
  //         dbody
  //         br e
  //
  // Say that `cbody` has a check that doesn't appear anywhere else. When propagating it backwards
  // to predecessors (a and b), we merge all of the predecessors attail states together with c'd
  // athead state. This will remove the check, since d didn't have the check.
  //
  // Now consider this criss-cross loop-that-isn't monster.
  //
  //     a:
  //         abody
  //         br c, d
  //     b:
  //         bbody
  //         br c, d
  //     c:
  //         check1
  //         cbody
  //         br c, d, e, f
  //     d:
  //         check2
  //         dbody
  //         br c, d, e, f
  //     e:
  //         ebody
  //         br somewhere
  //     f:
  //         fbody
  //         br somewhere
  //
  // There are three backedges here, c->c, d->d, and d->c (assuming c came before d in the
  // traversal). Also, critical edges aren't split. The backedges mean that we add bonus edges:
  // c->exit, d->exit. The exit block has no checks. This means that check1 and check2 can't get
  // propagated, because both c and d have c and d as predecessors, and c and d have attails that
  // will be forced empty by the exit block.
  //
  // Note that if we did split critical edges here, then check1 and check2 would experience some
  // backward motion.
  //
  // Now consider what happens in a simple loop where we had split critical edges.
  //
  //         header
  //         br loop
  //     reloop:
  //         br loop
  //     loop:
  //         check
  //         effect
  //         br reloop, end
  //     end:
  //         footer
  //
  // The reloop block exists because of critical edge splitting. Note that reloop->loop is a
  // backedge, so we will add a bonus edge reloop->exit. This forces reloop's attail state to be
  // empty and prevents the check from being hoisted. Also, the footer lacks the check anyway, so
  // the loop's attail state will be forced empty, too.
  //
  // FIXME: Looks like we need to run the forward analysis before the backward analysis! Also, it's
  // not obvious that pushing checks backward in a way that duplicates them is a good idea; it seems
  // like it'll cause code bloat. Maybe we should avoid pushing them back at all unless it leads to
  // a reduction of work.
  //
  // Let's consider what'll happen in the simple loop if we do forward propagation first. The
  // forward propagation will put the check in the attail of loop and reloop (minus the NotFree
  // part of the check). Then, I guess, we say that the forward attail overrides the backward
  // attail, or that the backward attail of a block always includes everything from the forward
  // attail (the forward part cannot be removed, somehow). So when we backprop the check from loop,
  // the merge of predecessor attails will include the check, so the check will end up in the
  // header. But, concretely, what does the equation look like?
  //
  // Say that forward propagation produces ForwardChecksAtTail and backward propagation is solving
  // for BackwardChecksAtTail. We need to answer three questions: how does the backwards execution
  // of a block start, how does the backwards execution of a block end, and how do checks get
  // scheduled?
  //
  // How to start backward block execution: the running Checks are bootstrapped from
  // BackwardChecksAtTail - ForwardChecksAtTail.
  //
  // How to end backward block execution: for each predecessor, compute the combined ChecksAtTail by
  // adding BackwardChecksAtTail to ForwardChecksAtTail. Merge the running Checks with each
  // ChecksAtTail. Then merge the running Checks into each predecessor's BackwardChecksAtTail.
  //
  // How checks are scheduled (assuming we ended backward execution in a block): for each
  // predecessor, compute the combined ChecksAtTail by adding BackwardChecksAtTail to
  // ForwardChecksAtTail. Save the running Checks as ExpectedChecks. Merge the running Checks with
  // each ChecksAtTail. Now, ExpectedChecks - Checks is the set of checks that needs to be executed
  // at the block's head.
  //
  // Alternative formulations:
  //
  // - Could alternatively say that backwards execution starts with BackwardChecksAtTail, without
  //   the subtraction. Note that in the loop case, the loop and reloop blocks will both have empty
  //   BackwardChecksAtTail anyway. But, I don't think that's right in general, since we want to
  //   backwards propagate the minimal set of checks on the grounds that we want a minimal schedule.
  //
  // - Could have end of backward execution subtract ForwardChecksAtTail from BackwardChecksAtTail
  //   after the merging of Checks into BackwardChecksAtTail.

  void updateLastDI(AccessCheck&, const AccessCheck&) {
  }

  void updateLastDI(AccessCheckWithDI& LastACRef, const AccessCheckWithDI& NewAC) {
    if (LastACRef == NewAC)
      LastACRef.DI = NewAC.DI;
  }

  // Produces a minimal set of access checks in an order that is suitable for executing them. Assumes
  // that given two equal checks, the DI of the latest one wins. This has no effect on forward prop,
  // since forward prop uses AccessCheck, not AccessCheckWithDI. For backward prop, it means that the
  // checks that dominate win the DI battle.
  template<typename CheckT>
  void canonicalizeAccessChecks(std::vector<CheckT>& Checks) {
    std::stable_sort(Checks.begin(), Checks.end());
  
    size_t DstIndex = 0;
    size_t SrcIndex = 0;
    CheckT LastAC;
    Value* CanonicalPtr = nullptr;
    int64_t Alignment = 0;
    int64_t AlignmentOffset = 0;
    bool AlignmentContradiction = false;
    while (SrcIndex < Checks.size()) {
      CheckT AC = Checks[SrcIndex++];
      if (AC.CanonicalPtr == LastAC.CanonicalPtr &&
          FundamentalCheckKind(AC.CK) == FundamentalCheckKind(LastAC.CK)) {
        switch (AC.CK) {
        case CheckKind::ValidObject:
        case CheckKind::CanWrite:
        case CheckKind::NotFree:
        case CheckKind::GetAuxPtr:
        case CheckKind::EnsureAuxPtr:
          // For these, we really only need one of these for each CanonicalPtr.
          updateLastDI(Checks[DstIndex - 1], AC);
          continue;
          
        case CheckKind::KnownLowerBound:
        case CheckKind::LowerBound:
        case CheckKind::UpperBound:
          // For these, the sort order is such that only the first one in the list matters.
          // FIXME: Need to assert that if there's an UpperBound, then we need a LowerBound.
          // And, that:
          // - The lower bound is below-equal to the type check offsets.
          // - If we have an upper bound, it's above-equal to the type check offsets.
          // And, these checks have to happen outside this switch, since this switch is for the case
          // where we have a check redundancy. Maybe that means having the check happen in some other
          // function?
          updateLastDI(Checks[DstIndex - 1], AC);
          continue;
  
        case CheckKind::KnownAlignment:
        case CheckKind::Alignment:
          assert(Alignment);
          assert(AlignmentOffset < Alignment);
          assert(AC.Size <= LastAC.Size);
          assert(AC.Size <= Alignment);
          if ((AlignmentOffset % AC.Size) == AC.Offset) {
            if (AC.Size == Alignment)
              updateLastDI(Checks[DstIndex - 1], AC);
            continue;
          }
          AlignmentContradiction = true;
          break;
        }
      }
      Checks[DstIndex++] = AC;
      LastAC = AC;
      if (AC.CanonicalPtr != CanonicalPtr) {
        CanonicalPtr = AC.CanonicalPtr;
        Alignment = 0;
        AlignmentOffset = 0;
        AlignmentContradiction = false;
      }
      if (FundamentalCheckKind(AC.CK) == CheckKind::Alignment && !AlignmentContradiction) {
        Alignment = AC.Size;
        AlignmentOffset = AC.Offset;
      }
    }
  
    Checks.resize(DstIndex);

    checkCanonicalizedAccessChecks(Checks);
  }
  
  // Given two lists of checks, produces the set of checks that is the superset of the two. This is the
  // opposite of merging.
  template<typename ToCheckT, typename FromCheckT>
  void addAccessChecks(std::vector<ToCheckT>& ToChecks,
                       const std::vector<FromCheckT>& FromChecks) {
    ToChecks.insert(ToChecks.end(), FromChecks.begin(), FromChecks.end());
    canonicalizeAccessChecks(ToChecks);
  }

  void mergeCheckDI(AccessCheck&, const AccessCheck&, const AccessCheck&) {
  }
  
  void mergeCheckDI(AccessCheckWithDI& NewAC,
                    const AccessCheckWithDI& AC1, const AccessCheckWithDI& AC2) {
    NewAC.DI = combineDI(AC1.DI, AC2.DI);
  }
  
  // Given two lists of canonicalized access checks, merges the second one into the first one. The
  // outcome may shrink ToChecks, or make some checks in ToChecks weaker. It will never grow ToChecks.
  //
  // Returns true if ToChecks is changed.
  template<typename CheckT>
  bool mergeAccessChecks(std::vector<CheckT>& ToChecks,
                         const std::vector<CheckT>& FromChecks,
                         AIDirection Direction) {
    size_t DstToIndex = 0;
    size_t SrcToIndex = 0;
    size_t FromIndex = 0;
    bool Result = false;
    Value* LastLowerBoundPtr = nullptr;
    while (SrcToIndex < ToChecks.size() && FromIndex < FromChecks.size()) {
      CheckT ToAC = ToChecks[SrcToIndex];
      CheckT FromAC = FromChecks[FromIndex];
  
      assert(!IsKnownCheckKind(ToAC.CK));
      assert(!IsKnownCheckKind(FromAC.CK));
      
      if (ToAC.CanonicalPtr == FromAC.CanonicalPtr &&
          FundamentalCheckKind(ToAC.CK) == FundamentalCheckKind(FromAC.CK)) {

        auto mergeToAC = [&] () {
          CheckT NewAC = ToAC;
          mergeCheckDI(NewAC, ToAC, FromAC);
          ToChecks[DstToIndex++] = NewAC;
          SrcToIndex++;
          FromIndex++;
        };
        
        auto mergeMaxAC = [&] () {
          CheckT NewAC = std::max(ToAC, FromAC);
          mergeCheckDI(NewAC, ToAC, FromAC);
          Result |= NewAC != ToAC;
          ToChecks[DstToIndex++] = NewAC;
          SrcToIndex++;
          FromIndex++;
        };
        
        switch (ToAC.CK) {
        case CheckKind::ValidObject:
        case CheckKind::CanWrite:
        case CheckKind::NotFree:
        case CheckKind::GetAuxPtr:
        case CheckKind::EnsureAuxPtr: {
          mergeToAC();
          continue;
        }
        case CheckKind::LowerBound:
        case CheckKind::UpperBound: {
          switch (Direction) {
          case AIDirection::Forward: {
            mergeMaxAC();
            continue;
          }
          case AIDirection::Backward: {
            // If we kept the lower bound check, then we can keep the upper bound check. We don't
            // keep either check if they're not exactly the same.
            if (ToAC.Offset != FromAC.Offset)
              break;
            if (ToAC.CK == CheckKind::LowerBound)
              LastLowerBoundPtr = ToAC.CanonicalPtr;
            else {
              assert(ToAC.CK == CheckKind::UpperBound);
              if (ToAC.CanonicalPtr != LastLowerBoundPtr)
                break;
            }
            mergeToAC();
            continue;
          } }
          break;
        }
        case CheckKind::Alignment: {
          switch (Direction) {
          case AIDirection::Forward: {
            int64_t Size = std::min(ToAC.Size, FromAC.Size);
            if ((ToAC.Offset % Size) == (FromAC.Offset % Size)) {
              CheckT NewAC = ToAC;
              NewAC.Size = Size;
              NewAC.Offset = NewAC.Offset % Size;
              mergeCheckDI(NewAC, ToAC, FromAC);
              Result |= NewAC != ToAC;
              ToChecks[DstToIndex++] = NewAC;
              SrcToIndex++;
              FromIndex++;
              continue;
            }
            break;
          }
          case AIDirection::Backward: {
            if (ToAC.Size == FromAC.Size &&
                ToAC.Offset == FromAC.Offset) {
              mergeToAC();
              continue;
            }
            break;
          } }
          break;
        }
        case CheckKind::KnownAlignment:
        case CheckKind::KnownLowerBound:
          llvm_unreachable("Should never see Known kinds in merging");
          break;
        }
      }
  
      if (ToAC < FromAC)
        SrcToIndex++;
      else
        FromIndex++;
    }
  
    Result |= DstToIndex != ToChecks.size();
    ToChecks.resize(DstToIndex);
  
    return Result;
  }

  // Removes checks in ToChecks that are subsumed by FromChecks. Conservatively keeps ToChecks.
  template<typename ToCheckT, typename FromCheckT>
  void subtractChecks(std::vector<ToCheckT>& ToChecks,
                      const std::vector<FromCheckT>& FromChecks,
                      bool CreateKnowns = false) {
    size_t DstToIndex = 0;
    size_t SrcToIndex = 0;
    size_t FromIndex = 0;
    while (SrcToIndex < ToChecks.size() && FromIndex < FromChecks.size()) {
      ToCheckT ToAC = ToChecks[SrcToIndex];
      FromCheckT FromAC = FromChecks[FromIndex];
  
      assert(!IsKnownCheckKind(ToAC.CK));
      assert(!IsKnownCheckKind(FromAC.CK));
      
      if (ToAC.CanonicalPtr == FromAC.CanonicalPtr &&
          FundamentalCheckKind(ToAC.CK) == FundamentalCheckKind(FromAC.CK)) {
        switch (ToAC.CK) {
        case CheckKind::ValidObject:
        case CheckKind::CanWrite:
        case CheckKind::NotFree:
        case CheckKind::GetAuxPtr:
        case CheckKind::EnsureAuxPtr:
          SrcToIndex++;
          FromIndex++;
          continue;
  
        case CheckKind::LowerBound:
        case CheckKind::UpperBound:
          if (ToAC < FromAC)
            ToChecks[DstToIndex++] = ToAC;
          else if (ToAC.CK == CheckKind::LowerBound) {
            if (CreateKnowns) {
              // We can keep the ToAC LowerBound and make it Known, since there's nothing to be gained
              // from knowing that we proved a more aggressive lower bound.
              ToAC.CK = CheckKind::KnownLowerBound;
            }

            // If we're not creating knowns, then keep the lower bound as-is.
            ToChecks[DstToIndex++] = ToAC;
          }
          SrcToIndex++;
          FromIndex++;
          continue;
  
        case CheckKind::Alignment:
          if (CreateKnowns && FromAC.Size >= ToAC.Size
              && (FromAC.Offset % ToAC.Size) == ToAC.Offset) {
            FromAC.CK = CheckKind::KnownAlignment;
            ToChecks[DstToIndex++] = FromAC;
          } else
            ToChecks[DstToIndex++] = ToAC;
          SrcToIndex++;
          FromIndex++;
          continue;
          
        case CheckKind::KnownAlignment:
        case CheckKind::KnownLowerBound:
          llvm_unreachable("Should never see Known kinds in merging");
          break;
        }
      }
  
      if (ToAC < FromAC) {
        ToChecks[DstToIndex++] = ToAC;
        SrcToIndex++;
      } else
        FromIndex++;
    }

    while (SrcToIndex < ToChecks.size())
      ToChecks[DstToIndex++] = ToChecks[SrcToIndex++];
  
    ToChecks.resize(DstToIndex);
    checkCanonicalizedAccessChecks(ToChecks);
  }

  template<typename ToCheckT, typename FromCheckT>
  void subtractChecksCreatingKnowns(std::vector<ToCheckT>& ToChecks,
                                    const std::vector<FromCheckT>& FromChecks) {
    subtractChecks(ToChecks, FromChecks, /*CreateKnowns=*/true);
  }
  
  void removeRedundantChecksUsingForwardAI(
    const std::vector<BasicBlock*>& Blocks,
    const std::unordered_set<const BasicBlock*>& BackEdgePreds,
    const std::unordered_map<BasicBlock*, std::unordered_set<Value*>> CanonicalPtrLiveAtTail,
    std::unordered_map<BasicBlock*, ChecksOrBottom>& ForwardChecksAtHead,
    std::unordered_map<BasicBlock*, std::vector<AccessCheck>>& ForwardChecksAtTail) {

    ForwardChecksAtHead.clear();
    ForwardChecksAtTail.clear();

    ForwardChecksAtHead[&NewF->getEntryBlock()].Bottom = false;

    bool Changed = true;
    while (Changed) {
      Changed = false;
      for (BasicBlock* BB : Blocks) {
        if (verbose)
          errs() << "Forward propagating in " << BB->getName() << "\n";
        
        const ChecksOrBottom& COB = ForwardChecksAtHead[BB];
        if (COB.Bottom)
          continue;

        std::vector<AccessCheck> Checks = COB.Checks;
        if (verbose)
          errs() << "Checks at head: " << Checks << "\n";

        for (Instruction& I : *BB) {
          auto HandleEffects = [&] () {
            EraseIf(Checks, [&] (const AccessCheck& AC) -> bool {
              return AC.CK == CheckKind::NotFree || AC.CK == CheckKind::GetAuxPtr;
            });
          };

          if (&I == BB->getTerminator() && BackEdgePreds.count(BB)) {
            // Execute the pollcheck.
            HandleEffects();
          }

          auto Iter = ChecksForInst.find(&I);
          if (Iter != ChecksForInst.end())
            Checks.insert(Checks.end(), Iter->second.begin(), Iter->second.end());
          
          // For now, just conservatively assume that all calls may free stuff.
          if (isa<CallBase>(&I))
            HandleEffects();
          else if (isa<StoreInst>(&I) || isa<AtomicCmpXchgInst>(&I) || isa<AtomicRMWInst>(&I)) {
            EraseIf(Checks, [&] (const AccessCheck& AC) -> bool {
              return AC.CK == CheckKind::GetAuxPtr;
            });
          }

          if (verbose)
            errs() << "Checks after " << I << ":\n    " << Checks << "\n";
        }

        auto Iter = CanonicalPtrLiveAtTail.find(BB);
        assert(Iter != CanonicalPtrLiveAtTail.end());
        const std::unordered_set<Value*>& Live = Iter->second;
        EraseIf(Checks, [&] (const AccessCheck& AC) -> bool {
          assert(AC.CanonicalPtr);
          return !Live.count(AC.CanonicalPtr);
        });
        canonicalizeAccessChecks(Checks);
        if (verbose)
          errs() << "Liveness-pruned and canonicalized checks at tail: " << Checks << "\n";
        ForwardChecksAtTail[BB] = Checks;

        for (BasicBlock* SBB : successors(BB)) {
          ChecksOrBottom& SCOB = ForwardChecksAtHead[SBB];
          if (SCOB.Bottom) {
            SCOB.Bottom = false;
            SCOB.Checks = Checks;
            Changed = true;
          } else
            Changed |= mergeAccessChecks(SCOB.Checks, Checks, AIDirection::Forward);
        }
      }
    }

    // We have to pick a check schedule at this point. If we don't then we'll forget what we learned
    // from forward propagation.

    std::unordered_map<Instruction*, std::vector<AccessCheckWithDI>> NewChecksForInst;
    
    for (BasicBlock* BB : Blocks) {
      if (verbose)
        errs() << "Optimizing " << BB->getName() << " using forward propagation results.\n";
      const ChecksOrBottom& StateWithBottom = ForwardChecksAtHead[BB];
      
      // It's weird, but possible, that we have an unreachable block.
      if (StateWithBottom.Bottom)
        continue;
      
      std::vector<AccessCheck> Checks = StateWithBottom.Checks;
      bool NeedToCanonicalize = false;

      if (verbose)
        errs() << "Checks at head: " << Checks << "\n";
      
      for (Instruction& I : *BB) {
        auto HandleEffects = [&] () {
          EraseIf(Checks, [&] (const AccessCheck& AC) -> bool {
            return AC.CK == CheckKind::NotFree || AC.CK == CheckKind::GetAuxPtr;
          });
        };
        
        if (&I == BB->getTerminator() && BackEdgePreds.count(BB)) {
          // Execute the pollcheck.
          HandleEffects();
        }

        auto Iter = ChecksForInst.find(&I);
        if (Iter != ChecksForInst.end()) {
          std::vector<AccessCheckWithDI> NewChecks = Iter->second;
          if (verbose) {
            errs() << "Dealing with previously scheduled checks at " << I << ":\n    "
                   << NewChecks << "\n";
          }
          if (NeedToCanonicalize) {
            canonicalizeAccessChecks(Checks);
            NeedToCanonicalize = false;
          }
          subtractChecksCreatingKnowns(NewChecks, Checks);
          if (!NewChecks.empty()) {
            assert(!NewChecksForInst.count(&I));
            if (verbose)
              errs() << "Checks scheduled at " << I << ":\n    " << NewChecks << "\n";
            NewChecksForInst[&I] = NewChecks;
            EraseIf(NewChecks, [&] (const AccessCheck& AC) -> bool {
              return IsKnownCheckKind(AC.CK);
            });
            if (!NewChecks.empty()) {
              Checks.insert(Checks.end(), NewChecks.begin(), NewChecks.end());
              NeedToCanonicalize = true;
            }
          }
        }
        
        // For now, just conservatively assume that all calls may free stuff.
        if (isa<CallBase>(&I))
          HandleEffects();
        else if (isa<StoreInst>(&I) || isa<AtomicCmpXchgInst>(&I) || isa<AtomicRMWInst>(&I)) {
          EraseIf(Checks, [&] (const AccessCheck& AC) -> bool {
            return AC.CK == CheckKind::GetAuxPtr;
          });
        }
        
        if (verbose)
          errs() << "Checks after " << I << ":\n    " << Checks << "\n";
      }
    }

    ChecksForInst = std::move(NewChecksForInst);
  }
  
  void scheduleChecks(
    const std::vector<BasicBlock*>& Blocks,
    const std::unordered_set<const BasicBlock*>& BackEdgePreds) {

    if (verbose)
      errs() << "Scheduling checks for " << OldF->getName() << "\n";

    ChecksForInst.clear();
    
    for (BasicBlock* BB : Blocks) {
      for (Instruction& I : *BB) {
        std::vector<AccessCheckWithDI> Checks;
        buildChecks(&I, Checks);
        if (!Checks.empty()) {
          assert(!ChecksForInst.count(&I));
          if (verbose)
            errs() << "Checks for " << I << ":\n    " << Checks << "\n";
          ChecksForInst[&I] = std::move(Checks);
        }
      }
    }
    
    if (!optimizeChecks) {
      if (verbose)
        errs() << "Not optimizing the check schedule.\n";
      return;
    }
    
    bool Changed;
    
    // Liveness of canonical ptrs.
    //
    // We need this so that we can GC the abstract state. Without this, the abstract state is likely
    // to get very large, causing memory usage issues and long running times.
    
    std::unordered_map<BasicBlock*, std::unordered_set<Value*>> CanonicalPtrLiveAtTail;
    Changed = true;
    while (Changed) {
      Changed = false;
      for (size_t BlockIndex = Blocks.size(); BlockIndex--;) {
        BasicBlock* BB = Blocks[BlockIndex];
        std::unordered_set<Value*> Live = CanonicalPtrLiveAtTail[BB];

        for (auto It = BB->rbegin(); It != BB->rend(); ++It) {
          Instruction* I = &*It;
          Live.erase(I);
          forEachCanonicalPtrOperand(I, [&] (PtrAndOffset PAO) {
            Value* P = PAO.HighP;
            assert(isa<Instruction>(P) || isa<Argument>(P) || isa<Constant>(P));
            Live.insert(P);
          });
        }

        if (!BlockIndex) {
          for (Value* P : Live)
            assert(isa<Argument>(P) || isa<Constant>(P));
        }

        for (BasicBlock* PBB : predecessors(BB)) {
          for (Value* P : Live)
            Changed |= CanonicalPtrLiveAtTail[PBB].insert(P).second;
        }
      }
    }

    if (verbose)
      errs() << "Liveness done, doing forward propagation.\n";
    
    // Forward propagation
    //
    // This eliminates redundant checks and also shows us which checks are definitely performed along
    // which paths, which aids in making good choices during backward propagation.
    
    std::unordered_map<BasicBlock*, ChecksOrBottom> ForwardChecksAtHead;
    std::unordered_map<BasicBlock*, std::vector<AccessCheck>> ForwardChecksAtTail;

    removeRedundantChecksUsingForwardAI(
      Blocks, BackEdgePreds, CanonicalPtrLiveAtTail, ForwardChecksAtHead, ForwardChecksAtTail);

    if (!propagateChecksBackward) {
      if (verbose)
        errs() << "Not doing backwards propagation.\n";
      return;
    }

    if (verbose)
      errs() << "Forward propagation done, proceeding to backward propagation.\n";

    // For the purpose of backward propagation, we revert known checks to unknown.
    for (auto& Pair : ChecksForInst) {
      for (AccessCheck& AC : Pair.second)
        AC.CK = FundamentalCheckKind(AC.CK);
    }
    
    std::unordered_map<BasicBlock*, ChecksWithDIOrBottom> BackwardChecksAtTail;
    std::unordered_map<BasicBlock*, std::vector<AccessCheckWithDI>> BackwardChecksAtHead;

    if (verbose) {
      for (const BasicBlock* BB : BackEdgePreds)
        errs() << "Backwards edge predecessor: " << BB->getName() << "\n";
    }

    for (BasicBlock* BB : Blocks) {
      // The following cases mean that the block cannot have any checks hoisted into the tail of it:
      //
      // - It's a block that backward-branches.
      // - The terminator is a call.
      //
      // And if the block's terminator represents an exit, then we need to bootstrap it with no
      // checks.
      if (BackEdgePreds.count(BB) ||
          isa<ReturnInst>(BB->getTerminator()) ||
          isa<ResumeInst>(BB->getTerminator()) ||
          isa<UnreachableInst>(BB->getTerminator()) ||
          isa<CallBase>(BB->getTerminator())) {
        if (verbose)
          errs() << "Labeling " << BB->getName() << " as being a non-bottom.\n";
        BackwardChecksAtTail[BB].Bottom = false;
      }
    }

    Changed = true;
    while (Changed) {
      Changed = false;
      for (size_t BlockIndex = Blocks.size(); BlockIndex--;) {
        BasicBlock* BB = Blocks[BlockIndex];

        if (verbose)
          errs() << "Backwards propagation at " << BB->getName() << "\n";
        
        ChecksWithDIOrBottom& COB = BackwardChecksAtTail[BB];
        if (COB.Bottom)
          continue;

        std::vector<AccessCheckWithDI> Checks = COB.Checks;

        if (verbose)
          errs() << "Starting with checks: " << Checks << "\n";
        
        subtractChecks(Checks, ForwardChecksAtTail[BB]);

        if (verbose)
          errs() << "Checks after forward-pruning and removing unprofitable: " << Checks << "\n";

        for (auto It = BB->rbegin(); It != BB->rend(); ++It) {
          Instruction* I = &*It;

          EraseIf(Checks, [&] (const AccessCheck& AC) -> bool {
            return AC.CanonicalPtr == I;
          });

          if (isa<CallBase>(I)) {
            // Conservatively assume that the call might not return!
            Checks.clear();
          }

          auto Iter = ChecksForInst.find(I);
          if (Iter != ChecksForInst.end()) {
            std::vector<AccessCheckWithDI> ChecksForThisInst = Iter->second;
            EraseIf(ChecksForThisInst, [&] (const AccessCheckWithDI& AC) ->bool {
              return AC.CK == CheckKind::GetAuxPtr || AC.CK == CheckKind::EnsureAuxPtr;
            });
            Checks.insert(Checks.end(), ChecksForThisInst.begin(), ChecksForThisInst.end());
          }

          if (I == BB->getTerminator() && BackEdgePreds.count(BB)) {
            EraseIf(Checks, [&] (const AccessCheck& AC) -> bool {
              return AC.CK == CheckKind::NotFree;
            });
          }

          if (verbose)
            errs() << "Checks after " << *I << ":\n    " << Checks << "\n";
        }
        
        if (pred_empty(BB))
          Checks.clear();
        else {
          canonicalizeAccessChecks(Checks);
          for (BasicBlock* PBB : predecessors(BB)) {
            ChecksWithDIOrBottom& PCOB = BackwardChecksAtTail[PBB];
            if (PCOB.Bottom)
              continue;
            if (verbose)
              errs() << "Considering non-bottom predecessor " << PBB->getName() << "\n";
            std::vector<AccessCheckWithDI> ChecksAtTail = PCOB.Checks;
            if (verbose)
              errs() << "    Backwards checks at tail: " << ChecksAtTail << "\n";
            // Avoid having the merged Checks include all of the DI's if the predecessors' merged
            // Checks.
            for (AccessCheckWithDI& AC : ChecksAtTail)
              AC.DI = nullptr;
            if (verbose)
              errs() << "    Forward checks at tail: " << ForwardChecksAtTail[PBB] << "\n";
            addAccessChecks(ChecksAtTail, ForwardChecksAtTail[PBB]);
            if (verbose)
              errs() << "    Combined checks at tail: " << ChecksAtTail << "\n";
            mergeAccessChecks(Checks, ChecksAtTail, AIDirection::Forward);
            if (verbose)
              errs() << "    Merged checks: " << Checks << "\n";
          }
        }
        if (verbose)
          errs() << "Checks at head after merging with predecessors: " << Checks << "\n";
        BackwardChecksAtHead[BB] = Checks;
        for (BasicBlock* PBB : predecessors(BB)) {
          ChecksWithDIOrBottom& PCOB = BackwardChecksAtTail[PBB];
          if (PCOB.Bottom) {
            PCOB.Bottom = false;
            PCOB.Checks = Checks;
            Changed = true;
          } else
            Changed |= mergeAccessChecks(PCOB.Checks, Checks, AIDirection::Backward);
        }
      }
    }

    std::unordered_map<Instruction*, std::vector<AccessCheckWithDI>> NewChecksForInst;
    
    for (BasicBlock* BB : Blocks) {
      if (verbose) {
        errs() << "Scheduling checks in " << BB->getName()
               << " using results of backward propagation\n";
      }
      std::vector<AccessCheckWithDI> Checks = BackwardChecksAtTail[BB].Checks;
      if (verbose)
        errs() << "Starting with checks: " << Checks << "\n";
      subtractChecks(Checks, ForwardChecksAtTail[BB]);
      if (verbose)
        errs() << "Checks after forward-pruning and removing unprofitable: " << Checks << "\n";

      Instruction* LastI = nullptr;
      std::vector<AccessCheckWithDI> ChecksToEmit;
      bool SawPhi = false;
      for (auto It = BB->rbegin(); It != BB->rend(); ++It) {
        Instruction* I = &*It;

        if (isa<CallBase>(I)) {
          // Conservatively assume that the call might not return!
          ChecksToEmit.insert(ChecksToEmit.end(), Checks.begin(), Checks.end());
          Checks.clear();
        } else {
          EraseIf(Checks, [&] (const AccessCheckWithDI& AC) -> bool {
            if (AC.CanonicalPtr == I) {
              ChecksToEmit.push_back(AC);
              return true;
            }
            return false;
          });
        }
        
        auto Iter = ChecksForInst.find(I);
        if (Iter != ChecksForInst.end()) {
          std::vector<AccessCheckWithDI> ChecksForThisInst = Iter->second;
          EraseIf(ChecksForThisInst, [&] (const AccessCheckWithDI& AC) -> bool {
            return AC.CK == CheckKind::GetAuxPtr || AC.CK == CheckKind::EnsureAuxPtr;
          });
          Checks.insert(Checks.end(), ChecksForThisInst.begin(), ChecksForThisInst.end());
        }

        if (isa<PHINode>(I))
          SawPhi = true;
        if (SawPhi)
          assert(isa<PHINode>(I));
        else {
          assert(!isa<PHINode>(I));
          if (!ChecksToEmit.empty()) {
            assert(LastI);
            assert(!NewChecksForInst.count(LastI));
            canonicalizeAccessChecks(ChecksToEmit);
            if (verbose)
              errs() << "Checks scheduled at " << *LastI << ":\n    " << ChecksToEmit << "\n";
            NewChecksForInst[LastI] = std::move(ChecksToEmit);
          }
          LastI = I;
        }

        if (I == BB->getTerminator() && BackEdgePreds.count(BB)) {
          EraseIf(Checks, [&] (const AccessCheckWithDI& AC) -> bool {
            if (AC.CK == CheckKind::NotFree) {
              // FIXME: We should be able to put these checks into the slow path of the pollcheck!
              ChecksToEmit.push_back(AC);
              return true;
            }
            return false;
          });
        }
        
        if (verbose)
          errs() << "Checks after " << *I << ":\n    " << Checks << "\n";
      }

      assert(LastI);
      if (LastI == BB->getTerminator() && BackEdgePreds.count(BB)) {
        assert(ChecksToEmit.empty());
        assert(Checks.empty());
      }
      canonicalizeAccessChecks(Checks);
      subtractChecks(Checks, BackwardChecksAtHead[BB]);
      ChecksToEmit.insert(ChecksToEmit.end(), Checks.begin(), Checks.end());
      assert(!NewChecksForInst.count(LastI));
      canonicalizeAccessChecks(ChecksToEmit);
      if (!ChecksToEmit.empty()) {
        if (verbose)
          errs() << "Checks scheduled at " << *LastI << ":\n    " << ChecksToEmit << "\n";
        NewChecksForInst[LastI] = std::move(ChecksToEmit);
      }
    }

    for (auto& Pair : ChecksForInst) {
      std::vector<AccessCheckWithDI>* Checks = nullptr;
      for (const AccessCheckWithDI& AC : Pair.second) {
        if (AC.CK == CheckKind::GetAuxPtr || AC.CK == CheckKind::EnsureAuxPtr) {
          if (!Checks)
            Checks = &NewChecksForInst[Pair.first];
          Checks->push_back(AC);
        }
      }
      if (Checks)
        canonicalizeAccessChecks(*Checks);
    }
    ChecksForInst = std::move(NewChecksForInst);

    if (verbose)
      errs() << "Backwards propagation done, doing another round of forward propagation.\n";

    // Now we need to remove redundant checks again. Note that this primarily solves the problem of
    // identifying cases of known alignment and known lower bound.

    removeRedundantChecksUsingForwardAI(
      Blocks, BackEdgePreds, CanonicalPtrLiveAtTail, ForwardChecksAtHead, ForwardChecksAtTail);
  }

  Type* argType(Type* T) {
    if (IntegerType* IT = dyn_cast<IntegerType>(T)) {
      if (IT->getBitWidth() < IntPtrTy->getBitWidth())
        return IntPtrTy;
      return IT;
    }
    if (T == FloatTy)
      return DoubleTy;
    return T;
  }

  StructType* argsType(ArrayRef<Type*> Elements) {
    std::vector<Type*> NewElements;
    for (Type* T : Elements)
      NewElements.push_back(argType(T));
    return StructType::get(C, NewElements);
  }
  
  StructType* argsType(FunctionType* FT) {
    return argsType(FT->params());
  }

  Value* castToArg(Value* V, Type* OriginalArgT, Type* CanonicalArgT, Instruction* InsertBefore) {
    if (OriginalArgT == CanonicalArgT)
      return V;
    if (IntegerType* VIT = dyn_cast<IntegerType>(OriginalArgT)) {
      IntegerType* CanonicalArgIT = cast<IntegerType>(CanonicalArgT);
      assert(VIT->getBitWidth() < CanonicalArgIT->getBitWidth());
      Instruction *ZExt = new ZExtInst(V, CanonicalArgT, "filc_cast_to_arg_int", InsertBefore);
      ZExt->setDebugLoc(InsertBefore->getDebugLoc());
      return ZExt;
    }
    assert(OriginalArgT == FloatTy);
    assert(CanonicalArgT == DoubleTy);
    Instruction* FPExt = new FPExtInst(V, DoubleTy, "filc_cast_to_arg_double", InsertBefore);
    FPExt->setDebugLoc(InsertBefore->getDebugLoc());
    return FPExt;
  }

  Value* castFromArg(Value* V, Type* OriginalArgT, Instruction* InsertBefore) {
    if (V->getType() == OriginalArgT)
      return V;
    if (IntegerType* VIT = dyn_cast<IntegerType>(V->getType())) {
      IntegerType* OriginalArgIT = cast<IntegerType>(OriginalArgT);
      assert(VIT->getBitWidth() > OriginalArgIT->getBitWidth());
      Instruction* Trunc = new TruncInst(V, OriginalArgT, "filc_cast_from_arg_int", InsertBefore);
      Trunc->setDebugLoc(InsertBefore->getDebugLoc());
      return Trunc;
    }
    assert(V->getType() == DoubleTy);
    assert(OriginalArgT == FloatTy);
    Instruction* FPTrunc = new FPTruncInst(V, FloatTy, "filc_cast_from_arg_double", InsertBefore);
    FPTrunc->setDebugLoc(InsertBefore->getDebugLoc());
    return FPTrunc;
  }

  Value* loadCC(Type* T, Value* PassedSize, FunctionCallee CCCheckFailure, Instruction* InsertBefore,
                DebugLoc DI) {
    size_t TypeSize = (DL.getTypeStoreSize(T) + WordSize - 1) / WordSize * WordSize;
    assert(TypeSize);
    Instruction* PassedEnough = new ICmpInst(
      InsertBefore, ICmpInst::ICMP_ULE, ConstantInt::get(IntPtrTy, TypeSize), PassedSize,
      "filc_cc_passed_enough");
    PassedEnough->setDebugLoc(DI);
    Instruction* FailTerm = SplitBlockAndInsertIfElse(
      expectTrue(PassedEnough, InsertBefore), InsertBefore, true);
    CallInst::Create(
      CCCheckFailure, { PassedSize, ConstantInt::get(IntPtrTy, TypeSize), getOrigin(DI) }, "",
      FailTerm)->setDebugLoc(DI);
    AllocaInst* PayloadAlloca = new AllocaInst(
      Int8Ty, 0, ConstantInt::get(IntPtrTy, TypeSize), DL.getABITypeAlign(T), "filc_payload_alloca",
      &NewF->getEntryBlock().front());
    AllocaInst* AuxAlloca = new AllocaInst(
      Int8Ty, 0, ConstantInt::get(IntPtrTy, TypeSize), Align(WordSize),
      "filc_payload_alloca", &NewF->getEntryBlock().front());
    PayloadAlloca->setDebugLoc(DI);
    AuxAlloca->setDebugLoc(DI);
    CallInst::Create(LifetimeStart, { ConstantInt::get(IntPtrTy, TypeSize), PayloadAlloca }, "",
                     InsertBefore)->setDebugLoc(DI);
    CallInst::Create(LifetimeStart, { ConstantInt::get(IntPtrTy, TypeSize), AuxAlloca }, "",
                     InsertBefore)->setDebugLoc(DI);
    Value* InlineBuffer = threadCCInlineBufferPtr(MyThread, InsertBefore);
    CallInst* Call = CallInst::Create(
      RealMemcpy,
      { PayloadAlloca, InlineBuffer, ConstantInt::get(IntPtrTy, std::min(TypeSize, CCInlineSize)),
        ConstantInt::getBool(Int1Ty, false) },
      "", InsertBefore);
    Call->addParamAttr(0, Attribute::getWithAlignment(C, Align(WordSize)));
    Call->addParamAttr(
      1, Attribute::getWithAlignment(C, Align(MinAlign(CCAlignment, DL.getABITypeAlign(T).value()))));
    Call->setDebugLoc(DI);
    Value* InlineAuxBuffer = threadCCInlineAuxBufferPtr(MyThread, InsertBefore);
    Call = CallInst::Create(
      RealMemcpy,
      { AuxAlloca, InlineAuxBuffer, ConstantInt::get(IntPtrTy, std::min(TypeSize, CCInlineSize)),
        ConstantInt::getBool(Int1Ty, false) },
      "", InsertBefore);
    Call->addParamAttr(0, Attribute::getWithAlignment(C, Align(WordSize)));
    Call->addParamAttr(1, Attribute::getWithAlignment(C, Align(CCAlignment)));
    Call->setDebugLoc(DI);
    if (TypeSize > CCInlineSize) {
      Instruction* PayloadAtOutlineOffset = GetElementPtrInst::Create(
        Int8Ty, PayloadAlloca, { ConstantInt::get(IntPtrTy, CCInlineSize) },
        "filc_payload_at_outline_offset", InsertBefore);
      PayloadAtOutlineOffset->setDebugLoc(DI);
      Value* OutlineBufferPtr = threadCCOutlineBufferPtr(MyThread, InsertBefore);
      Instruction* OutlineBuffer = new LoadInst(
        RawPtrTy, OutlineBufferPtr, "filc_cc_outline_buffer", InsertBefore);
      OutlineBuffer->setDebugLoc(DI);
      Call = CallInst::Create(
        RealMemcpy,
        { PayloadAtOutlineOffset, OutlineBuffer, ConstantInt::get(IntPtrTy, TypeSize - CCInlineSize),
          ConstantInt::getBool(Int1Ty, false) },
        "", InsertBefore);
      Call->addParamAttr(0, Attribute::getWithAlignment(C, Align(WordSize)));
      Call->addParamAttr(
        1, Attribute::getWithAlignment(
          C, Align(MinAlign(CCAlignment, DL.getABITypeAlign(T).value()))));
      Call->setDebugLoc(DI);
      Instruction* AuxAtOutlineOffset = GetElementPtrInst::Create(
        Int8Ty, AuxAlloca, { ConstantInt::get(IntPtrTy, CCInlineSize) },
        "filc_aux_at_outline_offset", InsertBefore);
      AuxAtOutlineOffset->setDebugLoc(DI);
      Value* OutlineAuxBufferPtr = threadCCOutlineAuxBufferPtr(MyThread, InsertBefore);
      Instruction* OutlineAuxBuffer = new LoadInst(
        RawPtrTy, OutlineAuxBufferPtr, "filc_cc_outline_aux_buffer", InsertBefore);
      OutlineAuxBuffer->setDebugLoc(DI);
      Call = CallInst::Create(
        RealMemcpy,
        { AuxAtOutlineOffset, OutlineAuxBuffer, ConstantInt::get(IntPtrTy, TypeSize - CCInlineSize),
          ConstantInt::getBool(Int1Ty, false) },
        "", InsertBefore);
      Call->addParamAttr(0, Attribute::getWithAlignment(C, Align(WordSize)));
      Call->addParamAttr(1, Attribute::getWithAlignment(C, Align(CCAlignment)));
      Call->setDebugLoc(DI);
    }
    Value* Result = loadValueRecurseAfterCheck(
      T, PayloadAlloca, AuxAlloca, AuxAlloca, false, DL.getABITypeAlign(T), AtomicOrdering::NotAtomic,
      SyncScope::System, MemoryKind::CC, InsertBefore);
    CallInst::Create(LifetimeEnd, { ConstantInt::get(IntPtrTy, TypeSize), PayloadAlloca }, "",
                     InsertBefore)->setDebugLoc(DI);
    CallInst::Create(LifetimeEnd, { ConstantInt::get(IntPtrTy, TypeSize), AuxAlloca }, "",
                     InsertBefore)->setDebugLoc(DI);
    return Result;
  }

  // Returns the passed CC size. That's just for convenience, since you could calculate it yourself
  // from the type.
  Value* storeCC(Type* T, Value* V, Instruction* InsertBefore, DebugLoc DI) {
    size_t TypeSize = (DL.getTypeStoreSize(T) + WordSize - 1) / WordSize * WordSize;
    assert(TypeSize);
    if (TypeSize > CCInlineSize) {
      size_t DesiredOutlineSize = TypeSize - CCInlineSize;
      Instruction* ActualOutlineSize = new LoadInst(
        IntPtrTy, threadCCOutlineSizePtr(MyThread, InsertBefore), "filc_thread_cc_outline_size",
        InsertBefore);
      ActualOutlineSize->setDebugLoc(DI);
      Instruction* OutlineBigEnough = new ICmpInst(
        InsertBefore, ICmpInst::ICMP_ULE, ConstantInt::get(IntPtrTy, DesiredOutlineSize),
        ActualOutlineSize, "filc_cc_passed_enough");
      OutlineBigEnough->setDebugLoc(DI);
      Instruction* SlowTerm = SplitBlockAndInsertIfElse(
        expectTrue(OutlineBigEnough, InsertBefore), InsertBefore, false);
      CallInst::Create(
        ThreadEnsureCCOutlineBufferSlow,
        { MyThread, ConstantInt::get(IntPtrTy, DesiredOutlineSize) }, "", SlowTerm)->setDebugLoc(DI);
    }
    AllocaInst* PayloadAlloca = new AllocaInst(
      Int8Ty, 0, ConstantInt::get(IntPtrTy, TypeSize), DL.getABITypeAlign(T), "filc_payload_alloca",
      &NewF->getEntryBlock().front());
    AllocaInst* AuxAlloca = new AllocaInst(
      Int8Ty, 0, ConstantInt::get(IntPtrTy, TypeSize), Align(WordSize),
      "filc_payload_alloca", &NewF->getEntryBlock().front());
    PayloadAlloca->setDebugLoc(DI);
    AuxAlloca->setDebugLoc(DI);
    CallInst::Create(LifetimeStart, { ConstantInt::get(IntPtrTy, TypeSize), PayloadAlloca }, "",
                     InsertBefore)->setDebugLoc(DI);
    CallInst::Create(LifetimeStart, { ConstantInt::get(IntPtrTy, TypeSize), AuxAlloca }, "",
                     InsertBefore)->setDebugLoc(DI);
    CallInst* Call = CallInst::Create(
      RealMemset,
      { PayloadAlloca, ConstantInt::get(Int8Ty, 0), ConstantInt::get(IntPtrTy, TypeSize),
        ConstantInt::getBool(Int1Ty, false) }, "", InsertBefore);
    Call->addParamAttr(0, Attribute::getWithAlignment(C, DL.getABITypeAlign(T)));
    Call->setDebugLoc(DI);
    Call = CallInst::Create(
      RealMemset,
      { AuxAlloca, ConstantInt::get(Int8Ty, 0), ConstantInt::get(IntPtrTy, TypeSize),
        ConstantInt::getBool(Int1Ty, false) }, "", InsertBefore);
    Call->addParamAttr(0, Attribute::getWithAlignment(C, Align(WordSize)));
    Call->setDebugLoc(DI);
    storeValueRecurseAfterCheck(
      T, V, PayloadAlloca, AuxAlloca, false, DL.getABITypeAlign(T), AtomicOrdering::NotAtomic,
      SyncScope::System, MemoryKind::CC, InsertBefore);
    Call = CallInst::Create(
      RealMemcpy,
      { threadCCInlineBufferPtr(MyThread, InsertBefore), PayloadAlloca,
        ConstantInt::get(IntPtrTy, std::min(TypeSize, CCInlineSize)),
        ConstantInt::getBool(Int1Ty, false) },
      "", InsertBefore);
    Call->addParamAttr(
      0, Attribute::getWithAlignment(C, Align(MinAlign(CCAlignment, DL.getABITypeAlign(T).value()))));
    Call->addParamAttr(1, Attribute::getWithAlignment(C, Align(WordSize)));
    Call->setDebugLoc(DI);
    Call = CallInst::Create(
      RealMemcpy,
      { threadCCInlineAuxBufferPtr(MyThread, InsertBefore), AuxAlloca,
        ConstantInt::get(IntPtrTy, std::min(TypeSize, CCInlineSize)),
        ConstantInt::getBool(Int1Ty, false) },
      "", InsertBefore);
    Call->addParamAttr(0, Attribute::getWithAlignment(C, Align(CCAlignment)));
    Call->addParamAttr(1, Attribute::getWithAlignment(C, Align(WordSize)));
    Call->setDebugLoc(DI);
    if (TypeSize > CCInlineSize) {
      Instruction* PayloadAtOutlineOffset = GetElementPtrInst::Create(
        Int8Ty, PayloadAlloca, { ConstantInt::get(IntPtrTy, CCInlineSize) },
        "filc_payload_at_outline_offset", InsertBefore);
      PayloadAtOutlineOffset->setDebugLoc(DI);
      Value* OutlineBufferPtr = threadCCOutlineBufferPtr(MyThread, InsertBefore);
      Instruction* OutlineBuffer = new LoadInst(
        RawPtrTy, OutlineBufferPtr, "filc_cc_outline_buffer", InsertBefore);
      OutlineBuffer->setDebugLoc(DI);
      Call = CallInst::Create(
        RealMemcpy,
        { OutlineBuffer, PayloadAtOutlineOffset, ConstantInt::get(IntPtrTy, TypeSize - CCInlineSize),
          ConstantInt::getBool(Int1Ty, false) },
        "", InsertBefore);
      Call->addParamAttr(
        0, Attribute::getWithAlignment(
          C, Align(MinAlign(CCAlignment, DL.getABITypeAlign(T).value()))));
      Call->addParamAttr(1, Attribute::getWithAlignment(C, Align(WordSize)));
      Call->setDebugLoc(DI);
      Instruction* AuxAtOutlineOffset = GetElementPtrInst::Create(
        Int8Ty, AuxAlloca, { ConstantInt::get(IntPtrTy, CCInlineSize) },
        "filc_aux_at_outline_offset", InsertBefore);
      AuxAtOutlineOffset->setDebugLoc(DI);
      Value* OutlineAuxBufferPtr = threadCCOutlineAuxBufferPtr(MyThread, InsertBefore);
      Instruction* OutlineAuxBuffer = new LoadInst(
        RawPtrTy, OutlineAuxBufferPtr, "filc_cc_outline_aux_buffer", InsertBefore);
      OutlineAuxBuffer->setDebugLoc(DI);
      Call = CallInst::Create(
        RealMemcpy,
        { OutlineAuxBuffer, AuxAtOutlineOffset, ConstantInt::get(IntPtrTy, TypeSize - CCInlineSize),
          ConstantInt::getBool(Int1Ty, false) },
        "", InsertBefore);
      Call->addParamAttr(0, Attribute::getWithAlignment(C, Align(CCAlignment)));
      Call->addParamAttr(1, Attribute::getWithAlignment(C, Align(WordSize)));
      Call->setDebugLoc(DI);
    }
    CallInst::Create(LifetimeEnd, { ConstantInt::get(IntPtrTy, TypeSize), PayloadAlloca }, "",
                     InsertBefore)->setDebugLoc(DI);
    CallInst::Create(LifetimeEnd, { ConstantInt::get(IntPtrTy, TypeSize), AuxAlloca }, "",
                     InsertBefore)->setDebugLoc(DI);
    return ConstantInt::get(IntPtrTy, TypeSize);
  }

  bool hasNonNullPtrs(Constant* C) {
    if (isa<ConstantData>(C))
      return false;
    if (ConstantAggregate* CA = dyn_cast<ConstantAggregate>(C)) {
      for (size_t Index = CA->getNumOperands(); Index--;) {
        if (hasNonNullPtrs(CA->getOperand(Index)))
          return true;
      }
      return false;
    }
    return hasPtrs(C->getType());
  }

  Constant* paddedConstant(Constant* C) {
    Type* LowT = C->getType();
    size_t TypeSize = DL.getTypeStoreSize(LowT);
    if (!(TypeSize % WordSize))
      return C;
    size_t PaddingSize = WordSize - (TypeSize % WordSize);
    if (verbose)
      errs() << "TypeSize = " << TypeSize << ", PaddingSize = " << PaddingSize << "\n";
    ArrayType* PaddingTy = ArrayType::get(Int8Ty, PaddingSize);
    StructType* PaddedTy = StructType::get(this->C, { LowT, PaddingTy });
    return ConstantStruct::get(PaddedTy, { C, ConstantAggregateZero::get(PaddingTy) });
  }

  enum class TryLowerConstantImplMode {
    NeedFullFlightConstant,
    NeedRestConstantWithPtrPlaceholders
  };
  Constant* tryLowerConstantImpl(Constant* C, TryLowerConstantImplMode LM) {
    assert(C->getType() != FlightPtrTy);

    bool needsFlight = LM == TryLowerConstantImplMode::NeedFullFlightConstant;
    Type* ResultT;
    if (needsFlight)
      ResultT = toFlightType(C->getType());
    else
      ResultT = C->getType();
    
    if (isa<UndefValue>(C)) {
      if (isa<IntegerType>(C->getType()))
        return ConstantInt::get(C->getType(), 0);
      if (C->getType()->isFloatingPointTy())
        return ConstantFP::get(C->getType(), 0.);
      if (C->getType() == RawPtrTy)
        return needsFlight ? FlightNull : RawNull;
      return ConstantAggregateZero::get(ResultT);
    }
    
    if (isa<ConstantPointerNull>(C))
      return needsFlight ? FlightNull : RawNull;

    if (isa<ConstantAggregateZero>(C))
      return ConstantAggregateZero::get(ResultT);

    if (GlobalValue* G = dyn_cast<GlobalValue>(C)) {
      assert(!shouldPassThrough(G));
      switch (LM) {
      case TryLowerConstantImplMode::NeedFullFlightConstant:
        return nullptr;
      case TryLowerConstantImplMode::NeedRestConstantWithPtrPlaceholders:
        return RawNull;
      }
      llvm_unreachable("should not get here");
    }

    if (isa<ConstantData>(C))
      return C;

    if (ConstantArray* CA = dyn_cast<ConstantArray>(C)) {
      std::vector<Constant*> Args;
      for (size_t Index = 0; Index < CA->getNumOperands(); ++Index) {
        Constant* LowC = tryLowerConstantImpl(CA->getOperand(Index), LM);
        if (!LowC)
          return nullptr;
        Args.push_back(LowC);
      }
      return ConstantArray::get(cast<ArrayType>(ResultT), Args);
    }
    if (ConstantStruct* CS = dyn_cast<ConstantStruct>(C)) {
      if (verbose)
        errs() << "Dealing with CS = " << *CS << "\n";
      std::vector<Constant*> Args;
      for (size_t Index = 0; Index < CS->getNumOperands(); ++Index) {
        Constant* LowC = tryLowerConstantImpl(CS->getOperand(Index), LM);
        if (!LowC)
          return nullptr;
        if (verbose)
          errs() << "Index = " << Index << ", LowC = " << *LowC << "\n";
        Args.push_back(LowC);
      }
      return ConstantStruct::get(cast<StructType>(ResultT), Args);
    }
    if (ConstantVector* CV = dyn_cast<ConstantVector>(C)) {
      std::vector<Constant*> Args;
      for (size_t Index = 0; Index < CV->getNumOperands(); ++Index) {
        Constant* LowC = tryLowerConstantImpl(CV->getOperand(Index), LM);
        if (!LowC)
          return nullptr;
        Args.push_back(LowC);
      }
      return ConstantVector::get(Args);
    }

    assert(isa<ConstantExpr>(C));
    ConstantExpr* CE = cast<ConstantExpr>(C);

    if (verbose)
      errs() << "Lowering CE = " << *CE << "\n";
    switch (LM) {
    case TryLowerConstantImplMode::NeedFullFlightConstant:
      return nullptr;
    case TryLowerConstantImplMode::NeedRestConstantWithPtrPlaceholders:
      if (isa<IntegerType>(CE->getType()))
        return ConstantInt::get(CE->getType(), 0);
      if (CE->getType() == RawPtrTy)
        return RawNull;
      
      llvm_unreachable("wtf kind of CE is that");
      return nullptr;
    }
    llvm_unreachable("bad RM");
  }

  Constant* constantToRestConstantWithPtrPlaceholders(Constant* C) {
    return tryLowerConstantImpl(C, TryLowerConstantImplMode::NeedRestConstantWithPtrPlaceholders);
  }

  Constant* tryConstantToFlightConstant(Constant* C) {
    return tryLowerConstantImpl(C, TryLowerConstantImplMode::NeedFullFlightConstant);
  }

  ConstantTarget constexprRecurse(Constant* C) {
    assert(C->getType() != FlightPtrTy);
    
    if (GlobalValue* G = dyn_cast<GlobalValue>(C)) {
      assert(!shouldPassThrough(G));
      assert(!Getters.count(G));
      assert(GlobalToGetter.count(G));
      Function* Getter = GlobalToGetter[G];
      return ConstantTarget(ConstantKind::Global, Getter);
    }

    if (ConstantExpr* CE = dyn_cast<ConstantExpr>(C)) {
      switch (CE->getOpcode()) {
      case Instruction::GetElementPtr: {
        ConstantTarget Target = constexprRecurse(CE->getOperand(0));
        APInt OffsetAP(64, 0, false);
        GetElementPtrInst* GEP = cast<GetElementPtrInst>(CE->getAsInstruction());
        bool result = GEP->accumulateConstantOffset(DL, OffsetAP);
        GEP->deleteValue();
        if (!result)
          return ConstantTarget();
        uint64_t Offset = OffsetAP.getZExtValue();
        Constant* CS = ConstantStruct::get(
          ConstexprNodeTy,
          { ConstantInt::get(Int32Ty, static_cast<unsigned>(ConstexprOpcode::AddPtrImmediate)),
            ConstantInt::get(Int32Ty, static_cast<unsigned>(Target.Kind)),
            Target.Target,
            ConstantInt::get(IntPtrTy, Offset) });
        GlobalVariable* ExprG = new GlobalVariable(
          M, ConstexprNodeTy, true, GlobalVariable::PrivateLinkage, CS, "filc_constexpr_gep_node");
        return ConstantTarget(ConstantKind::Expr, ExprG);
      }
      default:
        return ConstantTarget();
      }
    }

    return ConstantTarget();
  }

  bool computeConstantRelocations(Constant* C, std::vector<ConstantRelocation>& Result, size_t Offset = 0) {
    assert(C->getType() != FlightPtrTy);

    if (ConstantTarget CT = constexprRecurse(C)) {
      assert(!(Offset % WordSize));
      Result.push_back(ConstantRelocation(Offset, CT.Kind, CT.Target));
      return true;
    }
    
    assert(!isa<GlobalValue>(C)); // Should have been caught by constexprRecurse.

    if (isa<UndefValue>(C))
      return true;
    
    if (isa<ConstantPointerNull>(C))
      return true;

    if (isa<ConstantAggregateZero>(C))
      return true;

    if (isa<ConstantData>(C))
      return true;

    if (ConstantArray* CA = dyn_cast<ConstantArray>(C)) {
      size_t ElementSize = DL.getTypeAllocSize(CA->getType()->getElementType());
      for (size_t Index = 0; Index < CA->getNumOperands(); ++Index) {
        if (!computeConstantRelocations(CA->getOperand(Index), Result, Offset + Index * ElementSize))
          return false;
      }
      return true;
    }
    if (ConstantStruct* CS = dyn_cast<ConstantStruct>(C)) {
      if (verbose)
        errs() << "Dealing with CS = " << *CS << "\n";
      const StructLayout* SL = DL.getStructLayout(CS->getType());
      for (size_t Index = 0; Index < CS->getNumOperands(); ++Index) {
        if (!computeConstantRelocations(
              CS->getOperand(Index), Result, Offset + SL->getElementOffset(Index)))
          return false;
      }
      return true;
    }
    if (ConstantVector* CV = dyn_cast<ConstantVector>(C)) {
      size_t ElementSize = DL.getTypeAllocSize(CV->getType()->getElementType());
      for (size_t Index = 0; Index < CV->getNumOperands(); ++Index) {
        if (!computeConstantRelocations(CV->getOperand(Index), Result, Offset + Index * ElementSize))
          return false;
      }
      return true;
    }

    assert(isa<ConstantExpr>(C));
    if (verbose)
      errs() << "Failing to handle CE: " << *C << "\n";
    return false;
  }

  Value* constantToFlightValue(Constant* C, Instruction* InsertBefore, Value* InitializationContext) {
    if (ultraVerbose)
      errs() << "constantToFlightValue(" << *C << ", ..., " << *InitializationContext << ")\n";
    assert(C->getType() != FlightPtrTy);

    if (Constant* LowC = tryConstantToFlightConstant(C))
      return LowC;
    
    if (GlobalValue* G = dyn_cast<GlobalValue>(C)) {
      assert(!shouldPassThrough(G)); // This is a necessary safety check, at least for setjmp, probably for other things too.
      assert(!GlobalToGetter.count(nullptr));
      assert(!Getters.count(nullptr));
      assert(!Getters.count(G));
      assert(GlobalToGetter.count(G));
      Function* Getter = GlobalToGetter[G];
      assert(Getter);
      if (Getter->isDeclaration() &&
          (Getter->hasWeakAnyLinkage() || Getter->hasExternalWeakLinkage())) {
        ICmpInst* IsNull = new ICmpInst(
          InsertBefore, ICmpInst::ICMP_EQ, Getter, RawNull, "filc_weak_symbol_is_null");
        IsNull->setDebugLoc(InsertBefore->getDebugLoc());
        Instruction* NotNullTerm = SplitBlockAndInsertIfElse(IsNull, InsertBefore, false);
        Instruction* NotNullResult = CallInst::Create(
        GlobalGetterTy, Getter, { InitializationContext }, "filc_call_weak_getter", NotNullTerm);
        NotNullResult->setDebugLoc(InsertBefore->getDebugLoc());
        PHINode* Result = PHINode::Create(FlightPtrTy, 2, "filc_weak_getter_result", InsertBefore);
        Result->setDebugLoc(InsertBefore->getDebugLoc());
        Result->addIncoming(FlightNull, IsNull->getParent());
        Result->addIncoming(NotNullResult, NotNullResult->getParent());
        return Result;
      }
      Instruction* Result = CallInst::Create(
        GlobalGetterTy, Getter, { InitializationContext }, "filc_call_getter", InsertBefore);
      Result->setDebugLoc(InsertBefore->getDebugLoc());
      return Result;
    }

    if (isa<ConstantData>(C))
      return C;

    if (ConstantArray* CA = dyn_cast<ConstantArray>(C)) {
      Value* Result = UndefValue::get(toFlightType(CA->getType()));
      for (size_t Index = 0; Index < CA->getNumOperands(); ++Index) {
        Instruction* Insert = InsertValueInst::Create(
          Result, constantToFlightValue(CA->getOperand(Index), InsertBefore, InitializationContext),
          static_cast<unsigned>(Index), "filc_insert_array", InsertBefore);
        Insert->setDebugLoc(InsertBefore->getDebugLoc());
        Result = Insert;
      }
      return Result;
    }
    if (ConstantStruct* CS = dyn_cast<ConstantStruct>(C)) {
      if (verbose)
        errs() << "Dealing with CS = " << *CS << "\n";
      Value* Result = UndefValue::get(toFlightType(CS->getType()));
      for (size_t Index = 0; Index < CS->getNumOperands(); ++Index) {
        Value* LowC = constantToFlightValue(
          CS->getOperand(Index), InsertBefore, InitializationContext);
        if (verbose)
          errs() << "Index = " << Index << ", LowC = " << *LowC << "\n";
        Instruction* Insert = InsertValueInst::Create(
          Result, LowC, static_cast<unsigned>(Index), "filc_insert_struct", InsertBefore);
        Insert->setDebugLoc(InsertBefore->getDebugLoc());
        Result = Insert;
      }
      return Result;
    }
    if (ConstantVector* CV = dyn_cast<ConstantVector>(C)) {
      Value* Result = UndefValue::get(toFlightType(CV->getType()));
      for (size_t Index = 0; Index < CV->getNumOperands(); ++Index) {
        Instruction* Insert = InsertElementInst::Create(
          Result, constantToFlightValue(CV->getOperand(Index), InsertBefore, InitializationContext),
          ConstantInt::get(IntPtrTy, Index), "filc_insert_vector", InsertBefore);
        Insert->setDebugLoc(InsertBefore->getDebugLoc());
        Result = Insert;
      }
      return Result;
    }

    assert(isa<ConstantExpr>(C));
    ConstantExpr* CE = cast<ConstantExpr>(C);

    if (verbose)
      errs() << "Lowering CE = " << *CE << "\n";

    Instruction* CEInst = CE->getAsInstruction(InsertBefore);
    CEInst->setDebugLoc(InsertBefore->getDebugLoc());

    // I am the worst compiler programmer.
    StoreInst* DummyUser = new StoreInst(
      CEInst, RawNull, false, Align(), AtomicOrdering::NotAtomic, SyncScope::System);
    lowerInstruction(CEInst, InitializationContext);
    Value* Result = DummyUser->getOperand(0);
    DummyUser->deleteValue();
    return Result;
  }

  Value* castInt(Value* V, Type* T, Instruction *InsertionPoint) {
    if (V->getType() == T)
      return V;
    Instruction* Result =
      CastInst::CreateIntegerCast(V, T, false, "filc_makeintptr", InsertionPoint);
    Result->setDebugLoc(InsertionPoint->getDebugLoc());
    return Result;
  }

  Value* makeIntPtr(Value* V, Instruction *InsertionPoint) {
    return castInt(V, IntPtrTy, InsertionPoint);
  }

  template<typename Func>
  void hackRAUW(Value* V, const Func& GetNewValue) {
    assert(!Dummy->getNumUses());
    V->replaceAllUsesWith(Dummy);
    Dummy->replaceAllUsesWith(GetNewValue());
  }

  void captureTypesIfNecessary(Instruction* I) {
    if (StoreInst* SI = dyn_cast<StoreInst>(I)) {
      InstTypes[I] = SI->getValueOperand()->getType();
      return;
    }

    if (AtomicCmpXchgInst* AI = dyn_cast<AtomicCmpXchgInst>(I)) {
      InstTypes[I] = AI->getNewValOperand()->getType();
      return;
    }

    if (AtomicRMWInst* AI = dyn_cast<AtomicRMWInst>(I)) {
      InstTypes[I] = AI->getValOperand()->getType();
      return;
    }

    if (CallBase* CI = dyn_cast<CallBase>(I)) {
      std::vector<Type*> Types;
      for (size_t Index = 0; Index < CI->arg_size(); ++Index)
        Types.push_back(CI->getArgOperand(Index)->getType());
      InstTypeVectors[I] = std::move(Types);
      if (InvokeInst* II = dyn_cast<InvokeInst>(I))
        LPIs[II] = II->getLandingPadInst();
      return;
    }
  }

  Value* lowerConstantValue(Value* V, Instruction* I, Value* InitializationContext) {
    assert(!isa<PHINode>(I));
    if (Constant* C = dyn_cast<Constant>(V)) {
      if (ultraVerbose)
        errs() << "Got V = " << *V << ", C = " << *C << "\n";
      Value* NewC = constantToFlightValue(C, I, InitializationContext);
      if (ultraVerbose)
        errs() << "Got NewC = " << *NewC <<"\n";
      return NewC;
    }
    if (Argument* A = dyn_cast<Argument>(V)) {
      if (ultraVerbose) {
        errs() << "A = " << *A << "\n";
        errs() << "A->getArgNo() == " << A->getArgNo() << "\n";
        errs() << "Args[A->getArgNo()] == " << *Args[A->getArgNo()] << "\n";
      }
      return Args[A->getArgNo()];
    }
    return V;
  }

  void lowerConstantOperand(Use& U, Instruction* I, Value* InitializationContext) {
    U = lowerConstantValue(U, I, InitializationContext);
  }

  void lowerConstantOperands(Instruction* I, Value* InitializationContext) {
    if (verbose)
      errs() << "Before arg lowering: " << *I << "\n";

    for (unsigned Index = I->getNumOperands(); Index--;) {
      Use& U = I->getOperandUse(Index);
      lowerConstantOperand(U, I, InitializationContext);
      if (ultraVerbose)
        errs() << "After Index = " << Index << ", I = " << *I << "\n";
    }
    
    if (verbose)
      errs() << "After arg lowering: " << *I << "\n";
  }

  bool earlyLowerInstruction(Instruction* I) {
    if (verbose)
      errs() << "Early lowering: " << *I << "\n";

    if (IntrinsicInst* II = dyn_cast<IntrinsicInst>(I)) {
      if (verbose)
        errs() << "It's an intrinsic.\n";
      switch (II->getIntrinsicID()) {
      case Intrinsic::memset:
      case Intrinsic::memset_inline: {
        lowerConstantOperand(II->getArgOperandUse(0), I, RawNull);
        lowerConstantOperand(II->getArgOperandUse(1), I, RawNull);
        lowerConstantOperand(II->getArgOperandUse(2), I, RawNull);
        Instruction* CI = CallInst::Create(
          Memset,
          { MyThread, II->getArgOperand(0), castInt(II->getArgOperand(1), Int32Ty, II),
            makeIntPtr(II->getArgOperand(2), II), getOrigin(II->getDebugLoc()) });
        ReplaceInstWithInst(II, CI);
        return true;
      }
      case Intrinsic::memcpy:
      case Intrinsic::memcpy_inline:
      case Intrinsic::memmove: {
        lowerConstantOperand(II->getArgOperandUse(0), I, RawNull);
        lowerConstantOperand(II->getArgOperandUse(1), I, RawNull);
        lowerConstantOperand(II->getArgOperandUse(2), I, RawNull);
        Instruction* CI = CallInst::Create(
          Memmove,
          { MyThread, II->getArgOperand(0), II->getArgOperand(1),
            makeIntPtr(II->getArgOperand(2), II), getOrigin(II->getDebugLoc()) });
        ReplaceInstWithInst(II, CI);
        return true;
      }

      case Intrinsic::lifetime_start:
      case Intrinsic::lifetime_end:
      case Intrinsic::stacksave:
      case Intrinsic::stackrestore:
      case Intrinsic::assume:
      case Intrinsic::experimental_noalias_scope_decl:
      case Intrinsic::donothing:
      case Intrinsic::dbg_declare:
      case Intrinsic::dbg_value:
      case Intrinsic::dbg_assign:
      case Intrinsic::dbg_label:
      case Intrinsic::invariant_start:
      case Intrinsic::invariant_end:
      case Intrinsic::launder_invariant_group:
      case Intrinsic::strip_invariant_group:
        llvm_unreachable("Should have already been erased");
        return true;

      case Intrinsic::vastart:
        assert(UsesVastartOrZargs);
        lowerConstantOperand(II->getArgOperandUse(0), I, RawNull);
        storePtr(SnapshottedArgsPtrForVastart, flightPtrPtr(II->getArgOperand(0), II),
                 auxPtrForOperand(II->getArgOperand(0), II, 0, II).P, II);
        II->eraseFromParent();
        return true;
        
      case Intrinsic::vacopy: {
        lowerConstantOperand(II->getArgOperandUse(0), I, RawNull);
        lowerConstantOperand(II->getArgOperandUse(1), I, RawNull);
        Value* Load = loadPtr(flightPtrPtr(II->getArgOperand(1), II),
                              auxPtrForOperand(II->getArgOperand(1), II, 1, II), II);
        storePtr(Load, flightPtrPtr(II->getArgOperand(0), II),
                 auxPtrForOperand(II->getArgOperand(0), II, 0, II).P, II);
        II->eraseFromParent();
        return true;
      }
        
      case Intrinsic::vaend:
        II->eraseFromParent();
        return true;

      case Intrinsic::eh_typeid_for: {
        Constant* TypeConstant = cast<Constant>(II->getArgOperand(0));
        assert(EHTypeIDs.count(TypeConstant));
        II->replaceAllUsesWith(ConstantInt::get(Int32Ty, EHTypeIDs[TypeConstant]));
        II->eraseFromParent();
        return true;
      }

      case Intrinsic::eh_sjlj_functioncontext:
      case Intrinsic::eh_sjlj_setjmp:
      case Intrinsic::eh_sjlj_longjmp:
      case Intrinsic::eh_sjlj_setup_dispatch:
        llvm_unreachable("Cannot use sjlj intrinsics.");
        return true;

      case Intrinsic::x86_xgetbv:
        return true;

      default:
        if (!II->getCalledFunction()->doesNotAccessMemory()
            && !isa<ConstrainedFPIntrinsic>(II)) {
          if (verbose)
            llvm::errs() << "Unhandled intrinsic: " << *II << "\n";
          std::string str;
          raw_string_ostream outs(str);
          outs << "Unhandled intrinsic: " << *II;
          CallInst::Create(Error, { getString(str), getOrigin(I->getDebugLoc()) }, "", II)
            ->setDebugLoc(II->getDebugLoc());
        }
        for (Use& U : II->data_ops()) {
          lowerConstantOperand(U, I, RawNull);
          if (hasPtrs(U->getType()))
            U = flightPtrPtr(U, II);
        }
        if (hasPtrs(II->getType()))
          hackRAUW(II, [&] () { return badFlightPtr(II, II->getNextNode()); });
        return true;
      }
    }
    
    if (CallBase* CI = dyn_cast<CallBase>(I)) {
      if (verbose) {
        errs() << "It's a call!\n";
        errs() << "Callee = " << CI->getCalledOperand() << "\n";
        if (CI->getCalledOperand())
          errs() << "Callee name = " << CI->getCalledOperand()->getName() << "\n";
      }

      if (Function* F = dyn_cast<Function>(CI->getCalledOperand())) {
        FunctionType* FT = CI->getFunctionType();

        auto Erasify = [&] () {
          if (InvokeInst* II = dyn_cast<InvokeInst>(CI))
            BranchInst::Create(II->getNormalDest(), CI)->setDebugLoc(CI->getDebugLoc());
          CI->eraseFromParent();
        };
        
        if (F->isIntrinsic() &&
            F->getIntrinsicID() == Intrinsic::donothing) {
          llvm_unreachable("Should not see donothing intrinsic here.");
          return true;
        }
        
        if (isSetjmp(F)) {
          if (verbose)
            errs() << "Lowering some kind of setjmp\n";
          for (Use& Arg : CI->args())
            lowerConstantOperand(Arg, CI, RawNull);
          assert(FT == F->getFunctionType());
          assert(CI->hasFnAttr(Attribute::ReturnsTwice));
          assert(Setjmps.count(CI));
          Value* ValueArg;
          if (F->getName() == "sigsetjmp") {
            assert(CI->arg_size() == 2);
            ValueArg = CI->getArgOperand(1);
          } else {
            assert(CI->arg_size() == 1);
            ValueArg = ConstantInt::get(Int32Ty, 0);
          }
          unsigned FrameIndex = Setjmps[CI];
          storeOrigin(getOrigin(CI->getDebugLoc()), CI);
          CallInst* Create = CallInst::Create(
            JmpBufCreate,
            { MyThread, ConstantInt::get(Int32Ty, static_cast<int>(getJmpBufKindForSetjmp(F))),
              ValueArg },
            "filc_jmp_buf_create", CI);
          Create->setDebugLoc(CI->getDebugLoc());
          Value* UserJmpBuf = CI->getArgOperand(0);
          storePtr(flightPtrForPayload(Create, CI), flightPtrPtr(UserJmpBuf, CI),
                   auxPtrForOperand(UserJmpBuf, CI, 0, CI).P, CI);
          CallInst* NewCI = CallInst::Create(_Setjmp, { Create }, "filc_setjmp", CI);
          recordLowerAtIndex(Create, FrameIndex, CI);
          CI->replaceAllUsesWith(NewCI);
          NewCI->setDebugLoc(CI->getDebugLoc());
          Erasify();
          return true;
        }

        if (F->getName() == "zargs" &&
            !FT->getNumParams() &&
            FT->getReturnType() == RawPtrTy) {
          assert(UsesVastartOrZargs);
          CI->replaceAllUsesWith(SnapshottedArgsPtrForZargs);
          Erasify();
          return true;
        }

        if (F->getName() == "zreturn" &&
            FT->getNumParams() == 1 &&
            FT->getReturnType() == VoidTy &&
            FT->getParamType(0) == RawPtrTy) {
          Instruction* Prepare = CallInst::Create(
            PrepareToReturnWithData, { MyThread, CI->getArgOperand(0), getOrigin(CI->getDebugLoc()) },
            "filc_prepare_to_return_with_data", CI);
          Prepare->setDebugLoc(CI->getDebugLoc());
          RetSizePhi->addIncoming(Prepare, Prepare->getParent());
          BasicBlock* OriginalB = CI->getParent();
          SplitBlock(OriginalB, CI);
          cast<BranchInst>(OriginalB->getTerminator())->setSuccessor(0, ReallyReturnB);
          Erasify();
          return true;
        }

        if ((F->getName() == "zgetlower" || F->getName() == "zgetupper") &&
            FT->getNumParams() == 1 &&
            !FT->isVarArg() &&
            FT->getParamType(0) == RawPtrTy &&
            FT->getReturnType() == RawPtrTy) {
          lowerConstantOperand(CI->getArgOperandUse(0), CI, RawNull);
          Value* Lower = flightPtrLower(CI->getArgOperand(0), CI);
          ICmpInst* NullLower = new ICmpInst(
            CI, ICmpInst::ICMP_EQ, Lower, RawNull, "filc_is_null_lower");
          NullLower->setDebugLoc(CI->getDebugLoc());
          Instruction* NotNullTerm = SplitBlockAndInsertIfElse(NullLower, CI, false);
          Value* LowerUpper;
          if (F->getName() == "zgetlower")
            LowerUpper = Lower;
          else {
            assert(F->getName() == "zgetupper");
            LowerUpper = upperForLower(Lower, NotNullTerm);
          }
          PHINode* Phi = PHINode::Create(RawPtrTy, 2, "filc_lower_upper_phi", CI);
          Phi->addIncoming(RawNull, NullLower->getParent());
          Phi->addIncoming(LowerUpper, NotNullTerm->getParent());
          CI->replaceAllUsesWith(flightPtrWithPtr(CI->getArgOperand(0), Phi, CI));
          Erasify();
          return true;
        }

        if (F->getName() == "zthread_self_id" &&
            FT->getNumParams() == 0 &&
            !FT->isVarArg() &&
            FT->getReturnType() == Int32Ty) {
          Value* TidPtr = threadTIDPtr(MyThread, CI);
          Instruction* Tid = new LoadInst(Int32Ty, TidPtr, "filc_tid", CI);
          Tid->setDebugLoc(CI->getDebugLoc());
          CI->replaceAllUsesWith(Tid);
          Erasify();
          return true;
        }

        if (F->getName() == "zthread_self" &&
            FT->getNumParams() == 0 &&
            !FT->isVarArg() &&
            FT->getReturnType() == RawPtrTy) {
          CI->replaceAllUsesWith(flightPtrForPayload(MyThread, CI));
          Erasify();
          return true;
        }

        if (F->getName() == "zthread_self_cookie" &&
            FT->getNumParams() == 0 &&
            !FT->isVarArg() &&
            FT->getReturnType() == RawPtrTy) {
          Value* CookiePtr = threadCookiePtrPtr(MyThread, CI);
          Instruction* Cookie = new LoadInst(FlightPtrTy, CookiePtr, "filc_cookie", CI);
          Cookie->setDebugLoc(CI->getDebugLoc());
          CI->replaceAllUsesWith(Cookie);
          Erasify();
          return true;
        }

        if (shouldPassThrough(F)) {
          for (Use& Arg : CI->args())
            lowerConstantOperand(Arg, CI, RawNull);
          return true;
        }
      }
    }
    
    if (isa<LandingPadInst>(I)) {
      CallInst* CI = CallInst::Create(LandingPad, { MyThread }, "filc_landing_pad", I);
      CI->setDebugLoc(I->getDebugLoc());
      SplitBlockAndInsertIfElse(CI, I, false, nullptr, nullptr, nullptr, ResumeB);
      StructType* ST = cast<StructType>(I->getType());
      assert(!ST->isOpaque());
      assert(ST->getNumElements() <= NumUnwindRegisters);
      Value* Result = UndefValue::get(toFlightType(ST));
      for (unsigned Idx = ST->getNumElements(); Idx--;) {
        Value* UnwindRegisterPtr = threadUnwindRegisterPtr(MyThread, Idx, I);
        LoadInst* RawUnwindRegister = new LoadInst(
          FlightPtrTy, UnwindRegisterPtr, "filc_load_unwind_register", I);
        (new StoreInst(FlightNull, UnwindRegisterPtr, I))->setDebugLoc(I->getDebugLoc());
        Value* UnwindRegister;
        if (ST->getElementType(Idx) == RawPtrTy)
          UnwindRegister = RawUnwindRegister;
        else {
          assert(isa<IntegerType>(ST->getElementType(Idx)));
          Instruction* Trunc = new TruncInst(
            flightPtrPtrAsInt(RawUnwindRegister, I), ST->getElementType(Idx),
            "filc_trunc_unwind_register", I);
          Trunc->setDebugLoc(I->getDebugLoc());
          UnwindRegister = Trunc;
        }
        Instruction* InsertValue = InsertValueInst::Create(
          Result, UnwindRegister, { Idx }, "filc_insert_unwind_register", I);
        InsertValue->setDebugLoc(I->getDebugLoc());
        Result = InsertValue;
      }
      I->replaceAllUsesWith(Result);
      I->removeFromParent();
      ToErase.push_back(I);
      return true;
    }

    return false;
  }
  
  // This lowers the instruction "in place", so all references to it are fixed up after this runs.
  void lowerInstruction(Instruction *I, Value* InitializationContext) {
    if (verbose)
      errs() << "Lowering: " << *I << "\n";

    if (PHINode* P = dyn_cast<PHINode>(I)) {
      assert(InitializationContext == RawNull);
      for (unsigned Index = P->getNumIncomingValues(); Index--;) {
        lowerConstantOperand(
          P->getOperandUse(Index), P->getIncomingBlock(Index)->getTerminator(), RawNull);
      }
      P->mutateType(toFlightType(P->getType()));
      return;
    }

    lowerConstantOperands(I, InitializationContext);
    
    if (AllocaInst* AI = dyn_cast<AllocaInst>(I)) {
      if (!AI->hasNUsesOrMore(1)) {
        // By this point we may have dead allocas, due to earlyLowerInstruction. Only happens for allocas
        // used as type hacks for stdfil API.
        return;
      }
      
      Type* T = AI->getAllocatedType();
      Value* Length = AI->getArraySize();
      if (Length->getType() != IntPtrTy) {
        Instruction* ZExt = new ZExtInst(Length, IntPtrTy, "filc_alloca_length_zext", AI);
        ZExt->setDebugLoc(AI->getDebugLoc());
        Length = ZExt;
      }
      Instruction* Size = BinaryOperator::Create(
        Instruction::Mul, Length, ConstantInt::get(IntPtrTy, DL.getTypeAllocSize(T)),
        "filc_alloca_size", AI);
      Size->setDebugLoc(AI->getDebugLoc());
      AI->replaceAllUsesWith(allocate(Size, DL.getABITypeAlign(T).value(), AI));
      AI->eraseFromParent();
      return;
    }

    if (LoadInst* LI = dyn_cast<LoadInst>(I)) {
      Type *T = LI->getType();
      Value* HighP = LI->getPointerOperand();
      Value* Result = loadValueRecurseAfterCheck(
        T, flightPtrPtr(HighP, LI), auxPtrForOperand(HighP, LI, 0, LI), LI->isVolatile(),
        LI->getAlign(), LI->getOrdering(), LI->getSyncScopeID(), MemoryKind::Heap, LI);
      LI->replaceAllUsesWith(Result);
      LI->eraseFromParent();
      return;
    }

    if (StoreInst* SI = dyn_cast<StoreInst>(I)) {
      Value* HighP = SI->getPointerOperand();
      storeValueRecurseAfterCheck(
        InstTypes[SI], SI->getValueOperand(), flightPtrPtr(HighP, SI),
        auxPtrForOperand(HighP, SI, 0, SI).P, SI->isVolatile(), SI->getAlign(),
        SI->getOrdering(), SI->getSyncScopeID(), MemoryKind::Heap, SI);
      SI->eraseFromParent();
      return;
    }

    if (isa<FenceInst>(I)) {
      // We don't need to do anything because it doesn't take operands.
      return;
    }

    if (AtomicCmpXchgInst* AI = dyn_cast<AtomicCmpXchgInst>(I)) {
      if (InstTypes[AI] == RawPtrTy) {
        storeOrigin(getOrigin(AI->getDebugLoc()), AI);
        Instruction* CAS = CallInst::Create(
          StrongCasPtr,
          { MyThread, AI->getPointerOperand(), ConstantInt::get(IntPtrTy, 0), AI->getCompareOperand(),
            AI->getNewValOperand() }, "filc_strong_cas_ptr", AI);
        StructType* ResultTy = StructType::get(C, { FlightPtrTy, Int1Ty });
        Instruction* Result = InsertValueInst::Create(
          UndefValue::get(ResultTy), CAS, { 0 }, "filc_cas_create_result_word", AI);
        Result->setDebugLoc(AI->getDebugLoc());
        Instruction* Equal = new ICmpInst(
          AI, ICmpInst::ICMP_EQ, flightPtrPtr(CAS, AI), flightPtrPtr(AI->getCompareOperand(), AI),
          "filc_cas_succeeded");
        Equal->setDebugLoc(AI->getDebugLoc());
        Result = InsertValueInst::Create(Result, Equal, { 1 }, "filc_cas_create_result_bit", AI);
        Result->setDebugLoc(AI->getDebugLoc());
        AI->replaceAllUsesWith(Result);
        AI->eraseFromParent();
        return;
      }
      assert(!hasPtrs(InstTypes[AI]));
      AI->getOperandUse(AtomicCmpXchgInst::getPointerOperandIndex()) =
        flightPtrPtr(AI->getPointerOperand(), AI);
      return;
    }

    if (AtomicRMWInst* AI = dyn_cast<AtomicRMWInst>(I)) {
      if (InstTypes[AI] == RawPtrTy) {
        assert(AI->getOperation() == AtomicRMWInst::Xchg);
        storeOrigin(getOrigin(AI->getDebugLoc()), AI);
        Instruction* Xchg = CallInst::Create(
          XchgPtr,
          { MyThread, AI->getPointerOperand(), ConstantInt::get(IntPtrTy, 0), AI->getValOperand() },
          "filc_xchg_ptr", AI);
        Xchg->setDebugLoc(AI->getDebugLoc());
        AI->replaceAllUsesWith(Xchg);
        AI->eraseFromParent();
        return;
      }
      AI->getOperandUse(AtomicRMWInst::getPointerOperandIndex()) =
        flightPtrPtr(AI->getPointerOperand(), AI);
      assert(!hasPtrs(InstTypes[AI]));
      return;
    }

    if (GetElementPtrInst* GI = dyn_cast<GetElementPtrInst>(I)) {
      Value* HighP = GI->getOperand(0);
      GI->getOperandUse(0) = flightPtrPtr(HighP, GI);
      assert(!GI->isInBounds()); // Should have been cleared by dropUB().
      hackRAUW(GI, [&] () { return flightPtrWithPtr(HighP, GI, GI->getNextNode()); });
      return;
    }

    if (ICmpInst* CI = dyn_cast<ICmpInst>(I)) {
      if (hasPtrs(CI->getOperand(0)->getType())) {
        CI->getOperandUse(0) = flightPtrPtr(CI->getOperand(0), CI);
        CI->getOperandUse(1) = flightPtrPtr(CI->getOperand(1), CI);
      }
      return;
    }

    if (isa<FCmpInst>(I) ||
        isa<BranchInst>(I) ||
        isa<SwitchInst>(I) ||
        isa<TruncInst>(I) ||
        isa<ZExtInst>(I) ||
        isa<SExtInst>(I) ||
        isa<FPTruncInst>(I) ||
        isa<FPExtInst>(I) ||
        isa<UIToFPInst>(I) ||
        isa<SIToFPInst>(I) ||
        isa<FPToUIInst>(I) ||
        isa<FPToSIInst>(I) ||
        isa<BinaryOperator>(I) ||
        isa<UnaryOperator>(I)) {
      // We're gucci.
      return;
    }

    if (isa<ReturnInst>(I)) {
      if (OldF->getReturnType() != VoidTy)
        ReturnPhi->addIncoming(I->getOperand(0), I->getParent());
      ReplaceInstWithInst(I, BranchInst::Create(ReturnB));
      return;
    }

    if (isa<CallBrInst>(I)) {
      llvm_unreachable("Don't support CallBr yet (and maybe never will)");
      return;
    }

    if (CallBase* CI = dyn_cast<CallBase>(I)) {
      assert(isa<CallInst>(CI) || isa<InvokeInst>(CI));

      // FIXME: It would be cool to emit a direct call to the function, if:
      // - We know who the callee is.
      // - The original called signature according to the call instruction matches the original
      //   function type of the callee.
      // - The callee is a definition in this module, so we can call the hidden function.
      //
      // The trouble with this is that to make this totally effective:
      // - We'd want to eliminate the constant lowering of the callee, so we don't end up with a call
      //   to the pizlonated_getter.
      // - We'd want to call a version of the hidden function that "just" takes the arguments, without
      //   any CC shenanigans.
      //
      // To achieve the latter requirement, I'd probably want to emit all functions as a collection of
      // three functions:
      // - Hidden function that is the actual implementation. It takes its arguments and a frame
      //   pointer. It expects the caller to set up its frame and do all argument/return checking.
      // - Hidden function that uses the Fil-C CC and sets up the frame, then calls the
      //   implementation.
      // - Hidden function that uses a direct CC and sets up the frame, then calls the implementation.
      //
      // We can rely on the implementation to get inlined into the other functions whenever either of
      // these things is true:
      // 1. The implementation is small.
      // 2. Only the Fil-C CC, or only the direct CC, version are used.
      //
      // But this risks suboptimal codegen if the implementation isn't inlined. Yuck! The trick is that
      // we want the Fil-C CC shenanigans to happen with the frame already set up, so we can't simply
      // have the Fil-C CC version wrap the direct version.
      
      if (CI->isInlineAsm()) {
        assert(isa<CallInst>(CI));
        std::string str;
        raw_string_ostream outs(str);
        outs << "Cannot handle inline asm: " << *CI;
        CallInst::Create(Error, { getString(str), getOrigin(I->getDebugLoc()) }, "", I)
          ->setDebugLoc(I->getDebugLoc());
        if (I->getType() != VoidTy) {
          // We need to produce something to RAUW the call with, but it cannot be a constant, since
          // that would upset lowerConstant.
          Type* LowT = toFlightType(I->getType());
          LoadInst* LI = new LoadInst(LowT, RawNull, "filc_fake_load", I);
          LI->setDebugLoc(I->getDebugLoc());
          CI->replaceAllUsesWith(LI);
        }
        CI->eraseFromParent();
        return;
      }

      if (verbose)
        errs() << "Dealing with called operand: " << *CI->getCalledOperand() << "\n";

      if (Function* F = dyn_cast<Function>(CI->getCalledOperand()))
        assert(!shouldPassThrough(F));

      FunctionType *FT = CI->getFunctionType();
      Value* ArgSize;
      if (CI->arg_size()) {
        assert(InstTypeVectors.count(CI));
        std::vector<Type*> ArgTypes = InstTypeVectors[CI];
        StructType* ArgsType = argsType(ArgTypes);
        Value* Args = UndefValue::get(toFlightType(ArgsType));
        for (size_t Index = CI->arg_size(); Index--;) {
          Type* OriginalT = ArgTypes[Index];
          Type* CanonicalT = ArgsType->getElementType(Index);
          Instruction* Insert = InsertValueInst::Create(
            Args, castToArg(CI->getArgOperand(Index), toFlightType(OriginalT),
                            toFlightType(CanonicalT), CI),
            static_cast<unsigned>(Index), "filc_insert_arg", CI);
          Insert->setDebugLoc(CI->getDebugLoc());
          Args = Insert;
        }
        ArgSize = storeCC(ArgsType, Args, CI, CI->getDebugLoc());
      } else
        ArgSize = ConstantInt::get(IntPtrTy, 0);

      bool CanCatch;
      LandingPadInst* LPI;
      if (isa<CallInst>(CI)) {
        CanCatch = !OldF->doesNotThrow();
        LPI = nullptr;
      } else {
        CanCatch = true;
        assert(LPIs.count(cast<InvokeInst>(CI)));
        LPI = LPIs[cast<InvokeInst>(CI)];
      }

      storeOrigin(getOrigin(CI->getDebugLoc(), CanCatch, LPI), CI);

      Value* CalledLower = flightPtrLower(CI->getCalledOperand(), CI);
      ICmpInst* NullLower = new ICmpInst(
        CI, ICmpInst::ICMP_EQ, CalledLower, RawNull, "filc_null_called_lower");
      NullLower->setDebugLoc(CI->getDebugLoc());
      Instruction* NewBlockTerm = SplitBlockAndInsertIfThen(expectFalse(NullLower, CI), CI, true);
      BasicBlock* NewBlock = NewBlockTerm->getParent();
      CallInst::Create(
        CheckFunctionCallFail, { CI->getCalledOperand() }, "", NewBlockTerm)
        ->setDebugLoc(CI->getDebugLoc());
      ICmpInst* AtAuxPtr = new ICmpInst(
        CI, ICmpInst::ICMP_EQ, flightPtrPtr(CI->getCalledOperand(), CI),
        auxPtrForLower(CalledLower, CI), "filc_call_at_lower");
      AtAuxPtr->setDebugLoc(CI->getDebugLoc());
      SplitBlockAndInsertIfElse(
        expectTrue(AtAuxPtr, CI), CI, false, nullptr, nullptr, nullptr, NewBlock);
      BinaryOperator* Masked = BinaryOperator::Create(
        Instruction::And, flagsForLower(CalledLower, CI),
        ConstantInt::get(IntPtrTy, SpecialTypeMask << ObjectFlagsSpecialShift),
        "filc_call_mask_special_type", CI);
      Masked->setDebugLoc(CI->getDebugLoc());
      ICmpInst* IsFunction = new ICmpInst(
        CI, ICmpInst::ICMP_EQ, Masked,
        ConstantInt::get(IntPtrTy, SpecialTypeFunction << ObjectFlagsSpecialShift),
        "filc_call_is_function");
      IsFunction->setDebugLoc(CI->getDebugLoc());
      SplitBlockAndInsertIfElse(
        expectTrue(IsFunction, CI), CI, false, nullptr, nullptr, nullptr, NewBlock);

      assert(!CI->hasOperandBundles());
      CallInst* TheCall = CallInst::Create(
        PizlonatedFuncTy, flightPtrPtr(CI->getCalledOperand(), CI), { MyThread, ArgSize },
        "filc_call", CI);
      TheCall->setDebugLoc(CI->getDebugLoc());
      Instruction* HasException = ExtractValueInst::Create(
        Int1Ty, TheCall, { 0 }, "filc_has_exception", CI);
      HasException->setDebugLoc(CI->getDebugLoc());
      Instruction* RetSize = ExtractValueInst::Create(
        IntPtrTy, TheCall, { 1 }, "filc_ret_size", CI);
      RetSize->setDebugLoc(CI->getDebugLoc());

      if (isa<CallInst>(CI) && CanCatch) {
        SplitBlockAndInsertIfThen(
          expectFalse(HasException, CI), CI, false, nullptr, nullptr, nullptr, ResumeB);
      } else if (InvokeInst* II = dyn_cast<InvokeInst>(CI)) {
        BranchInst::Create(
          II->getUnwindDest(), II->getNormalDest(), expectFalse(HasException, II), II)
          ->setDebugLoc(II->getDebugLoc());
      }
      
      Instruction* PostInsertionPt;
      if (isa<CallInst>(CI))
        PostInsertionPt = CI;
      else
        PostInsertionPt = &*cast<InvokeInst>(CI)->getNormalDest()->getFirstInsertionPt();

      if (FT->getReturnType() != VoidTy) {
        CI->replaceAllUsesWith(
          loadCC(FT->getReturnType(), RetSize, CCRetsCheckFailure, PostInsertionPt,
                 CI->getDebugLoc()));
      }

      CI->eraseFromParent();
      return;
    }

    if (VAArgInst* VI = dyn_cast<VAArgInst>(I)) {
      Type* T = VI->getType();
      Type* CanonicalT = argType(T);
      size_t Size = DL.getTypeAllocSize(CanonicalT);
      size_t Alignment = DL.getABITypeAlign(CanonicalT).value();
      storeOrigin(getOrigin(VI->getDebugLoc()), VI);
      // FIXME: We could optimize this by calling GetNextBytesForVAArg (sans the Ptr part) in cases
      // where we're loading a non-ptr type.
      CallInst* Call = CallInst::Create(
        GetNextPtrBytesForVAArg,
        { VI->getPointerOperand(), ConstantInt::get(IntPtrTy, Size),
          ConstantInt::get(IntPtrTy, Alignment) },
        "filc_va_arg", VI);
      Call->setDebugLoc(VI->getDebugLoc());
      Instruction* P = ExtractValueInst::Create(RawPtrTy, Call, { 0 }, "filc_ptr_pair_raw_ptr", VI);
      P->setDebugLoc(VI->getDebugLoc());
      Instruction* AuxP = ExtractValueInst::Create(
        RawPtrTy, Call, { 1 }, "filc_ptr_pair_aux_ptr", VI);
      P->setDebugLoc(VI->getDebugLoc());
      Value* Load = loadValueRecurseAfterCheck(
        CanonicalT, P, AuxP, AuxP, false, DL.getABITypeAlign(CanonicalT),
        AtomicOrdering::NotAtomic, SyncScope::System, MemoryKind::Heap, VI);
      VI->replaceAllUsesWith(castFromArg(Load, toFlightType(T), VI));
      VI->eraseFromParent();
      return;
    }

    if (isa<ExtractElementInst>(I) ||
        isa<InsertElementInst>(I) ||
        isa<ShuffleVectorInst>(I) ||
        isa<ExtractValueInst>(I) ||
        isa<InsertValueInst>(I) ||
        isa<SelectInst>(I) ||
        isa<FreezeInst>(I)) {
      I->mutateType(toFlightType(I->getType()));
      return;
    }

    if (isa<LandingPadInst>(I)) {
      llvm_unreachable("Shouldn't see LandingPad because it should have been handled by "
                       "earlyLowerInstruction.");
      return;
    }

    if (isa<ResumeInst>(I)) {
      // NOTE: This function call is only necessary for checks. If our unwind machinery is working
      // correctly, then these checks should never fire. But I'm paranoid.
      CallInst::Create(ResumeUnwind, { MyThread, getOrigin(I->getDebugLoc()) }, "", I);
      BranchInst* BI = BranchInst::Create(ResumeB, I);
      BI->setDebugLoc(I->getDebugLoc());
      I->eraseFromParent();
      return;
    }

    if (isa<IndirectBrInst>(I)) {
      llvm_unreachable("Don't support IndirectBr yet (and maybe never will)");
      return;
    }

    if (isa<CatchSwitchInst>(I)) {
      llvm_unreachable("Don't support CatchSwitch yet");
      return;
    }

    if (isa<CleanupPadInst>(I)) {
      llvm_unreachable("Don't support CleanupPad yet");
      return;
    }

    if (isa<CatchPadInst>(I)) {
      llvm_unreachable("Don't support CatchPad yet");
      return;
    }

    if (isa<CatchReturnInst>(I)) {
      llvm_unreachable("Don't support CatchReturn yet");
      return;
    }

    if (isa<CleanupReturnInst>(I)) {
      llvm_unreachable("Don't support CleanupReturn yet");
      return;
    }

    if (isa<UnreachableInst>(I)) {
      CallInst::Create(
        Error, { getString("llvm unreachable instruction"), getOrigin(I->getDebugLoc()) }, "", I)
        ->setDebugLoc(I->getDebugLoc());
      return;
    }

    if (isa<IntToPtrInst>(I)) {
      hackRAUW(I, [&] () { return badFlightPtr(I, I->getNextNode()); });
      return;
    }

    if (isa<PtrToIntInst>(I)) {
      I->getOperandUse(0) = flightPtrPtr(I->getOperand(0), I);
      return;
    }

    if (isa<BitCastInst>(I)) {
      if (hasPtrs(I->getType())) {
        assert(hasPtrs(I->getOperand(0)->getType()));
        assert(I->getType() == RawPtrTy || I->getType() == FlightPtrTy);
        assert(I->getOperand(0)->getType() == RawPtrTy ||
               I->getOperand(0)->getType() == FlightPtrTy);
        I->replaceAllUsesWith(I->getOperand(0));
        I->eraseFromParent();
      } else
        assert(!hasPtrs(I->getOperand(0)->getType()));
      return;
    }

    if (isa<AddrSpaceCastInst>(I)) {
      if (hasPtrs(I->getType())) {
        if (hasPtrs(I->getOperand(0)->getType())) {
          I->replaceAllUsesWith(I->getOperand(0));
          I->eraseFromParent();
        } else
          hackRAUW(I, [&] () { return badFlightPtr(I, I->getNextNode()); });
      } else if (hasPtrs(I->getOperand(0)->getType()))
        I->getOperandUse(0) = flightPtrPtr(I->getOperand(0), I);
      return;
    }

    errs() << "Unrecognized instruction: " << *I << "\n";
    llvm_unreachable("Unknown instruction");
  }

  bool isSetjmp(Function* F) {
    return (F->getName() == "setjmp" ||
            F->getName() == "_setjmp" ||
            F->getName() == "sigsetjmp");
  }

  JmpBufKind getJmpBufKindForSetjmp(Function* F) {
    if (F->getName() == "setjmp")
      return JmpBufKind::setjmp;
    if (F->getName() == "_setjmp")
      return JmpBufKind::_setjmp;
    if (F->getName() == "sigsetjmp")
      return JmpBufKind::sigsetjmp;
    llvm_unreachable("Bad setjmp kind");
    return JmpBufKind::setjmp;
  }

  bool shouldPassThrough(Function* F) {
    return (F->getName() == "__divdc3" ||
            F->getName() == "__muldc3" ||
            F->getName() == "__divsc3" ||
            F->getName() == "__mulsc3" ||
            F->getName() == "__mulxc3" ||
            isSetjmp(F));
  }

  bool shouldPassThrough(GlobalVariable* G) {
    return (G->getName() == "llvm.global_ctors" ||
            G->getName() == "llvm.global_dtors" ||
            G->getName() == "llvm.used" ||
            G->getName() == "llvm.compiler.used");
  }

  bool shouldPassThrough(GlobalValue* G) {
    if (Function* F = dyn_cast<Function>(G))
      return shouldPassThrough(F);
    if (GlobalVariable* V = dyn_cast<GlobalVariable>(G))
      return shouldPassThrough(V);
    return false;
  }

  void stackOverflowCheck(Instruction* InsertBefore) {
    assert(MyThread);
    Value* GEP = threadStackLimitPtr(MyThread, InsertBefore);
    CallInst* CI = CallInst::Create(StackCheckAsm, { GEP }, "", InsertBefore);
    CI->addParamAttr(0, Attribute::get(C, Attribute::ElementType, RawPtrTy));
  }
  
  void undefineAvailableExternally() {
    for (GlobalVariable& G : M.globals()) {
      if (G.getLinkage() == GlobalValue::AvailableExternallyLinkage) {
        G.setInitializer(nullptr);
        G.setLinkage(GlobalValue::ExternalLinkage);
      }
    }
    for (Function& F : M.functions()) {
      if (F.getLinkage() == GlobalValue::AvailableExternallyLinkage) {
        F.deleteBody();
        F.setIsMaterializable(false);
      }
    }
    /* FIXME: Should be able to do something for these. */
    for (GlobalAlias& G : M.aliases())
      assert(G.getLinkage() != GlobalValue::AvailableExternallyLinkage);
  }

  void expandConstantExprOperands(Instruction* I) {
    for (unsigned Index = I->getNumOperands(); Index--;) {
      Instruction* InsertBefore;
      if (PHINode* P = dyn_cast<PHINode>(I))
        InsertBefore = P->getIncomingBlock(Index)->getTerminator();
      else
        InsertBefore = I;
      
      Use& U = I->getOperandUse(Index);
      if (ConstantExpr* CE = dyn_cast<ConstantExpr>(U)) {
        Instruction* NewI = CE->getAsInstruction(InsertBefore);
        expandConstantExprOperands(NewI);
        U = NewI;
      }
    }
  }

  void expandConstantExprs() {
    for (Function& F : M.functions()) {
      if (F.isDeclaration())
        continue;
      
      for (BasicBlock& BB : F) {
        std::vector<Instruction*> Insts;
        for (Instruction& I : BB)
          Insts.push_back(&I);
        for (Instruction* I : Insts)
          expandConstantExprOperands(I);
      }
    }
  }

  void inferPointerAsIntLaunderingForFunction(Function& F) {
    // If an inttoptr's inputs unambiguously lead to a single ptrtoint, then we can take that
    // ptrtoint's capability.
    //
    // If an inttoptr's inputs unambigiously lead to a phi and that phi knows how to account for the
    // capability of each of its inputs, then we can construct a phi to collect those capabilities. In
    // fact, we can even construct such a phi if some of the inputs don't have capabilities.
    //
    // This suggests a simple kind of fixpoint where for each instruction, we either:
    //
    // - Don't know whether we can deduce the capability for that instruction because we haven't
    //   considered it yet, or we know that there are zero possible capabilities (BOTTOM).
    // - Know that there is a capability for that instruction.
    // - Know that there is a conflict of capabilities for that instruction (TOP).
    //
    // It's cool how easy it is to write this as an abstract interpreter.

    std::unordered_map<Instruction*, InferredCapability> InferredCapabilities;

    std::vector<Instruction*> AllInsts;
    for (BasicBlock& BB : F) {
      for (Instruction& I : BB)
        AllInsts.push_back(&I);
    }
    std::vector<Instruction*> InferInsts;
    for (Instruction* I : AllInsts) {
      if (PtrToIntInst* PtrToInt = dyn_cast<PtrToIntInst>(I)) {
        InferredCapabilities[I] = InferredCapability(
          CapabilityInferenceState::Definite, PtrToInt->getPointerOperand());
        continue;
      }
      if (!I->getType()->isIntegerTy())
        continue;
      // Maybe we should cause all casts to lose the capability, but it's not obvious. For sure,
      // truncing is suspect.
      if (isa<CallBase>(I) || isa<LoadInst>(I) || isa<AtomicCmpXchgInst>(I) ||
          isa<AtomicRMWInst>(I) || isa<CmpInst>(I) || isa<VAArgInst>(I) ||
          isa<ExtractElementInst>(I) || isa<ExtractValueInst>(I) || isa<LandingPadInst>(I) ||
          isa<FPToUIInst>(I) || isa<FPToSIInst>(I)) {
        InferredCapabilities[I] = InferredCapability(CapabilityInferenceState::Bottom, nullptr);
        continue;
      }
      InferInsts.push_back(I);
    }

    bool Changed = true;
    while (Changed) {
      Changed = false;
      for (Instruction* I : InferInsts) {
        for (Value* V : I->operands()) {
          if (Instruction* I2 = dyn_cast<Instruction>(V)) {
            InferredCapability I2Cap = InferredCapabilities[I2];
            if ((isa<PHINode>(I) || isa<SelectInst>(I))
                && I2Cap.State != CapabilityInferenceState::Bottom) {
              InferredCapability ICap = InferredCapabilities[I];
              assert(ICap.State == CapabilityInferenceState::Bottom ||
                     ICap.State == CapabilityInferenceState::Definite);
              Value* Capability = ICap.Capability;
              if (!Capability) {
                assert(ICap.State == CapabilityInferenceState::Bottom);
                if (PHINode* Phi = dyn_cast<PHINode>(I)) {
                  PHINode* NewPhi = PHINode::Create(
                    RawPtrTy, Phi->getNumOperands(), "filc_capability_phi", Phi);
                  NewPhi->setDebugLoc(Phi->getDebugLoc());
                  Capability = NewPhi;
                } else {
                  SelectInst* Select = dyn_cast<SelectInst>(I);
                  assert(Select);
                  SelectInst* NewSelect = SelectInst::Create(
                    Select->getCondition(), RawNull, RawNull, "filc_capability_select", Select);
                  Capability = NewSelect;
                }
              }
              assert(Capability);
              I2Cap = InferredCapability(CapabilityInferenceState::Definite, Capability);
            }
            Changed |= InferredCapabilities[I].merge(I2Cap);
          }
        }
      }
    }
    
    auto CapabilityOf = [&] (Value* Incoming) -> Value* {
      Instruction* I2 = dyn_cast<Instruction>(Incoming);
      if (!I2)
        return RawNull;
      InferredCapability I2Cap = InferredCapabilities[I2];
      if (I2Cap.Capability) {
        assert(I2Cap.State == CapabilityInferenceState::Definite);
        assert(I2Cap.Capability->getType() == RawPtrTy);
        return I2Cap.Capability;
      }
      return RawNull;
    };
      
    for (auto& Pair : InferredCapabilities) {
      Instruction* I = Pair.first;
      InferredCapability ICap = Pair.second;
      if (!ICap.Capability)
        continue;

      if (PHINode* Phi = dyn_cast<PHINode>(I)) {
        PHINode* NewPhi = cast<PHINode>(ICap.Capability);
        for (unsigned Index = Phi->getNumIncomingValues(); Index--;) {
          NewPhi->addIncoming(CapabilityOf(Phi->getIncomingValue(Index)),
                              Phi->getIncomingBlock(Index));
        }
      }
      if (SelectInst* Select = dyn_cast<SelectInst>(I)) {
        SelectInst* NewSelect = cast<SelectInst>(ICap.Capability);
        NewSelect->setTrueValue(CapabilityOf(Select->getTrueValue()));
        NewSelect->setFalseValue(CapabilityOf(Select->getFalseValue()));
      }
    }

    for (Instruction* I : AllInsts) {
      IntToPtrInst* IntToPtr = dyn_cast<IntToPtrInst>(I);
      if (!IntToPtr)
        continue;

      Value* Capability = CapabilityOf(IntToPtr->getOperand(0));
      if (Capability == RawNull)
        continue;
      
      Instruction* PtrToInt = new PtrToIntInst(Capability, IntPtrTy, "filc_ptr_to_int", I);
      PtrToInt->setDebugLoc(I->getDebugLoc());
      Instruction* Neg = BinaryOperator::Create(
        Instruction::Sub, ConstantInt::get(IntPtrTy, 0), PtrToInt, "filc_ptr_neg", I);
      Neg->setDebugLoc(I->getDebugLoc());
      Instruction* Sub = GetElementPtrInst::Create(Int8Ty, Capability, { Neg }, "filc_ptr_sub", I);
      Sub->setDebugLoc(I->getDebugLoc());
      Instruction* Add = GetElementPtrInst::Create(Int8Ty, Sub, { IntToPtr->getOperand(0) },
                                                   "filc_ptr_add", I);
      Add->setDebugLoc(I->getDebugLoc());
      IntToPtr->replaceAllUsesWith(Add);
      IntToPtr->eraseFromParent();
    }
  }

  void inferPointerAsIntLaundering() {
    for (Function& F : M.functions()) {
      if (!F.isDeclaration())
        inferPointerAsIntLaunderingForFunction(F);
    }
  }

  void lowerThreadLocalOperands(Instruction* I) {
    IntrinsicInst* II = dyn_cast<IntrinsicInst>(I);
    if (II && II->getIntrinsicID() == Intrinsic::threadlocal_address)
      return;
    
    for (unsigned Index = I->getNumOperands(); Index--;) {
      Instruction* InsertBefore;
      if (PHINode* P = dyn_cast<PHINode>(I))
        InsertBefore = P->getIncomingBlock(Index)->getTerminator();
      else
        InsertBefore = I;
      
      Use& U = I->getOperandUse(Index);
      if (GlobalVariable* G = dyn_cast<GlobalVariable>(U)) {
        if (G->isThreadLocal()) {
          CallInst* CI = CallInst::Create(
            ThreadlocalAddress, { U }, "filc_threadlocal_address", InsertBefore);
          CI->setDebugLoc(I->getDebugLoc());
          U = CI;
        }
        continue;
      }

      assert(!isa<ConstantExpr>(U));
    }
  }

  void lowerThreadLocals() {
    // All uses of threadlocals except threadlocal_address need to go through threadlocal_address.
    for (Function& F : M.functions()) {
      if (F.isDeclaration())
        continue;
      
      for (BasicBlock& BB : F) {
        std::vector<Instruction*> Insts;
        for (Instruction& I : BB)
          Insts.push_back(&I);
        for (Instruction* I : Insts)
          lowerThreadLocalOperands(I);
      }

      std::unordered_map<Value*, std::vector<Instruction*>> ThreadlocalAddressCalls;
      for (BasicBlock& BB : F) {
        for (Instruction& I : BB) {
          IntrinsicInst* II = dyn_cast<IntrinsicInst>(&I);
          if (!II || II->getIntrinsicID() != Intrinsic::threadlocal_address)
            continue;
          
          ThreadlocalAddressCalls[II->getArgOperand(0)].push_back(II);
        }
      }
      
      simpleCSE(F, ThreadlocalAddressCalls);
    }
    
    // - Lower all threadlocal variables to pthread_key_t, which is an i32, and a function that allocates
    //   the initial "value" (i.e. object containing the initial value). I guess that function can call
    //   malloc and just store the value.
    // - For all threadlocal variables, add a global ctor that pthread_key_create's them.
    // - For all threadlocal variables, add a global dtor that pthread_key_delete's them.
    // - Lower all llvm.threadlocal.address calls to pthread_getspecific_with_default_np, which passes
    //   the key and the default value function.
    //
    // Since this runs before any Fil-C transformations, the resulting code will be made memory safe by
    // those later transformations.
    //
    // Possible problem: if I do it this way then I'm almost certainly interacting badly with thread local
    // destruction. In particular: you'll be able to observe the __thread variable's value being reset
    // if you access it from one of your own destructors. Except! It appears that my implementation of
    // thread locals in musl doesn't behave that way - if a threadlocal has no destructor then we never
    // NULL it.

    std::vector<GlobalVariable*> ThreadLocals;

    for (GlobalVariable& G : M.globals()) {
      if (G.isThreadLocal())
        ThreadLocals.push_back(&G);
    }
    for (Function& F : M.functions())
      assert(!F.isThreadLocal());
    for (GlobalAlias& G : M.aliases())
      assert(!G.isThreadLocal()); // We could probably support this.
    assert(M.ifunc_empty());

    if (ThreadLocals.empty())
      return;

    std::unordered_map<GlobalVariable*, GlobalVariable*> ThreadLocalKeyMap;
    std::unordered_map<GlobalVariable*, Function*> ThreadLocalInitializerMap;

    FunctionType* InitializerTy = FunctionType::get(RawPtrTy, false);
    FunctionCallee Malloc = M.getOrInsertFunction("malloc", RawPtrTy, IntPtrTy);
    FunctionCallee PthreadKeyCreate = M.getOrInsertFunction("pthread_key_create", Int32Ty, RawPtrTy, RawPtrTy);
    FunctionCallee PthreadGetspecificWithDefaultNP = M.getOrInsertFunction("pthread_getspecific_with_default_np", RawPtrTy, Int32Ty, RawPtrTy);

    for (GlobalVariable* G : ThreadLocals) {
      GlobalVariable* Key = new GlobalVariable(
        M, Int32Ty, false, G->getLinkage(),
        !G->isDeclaration() ? ConstantInt::get(Int32Ty, 0) : nullptr,
        "__piztk_" + G->getName());
      ThreadLocalKeyMap[G] = Key;

      Function* Initializer = Function::Create(
        InitializerTy, G->getLinkage(), 0, "__pizti_" + G->getName(), &M);
      ThreadLocalInitializerMap[G] = Initializer;

      if (!G->isDeclaration()) {
        BasicBlock* RootBB = BasicBlock::Create(C, "filc_thread_local_initializer_root", Initializer);
        Value* Result = CallInst::Create(
          Malloc,
          { ConstantInt::get(IntPtrTy, DLBefore.getTypeAllocSize(G->getInitializer()->getType())) },
          "filc_thread_local_allocate", RootBB);
        new StoreInst(G->getInitializer(), Result, RootBB);
        ReturnInst::Create(C, Result, RootBB);
      }
    }

    Function* Ctor = Function::Create(
      CtorDtorTy, GlobalValue::InternalLinkage, 0, "filc_threadlocal_ctor", &M);
    BasicBlock* RootBB = BasicBlock::Create(C, "filc_threadlocal_ctor_root", Ctor);
    for (GlobalVariable* G : ThreadLocals) {
      GlobalVariable* Key = ThreadLocalKeyMap[G];
      assert(Key);
      CallInst::Create(PthreadKeyCreate, { Key, RawNull }, "", RootBB);
    }
    ReturnInst::Create(C, RootBB);
    appendToGlobalCtors(M, Ctor, 0, RawNull);

    // FIXME: We aren't currently registering anything to clean up the threadlocals. Oh well?

    for (Function& F : M.functions()) {
      for (BasicBlock& BB : F) {
        std::vector<Instruction*> Insts;
        for (Instruction& I : BB)
          Insts.push_back(&I);
        for (Instruction* I : Insts) {
          IntrinsicInst* II = dyn_cast<IntrinsicInst>(I);
          if (!II)
            continue;

          if (II->getIntrinsicID() != Intrinsic::threadlocal_address)
            continue;

          GlobalVariable* OldKey = cast<GlobalVariable>(II->getArgOperand(0));
          assert(OldKey);
          GlobalVariable* Key = ThreadLocalKeyMap[OldKey];
          Function* Initializer = ThreadLocalInitializerMap[OldKey];
          assert(Key);
          assert(Initializer);

          Instruction* KeyValue = new LoadInst(Int32Ty, Key, "filc_threadlocal_key_load", II);
          KeyValue->setDebugLoc(II->getDebugLoc());
          Instruction* Getspecific = CallInst::Create(
            PthreadGetspecificWithDefaultNP, { KeyValue, Initializer }, "filc_getspecific", II);
          Getspecific->setDebugLoc(II->getDebugLoc());
          II->replaceAllUsesWith(Getspecific);
          II->eraseFromParent();
        }
      }
    }

    for (GlobalVariable* G : ThreadLocals) {
      G->replaceAllUsesWith(RawNull);
      G->eraseFromParent();
    }
  }

  void makeEHDatas() {
    std::vector<LandingPadInst*> LPIs;
    
    for (Function& F : M.functions()) {
      for (BasicBlock& BB : F) {
        LandingPadInst* LPI = dyn_cast<LandingPadInst>(BB.getFirstNonPHI());
        if (LPI)
          LPIs.push_back(LPI);
      }
    }

    if (LPIs.empty())
      return;
    
    std::vector<Constant*> LowTypesAndFilters;
    unsigned NumTypes = 0;
    unsigned NumFilters = 0;
    std::unordered_map<Constant*, int> TypeOrFilterToAction;

    for (LandingPadInst* LPI : LPIs) {
      for (unsigned Idx = LPI->getNumClauses(); Idx--;) {
        if (!LPI->isCatch(Idx))
          continue;

        if (TypeOrFilterToAction.count(LPI->getClause(Idx)))
          continue;

        int EHTypeID = NumTypes++ + 1;
        TypeOrFilterToAction[LPI->getClause(Idx)] = EHTypeID;
        EHTypeIDs[LPI->getClause(Idx)] = EHTypeID;
        LowTypesAndFilters.push_back(LPI->getClause(Idx));
      }
    }

    for (LandingPadInst* LPI : LPIs) {
      for (unsigned ClauseIdx = LPI->getNumClauses(); ClauseIdx--;) {
        if (!LPI->isFilter(ClauseIdx))
          continue;

        Constant* C = LPI->getClause(ClauseIdx);
        if (TypeOrFilterToAction.count(C))
          continue;

        StructType* FilterTy;
        Constant* FilterCS;
        if (cast<ArrayType>(C->getType())->getNumElements()) {
          FilterTy = StructType::get(this->C, { Int32Ty, C->getType() });
          FilterCS = ConstantStruct::get(
            FilterTy,
            { ConstantInt::get(Int32Ty, cast<ArrayType>(C->getType())->getNumElements()), C });
        } else {
          FilterTy = StructType::get(this->C, ArrayRef<Type*>(Int32Ty));
          FilterCS = ConstantStruct::get(FilterTy, ConstantInt::get(Int32Ty, 0));
        }
        Constant* LowC = new GlobalVariable(
          M, FilterTy, true, GlobalValue::PrivateLinkage, FilterCS, "filc_eh_filter");
        TypeOrFilterToAction[C] = -(NumFilters++ + 1);
        LowTypesAndFilters.push_back(LowC);
      }
    }

    Constant* TypeTableC;
    if (LowTypesAndFilters.empty())
      TypeTableC = RawNull;
    else {
      ArrayType* TypeTableArrayTy = ArrayType::get(RawPtrTy, LowTypesAndFilters.size());
      StructType* TypeTableTy = StructType::get(C, { Int32Ty, TypeTableArrayTy });
      Constant* TypeTableCS = ConstantStruct::get(
        TypeTableTy,
        { ConstantInt::get(Int32Ty, NumTypes),
          ConstantArray::get(TypeTableArrayTy, LowTypesAndFilters) });
      TypeTableC = new GlobalVariable(
        M, TypeTableTy, true, GlobalValue::PrivateLinkage, TypeTableCS, "filc_type_table");
    }

    for (LandingPadInst* LPI : LPIs) {
      if (EHDatas.count(EHDataKey(LPI)))
        continue;

      std::vector<int> Actions;

      for (unsigned Idx = 0; Idx < LPI->getNumClauses(); ++Idx) {
        assert(TypeOrFilterToAction.count(LPI->getClause(Idx)));
        Actions.push_back(TypeOrFilterToAction[LPI->getClause(Idx)]);
      }
      
      if (LPI->isCleanup())
        Actions.push_back(0);

      StructType* EHDataTy = StructType::get(
        C, { RawPtrTy, Int32Ty, ArrayType::get(Int32Ty, Actions.size()) });
      Constant* EHDataCS = ConstantStruct::get(
        EHDataTy,
        { TypeTableC, ConstantInt::get(Int32Ty, Actions.size()), ConstantDataArray::get(C, Actions) });
      EHDatas[EHDataKey(LPI)] = new GlobalVariable(
        M, EHDataTy, true, GlobalValue::PrivateLinkage, EHDataCS, "filc_eh_data");
      if (verbose)
        errs() << "For " << *LPI << " created eh data: " << *EHDatas[EHDataKey(LPI)] << "\n";
    }
  }

  GlobalValue* getGlobal(StringRef Name) {
    if (GlobalVariable* GV = M.getNamedGlobal(Name))
      return GV;
    if (GlobalAlias* GA = M.getNamedAlias(Name))
      return GA;
    if (Function* F = M.getFunction(Name))
      return F;
    return nullptr;
  }

  void compileModuleAsm() {
    MATokenizer MAT(M.getModuleInlineAsm());

    for (;;) {
      MAToken Tok = MAT.getNext();
      if (Tok.Kind == MATokenKind::None)
        break;
      if (Tok.Kind == MATokenKind::Error) {
        errs() << "Cannot parse module asm: " << Tok.Str << "\n";
        llvm_unreachable("Error parsing module asm");
      }
      if (Tok.Kind == MATokenKind::EndLine)
        continue;
      if (Tok.Kind == MATokenKind::Directive) {
        if (Tok.Str == ".filc_weak" ||
            Tok.Str == ".filc_globl") {
          std::string Name = MAT.getNextSpecific(MATokenKind::Identifier).Str;
          MAT.getNextSpecific(MATokenKind::EndLine);
          GlobalValue* GV = getGlobal(Name);
          if (!GV) {
            errs() << "Cannot do " << Tok.Str << " to " << Name << " because it doesn't exist.\n";
            llvm_unreachable("Error interpreting module asm");
          }
          if (Tok.Str == ".filc_weak")
            GV->setLinkage(GlobalValue::WeakAnyLinkage);
          else {
            assert(Tok.Str == ".filc_globl");
            GV->setLinkage(GlobalValue::ExternalLinkage);
          }
          continue;
        }
        if (Tok.Str == ".filc_weak_alias" ||
            Tok.Str == ".filc_alias") {
          std::string OldName = MAT.getNextSpecific(MATokenKind::Identifier).Str;
          MAT.getNextSpecific(MATokenKind::Comma);
          std::string NewName = MAT.getNextSpecific(MATokenKind::Identifier).Str;
          MAT.getNextSpecific(MATokenKind::EndLine);
          GlobalValue* GV = getGlobal(OldName);
          Type* T;
          GlobalValue* ExistingGV = getGlobal(NewName);
          if (ExistingGV) {
            T = ExistingGV->getValueType();
            assert(ExistingGV->isDeclaration());
            assert(!Dummy->getNumUses());
            if (!GV) {
              if (isa<Function>(ExistingGV)) {
                assert(isa<FunctionType>(T));
                GV = Function::Create(
                  cast<FunctionType>(T), GlobalValue::ExternalLinkage, OldName, &M);
              } else {
                assert(isa<GlobalVariable>(ExistingGV));
                GV = new GlobalVariable(M, T, false, GlobalValue::ExternalLinkage, nullptr, OldName);
              }
            }
            ExistingGV->replaceAllUsesWith(Dummy);
            ExistingGV->eraseFromParent();
          } else if (GV)
            T = GV->getValueType();
          else {
            T = Int8Ty;
            GV = new GlobalVariable(M, Int8Ty, false, GlobalValue::ExternalLinkage, nullptr, OldName);
          }
          GlobalValue::LinkageTypes Linkage;
          if (Tok.Str == ".filc_weak_alias")
            Linkage = GlobalValue::WeakAnyLinkage;
          else {
            assert(Tok.Str == ".filc_alias");
            Linkage = GlobalValue::ExternalLinkage;
          }
          GlobalAlias* NewGA = GlobalAlias::create(T, GV->getAddressSpace(), Linkage, NewName, GV, &M);
          if (ExistingGV)
            Dummy->replaceAllUsesWith(NewGA);
          continue;
        }
        errs() << "Invalid directive: " << Tok.Str << "\n";
        llvm_unreachable("Error parsing module asm");
      }
      errs() << "Unexpected token: " << Tok.Str << "\n";
      llvm_unreachable("Error parsing module asm");
    }

    M.setModuleInlineAsm("");
  }

  void removeIrrelevantIntrinsics() {
    for (Function& F : M) {
      if (F.isDeclaration())
        continue;
      for (BasicBlock& BB : F) {
        std::vector<Instruction*> ToErase;
        for (Instruction& I : BB) {
          CallBase* CI = dyn_cast<CallBase>(&I);
          if (!CI)
            continue;
          Function* Callee = dyn_cast<Function>(CI->getCalledOperand());
          if (!Callee || !Callee->isIntrinsic())
            continue;
          bool ShouldErase = false;
          switch (Callee->getIntrinsicID()) {
          case Intrinsic::lifetime_start:
          case Intrinsic::lifetime_end:
          case Intrinsic::stackrestore:
          case Intrinsic::assume:
          case Intrinsic::dbg_declare:
          case Intrinsic::dbg_value:
          case Intrinsic::dbg_assign:
          case Intrinsic::dbg_label:
          case Intrinsic::donothing:
          case Intrinsic::experimental_noalias_scope_decl:
          case Intrinsic::invariant_start:
          case Intrinsic::invariant_end:
          case Intrinsic::launder_invariant_group:
          case Intrinsic::strip_invariant_group:
            ShouldErase = true;
            break;
          case Intrinsic::stacksave:
            CI->replaceAllUsesWith(RawNull);
            ShouldErase = true;
            break;
          default:
            break;
          }
          if (ShouldErase) {
            if (InvokeInst* II = dyn_cast<InvokeInst>(CI))
              BranchInst::Create(II->getNormalDest(), CI)->setDebugLoc(CI->getDebugLoc());
            ToErase.push_back(CI);
          }
        }
        for (Instruction* I : ToErase)
          I->eraseFromParent();
      }
    }
  }

  void lazifyAllocasInFunction(Function& F) {
    if (F.isDeclaration())
      return;
    
    std::unordered_set<AllocaInst*> Allocas;

    for (Instruction& I : F.getEntryBlock()) {
      if (AllocaInst* AI = dyn_cast<AllocaInst>(&I)) {
        // For now, don't bother with AllocaInsts that flow into PHINodes. Pretty sure that doesn't
        // happen and it would be annoying to deal with.
        bool FoundPhi = false;

        for (User* U : AI->users()) {
          if (isa<PHINode>(U)) {
            FoundPhi = true;
            break;
          }
        }
        if (!FoundPhi)
          Allocas.insert(AI);
      }
    }

    std::unordered_map<BasicBlock*, std::unordered_map<AllocaInst*, AIState>> AtHeadForBB;
    for (BasicBlock& BB : F) {
      AIState State;
      if (&BB == &F.getEntryBlock())
        State = AIState::Uninitialized;
      else
        State = AIState::Unknown;
      std::unordered_map<AllocaInst*, AIState>& AtHead = AtHeadForBB[&BB];
      for (AllocaInst* AI : Allocas)
        AtHead[AI] = State;
    }

    bool Changed = true;
    while (Changed) {
      Changed = false;
      for (BasicBlock& BB : F) {
        std::unordered_map<AllocaInst*, AIState> State = AtHeadForBB[&BB];
        for (Instruction& I : BB) {
          for (Use& U : I.operands()) {
            AllocaInst* AI = dyn_cast<AllocaInst>(U);
            if (!AI || !Allocas.count(AI))
              continue;
            State[AI] = AIState::Initialized;
          }
        }
        for (BasicBlock* SBB : successors(&BB)) {
          std::unordered_map<AllocaInst*, AIState>& AtSuccessorHead = AtHeadForBB[SBB];
          for (auto& Pair : State) {
            AIState MyState = Pair.second;
            AIState SuccessorState = AtSuccessorHead[Pair.first];
            AIState NewSuccessorState = mergeAIState(MyState, SuccessorState);
            if (NewSuccessorState != SuccessorState) {
              AtSuccessorHead[Pair.first] = NewSuccessorState;
              Changed = true;
            }
          }
        }
      }
    }

    std::unordered_map<AllocaInst*, AllocaInst*> ToLazyAlloca;
    std::vector<AllocaInst*> LazyAllocas;
    for (AllocaInst* AI : Allocas) {
      AllocaInst* LazyAI = new AllocaInst(RawPtrTy, 0, nullptr, "filc_lazy_alloca", AI);
      LazyAI->setDebugLoc(AI->getDebugLoc());
      (new StoreInst(RawNull, LazyAI, AI))->setDebugLoc(AI->getDebugLoc());
      ToLazyAlloca[AI] = LazyAI;
      LazyAllocas.push_back(LazyAI);
    }

    std::vector<Use*> MaybeInitialized;
    std::vector<Use*> Uninitialized;
    std::vector<Use*> NeedsLoad;
    
    for (BasicBlock& BB : F) {
      std::unordered_map<AllocaInst*, AIState> State = AtHeadForBB[&BB];
      for (Instruction& I : BB) {
        for (Use& U : I.operands()) {
          AllocaInst* AI = dyn_cast<AllocaInst>(U);
          if (!AI || !Allocas.count(AI))
            continue;
          AIState OldState = State[AI];
          NeedsLoad.push_back(&U);
          if (OldState == AIState::Initialized)
            continue;
          if (OldState == AIState::MaybeInitialized)
            MaybeInitialized.push_back(&U);
          else
            Uninitialized.push_back(&U);
          State[AI] = AIState::Initialized;
        }
      }
    }

    auto CloneAI = [] (AllocaInst* AI, Instruction* InsertBefore) -> AllocaInst* {
      AllocaInst* Result = new AllocaInst(
        AI->getAllocatedType(), AI->getAddressSpace(), AI->getArraySize(), AI->getAlign(),
        "filc_lazy_clone_" + AI->getName(), InsertBefore);
      Result->setDebugLoc(InsertBefore->getDebugLoc());
      return Result;
    };

    for (Use* U : MaybeInitialized) {
      Instruction* I = cast<Instruction>(U->getUser());
      AllocaInst* AI = cast<AllocaInst>(*U);
      assert(Allocas.count(AI));
      assert(!isa<PHINode>(I));
      AllocaInst* LazyAI = ToLazyAlloca[AI];
      assert(LazyAI);
      LoadInst* LI = new LoadInst(RawPtrTy, LazyAI, "filc_load_lazy_alloca_for_check", I);
      LI->setDebugLoc(I->getDebugLoc());
      ICmpInst* NotInitialized =
        new ICmpInst(I, ICmpInst::ICMP_EQ, LI, RawNull, "filc_lazy_alloca_not_initialized");
      NotInitialized->setDebugLoc(I->getDebugLoc());
      Instruction* InitializeTerm = SplitBlockAndInsertIfThen(NotInitialized, I, false);
      (new StoreInst(CloneAI(AI, InitializeTerm), LazyAI, InitializeTerm))
        ->setDebugLoc(I->getDebugLoc());
    }

    for (Use* U : Uninitialized) {
      Instruction* I = cast<Instruction>(U->getUser());
      AllocaInst* AI = cast<AllocaInst>(*U);
      assert(Allocas.count(AI));
      assert(!isa<PHINode>(I));
      AllocaInst* LazyAI = ToLazyAlloca[AI];
      assert(LazyAI);
      (new StoreInst(CloneAI(AI, I), LazyAI, I))->setDebugLoc(I->getDebugLoc());
    }

    for (Use* U : NeedsLoad) {
      Instruction* I = cast<Instruction>(U->getUser());
      AllocaInst* AI = cast<AllocaInst>(*U);
      assert(Allocas.count(AI));
      assert(!isa<PHINode>(I));
      AllocaInst* LazyAI = ToLazyAlloca[AI];
      assert(LazyAI);
      LoadInst* LI = new LoadInst(RawPtrTy, LazyAI, "filc_load_lazy_alloca", I);
      LI->setDebugLoc(I->getDebugLoc());
      *U = LI;
    }

    for (AllocaInst* AI : Allocas) {
      assert(!AI->hasNUsesOrMore(1));
      AI->eraseFromParent();
    }

    DominatorTree DT(F);
    PromoteMemToReg(LazyAllocas, DT);
  }
  
  void lazifyAllocas() {
    for (Function& F : M.functions())
      lazifyAllocasInFunction(F);
  }

  void canonicalizeGEP(Instruction* I) {
    GetElementPtrInst* GEP = dyn_cast<GetElementPtrInst>(I);
    if (!GEP)
      return;
    
    unsigned ConstantOperandIndexStart = GEP->getNumOperands();
    for (unsigned Index = GEP->getNumOperands(); Index-- > 1;) {
      if (!isa<Constant>(GEP->getOperand(Index)))
        break;
      ConstantOperandIndexStart = Index;
    }

    assert(ConstantOperandIndexStart >= 1);
    assert(ConstantOperandIndexStart <= GEP->getNumOperands());
    if (ConstantOperandIndexStart == GEP->getNumOperands()) {
      // It's not constant at all.
      return;
    }
    if (ConstantOperandIndexStart == 1) {
      // It's all constants.
      return;
    }

    if (verbose)
      errs() << "Canonicalizing GEP = " << *GEP << "\n";

    std::vector<Value*> LeadingOperands;
    std::vector<Value*> TrailingOperands;
    for (unsigned Index = 1; Index < ConstantOperandIndexStart; ++Index)
      LeadingOperands.push_back(GEP->getOperand(Index));
    TrailingOperands.push_back(ConstantInt::get(Int32Ty, 0));
    for (unsigned Index = ConstantOperandIndexStart; Index < GEP->getNumOperands(); ++Index)
      TrailingOperands.push_back(GEP->getOperand(Index));

    Type* LeadingResult = GetElementPtrInst::getIndexedType(GEP->getSourceElementType(),
                                                            LeadingOperands);

    GetElementPtrInst* LeadingGEP = GetElementPtrInst::Create(
      GEP->getSourceElementType(), GEP->getPointerOperand(), LeadingOperands, "filc_leading_gep", I);
    LeadingGEP->setDebugLoc(I->getDebugLoc());
    GetElementPtrInst* TrailingGEP = GetElementPtrInst::Create(
      LeadingResult, LeadingGEP, TrailingOperands, "filc_trailing_gep", I);
    TrailingGEP->setDebugLoc(I->getDebugLoc());
    GEP->replaceAllUsesWith(TrailingGEP);
    GEP->eraseFromParent();

    if (verbose)
      errs() << "New GEPs:\n" << "    " << *LeadingGEP << "\n    " << *TrailingGEP << "\n";
  }

  void canonicalizeGEPsInFunction(Function& F) {
    if (F.isDeclaration())
      return;

    for (BasicBlock& BB : F) {
      std::vector<Instruction*> Instructions;
      for (Instruction& I : BB)
        Instructions.push_back(&I);
      for (Instruction* I : Instructions)
        canonicalizeGEP(I);
    }
  }

  void canonicalizeGEPs() {
    for (Function& F : M.functions()) {
      canonicalizeGEPsInFunction(F);
      optimizeGEPsInFunction(F);
    }
  }

  void dropUB() {
    for (Function& F : M.functions()) {
      for (BasicBlock& BB : F) {
        for (Instruction& I : BB) {
          I.dropUnknownNonDebugMetadata();
          I.dropPoisonGeneratingFlagsAndMetadata();
          I.dropUBImplyingAttrsAndUnknownMetadata();
        }
      }
    }
  }

  void prepare() {
    for (Function& F : M.functions()) {
      for (BasicBlock& BB : F) {
        for (Instruction& I : BB) {
          if (isa<IndirectBrInst>(&I))
            llvm_unreachable("Don't support IndirectBr yet (and maybe never will)");
          if (LoadInst* LI = dyn_cast<LoadInst>(&I))
            assert(!LI->getPointerAddressSpace());
          if (StoreInst* SI = dyn_cast<StoreInst>(&I))
            assert(!SI->getPointerAddressSpace());
          if (AtomicCmpXchgInst* AI = dyn_cast<AtomicCmpXchgInst>(&I))
            assert(!AI->getPointerAddressSpace());
          if (AtomicRMWInst* AI = dyn_cast<AtomicRMWInst>(&I))
            assert(!AI->getPointerAddressSpace());
        }
      }

      // FIXME: Maybe we don't need to split critical edges in those cases where the target is a
      // landing pad?
      //
      // We split critical edges to handle the case where a phi has a constant input, and we need to
      // lower the constant in the predecessor block. However, it's not clear if it's even necessary
      // to split critical edges for that case. If we don't split, then at worst, the predecessors of
      // phis, where the phi is reached via a critical edge, will have some constant lowering that is
      // dead along the other edges.
      SplitAllCriticalEdges(F);
    }
  }

  template<typename KeyType>
  void simpleCSE(Function& F, std::unordered_map<KeyType, std::vector<Instruction*>>& InstMap) {
    DominatorTree DT(F);

    for (auto& Pair : InstMap) {
      std::vector<Instruction*>& Insts = Pair.second;

      if (verbose) {
        errs() << "Considering insts:\n";
        for (Instruction* I : Insts)
          errs() << "    " << *I << "\n";
      }

      assert(Insts.size() >= 1);
      for (size_t Index = Insts.size(); Index--;) {
        if (!Insts[Index])
          continue;
        for (size_t Index2 = Insts.size(); Index2--;) {
          if (!Insts[Index2])
            continue;
          if (Insts[Index] == Insts[Index2])
            continue;
          if (verbose)
            errs() << "Considering " << *Insts[Index] << " and " << *Insts[Index2] << "\n";
          if (DT.dominates(Insts[Index], Insts[Index2])) {
            if (verbose)
              errs() << "    Dominates!\n";
            Insts[Index2]->replaceAllUsesWith(Insts[Index]);
            Insts[Index2]->eraseFromParent();
            Insts[Index2] = nullptr;
          }
        }
      }
    }
  }

  // This does one pass of GEP CSE and is exactly what we need after canonicalizing GEPs.
  void optimizeGEPsInFunction(Function& F) {
    if (F.isDeclaration())
      return;

    std::unordered_map<GEPKey, std::vector<Instruction*>> GEPMap;

    for (BasicBlock& BB : F) {
      for (Instruction& I : BB) {
        GetElementPtrInst* GEP = dyn_cast<GetElementPtrInst>(&I);
        if (!GEP)
          continue;
        GEPMap[GEP].push_back(GEP);
      }
    }

    simpleCSE(F, GEPMap);
  }

  void optimizeGetters() {
    std::unordered_map<Value*, std::vector<Instruction*>> GetterCallsForGetter;

    for (BasicBlock& BB : *NewF) {
      for (Instruction& I : BB) {
        CallInst* CI = dyn_cast<CallInst>(&I);
        if (!CI)
          continue;

        Function* Getter = CI->getCalledFunction();
        if (!Getter)
          continue;

        if (!Getters.count(Getter))
          continue;

        assert(CI->getFunctionType() == GlobalGetterTy);

        GetterCallsForGetter[Getter].push_back(CI);
      }
    }

    simpleCSE(*NewF, GetterCallsForGetter);
  }

public:
  Pizlonator(Module &M)
    : C(M.getContext()), M(M), DLBefore(M.getDataLayout()), DL(M.getDataLayoutAfterFilC()) {
  }

  void run() {
    if (verbose)
      errs() << "Going to town on module:\n" << M << "\n";

    assert(DLBefore.getPointerSizeInBits(TargetAS) == 64);
    assert(DLBefore.getPointerABIAlignment(TargetAS) == 8);
    assert(DLBefore.isNonIntegralAddressSpace(TargetAS));
    assert(DL.getPointerSizeInBits(TargetAS) == 64);
    assert(DL.getPointerABIAlignment(TargetAS) == 8);
    assert(!DL.isNonIntegralAddressSpace(TargetAS));

    PtrBits = DL.getPointerSizeInBits(TargetAS);
    VoidTy = Type::getVoidTy(C);
    Int1Ty = Type::getInt1Ty(C);
    Int8Ty = Type::getInt8Ty(C);
    Int16Ty = Type::getInt16Ty(C);
    Int32Ty = Type::getInt32Ty(C);
    IntPtrTy = Type::getIntNTy(C, PtrBits);
    assert(IntPtrTy == Type::getInt64Ty(C)); // FilC is 64-bit-only, for now.
    Int128Ty = Type::getInt128Ty(C);
    FloatTy = Type::getFloatTy(C);
    DoubleTy = Type::getDoubleTy(C);
    RawPtrTy = PointerType::get(C, TargetAS);
    CtorDtorTy = FunctionType::get(VoidTy, false);
    SetjmpTy = FunctionType::get(Int32Ty, RawPtrTy, false);
    SigsetjmpTy = FunctionType::get(Int32Ty, { RawPtrTy, Int32Ty }, false);
    RawNull = ConstantPointerNull::get(RawPtrTy);
    ThreadlocalAddress = Intrinsic::getDeclaration(&M, Intrinsic::threadlocal_address, { RawPtrTy });

    Dummy = makeDummy(Int32Ty);

    undefineAvailableExternally();
    removeIrrelevantIntrinsics();
    expandConstantExprs();
    inferPointerAsIntLaundering();

    if (verbose) {
      errs() << "Module with irrelevant intrinsics removed, constexprs expanded, and pointer "
             << "laundering inferred:\n" << M << "\n";
    }
    
    lowerThreadLocals();

    if (verbose) {
      errs() << "Module with lowered thread locals:\n" << M << "\n";
    }
    
    makeEHDatas();
    compileModuleAsm();
    lazifyAllocas();
    canonicalizeGEPs();
    dropUB();
    
    if (verbose) {
      errs() << "Module with lowered EH data, lowered module asm, lazified allocas, and UB "
             << "dropped:\n" << M << "\n";
    }

    prepare();

    // FIXME: We could probably do this anywhere in the pass, and really all we're doing is turning
    // off the non-integralness of address space 0.
    M.setDataLayout(M.getDataLayoutAfterFilC());
    
    if (verbose)
      errs() << "Prepared module:\n" << M << "\n";

    FunctionName = "<internal>";
    
    FlightPtrTy = StructType::create({ RawPtrTy, RawPtrTy }, "filc_flight_ptr");
    OriginNodeTy = StructType::create({ RawPtrTy, RawPtrTy, Int32Ty }, "filc_origin_node");
    FunctionOriginTy = StructType::create(
      { OriginNodeTy, RawPtrTy, Int8Ty, Int8Ty, Int32Ty }, "filc_function_origin");
    OriginTy = StructType::create({ RawPtrTy, Int32Ty, Int32Ty }, "filc_origin");
    InlineFrameTy = StructType::create({ OriginNodeTy, OriginTy }, "filc_inline_frame");
    OriginWithEHTy = StructType::create(
      { RawPtrTy, Int32Ty, Int32Ty, RawPtrTy }, "filc_origin_with_eh");
    ObjectTy = StructType::create({ RawPtrTy, RawPtrTy }, "filc_object");
    FrameTy = StructType::create({ RawPtrTy, RawPtrTy, RawPtrTy }, "filc_frame");

    std::vector<Type*> ThreadMembers;
    ThreadMembers.push_back(IntPtrTy); // stack_limit, index 0
    ThreadMembers.push_back(Int8Ty); // state, index 1
    ThreadMembers.push_back(Int32Ty); // tid, index 2
    ThreadMembers.push_back(RawPtrTy); // top_frame, index 3
    ThreadMembers.push_back(RawPtrTy); // alignment word, index 4
    ThreadMembers.push_back(
      ArrayType::get(FlightPtrTy, NumUnwindRegisters)); // unwind_registers, index 5
    ThreadMembers.push_back(FlightPtrTy); // cookie_ptr, index 6
    ThreadMembers.push_back(Int8Ty); // alignment point, index 7
    ThreadTy = StructType::create(ThreadMembers, "filc_thread_ish_base2");
    const StructLayout* ThreadLayout = DL.getStructLayout(ThreadTy);
    size_t AlignmentPoint = ThreadLayout->getElementOffset(7);
    ThreadMembers.pop_back();
    // Create an alignment buffer at index 7.
    ThreadMembers.push_back(
      ArrayType::get(
        Int8Ty, ((AlignmentPoint + CCAlignment) & -CCAlignment) - AlignmentPoint));
    ThreadMembers.push_back(ArrayType::get(Int8Ty, CCInlineSize)); // cc_inline_buffer, index 8
    ThreadMembers.push_back(ArrayType::get(Int8Ty, CCInlineSize)); // cc_inline_aux_buffer, index 9
    ThreadMembers.push_back(RawPtrTy); // cc_outline_buffer, index 10
    ThreadMembers.push_back(RawPtrTy); // cc_outline_aux_buffer, index 11
    ThreadMembers.push_back(IntPtrTy); // cc_outline_size, index 12
    ThreadTy = StructType::create(ThreadMembers, "filc_thread_ish");
    ThreadLayout = DL.getStructLayout(ThreadTy);
    assert(!(ThreadLayout->getElementOffset(8) & (CCAlignment - 1)));
    
    ConstantRelocationTy = StructType::create(
      { IntPtrTy, Int32Ty, RawPtrTy }, "filc_constant_relocation");
    ConstexprNodeTy = StructType::create(
      { Int32Ty, Int32Ty, RawPtrTy, IntPtrTy }, "filc_constexpr_node");
    AlignmentAndOffsetTy = StructType::create({ Int8Ty, Int8Ty }, "filc_alignment_and_offset");
    PizlonatedReturnValueTy = StructType::create({ Int1Ty, IntPtrTy }, "pizlonated_return_value");
    PtrPairTy = StructType::create({ RawPtrTy, RawPtrTy }, "filc_ptr_pair");
    PizlonatedFuncTy = FunctionType::get(
      PizlonatedReturnValueTy, { RawPtrTy, IntPtrTy }, false);
    GlobalGetterTy = FunctionType::get(FlightPtrTy, { RawPtrTy }, false);

    // FIXME: Eventually, we'll want to do something with the DSO handle. But for now it doesn't
    // matter because we don't support dlclose.
    if (GlobalVariable* DSO = M.getGlobalVariable("__dso_handle")) {
      assert(DSO->isDeclaration());
      DSO->replaceAllUsesWith(RawNull);
      DSO->eraseFromParent();
    }
    
    for (GlobalVariable &G : M.globals()) {
      if (shouldPassThrough(&G))
        continue;
      Globals.push_back(&G);
    }
    for (Function &F : M.functions()) {
      if (shouldPassThrough(&F)) {
        if (!F.isDeclaration()) {
          errs() << "Cannot define " << F.getName() << "\n";
          llvm_unreachable("Attempt to define pass-through function.");
        }
        if (isSetjmp(&F)) {
          assert(F.hasFnAttribute(Attribute::ReturnsTwice));
          if (F.getName() == "setjmp" || F.getName() == "_setjmp") {
            if (F.getFunctionType() != SetjmpTy) {
              errs() << "Unexpected setjmp signature: " << *F.getFunctionType()
                     << ", expected: " << *SetjmpTy << "\n";
            }
            assert(F.getFunctionType() == SetjmpTy);
          } else {
            if (F.getFunctionType() != SigsetjmpTy) {
              errs() << "Unexpected setjmp signature: " << *F.getFunctionType()
                     << ", expected: " << *SigsetjmpTy << "\n";
            }
            assert(F.getName() == "sigsetjmp");
            assert(F.getFunctionType() == SigsetjmpTy);
          }
        }
        continue;
      }
      Functions.push_back(&F);
    }
    for (GlobalAlias &G : M.aliases())
      Aliases.push_back(&G);
    if (!M.ifunc_empty())
      llvm_unreachable("Don't handle ifuncs yet.");

    FlightNull = ConstantAggregateZero::get(FlightPtrTy);
    if (verbose)
      errs() << "FlightNull = " << *FlightNull << "\n";

    PollcheckSlow = M.getOrInsertFunction(
      "filc_pollcheck_slow", VoidTy, RawPtrTy, RawPtrTy);
    StoreBarrierForLowerSlow = M.getOrInsertFunction(
      "filc_store_barrier_for_lower_slow", VoidTy, RawPtrTy, RawPtrTy);
    StorePtrAtomicWithPtrPairOutline = M.getOrInsertFunction(
      "filc_store_ptr_atomic_with_ptr_pair_outline",
      VoidTy, RawPtrTy, RawPtrTy, RawPtrTy, FlightPtrTy);
    ObjectEnsureAuxPtrOutline = M.getOrInsertFunction(
      "filc_object_ensure_aux_ptr_outline", RawPtrTy, RawPtrTy, RawPtrTy);
    ThreadEnsureCCOutlineBufferSlow = M.getOrInsertFunction(
      "filc_thread_ensure_cc_outline_buffer_slow", VoidTy, RawPtrTy, IntPtrTy);
    StrongCasPtr = M.getOrInsertFunction(
      "filc_strong_cas_ptr_with_manual_tracking",
      FlightPtrTy, RawPtrTy, FlightPtrTy, IntPtrTy, FlightPtrTy, FlightPtrTy);
    XchgPtr = M.getOrInsertFunction(
      "filc_xchg_ptr_with_manual_tracking",
      FlightPtrTy, RawPtrTy, FlightPtrTy, IntPtrTy, FlightPtrTy);
    GetNextPtrBytesForVAArg = M.getOrInsertFunction(
      "filc_get_next_ptr_bytes_for_va_arg", PtrPairTy, FlightPtrTy, IntPtrTy, IntPtrTy);
    Allocate = M.getOrInsertFunction(
      "filc_allocate", RawPtrTy, RawPtrTy, IntPtrTy);
    AllocateWithAlignment = M.getOrInsertFunction(
      "filc_allocate_with_alignment", RawPtrTy, RawPtrTy, IntPtrTy, IntPtrTy);
    CheckFunctionCallFail = M.getOrInsertFunction(
      "filc_check_function_call_fail", VoidTy, FlightPtrTy);
    OptimizedAlignmentContradiction = M.getOrInsertFunction(
      "filc_optimized_alignment_contradiction", VoidTy, FlightPtrTy, RawPtrTy);
    OptimizedAccessCheckFail = M.getOrInsertFunction(
      "filc_optimized_access_check_fail", VoidTy, FlightPtrTy, RawPtrTy);
    Memset = M.getOrInsertFunction(
      "filc_memset", VoidTy, RawPtrTy, FlightPtrTy, Int32Ty, IntPtrTy, RawPtrTy);
    Memmove = M.getOrInsertFunction(
      "filc_memmove", VoidTy, RawPtrTy, FlightPtrTy, FlightPtrTy, IntPtrTy, RawPtrTy);
    GlobalInitializationContextCreate = M.getOrInsertFunction(
      "filc_global_initialization_context_create", RawPtrTy, RawPtrTy);
    GlobalInitializationContextAdd = M.getOrInsertFunction(
      "filc_global_initialization_context_add", Int1Ty, RawPtrTy, RawPtrTy, RawPtrTy);
    GlobalInitializationContextDestroy = M.getOrInsertFunction(
      "filc_global_initialization_context_destroy", VoidTy, RawPtrTy);
    ExecuteConstantRelocations = M.getOrInsertFunction(
      "filc_execute_constant_relocations", VoidTy, RawPtrTy, RawPtrTy, IntPtrTy, RawPtrTy);
    DeferOrRunGlobalCtor = M.getOrInsertFunction(
      "filc_defer_or_run_global_ctor", VoidTy, RawPtrTy);
    RunGlobalDtor = M.getOrInsertFunction(
      "filc_run_global_dtor", VoidTy, RawPtrTy);
    Error = M.getOrInsertFunction(
      "filc_error", VoidTy, RawPtrTy, RawPtrTy);
    RealMemset = M.getOrInsertFunction(
      "llvm.memset.p0.i64", VoidTy, RawPtrTy, Int8Ty, IntPtrTy, Int1Ty);
    RealMemcpy = M.getOrInsertFunction(
      "llvm.memcpy.p0.p0.i64", VoidTy, RawPtrTy, RawPtrTy, IntPtrTy, Int1Ty);
    LandingPad = M.getOrInsertFunction(
      "filc_landing_pad", Int1Ty, RawPtrTy);
    ResumeUnwind = M.getOrInsertFunction(
      "filc_resume_unwind", VoidTy, RawPtrTy, RawPtrTy);
    JmpBufCreate = M.getOrInsertFunction(
      "filc_jmp_buf_create", RawPtrTy, RawPtrTy, Int32Ty, Int32Ty);
    PromoteArgsToHeap = M.getOrInsertFunction(
      "filc_promote_args_to_heap", FlightPtrTy, RawPtrTy, IntPtrTy);
    PrepareToReturnWithData = M.getOrInsertFunction(
      "filc_prepare_to_return_with_data", IntPtrTy, RawPtrTy, FlightPtrTy, RawPtrTy);
    CCArgsCheckFailure = M.getOrInsertFunction(
      "filc_cc_args_check_failure", VoidTy, IntPtrTy, IntPtrTy, RawPtrTy);
    CCRetsCheckFailure = M.getOrInsertFunction(
      "filc_cc_rets_check_failure", VoidTy, IntPtrTy, IntPtrTy, RawPtrTy);
    _Setjmp = M.getOrInsertFunction(
      "_setjmp", Int32Ty, RawPtrTy);
    cast<Function>(_Setjmp.getCallee())->addFnAttr(Attribute::ReturnsTwice);
    ExpectI1 = Intrinsic::getDeclaration(&M, Intrinsic::expect, Int1Ty);
    LifetimeStart = Intrinsic::getDeclaration(&M, Intrinsic::lifetime_start, { RawPtrTy });
    LifetimeEnd = Intrinsic::getDeclaration(&M, Intrinsic::lifetime_end, { RawPtrTy });
    StackCheckAsm = InlineAsm::get(
      FunctionType::get(VoidTy, { RawPtrTy }, false),
      "cmp %rsp, $0\n\t"
      "jae filc_stack_overflow_failure@PLT",
      "*m,~{memory},~{dirflag},~{fpsr},~{flags}",
      /*hasSideEffects=*/true);

    cast<Function>(OptimizedAlignmentContradiction.getCallee())->addFnAttr(Attribute::NoReturn);
    cast<Function>(OptimizedAccessCheckFail.getCallee())->addFnAttr(Attribute::NoReturn);

    IsMarking = M.getOrInsertGlobal("filc_is_marking", Int8Ty);

    std::vector<GlobalValue*> ToDelete;
    auto HandleGlobal = [&] (GlobalValue* G) {
      if (verbose)
        errs() << "Handling global: " << G->getName() << "\n";
      Function* NewF = Function::Create(GlobalGetterTy, G->getLinkage(), G->getAddressSpace(),
                                        "pizlonated_" + G->getName(), &M);
      NewF->setVisibility(G->getVisibility());
      GlobalToGetter[G] = NewF;
      Getters.insert(NewF);
      ToDelete.push_back(G);
    };
    for (GlobalVariable* G : Globals)
      HandleGlobal(G);
    for (GlobalAlias* G : Aliases)
      HandleGlobal(G);
    for (Function* F : Functions) {
      if (F->isIntrinsic())
        continue;
      HandleGlobal(F);
      if (!F->isDeclaration()) {
        FunctionToHiddenFunction[F] = Function::Create(
          PizlonatedFuncTy,
          GlobalValue::InternalLinkage, F->getAddressSpace(),
          "Jf_" + F->getName(), &M);
      }
    }
    if (verbose) {
      errs() << "ToDelete values:";
      for (GlobalValue* GV : ToDelete)
        errs() << " " << GV->getName();
      errs() << "\n";
    }
    
    if (GlobalVariable* GlobalCtors = M.getGlobalVariable("llvm.global_ctors")) {
      ConstantArray* Array = cast<ConstantArray>(GlobalCtors->getInitializer());
      std::vector<Constant*> Args;
      for (size_t Index = 0; Index < Array->getNumOperands(); ++Index) {
        ConstantStruct* Struct = cast<ConstantStruct>(Array->getOperand(Index));
        assert(Struct->getOperand(2) == RawNull);
        Function* Ctor = cast<Function>(Struct->getOperand(1));
        Function* HiddenCtor = FunctionToHiddenFunction[Ctor];
        assert(HiddenCtor);
        Function* NewF = Function::Create(
          CtorDtorTy, GlobalValue::InternalLinkage, 0, "filc_ctor_forwarder", &M);
        BasicBlock* RootBB = BasicBlock::Create(C, "filc_ctor_forwarder_root", NewF);
        CallInst::Create(DeferOrRunGlobalCtor, { HiddenCtor }, "", RootBB);
        ReturnInst::Create(C, RootBB);
        Args.push_back(ConstantStruct::get(Struct->getType(), Struct->getOperand(0), NewF, RawNull));
      }
      GlobalCtors->setInitializer(ConstantArray::get(Array->getType(), Args));
    }

    // NOTE: This *might* be dead code, since modern C/C++ says that the compiler has to do
    // __cxa_atexit from a global constructor instead of registering a global destructor.
    if (GlobalVariable* GlobalDtors = M.getGlobalVariable("llvm.global_dtors")) {
      ConstantArray* Array = cast<ConstantArray>(GlobalDtors->getInitializer());
      std::vector<Constant*> Args;
      for (size_t Index = 0; Index < Array->getNumOperands(); ++Index) {
        ConstantStruct* Struct = cast<ConstantStruct>(Array->getOperand(Index));
        assert(Struct->getOperand(2) == RawNull);
        Function* Dtor = cast<Function>(Struct->getOperand(1));
        Function* HiddenDtor = FunctionToHiddenFunction[Dtor];
        Function* NewF = Function::Create(
          CtorDtorTy, GlobalValue::InternalLinkage, 0, "filc_dtor_forwarder", &M);
        BasicBlock* RootBB = BasicBlock::Create(C, "filc_dtor_forwarder_root", NewF);
        CallInst::Create(RunGlobalDtor, { HiddenDtor }, "", RootBB);
        ReturnInst::Create(C, RootBB);
        Args.push_back(ConstantStruct::get(Struct->getType(), Struct->getOperand(0), NewF, RawNull));
      }
      GlobalDtors->setInitializer(ConstantArray::get(Array->getType(), Args));
    }

    auto HandleUsed = [&] (GlobalVariable* Used) {
      ConstantArray* Array = cast<ConstantArray>(Used->getInitializer());
      std::vector<Constant*> Args;
      for (size_t Index = 0; Index < Array->getNumOperands(); ++Index) {
        // NOTE: This could have a GEP, supposedly. Pretend it can't for now.
        Function* GetterF = GlobalToGetter[cast<GlobalValue>(Array->getOperand(Index))];
        assert(GetterF);
        Args.push_back(GetterF);
      }
      Used->setInitializer(ConstantArray::get(Array->getType(), Args));
    };
    if (GlobalVariable* Used = M.getGlobalVariable("llvm.used"))
      HandleUsed(Used);
    if (GlobalVariable* Used = M.getGlobalVariable("llvm.compiler.used"))
      HandleUsed(Used);

    for (GlobalVariable* G : Globals) {
      // We've already lowered thread locals by the time we get here.
      assert(G->getThreadLocalMode() == GlobalValue::NotThreadLocal);

      Function* NewF = GlobalToGetter[G];
      assert(NewF);
      assert(NewF->isDeclaration());

      if (verbose)
        errs() << "Dealing with global: " << *G << "\n";

      if (G->isDeclaration())
        continue;

      Function* SlowF = Function::Create(GlobalGetterTy, GlobalValue::PrivateLinkage,
                                         G->getAddressSpace(), "filc_getter_slow", &M);
      SlowF->addFnAttr(Attribute::NoInline);

      Constant* NewC = paddedConstant(
        constantToRestConstantWithPtrPlaceholders(G->getInitializer()));
      assert(NewC);
      size_t CSize = DL.getTypeStoreSize(NewC->getType());
      assert(!(CSize % WordSize));

      std::vector<ConstantRelocation> Relocations;
      bool RelocationsSucceeded = computeConstantRelocations(G->getInitializer(), Relocations);
      bool IsConstant = G->isConstant() && RelocationsSucceeded && !Relocations.size();

      GlobalVariable* AuxG;
      Constant* AuxPtr;
      if (hasNonNullPtrs(G->getInitializer())) {
        ArrayType* AuxTy = ArrayType::get(RawPtrTy, CSize / WordSize);
        AuxG = new GlobalVariable(
          M, AuxTy, IsConstant, GlobalValue::InternalLinkage, ConstantAggregateZero::get(AuxTy),
          "Jga_" + G->getName());
        AuxPtr = AuxG;
      } else {
        AuxG = nullptr;
        AuxPtr = RawNull;
      }

      std::vector<Type*> ObjectGTyFields;
      size_t AlignmentOffset = 0;
      ArrayType* AlignmentTy = nullptr;
      if (G->getAlignment() > ObjectSize) {
        AlignmentOffset = G->getAlignment() - ObjectSize;
        assert(AlignmentOffset);
        AlignmentTy = ArrayType::get(Int8Ty, AlignmentOffset);
        ObjectGTyFields.push_back(AlignmentTy);
      }
      ObjectGTyFields.push_back(RawPtrTy);
      ObjectGTyFields.push_back(RawPtrTy);
      ObjectGTyFields.push_back(NewC->getType());
      StructType* ObjectGTy = StructType::get(C, ObjectGTyFields);

      GlobalVariable* NewDataG = new GlobalVariable(
        M, ObjectGTy, IsConstant, GlobalValue::InternalLinkage, nullptr, "Jgo_" + G->getName());

      uint16_t ObjectFlags = ObjectFlagGlobal;
      if (AuxG)
        ObjectFlags |= ObjectFlagGlobalAux;
      if (G->isConstant())
        ObjectFlags |= ObjectFlagReadonly;
      std::vector<Constant*> NewObjCFields;
      if (AlignmentTy)
        NewObjCFields.push_back(ConstantAggregateZero::get(AlignmentTy));
      NewObjCFields.push_back(
        ConstantExpr::getGetElementPtr(ObjectGTy, NewDataG, ConstantInt::get(IntPtrTy, 1)));
      NewObjCFields.push_back(
        ConstantExpr::getGetElementPtr(
          Int8Ty, AuxPtr,
          ConstantInt::get(IntPtrTy, static_cast<uintptr_t>(ObjectFlags) << ObjectAuxFlagsShift)));
      NewObjCFields.push_back(NewC);
      Constant* NewObjC = ConstantStruct::get(ObjectGTy, NewObjCFields);
      NewDataG->setInitializer(NewObjC);

      if (G->getAlignment() < WordSize)
        NewDataG->setAlignment(Align(WordSize));
      else
        NewDataG->setAlignment(Align(G->getAlignment()));

      Constant* NewDataPayloadC = ConstantExpr::getGetElementPtr(
        Int8Ty, NewDataG, ConstantInt::get(IntPtrTy, ObjectSize + AlignmentOffset));
      Constant* NewDataObjectC = ConstantExpr::getGetElementPtr(
        Int8Ty, NewDataG, ConstantInt::get(IntPtrTy, AlignmentOffset));
      
      GlobalVariable* NewPtrG = new GlobalVariable(
        M, FlightPtrTy, false, GlobalValue::PrivateLinkage, FlightNull,
        "filc_gptr_" + G->getName());
      NewPtrG->setAlignment(Align(FlightPtrAlign));
      
      BasicBlock* RootBB = BasicBlock::Create(C, "filc_global_getter_root", NewF);
      BasicBlock* OtherCheckBB = BasicBlock::Create(C, "filc_global_getter_other_check", NewF);
      BasicBlock* FastBB = BasicBlock::Create(C, "filc_global_getter_fast", NewF);
      BasicBlock* SlowBB = BasicBlock::Create(C, "filc_global_getter_slow", NewF);
      BasicBlock* SlowRootBB = BasicBlock::Create(C, "filc_global_getter_slow_root", SlowF);
      BasicBlock* RecurseBB = BasicBlock::Create(C, "filc_global_getter_recurse", SlowF);
      BasicBlock* BuildBB = BasicBlock::Create(C, "filc_global_getter_build", SlowF);

      // We can load the NewPtrG in two 64-bit chunks and then check if either the lower or the
      // raw ptr as NULL. If either are NULL, then it's either not initialized yet, or we experienced
      // ptr tearing. This allows us to avoid an expensive 128-bit atomic.
      
      Instruction* Branch = BranchInst::Create(SlowBB, OtherCheckBB, UndefValue::get(Int1Ty), RootBB);
      Value* LoadPtr = loadFlightPtr(NewPtrG, Branch);
      Branch->getOperandUse(0) = new ICmpInst(
        Branch, ICmpInst::ICMP_EQ, flightPtrPtr(LoadPtr, Branch), RawNull, "filc_check_global");

      Instruction* OtherBranch = BranchInst::Create(
        SlowBB, FastBB, UndefValue::get(Int1Ty), OtherCheckBB);
      OtherBranch->getOperandUse(0) = new ICmpInst(
        OtherBranch, ICmpInst::ICMP_EQ, flightPtrLower(LoadPtr, OtherBranch), RawNull,
        "filc_check_global");
      
      ReturnInst::Create(C, LoadPtr, FastBB);

      ReturnInst::Create(
        C,
        CallInst::Create(GlobalGetterTy, SlowF, { NewF->getArg(0) }, "filc_call_getter_slow", SlowBB),
        SlowBB);

      Branch = BranchInst::Create(BuildBB, RecurseBB, UndefValue::get(Int1Ty), SlowRootBB);
      Instruction* MyInitializationContext = CallInst::Create(
        GlobalInitializationContextCreate, { SlowF->getArg(0) }, "filc_context_create",
        Branch);
      Value* Ptr = flightPtrForPayload(NewDataPayloadC, Branch);
      // NOTE: This function call is necessary even for the cases of globals with no meaningful
      // initializer or an initializer we could just optimize to pure data, since we have to tell the
      // runtime about this global so that the GC can find it.
      Instruction* Add = CallInst::Create(
        GlobalInitializationContextAdd,
        { MyInitializationContext, NewPtrG, NewDataObjectC },
        "filc_context_add", Branch);
      Branch->getOperandUse(0) = Add;

      CallInst::Create(
        GlobalInitializationContextDestroy, { MyInitializationContext }, "", RecurseBB);
      ReturnInst::Create(C, Ptr, RecurseBB);

      Instruction* Return = ReturnInst::Create(C, Ptr, BuildBB);
      if (verbose)
        errs() << "Lowering constant " << *G->getInitializer() << " with initialization context = " << *MyInitializationContext << "\n";
      if (RelocationsSucceeded) {
        if (Relocations.size()) {
          std::vector<Constant*> Constants;
          for (const ConstantRelocation& Relocation : Relocations) {
            Constants.push_back(
              ConstantStruct::get(
                ConstantRelocationTy,
                { ConstantInt::get(IntPtrTy, Relocation.Offset),
                  ConstantInt::get(Int32Ty, static_cast<unsigned>(Relocation.Kind)),
                  Relocation.Target }));
          }
          ArrayType* AT = ArrayType::get(ConstantRelocationTy, Constants.size());
          Constant* CA = ConstantArray::get(AT, Constants);
          GlobalVariable* RelocG = new GlobalVariable(
            M, AT, true, GlobalVariable::PrivateLinkage, CA, "filc_constant_relocations");
          CallInst::Create(
            ExecuteConstantRelocations,
            { NewDataObjectC, RelocG, ConstantInt::get(IntPtrTy, Constants.size()),
              MyInitializationContext },
            "", Return);
        }
      } else {
        Value* C = constantToFlightValue(G->getInitializer(), Return, MyInitializationContext);
        storeValueRecurseAfterCheck(
          G->getInitializer()->getType(), C, NewDataPayloadC, AuxPtr, false, Align(G->getAlignment()),
          AtomicOrdering::NotAtomic, SyncScope::System, MemoryKind::GlobalInit, Return);
      }
      
      CallInst::Create(GlobalInitializationContextDestroy, { MyInitializationContext }, "", Return);
    }
    for (Function* F : Functions) {
      if (F->isIntrinsic())
        continue;
      
      if (verbose)
        errs() << "Function before lowering: " << *F << "\n";

      if (!F->isDeclaration()) {
        FunctionName = getFunctionName(F);
        OldF = F;
        NewF = FunctionToHiddenFunction[F];
        AttrBuilder AB(C, OldF->getAttributes().getFnAttrs());
        AB.removeAttribute(Attribute::AllocKind);
        AB.removeAttribute(Attribute::AllocSize);
        AB.removeAttribute(Attribute::AlwaysInline);
        AB.removeAttribute(Attribute::Builtin);
        AB.removeAttribute(Attribute::Convergent);
        AB.removeAttribute(Attribute::FnRetThunkExtern);
        AB.removeAttribute(Attribute::InlineHint);
        AB.removeAttribute(Attribute::JumpTable);
        AB.removeAttribute(Attribute::Memory);
        AB.removeAttribute(Attribute::Naked);
        AB.removeAttribute(Attribute::NoBuiltin);
        AB.removeAttribute(Attribute::NoCallback);
        AB.removeAttribute(Attribute::NoDuplicate);
        AB.removeAttribute(Attribute::NoFree);
        AB.removeAttribute(Attribute::NonLazyBind);
        AB.removeAttribute(Attribute::NoMerge);
        AB.removeAttribute(Attribute::NoRecurse);
        AB.removeAttribute(Attribute::NoRedZone);
        AB.removeAttribute(Attribute::NoReturn);
        AB.removeAttribute(Attribute::NoSync);
        AB.addAttribute(Attribute::NoUnwind);
        AB.removeAttribute(Attribute::NullPointerIsValid);
        AB.removeAttribute(Attribute::Preallocated);
        AB.removeAttribute(Attribute::ReturnsTwice);
        AB.removeAttribute(Attribute::UWTable);
        AB.removeAttribute(Attribute::WillReturn);
        AB.removeAttribute(Attribute::MustProgress);
        AB.removeAttribute(Attribute::PresplitCoroutine);
        NewF->addFnAttrs(AB);
        assert(NewF);
        OptimizedAccessCheckOrigins.clear();
        InstTypes.clear();
        InstTypeVectors.clear();
        CanonicalPtrAuxBaseVars.clear();
        AuxBaseVarOperands.clear();

        SmallVector<std::pair<const BasicBlock*, const BasicBlock*>> BackEdges;
        FindFunctionBackedges(*F, BackEdges);
        std::unordered_set<const BasicBlock*> BackEdgePreds;
        for (std::pair<const BasicBlock*, const BasicBlock*>& Edge : BackEdges)
          BackEdgePreds.insert(Edge.first);
      
        MyThread = NewF->getArg(0);
        std::vector<BasicBlock*> Blocks;
        for (BasicBlock& BB : *F)
          Blocks.push_back(&BB);
        assert(!Blocks.empty());
        Args.clear();
        for (BasicBlock* BB : Blocks) {
          BB->removeFromParent();
          BB->insertInto(NewF);
        }
        computeFrameIndexMap(Blocks);
        scheduleChecks(Blocks, BackEdgePreds);
        // Snapshot the instructions before we do crazy stuff.
        std::vector<Instruction*> Instructions;
        for (BasicBlock* BB : Blocks) {
          for (Instruction& I : *BB) {
            Instructions.push_back(&I);
            captureTypesIfNecessary(&I);
          }

          if (BackEdgePreds.count(BB)) {
            DebugLoc DL = BB->getTerminator()->getDebugLoc();
            Value* StatePtr = threadStatePtr(MyThread, BB->getTerminator());
            LoadInst* StateLoad = new LoadInst(
              Int8Ty, StatePtr, "filc_thread_state_load", BB->getTerminator());
            StateLoad->setDebugLoc(DL);
            BinaryOperator* Masked = BinaryOperator::Create(
              Instruction::And, StateLoad,
              ConstantInt::get(
                Int8Ty,
                ThreadStateCheckRequested | ThreadStateStopRequested | ThreadStateDeferredSignal),
              "filc_thread_state_masked", BB->getTerminator());
            Masked->setDebugLoc(DL);
            ICmpInst* PollcheckNotNeeded = new ICmpInst(
              BB->getTerminator(), ICmpInst::ICMP_EQ, Masked, ConstantInt::get(Int8Ty, 0),
              "filc_pollcheck_not_needed");
            Masked->setDebugLoc(DL);
            Instruction* NewTerm =
              SplitBlockAndInsertIfElse(
                expectTrue(PollcheckNotNeeded, BB->getTerminator()), BB->getTerminator(), false);
            CallInst::Create(
              PollcheckSlow, { MyThread, getOrigin(DL) }, "", NewTerm)->setDebugLoc(DL);
          }
        }

        // Make sure that when folks want to add allocas to the root block, they get a pristine block.
        BasicBlock* RootB = Blocks[0];
        Instruction* InsertionPoint = &*Blocks[0]->getFirstInsertionPt();
        SplitBlock(RootB, InsertionPoint);
        Instruction* AllocaInsertionPoint = RootB->getTerminator();

        ReturnB = BasicBlock::Create(C, "filc_return_block", NewF);
        if (F->getReturnType() != VoidTy) {
          ReturnPhi = PHINode::Create(
            toFlightType(F->getReturnType()), 1, "filc_return_value", ReturnB);
        }

        ReallyReturnB = BasicBlock::Create(C, "filc_really_return_block", NewF);
        BranchInst* ReturnBranch = BranchInst::Create(ReallyReturnB, ReturnB);
        RetSizePhi = PHINode::Create(IntPtrTy, 1, "filc_ret_size", ReallyReturnB);
        Instruction* ReturnValue = InsertValueInst::Create(
          UndefValue::get(PizlonatedReturnValueTy), ConstantInt::getFalse(Int1Ty), { 0 },
          "filc_insert_has_exception", ReallyReturnB);
        ReturnValue = InsertValueInst::Create(
          ReturnValue, RetSizePhi, { 1 }, "filc_insert_ret_size", ReallyReturnB);
        ReturnInst* Return = ReturnInst::Create(C, ReturnValue, ReallyReturnB);

        if (F->getReturnType() != VoidTy) {
          Type* T = F->getReturnType();
          RetSizePhi->addIncoming(storeCC(T, ReturnPhi, ReturnBranch, DebugLoc()), ReturnB);
        } else {
          RetSizePhi->addIncoming(
              storeCC(IntPtrTy, ConstantInt::get(IntPtrTy, 0), ReturnBranch, DebugLoc()), ReturnB);
        }

        ResumeB = BasicBlock::Create(C, "filc_resume_block", NewF);
        ReturnValue = InsertValueInst::Create(
          UndefValue::get(PizlonatedReturnValueTy), ConstantInt::getTrue(Int1Ty), { 0 },
          "filc_insert_has_exception", ResumeB);
        ReturnValue = InsertValueInst::Create(
          ReturnValue, ConstantInt::get(IntPtrTy, 0), { 1 }, "filc_insert_ret_size", ResumeB);
        ReturnInst* ResumeReturn = ReturnInst::Create(C, ReturnValue, ResumeB);

        StructType* MyFrameTy = StructType::get(
          C, { RawPtrTy, RawPtrTy, ArrayType::get(RawPtrTy, FrameSize) });
        Frame = new AllocaInst(MyFrameTy, 0, "filc_my_frame", AllocaInsertionPoint);
        stackOverflowCheck(InsertionPoint);
        Value* ThreadTopFramePtr = threadTopFramePtr(MyThread, InsertionPoint);
        new StoreInst(
          new LoadInst(RawPtrTy, ThreadTopFramePtr, "filc_thread_top_frame", InsertionPoint),
          GetElementPtrInst::Create(
            FrameTy, Frame, { ConstantInt::get(Int32Ty, 0), ConstantInt::get(Int32Ty, 0) },
            "filc_frame_parent_ptr", InsertionPoint),
          InsertionPoint);
        new StoreInst(Frame, ThreadTopFramePtr, InsertionPoint);
        new StoreInst(
          getOrigin(DebugLoc()),
          GetElementPtrInst::Create(
            FrameTy, Frame, { ConstantInt::get(Int32Ty, 0), ConstantInt::get(Int32Ty, 1) },
            "filc_frame_parent_ptr", InsertionPoint),
          InsertionPoint);
        for (size_t FrameIndex = FrameSize; FrameIndex--;)
          recordLowerAtIndex(RawNull, FrameIndex, InsertionPoint);

        auto PopFrame = [&] (Instruction* Return) {
          new StoreInst(
            new LoadInst(
              RawPtrTy,
              GetElementPtrInst::Create(
                FrameTy, Frame, { ConstantInt::get(Int32Ty, 0), ConstantInt::get(Int32Ty, 0) },
                "filc_frame_parent_ptr", Return),
              "filc_frame_parent", Return),
            threadTopFramePtr(MyThread, Return),
            Return);
        };
        PopFrame(Return);
        PopFrame(ResumeReturn);

        size_t LastOffset = 0;
        if (F->getFunctionType()->getNumParams()) {
          StructType* ArgsTy = argsType(F->getFunctionType());
          const StructLayout* SL = DL.getStructLayout(ArgsTy);
          Value* ArgsV = loadCC(ArgsTy, NewF->getArg(1), CCArgsCheckFailure, InsertionPoint, DebugLoc());
          for (unsigned Index = 0; Index < F->getFunctionType()->getNumParams(); ++Index) {
            Type* CanonicalT = ArgsTy->getElementType(Index);
            Type* OriginalT = F->getFunctionType()->getParamType(Index);
            Instruction* Extract = ExtractValueInst::Create(
              toFlightType(CanonicalT), ArgsV, Index, "filc_extract_arg", InsertionPoint);
            Args.push_back(castFromArg(Extract, toFlightType(OriginalT), InsertionPoint));
            LastOffset = SL->getElementOffset(Index) + DL.getTypeAllocSize(CanonicalT);
          }
        }
        if (UsesVastartOrZargs) {
          // Do this after we have recorded all the args for GC, so it's safe to have a pollcheck.
          Value* SnapshottedArgsPtr = CallInst::Create(
            PromoteArgsToHeap, { MyThread, NewF->getArg(1) }, "", InsertionPoint);
          recordLowerAtIndex(
            flightPtrLower(SnapshottedArgsPtr, InsertionPoint), SnapshottedArgsFrameIndex,
            InsertionPoint);
          SnapshottedArgsPtrForVastart = flightPtrWithOffset(
            SnapshottedArgsPtr, ConstantInt::get(IntPtrTy, LastOffset), InsertionPoint);
          SnapshottedArgsPtrForZargs = SnapshottedArgsPtr;
        }

        FirstRealBlock = InsertionPoint->getParent();

        std::vector<PHINode*> Phis;
        for (Instruction* I : Instructions) {
          if (PHINode* Phi = dyn_cast<PHINode>(I)) {
            Phis.push_back(Phi);
            continue;
          }
          if (!Phis.empty()) {
            for (PHINode* Phi : Phis)
              recordLowers(Phi, I);
            Phis.clear();
          }
          if (InvokeInst* II = dyn_cast<InvokeInst>(I))
            recordLowers(I, &*II->getNormalDest()->getFirstInsertionPt());
          else if (I->isTerminator())
            assert(I->getType()->isVoidTy());
          else
            recordLowers(I, I->getNextNode());
        }
        assert(Phis.empty());
        captureAuxBaseVars(Instructions);
        for (Instruction* I : Instructions)
          emitChecksForInst(I);
        erase_if(Instructions, [&] (Instruction* I) { return earlyLowerInstruction(I); });
        for (Instruction* I : Instructions)
          lowerInstruction(I, RawNull);

        GlobalVariable* NewObjectG = new GlobalVariable(
          M, ObjectTy, true, GlobalValue::InternalLinkage, nullptr, "Jfo_" + OldF->getName());
        Constant* LowerAndUpper =
          ConstantExpr::getGetElementPtr(ObjectTy, NewObjectG, ConstantInt::get(IntPtrTy, 1));
        uint16_t ObjectFlags =
          ObjectFlagGlobal |
          ObjectFlagReadonly |
          (SpecialTypeFunction << ObjectFlagsSpecialShift);
        Constant* NewObjC = ConstantStruct::get(
          ObjectTy,
          { LowerAndUpper,
            ConstantExpr::getGetElementPtr(
              Int8Ty, NewF,
              ConstantInt::get(
                IntPtrTy, static_cast<uintptr_t>(ObjectFlags) << ObjectAuxFlagsShift)) });
        NewObjectG->setInitializer(NewObjC);
        
        Function* GetterF = GlobalToGetter[OldF];
        assert(GetterF);
        assert(GetterF->isDeclaration());
        BasicBlock* RootBB = BasicBlock::Create(C, "filc_function_getter_root", GetterF);
        Return = ReturnInst::Create(C, UndefValue::get(FlightPtrTy), RootBB);
        Return->getOperandUse(0) = createFlightPtr(LowerAndUpper, NewF, Return);

        if (Setjmps.size())
          assert(NewF->callsFunctionThatReturnsTwice());
        
        if (verbose)
          errs() << "New function: " << *NewF << "\n";

        optimizeGetters();

        if (verbose)
          errs() << "New function after getter optimization: " << *NewF << "\n";
      }
      
      FunctionName = "<internal>";
      OldF = nullptr;
      NewF = nullptr;
    }
    for (GlobalAlias* G : Aliases) {
      Constant* C = G->getAliasee();
      Function* NewF = GlobalToGetter[G];
      Function* TargetF = GlobalToGetter[cast<GlobalValue>(C)];
      assert(NewF);
      assert(TargetF);
      BasicBlock* BB = BasicBlock::Create(this->C, "filc_alias_global", NewF);
      ReturnInst::Create(
        this->C,
        CallInst::Create(GlobalGetterTy, TargetF, { NewF->getArg(0) },
                         "filc_forward_global", BB),
        BB);
    }

    Dummy->deleteValue();

    if (verbose)
      errs() << "Deleting ToErase values.\n";
    for (Instruction* I : ToErase)
      I->deleteValue();
    if (verbose)
      errs() << "RAUWing ToDelete values.\n";
    for (GlobalValue* G : ToDelete)
      G->replaceAllUsesWith(UndefValue::get(G->getType())); // FIXME - should be zero
    if (verbose)
      errs() << "Erasing ToDelete values.\n";
    for (GlobalValue* G : ToDelete) {
      if (verbose)
        errs() << "Erasing " << G->getName() << "\n";
      G->eraseFromParent();
    }

    if (verbose)
      errs() << "Here's the pizlonated module:\n" << M << "\n";
    verifyModule(M);
  }
};

} // anonymous namespace

PreservedAnalyses FilPizlonatorPass::run(Module &M, ModuleAnalysisManager&) {
  Pizlonator P(M);
  P.run();
  return PreservedAnalyses::none();
}

