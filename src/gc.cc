#include <utility>

#include "machine.h"

// Out-of-line GcHeap implementation. The class lives in machine.h; these bodies
// are large (allocation, mark, sweep) so they are kept out of the header.

GcHeap::GcHeap(GcHeap &&o) noexcept
    : chunks_(std::move(o.chunks_)),
      free_(o.free_),
      large_free_(o.large_free_),
      mark_stack_(std::move(o.mark_stack_)),
      live_(o.live_),
      alloc_count_(o.alloc_count_),
      bytes_since_(o.bytes_since_) {
  o.free_.fill(nullptr);
  o.large_free_ = nullptr;
}

GcHeap &GcHeap::operator=(GcHeap &&o) noexcept {
  if (this != &o) {
    FreeChunks();
    chunks_ = std::move(o.chunks_);
    free_ = o.free_;
    large_free_ = o.large_free_;
    mark_stack_ = std::move(o.mark_stack_);
    live_ = o.live_;
    alloc_count_ = o.alloc_count_;
    bytes_since_ = o.bytes_since_;
    o.free_.fill(nullptr);
    o.large_free_ = nullptr;
  }
  return *this;
}

GcHeap::~GcHeap() { FreeChunks(); }

GcHeader *GcHeap::AllocBytes(size_t raw, GcKind kind) {
  size_t obj_size = (raw + alignof(GcHeader) - 1) & ~(alignof(GcHeader) - 1);
  GcHeader *o = PopFree(obj_size);
  if (o == nullptr) {
    if (chunks_.empty() || chunks_.back().used + obj_size > chunks_.back().cap) {
      GrowChunk(obj_size);
    }
    GcChunk &c = chunks_.back();
    o = reinterpret_cast<GcHeader *>(c.base + c.used);
    c.used += obj_size;
    o->gc_size_ = obj_size;  // fixed for the lifetime of the slot
  }
  o->gc_flags_ = GcFlagsFor(kind);  // mark cleared, kind stamped
  o->gc_aux_ = 0;
  ++alloc_count_;
  bytes_since_ += obj_size;
  return o;
}

char *GcHeap::Alloc(const char *src, size_t len) {
  StrObj *o =
      reinterpret_cast<StrObj *>(AllocBytes(offsetof(StrObj, gc_chars_) + len + 1, GcKind::Str));
  o->h.gc_aux_ = static_cast<uint32_t>(len);
  char *data = &o->gc_chars_[0];
  memcpy(data, src, len);
  data[len] = '\0';
  return data;
}

char *GcHeap::AllocRaw(size_t len) {
  StrObj *o =
      reinterpret_cast<StrObj *>(AllocBytes(offsetof(StrObj, gc_chars_) + len + 1, GcKind::Str));
  o->h.gc_aux_ = static_cast<uint32_t>(len);
  return &o->gc_chars_[0];
}

ArrData *GcHeap::AllocArrData(uint32_t capacity) {
  size_t raw = offsetof(ArrData, items) + static_cast<size_t>(capacity) * sizeof(Unit);
  ArrData *d = reinterpret_cast<ArrData *>(AllocBytes(raw, GcKind::ArrData));
  d->h.gc_aux_ = capacity;  // exact element count (a reused slot may be larger)
  for (uint32_t i = 0; i < capacity; ++i) {
    d->items[i] = Unit();  // Invalid hole, never traced
  }
  return d;
}

ArrObj *GcHeap::AllocArr(uint32_t capacity) {
  ArrObj *a = reinterpret_cast<ArrObj *>(AllocBytes(sizeof(ArrObj), GcKind::Arr));
  a->length = 0;
  a->capacity = capacity;
  a->data = capacity > 0 ? AllocArrData(capacity) : nullptr;
  return a;
}

