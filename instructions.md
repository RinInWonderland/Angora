### Instruction Set Architecture Reference

#### Instruction Encoding & Immediate Widths

Every instruction is one 32-bit word: a 7-bit opcode (bits 0-6) and a 25-bit
argument (bits 7-31), i.e. `arg = word >> 7`.

```
  31                                  7 6         0
 +------------------------------------+-----------+
 |          argument  (25 bits)       | opcode (7)|
 +------------------------------------+-----------+
      arg = word >> 7          opcode = word & 0x7F
```

Several instructions sub-decode the 25-bit argument, so the usable immediate
width differs. Bit indices below are within the argument field:

- **Folded ops** (arithmetic, comparison, shift, rotate, bitwise) reuse the low
  2 bits as a variant; the assembler derives it from the mnemonic suffix
  (`addu`/`addf`, `ult`/`ltf`, `asr`, `xor`, ...), so assembly source is
  unchanged.

```
    24                                 2 1 0
   +-----------------------------------+-----+
   |            0 / unused             | var |
   +-----------------------------------+-----+
     var (arith/cmp): 0=int  1=uint  2=float
     var (bitwise):   0=and  1=or    2=xor
     var (shift):     0=sl   1=lsr   2=asr
     var (rotate):    0=rl   1=rr
```

- **Folded immediate shift / rotate** (`ShiftImm` / `RotateImm`) pack the count
  above the variant: `arg = (count << 2) | variant`.

```
    24                                 2 1 0
   +-----------------------------------+-----+
   |            count  (23 bits)       | var |
   +-----------------------------------+-----+
     count max = 0x7FFFFF
```

- **Half-word immediates**: `pushhwi` / `pushhwisl16` carry a **16-bit**
  half-word. The `pushimm` / `pushuimm` / `pushfp` macros split a wider literal
  into half-words and recombine them (`addu` / `sli` / `spawnsint` / `spawnfp`).

```
    24       16 15                     0
   +-----------+------------------------+
   | 0 (pad)   |      half-word (16)    |
   +-----------+------------------------+
```

- **Code offsets**: `jmp` / `branch` / `call` / `closnew` carry a **25-bit**
  target (maximum `0x1FFFFFF`). Larger targets are reached with `farjmp` /
  `farbranch`, which take the address from the stack; `call` / `closnew` have no
  far form and reject offsets beyond 25 bits.

```
   +------------------------------------+
   |            target  (25 bits)       |
   +------------------------------------+
```

- **Plain immediates** (`claim`, `ret`, `dupn`, `alloci`, `closget`, and the
  `ld` / `st` / `ldr` / `str` offsets, plus the `ldc` pool index) use the full
  **25-bit** field.

```
   +------------------------------------+
   |            value   (25 bits)       |
   +------------------------------------+
```

#### Arithmetic Instructions

`Add`, `Sub`, `Mul`, `Div` and `Mod` are **folded**: a 2-bit variant selects the
operand type — `0` = signed `int64_t`, `1` = unsigned `uint64_t`, `2` =
`double` — chosen by the assembler from the mnemonic. Operands are
`stack[top-1]` (X) and `stack[top]` (Y); the result replaces both.

| Instruction | Mnemonics              | Description                                                       |
| :---------- | :--------------------- | :---------------------------------------------------------------- |
| `Add`       | `add` / `addu` / `addf` | `X + Y` (signed / unsigned / float)                               |
| `Sub`       | `sub` / `subu` / `subf` | `X - Y`                                                           |
| `Mul`       | `mul` / `mulu` / `mulf` | `X * Y`                                                           |
| `Div`       | `div` / `divu` / `divf` | `X / Y`                                                           |
| `Mod`       | `mod` / `modu` / `modf` | `X % Y` for int/uint; `std::fmod(X, Y)` for float                 |

The `*u` form reads both operands as `uint64_t`, the `*f` form as `double`; the
plain form is signed `int64_t`.

```
  argument (25 bits):     +---------------------------------+-----+
                          |               0                 | var |
                          +---------------------------------+-----+
                                                            2 1 0
  var = 0 signed (add)   1 unsigned (addu)   2 float (addf)
```

