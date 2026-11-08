#include <algorithm>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <string_view>
#include <vector>

#include "bytecode-io.h"
#include "log.h"
#include "machine.h"

using std::feof;
using std::ferror;
using std::fgetc;
using std::FILE;
using std::fprintf;
using std::fopen;
using std::stod;
using std::stoll;
using std::stoull;
using std::strcmp;
using std::string;
using std::string_view;
using std::vector;

// Largest code offset that fits the 25-bit instruction argument field
// (bits[31:7]). Targets beyond this must be reached indirectly (push the
// address and FarJump/FarBranch); Call/ClosureNew have no far form.
constexpr uint32_t kMaxJumpArg = 0x1FFFFFF;

// Folded immediate shift/rotate share the 25-bit argument with a 2-bit variant,
// so the count is limited to the upper = 23 bits.
constexpr uint32_t kMaxFoldedImm = (1u << (25 - kVarBits)) - 1;

int kDebugMsgSwitch = 0;

static void PrintUsage(FILE *out) {
  fprintf(out, "usage: angora-interp <file.asm> [-run|-compile] [-debug] [-help]\n");
}

// Source file attached to assembler diagnostics; set in main once known, so
// messages before that (command-line errors) carry no file.
static const char *g_asm_file = nullptr;

// Set by ReadInst on an unrecoverable lexical error (I/O failure or an
// unterminated string). ReadInst reports only "line delivered vs. EOF", so this
// flag lets the top level fail parsing instead of continuing to an empty program.
static bool g_lex_error = false;

// Emit one assembler diagnostic to stderr through log.h. `line` is 0 when the
// location is unknown.
static void AsmDiag(LogSeverity sev, long line, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  LogVWrite(sev, "angora-interp", LogLocation{g_asm_file, line}, fmt, ap);
  va_end(ap);
}

#define AsmError(_line, ...) AsmDiag(LogSeverity::Error, (_line), __VA_ARGS__)
#define AsmWarning(_line, ...) AsmDiag(LogSeverity::Warning, (_line), __VA_ARGS__)
#define AsmInfo(...) AsmDiag(LogSeverity::Info, 0, __VA_ARGS__)

bool ReadInst(vector<string> &dest, FILE *fp, size_t &line, size_t &line_of_tokens) {
  bool result = true;

  dest.clear();
  line_of_tokens = line;
  unsigned char c;
  string buf;

  auto read_char = [&]() -> unsigned char {
    int ch = fgetc(fp);
    if (ch == '\n') {
      line += 1;
    }
    return static_cast<unsigned char>(ch);
  };

  while (true) {
    c = read_char();

    if (feof(fp)) {
      if (buf.empty()) {
        result = false;
      } else if (buf == kCStrSectEnd) {
        // Reaching code section ending.
        dest.push_back(buf);
        result = false;
      } else {
        // Deliver the final logical line; the next call observes real EOF.
        dest.push_back(buf);
        result = true;
      }
      break;
    }

    if (ferror(fp)) {
          g_lex_error = true;
          AsmError(line, "cannot read source");
      result = false;
      break;
    }

    if (c == '\r' || c == '\n') {
      if (!buf.empty()) {
        if (buf == kCStrSectEnd) {
          // Reaching code section ending.
          result = false;
          dest.push_back(buf);
          break;
        }

        dest.push_back(buf);
      }
      break;
    } else if (c == ' ' || c == '\t') {
      if (!buf.empty()) {
        dest.push_back(buf);
        buf.clear();
      }
      // if buffer empty, just skip this character.
    }
    // Comment processing.
    else if (c == '/') {
      if (!buf.empty()) {
        if (buf.back() != '/') {
          dest.push_back(buf);
          buf.clear();
          buf.append(1, c);
        } else {
          while (true) {
            c = read_char();
            if (feof(fp)) {
              result = false;
              break;
            }
            if (ferror(fp)) {
      g_lex_error = true;
      AsmError(line, "cannot read source");
              result = false;
              break;
            }
            if (c == '\n') {
              break;
            }
            buf.clear();
            buf.append(1, c);
          }
          break;
        }
      } else {
        buf.append(1, c);
      }
    }
    // New instruction line separator.
    else if (c == ';') {
      if (!buf.empty()) {
        dest.push_back(buf);
        buf.clear();
      }
      break;
    } else if (c == '\"') {
      // String literal processing
      string str_content;
      bool escaped = false;
      while (true) {
        c = read_char();
        if (feof(fp)) {
          g_lex_error = true;
          AsmError(line, "unterminated string literal");
          result = false;
          return result;
        }
        if (ferror(fp)) {
              g_lex_error = true;
              AsmError(line, "cannot read source");
          result = false;
          break;
        }

        if (escaped) {
          if (c == '\"')
            str_content.push_back('\"');
          else if (c == 't')
            str_content.push_back('\t');
          else if (c == '\\')
            str_content.push_back('\\');
          else {
            // Handle other escapes or treat as literal backslash + char
            str_content.push_back('\\');
            str_content.push_back(static_cast<char>(c));
          }
          escaped = false;
        } else if (c == '\\') {
          escaped = true;
        } else if (c == '\"') {
          break;  // End of string
        } else {
          str_content.push_back(static_cast<char>(c));
        }
      }
      // After closing quote, handle the buffer that might have been accumulating
      if (!buf.empty()) {
        dest.push_back(buf);
        buf.clear();
      }
      dest.push_back(str_content);
    } else {
      buf.append(1, c);
    }
  }

  return result;
}

