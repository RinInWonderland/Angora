#ifndef ANGORA_MACHINE_H_
#define ANGORA_MACHINE_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <new>
#include <stack>
#include <string>
#include <unordered_map>
#include <vector>

// One encoded instruction word (32 bits).
// Layout: |-------- args (25 bits) --------|-- inst (7 bits) --|
//   bits[6:0]  = Inst opcode (see instruction.h / kInstStrings)
//   bits[31:7] = argument (immediate, data index, or label/pc offset)
// Decoded with GET_INST / GET_ARGS (machine.cc); stored little-endian in .bc.
using Code = uint32_t;

// Process exit codes shared by the assembler and the VM entry points.
constexpr int kExitOk = 0;     // success
constexpr int kExitError = 1;  // parse / load / run failure
constexpr int kExitUsage = 2;  // invalid command line

// 1 Word = 32bits
// HalfWord, DoubleWord, Byte

// Maybe use better container design?
using Program = std::vector<Code>;

using std::array;
using std::pair;
using std::stack;
using std::string;
using std::vector;

// Forward declarations: the heap object types and Unit are mutually recursive (a
// Unit can hold a string or an array, and the collector walks Units), so they are
// declared here and fully defined below.
struct StrObj;
struct ArrObj;
struct ArrData;
struct ClosureObj;
struct TableObj;
struct TableNodes;
class Unit;

union UnitValue {
  uint64_t ui;
  int64_t i;
  double fp;
  char *str;            // points at a GC string's character data (see StrObj)
  ArrObj *arr;          // points at a GC array handle (see ArrObj)
  ClosureObj *closure;  // points at a GC closure (see ClosureObj)
  TableObj *table;      // points at a GC table (see TableObj)
  void *ptr;
};

enum class UnitType {
  Int = 0x01,      // int64_t
  UInt = 0x02,     // uint64_t
  FP = 0x03,       // double
  Str = 0x04,      // char array
  Arr = 0x05,      // GC array (see ArrObj)
  Closure = 0x06,  // GC closure (see ClosureObj)
  Nil = 0x07,      // explicit null; no payload (value is always 0)
  Table = 0x08,    // GC table (see TableObj)
  Invalid = 0x00,  // uninitialized / array hole (distinct from Nil)
};

// All heap objects share this header. The kind is packed into gc_flags_ (a few
// spare bits), so tagging costs no memory and every object stays 8-aligned,
// which lets the sweep stride a chunk purely by gc_size_.
enum class GcKind : uint32_t {
  Str = 0,
  Arr = 1,
  ArrData = 2,
  Closure = 3,
  Table = 4,
  TableNodes = 5,
};

struct GcHeader {
  size_t gc_size_;     // full object size in bytes (8-aligned)
  uint32_t gc_flags_;  // bit0: mark; bits 1..3: GcKind
  uint32_t gc_aux_;    // kind-specific scratch (Str: length in bytes)
  GcHeader *gc_next_;  // free-list link while unreachable
};

constexpr uint32_t kGcMarkBit = 0x1u;
constexpr uint32_t kGcKindShift = 1;
constexpr uint32_t kGcKindMask = 0x7u << kGcKindShift;
inline bool GcMarked(const GcHeader *h) { return (h->gc_flags_ & kGcMarkBit) != 0; }
inline void GcMarkObj(GcHeader *h) { h->gc_flags_ |= kGcMarkBit; }
inline GcKind GcKindOf(const GcHeader *h) {
  return static_cast<GcKind>((h->gc_flags_ & kGcKindMask) >> kGcKindShift);
}
inline uint32_t GcFlagsFor(GcKind k) { return static_cast<uint32_t>(k) << kGcKindShift; }

// Traceable GC string payload: a shared header followed by the NUL-terminated
// characters inline. `UnitValue::str` points at gc_chars_, so string access —
// STRVAL, indexing, printing — is unchanged. There is no refcount: a copy of a
// string-bearing Unit is a shallow alias reclaimed by the tracing collector.
struct StrObj {
  GcHeader h;
  char gc_chars_[1];  // flexible-array emulation; inline char buffer
};
static_assert(offsetof(StrObj, gc_chars_) == sizeof(GcHeader),
              "string data must follow the header");

