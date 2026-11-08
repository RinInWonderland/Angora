// Angora Bytecode Inst Definitions.
// U - Unsigned
// SL - Shift left
// AH - Assemble Highpart
// F - Float

// Encoding: one instruction is one 32-bit Code word, (arg << 7) | opcode.
//   bits  0..6  : opcode (7 bits, at most 128 instructions)
//   bits  7..31 : argument (25 bits)
//
// Some instructions sub-decode the 25-bit argument:
//   - folded arithmetic / comparison / bitwise / shift / rotate ops: the low
//     kVarBits (2) bits select the operand interpretation (type / direction /
//     bit-op); see the kTy*/kBit*/kShift*/kRot* constants in machine.h.
//   - folded immediate shift / rotate (ShiftImm, RotateImm): the low 2 bits are
//     the variant and the upper 23 bits are the count, i.e. arg = (count << 2)
//     | variant, so the count is a 23-bit immediate.
//   - PushHalfWordImm / PushHalfWordImmSL16: one 16-bit half-word. The
//     assembler's value macros split a wider literal into half-words and
//     recombine them, so these are the building blocks for 32/64-bit literals.
//   - Jump / Branch / Call / ClosureNew: a 25-bit code offset (a resolved
//     @label).
//   - every other immediate operand (claim/ret/dupn/alloci/closget/ld/st/...) is
//     a plain 25-bit value.
// Instructions with no immediate leave the argument at 0 (or at the packed
// variant for folded ops).

#ifdef INIT_INSTID
#define DEF_INST(_id, _str) _id,
#endif

#ifdef INIT_INSTSTR
#define DEF_INST(_id, _str) _str,
#endif

// Make editor completion happy.
#ifndef DEF_INST
#define DEF_INST(_id, _str)
#endif

// Arithmetic. The 2-bit variant selects int64_t (kTyInt), uint64_t (kTyUInt) or
// double (kTyFP); the assembler exposes them as add/addu/addf, sub/subu/subf,
// mul/mulu/mulf, div/divu/divf, mod/modu/modf. Both operands are on the stack:
// a = stack[top-1], b = stack[top]; the result replaces them.
DEF_INST(Add, "add")
DEF_INST(Sub, "sub")
DEF_INST(Mul, "mul")
DEF_INST(Div, "div")
DEF_INST(Mod, "mod")

// Comparison. Same int/uint/fp variants as arithmetic (lt/ult/ltf, le/ule/lef,
// gt/ugt/gtf, ge/uge/gef); replaces both operands with a UInt 1 or 0.
DEF_INST(Lt, "lt")
DEF_INST(Le, "le")
DEF_INST(Gt, "gt")
DEF_INST(Ge, "ge")

DEF_INST(PushHalfWordImm, "pushhwi")
DEF_INST(PushHalfWordImmSL16, "pushhwisl16")

// Use 2 Words to assemble FP value
DEF_INST(SpawnFP, "spawnfp")

DEF_INST(SpawnSignedInt, "spawnsint")

DEF_INST(Jump, "jmp")
DEF_INST(Branch, "branch")

// Generate farjump or farbranch if offset large than 25bits
// Consume offset from stack top
DEF_INST(FarJump, "farjmp")
// Consume 2 stack-top value, first addr, second cond
DEF_INST(FarBranch, "farbranch")

// Subroutine call/return on a dedicated call stack (separate from the
// value stack). `call` records a call frame (return pc, the value-stack
// and data-segment watermarks) and jumps to a 25-bit target (a function
// entry); `claim <argc>` lower the frame base below the arguments so the
// return values land where the arguments were; `ret [n]` rolls stack_/mem_
// back to the frame's watermarks, keeps the top `n` return values (default
// 0), and pops the call stack back into pc. `exit` halts execution.
DEF_INST(Call, "call")
DEF_INST(ClaimArgs, "claim")
DEF_INST(Ret, "ret")
DEF_INST(Exit, "exit")

DEF_INST(Pop, "pop")
DEF_INST(Print, "print")

// Shift and rotate. Register forms (Shift/Rotate) take the count from TOP();
// immediate forms (ShiftImm/RotateImm) pack the count into the upper 23 bits of
// the argument. The 2-bit variant selects the direction:
//   Shift:     kShiftL (sl, <<), kShiftLsr (lsr, unsigned >>), kShiftAsr (asr, signed >>)
//   ShiftImm:  sli / lsri / asri
//   Rotate:    kRotL (rl), kRotR (rr)
//   RotateImm: rli / rri
DEF_INST(Shift, "sl")
DEF_INST(ShiftImm, "sli")
DEF_INST(Rotate, "rl")
DEF_INST(RotateImm, "rli")

// Bitwise binary ops. The variant selects kBitAnd (and), kBitOr (or) or
// kBitXor (xor); operands a = stack[top-1], b = stack[top].
DEF_INST(BitOp, "and")
// Unary bitwise NOT: stack[top] = ~stack[top] (unsigned).
DEF_INST(Not, "not")
// EqualsZero: stack[top] = (stack[top] == 0) (unsigned, boolean).
DEF_INST(EqualsZero, "eqz")

DEF_INST(Swap, "swap")
DEF_INST(Dup, "dup")
DEF_INST(DupN, "dupn")

// memory load/store related.
DEF_INST(Load, "ld")
DEF_INST(Store, "st")
// Relative load/store: use TOP()/NEX() as base with immediate offset.
DEF_INST(RelativeLoad, "ldr")
DEF_INST(RelativeStore, "str")