bool GetInst(uint32_t &dest, string_view str) {
  bool result = false;

  for (size_t idx = 0, size = kInstStrings.size(); idx < size; idx += 1) {
    if (strcmp(str.data(), kInstStrings[idx]) == 0) {
      dest = static_cast<uint32_t>(idx);
      result = true;
      break;
    }
  }

  return result;
}

// Folded instructions keep familiar mnemonics but encode the operand
// interpretation in the low kVarBits of the argument. Entries below cover the
// non-default variants only; each family's default variant is its canonical
// mnemonic in instruction.h ("add", "lt", "sl", "sli", "rl", "rli", "and").
struct FoldedAlias {
  const char *name;
  Inst op;
  uint32_t variant;
};

const FoldedAlias kFoldedAliases[] = {
    {"addu", Inst::Add, kTyUInt},        {"addf", Inst::Add, kTyFP},
    {"subu", Inst::Sub, kTyUInt},        {"subf", Inst::Sub, kTyFP},
    {"mulu", Inst::Mul, kTyUInt},        {"mulf", Inst::Mul, kTyFP},
    {"divu", Inst::Div, kTyUInt},        {"divf", Inst::Div, kTyFP},
    {"modu", Inst::Mod, kTyUInt},        {"modf", Inst::Mod, kTyFP},
    {"ult", Inst::Lt, kTyUInt},          {"ltf", Inst::Lt, kTyFP},
    {"ule", Inst::Le, kTyUInt},          {"lef", Inst::Le, kTyFP},
    {"ugt", Inst::Gt, kTyUInt},          {"gtf", Inst::Gt, kTyFP},
    {"uge", Inst::Ge, kTyUInt},          {"gef", Inst::Ge, kTyFP},
    {"lsr", Inst::Shift, kShiftLsr},     {"asr", Inst::Shift, kShiftAsr},
    {"lsri", Inst::ShiftImm, kShiftLsr}, {"asri", Inst::ShiftImm, kShiftAsr},
    {"rr", Inst::Rotate, kRotR},         {"rri", Inst::RotateImm, kRotR},
    {"or", Inst::BitOp, kBitOr},         {"xor", Inst::BitOp, kBitXor},
};

// True for the folded opcodes whose low argument bits are a variant selector.
inline bool IsFoldedOp(Inst op) {
  switch (op) {
    case Inst::Add:
    case Inst::Sub:
    case Inst::Mul:
    case Inst::Div:
    case Inst::Mod:
    case Inst::Lt:
    case Inst::Le:
    case Inst::Gt:
    case Inst::Ge:
    case Inst::Shift:
    case Inst::ShiftImm:
    case Inst::Rotate:
    case Inst::RotateImm:
    case Inst::BitOp:
      return true;
    default:
      return false;
  }
}

// True for folded ops whose immediate (shift/rotate count) is packed above the
// variant bits as arg = (count << kVarBits) | variant.
inline bool IsFoldedImmOp(Inst op) {
  return op == Inst::ShiftImm || op == Inst::RotateImm;
}

// Encode a folded op whose whole argument is the variant, e.g. `addu`
// (Inst::Add, kTyUInt) or `asr` (Inst::Shift, kShiftAsr).
inline Code FoldedCode(Inst op, uint32_t variant) {
  return (variant << 7) | static_cast<uint32_t>(op);
}