void GcHeap::ArrReserve(ArrObj *a, uint32_t need) {
  if (need <= a->capacity) {
    return;
  }
  uint32_t cap = a->capacity == 0 ? 4u : a->capacity;
  while (cap < need) {
    cap *= 2u;
  }
  ArrData *nd = AllocArrData(cap);
  for (uint32_t i = 0; i < a->length; ++i) {
    nd->items[i] = a->data->items[i];  // shallow alias; strings/arrays stay GC-owned
  }
  a->data = nd;
  a->capacity = cap;
}

ClosureObj *GcHeap::AllocClosure(uint32_t entry, uint32_t nupvals) {
  size_t raw = offsetof(ClosureObj, upvals) + static_cast<size_t>(nupvals) * sizeof(Unit);
  ClosureObj *c = reinterpret_cast<ClosureObj *>(AllocBytes(raw, GcKind::Closure));
  c->h.gc_aux_ = nupvals;  // exact capture count (a reused slot may be larger)
  c->entry_pc = entry;
  for (uint32_t i = 0; i < nupvals; ++i) {
    c->upvals[i] = Unit();  // Invalid hole, never traced
  }
  return c;
}

TableObj *GcHeap::AllocTable() {
  TableObj *t = reinterpret_cast<TableObj *>(AllocBytes(sizeof(TableObj), GcKind::Table));
  t->array = nullptr;
  t->array_cap = 0;
  t->array_size = 0;
  t->nodes = nullptr;
  t->node_count = 0;
  t->hash_count = 0;
  t->free_hint = 0;
  t->size = 0;
  return t;
}

TableNodes *GcHeap::AllocTableNodes(uint32_t count) {
  size_t raw = offsetof(TableNodes, items) + static_cast<size_t>(count) * sizeof(TableNode);
  TableNodes *n = reinterpret_cast<TableNodes *>(AllocBytes(raw, GcKind::TableNodes));
  n->h.gc_aux_ = count;
  for (uint32_t i = 0; i < count; ++i) {
    n->items[i].key = Unit();  // Invalid => free node
    n->items[i].value = Unit();
    n->items[i].next = 0;
  }
  return n;
}

// Trace from the three root Unit sets (value stack, data segment, constant
// pool) and every in-flight closure on the call stack. A copy of a
// heap-bearing Unit is a shallow alias, so roots are marked directly; arrays and
// closures are then walked through their payload. An explicit worklist
// (mark-before-push) handles nesting and cycles.
void GcHeap::Mark(const vector<Unit> &a, const vector<Unit> &b, const vector<Unit> &c,
                  const vector<CallFrame> &frames) {
  mark_stack_.clear();

  auto push_obj = [&](GcHeader *o) {
    if (!GcMarked(o)) {
      GcMarkObj(o);
      mark_stack_.push_back(o);
    }
  };
  auto push_unit = [&](const Unit &u) {
    if (u.type == UnitType::Str && u.value.str != nullptr) {
      push_obj(reinterpret_cast<GcHeader *>(GcObjOf(u.value.str)));
    } else if (u.type == UnitType::Arr && u.value.arr != nullptr) {
      push_obj(reinterpret_cast<GcHeader *>(u.value.arr));
    } else if (u.type == UnitType::Closure && u.value.closure != nullptr) {
      push_obj(reinterpret_cast<GcHeader *>(u.value.closure));
    } else if (u.type == UnitType::Table && u.value.table != nullptr) {
      push_obj(reinterpret_cast<GcHeader *>(u.value.table));
    }
  };

  for (const Unit &u : a) {
    push_unit(u);
  }
  for (const Unit &u : b) {
    push_unit(u);
  }
  for (const Unit &u : c) {
    push_unit(u);
  }
  // Every in-flight closure on the call stack is a live root: `callc` removes
  // the closure from the value stack, so the frame is the only reference.
  for (const CallFrame &f : frames) {
    if (f.closure != nullptr) {
      push_obj(reinterpret_cast<GcHeader *>(f.closure));
    }
  }

  while (!mark_stack_.empty()) {
    GcHeader *o = mark_stack_.back();
    mark_stack_.pop_back();
    switch (GcKindOf(o)) {
      case GcKind::Arr: {
        ArrObj *arr = reinterpret_cast<ArrObj *>(o);
        if (arr->data != nullptr) {
          push_obj(reinterpret_cast<GcHeader *>(arr->data));
        }
        break;
      }
      case GcKind::ArrData: {
        ArrData *d = reinterpret_cast<ArrData *>(o);
        size_t n = GcArrDataLen(d);
        for (size_t i = 0; i < n; ++i) {
          push_unit(d->items[i]);
        }
        break;
      }
      case GcKind::Closure: {
        ClosureObj *c = reinterpret_cast<ClosureObj *>(o);
        size_t n = GcClosureNupvals(c);
        for (size_t i = 0; i < n; ++i) {
          push_unit(c->upvals[i]);
        }
        break;
      }
      case GcKind::Table: {
        TableObj *t = reinterpret_cast<TableObj *>(o);
        if (t->array != nullptr) {
          push_obj(reinterpret_cast<GcHeader *>(t->array));
        }
        if (t->nodes != nullptr) {
          push_obj(reinterpret_cast<GcHeader *>(t->nodes));
        }
        break;
      }
      case GcKind::TableNodes: {
        TableNodes *n = reinterpret_cast<TableNodes *>(o);
        size_t count = GcTableNodeCount(n);
        for (size_t i = 0; i < count; ++i) {
          push_unit(n->items[i].key);    // Invalid free nodes are ignored
          push_unit(n->items[i].value);
        }
        break;
      }
      case GcKind::Str:
      default:
        break;
    }
  }
}