// Walk a char* (pointing at a payload's char data) back to its heap head.
inline StrObj *GcObjOf(const char *p) {
  return reinterpret_cast<StrObj *>(const_cast<char *>(p) - offsetof(StrObj, gc_chars_));
}

// Cached string length: O(1), read from the header instead of scanning for NUL.
inline size_t GcLenOf(const char *p) { return GcObjOf(p)->h.gc_aux_; }

// GC heap. Objects live in a list of fixed-size chunks; a chunk's base is a
// stable allocation that never moves, so a `Unit`'s embedded string pointer stays
// valid forever — including across a chunk rollover. The heap is owned by a
// Machine, so two Machines never share (or sweep) each other's strings, and each
// collection traces only that Machine's roots.
struct GcChunk {
  char *base;
  size_t cap;
  size_t used;
};

// One activation record. `call`/`callc` save the return pc, the value-stack and
// data-segment watermarks, and the closure currently executing; `ret` restores
// them so unwinding a function returns stack_/mem_ to exactly their pre-call
// state. `closure` is the running closure for `closget`, or nullptr for a
// statically-called function. Defined before GcHeap so the collector can treat
// every in-flight closure on the call stack as a root.
struct CallFrame {
  uint32_t ret_pc;         // pc to resume at after `ret`.
  uint32_t stack_mark;     // stack_ size to truncate to (frame base).
  uint32_t mem_mark;       // mem_ size to truncate to.
  ClosureObj *closure;     // currently executing closure, or nullptr.
};

// The collector state. `Alloc` mints objects, `Collect` marks from a root set and
// sweeps. A freed slot is never moved or shrunk, so every object's recorded size
// stays fixed and the sweep can walk chunks by striding `gc_size_`.
class GcHeap {
public:
  GcHeap() = default;
  GcHeap(const GcHeap &) = delete;
  GcHeap &operator=(const GcHeap &) = delete;
  // Chunk bases are stable heap allocations, so moving a GcHeap just transfers
  // ownership of the chunk list; every `Unit` string pointer stays valid.
  GcHeap(GcHeap &&o) noexcept;
  GcHeap &operator=(GcHeap &&o) noexcept;
  ~GcHeap();

  // Core allocation: `raw` is the full object size in bytes including the header.
  // The size is rounded to the header alignment and fixed for the slot's lifetime;
  // the kind is stamped each time (a slot may be recycled for a different kind).
  // Counters (`alloc_count_`, `bytes_since_`) are bumped here. The returned header
  // is otherwise uninitialized.
  GcHeader *AllocBytes(size_t raw, GcKind kind);

  // Allocate a copy of `src` (`len` chars) and return a pointer to its character
  // data. Reuses a freed slot when one is available; otherwise bumps a chunk.
  char *Alloc(const char *src, size_t len);

  // Allocate an uninitialized string of `len` chars and return a pointer to its
  // character data; the caller must fill `[0, len)` and the trailing NUL. Used by
  // concatenation to build the result in place, avoiding an intermediate buffer.
  char *AllocRaw(size_t len);

  // Array support (definitions in gc.cc).
  ArrData *AllocArrData(uint32_t capacity);
  ArrObj *AllocArr(uint32_t capacity);
  // Grow `a`'s element buffer to hold at least `need` elements, copying existing
  // elements. Only fields inside the stable ArrObj change, so aliases stay in sync.
  void ArrReserve(ArrObj *a, uint32_t need);

  // Closure support (definition in gc.cc).
  ClosureObj *AllocClosure(uint32_t entry, uint32_t nupvals);

  // Table support (definitions in gc.cc). A table owns an optional ArrData
  // array part and an optional TableNodes hash part; both are separate GC
  // objects so the table handle itself never moves.
  TableObj *AllocTable();
  TableNodes *AllocTableNodes(uint32_t count);