// Dynamic memory allocation (Scheme B: monotonic bump arena).
// Both bump N words over the growable data segment and push the start index
// onto the value stack; they differ only in where N comes from:
//   `alloci N`  : N is the instruction's immediate argument.
//   `alloc`     : N is popped from the value stack top (TOP()).
// There is no `free`: unreachable objects are reclaimed by the tracing GC.
DEF_INST(Alloc, "alloc")
DEF_INST(AllocImm, "alloci")

// string operation.
// Replace the string at TOP() with its length as a UInt.
DEF_INST(StringLen, "strlen")
DEF_INST(StringChar, "strchar")
DEF_INST(StringConcat, "strconcat")
// consume 2 stack unit, first char, second index
DEF_INST(StringSetChar, "strsetchar")
// Copy the string at TOP() into a freshly allocated buffer, wrap it in a new
// Unit, and push it; the original string stays on the stack.
DEF_INST(StringCopy, "strcpy")

// Copy a block of `len` words from mem_[src..src+len) to mem_[dst..dst+len).
// Operands are popped in the order len, src, dst (len on stack top), matching
// C's memcpy(dst, src, len). Both regions must be pre-allocated. Overlapping
// ranges are handled like memmove. String payloads are shallow aliases under GC.
DEF_INST(MemCopy, "memcpy")

// Dynamic arrays: a first-class, GC-managed, growable sequence (reference type,
// so aliases share one array). 0-based indices.
//   arrnew         : -> arr                         new empty array
//   arrlen         : arr -> arr len                 push length (array kept)
//   arrget         : arr idx -> arr elem            read arr[idx] (OOB errors)
//   arrput         : arr idx val -> arr idx         write arr[idx]=val (bounded)
//   arrpush        : arr val -> arr                 append (grows)
//   arrpop         : arr -> arr elem                remove and push last
//   arrcpy         : arr -> arr arr                 shallow copy (original kept)
DEF_INST(ArrNew, "arrnew")
DEF_INST(ArrLen, "arrlen")
DEF_INST(ArrGet, "arrget")
DEF_INST(ArrPut, "arrput")
DEF_INST(ArrPush, "arrpush")
DEF_INST(ArrPop, "arrpop")
DEF_INST(ArrCopy, "arrcpy")

// Push two UInt on top: total GC objects bump-allocated so far, then the number
// reachable at the most recent sweep. Lets a program observe that the collector
// is reclaiming (alloc keeps climbing while live stays small).
DEF_INST(GcStat, "gcstat")

DEF_INST(Nop, "nop")

// Push one 64-bit UInt drawn from the host entropy source (std::random_device).
// Nondeterministic: the sequence differs on every run and is not reproducible.
DEF_INST(Rand, "rand")

// First-class closures. `closnew @f` pops a capture count N then N values
// (the first pushed becomes upvals[0]) and pushes a closure with the entry
// from the 25-bit label argument. `callc` pops a closure and enters it; the
// explicit arguments stay below it and `claim`/`ret` are unchanged.
// `closget i` pushes captured value i of the closure currently executing.
DEF_INST(ClosureNew, "closnew")
DEF_INST(CallClosure, "callc")
DEF_INST(ClosureGet, "closget")

// Load a value from the read-only constant pool: push consts_[arg] with the
// type the pool entry carries (Int/UInt/FP/Str). One instruction replaces the
// multi-instruction synthesis the value macros used to emit, so wide (e.g.
// 64-bit) literals cost a single word. `arg` is a 25-bit pool index.
DEF_INST(LoadConst, "ldc")

// Unambiguous companion to `print`: strings are always quoted and escaped,
// arrays are bounded by depth/length, and closures show their capture count.
// Appended at the end so the existing opcode numbering (and any compiled .bc)
// stays stable.
DEF_INST(Repr, "repr")

// nil support. `pushnil` pushes the Nil value; `isnil` replaces the top with a
// UInt 1/0 by type tag (it never reads a numeric payload). Appended so existing
// opcode numbering (and any compiled .bc) stays stable.
DEF_INST(PushNil, "pushnil")
DEF_INST(IsNil, "isnil")

// Tables: pure key/value maps (no Lua array/sequence semantics; `tbllen` is the
// entry count, not a border). Dense non-negative integer keys live in an array
// part; every other key lives in a hash part. Nil/NaN keys are rejected and
// assigning nil deletes the key. Keys: Int/UInt/FP/Str or a heap object
// (identity); Int and small UInt of equal value are the same key.
//
//   tblnew   : -> tbl
//   tbllen   : tbl -> tbl len            (entry count, UInt)
//   tblget   : tbl key -> tbl val        (missing key => nil)
//   tblput   : tbl key val -> tbl key    (val nil deletes)
//   tblnext  : tbl key -> tbl key val    (key nil starts; key nil ends)
//   tblhas   : tbl key -> tbl found      (UInt 1/0)
//   tblerase : tbl key -> tbl            (delete key)
//   tblcpy   : tbl -> tbl tbl            (shallow copy)
//   tblgeti  : tbl idx -> tbl val        (integer-key fast path)
//   tblputi  : tbl idx val -> tbl idx    (integer-key fast path)
DEF_INST(TableNew, "tblnew")
DEF_INST(TableLen, "tbllen")
DEF_INST(TableGet, "tblget")
DEF_INST(TablePut, "tblput")
DEF_INST(TableNext, "tblnext")
DEF_INST(TableHas, "tblhas")
DEF_INST(TableErase, "tblerase")
DEF_INST(TableCopy, "tblcpy")
DEF_INST(TableGetInt, "tblgeti")
DEF_INST(TablePutInt, "tblputi")

#undef DEF_INST
