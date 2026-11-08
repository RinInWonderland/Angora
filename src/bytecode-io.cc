#include "bytecode-io.h"

#include <cstdarg>

#include "log.h"

// See bytecode-io.h for the format contract. All multi-byte fields go through
// ReadLE32/WriteLE32 so the on-disk bytes are identical on any host.

// Source file attached to loader diagnostics; set by the VM once the input path
// is known. Loader warnings/errors are emitted through BcLog below.
static const char *g_bc_file = nullptr;

void SetBytecodeSource(const char *file) { g_bc_file = file; }

static void BcLog(LogSeverity sev, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  LogVWrite(sev, "angora-vm", LogLocation{g_bc_file, 0}, fmt, ap);
  va_end(ap);
}

// Read one typed entry body (its type tag already consumed) into `unit`.
// Returns false on a malformed type or a short read.
static bool ReadUnitBody(FILE *fp, GcHeap &heap, UnitType type, Unit &unit) {
  unit = Unit();
  unit.type = type;

  if (type == UnitType::Int || type == UnitType::UInt || type == UnitType::FP) {
    uint32_t hi, lo;
    if (!ReadLE32(fp, hi)) return false;
    if (!ReadLE32(fp, lo)) return false;
    unit.value.ui = (static_cast<uint64_t>(hi) << 32) | lo;
    return true;
  }
  if (type == UnitType::Str) {
    // Strings are stored as 4-byte chunks terminated by a zero word.
    vector<char> temp_buf;
    uint32_t chunk = 0;
    while (true) {
      if (!ReadLE32(fp, chunk)) {
        return false;  // truncated: no terminating zero word
      }
      if (chunk == 0) {
        break;
      }
      for (int i = 0; i < 4; ++i) {
        temp_buf.push_back(static_cast<char>((chunk >> (i * 8)) & 0xFF));
      }
    }
    // The final 4-byte word is zero-padded, so trim at the first NUL to get the
    // true byte length (the whole buffer is otherwise NUL-terminated already).
    size_t len = 0;
    while (len < temp_buf.size() && temp_buf[len] != '\0') {
      ++len;
    }
    unit.value.str = heap.Alloc(temp_buf.data(), len);
    return true;
  }
  if (type == UnitType::Nil) {
    return true;  // no payload; value stays 0
  }
  return false;
}

// True for the entry types a data/const section may carry. Arr/Closure are
// valid UnitType values but are never serialized.
static bool IsSerializableType(UnitType type) {
  return type == UnitType::Int || type == UnitType::UInt || type == UnitType::FP ||
         type == UnitType::Str || type == UnitType::Nil;
}

// Read a `<type> <payload>` entry run terminated by kMagicSectEnd. Returns false
// (after logging a diagnostic with the file offset) on an invalid entry type,
// a short read, or an unterminated section. `section` names the section in
// diagnostics ("data" or "const").
static bool ReadUnitsUntilEnd(FILE *fp, GcHeap &heap, vector<Unit> &out, const char *section) {
  while (true) {
    uint32_t type_raw = 0;
    if (!ReadLE32(fp, type_raw)) {
      BcLog(LogSeverity::Error, "unterminated %s section at offset=%ld", section, ftell(fp));
      return false;
    }
    if (type_raw == kMagicSectEnd) {
      return true;
    }

    long type_off = ftell(fp) - 4;
    UnitType type = static_cast<UnitType>(type_raw);
    if (!IsSerializableType(type)) {
      BcLog(LogSeverity::Error, "invalid %s entry type=0x%x at offset=%ld", section, type_raw,
            type_off);
      return false;
    }

    Unit unit;
    if (!ReadUnitBody(fp, heap, type, unit)) {
      BcLog(LogSeverity::Error, "truncated %s entry at offset=%ld", section, ftell(fp));
      return false;
    }
    out.push_back(unit);
  }
}

// Write one typed entry body (type + payload), shared by the data and const
// sections. Strings are compacted four bytes per u32 and NUL-terminated.
static void WriteUnitBody(FILE *fp, const Unit &unit) {
  WriteLE32(fp, static_cast<uint32_t>(unit.type));
  switch (unit.type) {
    case UnitType::Int:
    case UnitType::UInt:
    case UnitType::FP: {
      uint32_t hi =
          static_cast<uint32_t>((static_cast<uint64_t>(unit.value.ui) >> 32) & 0xFFFFFFFF);
      uint32_t lo = static_cast<uint32_t>(unit.value.ui) & 0xFFFFFFFF;
      WriteLE32(fp, hi);
      WriteLE32(fp, lo);
      break;
    }
    case UnitType::Str: {
      const char *str = unit.value.str;
      size_t size = strlen(str);
      uint32_t compacted;
      for (size_t i = 0; i < size; i += 4) {
        compacted = 0;
        for (size_t j = 0; j < 4; ++j) {
          if (i + j < size) {
            compacted |=
                (static_cast<uint32_t>(static_cast<unsigned char>(str[i + j])) << (j * 8));
          }
        }
        WriteLE32(fp, compacted);
      }
      compacted = 0;
      WriteLE32(fp, compacted);
      break;
    }
    case UnitType::Nil:
    case UnitType::Invalid:
    default: {
      break;
    }
  }
}

bool ParseDataSection(FILE *fp, GcHeap &heap, vector<Unit> &data_section) {
  return ReadUnitsUntilEnd(fp, heap, data_section, "data");
}

bool ParseConstSection(FILE *fp, GcHeap &heap, vector<Unit> &consts) {
  return ReadUnitsUntilEnd(fp, heap, consts, "const");
}