// Encode a folded immediate op: arg = (imm << kVarBits) | variant, e.g. the
// `32` shift in the 64-bit literal macros (Inst::ShiftImm, kShiftL, 32).
inline Code FoldedImmCode(Inst op, uint32_t variant, uint32_t imm) {
  return (((imm << kVarBits) | variant) << 7) | static_cast<uint32_t>(op);
}

// Resolve a mnemonic to its opcode, packed variant, and whether it is folded.
// Aliases are checked first so the non-default variants resolve; otherwise the
// canonical kInstStrings table is used with the default variant (0).
bool ResolveInst(uint32_t &op, uint32_t &variant, bool &folded, const string &name) {
  for (const FoldedAlias &a : kFoldedAliases) {
    if (name == a.name) {
      op = static_cast<uint32_t>(a.op);
      variant = a.variant;
      folded = true;
      return true;
    }
  }
  if (!GetInst(op, name)) {
    return false;
  }
  variant = 0;
  folded = IsFoldedOp(static_cast<Inst>(op));
  return true;
}

inline bool IsUnsignedInst(Inst inst) {
  return inst == Inst::PushHalfWordImm || inst == Inst::PushHalfWordImmSL16 ||
         inst == Inst::Jump || inst == Inst::Branch || inst == Inst::ClaimArgs ||
         inst == Inst::Ret || inst == Inst::ClosureGet;
}

inline bool IsJumpInst(Inst inst) {
  return inst == Inst::Jump || inst == Inst::Branch || inst == Inst::FarJump ||
         inst == Inst::FarBranch || inst == Inst::Call || inst == Inst::ClosureNew;
}

inline bool IsAlpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

inline bool IsNumber(char c) { return (c >= '0' && c <= '9'); }

inline bool IsUnderscore(char c) { return c == '_'; }

// Resolve a data-section symbol reference of the form <prefix><name>
// (e.g. "&foo") to its index in `symbol_names`. Returns false when the
// token lacks the prefix, carries no name, or names an unknown symbol.
inline bool ResolveDataSymbol(const string &token, const vector<string> &symbol_names,
                              uint32_t &dest) {
  if (token.size() < 2 || token[0] != kCharDataIdPrefix) {
    return false;
  }

  string name = token.substr(1);
  auto it = find(symbol_names.begin(), symbol_names.end(), name);

  if (it == symbol_names.end()) {
    return false;
  }

  dest = static_cast<uint32_t>(distance(symbol_names.begin(), it));
  return true;
}

// Parse a jump-target label reference of the form <prefix><name>
// (e.g. "@loop"). Stores the bare label name in `dest`. Returns false
// when the token lacks the prefix or carries no name.
inline bool ParseLabelRef(const string &token, string &dest) {
  if (token.size() < 2 || token[0] != kCharLabelRefPrefix) {
    return false;
  }

  dest = token.substr(1);
  return true;
}

// Label string format: <LabelName><:>
inline bool IsLabelString(string_view src) {
  if (src.size() < 2) {
    return false;
  }

  if (src[src.size() - 1] != ':') {
    return false;
  }

  for (size_t i = 0; i < src.size() - 1; i += 1) {
    if (!IsAlpha(src[i]) && !IsNumber(src[i]) && !IsUnderscore(src[i])) {
      return false;
    }
  }

  return true;
}

int GetIntLiteralBase(string_view str) {
  if (str.size() == 1) {
    return 10;
  }

  auto prefix = str.substr(0, 2);

  if (prefix[0] == '+' || prefix[0] == '-') {
    return 10;
  }

  if (prefix == "0x" || prefix == "0X") {
    return 16;
  }

  if (prefix == "0b" || prefix == "0B") {
    return 2;
  }

  if (prefix[0] == '0') {
    return 8;
  }

  return 10;
}

bool TryParseImm(const string &token, bool is_unsigned, uint32_t &dest) {
  if (token.empty()) {
    return false;
  }

  int base = GetIntLiteralBase(token);
  string_view digits(token);

  if (base == 16 && digits.size() >= 2 && digits[0] == '0' &&
      (digits[1] == 'x' || digits[1] == 'X')) {
    digits.remove_prefix(2);
  } else if (base == 2 && digits.size() >= 2) {
    digits.remove_prefix(2);
  }

  if (digits.empty()) {
    return false;
  }

  try {
    size_t pos = 0;
    if (is_unsigned) {
      if (digits[0] == '+' || digits[0] == '-') {
        return false;
      }
      uint64_t value = stoull(string(digits), &pos, base);
      if (pos != digits.size()) {
        return false;
      }
      dest = static_cast<uint32_t>(value);
    } else {
      int64_t value = stoll(string(digits), &pos, base);
      if (pos != digits.size()) {
        return false;
      }
      dest = static_cast<uint32_t>(static_cast<int32_t>(value));
    }
  } catch (...) {
    return false;
  }

  return true;
}

