CC      = clang
CFLAGS  = -std=c89 -Wall -Wextra -O2 -Isrc

BUILD   = build
TARGET  = $(BUILD)/ir4c
SRCS    = src/ir4c.c src/frontend/frontend.c src/frontend/lexer.c src/frontend/ir_emit.c src/codebuf.c src/path.c src/strpool.c src/vec.c \
          src/backend/z80.c src/backend/ez80.c src/backend/6502.c src/backend/65816.c src/backend/arm64.c src/backend/rv32.c src/backend/bc.c \
          src/platform/cpm.c src/platform/zx81.c src/platform/agon.c src/platform/sim65.c src/platform/sim65816.c src/platform/c64.c src/platform/x16.c \
          src/platform/jit.c src/platform/elf.c src/platform/bc.c
OBJS    = $(patsubst src/%.c,$(BUILD)/%.o,$(SRCS))

AGON_EMU ?= $(HOME)/opt/fab-agon-emulator/fab-agon-emulator

.PHONY: all test test-err test-z80 test-ez80 test-6502 test-65816 test-rv32 rt rt-z80 rt-ez80 rt-6502 rt-rv32 clean

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

test: $(TARGET) tests/test_*.ir4
	./$(TARGET) -t z80   -o $(BUILD)/fib_z80.bin      tests/test_fib.ir4
	./$(TARGET) -t 6502  -o $(BUILD)/fib_6502.bin     tests/test_fib.ir4
	./$(TARGET) -t arm64 -o $(BUILD)/fib_arm64.bin    --run tests/test_fib.ir4
	./$(TARGET) -t z80   -o $(BUILD)/core_z80.bin     tests/test_core.ir4
	./$(TARGET) -t 6502  -o $(BUILD)/core_6502.bin    tests/test_core.ir4
	./$(TARGET) -t arm64 -o $(BUILD)/core_arm64.bin   --run tests/test_core.ir4
	./$(TARGET) -t z80   -o $(BUILD)/muldiv_z80.bin   tests/test_muldiv.ir4
	./$(TARGET) -t 6502  -o $(BUILD)/muldiv_6502.bin  tests/test_muldiv.ir4
	./$(TARGET) -t arm64 -o $(BUILD)/muldiv_arm64.bin --run tests/test_muldiv.ir4
	./$(TARGET) -t z80   -o $(BUILD)/mem_z80.bin      tests/test_mem.ir4
	./$(TARGET) -t 6502  -o $(BUILD)/mem_6502.bin     tests/test_mem.ir4
	./$(TARGET) -t arm64 -o $(BUILD)/mem_arm64.bin    --run tests/test_mem.ir4
	./$(TARGET) -t z80   -o $(BUILD)/float_z80.bin     tests/test_float.ir4
	./$(TARGET) -t 6502  -o $(BUILD)/float_6502.bin    tests/test_float.ir4
	./$(TARGET) -t arm64 -o $(BUILD)/float_arm64.bin --run tests/test_float.ir4
	./$(TARGET) -t z80   -o $(BUILD)/req_z80.bin       tests/test_req.ir4
	./$(TARGET) -t 6502  -o $(BUILD)/req_6502.bin      tests/test_req.ir4
	./$(TARGET) -t arm64 -o $(BUILD)/req_arm64.bin   --run tests/test_req.ir4
	./$(TARGET) -t z80   -o $(BUILD)/data_z80.bin      tests/test_data.ir4
	./$(TARGET) -t 6502  -o $(BUILD)/data_6502.bin     tests/test_data.ir4
	./$(TARGET) -t arm64 -o $(BUILD)/data_arm64.bin  --run tests/test_data.ir4
	./$(TARGET) -t arm64 -o $(BUILD)/ext_arm64.bin   --run tests/test_ext.ir4
	./$(TARGET) -t z80   -o $(BUILD)/math_z80.bin     tests/test_math.ir4
	./$(TARGET) -t 6502  -o $(BUILD)/math_6502.bin    tests/test_math.ir4
	./$(TARGET) -t arm64 -o $(BUILD)/math_arm64.bin   --run tests/test_math.ir4
	./$(TARGET) -t z80   -o $(BUILD)/i32_z80.bin      tests/test_i32.ir4
	./$(TARGET) -t 6502  -o $(BUILD)/i32_6502.bin     tests/test_i32.ir4
	./$(TARGET) -t arm64 -o $(BUILD)/i32_arm64.bin    --run tests/test_i32.ir4
	./$(TARGET) -t z80   -o $(BUILD)/fnptr_z80.bin    tests/test_fnptr.ir4
	./$(TARGET) -t 6502  -o $(BUILD)/fnptr_6502.bin   tests/test_fnptr.ir4
	./$(TARGET) -t arm64 -o $(BUILD)/fnptr_arm64.bin  --run tests/test_fnptr.ir4
	./$(TARGET) -t z80   -o $(BUILD)/peephole_z80.bin   tests/test_peephole.ir4
	./$(TARGET) -t 6502  -o $(BUILD)/peephole_6502.bin  tests/test_peephole.ir4
	./$(TARGET) -t arm64 -o $(BUILD)/peephole_arm64.bin --run tests/test_peephole.ir4
	./$(TARGET) -O0 -t arm64 -o $(BUILD)/peephole_o0_arm64.bin --run tests/test_peephole.ir4
	./$(TARGET) -t cpm   -o $(BUILD)/file_cpm.com     tests/test_file.ir4
	cd $(BUILD) && ../$(TARGET) -t arm64 -o file_arm64.bin --run ../tests/test_file.ir4 && rm -f IR4TEST.TMP
	./$(TARGET) -t c64   -o $(BUILD)/system_c64.prg     tests/test_system.ir4
	./$(TARGET) -t x16   -o $(BUILD)/system_x16.prg     tests/test_system.ir4
	./$(TARGET) -t cpm   -o $(BUILD)/system_cpm.com     tests/test_system.ir4
	./$(TARGET) -t arm64 -o $(BUILD)/system_arm64.bin   --run tests/test_system.ir4 -- ALPHA BETA
	./$(TARGET) -t c64   -o $(BUILD)/rtl_c64.prg        tests/test_rtl.ir4
	./$(TARGET) -t x16   -o $(BUILD)/rtl_x16.prg        tests/test_rtl.ir4
	./$(TARGET) -t arm64 -o $(BUILD)/rtl_arm64.bin      --run tests/test_rtl.ir4 >/dev/null
	./$(TARGET) -t c64   -o $(BUILD)/stdio_c64.prg      tests/test_stdio.ir4
	./$(TARGET) -t x16   -o $(BUILD)/stdio_x16.prg      tests/test_stdio.ir4
	./$(TARGET) -t arm64 -o $(BUILD)/stdio_arm64.bin    --run tests/test_stdio.ir4 >/dev/null
	./$(TARGET) -t c64   -o $(BUILD)/terminal_c64.prg   tests/test_terminal.ir4
	./$(TARGET) -t x16   -o $(BUILD)/terminal_x16.prg   tests/test_terminal.ir4
	./$(TARGET) -t arm64 -o $(BUILD)/terminal_arm64.bin --run tests/test_terminal.ir4 >/dev/null
	./$(TARGET) -t c64   -o $(BUILD)/graphics_c64.prg   tests/test_graphics.ir4
	./$(TARGET) -t x16   -o $(BUILD)/graphics_x16.prg   tests/test_graphics.ir4
	./$(TARGET) -t arm64 -o $(BUILD)/graphics_arm64.bin --run tests/test_graphics.ir4 >/dev/null
	./$(TARGET) -t c64   -o $(BUILD)/joystick_c64.prg   tests/test_joystick.ir4
	./$(TARGET) -t x16   -o $(BUILD)/joystick_x16.prg   tests/test_joystick.ir4
	./$(TARGET) -t arm64 -o $(BUILD)/joystick_arm64.bin --run tests/test_joystick.ir4
	./$(TARGET) -t c64   -o $(BUILD)/sound_c64.prg      tests/test_sound.ir4
	./$(TARGET) -t x16   -o $(BUILD)/sound_x16.prg      tests/test_sound.ir4
	./$(TARGET) -t arm64 -o $(BUILD)/sound_arm64.bin    --run tests/test_sound.ir4
	./$(TARGET) -t c64   -o $(BUILD)/sprite_c64.prg     tests/test_sprite.ir4
	./$(TARGET) -t x16   -o $(BUILD)/sprite_x16.prg     tests/test_sprite.ir4
	./$(TARGET) -t arm64 -o $(BUILD)/sprite_arm64.bin   --run tests/test_sprite.ir4 >/dev/null
	$(MAKE) test-err
	$(MAKE) test-examples