  // Tracing (definitions in gc.cc). `a`/`b` are the value stack and data segment,
  // `c` is the constant pool (also a permanent root), and `frames` contributes
  // every in-flight closure on the call stack as a root.
  void Mark(const vector<Unit> &a, const vector<Unit> &b, const vector<Unit> &c,
            const vector<CallFrame> &frames);
  void Sweep();
  void Collect(const vector<Unit> &a, const vector<Unit> &b, const vector<Unit> &c,
               const vector<CallFrame> &frames);

  size_t Live() const { return live_; }
  size_t Allocated() const { return alloc_count_; }
  size_t BytesSinceCollect() const { return bytes_since_; }

private:
  // Release every chunk back to the allocator.
  void FreeChunks();

  // Pop a free slot of exactly `obj_size` (small, O(1)) or the first slot at
  // least that big (large). Returns nullptr when none is available.
  GcHeader *PopFree(size_t obj_size);

  // Chain an unreachable object back onto its size class so it can be reused.
  void PushFree(GcHeader *o);

  // Append a fresh chunk large enough to hold `need` bytes. Chunk caps double each
  // time so amortized growth is O(1); the base is a fresh allocation and never moves.
  void GrowChunk(size_t need);

  // Segregated free lists: small objects are bucketed by exact size
  // (`gc_size_ / kAlign`), so reuse is O(1); objects larger than kSmallMax share a
  // single first-fit list. A slot keeps its original size forever, so the sweep
  // can still walk a chunk by striding `gc_size_`.
  static constexpr size_t kAlign = alignof(GcHeader);
  static constexpr size_t kSmallMax = 4096;
  static constexpr size_t kNumSmall = kSmallMax / kAlign;

  vector<GcChunk> chunks_;
  array<GcHeader *, kNumSmall> free_{};
  GcHeader *large_free_ = nullptr;
  vector<GcHeader *> mark_stack_;  // tracing worklist, reused across cycles
  size_t live_ = 0;                // objects reachable at the last sweep
  size_t alloc_count_ = 0;         // total objects allocated so far
  size_t bytes_since_ = 0;         // bytes allocated since the last collection
};

// A stack/memory value. Scalar payloads (Int/UInt/FP) are copied by value; the
// string payload aliases a traceable GC string buffer. Copy/move/assign are pure
// shallow operations (the collector, not a refcount, reclaims unreachable
// strings), so any copy of a string-bearing Unit is just another alias.
class Unit {
public:
  UnitValue value;
  UnitType type;

  Unit() : value{0}, type(UnitType::Invalid) {}

  Unit(const Unit &o) : value(o.value), type(o.type) {}

  Unit(Unit &&o) noexcept : value(o.value), type(o.type) {
    o.value = UnitValue{0};
    o.type = UnitType::Invalid;
  }

  Unit &operator=(const Unit &o) {
    if (this != &o) {
      value = o.value;
      type = o.type;
    }
    return *this;
  }

  Unit &operator=(Unit &&o) noexcept {
    if (this != &o) {
      value = o.value;
      type = o.type;
      o.value = UnitValue{0};
      o.type = UnitType::Invalid;
    }
    return *this;
  }

  ~Unit() = default;

  static Unit MakeInt(int64_t v) {
    Unit u;
    u.value.i = v;
    u.type = UnitType::Int;
    return u;
  }

  static Unit MakeUInt(uint64_t v) {
    Unit u;
    u.value.ui = v;
    u.type = UnitType::UInt;
    return u;
  }

  static Unit MakeFP(double v) {
    Unit u;
    u.value.fp = v;
    u.type = UnitType::FP;
    return u;
  }

  static Unit MakeStr(GcHeap &gc, const char *s, size_t n) {
    Unit u;
    u.value.str = gc.Alloc(s, n);
    u.type = UnitType::Str;
    return u;
  }