enum class MacroResult { NotMacro, Ok, Error };

// Constant pool built during assembly. Scalar literals are interned
// (deduplicated by type + 64-bit bit pattern) and emitted as a single
// `ldc <index>`, replacing the previous multi-instruction synthesis for wide
// immediates. The pool becomes its own .bc section and a GC root at run time.
struct ConstPool {
  vector<Unit> entries;
  std::map<std::pair<int, uint64_t>, uint32_t> seen;

  uint32_t Intern(const Unit &u) {
    std::pair<int, uint64_t> key{static_cast<int>(u.type), u.value.ui};
    auto it = seen.find(key);
    if (it != seen.end()) {
      return it->second;
    }
    uint32_t idx = static_cast<uint32_t>(entries.size());
    entries.push_back(u);
    seen.emplace(key, idx);
    return idx;
  }
};

MacroResult TryExpandMacro(vector<string> &assembly, Program &prog, vector<Unit> &data_section,
                           vector<string> &symbol_names, ConstPool &pool, size_t line) {
  if (assembly.empty()) {
    return MacroResult::NotMacro;
  }

  bool is_macro = strcmp(assembly[0].data(), kCStrPushImm) == 0 ||
                  strcmp(assembly[0].data(), kCStrPushUImm) == 0 ||
                  strcmp(assembly[0].data(), kCStrPushFP) == 0;
  if (!is_macro) {
    return MacroResult::NotMacro;
  }

  if (assembly.size() < 2) {
    AsmError(line, "missing literal for macro: %s", assembly[0].c_str());
    return MacroResult::Error;
  }

  // Helper to construct instruction: (arg << 7) | inst_id
  auto make_code = [](uint32_t arg, Inst id) -> Code {
    return (arg << 7) | static_cast<uint32_t>(id);
  };

  // Intern `unit` in the pool and emit one `ldc <index>`.
  auto emit_ldc = [&](const Unit &unit) {
    uint32_t idx = pool.Intern(unit);
    prog.push_back(make_code(idx, Inst::LoadConst));
  };

#define IS_MACRO(_str) (strcmp(assembly[0].data(), _str) == 0)

  if (IS_MACRO(kCStrPushImm)) {
    int base = GetIntLiteralBase(assembly[1]);
    if (base == 2) {
      assembly[1] = assembly[1].substr(2, assembly[1].size() - 2);
    }
    int64_t value;
    try {
      value = stoll(assembly[1], nullptr, base);
    } catch (const std::exception &) {
      AsmError(line, "invalid literal for pushimm");
      return MacroResult::Error;
    }
    emit_ldc(Unit::MakeInt(value));
  } else if (IS_MACRO(kCStrPushUImm)) {
    int base = GetIntLiteralBase(assembly[1]);
    if (base == 2) {
      assembly[1] = assembly[1].substr(2, assembly[1].size() - 2);
    }
    uint64_t value;
    try {
      value = stoull(assembly[1], nullptr, base);
    } catch (const std::exception &) {
      AsmError(line, "invalid literal for pushuimm");
      return MacroResult::Error;
    }
    emit_ldc(Unit::MakeUInt(value));
  } else if (IS_MACRO(kCStrPushFP)) {
    double value;
    try {
      value = stod(assembly[1]);
    } catch (const std::exception &) {
      AsmError(line, "invalid literal for pushfp");
      return MacroResult::Error;
    }
    emit_ldc(Unit::MakeFP(value));
  }
#undef IS_MACRO

  return MacroResult::Ok;
}