test-examples: $(TARGET)
	@for f in examples/*.ir4; do \
		./$(TARGET) -t arm64 -o $(BUILD)/ex_arm64.bin $$f >/dev/null; \
		./$(TARGET) -t c64   -o $(BUILD)/ex_c64.prg   $$f >/dev/null; \
		./$(TARGET) -t x16   -o $(BUILD)/ex_x16.prg   $$f >/dev/null; \
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

test-z80: $(TARGET) tests/test_*.ir4
	./$(TARGET) -t cpm -o build/fib.com      tests/test_fib.ir4
	./$(TARGET) -t cpm -o build/core.com     tests/test_core.ir4
	./$(TARGET) -t cpm -o build/muldiv.com   tests/test_muldiv.ir4
	./$(TARGET) -t cpm -o build/mem.com      tests/test_mem.ir4
	./$(TARGET) -t cpm -o build/float.com    tests/test_float.ir4
	./$(TARGET) -t cpm -o build/req.com      tests/test_req.ir4
	./$(TARGET) -t cpm -o build/data.com     tests/test_data.ir4
	./$(TARGET) -t cpm -o build/ext.com      tests/test_ext.ir4
	./$(TARGET) -t cpm -o build/i32.com      tests/test_i32.ir4
	./$(TARGET) -t cpm -o build/fnptr.com    tests/test_fnptr.ir4
	./$(TARGET) -t cpm -o build/peephole.com tests/test_peephole.ir4
	./$(TARGET) -t cpm -o build/file.com     tests/test_file.ir4
	./$(TARGET) -t cpm -o build/system.com   tests/test_system.ir4
	./$(TARGET) -t cpm -o build/rtl.com      tests/test_rtl.ir4
	./$(TARGET) -t cpm -o build/stdio.com    tests/test_stdio.ir4
	./$(TARGET) -t cpm -o build/terminal.com tests/test_terminal.ir4
	./$(TARGET) -t cpm -o build/graphics.com tests/test_graphics.ir4
	./$(TARGET) -t cpm -o build/joystick.com tests/test_joystick.ir4
	./$(TARGET) -t cpm -o build/sound.com    tests/test_sound.ir4
	./$(TARGET) -t cpm -o build/sprite.com   tests/test_sprite.ir4
	@if command -v tnylpo >/dev/null 2>&1; then \
		for pair in "fib.com:0" "core.com:0" "muldiv.com:0" "mem.com:0" "float.com:0" "req.com:0" "data.com:0" "ext.com:OK0" "i32.com:0" "fnptr.com:0" "peephole.com:0" "file.com:0" "system.com:0" "joystick.com:0" "sound.com:0"; do \
			f=$${pair%%:*}; exp=$${pair#*:}; \
			out=$$(cd $(BUILD) && tnylpo $$f | tr -d '\r\n'); \
			echo "tnylpo $$f -> $$out"; \
			test "$$out" = "$$exp" || exit 1; \
		done; \
		out=$$(cd $(BUILD) && tnylpo system.com ALPHA BETA | tr -d '\r\n'); \
		echo "tnylpo system.com ALPHA BETA -> $$out"; \
		test "$$out" = "0" || exit 1; \
		rm -f $(BUILD)/IR4TEST.TMP; \
		for f in rtl.com stdio.com terminal.com graphics.com sprite.com; do \
			out=$$(cd $(BUILD) && tnylpo $$f | tr -d '\r' | grep . | tail -n 1); \
			echo "tnylpo $$f -> $$out"; \
			test "$$out" = "0" || exit 1; \
		done; \
	else \
		echo "tnylpo not found; skipping Z80 execution"; \
	fi

test-ez80: $(TARGET) tests/test_*.ir4
	./$(TARGET) -t agon -o $(BUILD)/fib_agon.bin      tests/test_fib.ir4
	./$(TARGET) -t agon -o $(BUILD)/core_agon.bin     tests/test_core.ir4
	./$(TARGET) -t agon -o $(BUILD)/muldiv_agon.bin   tests/test_muldiv.ir4
	./$(TARGET) -t agon -o $(BUILD)/mem_agon.bin      tests/test_mem.ir4
	./$(TARGET) -t agon -o $(BUILD)/float_agon.bin    tests/test_float.ir4
	./$(TARGET) -t agon -o $(BUILD)/req_agon.bin      tests/test_req.ir4
	./$(TARGET) -t agon -o $(BUILD)/data_agon.bin     tests/test_data.ir4
	./$(TARGET) -t agon -o $(BUILD)/ext_agon.bin      tests/test_ext.ir4
	./$(TARGET) -t agon -o $(BUILD)/math_agon.bin     tests/test_math.ir4
	./$(TARGET) -t agon -o $(BUILD)/fnptr_agon.bin    tests/test_fnptr.ir4
	./$(TARGET) -t agon -o $(BUILD)/i32_agon.bin      tests/test_i32.ir4
	./$(TARGET) -t agon -o $(BUILD)/peephole_agon.bin tests/test_peephole.ir4
	./$(TARGET) -t agon -o $(BUILD)/file_agon.bin     tests/test_file.ir4
	./$(TARGET) -t agon -o $(BUILD)/system_agon.bin   tests/test_system.ir4
	./$(TARGET) -t agon -o $(BUILD)/rtl_agon.bin      tests/test_rtl.ir4
	./$(TARGET) -t agon -o $(BUILD)/stdio_agon.bin    tests/test_stdio.ir4
	./$(TARGET) -t agon -o $(BUILD)/terminal_agon.bin tests/test_terminal.ir4
	./$(TARGET) -t agon -o $(BUILD)/graphics_agon.bin tests/test_graphics.ir4
	./$(TARGET) -t agon -o $(BUILD)/joystick_agon.bin tests/test_joystick.ir4
	./$(TARGET) -t agon -o $(BUILD)/sound_agon.bin    tests/test_sound.ir4
	./$(TARGET) -t agon -o $(BUILD)/sprite_agon.bin   tests/test_sprite.ir4
	@emu=$$(command -v "$(AGON_EMU)" 2>/dev/null || true); \
	if [ -n "$$emu" ] && [ -x "$$emu" ]; then \
		sd="$$(pwd)/$(BUILD)/agon-sd"; \
		mkdir -p "$$sd"; \
		for pair in "fib_agon.bin:000000" "core_agon.bin:000000" "muldiv_agon.bin:000000" "mem_agon.bin:000000" "float_agon.bin:000000" "req_agon.bin:000000" "data_agon.bin:000000" "ext_agon.bin:000000" "math_agon.bin:000000" "fnptr_agon.bin:000000" "i32_agon.bin:000000" "peephole_agon.bin:000000" "file_agon.bin:000000" "system_agon.bin:000000" "rtl_agon.bin:000000" "stdio_agon.bin:000000" "terminal_agon.bin:000000" "graphics_agon.bin:000000" "joystick_agon.bin:000000" "sound_agon.bin:000000" "sprite_agon.bin:000000"; do \
			f=$${pair%%:*}; exp=$${pair#*:}; \
			cp $(BUILD)/$$f "$$sd/test.bin"; \
			printf "load test.bin\nrun\n" > "$$sd/autoexec.txt"; \
			bc=$$(cd "$$(dirname "$$emu")" && printf "state\nexit\n" | SDL_AUDIODRIVER=dummy SDL_VIDEO_DRIVER=dummy "./$$(basename "$$emu")" -u --sdcard "$$sd" --debugger -b 0x000d5f | sed -n 's/.*\* 000d5f:.* BC:\([0-9a-fA-F]*\).*/\1/p' | head -n1); \
			echo "fab-agon-emulator $$f -> BC=$$bc"; \
			test "$$bc" = "$$exp" || exit 1; \
		done; \
	else \
		echo "fab-agon-emulator not found; skipping eZ80 execution"; \
	fi

