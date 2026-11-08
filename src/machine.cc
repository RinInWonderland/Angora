#include "machine.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <utility>

#include "log.h"

using std::fputs;
using std::printf;
using std::snprintf;
using std::strtod;

// useful macros
#define GET_INST(_code) static_cast<uint8_t>(_code & 0x7F)
#define GET_ARGS(_code) static_cast<uint32_t>(_code >> 7)
#define HAS_ARGS(_code) ((_code >> 7) != 0x0)
// Folded-instruction helpers: the low kVarBits of the argument are the variant
// (type / direction / bit-op); immediate folded forms carry the value above it.
#define GET_VAR(_code) (GET_ARGS(_code) & kVarMask)
#define GET_FOLDED_IMM(_code) (GET_ARGS(_code) >> kVarBits)

#define POP_VALUE_TO(_tmp) \
  _tmp = stack_.back();    \
  stack_.pop_back();

// Access the top / next-to-top slot without popping, so binary/unary
// operators can write their result in place and pop only what they must.
#define TOP() stack_.back()
#define NEX() stack_[stack_.size() - 2]

// Human-readable value type, used to describe mismatches in diagnostics.
static const char *TypeName(UnitType t) {
  switch (t) {
    case UnitType::Int:
      return "Int";
    case UnitType::UInt:
      return "UInt";
    case UnitType::FP:
      return "FP";
    case UnitType::Str:
      return "Str";
    case UnitType::Arr:
      return "Arr";
    case UnitType::Closure:
      return "Closure";
    case UnitType::Nil:
      return "Nil";
    case UnitType::Table:
      return "Table";
    default:
      return "Invalid";
  }
}

// Every read of a numeric payload must pass this guard first: nil (and Invalid,
// Str, Arr, Closure) are rejected instead of being silently treated as 0.
// Compile with -DANGORA_NO_GUARDS to drop the checks (trusted bytecode only).
#ifndef ANGORA_NO_GUARDS
static inline void RequireNumeric(const Unit &u, const char *what) {
  if (u.type != UnitType::Int && u.type != UnitType::UInt && u.type != UnitType::FP) {
    throw MachineException(std::string(what) + ": expected a number, got " + TypeName(u.type));
  }
}
#define REQUIRE_NUMERIC(_u, _what) RequireNumeric((_u), (_what))
#else
#define REQUIRE_NUMERIC(_u, _what) ((void)0)
#endif

// Mnemonic for an encoded instruction word.
static const char *InstName(Code c) {
  uint8_t op = GET_INST(c);
  return op < kInstStrings.size() ? kInstStrings[op] : "?";
}

// Best-effort name of the label/function enclosing `pc`: the label whose offset
// is the greatest one not exceeding `pc`, or "<unknown>" when there is none.
static const char *FuncForPc(const Labels &labels, uint64_t pc) {
  const string *best = nullptr;
  size_t best_off = 0;
  for (const auto &kv : labels) {
    if (kv.second <= pc && (best == nullptr || kv.second >= best_off)) {
      best = &kv.first;
      best_off = kv.second;
    }
  }
  return best != nullptr ? best->c_str() : "<unknown>";
}

// Folded arithmetic: the operator is the opcode and the variant selects int
// (kTyInt), uint (kTyUInt) or double (kTyFP). Both operands are on the stack
// (a = NEX, b = TOP); the result replaces them. Add/Sub/Mul/Div share infix
// operators, so one macro covers all three variants; Mod is spelled out below
// because the float form uses std::fmod instead of an infix operator.
#define ARITH_BINOP(_inst, _iop, _uop, _fop)          \
  case Inst::_inst: {                                 \
    Unit &a = NEX();                                  \
    REQUIRE_NUMERIC(a, "arith");                      \
    REQUIRE_NUMERIC(TOP(), "arith");                  \
    switch (GET_VAR(current)) {                       \
      case kTyInt:                                    \
        a.value.i = a.value.i _iop INTVAL(TOP());     \
        a.type = UnitType::Int;                       \
        break;                                        \
      case kTyUInt:                                   \
        a.value.ui = a.value.ui _uop UINTVAL(TOP());  \
        a.type = UnitType::UInt;                      \
        break;                                        \
      case kTyFP:                                     \
        a.value.fp = a.value.fp _fop FPVAL(TOP());    \
        a.type = UnitType::FP;                        \
        break;                                        \
      default:                                        \
        throw MachineException("bad arithmetic variant: variant=" + std::to_string(GET_VAR(current))); \
    }                                                 \
    stack_.pop_back();                                \
    break;                                            \
  }

#define ARITH_MOD()                                   \
  case Inst::Mod: {                                   \
    Unit &a = NEX();                                  \
    REQUIRE_NUMERIC(a, "mod");                        \
    REQUIRE_NUMERIC(TOP(), "mod");                    \
    switch (GET_VAR(current)) {                       \
      case kTyInt:                                    \
        a.value.i = INTVAL(a) % INTVAL(TOP());        \
        a.type = UnitType::Int;                       \
        break;                                        \
      case kTyUInt:                                   \
        a.value.ui = UINTVAL(a) % UINTVAL(TOP());     \
        a.type = UnitType::UInt;                      \
        break;                                        \
      case kTyFP:                                     \
        a.value.fp = std::fmod(FPVAL(a), FPVAL(TOP())); \
        a.type = UnitType::FP;                        \
        break;                                        \
      default:                                        \
        throw MachineException("bad modulo variant: variant=" + std::to_string(GET_VAR(current))); \
    }                                                 \
    stack_.pop_back();                                \
    break;                                            \
  }