// Non-compacting sweep over every chunk: chain unreachable objects into their
// size-classed free list so a later `Alloc` reuses the slot, then clear marks for
// the next cycle. No object or its payload is moved.
void GcHeap::Sweep() {
  live_ = 0;
  free_.fill(nullptr);
  large_free_ = nullptr;
  for (GcChunk &c : chunks_) {
    for (char *p = c.base; p < c.base + c.used;) {
      GcHeader *o = reinterpret_cast<GcHeader *>(p);
      p += o->gc_size_;
      if (GcMarked(o)) {
        ++live_;
        o->gc_flags_ &= ~kGcMarkBit;
      } else {
        PushFree(o);
      }
    }
  }
}

// Mark-sweep one full collection: mark all reachable objects, then sweep. Clears
// the allocation counter so the next collection is triggered on fresh garbage.
void GcHeap::Collect(const vector<Unit> &a, const vector<Unit> &b, const vector<Unit> &c,
                     const vector<CallFrame> &frames) {
  Mark(a, b, c, frames);
  Sweep();
  bytes_since_ = 0;
}

void GcHeap::FreeChunks() {
  for (GcChunk &c : chunks_) {
    operator delete(c.base);
  }
  chunks_.clear();
}

GcHeader *GcHeap::PopFree(size_t obj_size) {
  if (obj_size <= kSmallMax) {
    GcHeader *&head = free_[obj_size / kAlign];
    GcHeader *o = head;
    if (o != nullptr) {
      head = o->gc_next_;
    }
    return o;
  }
  GcHeader **link = &large_free_;
  while (*link != nullptr) {
    if ((*link)->gc_size_ >= obj_size) {
      GcHeader *o = *link;
      *link = o->gc_next_;
      return o;
    }
    link = &(*link)->gc_next_;
  }
  return nullptr;
}

void GcHeap::PushFree(GcHeader *o) {
  if (o->gc_size_ <= kSmallMax) {
    size_t b = o->gc_size_ / kAlign;
    o->gc_next_ = free_[b];
    free_[b] = o;
  } else {
    o->gc_next_ = large_free_;
    large_free_ = o;
  }
}

void GcHeap::GrowChunk(size_t need) {
  size_t cap = chunks_.empty() ? 64u * 1024u : chunks_.back().cap * 2u;
  if (cap < need) {
    cap = need;
  }
  GcChunk c{static_cast<char *>(operator new(cap)), cap, 0u};
  chunks_.push_back(c);
}
