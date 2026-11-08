// Golden-byte test for the .bc serialization layer.
//
// Drives LoadProgram / WriteDataSectionToFile / WriteBytecode through fmemopen
// and open_memstream with HARDCODED little-endian byte vectors. Because the
// ReadLE32/WriteLE32 helpers are byte-oriented, this test is host-order
// independent: any place that still does a native-order integer read/write
// would diverge from the golden bytes (notably on a big-endian host), so this
// locks the on-disk format contract without needing a big-endian machine.

#define _POSIX_C_SOURCE 200809L

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "bytecode-io.h"
#include "machine.h"

static int g_failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                        \
    }                                                                      \
  } while (0)

// data: UInt 0x1122334455667788 ; code: 0xDEADBEEF, 0x01020304 ; funcs: main->0
static const unsigned char kGoldenRead[] = {
    0x41, 0x54, 0x41, 0x44,                          // kMagicSectData
    0x02, 0x00, 0x00, 0x00,                          // UnitType::UInt
    0x44, 0x33, 0x22, 0x11, 0x88, 0x77, 0x66, 0x55,  // hi, lo
    0x44, 0x4E, 0x45, 0x2E,                          // kMagicSectEnd
    0x02, 0x00, 0x00, 0x00,                          // code_count = 2
    0xEF, 0xBE, 0xAD, 0xDE,                          // code[0]
    0x04, 0x03, 0x02, 0x01,                          // code[1]
    0x01, 0x00, 0x00, 0x00,                          // func_count = 1
    0x04, 0x00, 0x00, 0x00,                          // name_len = 4
    0x6D, 0x61, 0x69, 0x6E,                          // "main"
    0x00, 0x00, 0x00, 0x00,                          // entry = 0
    0x43, 0x44, 0x4E, 0x45,                          // kMagicSectEndcode
};

// Like kGoldenRead but with an extra Str "hi", used to check the writer
// byte-for-byte (including the packed-string path).
static const unsigned char kGoldenWrite[] = {
    0x41, 0x54, 0x41, 0x44,                          // kMagicSectData
    0x02, 0x00, 0x00, 0x00,                          // UnitType::UInt
    0x44, 0x33, 0x22, 0x11, 0x88, 0x77, 0x66, 0x55,  // hi, lo
    0x04, 0x00, 0x00, 0x00,                          // UnitType::Str
    0x68, 0x69, 0x00, 0x00,                          // "hi"
    0x00, 0x00, 0x00, 0x00,                          // string terminator
    0x44, 0x4E, 0x45, 0x2E,                          // kMagicSectEnd
    0x02, 0x00, 0x00, 0x00,                          // code_count = 2
    0xEF, 0xBE, 0xAD, 0xDE,                          // code[0]
    0x04, 0x03, 0x02, 0x01,                          // code[1]
    0x01, 0x00, 0x00, 0x00,                          // func_count = 1
    0x04, 0x00, 0x00, 0x00,                          // name_len = 4
    0x6D, 0x61, 0x69, 0x6E,                          // "main"
    0x00, 0x00, 0x00, 0x00,                          // entry = 0
    0x43, 0x44, 0x4E, 0x45,                          // kMagicSectEndcode
};

// ReadLE32/WriteLE32 convert to/from the exact on-disk byte order.
static void TestLE32() {
  const unsigned char in_bytes[4] = {0x01, 0x02, 0x03, 0x04};
  std::vector<unsigned char> in_buf(in_bytes, in_bytes + 4);
  FILE *in = fmemopen(in_buf.data(), in_buf.size(), "rb");
  uint32_t v = 0;
  CHECK(ReadLE32(in, v));
  CHECK(v == 0x04030201u);
  fclose(in);

  char *out = nullptr;
  size_t n = 0;
  FILE *os = open_memstream(&out, &n);
  WriteLE32(os, 0x04030201u);
  fclose(os);
  CHECK(n == 4);
  CHECK(n == 4 && std::memcmp(out, in_bytes, 4) == 0);
  std::free(out);
}