#### Comparison Instructions

Folded like arithmetic: the variant selects signed (`0`), unsigned (`1`) or
floating-point (`2`). Compares `stack[top-1]` (X) against `stack[top]` (Y) and
replaces both with a `UInt` `1` if true, else `0`.

| Instruction | Mnemonics              | Description        |
| :---------- | :--------------------- | :----------------- |
| `Lt`        | `lt` / `ult` / `ltf`   | `1` if `X < Y`.    |
| `Le`        | `le` / `ule` / `lef`   | `1` if `X <= Y`.   |
| `Gt`        | `gt` / `ugt` / `gtf`   | `1` if `X > Y`.    |
| `Ge`        | `ge` / `uge` / `gef`   | `1` if `X >= Y`.   |

The floating-point forms (`*f`) are new with the folded encoding.

Same argument field as arithmetic (the 2-bit `var` selects signed / unsigned /
float); only the result differs (a `UInt` `1`/`0`).

#### nil, Truthiness & Numeric Guards

`nil` is a distinct scalar value with no payload: `pushnil` pushes it, `isnil`
tests it by tag and pushes a `UInt` `1`/`0`, and `print`/`repr` render it as
`nil`. The data section represents it as `.nil <id>` with **no value token**.

Truthiness (the condition of `branch`/`farbranch`): **nil and a numeric zero**
(`Int`/`UInt` 0, `FP` 0.0) are false; every other value — including an empty
string or array — is true. Testing nil uses its tag, never its payload.

To stop nil from being silently read as `0`, every instruction that reads a
numeric payload first requires its operand(s) to be `Int`, `UInt`, or `FP`;
otherwise it raises a runtime error. This covers arithmetic, comparison, bitwise
shifts/rotates, `not`/`eqz`, the counts/indices of `alloc`/`closnew`/`arrget`/
`arrput`/`strsetchar`/`memcpy`/`ldr`/`str`, and the `farjmp`/`farbranch`
address. Building with `-DANGORA_NO_GUARDS` compiles the checks out (trusted
bytecode only), where such operand misuse is undefined.

#### Stack & Construction Instructions

| Instruction           | Mnemonic      | Description                                                                         |
| :-------------------- | :------------ | :---------------------------------------------------------------------------------- |
| `PushHalfWordImm`     | `pushhwi`     | Push a half-word immediate value onto the stack.                                    |
| `PushHalfWordImmSL16` | `pushhwisl16` | Push a half-word immediate value onto the stack, shifted left by 16 bits.           |
| `SpawnFP`             | `spawnfp`     | Change the type of the current stack top to Floating-point.                         |
| `SpawnSignedInt`      | `spawnsint`   | Change the type of the current stack top to Integer.                                |
| `PushNil`             | `pushnil`     | Push `nil`.                                                                          |
| `IsNil`               | `isnil`       | Replace the top with `UInt` `1` if it is `nil`, else `0` (by tag; no payload read).  |
| `Pop`                 | `pop`         | Remove the top element from the stack.                                              |
| `Dup`                 | `dup`         | Duplicate the top element of the stack.                                             |
| `DupN`                | `dupn`        | Duplicate the top element of the stack $N$ times ($N$ is the instruction argument). |
| `Swap`                | `swap`        | Swap the top two elements of the stack.                                             |

#### Control Flow Instructions

| Instruction | Mnemonic    | Description                                                                                        |
| :---------- | :---------- | :------------------------------------------------------------------------------------------------- |
| `Jump`      | `jmp`       | Jump to the address provided as an instruction argument.                                           |
| `Branch`    | `branch`    | Jump to the address provided as an argument if the top of the stack is non-zero.                   |
| `FarJump`   | `farjmp`    | Pop the destination address from the stack and jump to it.                                         |
| `FarBranch` | `farbranch` | Pop a condition from the stack, then pop a destination address; jump if the condition is non-zero. |
| `Call`      | `call`      | Push a call frame (`pc+1`, the current `stack_`/`mem_` watermarks) and jump to the 25-bit function-entry argument. |
| `ClaimArgs` | `claim`     | Lower the current call frame's `stack_` watermark by the immediate `argc`, so return values land where the arguments were. |
| `Ret`       | `ret`       | Keep the top `n` values (immediate, default 0), roll `stack_` and `mem_` back to the current frame's watermarks, then pop the call frame into `pc`. |
| `Exit`      | `exit`      | Halt execution.                                                                                    |