// 0 = fine, 1 = warning, 2 = critical error
unsigned int TryExpandJumpInsn(vector<string> &assembly, Program &prog, Labels &labels) {
  if (assembly.size() < 2) {
    return false;
  }

  bool result = true;

  auto it = labels.find(assembly[1]);

  if (it == labels.end()) {
    AsmError(0, "undefined label in jump/branch/call: %s", assembly[1].c_str());
    result = false;
  } else {
    uint64_t offset = it->second;

#define IS_INSN(_str) (strcmp(assembly[0].data(), _str) == 0)
    // if offset's binary length large than 25bit, use FarJump/FarBranch instead.
    if (offset > kMaxJumpArg) {
      if (IS_INSN("call") || IS_INSN("closnew")) {
        // There is no far-call/far-closure opcode; the 25-bit argument cannot
        // reach this target.
        AsmError(0, "far call/closure not supported: %s", assembly[1].c_str());
        result = false;
      } else {
        if (offset <= UINT32_MAX) {
          uint32_t hi = static_cast<uint32_t>(offset >> 16);
          uint32_t lo = static_cast<uint32_t>(offset & UINT16_MAX);
          prog.push_back((hi << 7) + Code(Inst::PushHalfWordImmSL16));
          prog.push_back((lo << 7) + Code(Inst::PushHalfWordImm));
          prog.push_back(FoldedCode(Inst::Add, kTyUInt));
        } else {
          uint32_t hi32 = static_cast<uint32_t>(offset >> 32);
          uint32_t lo32 = static_cast<uint32_t>(offset & UINT32_MAX);
          // load hi32
          prog.push_back(((hi32 >> 16) << 7) + Code(Inst::PushHalfWordImmSL16));
          prog.push_back(((hi32 & UINT16_MAX) << 7) + Code(Inst::PushHalfWordImm));
          prog.push_back(FoldedCode(Inst::Add, kTyUInt));
          prog.push_back(FoldedImmCode(Inst::ShiftImm, kShiftL, 32u));
          // load lo32
          uint32_t hi = lo32 >> 16;
          uint32_t lo = lo32 & UINT16_MAX;
          prog.push_back((hi << 7) + Code(Inst::PushHalfWordImmSL16));
          prog.push_back((lo << 7) + Code(Inst::PushHalfWordImm));
          prog.push_back(FoldedCode(Inst::Add, kTyUInt));
          // final combination
          prog.push_back(FoldedCode(Inst::Add, kTyUInt));
        }

        // Push target insn that will consume stack top.
        if (IS_INSN("jmp") || IS_INSN("farjmp")) {
          prog.push_back(Code(Inst::FarJump));
        } else if (IS_INSN("branch") || IS_INSN("farbranch")) {
          prog.push_back(Code(Inst::FarBranch));
        }
      }
    } else {
      Code insn = offset << 7;

      if (IS_INSN("jmp") || IS_INSN("farjmp")) {
        insn += Code(Inst::Jump);
      } else if (IS_INSN("branch") || IS_INSN("farbranch")) {
        insn += Code(Inst::Branch);
      } else if (IS_INSN("call")) {
        insn += Code(Inst::Call);
      } else if (IS_INSN("closnew")) {
        insn += Code(Inst::ClosureNew);
      }

      prog.push_back(insn);
    }
#undef IS_INSN
  }

  return result;
}

SectionType GetSectionType(string_view str) {
  auto result = SectionType::Null;

  if (str == kCStrDataSectBegin)
    result = SectionType::Data;
  else if (str == kCStrAngoraHeaderBegin)
    result = SectionType::Identity;
  else if (str == kCStrCodeSectBegin)
    result = SectionType::Code;

  return result;
}