  static Unit MakeArr(GcHeap &gc, uint32_t capacity) {
    Unit u;
    u.value.arr = gc.AllocArr(capacity);
    u.type = UnitType::Arr;
    return u;
  }

  static Unit MakeClosure(ClosureObj *c) {
    Unit u;
    u.value.closure = c;
    u.type = UnitType::Closure;
    return u;
  }

  static Unit MakeNil() {
    Unit u;
    u.value = UnitValue{0};
    u.type = UnitType::Nil;
    return u;
  }

  static Unit MakeTable(TableObj *t) {
    Unit u;
    u.value.table = t;
    u.type = UnitType::Table;
    return u;
  }
};

// INT VALue, Unsigned INT VALue, Floating-Point VALue
#define ZERO_INTOBJ Unit::MakeInt(0)
#define ZERO_UINTOBJ Unit::MakeUInt(0)
#define ZERO_FPOBJ Unit::MakeFP(0.0)
#define NILOBJ Unit::MakeNil()

// Renaming them?
#define INTVAL(_unit) (_unit).value.i
#define UINTVAL(_unit) (_unit).value.ui
#define FPVAL(_unit) (_unit).value.fp
#define STRVAL(_unit) (_unit).value.str
#define ARRVAL(_unit) (_unit).value.arr
#define CLOSUREVAL(_unit) (_unit).value.closure
#define TABLEVAL(_unit) (_unit).value.table

// Truthiness used by `branch`/`farbranch`: only Nil (and the defensive Invalid)
// and a numeric zero are false; everything else (including empty strings/arrays)
// is true. Nil is decided by its tag, so its payload is never read.
inline bool IsTruthy(const Unit &u) {
  switch (u.type) {
    case UnitType::Nil:
    case UnitType::Invalid:
      return false;
    case UnitType::Int:
      return u.value.i != 0;
    case UnitType::UInt:
      return u.value.ui != 0;
    case UnitType::FP:
      return u.value.fp != 0.0;
    default:
      return true;
  }
}

// GC array handle: a small, stable descriptor that all aliases point to. Its
// elements live in a separate ArrData buffer; growing swaps the buffer and
// updates the fields here, so every alias observes the same array (reference
// semantics) even though the collector never moves objects.
struct ArrObj {
  GcHeader h;
  ArrData *data;      // element buffer, or nullptr when capacity is 0
  uint32_t length;    // number of live elements
  uint32_t capacity;  // element slots available in `data`
};

// GC array element buffer: `capacity` Units laid out inline. The exact element
// count is stored in gc_aux_ (not derived from gc_size_, since a reused free slot
// may be larger), so the sweep strides by size while the marker scans gc_aux_
// entries. Unused slots hold Invalid Units and are never traced.
struct ArrData {
  GcHeader h;
  Unit items[1];  // flexible-array emulation
};

// Number of Unit slots in an ArrData buffer (stored in the header, so a reused
// larger free slot's stale tail is never scanned).
inline size_t GcArrDataLen(const ArrData *d) { return d->h.gc_aux_; }

// GC closure: a stable descriptor holding the function entry pc and its captured
// values inline. The capture count lives in gc_aux_ (not derived from gc_size_,
// since a reused free slot may be larger), mirroring ArrData, so the sweep
// strides by size while the marker scans exactly that many Units. Closures are
// captured by value: each upval is a shallow alias copy of the Unit on the
// stack at `closnew` time, so sharing mutable state means capturing a heap cell
// (e.g. a 1-element array) whose handle is copied.
struct ClosureObj {
  GcHeader h;
  uint32_t entry_pc;
  Unit upvals[1];  // flexible-array emulation; count stored in gc_aux_
};

// Number of captured values in a ClosureObj (stored in the header).
inline size_t GcClosureNupvals(const ClosureObj *c) { return c->h.gc_aux_; }

// One hash-part node. `next` is a 1-based index into TableNodes::items (0 means
// "end of chain"), so the node buffer can be replaced on rehash without any
// dangling pointers. A node with an Invalid key is free.
struct TableNode {
  Unit key;
  Unit value;
  uint32_t next;
};

