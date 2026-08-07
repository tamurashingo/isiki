
PWD = $(shell pwd)

TARGET = esp_dir/EFI/BOOT/BOOTX64.EFI
SRCDIR = src/c
SRC = $(SRCDIR)/main.c $(SRCDIR)/kernel.c $(SRCDIR)/interrupt.c $(SRCDIR)/framebuffer.c $(SRCDIR)/process.c $(SRCDIR)/runtime.c $(SRCDIR)/lisp.c $(SRCDIR)/reader.c $(SRCDIR)/eval.c $(SRCDIR)/print.c $(SRCDIR)/repl.c $(SRCDIR)/subprimitive.c $(SRCDIR)/drivers/pci.c $(SRCDIR)/drivers/virtio.c $(SRCDIR)/drivers/virtqueue.c $(SRCDIR)/p9.c $(SRCDIR)/transport_virtio9p.c $(SRCDIR)/virtio9p.c $(SRCDIR)/stream.c $(SRCDIR)/stream_lisp.c $(SRCDIR)/load.c
HDR = $(SRCDIR)/kernel.h $(SRCDIR)/interrupt.h $(SRCDIR)/framebuffer.h $(SRCDIR)/process.h $(SRCDIR)/version.h $(SRCDIR)/font8x16.h $(SRCDIR)/runtime.h $(SRCDIR)/lisp.h $(SRCDIR)/reader.h $(SRCDIR)/eval.h $(SRCDIR)/print.h $(SRCDIR)/repl.h $(SRCDIR)/subprimitive.h $(SRCDIR)/drivers/pci.h $(SRCDIR)/drivers/virtio.h $(SRCDIR)/drivers/virtqueue.h $(SRCDIR)/p9.h $(SRCDIR)/p9_transport.h $(SRCDIR)/transport_virtio9p.h $(SRCDIR)/virtio9p.h $(SRCDIR)/stream.h $(SRCDIR)/stream_lisp.h $(SRCDIR)/load.h

GIT_HASH := $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)
BUILD_DATE := $(shell date -u +"%Y-%m-%dT%H:%M:%SZ")

TMPDIR = tmp
OBJ = $(patsubst $(SRCDIR)/%.c,$(TMPDIR)/%.o,$(SRC))

TESTDIR = test/c
TEST_COMMON_SRC = $(SRCDIR)/runtime.c $(SRCDIR)/lisp.c

TEST_SRC_RUNTIME = $(TEST_COMMON_SRC) $(TESTDIR)/runtime_test.c
TEST_BIN_RUNTIME = $(TMPDIR)/runtime_test

TEST_SRC_LISP = $(TEST_COMMON_SRC) $(TESTDIR)/lisp_test.c
TEST_BIN_LISP = $(TMPDIR)/lisp_test

TEST_SRC_PROCESS = $(TEST_COMMON_SRC) $(SRCDIR)/process.c $(TESTDIR)/process_test.c
TEST_BIN_PROCESS = $(TMPDIR)/process_test

TEST_SRC_READER = $(TEST_COMMON_SRC) $(SRCDIR)/process.c $(SRCDIR)/reader.c $(SRCDIR)/stream.c $(TESTDIR)/reader_test.c
TEST_BIN_READER = $(TMPDIR)/reader_test

TEST_SRC_EVAL = $(TEST_COMMON_SRC) $(SRCDIR)/eval.c $(TESTDIR)/eval_test.c
TEST_BIN_EVAL = $(TMPDIR)/eval_test

TEST_SRC_PRINT = $(TEST_COMMON_SRC) $(SRCDIR)/print.c $(TESTDIR)/print_test.c
TEST_BIN_PRINT = $(TMPDIR)/print_test

TEST_SRC_REPL = $(TEST_COMMON_SRC) $(SRCDIR)/process.c $(SRCDIR)/reader.c $(SRCDIR)/stream.c $(SRCDIR)/eval.c $(SRCDIR)/print.c $(SRCDIR)/repl.c $(TESTDIR)/repl_test.c
TEST_BIN_REPL = $(TMPDIR)/repl_test

TEST_SRC_SUBPRIMITIVE = $(TEST_COMMON_SRC) $(SRCDIR)/subprimitive.c $(TESTDIR)/subprimitive_test.c
TEST_BIN_SUBPRIMITIVE = $(TMPDIR)/subprimitive_test

