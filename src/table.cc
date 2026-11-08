#include "machine.h"

#include <cmath>
#include <cstdint>
#include <cstring>

// Table operations. See the TableObj/TableObj comments in machine.h for the
// layout and semantics. Keys are canonicalized so that Int and small UInt with
// the same numeric value are one key; an FP key is a distinct type. Nil,
// Invalid and NaN keys are rejected by ValidKey before these helpers run.

namespace {

// splitmix64 finalizer: spreads a 64-bit word so its low bits (used by the
// bucket mask) are well distributed.
inline uint64_t Mix64(uint64_t x) {
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebULL;
  x ^= x >> 31;
  return x;
}

// FNV-1a 64 over a byte run. Deterministic (no random seed) so table iteration
// and printed output stay reproducible.
uint64_t HashBytes(const char *s, size_t n) {
  uint64_t h = 14695981039346656037ULL;
  for (size_t i = 0; i < n; ++i) {
    h ^= static_cast<unsigned char>(s[i]);
    h *= 1099511628211ULL;
  }
  return h;
}

uint64_t HashUnit(const Unit &k) {
  switch (k.type) {
    case UnitType::Int:
      return Mix64(static_cast<uint64_t>(k.value.i));
    case UnitType::UInt:
      return Mix64(k.value.ui);
    case UnitType::FP: {
      double d = k.value.fp;
      uint64_t bits;
      if (d == 0.0) {
        bits = 0;  // normalize -0.0 to 0.0
      } else {
        std::memcpy(&bits, &d, sizeof(bits));
      }
      return Mix64(bits);
    }
    case UnitType::Str:
      return HashBytes(k.value.str, GcLenOf(k.value.str));
    case UnitType::Arr:
      return Mix64(reinterpret_cast<uintptr_t>(k.value.arr) >> 3);
    case UnitType::Closure:
      return Mix64(reinterpret_cast<uintptr_t>(k.value.closure) >> 3);
    case UnitType::Table:
      return Mix64(reinterpret_cast<uintptr_t>(k.value.table) >> 3);
    default:
      return 0;
  }
}

// Canonicalize Int/UInt: a UInt that fits in int64 becomes an Int, so
// `t[1]` and `t[1u]` are the same key. FP keys are left as-is (distinct type).
Unit CanonKey(const Unit &k) {
  if (k.type == UnitType::UInt && k.value.ui <= static_cast<uint64_t>(INT64_MAX)) {
    return Unit::MakeInt(static_cast<int64_t>(k.value.ui));
  }
  return k;
}

bool KeyEquals(const Unit &a, const Unit &b) {
  if (a.type != b.type) return false;
  switch (a.type) {
    case UnitType::Int:
      return a.value.i == b.value.i;
    case UnitType::UInt:
      return a.value.ui == b.value.ui;
    case UnitType::FP:
      return a.value.fp == b.value.fp;  // NaN is never equal
    case UnitType::Str: {
      size_t na = GcLenOf(a.value.str);
      size_t nb = GcLenOf(b.value.str);
      return na == nb && std::memcmp(a.value.str, b.value.str, na) == 0;
    }
    case UnitType::Arr:
      return a.value.arr == b.value.arr;
    case UnitType::Closure:
      return a.value.closure == b.value.closure;
    case UnitType::Table:
      return a.value.table == b.value.table;
    default:
      return false;
  }
}

// A canonical key is an array index when it is a non-negative Int.
bool CanonIndex(const Unit &k, uint64_t &idx) {
  if (k.type == UnitType::Int && k.value.i >= 0) {
    idx = static_cast<uint64_t>(k.value.i);
    return true;
  }
  return false;
}

void EnsureArrayCap(GcHeap &gc, TableObj *t, uint32_t need) {
  if (need <= t->array_cap) {
    return;
  }
  uint32_t cap = t->array_cap == 0 ? 8u : t->array_cap;
  while (cap < need) {
    cap *= 2u;
  }
  ArrData *nd = gc.AllocArrData(cap);  // fills Invalid holes
  if (t->array != nullptr) {
    for (uint32_t i = 0; i < t->array_size; ++i) {
      nd->items[i] = t->array->items[i];
    }
  }
  t->array = nd;
  t->array_cap = cap;
}

uint32_t MainPos(const TableObj *t, const Unit &canon) {
  return static_cast<uint32_t>(HashUnit(canon) & (t->node_count - 1));
}

bool HashFind(const TableObj *t, const Unit &canon, uint32_t &out_idx) {
  if (t->nodes == nullptr) {
    return false;
  }
  TableNode *nodes = t->nodes->items;
  uint32_t p = MainPos(t, canon);
  while (nodes[p].key.type != UnitType::Invalid) {
    if (KeyEquals(nodes[p].key, canon)) {
      out_idx = p;
      return true;
    }
    if (nodes[p].next == 0) {
      break;
    }
    p = nodes[p].next - 1;
  }
  return false;
}

// Pop the next free node slot (downward scan from free_hint). Returns
// UINT32_MAX when none is reachable.
uint32_t PopFreeSlot(TableObj *t) {
  TableNode *nodes = t->nodes->items;
  while (t->free_hint > 0) {
    uint32_t i = t->free_hint - 1;
    t->free_hint = i;
    if (nodes[i].key.type == UnitType::Invalid) {
      return i;
    }
  }
  return UINT32_MAX;
}

// Insert a key that is known to be absent. Returns false when no free node is
// reachable by the downward scan (the caller then resizes).
//
// Uses Lua's "Brent" placement: a node whose main position is occupied by a key
// that itself is NOT at its main position is moved to a free slot first, so a
// slot that is some key's main position always holds that key. This keeps
// deletion (which may promote a chain successor into the head slot) correct.
bool HashInsertNew(TableObj *t, const Unit &canon, const Unit &value) {
  uint32_t mi = MainPos(t, canon);
  TableNode *nodes = t->nodes->items;
  if (nodes[mi].key.type == UnitType::Invalid) {
    nodes[mi].key = canon;
    nodes[mi].value = value;
    nodes[mi].next = 0;
    t->hash_count += 1;
    return true;
  }
  uint32_t occupant_mp = MainPos(t, nodes[mi].key);
  uint32_t f = PopFreeSlot(t);
  if (f == UINT32_MAX) {
    return false;
  }
  if (occupant_mp != mi) {
    // The occupant is out of its main position: move it to the free slot and
    // relink its bucket, then take `mi` for the new key.
    uint32_t prev = occupant_mp;
    while (nodes[prev].next != mi + 1) {
      prev = nodes[prev].next - 1;
    }
    nodes[prev].next = f + 1;
    nodes[f] = nodes[mi];  // key, value and chain tail
    nodes[mi].key = canon;
    nodes[mi].value = value;
    nodes[mi].next = 0;
  } else {
    // Occupant is at its main position: chain the new key after it.
    nodes[f].key = canon;
    nodes[f].value = value;
    nodes[f].next = nodes[mi].next;
    nodes[mi].next = f + 1;
  }
  t->hash_count += 1;
  return true;
}

void HashResize(GcHeap &gc, TableObj *t, uint32_t new_count) {
  TableNodes *nn = gc.AllocTableNodes(new_count);
  TableNodes *old = t->nodes;
  uint32_t old_count = t->node_count;
  t->nodes = nn;
  t->node_count = new_count;
  t->free_hint = new_count;
  t->hash_count = 0;
  if (old != nullptr) {
    for (uint32_t i = 0; i < old_count; ++i) {
      if (old->items[i].key.type != UnitType::Invalid) {
        // new_count is large enough, so this always succeeds.
        HashInsertNew(t, old->items[i].key, old->items[i].value);
      }
    }
  }
}

// Insert or overwrite; returns true when the key was new.
bool HashPut(GcHeap &gc, TableObj *t, const Unit &canon, const Unit &value) {
  if (t->nodes == nullptr) {
    HashResize(gc, t, 8);
  }
  uint32_t idx;
  if (HashFind(t, canon, idx)) {
    t->nodes->items[idx].value = value;
    return false;
  }
  if ((t->hash_count + 1) * 4 > t->node_count * 3) {
    HashResize(gc, t, t->node_count * 2);
  }
  if (!HashInsertNew(t, canon, value)) {
    HashResize(gc, t, t->node_count * 2);
    if (!HashInsertNew(t, canon, value)) {
      throw MachineException("table: internal insert failure");
    }
  }
  return true;
}

bool HashErase(TableObj *t, const Unit &canon) {
  uint32_t idx;
  if (!HashFind(t, canon, idx)) {
    return false;
  }
  TableNode *nodes = t->nodes->items;
  uint32_t mi = MainPos(t, canon);
  if (idx == mi) {
    uint32_t nx = nodes[idx].next;
    if (nx == 0) {
      nodes[idx].key = Unit();
      nodes[idx].value = Unit();
      nodes[idx].next = 0;
    } else {
      // Move the successor into the main-position slot and free its old slot.
      uint32_t q = nx - 1;
      nodes[idx].key = nodes[q].key;
      nodes[idx].value = nodes[q].value;
      nodes[idx].next = nodes[q].next;
      nodes[q].key = Unit();
      nodes[q].value = Unit();
      nodes[q].next = 0;
    }
  } else {
    uint32_t p = mi;
    while (nodes[p].next != idx + 1) {
      p = nodes[p].next - 1;
    }
    nodes[p].next = nodes[idx].next;
    nodes[idx].key = Unit();
    nodes[idx].value = Unit();
    nodes[idx].next = 0;
  }
  t->hash_count -= 1;
  return true;
}

bool ScanNodes(const TableObj *t, uint32_t start, Unit &out_key, Unit &out_value) {
  if (t->nodes == nullptr) {
    return false;
  }
  for (uint32_t i = start; i < t->node_count; ++i) {
    if (t->nodes->items[i].key.type != UnitType::Invalid) {
      out_key = t->nodes->items[i].key;
      out_value = t->nodes->items[i].value;
      return true;
    }
  }
  return false;
}

}  // namespace