// GC-managed node buffer for the table hash part. gc_aux_ is its capacity.
struct TableNodes {
  GcHeader h;
  TableNode items[1];  // flexible-array emulation
};

// Number of node slots in a TableNodes buffer (stored in the header).
inline size_t GcTableNodeCount(const TableNodes *n) { return n->h.gc_aux_; }

// A pure key/value table. Integer keys in [0, array_size) live in the dense
// array part (a reused ArrData buffer, 0-based); every other key lives in the
// hash part, a power-of-two TableNodes array with chained buckets. There is no
// border / sequence-length semantics: `size` is the number of live entries and
// is what `tbllen` returns. Assigning nil deletes a key; nil is never stored.
struct TableObj {
  GcHeader h;
  ArrData *array;       // dense array part (0-based), or nullptr
  uint32_t array_cap;   // allocated capacity of `array`
  uint32_t array_size;  // logical extent [0, array_size)
  TableNodes *nodes;    // hash part, or nullptr
  uint32_t node_count;  // power of two (0 when nodes == nullptr)
  uint32_t hash_count;  // live entries in the hash part
  uint32_t free_hint;   // hash: upper bound for the downward free-slot scan
  uint32_t size;        // total live entries (array non-hole + hash)
};

// Table operations. Keys may be Int/UInt/FP/Str or a heap object (identity);
// Nil/Invalid/NaN are rejected. Int and UInt with the same numeric value are
// the same key; an FP key is a distinct key type. Assigning Nil deletes.
namespace table {
bool ValidKey(const Unit &key);
bool Get(const TableObj *t, const Unit &key, Unit &out);
bool Has(const TableObj *t, const Unit &key);
void Put(GcHeap &gc, TableObj *t, const Unit &key, const Unit &value);
bool Erase(TableObj *t, const Unit &key);
uint32_t Size(const TableObj *t);
TableObj *Copy(GcHeap &gc, const TableObj *src);
// Iteration: `key` == Nil starts; otherwise it must be a key returned by a
// previous call. Returns false when the table is exhausted.
bool Next(const TableObj *t, const Unit &key, Unit &out_key, Unit &out_value);
}  // namespace table

// token, offset
using Label = std::pair<string, size_t>;
using Labels = std::unordered_map<string, size_t>;

#define INIT_INSTID
enum class Inst : uint8_t {
#include "instruction.h"
};
#undef INIT_INSTID

#define INIT_INSTSTR
const vector<const char *> kInstStrings = {
#include "instruction.h"
};
#undef INIT_INSTSTR

// Folded-instruction variant selectors. The low kVarBits of the 25-bit
// argument of a folded op select the operand interpretation; the assembler picks
// them from the mnemonic (see the kFoldedAliases table in asm-interp.cc).
constexpr uint32_t kVarBits = 2;
constexpr uint32_t kVarMask = (1u << kVarBits) - 1;

// Arithmetic / comparison type variants.
constexpr uint32_t kTyInt = 0;   // signed int64_t
constexpr uint32_t kTyUInt = 1;  // unsigned uint64_t
constexpr uint32_t kTyFP = 2;    // double

// Bit-operation variants.
constexpr uint32_t kBitAnd = 0;
constexpr uint32_t kBitOr = 1;
constexpr uint32_t kBitXor = 2;

// Shift direction variants.
constexpr uint32_t kShiftL = 0;    // <<
constexpr uint32_t kShiftLsr = 1;  // unsigned >>
constexpr uint32_t kShiftAsr = 2;  // signed >>

// Rotate direction variants.
constexpr uint32_t kRotL = 0;
constexpr uint32_t kRotR = 1;

// Instruction macro strings
constexpr const char *kCStrPushImm = "pushimm";
constexpr const char *kCStrPushUImm = "pushuimm";
constexpr const char *kCStrPushFP = "pushfp";

// Some notes about symbol table design
// 1. Keep original notation from asm
// 2. Identify and access with array offset in real binary
// 3. Contains numeric constants and string constants.