test-6502: $(TARGET) tests/test_*.ir4
	./$(TARGET) -t sim65 -o $(BUILD)/fib_sim65.bin      tests/test_fib.ir4
	./$(TARGET) -t sim65 -o $(BUILD)/core_sim65.bin     tests/test_core.ir4
	./$(TARGET) -t sim65 -o $(BUILD)/muldiv_sim65.bin   tests/test_muldiv.ir4
	./$(TARGET) -t sim65 -o $(BUILD)/mem_sim65.bin      tests/test_mem.ir4
	./$(TARGET) -t sim65 -o $(BUILD)/float_sim65.bin    tests/test_float.ir4
	./$(TARGET) -t sim65 -o $(BUILD)/req_sim65.bin      tests/test_req.ir4
	./$(TARGET) -t sim65 -o $(BUILD)/data_sim65.bin     tests/test_data.ir4
	./$(TARGET) -t sim65 -o $(BUILD)/ext_sim65.bin      tests/test_ext.ir4
	./$(TARGET) -t sim65 -o $(BUILD)/math_sim65.bin     tests/test_math.ir4
	./$(TARGET) -t sim65 -o $(BUILD)/i32_sim65.bin      tests/test_i32.ir4
	./$(TARGET) -t sim65 -o $(BUILD)/fnptr_sim65.bin    tests/test_fnptr.ir4
	./$(TARGET) -t sim65 -o $(BUILD)/peephole_sim65.bin tests/test_peephole.ir4
	./$(TARGET) -t sim65 -o $(BUILD)/file_sim65.bin     tests/test_file.ir4
	./$(TARGET) -t sim65 -o $(BUILD)/system_sim65.bin   tests/test_system.ir4
	./$(TARGET) -t sim65 -o $(BUILD)/rtl_sim65.bin      tests/test_rtl.ir4
	./$(TARGET) -t sim65 -o $(BUILD)/stdio_sim65.bin    tests/test_stdio.ir4
	./$(TARGET) -t sim65 -o $(BUILD)/terminal_sim65.bin tests/test_terminal.ir4
	./$(TARGET) -t sim65 -o $(BUILD)/graphics_sim65.bin tests/test_graphics.ir4
	./$(TARGET) -t sim65 -o $(BUILD)/joystick_sim65.bin tests/test_joystick.ir4
	./$(TARGET) -t sim65 -o $(BUILD)/sound_sim65.bin    tests/test_sound.ir4
	./$(TARGET) -t sim65 -o $(BUILD)/sprite_sim65.bin   tests/test_sprite.ir4
	@if command -v sim65 >/dev/null 2>&1; then \
		for pair in "fib_sim65.bin:0" "core_sim65.bin:0" "muldiv_sim65.bin:0" "mem_sim65.bin:0" "float_sim65.bin:0" "req_sim65.bin:0" "data_sim65.bin:0" "ext_sim65.bin:OK0" "math_sim65.bin:0" "i32_sim65.bin:0" "fnptr_sim65.bin:0" "peephole_sim65.bin:0" "file_sim65.bin:0" "system_sim65.bin:0" "joystick_sim65.bin:0" "sound_sim65.bin:0"; do \
			f=$${pair%%:*}; exp=$${pair#*:}; \
			out=$$(sim65 $(BUILD)/$$f | tr -d '\r\n'); \
			echo "sim65 $$f -> $$out"; \
			test "$$out" = "$$exp" || exit 1; \
		done; \
		for f in rtl_sim65.bin stdio_sim65.bin terminal_sim65.bin graphics_sim65.bin sprite_sim65.bin; do \
			out=$$(sim65 $(BUILD)/$$f | tr -d '\r' | grep . | tail -n 1); \
			echo "sim65 $$f -> $$out"; \
			test "$$out" = "0" || exit 1; \
		done; \
	else \
		echo "sim65 not found; skipping 6502 execution"; \
	fi

test-65816: $(TARGET) tests/test_*.ir4
	@for name in fib core muldiv mem float req data math i32 fnptr peephole file system joystick sound; do \
		out=$$(./$(TARGET) -t sim65816 -o $(BUILD)/$${name}_65816.bin tests/test_$${name}.ir4 | tr -d '\r' | tail -n 1); \
		echo "sim65816 test_$${name}.ir4 -> $$out"; \
		test "$$out" = "0" || exit 1; \
	done
	@for name in rtl stdio; do \
		out=$$(./$(TARGET) -t sim65816 -o $(BUILD)/$${name}_65816.bin tests/test_$${name}.ir4 | tr -d '\r' | grep . | tail -n 1); \
		echo "sim65816 test_$${name}.ir4 -> $$out"; \
		test "$$out" = "0" || exit 1; \
	done

test-rv32: $(TARGET) tests/test_*.ir4
	./$(TARGET) -t virt -o $(BUILD)/fib_rv32.elf      tests/test_fib.ir4
	./$(TARGET) -t virt -o $(BUILD)/core_rv32.elf     tests/test_core.ir4
	./$(TARGET) -t virt -o $(BUILD)/muldiv_rv32.elf   tests/test_muldiv.ir4
	./$(TARGET) -t virt -o $(BUILD)/mem_rv32.elf      tests/test_mem.ir4
	./$(TARGET) -t virt -o $(BUILD)/float_rv32.elf    tests/test_float.ir4
	./$(TARGET) -t virt -o $(BUILD)/req_rv32.elf      tests/test_req.ir4
	./$(TARGET) -t virt -o $(BUILD)/data_rv32.elf     tests/test_data.ir4
	./$(TARGET) -t virt -o $(BUILD)/ext_rv32.elf      tests/test_ext.ir4
	./$(TARGET) -t virt -o $(BUILD)/math_rv32.elf     tests/test_math.ir4
	./$(TARGET) -t virt -o $(BUILD)/i32_rv32.elf      tests/test_i32.ir4
	./$(TARGET) -t virt -o $(BUILD)/fnptr_rv32.elf    tests/test_fnptr.ir4
	./$(TARGET) -t virt -o $(BUILD)/peephole_rv32.elf tests/test_peephole.ir4
	./$(TARGET) -t virt -o $(BUILD)/file_rv32.elf     tests/test_file.ir4
	./$(TARGET) -t virt -o $(BUILD)/system_rv32.elf   tests/test_system.ir4
	./$(TARGET) -t virt -o $(BUILD)/rtl_rv32.elf      tests/test_rtl.ir4
	./$(TARGET) -t virt -o $(BUILD)/stdio_rv32.elf    tests/test_stdio.ir4
	./$(TARGET) -t virt -o $(BUILD)/terminal_rv32.elf tests/test_terminal.ir4
	./$(TARGET) -t virt -o $(BUILD)/graphics_rv32.elf tests/test_graphics.ir4
	./$(TARGET) -t virt -o $(BUILD)/joystick_rv32.elf tests/test_joystick.ir4
	./$(TARGET) -t virt -o $(BUILD)/sound_rv32.elf    tests/test_sound.ir4
	./$(TARGET) -t virt -o $(BUILD)/sprite_rv32.elf   tests/test_sprite.ir4
	@if command -v qemu-system-riscv32 >/dev/null 2>&1; then \
		for pair in "fib_rv32.elf:0" "core_rv32.elf:0" "muldiv_rv32.elf:0" "mem_rv32.elf:0" "float_rv32.elf:0" "req_rv32.elf:0" "data_rv32.elf:0" "ext_rv32.elf:OK0" "math_rv32.elf:0" "i32_rv32.elf:0" "fnptr_rv32.elf:0" "peephole_rv32.elf:0" "file_rv32.elf:0" "system_rv32.elf:0" "joystick_rv32.elf:0" "sound_rv32.elf:0"; do \
			f=$${pair%%:*}; exp=$${pair#*:}; \
			out=$$(qemu-system-riscv32 -machine virt -cpu rv32 -bios none -nographic -kernel $(BUILD)/$$f | tr -d '\r\n'); \
			echo "qemu-system-riscv32 $$f -> $$out"; \
			test "$$out" = "$$exp" || exit 1; \
		done; \
		for f in rtl_rv32.elf stdio_rv32.elf terminal_rv32.elf graphics_rv32.elf sprite_rv32.elf; do \
			out=$$(qemu-system-riscv32 -machine virt -cpu rv32 -bios none -nographic -kernel $(BUILD)/$$f | tr -d '\r' | grep . | tail -n 1); \
			echo "qemu-system-riscv32 $$f -> $$out"; \
			test "$$out" = "0" || exit 1; \
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
	python3 tools/asm2ir4.py --cpu z80 --input tools/z80/mem.asm --output lib/z80/memory.ir4

rt-6502:
	python3 tools/6502/gen_wozfp.py --input tools/6502/wozfp.asm --output src/be_6502_wozfp.h
	python3 tools/asm2ir4.py --cpu 6502 --input tools/6502/i32.asm --output lib/6502/i32.ir4
	python3 tools/asm2ir4.py --cpu 6502 --input tools/6502/mem.asm --output lib/6502/memory.ir4

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
