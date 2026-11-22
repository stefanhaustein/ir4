CC      = clang
CFLAGS  = -std=c89 -Wall -Wextra -O2 -Isrc

BUILD   = build
TARGET  = $(BUILD)/ir4c
SRCS    = src/ir4c.c src/frontend/frontend.c src/frontend/lexer.c src/frontend/ir_emit.c src/codebuf.c src/path.c src/strpool.c src/vec.c \
          src/backend/z80.c src/backend/ez80.c src/backend/6502.c src/backend/arm64.c src/backend/rv32.c src/backend/bc.c \
          src/platform/cpm.c src/platform/zx81.c src/platform/agon.c src/platform/sim65.c src/platform/c64.c src/platform/x16.c \
          src/platform/jit.c src/platform/elf.c src/platform/bc.c
OBJS    = $(patsubst src/%.c,$(BUILD)/%.o,$(SRCS))

AGON_EMU ?= $(HOME)/opt/fab-agon-emulator/fab-agon-emulator

.PHONY: all test test-err test-z80 test-ez80 test-6502 test-rv32 rt rt-z80 rt-ez80 rt-6502 rt-rv32 clean

all: $(TARGET)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c src/ir4.h
	mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/ir4c.o: src/codebuf.h src/frontend.h src/path.h src/backend/bc.h
$(BUILD)/frontend/frontend.o: src/codebuf.h src/frontend.h src/frontend/internal.h src/strpool.h src/vec.h
$(BUILD)/frontend/lexer.o: src/frontend.h src/frontend/internal.h src/path.h src/strpool.h src/vec.h
$(BUILD)/frontend/ir_emit.o: src/codebuf.h src/frontend.h src/frontend/internal.h src/strpool.h src/vec.h
$(BUILD)/codebuf.o: src/codebuf.h src/strpool.h src/vec.h
$(BUILD)/path.o: src/path.h src/strpool.h src/vec.h
$(BUILD)/strpool.o: src/strpool.h src/vec.h
$(BUILD)/vec.o: src/vec.h
$(BUILD)/backend/bc.o: src/backend/bc.h src/codebuf.h src/vec.h
$(BUILD)/backend/z80.o: src/codebuf.h src/backend/z80.h src/backend/z80_math48.h src/vec.h
$(BUILD)/backend/ez80.o: src/codebuf.h src/backend/ez80.h src/backend/ez80_rt.h src/vec.h
$(BUILD)/backend/6502.o: src/codebuf.h src/backend/6502.h src/backend/6502_wozfp.h src/backend/bc.h src/vec.h
$(BUILD)/backend/arm64.o: src/codebuf.h src/vec.h
$(BUILD)/backend/rv32.o: src/codebuf.h src/backend/rv32.h src/backend/rv32_fp.h src/vec.h
$(BUILD)/platform/bc.o: src/backend/bc.h src/codebuf.h src/frontend.h
$(BUILD)/platform/cpm.o: src/codebuf.h src/frontend.h src/backend/z80.h
$(BUILD)/platform/zx81.o: src/codebuf.h src/frontend.h src/backend/z80.h
$(BUILD)/platform/agon.o: src/codebuf.h src/frontend.h src/backend/ez80.h
$(BUILD)/platform/sim65.o: src/codebuf.h src/frontend.h src/backend/6502.h
$(BUILD)/platform/c64.o: src/codebuf.h src/frontend.h src/backend/6502.h
$(BUILD)/platform/x16.o: src/codebuf.h src/frontend.h src/backend/6502.h
$(BUILD)/platform/jit.o: src/codebuf.h src/frontend.h
$(BUILD)/platform/elf.o: src/codebuf.h src/frontend.h src/backend/rv32.h

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $(TARGET) -lm