bool ParseDataSection(FILE *fp, GcHeap &heap, vector<Unit> &data_section,
                        vector<string> &symbol_names, size_t &line) {
  bool result = true;

  if (kDebugMsgSwitch) {
    AsmInfo("data section begin");
  }

  auto get_unit_type = [&](string_view str) -> UnitType {
    if (str == kCStrDataInt)
      return UnitType::Int;
    else if (str == kCStrDataUInt)
      return UnitType::UInt;
    else if (str == kCStrDataFP)
      return UnitType::FP;
    else if (str == kCStrDataStr)
      return UnitType::Str;
    else if (str == kCStrDataNil)
      return UnitType::Nil;
    return UnitType::Invalid;
  };

  // We can use ReadInst() here.
  // Store data symbols' orig name in separate vector, and use vector's offset as
  // index in real run.
  vector<string> data_sectline;
  size_t cur_line = line;
  // Data section format: <type string> <id> <data>
  while (ReadInst(data_sectline, fp, line, cur_line)) {
    if (data_sectline.empty()) continue;

    string_view type(data_sectline[0]);

    if (type == kCStrSectEnd) break;

    // Require at least 2 tokens: type + id
    if (data_sectline.size() < 2) {
      AsmError(cur_line, "invalid data section: missing token");
      result = false;
      break;
    }

    string_view id(data_sectline[1]);

    if (kDebugMsgSwitch) {
      string echo;
      for (size_t i = 0; i < data_sectline.size(); ++i) {
        echo += data_sectline[i];
        if (i != data_sectline.size() - 1) {
          echo += ' ';
        }
      }
      AsmInfo("%s", echo.c_str());
    }

    UnitType t = get_unit_type(type);

    if (t == UnitType::Invalid) {
      AsmError(cur_line, "invalid data symbol type");
      result = false;
      break;
    }

    // Nil carries no value; every other type needs the value token.
    if (t != UnitType::Nil && data_sectline.size() < 3) {
      AsmError(cur_line, "invalid data section: missing value");
      result = false;
      break;
    }

    string_view data_str = data_sectline.size() >= 3 ? string_view(data_sectline[2]) : string_view();

    Unit unit;
    unit.type = t;

    switch (t) {
      case UnitType::Int: {
        try {
          unit.value.ui = static_cast<uint64_t>(stoll(data_str.data()));
        } catch (...) {
          AsmError(cur_line, "invalid int literal in data section");
          result = false;
          break;
        }
        break;
      }
      case UnitType::UInt: {
        try {
          unit.value.ui = stoull(data_str.data());
        } catch (...) {
          AsmError(cur_line, "invalid uint literal in data section");
          result = false;
          break;
        }
        break;
      }
      case UnitType::FP: {
        try {
          unit.value.fp = stod(data_str.data());
        } catch (...) {
          AsmError(cur_line, "invalid float literal in data section");
          result = false;
          break;
        }
        break;
      }
      case UnitType::Str: {
        try {
          const char *str_data = data_str.data();
          size_t sz = data_str.size();
          unit.value.str = heap.Alloc(str_data, sz);
        } catch (...) {
          AsmError(cur_line, "string allocation failed in data section");
          result = false;
          break;
        }
        break;
      }
      case UnitType::Nil: {
        // No payload; `unit` already holds value 0.
        break;
      }
      default: {
        // Unreachable: `t` was already validated against UnitType::Invalid.
        break;
      }
    }

    if (!result) {
      break;
    }

    data_section.push_back(unit);
    symbol_names.push_back(string(id));
  }

  if (kDebugMsgSwitch) {
    AsmInfo("data section end");
  }

  return result;
}