// Folded comparison: like arithmetic but writes UInt 1/0 into the first slot.
#define CMP_BINOP(_inst, _sop, _uop, _fop)                         \
  case Inst::_inst: {                                              \
    Unit &a = NEX();                                               \
    REQUIRE_NUMERIC(a, "cmp");                                     \
    REQUIRE_NUMERIC(TOP(), "cmp");                                 \
    switch (GET_VAR(current)) {                                    \
      case kTyInt:                                                 \
        a.value.ui = (INTVAL(a) _sop INTVAL(TOP())) ? 1ull : 0ull; \
        break;                                                     \
      case kTyUInt:                                                \
        a.value.ui = (UINTVAL(a) _uop UINTVAL(TOP())) ? 1ull : 0ull; \
        break;                                                     \
      case kTyFP:                                                  \
        a.value.ui = (FPVAL(a) _fop FPVAL(TOP())) ? 1ull : 0ull;   \
        break;                                                     \
      default:                                                     \
        throw MachineException("bad comparison variant: variant=" + std::to_string(GET_VAR(current)));          \
    }                                                              \
    a.type = UnitType::UInt;                                       \
    stack_.pop_back();                                             \
    break;                                                         \
  }

// Folded shift: register form reads the count from TOP(), immediate form packs
// the count above the variant bits. The variant picks left / logic-right /
// arithmetic-right. The value type is left untouched, as before.
#define SHIFT_BINOP(_inst)                            \
  case Inst::_inst: {                                 \
    Unit &a = NEX();                                  \
    REQUIRE_NUMERIC(a, "shift");                      \
    REQUIRE_NUMERIC(TOP(), "shift");                  \
    uint32_t k = static_cast<uint32_t>(UINTVAL(TOP())); \
    switch (GET_VAR(current)) {                       \
      case kShiftL:                                   \
        a.value.ui <<= k;                             \
        break;                                        \
      case kShiftLsr:                                 \
        a.value.ui >>= k;                             \
        break;                                        \
      case kShiftAsr:                                 \
        a.value.i >>= k;                              \
        break;                                        \
      default:                                        \
        throw MachineException("bad shift variant: variant=" + std::to_string(GET_VAR(current)));  \
    }                                                 \
    stack_.pop_back();                                \
    break;                                            \
  }

#define SHIFT_IMM_BINOP(_inst)                          \
  case Inst::_inst: {                                   \
    Unit &a = stack_.back();                            \
    REQUIRE_NUMERIC(a, "shift");                        \
    uint32_t k = GET_FOLDED_IMM(current);               \
    switch (GET_VAR(current)) {                         \
      case kShiftL:                                     \
        a.value.ui <<= k;                               \
        break;                                          \
      case kShiftLsr:                                   \
        a.value.ui >>= k;                               \
        break;                                          \
      case kShiftAsr:                                   \
        a.value.i >>= k;                                \
        break;                                          \
      default:                                          \
        throw MachineException("bad shift-immediate variant: variant=" + std::to_string(GET_VAR(current))); \
    }                                                   \
    break;                                              \
  }

// Folded rotate: same shape as shift, over the unsigned bit pattern.
#define ROT_BINOP(_inst)                              \
  case Inst::_inst: {                                 \
    Unit &a = NEX();                                  \
    REQUIRE_NUMERIC(a, "rotate");                     \
    REQUIRE_NUMERIC(TOP(), "rotate");                 \
    switch (GET_VAR(current)) {                       \
      case kRotL:                                     \
        a.value.ui = std::rotl(a.value.ui, UINTVAL(TOP())); \
        break;                                        \
      case kRotR:                                     \
        a.value.ui = std::rotr(a.value.ui, UINTVAL(TOP())); \
        break;                                        \
      default:                                        \
        throw MachineException("bad rotate variant: variant=" + std::to_string(GET_VAR(current))); \
    }                                                 \
    a.type = UnitType::UInt;                          \
    stack_.pop_back();                                \
    break;                                            \
  }

#define ROT_IMM_BINOP(_inst)                          \
  case Inst::_inst: {                                 \
    Unit &a = stack_.back();                          \
    REQUIRE_NUMERIC(a, "rotate");                     \
    uint32_t k = GET_FOLDED_IMM(current);             \
    if (GET_VAR(current) == kRotL) {                  \
      a.value.ui = std::rotl(a.value.ui, k);          \
    } else {                                          \
      a.value.ui = std::rotr(a.value.ui, k);          \
    }                                                 \
    a.type = UnitType::UInt;                          \
    break;                                            \
  }

// Folded bitwise binary op: variant selects and / or / xor.
#define BIT_BINOP(_inst)                              \
  case Inst::_inst: {                                 \
    Unit &a = NEX();                                  \
    REQUIRE_NUMERIC(a, "bitop");                      \
    REQUIRE_NUMERIC(TOP(), "bitop");                  \
    switch (GET_VAR(current)) {                       \
      case kBitAnd:                                   \
        a.value.ui &= UINTVAL(TOP());                 \
        break;                                        \
      case kBitOr:                                    \
        a.value.ui |= UINTVAL(TOP());                 \
        break;                                        \
      case kBitXor:                                   \
        a.value.ui ^= UINTVAL(TOP());                 \
        break;                                        \
      default:                                        \
        throw MachineException("bad bit-op variant: variant=" + std::to_string(GET_VAR(current))); \
    }                                                 \
    a.type = UnitType::UInt;                          \
    stack_.pop_back();                                \
    break;                                            \
  }

// Bounds enforced only by `repr`, so a huge structure cannot flood stdout.
// Plain `print` prints everything.
constexpr int kMaxReprDepth = 6;
constexpr size_t kMaxReprItems = 32;

// Emit a floating-point value in the shortest decimal form that parses back to
// the exact same double. `%.*g` is tried at increasing precision and the first
// representation that round-trips wins; non-finite values are named explicitly.
static void PrintFP(double v) {
  if (std::isnan(v)) {
    fputs("nan", stdout);
    return;
  }
  if (std::isinf(v)) {
    fputs(v < 0 ? "-inf" : "inf", stdout);
    return;
  }
  char buf[32];
  int n = 0;
  for (int prec = 1; prec <= 17; prec += 1) {
    n = snprintf(buf, sizeof(buf), "%.*g", prec, v);
    if (strtod(buf, nullptr) == v) {
      break;
    }
  }
  fwrite(buf, 1, static_cast<size_t>(n), stdout);
}