test: $(TARGET) tests/fib.ir4 tests/core.ir4 tests/muldiv.ir4 tests/mem.ir4 tests/float.ir4
	./$(TARGET) -t z80   -o $(BUILD)/fib_z80.bin     tests/fib.ir4
	./$(TARGET) -t 6502  -o $(BUILD)/fib_6502.bin    tests/fib.ir4
	./$(TARGET) -t arm64 -o $(BUILD)/fib_arm64.bin   --run tests/fib.ir4
	./$(TARGET) -t z80   -o $(BUILD)/core_z80.bin    tests/core.ir4
	./$(TARGET) -t 6502  -o $(BUILD)/core_6502.bin   tests/core.ir4
	./$(TARGET) -t arm64 -o $(BUILD)/core_arm64.bin  --run tests/core.ir4
	./$(TARGET) -t z80   -o $(BUILD)/muldiv_z80.bin  tests/muldiv.ir4
	./$(TARGET) -t 6502  -o $(BUILD)/muldiv_6502.bin tests/muldiv.ir4
	./$(TARGET) -t arm64 -o $(BUILD)/muldiv_arm64.bin --run tests/muldiv.ir4
	./$(TARGET) -t z80   -o $(BUILD)/mem_z80.bin     tests/mem.ir4
	./$(TARGET) -t 6502  -o $(BUILD)/mem_6502.bin    tests/mem.ir4
	./$(TARGET) -t arm64 -o $(BUILD)/mem_arm64.bin   --run tests/mem.ir4
	./$(TARGET) -t z80   -o $(BUILD)/float_z80.bin     tests/float.ir4
	./$(TARGET) -t 6502  -o $(BUILD)/float_6502.bin    tests/float.ir4
	./$(TARGET) -t arm64 -o $(BUILD)/float_arm64.bin --run tests/float.ir4
	./$(TARGET) -t z80   -o $(BUILD)/req_z80.bin       tests/req.ir4
	./$(TARGET) -t 6502  -o $(BUILD)/req_6502.bin      tests/req.ir4
	./$(TARGET) -t arm64 -o $(BUILD)/req_arm64.bin   --run tests/req.ir4
	./$(TARGET) -t z80   -o $(BUILD)/data_z80.bin      tests/data.ir4
	./$(TARGET) -t 6502  -o $(BUILD)/data_6502.bin     tests/data.ir4
	./$(TARGET) -t arm64 -o $(BUILD)/data_arm64.bin  --run tests/data.ir4
	./$(TARGET) -t arm64 -o $(BUILD)/ext_arm64.bin   --run tests/ext.ir4
	./$(TARGET) -t z80   -o $(BUILD)/math_z80.bin     tests/math.ir4
	./$(TARGET) -t 6502  -o $(BUILD)/math_6502.bin    tests/math.ir4
	./$(TARGET) -t arm64 -o $(BUILD)/math_arm64.bin   --run tests/math.ir4
	./$(TARGET) -t z80   -o $(BUILD)/fnptr_z80.bin    tests/fnptr.ir4
	./$(TARGET) -t 6502  -o $(BUILD)/fnptr_6502.bin   tests/fnptr.ir4
	./$(TARGET) -t arm64 -o $(BUILD)/fnptr_arm64.bin  --run tests/fnptr.ir4
	./$(TARGET) -t z80   -o $(BUILD)/peephole_z80.bin   tests/peephole.ir4
	./$(TARGET) -t 6502  -o $(BUILD)/peephole_6502.bin  tests/peephole.ir4
	./$(TARGET) -t arm64 -o $(BUILD)/peephole_arm64.bin --run tests/peephole.ir4
	$(MAKE) test-err
	$(MAKE) test-examples