bool LoadProgram(FILE *fp, GcHeap &heap, Program &prog, vector<Unit> &data_section,
                 vector<Unit> &consts, Labels &func_labels_map, bool *had_warnings) {
  if (had_warnings != nullptr) {
    *had_warnings = false;
  }

  // Step 1: optional data section at the start of the file.
  uint32_t magic = 0;
  fseek(fp, 0, SEEK_SET);
  if (ReadLE32(fp, magic) && magic == kMagicSectData) {
    if (!ParseDataSection(fp, heap, data_section)) {
      return false;
    }
    if (data_section.empty()) {
      if (had_warnings != nullptr) {
        *had_warnings = true;
      }
      BcLog(LogSeverity::Warning, "empty data section at offset=%ld", ftell(fp));
    }
  } else {
    // No .data section - code bytes start at position 0.
    fseek(fp, 0, SEEK_SET);
  }

  // Step 2: optional constant-pool section, directly after the data section.
  long before_const = ftell(fp);
  uint32_t const_magic = 0;
  if (ReadLE32(fp, const_magic) && const_magic == kMagicSectConst) {
    if (!ParseConstSection(fp, heap, consts)) {
      return false;
    }
  } else if (before_const >= 0) {
    // No const pool - rewind so the code-count header is read from here.
    fseek(fp, before_const, SEEK_SET);
  }

  // Step 3: code_count + code words.
  uint32_t code_count = 0;
  if (!ReadLE32(fp, code_count)) {
    BcLog(LogSeverity::Error, "missing code section at offset=%ld", ftell(fp));
    return false;
  }
  for (uint32_t i = 0; i < code_count; ++i) {
    Code code;
    if (!ReadLE32(fp, code)) {
      BcLog(LogSeverity::Error, "truncated instruction stream at offset=%ld (word %u/%u)",
            ftell(fp), i, code_count);
      return false;
    }
    prog.push_back(code);
  }

  // Step 4: optional function table, matching the writer's layout:
  //   u32 func_count, then { u32 name_len, u8 name[], u32 entry } * count,
  //   then the u32 ENDCODE sentinel.
  uint32_t func_count = 0;
  if (ReadLE32(fp, func_count)) {
    for (uint32_t i = 0; i < func_count; ++i) {
      uint32_t str_len = 0;
      if (!ReadLE32(fp, str_len)) {
        BcLog(LogSeverity::Error, "truncated function table at offset=%ld (entry %u/%u)", ftell(fp),
              i, func_count);
        return false;
      }
      string name(str_len, '\0');
      if (str_len > 0 && fread(&name[0], 1, str_len, fp) != str_len) {
        BcLog(LogSeverity::Error, "truncated function name at offset=%ld (entry %u/%u)", ftell(fp),
              i, func_count);
        return false;
      }
      uint32_t entry = 0;
      if (!ReadLE32(fp, entry)) {
        BcLog(LogSeverity::Error, "truncated function entry at offset=%ld (entry %u/%u)",
              ftell(fp), i, func_count);
        return false;
      }
      func_labels_map[name] = static_cast<size_t>(entry);
    }
    // Consume the trailing sentinel (best-effort validation).
    uint32_t endcode = 0;
    if (!ReadLE32(fp, endcode)) {
      if (had_warnings != nullptr) {
        *had_warnings = true;
      }
      BcLog(LogSeverity::Warning, "missing ENDCODE sentinel at offset=%ld", ftell(fp));
    } else if (endcode != kMagicSectEndcode) {
      if (had_warnings != nullptr) {
        *had_warnings = true;
      }
      BcLog(LogSeverity::Warning, "unexpected ENDCODE sentinel=0x%x at offset=%ld", endcode,
            ftell(fp) - 4);
    }
  }

  return true;
}

void WriteDataSectionToFile(FILE *fp, const vector<Unit> &data_section) {
  WriteLE32(fp, kMagicSectData);

  for (auto &unit : data_section) {
    WriteUnitBody(fp, unit);
  }

  WriteLE32(fp, kMagicSectEnd);
}

void WriteConstSectionToFile(FILE *fp, const vector<Unit> &consts) {
  WriteLE32(fp, kMagicSectConst);

  for (auto &unit : consts) {
    WriteUnitBody(fp, unit);
  }

  WriteLE32(fp, kMagicSectEnd);
}

void WriteBytecode(FILE *fp, const Program &prog, const Labels &labels) {
  // Narrow to u32 before serializing: writing sizeof(uint32_t) bytes from a
  // size_t would emit its high half on a 64-bit big-endian host.
  const uint32_t code_count = static_cast<uint32_t>(prog.size());

  WriteLE32(fp, code_count);

  for (Code code : prog) {
    WriteLE32(fp, code);
  }

  vector<pair<string, size_t>> funcs;
  for (auto &kv : labels) {
    funcs.push_back(kv);
  }

  const uint32_t func_count = static_cast<uint32_t>(funcs.size());
  WriteLE32(fp, func_count);

  for (auto &f : funcs) {
    // Write string length then string data
    const uint32_t len = static_cast<uint32_t>(f.first.size());
    WriteLE32(fp, len);
    if (len > 0) {
      fwrite(f.first.data(), 1, len, fp);
    }
    // Write entry offset
    const uint32_t entry = static_cast<uint32_t>(f.second);
    WriteLE32(fp, entry);
  }

  // ENDCODE sentinel for future extensibility
  WriteLE32(fp, kMagicSectEndcode);
}