In assembly, every `jmp`/`branch`/`farjmp`/`farbranch`/`call` target is a label
reference written as `@` followed by the label name, e.g. `jmp @loop`,
`call @factorial`. Labels themselves are defined as `<name>:` or via
`.func <name>`, without the prefix.

```
  jmp / branch / call / closnew:   +------------------------------------+
                                   |            target  (25 bits)       |
                                   +------------------------------------+
```

`farjmp` / `farbranch` instead pop the address (and `farbranch`, the condition)
from the value stack, so they can reach any address while the immediate forms
are limited to `0x1FFFFFF`.

#### Closures

Closures are first-class, GC-managed function values: a code entry plus a set of
captured values. They are captured **by value** — each captured value is a
shallow alias copy of the `Unit` on the stack at creation time, so a `Unit` that
shares a heap cell (an array or a closure) can still be used as shared mutable
state. `callc` enters a closure with the same calling convention as `call`:
explicit arguments stay on the value stack, `claim <argc>` lowers the frame
base, and `ret <n>` leaves the return values there.

| Instruction  | Mnemonic  | Description |
| :----------- | :-------- | :---------- |
| `ClosureNew` | `closnew` | Pop a capture count `N`, then `N` values (the first pushed becomes upval 0); push a new closure whose entry is the 25-bit label argument. |
| `CallClosure`| `callc`   | Pop a closure, push a call frame, and jump to its entry. The explicit arguments remain below the closure. |
| `ClosureGet` | `closget` | Push captured value `i` of the closure currently executing (errors outside a closure or when `i` is out of range). |

In assembly, `closnew` takes a label reference like `call`, e.g.
`closnew @adder`.

```
  closnew:   +------------------------------------+   (entry pc)
             |            target  (25 bits)       |
             +------------------------------------+
```

#### Output Instructions

`print` keeps human-friendly output: a top-level string is emitted raw, but
strings nested inside an array are quoted and escaped. `repr` is the
unambiguous debug form: every string is quoted/escaped and arrays are bounded by
depth (6) and element count (32), with `...` marking the elided tail. Both print
a `FP` in the shortest decimal form that reads back to the same `double`
(`inf`/`nan` named explicitly), and a closure as `<closure@<pc> nupvals=N>`.

| Instruction     | Mnemonic    | Description                                                                 |
| :-------------- | :---------- | :-------------------------------------------------------------------------- |
| `Print`         | `print`     | Print the top element (any value type); nested strings are quoted.          |
| `Repr`          | `repr`      | Debug form: strings always quoted, arrays bounded by depth/length.          |

#### Bitwise & Shift Instructions

Shift and rotate are folded by **direction**; bitwise binary ops are folded by
**operation**. For `ShiftImm` / `RotateImm` the count is the 23-bit immediate
packed above the variant bits, so the count is limited to `0x7FFFFF`.

```
  register form:        +---------------------------------+-----+
                        |               0                 | var |
                        +---------------------------------+-----+
  immediate form:       +---------------------------------+-----+
                        |          count (23 bits)        | var |
                        +---------------------------------+-----+
                                                          2 1 0
  var: shift 0 sl 1 lsr 2 asr   |   rotate 0 rl 1 rr   |   bitop 0 and 1 or 2 xor
```

