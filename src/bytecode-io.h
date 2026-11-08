#ifndef ANGORA_BYTECODE_IO_H_
#define ANGORA_BYTECODE_IO_H_

#include "machine.h"

// ---------------------------------------------------------------------------
// Shared bytecode (.bc) reader/writer.
//
// The on-disk format is fixed little-endian (see the format notes in
// machine.h); every multi-byte field goes through ReadLE32/WriteLE32, so these
// functions produce and consume identical bytes on any host. The assembler
// (`angora-interp -compile`), the bytecode VM (`angora-vm`) and the golden-byte
// format test (test/bc_format_test.cc) all share this code.
// ---------------------------------------------------------------------------

// Set the source file name attached to loader diagnostics (e.g. the VM's
// input path). Diagnostics go to stderr through log.h.
void SetBytecodeSource(const char *file);

// Parse the optional data section (`kMagicSectData` ... `kMagicSectEnd`),
// allocating strings in `heap`. Returns false (after logging a diagnostic with
// the file offset) on an invalid entry type, a short read, or an unterminated
// section.
bool ParseDataSection(FILE *fp, GcHeap &heap, vector<Unit> &data_section);

// Parse the optional constant-pool section (`kMagicSectConst` ...
// `kMagicSectEnd`), allocating strings in `heap`. Assumes the section magic has
// already been consumed. Returns false like ParseDataSection.
bool ParseConstSection(FILE *fp, GcHeap &heap, vector<Unit> &consts);

// Load a complete .bc stream: optional data section, optional constant pool,
// code, then the optional function table. Returns false (after logging a
// detailed error with the file offset) when the stream is malformed or
// truncated. The function table is optional; when absent `func_labels_map`
// stays empty. `had_warnings`, when non-null, is set to true if the stream
// loaded but carried non-fatal issues (empty data section, missing/unexpected
// ENDCODE sentinel).
bool LoadProgram(FILE *fp, GcHeap &heap, Program &prog, vector<Unit> &data_section,
                 vector<Unit> &consts, Labels &func_labels_map, bool *had_warnings = nullptr);

// Serialize the optional data section.
void WriteDataSectionToFile(FILE *fp, const vector<Unit> &data_section);

// Serialize the optional constant-pool section.
void WriteConstSectionToFile(FILE *fp, const vector<Unit> &consts);

// Serialize the code stream and function table.
void WriteBytecode(FILE *fp, const Program &prog, const Labels &labels);

#endif  // ANGORA_BYTECODE_IO_H_