// A hardcoded little-endian stream must parse to the expected program/data.
static void TestLoadGolden() {
  std::vector<unsigned char> bytes(kGoldenRead, kGoldenRead + sizeof(kGoldenRead));
  FILE *in = fmemopen(bytes.data(), bytes.size(), "rb");

  GcHeap heap;
  Program prog;
  std::vector<Unit> data;
  std::vector<Unit> consts;
  Labels labels;
  CHECK(LoadProgram(in, heap, prog, data, consts, labels));
  fclose(in);

  CHECK(data.size() == 1);
  CHECK(data[0].type == UnitType::UInt);
  CHECK(data[0].value.ui == 0x1122334455667788ull);

  CHECK(prog.size() == 2);
  CHECK(prog.size() == 2 && prog[0] == 0xDEADBEEFu);
  CHECK(prog.size() == 2 && prog[1] == 0x01020304u);

  CHECK(labels.size() == 1);
  CHECK(labels.find("main") != labels.end());
  CHECK(labels.find("main") != labels.end() && labels["main"] == 0);
}

// The writer must emit exactly the golden little-endian bytes.
static void TestWriteGolden() {
  GcHeap heap;
  std::vector<Unit> data;
  data.push_back(Unit::MakeUInt(0x1122334455667788ull));
  data.push_back(Unit::MakeStr(heap, "hi", 2));

  Program prog = {0xDEADBEEFu, 0x01020304u};
  Labels labels;
  labels["main"] = 0;

  char *out = nullptr;
  size_t n = 0;
  FILE *os = open_memstream(&out, &n);
  WriteDataSectionToFile(os, data);
  WriteBytecode(os, prog, labels);
  fclose(os);

  CHECK(n == sizeof(kGoldenWrite));
  CHECK(n == sizeof(kGoldenWrite) && std::memcmp(out, kGoldenWrite, n) == 0);
  std::free(out);
}

// write -> read round trip must preserve everything, including the constant pool.
static void TestRoundTrip() {
  GcHeap heap;
  std::vector<Unit> data;
  data.push_back(Unit::MakeUInt(0x1122334455667788ull));
  data.push_back(Unit::MakeStr(heap, "hi", 2));
  std::vector<Unit> consts;
  consts.push_back(Unit::MakeInt(-5));
  consts.push_back(Unit::MakeFP(3.5));
  consts.push_back(Unit::MakeUInt(0xFFFFFFFFFFFFFFFFull));
  Program prog = {0xDEADBEEFu, 0x01020304u};
  Labels labels;
  labels["main"] = 7;

  char *out = nullptr;
  size_t n = 0;
  FILE *os = open_memstream(&out, &n);
  WriteDataSectionToFile(os, data);
  WriteConstSectionToFile(os, consts);
  WriteBytecode(os, prog, labels);
  fclose(os);

  GcHeap heap2;
  Program prog2;
  std::vector<Unit> data2;
  std::vector<Unit> consts2;
  Labels labels2;
  FILE *in = fmemopen(out, n, "rb");
  CHECK(LoadProgram(in, heap2, prog2, data2, consts2, labels2));
  fclose(in);
  std::free(out);

  CHECK(prog2 == prog);
  CHECK(data2.size() == 2);
  CHECK(data2.size() == 2 && data2[0].type == UnitType::UInt);
  CHECK(data2.size() == 2 && data2[0].value.ui == 0x1122334455667788ull);
  CHECK(data2.size() == 2 && data2[1].type == UnitType::Str);
  CHECK(data2.size() == 2 && std::string(data2[1].value.str) == "hi");
  CHECK(labels2.size() == 1 && labels2["main"] == 7);

  CHECK(consts2.size() == 3);
  CHECK(consts2.size() == 3 && consts2[0].type == UnitType::Int);
  CHECK(consts2.size() == 3 && consts2[0].value.i == -5);
  CHECK(consts2.size() == 3 && consts2[1].type == UnitType::FP);
  CHECK(consts2.size() == 3 && consts2[1].value.fp == 3.5);
  CHECK(consts2.size() == 3 && consts2[2].type == UnitType::UInt);
  CHECK(consts2.size() == 3 && consts2[2].value.ui == 0xFFFFFFFFFFFFFFFFull);
}

int main() {
  TestLE32();
  TestLoadGolden();
  TestWriteGolden();
  TestRoundTrip();

  if (g_failures == 0) {
    std::printf("bc_format_test: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "bc_format_test: %d check(s) failed\n", g_failures);
  return 1;
}