| Instruction  | Mnemonics            | Description                                                                 |
| :----------- | :------------------- | :-------------------------------------------------------------------------- |
| `Shift`      | `sl` / `lsr` / `asr` | `stack[top-1] <<= stack[top]` (left); `>>=` unsigned (logic); `>>=` signed (arithmetic). Pops the count. |
| `ShiftImm`   | `sli` / `lsri` / `asri` | Same, but the count is the 23-bit immediate.                             |
| `Rotate`     | `rl` / `rr`          | `std::rotl` / `std::rotr(stack[top-1], stack[top])` (unsigned). Pops the count. |
| `RotateImm`  | `rli` / `rri`        | Same, but the count is the 23-bit immediate.                               |
| `BitOp`      | `and` / `or` / `xor` | `X & Y` / `X \| Y` / `X ^ Y` (unsigned), with X = `stack[top-1]`, Y = `stack[top]`. |
| `Not`        | `not`                | Bitwise NOT: `stack[top] = ~stack[top]` (unsigned).                        |
| `EqualsZero` | `eqz`                | `stack[top] = (stack[top] == 0)` (unsigned, boolean).                      |

The value's `UnitType` is left untouched by shifts (as before); `Rotate` and
`BitOp` set it to `UInt`.

#### Memory & String Instructions

| Instruction     | Mnemonic     | Description                                                       |
| :-------------- | :----------- | :---------------------------------------------------------------- |
| `Load`          | `ld`         | Load value from memory at address `arg` onto the stack.           |
| `Store`         | `st`         | Store top of stack to memory at address `arg`.                    |
| `RelativeLoad`  | `ldr`        | Push `mem_[stack[top] + arg]`; the base index stays on the stack. |
| `RelativeStore` | `str`        | Store `stack[top]` into `mem_[stack[top-1] + arg]`, then pop the stored value; the base index stays on the stack. |
| `StringLen`     | `strlen`     | Replace the string at `stack[top]` with its length as a `UInt`.    |
| `StringChar`    | `strchar`    | Get character at `index` from `string` (both popped from stack).  |
| `StringConcat`   | `strconcat`   | Concatenate two strings from the stack.                            |
| `StringSetChar` | `strsetchar` | Set character at `index` (popped) in the `string` (at stack top). |
| `StringCopy`      | `strcpy`       | Copy the string at `stack[top]` into a freshly allocated buffer, wrap it in a new `Unit`, and push it; the original string stays on the stack. |
| `MemCopy`          | `memcpy`        | Copy `N` words from `mem_[src..src+N)` to `mem_[dst..dst+N)`; operands `N`, `src`, `dst` are pushed onto the stack in that order with `N` on top. Both regions must be pre-allocated; overlapping ranges copy like `memmove`. |
| `LoadConst`        | `ldc`           | Push the read-only constant-pool entry `arg` (a 25-bit index), preserving the entry's type (`Int`/`UInt`/`FP`/`Str`). One instruction loads any width, including 64-bit literals. |

In assembly, `ld`/`st`/`ldr`/`str` address the data section through a symbol
reference written as `&` followed by the symbol name, e.g. `ld &foo`,
`str &arr`. `ldr`/`str` still accept a plain integer offset (e.g. `ldr 0`) for
addressing relative to a base index held on the stack.

`ldc` reads from a separate **constant pool**: a read-only, GC-rooted table of
scalar values, deduplicated by `(type, value)` and serialized as its own `.bc`
section. The value macros `pushimm` / `pushuimm` / `pushfp` compile to a single
`ldc` each, so wide immediates no longer expand into a `pushhwi`/`add`/`sli`
sequence. `pushhwi` / `pushhwisl16` remain as raw primitives (used internally
for far-address materialization).

```
  ldc:   +------------------------------------+
         |          pool index  (25 bits)     |
         +------------------------------------+
```

#### Array Instructions

Arrays are first-class, GC-managed, growable sequences and are **reference
types**: every copy (via `dup`/`ld`) aliases the same array. Indices are 0-based.
Notation below is `before -> after` with the stack top on the right.