// ---------------------------------------------------------------------------
// Bytecode file (.bc) format
// ---------------------------------------------------------------------------
// Produced by `angora-interp <file> -compile`, consumed by `angora-vm`.
//
// Endianness: the on-disk format is defined as LITTLE-ENDIAN and is the same on
// every host. All multi-byte integers go through the ReadLE32/WriteLE32 helpers
// below, which assemble/extract bytes explicitly, so a big-endian host reads and
// writes byte-identical files. Never fwrite/fread an integer type directly.
// Each magic constant is four ASCII bytes packed MSB-first, so a byte dump shows
// them reversed: e.g. kMagicSectData 0x44415441 reads as "DATA" but is stored as
// 41 54 41 44 ("ATAD").
//
// Layout (top to bottom):
//
//   [optional] data section:
//       u32   kMagicSectData                 // 0x44415441 "DATA"
//       repeated data entry (until kMagicSectEnd):
//           u32   UnitType                   // 1=Int 2=UInt 3=FP 4=Str 7=Nil
//           Int/UInt/FP: u32 hi, u32 lo      // value = (hi << 32) | lo
//           Str:         u32 word* ... u32 0 // packed chars + NUL word
//           Nil:         (no payload)
//       u32   kMagicSectEnd                  // 0x2E454E44 ".END"
//
//   [optional] constant pool (read by `ldc`), same entry encoding:
//       u32   kMagicSectConst                // 0x434F4E53 "CONS"
//       repeated entry (until kMagicSectEnd), same layout as a data entry
//       u32   kMagicSectEnd
//
//   code section:
//       u32   code_count                     // number of Code words
//       Code  code_count * sizeof(Code)      // Code = u32, encoding below
//
//   [optional] function table:
//       u32   func_count
//       repeated:
//           u32   name_len
//           u8    name[name_len]             // not NUL-terminated
//           u32   entry_pc
//       u32   kMagicSectEndcode              // 0x454E4443 "ENDC" sentinel
//
// Code word (32 bits):
//   |-------- args (25 bits) --------|-- inst (7 bits) --|
//   bits[6:0]  Inst opcode (see instruction.h / kInstStrings)
//   bits[31:7] argument (immediate, data index, or label/pc offset)
//
// String payload packing: up to four bytes are packed into each u32 with the
// j-th byte at bits [j*8, j*8+8); the byte run is terminated by an all-zero u32
// word. Written/read through the little-endian helpers, so characters appear in
// order in the file on every host.
//
// In assembly the data section is written as `<type string> <id> <data>`.

constexpr uint32_t kMagicSectData = 0x44415441;   // "DATA"
constexpr uint32_t kMagicSectConst = 0x434F4E53;  // "CONS" (constant pool section)
constexpr uint32_t kMagicSectCode = 0x434F4445;   // "CODE" (for bytecode file format)
constexpr uint32_t kMagicSectIdent = 0x4944454E;  // "IDEN"
constexpr uint32_t kMagicSectEnd = 0x2E454E44;    // ".END"

// Magic marker in bytecode file that terminates the optional function table
// (see the file-format description above). Spells ASCII "ENDC" when read
// MSB-first; stored little-endian, so the on-disk bytes are 43 44 4E 45.
constexpr uint32_t kMagicSectEndcode = 0x454E4443;

