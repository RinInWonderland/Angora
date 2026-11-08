# Variables
CXX := g++
CXXFLAGS := -O2 -std=c++20
BIN_DIR := bin
SRC_DIR := src

# Runtime numeric guards (reject nil/non-numeric operands) are on by default.
# Compile them out for trusted bytecode by adding -DANGORA_NO_GUARDS:
#   make interp CXXFLAGS="-O2 -std=c++20 -DANGORA_NO_GUARDS"

# Default target
.DEFAULT_GOAL := interp

.PHONY: interp vm check-bc check clean distclean

# 1. make interp - Ensure build runs every time via .PHONY
interp:
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(SRC_DIR)/asm-interp.cc $(SRC_DIR)/bytecode-io.cc $(SRC_DIR)/gc.cc $(SRC_DIR)/table.cc $(SRC_DIR)/machine.cc -I$(SRC_DIR) -o $(BIN_DIR)/angora-interp

# 2. make vm - Ensure build runs every time via .PHONY
vm:
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(SRC_DIR)/bytecode-interp.cc $(SRC_DIR)/bytecode-io.cc $(SRC_DIR)/gc.cc $(SRC_DIR)/table.cc $(SRC_DIR)/machine.cc -I$(SRC_DIR) -o $(BIN_DIR)/angora-vm

# 3. make check-bc - Build and run the golden-byte .bc format test.
#    Only the GC + bytecode-I/O layers are needed (no Machine::Run).
check-bc:
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -I$(SRC_DIR) test/bc_format_test.cc $(SRC_DIR)/bytecode-io.cc $(SRC_DIR)/gc.cc -o $(BIN_DIR)/bc-format-test
	$(BIN_DIR)/bc-format-test

# 4. make check - Build both engines and run the per-file regression suite.
#    Expectations are declared inside each test/*.asrc as `// expect:` /
#    `// expect-exit:` lines (see test/regress.sh).
check: interp vm
	./test/regress.sh

# 5. make clean - Remove generated *.o files without deleting bin directory
clean:
	rm -f $(SRC_DIR)/*.o

# 6. make distclean - Remove bin directory on top of make clean
distclean: clean
	rm -rf $(BIN_DIR)