// Parsing angora assembly file. Return true if parsing is fine.
// Collects all labels so the caller can write them into the bytecode.
bool ParseAsm(FILE *fp, GcHeap &heap, Program &prog, vector<Unit> &data_section,
                vector<string> &symbol_names, ConstPool &pool, Labels &out_labels, size_t &line) {
  Labels labels;
  uint32_t inst = 0, args = 0;
  vector<string> assembly;
  size_t cur_line = line;
  bool fine = true;

  // Forward-reference jumps (target label not yet seen) are recorded
  // here and patched after the whole program is read.
  struct PendingJump {
    size_t index;  // index of the placeholder in prog
    string label;  // target label name
    int kind;      // 0 = jmp, 1 = branch, 2 = call
  };
  vector<PendingJump> pending;

  while (ReadInst(assembly, fp, line, cur_line)) {
    if (assembly.empty()) {
      continue;
    }

    if (IsLabelString(assembly[0])) {
      auto substr = assembly[0].substr(0, assembly[0].size() - 1);
      labels.insert(Label(substr.data(), prog.size()));
      continue;
    }

    // Function section markers. `.func <name>` registers <name> as a
    // label at the next instruction offset (the entry); `.endfunc`
    // closes the block. Neither is emitted as an instruction.
    if (assembly[0] == kCStrFuncBegin) {
      if (assembly.size() < 2) {
        AsmError(cur_line, "invalid .func: missing name");
        fine = false;
        break;
      }
      labels.insert(Label(assembly[1], prog.size()));
      continue;
    }
    if (assembly[0] == kCStrFuncEnd) {
      continue;
    }
    // Identity-section directive; accepted and ignored.
    if (assembly[0] == kCStrDotVersion) {
      continue;
    }

    // if we found a non-code block, enter subprocess until parsing completion.
    switch (GetSectionType(assembly[0])) {
      case SectionType::Data: {
        fine = ParseDataSection(fp, heap, data_section, symbol_names, line);
        continue;
        break;
      }
      case SectionType::Identity: {
        // Identity-section marker: consume it; its directives are ignored.
        continue;
        break;
      }
      case SectionType::Code: {
        // Code-section marker: nothing to do; we don't follow as's syntax.
        continue;
        break;
      }
      default: {
        break;
      }
    }

    args = 0;
    if (assembly.size() > 2) {
      AsmError(cur_line, "too many operands for '%s'", assembly[0].c_str());
      fine = false;
      break;
    }

    if (kDebugMsgSwitch) {
      string echo;
      for (size_t i = 0; i < assembly.size(); ++i) {
        echo += assembly[i];
        if (i != assembly.size() - 1) {
          echo += ' ';
        }
      }
      AsmInfo("%s", echo.c_str());
    }

    MacroResult macro = TryExpandMacro(assembly, prog, data_section, symbol_names, pool, cur_line);
    if (macro == MacroResult::Ok) {
      continue;
    }
    if (macro == MacroResult::Error) {
      fine = false;
      break;
    }

    uint32_t variant = 0;
    bool folded = false;
    if (IsSectionRelatedString(assembly[0]) ||
        !ResolveInst(inst, variant, folded, assembly[0])) {
      AsmError(cur_line, "invalid instruction: %s", assembly[0].data());
      fine = false;

      break;
    }
    // Folded ops carry their variant in the low argument bits (0 for the rest).
    args = variant;

    if (IsJumpInst(static_cast<Inst>(inst))) {
      string target;

      if (assembly.size() < 2 || !ParseLabelRef(assembly[1], target)) {
        AsmError(cur_line, "invalid jump target (expected @label): %s",
                 assembly.size() >= 2 ? assembly[1].c_str() : "");
        fine = false;
        break;
      }

      // Strip the reference prefix so the label lookup and expansion below
      // work on the bare name.
      assembly[1] = target;

      if (labels.find(target) == labels.end()) {
        // Forward reference: emit a placeholder now, resolve it after
        // the whole program is read.
        int kind = 0;  // 0 = jmp, 1 = branch, 2 = call, 3 = closnew
        if (assembly[0] == "branch" || assembly[0] == "farbranch") {
          kind = 1;
        } else if (assembly[0] == "call") {
          kind = 2;
        } else if (assembly[0] == "closnew") {
          kind = 3;
        }
        pending.push_back({prog.size(), target, kind});
        prog.push_back(Code(0));
        continue;
      }
      // Known label: resolve now (handles near and far targets).
      if (!TryExpandJumpInsn(assembly, prog, labels)) {
        AsmError(cur_line, "failed to expand jump insn");
        fine = false;
      }
      continue;
    }

    if (inst == uint32_t(Inst::Load) || inst == uint32_t(Inst::Store)) {
      if (assembly.size() < 2) {
        AsmError(cur_line, "invalid ld/st instruction usage");
        fine = false;
      } else if (!ResolveDataSymbol(assembly[1], symbol_names, args)) {
        AsmError(cur_line, "undefined data symbol in ld/st: %s", assembly[1].c_str());
        fine = false;
      }
    } else if (inst == uint32_t(Inst::RelativeLoad) || inst == uint32_t(Inst::RelativeStore)) {
      if (assembly.size() < 2) {
        AsmError(cur_line, "invalid ldr/str instruction usage");
        fine = false;
      } else if (assembly[1][0] == kCharDataIdPrefix) {
        if (!ResolveDataSymbol(assembly[1], symbol_names, args)) {
          AsmError(cur_line, "undefined data symbol in ldr/str: %s", assembly[1].c_str());
          fine = false;
        }
      } else if (!TryParseImm(assembly[1], true, args)) {
        AsmError(cur_line, "invalid literal for ldr/str: %s", assembly[1].c_str());
        fine = false;
      }
    } else if (folded) {
      // Folded immediate forms (shift/rotate) pack the count above the variant;
      // all other folded ops are operand-free.
      if (IsFoldedImmOp(static_cast<Inst>(inst))) {
        uint32_t imm = 0;
        if (assembly.size() != 2 || !TryParseImm(assembly[1], true, imm) || imm > kMaxFoldedImm) {
          AsmError(cur_line, "'%s' needs a %u-bit immediate operand", assembly[0].c_str(),
                   25 - kVarBits);
          fine = false;
        } else {
          args = (imm << kVarBits) | variant;
        }
      } else if (assembly.size() == 2) {
        AsmError(cur_line, "'%s' takes no operand", assembly[0].c_str());
        fine = false;
      }
    } else if (assembly.size() == 2) {
      if (!TryParseImm(assembly[1], IsUnsignedInst(static_cast<Inst>(inst)), args)) {
        AsmError(cur_line, "invalid literal: %s", assembly[1].c_str());
        fine = false;
      }
    }

    if (!fine) break;

    Code output = args << 7;
    output += inst;

    if (kDebugMsgSwitch) {
      AsmInfo("encoded 0x%x", output);
    }

    prog.push_back(output);
  }

  // Resolve forward-reference jumps recorded above.
  for (auto &pj : pending) {
    auto it = labels.find(pj.label);
    if (it == labels.end()) {
      AsmError(cur_line, "undefined label in jump: %s", pj.label.c_str());
      fine = false;
      break;
    }
    size_t offset = it->second;
    if (offset > kMaxJumpArg) {
      AsmError(cur_line, "forward jump to '%s' exceeds 25-bit range", pj.label.c_str());
      fine = false;
      break;
    }
    Inst op = (pj.kind == 1) ? Inst::Branch
              : (pj.kind == 2) ? Inst::Call
              : (pj.kind == 3) ? Inst::ClosureNew
                               : Inst::Jump;
    prog[pj.index] = (Code(offset) << 7) + Code(op);
  }

  // Expose collected labels to the caller so they can be written
  // into a compiled bytecode file.
  out_labels = labels;

  return fine;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    AsmError(0, "invalid arguments");
    PrintUsage(stderr);
    return kExitUsage;
  }

  bool compile_mode = false;
  const char *asm_file = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "-debug") == 0) {
      kDebugMsgSwitch = 1;
    } else if (strcmp(argv[i], "-compile") == 0) {
      compile_mode = true;
    } else if (strcmp(argv[i], "-run") == 0) {
      // Default mode, accepted for compatibility.
    } else if (strcmp(argv[i], "-help") == 0) {
      PrintUsage(stdout);
      return kExitOk;
    } else if (asm_file == nullptr) {
      asm_file = argv[i];
    } else {
      AsmError(0, "invalid arguments");
      PrintUsage(stderr);
      return kExitUsage;
    }
  }

  if (asm_file == nullptr) {
    AsmError(0, "invalid arguments");
    PrintUsage(stderr);
    return kExitUsage;
  }

  g_asm_file = asm_file;

  if (kDebugMsgSwitch) {
    AsmInfo("registered %lu instructions", kInstStrings.size());
  }

  // open asm file
  auto fp = fopen(asm_file, "r");
  if (fp == nullptr) {
    AsmError(0, "cannot open assembly file: %s", asm_file);
    return kExitError;
  }

  Program prog;
  vector<Unit> data_section;
  vector<string> symbol_names;
  ConstPool const_pool;
  Labels labels;
  GcHeap heap;
  bool fine = true;
  size_t line = 1;

  // Backstop: a malformed input must never terminate the process. Any
  // exception that escapes the parser is reported as a normal parse error.
  try {
    fine = ParseAsm(fp, heap, prog, data_section, symbol_names, const_pool, labels, line);
  } catch (const std::exception &e) {
    AsmError(0, "parse error: %s", e.what());
    fine = false;
  } catch (...) {
    AsmError(0, "parse error: unknown exception");
    fine = false;
  }
  fclose(fp);

  if (g_lex_error) {
    fine = false;
  }

  if (!fine) {
    return kExitError;
  }

  if (!prog.empty()) {
    if (!compile_mode) {
      if (kDebugMsgSwitch) {
        AsmInfo("running program");
      }
      Machine machine(labels);
      machine.AdoptHeap(std::move(heap));
      machine.AcceptData(data_section);
      machine.AcceptConsts(const_pool.entries);
      machine.AcceptDbgNames(symbol_names);
      if (!machine.Run(prog)) {
        return kExitError;
      }
    } else {
      if (kDebugMsgSwitch) {
        AsmInfo("writing bytecode");
      }
      string out(asm_file);
      out.append(".bc");

      FILE *ofp = fopen(out.data(), "wb");
      if (ofp == nullptr) {
        AsmError(0, "cannot write bytecode file: %s", out.c_str());
        return kExitError;
      }

      if (!data_section.empty()) {
        WriteDataSectionToFile(ofp, data_section);
      }
      if (!const_pool.entries.empty()) {
        WriteConstSectionToFile(ofp, const_pool.entries);
      }
      WriteBytecode(ofp, prog, labels);
      fclose(ofp);
    }
  } else {
    AsmError(0, "empty program: no instructions to run");
    return kExitError;
  }

  return kExitOk;
}