// Little-endian bytecode I/O. The .bc format is fixed little-endian on disk, so
// these helpers are used for every multi-byte field by both the assembler
// (writing) and the bytecode VM (reading). They assemble/extract bytes by hand,
// which yields identical results on little- and big-endian hosts. The reader
// returns false on short read so callers can detect EOF/truncation.
inline bool ReadLE32(FILE *fp, uint32_t &out) {
  unsigned char b[4];
  if (fread(b, 1, sizeof(b), fp) != sizeof(b)) {
    return false;
  }
  out = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
        (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
  return true;
}

inline void WriteLE32(FILE *fp, uint32_t v) {
  unsigned char b[4] = {
      static_cast<unsigned char>(v & 0xFF),
      static_cast<unsigned char>((v >> 8) & 0xFF),
      static_cast<unsigned char>((v >> 16) & 0xFF),
      static_cast<unsigned char>((v >> 24) & 0xFF),
  };
  fwrite(b, 1, sizeof(b), fp);
}

constexpr const char *kCStrDataSectBegin = ".data";
constexpr const char *kCStrAngoraHeaderBegin = ".identity";
constexpr const char *kCStrCodeSectBegin = ".code";
constexpr const char *kCStrSectEnd = ".end";

// Function section markers (assembly level).
constexpr const char *kCStrFuncBegin = ".func";
constexpr const char *kCStrFuncEnd = ".endfunc";

enum class SectionType { Data, Identity, Code, Null };

// Data Section type string.
constexpr const char *kCStrDataInt = ".int";    // .int <id> <value>
constexpr const char *kCStrDataUInt = ".uint";  // .uint <id> <value>
constexpr const char *kCStrDataFP = ".fp";      // .fp <id> <value>
constexpr const char *kCStrDataStr = ".str";    // .str <id> <value>
constexpr const char *kCStrDataNil = ".nil";    // .nil <id>   (no value)

inline bool IsSectionRelatedString(const std::string &s) {
  return s == kCStrDataInt || s == kCStrDataUInt || s == kCStrDataFP || s == kCStrDataStr ||
         s == kCStrDataNil || s == kCStrDataSectBegin || s == kCStrAngoraHeaderBegin ||
         s == kCStrCodeSectBegin || s == kCStrSectEnd;
}

// Data symbol reference prefix
constexpr char kCharDataIdPrefix = '&';
// When reference a symbol in asm:
// <inst> &<symbol name>

// Label reference prefix in jump/branch/call.
constexpr char kCharLabelRefPrefix = '@';
// When jumping to a label in asm:
// <jump inst> @<label name>

// Identity Section string.
// Currently, only version is considered.
constexpr const char *kCStrDotVersion = ".version";

union DataUnit {
  int64_t i;
  uint64_t ui;
  double fp;
  char *str;
};

class MachineException : public std::exception {
private:
  std::string message_;

public:
  explicit MachineException(const std::string &msg) : message_(msg) {}
  virtual const char *what() const noexcept override { return message_.c_str(); }
};

class Machine {
protected:
  GcHeap gc_;
  vector<Unit> stack_;
  vector<Unit> mem_;
  vector<Unit> consts_;  // read-only constant pool indexed by `ldc`.
  vector<string> dbg_symbol_names_;
  uint64_t pc_;                 // Program counter.
  vector<CallFrame> call_stack_;  // Activation records for call/claim/ret.

  Labels labels_;

public:
  Machine() : gc_(), stack_(), pc_(0), labels_() {}
  Machine(Labels &labels) : gc_(), stack_(), mem_(), pc_(0), labels_(labels) {}

  // Take ownership of strings a parser/loader allocated before the Machine
  // existed. Chunk bases are stable, so the pointers in `data` stay valid.
  void AdoptHeap(GcHeap &&heap) { gc_ = std::move(heap); }

  GcHeap &Gc() { return gc_; }

  void AcceptData(vector<Unit> &data) { mem_ = std::move(data); }

  // Install the read-only constant pool that `ldc <index>` reads from. Like the
  // data segment it is a GC root, but it is never mutated at runtime.
  void AcceptConsts(vector<Unit> &consts) { consts_ = std::move(consts); }

  void AcceptDbgNames(vector<string> &names) { dbg_symbol_names_ = std::move(names); }

  // Register function name-to-offset mappings for label resolution.
  // The bytecode interpreter populates this when reading a compiled
  // .bc file that contains a function table section.
  void AcceptFunctions(vector<pair<string, size_t>> &funcs) {
    for (auto &f : funcs) labels_[f.first] = f.second;
  }

  bool Run(Program &prog);
};

#endif  // ANGORA_MACHINE_H_
