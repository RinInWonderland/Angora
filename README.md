# Angora

A virtual machine that is designed to be new runtime of a under-construction programming language.

## Requirement

Any macOS/Linux environment and C++ compiler with C++20 support. Windows is not verified for now.

## Instructon design & syntax

Read instructions.md for more details.

## Build & Test

### Makefile usage

```sh
make interp    # bin/angora-interp (default target)
make vm        # bin/angora-vm
make check-bc  # golden-byte .bc format test
make check     # build both engines and run the regression suite
make clean     # remove generated *.o files
make distclean # also remove bin/
```

Targets rebuild unconditionally; `make` alone builds `interp`.

### Regression tests

`make check` runs every `test/*.asrc` through both the assembler and the
compiled bytecode VM. Expected stdout and exit code are declared inline with
`// expect:` / `// expect-exit:` comments; files with neither marker only need
to exit 0.

## LLM disclosure

Base tech & framework design and coding are done by human. LLM agent writes the detail implementation of some module (GC, use of existing framework), and reviewed by human.

## License

MIT. Read LICENSE file for more detail.