| Instruction | Mnemonic  | Description                                                                 |
| :---------- | :-------- | :-------------------------------------------------------------------------- |
| `ArrNew`    | `arrnew`  | Push a new empty array: `-> arr`.                                           |
| `ArrLen`    | `arrlen`  | Push the element count, keeping the array: `arr -> arr len`.                |
| `ArrGet`    | `arrget`  | Read `arr[idx]`: `arr idx -> arr elem`; errors if `idx` is out of range.    |
| `ArrPut`    | `arrput`  | Write `arr[idx] = val` (bounded): `arr idx val -> arr idx`; errors if `idx` is out of range. Use `arrpush` to grow. |
| `ArrPush`   | `arrpush` | Append `val`, growing as needed: `arr val -> arr`.                          |
| `ArrPop`    | `arrpop`  | Remove and push the last element: `arr -> arr elem`; errors if empty.       |
| `ArrCopy`   | `arrcpy`  | Shallow copy: `arr -> arr arr`; the original stays, a new array is pushed (elements are shared aliases). |

#### Table Instructions

Tables are first-class, GC-managed **key/value maps** (reference types). Keys may
be `Int`/`UInt`/`FP`/`Str` or a heap object (identity); `Nil`, `Invalid` and
`NaN` are rejected. `Int` and a small `UInt` with the same value are the same
key; an `FP` key is a distinct type. There is **no border / `#` semantics**:
`tbllen` is the number of live entries. Assigning `nil` deletes a key (nil is
never stored). Dense non-negative integer keys are stored in an array part;
other keys live in a hash part. Notation is `before -> after`, top on the right.

| Instruction | Mnemonic  | Description |
| :---------- | :-------- | :---------- |
| `TableNew`  | `tblnew`  | Push a new empty table: `-> tbl`. |
| `TableLen`  | `tbllen`  | Push the entry count (UInt), keeping the table: `tbl -> tbl len`. |
| `TableGet`  | `tblget`  | Read `tbl[key]`: `tbl key -> tbl val`; a missing key yields `nil`. |
| `TablePut`  | `tblput`  | Write: `tbl key val -> tbl key`; a `nil` value deletes the key. |
| `TableNext` | `tblnext` | Iterate: `tbl key -> tbl key val`; pass `nil` to start, a `nil` returned key marks the end. |
| `TableHas`  | `tblhas`  | Test a key: `tbl key -> tbl found` (`UInt` 1/0). |
| `TableErase`| `tblerase`| Delete a key: `tbl key -> tbl`. |
| `TableCopy` | `tblcpy`  | Shallow copy: `tbl -> tbl tbl` (values are shared aliases). |
| `TableGetInt` | `tblgeti` | Integer-key fast path: `tbl idx -> tbl val`. |
| `TablePutInt` | `tblputi` | Integer-key fast path: `tbl idx val -> tbl idx`. |

`tblnext` visits the array part in index order first, then the hash nodes; the
order is stable while the table is not modified (adding a key may rehash and
invalidates an in-progress traversal).

#### Dynamic Memory Allocation (monotonic bump arena)

| Instruction | Mnemonic | Description                                                                                                                                                                                      |
| :---------- | :------- | :----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `Alloc`     | `alloc`  | Bump-allocate words over the growable data segment and push the start index onto the stack. The word count is popped from the stack top.                                                          |
| `AllocImm`  | `alloci` | Same as `alloc`, but the word count is the instruction's immediate argument.                                                                                                                     |

There is no `free`: reclamation of unreachable objects is entirely the tracing collector's job (see `gcstat` below), so the arena only ever bumps forward.

#### Garbage Collection

| Instruction | Mnemonic | Description |
| :---------- | :------- | :---------- |
| `GcStat`    | `gcstat` | Push two `UInt`: the total number of GC objects allocated so far, then the number still reachable at the most recent sweep (on top). Lets a program observe that the collector reclaims (alloc keeps climbing while live stays small). The collector runs automatically at instruction-boundary safe points once enough fresh garbage has accumulated since the last cycle. |

#### Other

| Instruction | Mnemonic | Description   |
| :---------- | :------- | :------------ |
| `Nop`       | `nop`    | No operation. |
| `Rand`      | `rand`   | Push one 64-bit `UInt` drawn from the host entropy source (`std::random_device`). Nondeterministic: the value differs every run, so program output is not reproducible. |