test-examples: $(TARGET)
	@for f in examples/*.ir4; do \
		./$(TARGET) -t arm64 -o $(BUILD)/ex_arm64.bin $$f >/dev/null; \
		./$(TARGET) -t c64   -o $(BUILD)/ex_c64.prg   $$f >/dev/null; \
		./$(TARGET) -t cpm   -o $(BUILD)/ex_cpm.com   $$f >/dev/null; \
		echo "OK (examples): $$f"; \
	done

test-err: $(TARGET)
	@for f in tests/errors/*.ir4; do \
		if ./$(TARGET) -t arm64 -o $(BUILD)/err.bin $$f >/dev/null 2>&1; then \
			echo "FAIL: expected error for $$f"; exit 1; \
		else \
			echo "OK (rejected): $$f"; \
		fi; \
	done

test-z80: $(TARGET) tests/fib.ir4 tests/core.ir4 tests/muldiv.ir4 tests/mem.ir4 tests/float.ir4 tests/req.ir4 tests/data.ir4 tests/ext.ir4
	./$(TARGET) -t cpm -o build/fib.com    tests/fib.ir4
	./$(TARGET) -t cpm -o build/core.com   tests/core.ir4
	./$(TARGET) -t cpm -o build/muldiv.com tests/muldiv.ir4
	./$(TARGET) -t cpm -o build/mem.com    tests/mem.ir4
	./$(TARGET) -t cpm -o build/float.com  tests/float.ir4
	./$(TARGET) -t cpm -o build/req.com    tests/req.ir4
	./$(TARGET) -t cpm -o build/data.com   tests/data.ir4
	./$(TARGET) -t cpm -o build/ext.com    tests/ext.ir4
	./$(TARGET) -t cpm -o build/fnptr.com  tests/fnptr.ir4
	./$(TARGET) -t cpm -o build/peephole.com tests/peephole.ir4
	@if command -v tnylpo >/dev/null 2>&1; then \
		out=$$(cd $(BUILD) && tnylpo fib.com | tr -d '\r\n') && \
		echo "tnylpo fib.com -> $$out" && \
		test "$$out" = "0" && \
		out=$$(cd $(BUILD) && tnylpo core.com | tr -d '\r\n') && \
		echo "tnylpo core.com -> $$out" && \
		test "$$out" = "0" && \
		out=$$(cd $(BUILD) && tnylpo muldiv.com | tr -d '\r\n') && \
		echo "tnylpo muldiv.com -> $$out" && \
		test "$$out" = "0" && \
		out=$$(cd $(BUILD) && tnylpo mem.com | tr -d '\r\n') && \
		echo "tnylpo mem.com -> $$out" && \
		test "$$out" = "0" && \
		out=$$(cd $(BUILD) && tnylpo float.com | tr -d '\r\n') && \
		echo "tnylpo float.com -> $$out" && \
		test "$$out" = "0" && \
		out=$$(cd $(BUILD) && tnylpo req.com | tr -d '\r\n') && \
		echo "tnylpo req.com -> $$out" && \
		test "$$out" = "0" && \
		out=$$(cd $(BUILD) && tnylpo data.com | tr -d '\r\n') && \
		echo "tnylpo data.com -> $$out" && \
		test "$$out" = "0" && \
		out=$$(cd $(BUILD) && tnylpo ext.com | tr -d '\r\n') && \
		echo "tnylpo ext.com -> $$out" && \
		test "$$out" = "OK0" && \
		out=$$(cd $(BUILD) && tnylpo peephole.com | tr -d '\r\n') && \
		echo "tnylpo peephole.com -> $$out" && \
		test "$$out" = "0"; \
	else \
		echo "tnylpo not found; skipping Z80 execution"; \
	fi

test-ez80: $(TARGET) tests/fib.ir4 tests/core.ir4 tests/muldiv.ir4 tests/mem.ir4 tests/float.ir4 tests/req.ir4 tests/data.ir4 tests/ext.ir4 tests/math.ir4 tests/fnptr.ir4 tests/i32.ir4
	./$(TARGET) -t agon -o $(BUILD)/fib_agon.bin    tests/fib.ir4
	./$(TARGET) -t agon -o $(BUILD)/core_agon.bin   tests/core.ir4
	./$(TARGET) -t agon -o $(BUILD)/muldiv_agon.bin tests/muldiv.ir4
	./$(TARGET) -t agon -o $(BUILD)/mem_agon.bin    tests/mem.ir4
	./$(TARGET) -t agon -o $(BUILD)/float_agon.bin  tests/float.ir4
	./$(TARGET) -t agon -o $(BUILD)/req_agon.bin    tests/req.ir4
	./$(TARGET) -t agon -o $(BUILD)/data_agon.bin   tests/data.ir4
	./$(TARGET) -t agon -o $(BUILD)/ext_agon.bin    tests/ext.ir4
	./$(TARGET) -t agon -o $(BUILD)/math_agon.bin   tests/math.ir4
	./$(TARGET) -t agon -o $(BUILD)/fnptr_agon.bin  tests/fnptr.ir4
	./$(TARGET) -t agon -o $(BUILD)/i32_agon.bin    tests/i32.ir4
	./$(TARGET) -t agon -o $(BUILD)/peephole_agon.bin tests/peephole.ir4
	@emu=$$(command -v "$(AGON_EMU)" 2>/dev/null || true); \
	if [ -n "$$emu" ] && [ -x "$$emu" ]; then \
		sd="$$(pwd)/$(BUILD)/agon-sd"; \
		mkdir -p "$$sd"; \
		for pair in "fib_agon.bin:000000" "core_agon.bin:000000" "muldiv_agon.bin:000000" "mem_agon.bin:000000" "float_agon.bin:000000" "req_agon.bin:000000" "data_agon.bin:000000" "ext_agon.bin:000000" "math_agon.bin:000000" "fnptr_agon.bin:000000" "i32_agon.bin:000000" "peephole_agon.bin:000000"; do \
			f=$${pair%%:*}; exp=$${pair#*:}; \
			cp $(BUILD)/$$f "$$sd/test.bin"; \
			printf "load test.bin\nrun\n" > "$$sd/autoexec.txt"; \
			bc=$$(cd "$$(dirname "$$emu")" && printf "state\nexit\n" | SDL_AUDIODRIVER=dummy SDL_VIDEO_DRIVER=dummy "./$$(basename "$$emu")" -u --sdcard "$$sd" --debugger -b 0x000d5f | sed -n 's/.*\* 000d5f:.* BC:\([0-9a-fA-F]*\).*/\1/p' | head -n1); \
			echo "fab-agon-emulator $$f -> BC=$$bc"; \
			test "$$bc" = "$$exp"; \
		done; \
	else \
		echo "fab-agon-emulator not found; skipping eZ80 execution"; \
	fi