// Emit a string with surrounding double quotes and C-style escapes. Used for
// strings nested inside an array and for every string in `repr`.
static void PrintStrQuoted(const char *s, size_t len) {
  putchar('"');
  for (size_t i = 0; i < len; ++i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    switch (c) {
      case '"':
        fputs("\\\"", stdout);
        break;
      case '\\':
        fputs("\\\\", stdout);
        break;
      case '\n':
        fputs("\\n", stdout);
        break;
      case '\t':
        fputs("\\t", stdout);
        break;
      case '\r':
        fputs("\\r", stdout);
        break;
      default:
        if (c < 0x20) {
          printf("\\x%02x", c);
        } else {
          putchar(c);
        }
        break;
    }
  }
  putchar('"');
}

// Print one value, recursing into arrays (e.g. [1, [2, 3]]). `repr` selects the
// unambiguous form (strings always quoted, arrays bounded by depth/length);
// `nested` marks values inside an array, whose strings are quoted even for plain
// `print`; `depth` is the current array nesting level.
static void PrintUnit(const Unit &u, bool repr, bool nested, int depth) {
  switch (u.type) {
    case UnitType::Int:
      printf("%lld", static_cast<long long>(u.value.i));
      break;
    case UnitType::UInt:
      printf("%llu", static_cast<unsigned long long>(u.value.ui));
      break;
    case UnitType::FP:
      PrintFP(u.value.fp);
      break;
    case UnitType::Str: {
      const char *s = u.value.str;
      if (repr || nested) {
        PrintStrQuoted(s, GcLenOf(s));
      } else {
        fwrite(s, 1, GcLenOf(s), stdout);
      }
      break;
    }
    case UnitType::Arr: {
      ArrObj *a = u.value.arr;
      if (repr && depth >= kMaxReprDepth) {
        fputs("...", stdout);
        break;
      }
      putchar('[');
      size_t shown = a->length;
      if (repr && shown > kMaxReprItems) {
        shown = kMaxReprItems;
      }
      for (uint32_t i = 0; i < shown; ++i) {
        if (i != 0) {
          printf(", ");
        }
        PrintUnit(a->data->items[i], repr, true, depth + 1);
      }
      if (repr && a->length > shown) {
        printf(", ... (%u more)", static_cast<unsigned>(a->length - shown));
      }
      putchar(']');
      break;
    }
    case UnitType::Closure: {
      ClosureObj *c = CLOSUREVAL(u);
      printf("<closure@%u nupvals=%zu>", c->entry_pc, GcClosureNupvals(c));
      break;
    }
    case UnitType::Nil:
      fputs("nil", stdout);
      break;
    case UnitType::Table: {
      TableObj *t = TABLEVAL(u);
      if (depth >= kMaxReprDepth) {
        fputs("...", stdout);
        break;
      }
      putchar('{');
      Unit key, value;
      Unit cursor = Unit::MakeNil();
      uint32_t shown = 0;
      bool more = false;
      while (table::Next(t, cursor, key, value)) {
        if (shown != 0) {
          printf(", ");
        }
        PrintUnit(key, repr, true, depth + 1);
        printf(" = ");
        PrintUnit(value, repr, true, depth + 1);
        cursor = key;
        shown += 1;
        if (shown >= kMaxReprItems) {
          more = true;
          break;
        }
      }
      if (more) {
        printf(", ...");
      }
      putchar('}');
      break;
    }
    default:
      printf("<invalid>");
      break;
  }
}

