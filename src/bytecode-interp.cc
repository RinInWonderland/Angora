#include <cstdio>
#include <cstring>
#include <vector>

#include "bytecode-io.h"
#include "log.h"
#include "machine.h"

using std::FILE;
using std::fopen;
using std::fprintf;

static void PrintUsage(FILE *out) {
  fprintf(out, "usage: angora-vm <file.bc> [-help]\n");
}

int main(int argc, char **argv) {
  if (argc == 2 && std::strcmp(argv[1], "-help") == 0) {
    PrintUsage(stdout);
    return kExitOk;
  }
  if (argc != 2) {
    LogWrite(LogSeverity::Error, "angora-vm", {}, "no bytecode file given");
    PrintUsage(stderr);
    return kExitUsage;
  }

  SetBytecodeSource(argv[1]);

  Program prog;
  std::vector<Unit> data_section;
  std::vector<Unit> consts;
  Labels func_labels_map;
  GcHeap heap;

  FILE *fp = fopen(argv[1], "rb");
  if (fp == nullptr) {
    LogWrite(LogSeverity::Error, "angora-vm", {argv[1], 0}, "cannot open file");
    return kExitError;
  }
  bool had_warnings = false;
  bool loaded =
      LoadProgram(fp, heap, prog, data_section, consts, func_labels_map, &had_warnings);
  fclose(fp);

  if (!loaded) {
    // LoadProgram already reported the precise failure (with file offset).
    return kExitError;
  }
  if (prog.empty()) {
    LogWrite(LogSeverity::Error, "angora-vm", {argv[1], 0}, "empty program");
    return kExitError;
  }
  Machine machine(func_labels_map);
  machine.AdoptHeap(std::move(heap));
  if (!data_section.empty()) {
    machine.AcceptData(data_section);
  }
  if (!consts.empty()) {
    machine.AcceptConsts(consts);
  }
  if (!machine.Run(prog)) {
    return kExitError;
  }

  return kExitOk;
}