test-6502: $(TARGET) tests/fib.ir4 tests/core.ir4 tests/muldiv.ir4 tests/mem.ir4 tests/float.ir4 tests/req.ir4 tests/data.ir4 tests/ext.ir4
	./$(TARGET) -t sim65 -o $(BUILD)/fib_sim65.bin    tests/fib.ir4
	./$(TARGET) -t sim65 -o $(BUILD)/core_sim65.bin   tests/core.ir4
	./$(TARGET) -t sim65 -o $(BUILD)/muldiv_sim65.bin tests/muldiv.ir4
	./$(TARGET) -t sim65 -o $(BUILD)/mem_sim65.bin    tests/mem.ir4
	./$(TARGET) -t sim65 -o $(BUILD)/float_sim65.bin  tests/float.ir4
	./$(TARGET) -t sim65 -o $(BUILD)/req_sim65.bin    tests/req.ir4
	./$(TARGET) -t sim65 -o $(BUILD)/data_sim65.bin   tests/data.ir4
	./$(TARGET) -t sim65 -o $(BUILD)/ext_sim65.bin    tests/ext.ir4
	@if command -v sim65 >/dev/null 2>&1; then \
		out=$$(sim65 $(BUILD)/fib_sim65.bin | tr -d '\r\n'); \
		echo "sim65 fib_sim65.bin -> $$out"; \
		test "$$out" = "0"; \
		out=$$(sim65 $(BUILD)/core_sim65.bin | tr -d '\r\n'); \
		echo "sim65 core_sim65.bin -> $$out"; \
		test "$$out" = "0"; \
		out=$$(sim65 $(BUILD)/muldiv_sim65.bin | tr -d '\r\n'); \
		echo "sim65 muldiv_sim65.bin -> $$out"; \
		test "$$out" = "0"; \
		out=$$(sim65 $(BUILD)/mem_sim65.bin | tr -d '\r\n'); \
		echo "sim65 mem_sim65.bin -> $$out"; \
		test "$$out" = "0"; \
		out=$$(sim65 $(BUILD)/float_sim65.bin | tr -d '\r\n'); \
		echo "sim65 float_sim65.bin -> $$out"; \
		test "$$out" = "0"; \
		out=$$(sim65 $(BUILD)/req_sim65.bin | tr -d '\r\n'); \
		echo "sim65 req_sim65.bin -> $$out"; \
		test "$$out" = "0"; \
		out=$$(sim65 $(BUILD)/data_sim65.bin | tr -d '\r\n'); \
		echo "sim65 data_sim65.bin -> $$out"; \
		test "$$out" = "0"; \
		out=$$(sim65 $(BUILD)/ext_sim65.bin | tr -d '\r\n'); \
		echo "sim65 ext_sim65.bin -> $$out"; \
		test "$$out" = "OK0"; \
		./$(TARGET) -t sim65 -o $(BUILD)/math_sim65.bin tests/math.ir4; \
		out=$$(sim65 $(BUILD)/math_sim65.bin | tr -d '\r\n'); \
		echo "sim65 math_sim65.bin -> $$out"; \
		test "$$out" = "0"; \
		./$(TARGET) -t sim65 -o $(BUILD)/fnptr_sim65.bin tests/fnptr.ir4; \
		out=$$(sim65 $(BUILD)/fnptr_sim65.bin | tr -d '\r\n'); \
		echo "sim65 fnptr_sim65.bin -> $$out"; \
		test "$$out" = "0"; \
		./$(TARGET) -t sim65 -o $(BUILD)/peephole_sim65.bin tests/peephole.ir4; \
		out=$$(sim65 $(BUILD)/peephole_sim65.bin | tr -d '\r\n'); \
		echo "sim65 peephole_sim65.bin -> $$out"; \
		test "$$out" = "0"; \
	else \
		echo "sim65 not found; skipping 6502 execution"; \
	fi

