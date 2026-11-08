#!/bin/sh
#
# Regression runner for test/*.asrc.
#
# A deterministic test declares its expected stdout inline, one line per output
# line, in order:
#
#     // expect: <line>
#
# and optionally its expected process exit code (default 0):
#
#     // expect-exit: <N>
#
# A file with neither marker is a *smoke* test: it must run and exit 0, but its
# stdout is not compared (used for nondeterministic programs such as qsort).
#
# Every file is run twice - through the assembler interpreter and through the
# compiled bytecode VM - and both must satisfy the expectations. A failed
# `-compile` step is itself a test failure (the VM is not run on a stale .bc).
#
# Usage: test/regress.sh   (from anywhere; it locates the repo root itself)

set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT" || exit 2

INTERP=bin/angora-interp
VM=bin/angora-vm
TIMEOUT=20

[ -x "$INTERP" ] || { echo "missing $INTERP (run: make interp)"; exit 2; }
[ -x "$VM" ] || { echo "missing $VM (run: make vm)"; exit 2; }

run() {
  if command -v timeout >/dev/null 2>&1; then
    timeout "$TIMEOUT" "$@"
  else
    "$@"
  fi
}

pass=0
fail=0
smoke=0

for f in test/*.asrc; do
  exp=$(mktemp)
  out_i=$(mktemp)
  out_v=$(mktemp)

  # Expected stdout: drop the marker and one optional separating space.
  sed -nE 's|^// expect: ?||p' "$f" > "$exp"
  # Expected exit code: the last `expect-exit:` wins; default 0.
  want_exit=$(sed -nE 's|^// expect-exit: ?([0-9]+).*|\1|p' "$f" | tail -n 1)
  [ -n "$want_exit" ] || want_exit=0

  marked=0
  [ -s "$exp" ] && marked=1
  grep -qE '^// expect-exit:' "$f" && marked=1

  run "$INTERP" "$f" > "$out_i" 2>/dev/null
  ri=$?
  run "$INTERP" "$f" -compile >/dev/null 2>&1
  rc=$?
  if [ "$rc" -eq 0 ]; then
    run "$VM" "$f.bc" > "$out_v" 2>/dev/null
    rv=$?
  else
    rv=-1  # compilation failed; do not run a possibly stale .bc
  fi

  ok=1
  [ "$rc" -eq 0 ] || ok=0  # a failed -compile is itself a test failure
  [ "$ri" -eq "$want_exit" ] || ok=0
  [ "$rv" -eq "$want_exit" ] || ok=0
  if [ -s "$exp" ]; then
    cmp -s "$exp" "$out_i" || ok=0
    if [ "$rc" -eq 0 ]; then
      cmp -s "$exp" "$out_v" || ok=0
    fi
  fi

  if [ "$ok" -eq 1 ]; then
    if [ "$marked" -eq 1 ]; then
      pass=$((pass + 1))
      echo "ok    $f"
    else
      smoke=$((smoke + 1))
      echo "smoke $f"
    fi
  else
    fail=$((fail + 1))
    echo "FAIL  $f (compile=$rc interp=$ri vm=$rv want_exit=$want_exit)"
    if [ -s "$exp" ]; then
      diff -u "$exp" "$out_i" | sed -n '1,16p'
    fi
  fi

  rm -f "$exp" "$out_i" "$out_v"
done

echo "pass=$pass fail=$fail smoke=$smoke"
[ "$fail" -eq 0 ]