TEST_SRC_SCRIPT = $(TEST_COMMON_SRC) $(SRCDIR)/process.c $(SRCDIR)/reader.c $(SRCDIR)/stream.c $(SRCDIR)/eval.c $(TESTDIR)/script_test.c
TEST_BIN_SCRIPT = $(TMPDIR)/script_test

TEST_SRC_STREAM = $(SRCDIR)/stream.c $(TESTDIR)/stream_test.c
TEST_BIN_STREAM = $(TMPDIR)/stream_test

TEST_SRC_LOAD = $(TEST_COMMON_SRC) $(SRCDIR)/eval.c $(SRCDIR)/reader.c $(SRCDIR)/stream.c $(SRCDIR)/load.c $(TESTDIR)/load_test.c
TEST_BIN_LOAD = $(TMPDIR)/load_test

TEST_SRC_STREAM_LISP = $(TEST_COMMON_SRC) $(SRCDIR)/process.c $(SRCDIR)/reader.c $(SRCDIR)/stream.c $(SRCDIR)/stream_lisp.c $(TESTDIR)/stream_lisp_test.c
TEST_BIN_STREAM_LISP = $(TMPDIR)/stream_lisp_test


.PHONY: all setup image transpile build compile run test clean

all: build


image:
	docker build -t isiki-builder .

build: $(SRC) $(HDR)
	mkdir -p esp_dir/EFI/BOOT
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint x86_64-w64-mingw32-gcc -v "$(PWD)":/workspace isiki-builder \
		-nostdlib -mno-red-zone -mgeneral-regs-only -O1 -shared \
		-mno-stack-arg-probe \
		-DISIKIOS_BUILD_HASH=\"$(GIT_HASH)\" \
		-DISIKIOS_BUILD_DATE=\"$(BUILD_DATE)\" \
		-Wl,--subsystem,10 \
		-Wl,--entry,EfiMain \
		-o $(TARGET) $(SRC)

# ファイル単体のコンパイルチェック用。リンクは行わず、生成した .o は tmp/ に捨てる
compile: $(OBJ)

$(TMPDIR):
	mkdir -p $(TMPDIR)

$(TMPDIR)/%.o: $(SRCDIR)/%.c $(HDR) | $(TMPDIR)
	mkdir -p $(dir $@)
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint x86_64-w64-mingw32-gcc -v "$(PWD)":/workspace isiki-builder \
		-nostdlib -mno-red-zone -mgeneral-regs-only -O1 -c \
		-mno-stack-arg-probe \
		-DISIKIOS_BUILD_HASH=\"$(GIT_HASH)\" \
		-DISIKIOS_BUILD_DATE=\"$(BUILD_DATE)\" \
		-o $@ $<