test-rv32: $(TARGET) tests/fib.ir4 tests/core.ir4 tests/muldiv.ir4 tests/mem.ir4 tests/float.ir4 tests/req.ir4 tests/data.ir4 tests/ext.ir4
	./$(TARGET) -t virt -o $(BUILD)/fib_rv32.elf    tests/fib.ir4
	./$(TARGET) -t virt -o $(BUILD)/core_rv32.elf   tests/core.ir4
	./$(TARGET) -t virt -o $(BUILD)/muldiv_rv32.elf tests/muldiv.ir4
	./$(TARGET) -t virt -o $(BUILD)/mem_rv32.elf    tests/mem.ir4
	./$(TARGET) -t virt -o $(BUILD)/float_rv32.elf  tests/float.ir4
	./$(TARGET) -t virt -o $(BUILD)/req_rv32.elf    tests/req.ir4
	./$(TARGET) -t virt -o $(BUILD)/data_rv32.elf   tests/data.ir4
	./$(TARGET) -t virt -o $(BUILD)/ext_rv32.elf    tests/ext.ir4
	./$(TARGET) -t virt -o $(BUILD)/math_rv32.elf   tests/math.ir4
	./$(TARGET) -t virt -o $(BUILD)/fnptr_rv32.elf  tests/fnptr.ir4
	./$(TARGET) -t virt -o $(BUILD)/peephole_rv32.elf tests/peephole.ir4
	@if command -v qemu-system-riscv32 >/dev/null 2>&1; then \
		for pair in "fib_rv32.elf:0" "core_rv32.elf:0" "muldiv_rv32.elf:0" "mem_rv32.elf:0" "float_rv32.elf:0" "req_rv32.elf:0" "data_rv32.elf:0" "ext_rv32.elf:OK0" "math_rv32.elf:0" "fnptr_rv32.elf:0" "peephole_rv32.elf:0"; do \
			f=$${pair%%:*}; exp=$${pair#*:}; \
			out=$$(qemu-system-riscv32 -machine virt -cpu rv32 -bios none -nographic -kernel $(BUILD)/$$f | tr -d '\r\n'); \
			echo "qemu-system-riscv32 $$f -> $$out"; \
			test "$$out" = "$$exp"; \
		done; \
	else \
		echo "qemu-system-riscv32 not found; skipping RV32 execution"; \
	fi

rt: rt-z80 rt-ez80 rt-6502 rt-rv32

rt-ez80:
	python3 tools/ez80/gen_ez80_rt.py --output src/be_ez80_rt.h

rt-z80:
	python3 tools/z80/gen_math48.py --input tools/z80/math48.asm --output src/be_z80_math48.h
	python3 tools/asm2ir4.py --cpu z80 --input tools/z80/i32.asm --output lib/z80/i32.ir4
	python3 tools/asm2ir4.py --cpu z80 --input tools/z80/mem.asm --output lib/z80/mem.ir4

rt-6502:
	python3 tools/6502/gen_wozfp.py --input tools/6502/wozfp.asm --output src/be_6502_wozfp.h
	python3 tools/asm2ir4.py --cpu 6502 --input tools/6502/i32.asm --output lib/6502/i32.ir4
	python3 tools/asm2ir4.py --cpu 6502 --input tools/6502/mem.asm --output lib/6502/mem.ir4

rt-rv32:
	python3 tools/rv32/gen_rv32_fp.py --input tools/rv32/rv32_fp.c --output src/be_rv32_fp.h

AGON_SRCS = src/ir4c.c src/frontend/frontend.c src/frontend/lexer.c src/frontend/ir_emit.c src/codebuf.c src/path.c src/strpool.c src/vec.c \
            src/backend/ez80.c src/platform/agon.c

agon: $(AGON_SRCS)
	@PATH="$(HOME)/opt/agondev/bin:$$PATH"; \
	if ! command -v agondev-config >/dev/null 2>&1; then \
		echo "agondev-config not found (install agondev or add its bin/ to PATH)"; exit 1; \
	fi; \
	mkdir -p $(BUILD)/agon-build/src; \
	cp $(AGON_SRCS) $(BUILD)/agon-build/src/; \
	{ \
		echo "NAME = ir4c"; \
		echo "LDHAS_ARG_PROCESSING = 1"; \
		echo "LDHAS_EXIT_HANDLER = 0"; \
		echo "include $$(agondev-config --makefile)"; \
		echo "CFLAGS += -I$(CURDIR)/src -I$(CURDIR)/src/frontend -DIR4_MINIMAL -DIR4_ENABLE_EZ80=1 -DIR4_ENABLE_AGON=1"; \
	} > $(BUILD)/agon-build/Makefile; \
	$(MAKE) -C $(BUILD)/agon-build; \
	cp $(BUILD)/agon-build/bin/ir4c.bin $(BUILD)/ir4c_agon.bin; \
	echo "Built $(BUILD)/ir4c_agon.bin"

clean:
	rm -rf $(BUILD)