bool Machine::Run(Program &prog) {
  bool result = true;

  // reset state
  pc_ = 0;
  stack_.clear();
  stack_.reserve(512);
  call_stack_.clear();

  // Default entry point: label "main". If found, jump there first;
  // otherwise fall back to PC=0 (legacy behaviour).
  auto it = labels_.find("main");
  if (it != labels_.end()) {
    pc_ = static_cast<uint64_t>(it->second);
  }

  auto prog_size = prog.size();
  Unit tmp0, tmp1;  // Register-like temp variable for operations.
  Code current = 0; // Current executed instruction.

  // Emit a runtime diagnostic: the faulting pc/mnemonic, the message, and a
  // best-effort call stack (nearest call site first).
  auto report_error = [&](const char *prefix, const char *msg) {
    LogWrite(LogSeverity::Error, "angora-runtime", {}, "pc=%llu (%s): %s%s",
             static_cast<unsigned long long>(pc_), InstName(current), prefix, msg);
    if (!call_stack_.empty()) {
      LogWrite(LogSeverity::Info, "angora-runtime", {}, "call stack (most recent first):");
      for (size_t i = call_stack_.size(); i-- > 0;) {
        uint32_t ret = call_stack_[i].ret_pc;
        uint32_t site = ret > 0 ? ret - 1 : 0;
        LogWrite(LogSeverity::Info, "angora-runtime", {}, "  #%zu %s called at pc=%u",
                 call_stack_.size() - i, FuncForPc(labels_, site), site);
      }
    }
  };

  // Shared bump-allocator body for `alloc` / `alloci`: bump `n` zeroed words
  // over the growable data segment and push the resulting start index onto the
  // value stack. Reclaim is handled by the tracing GC, not a per-arena free
  // stack, so `alloc`/`alloci` only ever extend the arena.
  auto bump_alloc = [&](uint64_t n) {
    uint64_t start = mem_.size();
    if (n > 0) {
      mem_.resize(start + n);
    }
    stack_.push_back(Unit::MakeUInt(start));
  };

  try {
    // Collect once enough fresh garbage has accumulated (bytes handed out since
    // the last cycle). Instruction boundaries are safe points: no `Unit&`
    // reference outlives the loop body, so marks/sweep see only stable roots
    // (stack_, mem_, consts_).
    constexpr size_t kGcBytesThreshold = 128u * 1024u;
    while (pc_ < prog_size) {
      if (gc_.BytesSinceCollect() >= kGcBytesThreshold) {
        gc_.Collect(stack_, mem_, consts_, call_stack_);
      }
      current = prog[pc_];
      switch (static_cast<Inst>(GET_INST(current))) {
        ARITH_BINOP(Add, +, +, +)
        ARITH_BINOP(Sub, -, -, -)
        ARITH_BINOP(Mul, *, *, *)
        ARITH_BINOP(Div, /, /, /)
        ARITH_MOD()
        case Inst::PushHalfWordImm: {
          stack_.push_back(Unit::MakeUInt(GET_ARGS(current)));
          break;
        }
        case Inst::PushHalfWordImmSL16: {
          UINTVAL(tmp0) = GET_ARGS(current);
          UINTVAL(tmp0) <<= 16;
          stack_.push_back(Unit::MakeUInt(UINTVAL(tmp0)));
          break;
        }
        case Inst::SpawnFP: {
          REQUIRE_NUMERIC(stack_.back(), "spawnfp");
          stack_.back().type = UnitType::FP;
          break;
        }
        case Inst::SpawnSignedInt: {
          REQUIRE_NUMERIC(stack_.back(), "spawnsint");
          stack_.back().type = UnitType::Int;
          break;
        }
        case Inst::PushNil: {
          stack_.push_back(Unit::MakeNil());
          break;
        }
        case Inst::IsNil: {
          Unit &a = stack_.back();
          a.value.ui = (a.type == UnitType::Nil) ? 1ull : 0ull;
          a.type = UnitType::UInt;
          break;
        }
        case Inst::Jump: {
          pc_ = GET_ARGS(current);
          continue;
        }
        case Inst::Branch: {
          if (!stack_.empty()) {
            if (IsTruthy(stack_.back())) {
              pc_ = GET_ARGS(current);
              continue;
            }
          }
          break;
        }
        case Inst::FarJump: {
          POP_VALUE_TO(tmp0);
          REQUIRE_NUMERIC(tmp0, "farjmp");
          pc_ = UINTVAL(tmp0);
          continue;
        }
        case Inst::FarBranch: {
          // addr
          POP_VALUE_TO(tmp1);
          REQUIRE_NUMERIC(tmp1, "farbranch");
          // condition
          POP_VALUE_TO(tmp0);
          if (IsTruthy(tmp0)) {
            pc_ = UINTVAL(tmp1);
            continue;
          }
          break;
        }
        case Inst::Call: {
          // Record an activation record before jumping: the resume pc, the
          // current watermarks of both stacks, and the caller's closure (a
          // statically-called function has no captured environment, so the
          // running closure becomes null until `ret` restores this frame).
          call_stack_.push_back(CallFrame{
              static_cast<uint32_t>(pc_ + 1),
              static_cast<uint32_t>(stack_.size()),
              static_cast<uint32_t>(mem_.size()),
              nullptr,
          });
          pc_ = GET_ARGS(current);
          continue;
        }
        case Inst::ClaimArgs: {
          // `claim <argc>`: lower the frame base below the arguments so the
          // return values end up where the arguments were. The arguments
          // themselves are already part of the frame the caller set up.
          if (call_stack_.empty()) {
            throw MachineException("claim: empty call stack");
          }
          uint32_t argc = GET_ARGS(current);
          CallFrame &frame = call_stack_.back();
          if (argc > frame.stack_mark) {
            throw MachineException("claim: argc=" + std::to_string(argc) +
                                   " exceeds frame size=" + std::to_string(frame.stack_mark));
          }
          frame.stack_mark -= argc;
          break;
        }
        case Inst::Ret: {
          if (call_stack_.empty()) {
            throw MachineException("ret: empty call stack");
          }
          const uint32_t nret = GET_ARGS(current);
          CallFrame frame = call_stack_.back();
          call_stack_.pop_back();

          // Keep the top `nret` results and drop everything else the callee
          // left, unwinding the value stack to the frame base. The results are
          // relocated downward (dest <= src) in ascending order, which is
          // memmove-safe and needs no temporary buffer.
          if (nret > stack_.size()) {
            throw MachineException("ret: nret=" + std::to_string(nret) +
                                   " exceeds stack size=" + std::to_string(stack_.size()));
          }
          const size_t base = frame.stack_mark;
          const size_t src = stack_.size() - nret;
          if (src < base) {
            throw MachineException("ret: return values below frame base=" + std::to_string(base));
          }
          for (size_t i = 0; i < nret; ++i) {
            stack_[base + i] = std::move(stack_[src + i]);
          }
          stack_.resize(base + nret);

          // Unwind the data-segment arena: everything the callee bump-allocated
          // above its frame is dropped. Capacity is retained, so the next call
          // reuses the same slots instead of growing mem_ forever.
          mem_.resize(frame.mem_mark);

          pc_ = frame.ret_pc;
          continue;
        }
        case Inst::ClosureNew: {
          // Stack: up1 ... upN count  (count on top) -> closure. The first
          // pushed value becomes upvals[0]; the entry is the instruction arg.
          if (stack_.empty()) {
            throw MachineException("closnew: empty stack");
          }
          REQUIRE_NUMERIC(stack_.back(), "closnew");
          uint32_t n = static_cast<uint32_t>(UINTVAL(stack_.back()));
          stack_.pop_back();
          if (n > stack_.size()) {
            throw MachineException("closnew: capture count=" + std::to_string(n) +
                                   " exceeds stack size=" + std::to_string(stack_.size()));
          }
          ClosureObj *clo = gc_.AllocClosure(GET_ARGS(current), n);
          for (uint32_t i = n; i-- > 0;) {
            clo->upvals[i] = std::move(stack_.back());
            stack_.pop_back();
          }
          stack_.push_back(Unit::MakeClosure(clo));
          break;
        }
        case Inst::CallClosure: {
          // Stack: arg1 ... argN closure (closure on top). The frame records
          // the watermarks above the arguments, exactly like a static `call`;
          // the running closure is held by the frame so `closget` reads it and
          // the collector treats it as a root.
          if (stack_.empty() || stack_.back().type != UnitType::Closure) {
            throw MachineException(
                "callc: expected Closure, got " +
                std::string(stack_.empty() ? "empty stack" : TypeName(stack_.back().type)));
          }
          ClosureObj *clo = CLOSUREVAL(stack_.back());
          stack_.pop_back();
          call_stack_.push_back(CallFrame{
              static_cast<uint32_t>(pc_ + 1),
              static_cast<uint32_t>(stack_.size()),
              static_cast<uint32_t>(mem_.size()),
              clo,
          });
          pc_ = clo->entry_pc;
          continue;
        }
        case Inst::ClosureGet: {
          // Push captured value i of the closure currently executing.
          if (call_stack_.empty() || call_stack_.back().closure == nullptr) {
            throw MachineException("closget: no active closure");
          }
          ClosureObj *clo = call_stack_.back().closure;
          uint32_t i = GET_ARGS(current);
          if (i >= GcClosureNupvals(clo)) {
            throw MachineException("closget: upvalue index=" + std::to_string(i) +
                                   " out of range (nupvals=" + std::to_string(GcClosureNupvals(clo)) +
                                   ")");
          }
          stack_.push_back(clo->upvals[i]);
          break;
        }
        case Inst::Exit: {
          pc_ = prog.size();
          continue;
        }
        case Inst::Pop: {
          if (!stack_.empty()) {
            stack_.pop_back();
          }
          break;
        }
        case Inst::Print: {
          if (!stack_.empty()) {
            PrintUnit(stack_.back(), /*repr=*/false, /*nested=*/false, 0);
            putchar('\n');
          } else {
            throw MachineException("print: empty stack");
          }
          break;
        }
        case Inst::Repr: {
          if (!stack_.empty()) {
            PrintUnit(stack_.back(), /*repr=*/true, /*nested=*/false, 0);
            putchar('\n');
          } else {
            throw MachineException("repr: empty stack");
          }
          break;
        }
        SHIFT_BINOP(Shift)
        SHIFT_IMM_BINOP(ShiftImm)

        BIT_BINOP(BitOp)
        case Inst::Not: {
          Unit &a = stack_.back();
          REQUIRE_NUMERIC(a, "not");
          a.value.ui = ~a.value.ui;
          a.type = UnitType::UInt;
          break;
        }
        case Inst::EqualsZero: {
          Unit &a = stack_.back();
          REQUIRE_NUMERIC(a, "eqz");
          a.value.ui = (a.value.ui == 0);
          a.type = UnitType::UInt;
          break;
        }
        // Comparison instructions: NEX() = X (stack[top-1]),
        // TOP() = Y (stack[top]); push 1 if X <op> Y, else 0 (UInt), with the
        // variant selecting signed / unsigned / floating-point operands.
        CMP_BINOP(Lt, <, <, <)
        CMP_BINOP(Le, <=, <=, <=)
        CMP_BINOP(Gt, >, >, >)
        CMP_BINOP(Ge, >=, >=, >=)

        // Rotate, using C++20 std::rotl/rotr for UB-free semantics.
        ROT_BINOP(Rotate)
        ROT_IMM_BINOP(RotateImm)

        case Inst::Swap: {
          std::swap(stack_[stack_.size() - 1], stack_[stack_.size() - 2]);
          break;
        }
        case Inst::Dup: {
          stack_.push_back(stack_.back());
          break;
        }
        case Inst::DupN: {
          UINTVAL(tmp0) = GET_ARGS(current);
          for (size_t i = 0; i < UINTVAL(tmp0); i += 1) {
            stack_.push_back(stack_.back());
          }
          break;
        }
        case Inst::Load: {
          size_t idx = static_cast<size_t>(GET_ARGS(current));
          if (idx >= mem_.size()) {
            throw MachineException("ld: index=" + std::to_string(idx) +
                                   " out of bounds (mem size=" + std::to_string(mem_.size()) + ")");
          }
          // Copy the slot onto the stack: the copy ctor shallow-shares any
          // string; both the slot and the stack entry are GC roots.
          stack_.push_back(mem_[idx]);
          break;
        }
        case Inst::Store: {
          size_t idx = GET_ARGS(current);
          if (idx >= mem_.size()) {
            throw MachineException("st: index=" + std::to_string(idx) +
                                   " out of bounds (mem size=" + std::to_string(mem_.size()) + ")");
          }
          // Move-assign drops the slot's old value (the overwritten string,
          // if now unreachable, is reclaimed by the next sweep) and steals
          // the value from the stack top (a shallow pointer move under GC).
          mem_[idx] = std::move(stack_.back());
          stack_.pop_back();
          break;
        }
        case Inst::LoadConst: {
          size_t idx = static_cast<size_t>(GET_ARGS(current));
          if (idx >= consts_.size()) {
            throw MachineException("ldc: index=" + std::to_string(idx) +
                                   " out of bounds (const pool size=" +
                                   std::to_string(consts_.size()) + ")");
          }
          // Copy the pool entry onto the stack, preserving its type; a string
          // entry shallow-shares the pool's GC string, which stays rooted.
          stack_.push_back(consts_[idx]);
          break;
        }
        case Inst::RelativeLoad: {
          uint32_t offset = GET_ARGS(current);
          REQUIRE_NUMERIC(TOP(), "ldr");
          uint64_t base_val = UINTVAL(TOP());
          size_t idx = static_cast<size_t>(base_val + offset);
          if (idx >= mem_.size()) {
            throw MachineException("ldr: index=" + std::to_string(idx) + " out of bounds (base=" +
                                   std::to_string(base_val) + " offset=" + std::to_string(offset) +
                                   " mem size=" + std::to_string(mem_.size()) + ")");
          }
          stack_.push_back(mem_[idx]);
          break;
        }
        case Inst::RelativeStore: {
          uint32_t offset = GET_ARGS(current);
          REQUIRE_NUMERIC(NEX(), "str");
          uint64_t base_val = UINTVAL(NEX());
          size_t idx = static_cast<size_t>(base_val + offset);
          if (idx >= mem_.size()) {
            throw MachineException("str: index=" + std::to_string(idx) + " out of bounds (base=" +
                                   std::to_string(base_val) + " offset=" + std::to_string(offset) +
                                   " mem size=" + std::to_string(mem_.size()) + ")");
          }
          mem_[idx] = std::move(stack_.back());
          stack_.pop_back();
          break;
        }
        case Inst::Alloc: {
          // `alloc`: the word count is popped from the stack top (TOP());
          // the remaining bump/reuse behaviour is identical to `alloci`.
          if (stack_.empty()) {
            throw MachineException("alloc: empty stack");
          }
          REQUIRE_NUMERIC(stack_.back(), "alloc");
          uint64_t n = UINTVAL(stack_.back());
          stack_.pop_back();
          bump_alloc(n);
          break;
        }
        case Inst::AllocImm: {
          // `alloci`: the word count comes from the instruction's immediate.
          bump_alloc(GET_ARGS(current));
          break;
        }
        case Inst::StringLen: {
          // Replace the top string with its length (UInt); length comes from the
          // GC header, so this is O(1) with no scan for NUL.
          Unit &a = stack_.back();
          if (a.type != UnitType::Str) {
            throw MachineException("strlen: expected Str, got " + std::string(TypeName(a.type)));
          }
          a.value.ui = GcLenOf(STRVAL(a));
          a.type = UnitType::UInt;
          break;
        }
        case Inst::StringChar: {
          // string on NEX, index on top; reuse the string slot for the result
          Unit &str = NEX();
          if (str.type != UnitType::Str) {
            throw MachineException("strchar: expected Str, got " + std::string(TypeName(str.type)));
          }
          REQUIRE_NUMERIC(TOP(), "strchar");
          const char *s = STRVAL(str);
          if (UINTVAL(TOP()) >= GcLenOf(s)) {
            throw MachineException("strchar: index=" + std::to_string(UINTVAL(TOP())) +
                                   " out of range (length=" + std::to_string(GcLenOf(s)) + ")");
          }
          int64_t c = int64_t(s[UINTVAL(TOP())]);
          // Overwrite the slot with the int result; Unit assignment is a plain
          // value copy now that strings are GC-managed, not refcounted.
          str = Unit::MakeInt(c);
          stack_.pop_back();
          break;
        }
        case Inst::StringConcat: {
          // 1st string on NEX (becomes the result slot), 2nd string on top
          Unit &a = NEX();
          Unit &b = stack_.back();
          if (a.type != UnitType::Str || b.type != UnitType::Str) {
            throw MachineException("strconcat: expected two Str, got a=" +
                                   std::string(TypeName(a.type)) + " b=" + TypeName(b.type));
          }
          // Build the result directly in the GC heap: one allocation, two copies,
          // no intermediate std::string. Both operands are roots, so allocating
          // cannot reclaim them; read their pointers before writing the result.
          const char *pa = STRVAL(a);
          const char *pb = STRVAL(b);
          size_t la = GcLenOf(pa);
          size_t lb = GcLenOf(pb);
          char *buf = gc_.AllocRaw(la + lb);
          memcpy(buf, pa, la);
          memcpy(buf + la, pb, lb);
          buf[la + lb] = '\0';
          a.value.str = buf;
          a.type = UnitType::Str;
          stack_.pop_back();  // releases the 2nd operand
          break;
        }
        case Inst::StringSetChar: {
          POP_VALUE_TO(tmp1);  // char
          POP_VALUE_TO(tmp0);  // index
                               // now, stack top is target string.
          if (stack_.back().type != UnitType::Str) {
            throw MachineException("strsetchar: expected Str, got " +
                                   std::string(TypeName(stack_.back().type)));
          }
          REQUIRE_NUMERIC(tmp0, "strsetchar");
          REQUIRE_NUMERIC(tmp1, "strsetchar");
          if (tmp0.value.ui >= GcLenOf(STRVAL(stack_.back()))) {
            throw MachineException("strsetchar: index=" + std::to_string(tmp0.value.ui) +
                                   " out of range (length=" +
                                   std::to_string(GcLenOf(STRVAL(stack_.back()))) + ")");
          }
          char val = static_cast<char>(tmp1.value.i);
          STRVAL(stack_.back())[tmp0.value.ui] = val;
          break;
        }
        case Inst::StringCopy: {
          // Copy the string at TOP() into a freshly allocated buffer, wrap it
          // in a new Unit, and push it; the original string stays on the stack.
          if (stack_.empty() || stack_.back().type != UnitType::Str) {
            throw MachineException(
                "strcpy: expected Str, got " +
                std::string(stack_.empty() ? "empty stack" : TypeName(stack_.back().type)));
          }
          const char *s = STRVAL(TOP());
          Unit result = Unit::MakeStr(gc_, s, GcLenOf(s));
          stack_.push_back(result);
          break;
        }
        case Inst::MemCopy: {
          if (stack_.size() < 3) {
            throw MachineException("memcpy: needs 3 operands, got " + std::to_string(stack_.size()));
          }
          // Pop in the order len, src, dst (len on stack top), per C memcpy.
          POP_VALUE_TO(tmp1);  // len
          REQUIRE_NUMERIC(tmp1, "memcpy");
          uint64_t len = UINTVAL(tmp1);
          REQUIRE_NUMERIC(TOP(), "memcpy");
          uint64_t src = UINTVAL(TOP());
          POP_VALUE_TO(tmp0);  // src
          REQUIRE_NUMERIC(TOP(), "memcpy");
          uint64_t dst = UINTVAL(TOP());
          stack_.pop_back();  // dst

          if (src + len > mem_.size() || dst + len > mem_.size()) {
            throw MachineException("memcpy: out of bounds (src=" + std::to_string(src) +
                                   " dst=" + std::to_string(dst) + " len=" + std::to_string(len) +
                                   " mem size=" + std::to_string(mem_.size()) + ")");
          }

          // Copy via a temporary so overlapping ranges behave like memmove;
          // each Unit's copy-assign is a shallow alias copy under the GC.
          vector<Unit> tmp_slice;
          for (uint64_t i = 0; i < len; ++i) {
            tmp_slice.push_back(mem_[src + i]);
          }
          for (uint64_t i = 0; i < len; ++i) {
            mem_[dst + i] = std::move(tmp_slice[i]);
          }
          break;
        }
        case Inst::ArrNew: {
          stack_.push_back(Unit::MakeArr(gc_, 0));
          break;
        }
        case Inst::ArrLen: {
          if (stack_.empty() || stack_.back().type != UnitType::Arr) {
            throw MachineException(
                "arrlen: expected Arr, got " +
                std::string(stack_.empty() ? "empty stack" : TypeName(stack_.back().type)));
          }
          // Keep the array on the stack and push its length on top.
          stack_.push_back(Unit::MakeUInt(ARRVAL(stack_.back())->length));
          break;
        }
        case Inst::ArrGet: {
          // arr on NEX, index on top; replace the index with the element.
          if (stack_.size() < 2 || NEX().type != UnitType::Arr) {
            throw MachineException(
                "arrget: expected Arr, got " +
                std::string(stack_.size() < 2 ? "missing operands" : TypeName(NEX().type)));
          }
          ArrObj *arr = ARRVAL(NEX());
          REQUIRE_NUMERIC(TOP(), "arrget");
          uint64_t idx = UINTVAL(TOP());
          if (idx >= arr->length) {
            throw MachineException("arrget: index=" + std::to_string(idx) +
                                   " out of range (length=" + std::to_string(arr->length) + ")");
          }
          Unit elem = arr->data->items[idx];  // shallow alias copy
          stack_.pop_back();                  // drop the index -> [arr]
          stack_.push_back(elem);             // -> [arr, elem]
          break;
        }
        case Inst::ArrPut: {
          // [arr, idx, val] -> [arr, idx]; bounded write (use arrpush to grow).
          if (stack_.size() < 3 || stack_[stack_.size() - 3].type != UnitType::Arr) {
            throw MachineException(
                "arrput: expected Arr, got " +
                std::string(stack_.size() < 3 ? "missing operands"
                                              : TypeName(stack_[stack_.size() - 3].type)));
          }
          ArrObj *arr = ARRVAL(stack_[stack_.size() - 3]);
          REQUIRE_NUMERIC(stack_[stack_.size() - 2], "arrput");
          uint64_t idx = UINTVAL(stack_[stack_.size() - 2]);
          if (idx >= arr->length) {
            throw MachineException("arrput: index=" + std::to_string(idx) +
                                   " out of range (length=" + std::to_string(arr->length) + ")");
          }
          arr->data->items[idx] = stack_.back();  // shallow alias copy
          stack_.pop_back();                      // drop the value -> [arr, idx]
          break;
        }
        case Inst::ArrPush: {
          // arr on NEX, value on top; append and drop the value -> [arr].
          if (stack_.size() < 2 || NEX().type != UnitType::Arr) {
            throw MachineException(
                "arrpush: expected Arr, got " +
                std::string(stack_.size() < 2 ? "missing operands" : TypeName(NEX().type)));
          }
          ArrObj *arr = ARRVAL(NEX());
          gc_.ArrReserve(arr, arr->length + 1);
          arr->data->items[arr->length] = stack_.back();
          arr->length += 1;
          stack_.pop_back();
          break;
        }
        case Inst::ArrPop: {
          if (stack_.empty() || stack_.back().type != UnitType::Arr) {
            throw MachineException(
                "arrpop: expected Arr, got " +
                std::string(stack_.empty() ? "empty stack" : TypeName(stack_.back().type)));
          }
          ArrObj *arr = ARRVAL(stack_.back());
          if (arr->length == 0) {
            throw MachineException("arrpop: empty array");
          }
          arr->length -= 1;
          Unit elem = arr->data->items[arr->length];
          arr->data->items[arr->length] = Unit();  // clear so the slot is not traced
          stack_.push_back(elem);
          break;
        }
        case Inst::ArrCopy: {
          // Shallow copy: a new array with the same (aliased) elements; the
          // original stays on the stack, the copy is pushed.
          if (stack_.empty() || stack_.back().type != UnitType::Arr) {
            throw MachineException(
                "arrcpy: expected Arr, got " +
                std::string(stack_.empty() ? "empty stack" : TypeName(stack_.back().type)));
          }
          ArrObj *src = ARRVAL(stack_.back());
          Unit result = Unit::MakeArr(gc_, src->length);
          ArrObj *dst = ARRVAL(result);
          dst->length = src->length;
          for (uint32_t i = 0; i < src->length; ++i) {
            dst->data->items[i] = src->data->items[i];
          }
          stack_.push_back(result);
          break;
        }

        case Inst::TableNew: {
          stack_.push_back(Unit::MakeTable(gc_.AllocTable()));
          break;
        }
        case Inst::TableLen: {
          if (stack_.empty() || stack_.back().type != UnitType::Table) {
            throw MachineException(
                "tbllen: expected Table, got " +
                std::string(stack_.empty() ? "empty stack" : TypeName(stack_.back().type)));
          }
          stack_.push_back(Unit::MakeUInt(table::Size(TABLEVAL(stack_.back()))));
          break;
        }
        case Inst::TableGet: {
          // tbl on NEX, key on top; replace the key with the value (or nil).
          if (stack_.size() < 2 || NEX().type != UnitType::Table) {
            throw MachineException(
                "tblget: expected Table, got " +
                std::string(stack_.size() < 2 ? "missing operands" : TypeName(NEX().type)));
          }
          TableObj *t = TABLEVAL(NEX());
          if (!table::ValidKey(TOP())) {
            throw MachineException("tblget: invalid key (nil/NaN)");
          }
          Unit value;
          if (table::Get(t, TOP(), value)) {
            TOP() = value;
          } else {
            TOP() = Unit::MakeNil();
          }
          break;
        }
        case Inst::TableGetInt: {
          if (stack_.size() < 2 || NEX().type != UnitType::Table) {
            throw MachineException(
                "tblgeti: expected Table, got " +
                std::string(stack_.size() < 2 ? "missing operands" : TypeName(NEX().type)));
          }
          Unit &key = TOP();
          if (key.type != UnitType::Int && key.type != UnitType::UInt) {
            throw MachineException("tblgeti: expected integer key, got " +
                                   std::string(TypeName(key.type)));
          }
          TableObj *t = TABLEVAL(NEX());
          Unit value;
          if (table::Get(t, key, value)) {
            key = value;
          } else {
            key = Unit::MakeNil();
          }
          break;
        }
        case Inst::TablePut: {
          // [tbl, key, val] -> [tbl, key]; val nil deletes.
          if (stack_.size() < 3 || stack_[stack_.size() - 3].type != UnitType::Table) {
            throw MachineException(
                "tblput: expected Table, got " +
                std::string(stack_.size() < 3 ? "missing operands"
                                              : TypeName(stack_[stack_.size() - 3].type)));
          }
          TableObj *t = TABLEVAL(stack_[stack_.size() - 3]);
          Unit &key = stack_[stack_.size() - 2];
          if (!table::ValidKey(key)) {
            throw MachineException("tblput: invalid key (nil/NaN)");
          }
          table::Put(gc_, t, key, stack_.back());
          stack_.pop_back();
          break;
        }
        case Inst::TablePutInt: {
          if (stack_.size() < 3 || stack_[stack_.size() - 3].type != UnitType::Table) {
            throw MachineException(
                "tblputi: expected Table, got " +
                std::string(stack_.size() < 3 ? "missing operands"
                                              : TypeName(stack_[stack_.size() - 3].type)));
          }
          Unit &key = stack_[stack_.size() - 2];
          if (key.type != UnitType::Int && key.type != UnitType::UInt) {
            throw MachineException("tblputi: expected integer key, got " +
                                   std::string(TypeName(key.type)));
          }
          TableObj *t = TABLEVAL(stack_[stack_.size() - 3]);
          table::Put(gc_, t, key, stack_.back());
          stack_.pop_back();
          break;
        }
        case Inst::TableHas: {
          // [tbl, key] -> [tbl, found]
          if (stack_.size() < 2 || NEX().type != UnitType::Table) {
            throw MachineException(
                "tblhas: expected Table, got " +
                std::string(stack_.size() < 2 ? "missing operands" : TypeName(NEX().type)));
          }
          TableObj *t = TABLEVAL(NEX());
          if (!table::ValidKey(TOP())) {
            throw MachineException("tblhas: invalid key (nil/NaN)");
          }
          bool found = table::Has(t, TOP());
          TOP() = Unit::MakeUInt(found ? 1u : 0u);
          break;
        }
        case Inst::TableErase: {
          // [tbl, key] -> [tbl]
          if (stack_.size() < 2 || NEX().type != UnitType::Table) {
            throw MachineException(
                "tblerase: expected Table, got " +
                std::string(stack_.size() < 2 ? "missing operands" : TypeName(NEX().type)));
          }
          TableObj *t = TABLEVAL(NEX());
          if (!table::ValidKey(TOP())) {
            throw MachineException("tblerase: invalid key (nil/NaN)");
          }
          table::Erase(t, TOP());
          stack_.pop_back();
          break;
        }
        case Inst::TableCopy: {
          if (stack_.empty() || stack_.back().type != UnitType::Table) {
            throw MachineException(
                "tblcpy: expected Table, got " +
                std::string(stack_.empty() ? "empty stack" : TypeName(stack_.back().type)));
          }
          TableObj *src = TABLEVAL(stack_.back());
          stack_.push_back(Unit::MakeTable(table::Copy(gc_, src)));
          break;
        }
        case Inst::TableNext: {
          // [tbl, key] -> [tbl, nextkey, nextval]; at the end both are nil.
          if (stack_.size() < 2 || NEX().type != UnitType::Table) {
            throw MachineException(
                "tblnext: expected Table, got " +
                std::string(stack_.size() < 2 ? "missing operands" : TypeName(NEX().type)));
          }
          TableObj *t = TABLEVAL(NEX());
          Unit out_key, out_value;
          bool more = table::Next(t, TOP(), out_key, out_value);
          stack_.pop_back();  // drop the cursor key
          if (more) {
            stack_.push_back(out_key);
            stack_.push_back(out_value);
          } else {
            stack_.push_back(Unit::MakeNil());
            stack_.push_back(Unit::MakeNil());
          }
          break;
        }

        case Inst::GcStat: {
          // Push two UInt on top: total allocated so far (gc_.Allocated()) then
          // reachable at the last sweep (gc_.Live()). The last pushed sits on top.
          stack_.push_back(Unit::MakeUInt(gc_.Allocated()));
          stack_.push_back(Unit::MakeUInt(gc_.Live()));
          break;
        }

        case Inst::Rand: {
          // Draw 64 bits of host entropy (two 32-bit random_device results) and
          // push them as a UInt. Nondeterministic by design; the device is kept
          // in a function-local static so it is opened once per process.
          static std::random_device rd;
          uint64_t hi = static_cast<uint64_t>(rd());
          uint64_t lo = static_cast<uint64_t>(rd());
          stack_.push_back(Unit::MakeUInt((hi << 32) | lo));
          break;
        }

        case Inst::Nop:
        default: {
          break;
        }
      }

      pc_ += 1;
    }
  } catch (const MachineException &e) {
    report_error("", e.what());
    result = false;
  } catch (const std::exception &e) {
    report_error("internal error: ", e.what());
    result = false;
  } catch (...) {
    report_error("internal error: ", "unknown exception");
    result = false;
  }

  return result;
}