namespace table {

bool ValidKey(const Unit &key) {
  if (key.type == UnitType::Nil || key.type == UnitType::Invalid) {
    return false;
  }
  if (key.type == UnitType::FP && std::isnan(key.value.fp)) {
    return false;
  }
  return true;
}

bool Get(const TableObj *t, const Unit &key, Unit &out) {
  Unit ck = CanonKey(key);
  uint64_t ai;
  if (CanonIndex(ck, ai) && ai < t->array_size) {
    const Unit &slot = t->array->items[ai];
    if (slot.type == UnitType::Invalid) {
      return false;
    }
    out = slot;
    return true;
  }
  uint32_t idx;
  if (HashFind(t, ck, idx)) {
    out = t->nodes->items[idx].value;
    return true;
  }
  return false;
}

bool Has(const TableObj *t, const Unit &key) {
  Unit ignored;
  return Get(t, key, ignored);
}

void Put(GcHeap &gc, TableObj *t, const Unit &key, const Unit &value) {
  Unit ck = CanonKey(key);
  bool del = (value.type == UnitType::Nil || value.type == UnitType::Invalid);
  uint64_t ai;
  if (CanonIndex(ck, ai)) {
    if (ai < t->array_size) {
      Unit &slot = t->array->items[ai];
      if (del) {
        if (slot.type != UnitType::Invalid) {
          slot = Unit();
          t->size -= 1;
        }
      } else {
        if (slot.type == UnitType::Invalid) {
          t->size += 1;
        }
        slot = value;
      }
      return;
    }
    if (ai == t->array_size) {
      if (del) {
        if (HashErase(t, ck)) {
          t->size -= 1;
        }
        return;
      }
      bool existed = HashErase(t, ck);  // migrate an out-of-order insert
      EnsureArrayCap(gc, t, t->array_size + 1);
      t->array->items[t->array_size] = value;
      t->array_size += 1;
      if (!existed) {
        t->size += 1;
      }
      return;
    }
  }
  if (del) {
    if (HashErase(t, ck)) {
      t->size -= 1;
    }
    return;
  }
  if (HashPut(gc, t, ck, value)) {
    t->size += 1;
  }
}

bool Erase(TableObj *t, const Unit &key) {
  Unit ck = CanonKey(key);
  uint64_t ai;
  if (CanonIndex(ck, ai) && ai < t->array_size) {
    Unit &slot = t->array->items[ai];
    if (slot.type == UnitType::Invalid) {
      return false;
    }
    slot = Unit();
    t->size -= 1;
    return true;
  }
  if (HashErase(t, ck)) {
    t->size -= 1;
    return true;
  }
  return false;
}

uint32_t Size(const TableObj *t) { return t->size; }

TableObj *Copy(GcHeap &gc, const TableObj *src) {
  TableObj *dst = gc.AllocTable();
  Unit k, v;
  Unit cur = Unit::MakeNil();
  while (Next(src, cur, k, v)) {
    Put(gc, dst, k, v);
    cur = k;
  }
  return dst;
}

bool Next(const TableObj *t, const Unit &key, Unit &out_key, Unit &out_value) {
  if (key.type == UnitType::Nil) {  // start
    for (uint32_t i = 0; i < t->array_size; ++i) {
      if (t->array->items[i].type != UnitType::Invalid) {
        out_key = Unit::MakeInt(static_cast<int64_t>(i));
        out_value = t->array->items[i];
        return true;
      }
    }
    return ScanNodes(t, 0, out_key, out_value);
  }
  Unit ck = CanonKey(key);
  uint64_t ai;
  if (CanonIndex(ck, ai) && ai < t->array_size) {
    for (uint32_t i = static_cast<uint32_t>(ai) + 1; i < t->array_size; ++i) {
      if (t->array->items[i].type != UnitType::Invalid) {
        out_key = Unit::MakeInt(static_cast<int64_t>(i));
        out_value = t->array->items[i];
        return true;
      }
    }
    return ScanNodes(t, 0, out_key, out_value);
  }
  uint32_t idx;
  if (!HashFind(t, ck, idx)) {
    throw MachineException("tblnext: invalid key");
  }
  return ScanNodes(t, idx + 1, out_key, out_value);
}

}  // namespace table