# ネイティブgccでビルドし、そのままコンテナ内で実行するユニットテスト
test: $(TEST_SRC_RUNTIME) $(TEST_SRC_LISP) $(TEST_SRC_PROCESS) $(TEST_SRC_READER) $(TEST_SRC_EVAL) $(TEST_SRC_PRINT) $(TEST_SRC_REPL) $(TEST_SRC_SUBPRIMITIVE) $(TEST_SRC_SCRIPT) $(TEST_SRC_STREAM) $(TEST_SRC_LOAD) $(TEST_SRC_STREAM_LISP) $(HDR) | $(TMPDIR)
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint gcc -v "$(PWD)":/workspace isiki-builder \
		-std=c11 -Wall -Wextra \
		-DISIKIOS_UNIT_TEST \
		-I$(SRCDIR) \
		-o $(TEST_BIN_RUNTIME) $(TEST_SRC_RUNTIME)
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint gcc -v "$(PWD)":/workspace isiki-builder \
		-std=c11 -Wall -Wextra \
		-DISIKIOS_UNIT_TEST \
		-I$(SRCDIR) \
		-o $(TEST_BIN_LISP) $(TEST_SRC_LISP)
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint gcc -v "$(PWD)":/workspace isiki-builder \
		-std=c11 -Wall -Wextra \
		-DISIKIOS_UNIT_TEST \
		-I$(SRCDIR) \
		-o $(TEST_BIN_PROCESS) $(TEST_SRC_PROCESS)
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint gcc -v "$(PWD)":/workspace isiki-builder \
		-std=c11 -Wall -Wextra \
		-DISIKIOS_UNIT_TEST \
		-I$(SRCDIR) \
		-o $(TEST_BIN_READER) $(TEST_SRC_READER)
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint gcc -v "$(PWD)":/workspace isiki-builder \
		-std=c11 -Wall -Wextra \
		-DISIKIOS_UNIT_TEST \
		-I$(SRCDIR) \
		-o $(TEST_BIN_EVAL) $(TEST_SRC_EVAL)
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint gcc -v "$(PWD)":/workspace isiki-builder \
		-std=c11 -Wall -Wextra \
		-DISIKIOS_UNIT_TEST \
		-I$(SRCDIR) \
		-o $(TEST_BIN_PRINT) $(TEST_SRC_PRINT)
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint gcc -v "$(PWD)":/workspace isiki-builder \
		-std=c11 -Wall -Wextra \
		-DISIKIOS_UNIT_TEST \
		-I$(SRCDIR) \
		-o $(TEST_BIN_REPL) $(TEST_SRC_REPL)
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint gcc -v "$(PWD)":/workspace isiki-builder \
		-std=c11 -Wall -Wextra \
		-DISIKIOS_UNIT_TEST \
		-I$(SRCDIR) \
		-o $(TEST_BIN_SUBPRIMITIVE) $(TEST_SRC_SUBPRIMITIVE)
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint gcc -v "$(PWD)":/workspace isiki-builder \
		-std=c11 -Wall -Wextra \
		-DISIKIOS_UNIT_TEST \
		-I$(SRCDIR) \
		-o $(TEST_BIN_SCRIPT) $(TEST_SRC_SCRIPT)
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint gcc -v "$(PWD)":/workspace isiki-builder \
		-std=c11 -Wall -Wextra \
		-DISIKIOS_UNIT_TEST \
		-I$(SRCDIR) \
		-o $(TEST_BIN_STREAM) $(TEST_SRC_STREAM)
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint gcc -v "$(PWD)":/workspace isiki-builder \
		-std=c11 -Wall -Wextra \
		-DISIKIOS_UNIT_TEST \
		-I$(SRCDIR) \
		-o $(TEST_BIN_LOAD) $(TEST_SRC_LOAD)
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint gcc -v "$(PWD)":/workspace isiki-builder \
		-std=c11 -Wall -Wextra \
		-DISIKIOS_UNIT_TEST \
		-I$(SRCDIR) \
		-o $(TEST_BIN_STREAM_LISP) $(TEST_SRC_STREAM_LISP)
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint /workspace/$(TEST_BIN_RUNTIME) -v "$(PWD)":/workspace isiki-builder
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint /workspace/$(TEST_BIN_LISP) -v "$(PWD)":/workspace isiki-builder
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint /workspace/$(TEST_BIN_PROCESS) -v "$(PWD)":/workspace isiki-builder
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint /workspace/$(TEST_BIN_READER) -v "$(PWD)":/workspace isiki-builder
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint /workspace/$(TEST_BIN_EVAL) -v "$(PWD)":/workspace isiki-builder
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint /workspace/$(TEST_BIN_PRINT) -v "$(PWD)":/workspace isiki-builder
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint /workspace/$(TEST_BIN_REPL) -v "$(PWD)":/workspace isiki-builder
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint /workspace/$(TEST_BIN_SUBPRIMITIVE) -v "$(PWD)":/workspace isiki-builder
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint /workspace/$(TEST_BIN_SCRIPT) -v "$(PWD)":/workspace isiki-builder
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint /workspace/$(TEST_BIN_STREAM) -v "$(PWD)":/workspace isiki-builder
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint /workspace/$(TEST_BIN_LOAD) -v "$(PWD)":/workspace isiki-builder
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint /workspace/$(TEST_BIN_STREAM_LISP) -v "$(PWD)":/workspace isiki-builder

clean:
	rm -rf esp_dir $(TMPDIR)

run:
	qemu-system-x86_64 \
		-m 256M \
		-drive if=pflash,format=raw,readonly=on,file=/opt/homebrew/opt/qemu/share/qemu/edk2-x86_64-code.fd \
		-drive format=raw,file=fat:rw:./esp_dir \
		-fsdev local,id=fsdev9p,path=$(PWD),security_model=none,readonly=on \
		-device virtio-9p-pci,fsdev=fsdev9p,mount_tag=hostshare

debug:
	qemu-system-x86_64 \
		-m 256M \
		-drive if=pflash,format=raw,readonly=on,file=/opt/homebrew/opt/qemu/share/qemu/edk2-x86_64-code.fd \
		-drive format=raw,file=fat:rw:./esp_dir \
		-fsdev local,id=fsdev9p,path=$(PWD),security_model=none,readonly=on \
		-device virtio-9p-pci,fsdev=fsdev9p,mount_tag=hostshare \
		-d cpu_reset,int -D qemu.log
