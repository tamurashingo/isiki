
PWD = $(shell pwd)

# OVMFのcode pflashイメージ。Homebrew版qemuでの既定値。CI等では
# `make test-qemu OVMF_CODE=/usr/share/OVMF/OVMF_CODE.fd` のように上書きする
OVMF_CODE ?= /opt/homebrew/opt/qemu/share/qemu/edk2-x86_64-code.fd

TARGET = esp_dir/EFI/BOOT/BOOTX64.EFI
SRCDIR = src/c
# トランスパイラ(transpileターゲット)の生成物。git管理対象外で、
# buildやtest実行時にtranspileターゲット経由で都度生成される
LISP_COMPILED = $(SRCDIR)/lisp_compiled.c
SRC = $(SRCDIR)/main.c $(SRCDIR)/kernel.c $(SRCDIR)/interrupt.c $(SRCDIR)/framebuffer.c $(SRCDIR)/process.c $(SRCDIR)/runtime.c $(SRCDIR)/lisp.c $(SRCDIR)/reader.c $(SRCDIR)/za.c $(SRCDIR)/eval.c $(SRCDIR)/print.c $(SRCDIR)/repl.c $(SRCDIR)/subprimitive.c $(SRCDIR)/drivers/pci.c $(SRCDIR)/drivers/virtio.c $(SRCDIR)/drivers/virtqueue.c $(SRCDIR)/p9.c $(SRCDIR)/transport_virtio9p.c $(SRCDIR)/virtio9p.c $(SRCDIR)/stream.c $(SRCDIR)/stream_lisp.c $(SRCDIR)/format.c $(SRCDIR)/load.c $(SRCDIR)/clock.c $(LISP_COMPILED)
HDR = $(SRCDIR)/kernel.h $(SRCDIR)/interrupt.h $(SRCDIR)/framebuffer.h $(SRCDIR)/process.h $(SRCDIR)/version.h $(SRCDIR)/font8x16.h $(SRCDIR)/runtime.h $(SRCDIR)/lisp.h $(SRCDIR)/reader.h $(SRCDIR)/za.h $(SRCDIR)/eval.h $(SRCDIR)/print.h $(SRCDIR)/repl.h $(SRCDIR)/subprimitive.h $(SRCDIR)/drivers/pci.h $(SRCDIR)/drivers/virtio.h $(SRCDIR)/drivers/virtqueue.h $(SRCDIR)/p9.h $(SRCDIR)/p9_transport.h $(SRCDIR)/transport_virtio9p.h $(SRCDIR)/virtio9p.h $(SRCDIR)/stream.h $(SRCDIR)/stream_lisp.h $(SRCDIR)/format.h $(SRCDIR)/load.h $(SRCDIR)/clock.h

GIT_HASH := $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)
BUILD_DATE := $(shell date -u +"%Y-%m-%dT%H:%M:%SZ")

BUILD_TMPDIR = tmp
OBJ = $(patsubst $(SRCDIR)/%.c,$(BUILD_TMPDIR)/%.o,$(SRC))

TESTDIR = test/c
TEST_COMMON_SRC = $(SRCDIR)/runtime.c $(SRCDIR)/lisp.c

TEST_SRC_RUNTIME = $(TEST_COMMON_SRC) $(SRCDIR)/process.c $(SRCDIR)/za.c $(SRCDIR)/eval.c $(SRCDIR)/reader.c $(SRCDIR)/stream.c $(TESTDIR)/runtime_test.c
TEST_BIN_RUNTIME = $(BUILD_TMPDIR)/runtime_test

TEST_SRC_LISP = $(TEST_COMMON_SRC) $(SRCDIR)/process.c $(SRCDIR)/za.c $(SRCDIR)/eval.c $(SRCDIR)/reader.c $(SRCDIR)/stream.c $(TESTDIR)/lisp_test.c
TEST_BIN_LISP = $(BUILD_TMPDIR)/lisp_test

TEST_SRC_PROCESS = $(TEST_COMMON_SRC) $(SRCDIR)/process.c $(SRCDIR)/za.c $(SRCDIR)/eval.c $(SRCDIR)/reader.c $(SRCDIR)/stream.c $(TESTDIR)/process_test.c
TEST_BIN_PROCESS = $(BUILD_TMPDIR)/process_test

TEST_SRC_READER = $(TEST_COMMON_SRC) $(SRCDIR)/process.c $(SRCDIR)/reader.c $(SRCDIR)/stream.c $(SRCDIR)/za.c $(SRCDIR)/eval.c $(TESTDIR)/reader_test.c
TEST_BIN_READER = $(BUILD_TMPDIR)/reader_test

TEST_SRC_EVAL = $(TEST_COMMON_SRC) $(SRCDIR)/process.c $(SRCDIR)/za.c $(SRCDIR)/eval.c $(SRCDIR)/reader.c $(SRCDIR)/stream.c $(TESTDIR)/eval_test.c
TEST_BIN_EVAL = $(BUILD_TMPDIR)/eval_test

TEST_SRC_PRINT = $(TEST_COMMON_SRC) $(SRCDIR)/process.c $(SRCDIR)/print.c $(SRCDIR)/za.c $(SRCDIR)/eval.c $(SRCDIR)/reader.c $(SRCDIR)/stream.c $(TESTDIR)/print_test.c
TEST_BIN_PRINT = $(BUILD_TMPDIR)/print_test

TEST_SRC_REPL = $(TEST_COMMON_SRC) $(SRCDIR)/process.c $(SRCDIR)/reader.c $(SRCDIR)/stream.c $(SRCDIR)/za.c $(SRCDIR)/eval.c $(SRCDIR)/print.c $(SRCDIR)/repl.c $(TESTDIR)/repl_test.c
TEST_BIN_REPL = $(BUILD_TMPDIR)/repl_test

TEST_SRC_SUBPRIMITIVE = $(TEST_COMMON_SRC) $(SRCDIR)/process.c $(SRCDIR)/subprimitive.c $(SRCDIR)/za.c $(SRCDIR)/eval.c $(SRCDIR)/reader.c $(SRCDIR)/stream.c $(TESTDIR)/subprimitive_test.c
TEST_BIN_SUBPRIMITIVE = $(BUILD_TMPDIR)/subprimitive_test

TEST_SRC_SCRIPT = $(TEST_COMMON_SRC) $(SRCDIR)/process.c $(SRCDIR)/reader.c $(SRCDIR)/stream.c $(SRCDIR)/stream_lisp.c $(SRCDIR)/za.c $(SRCDIR)/eval.c $(SRCDIR)/print.c $(SRCDIR)/format.c $(LISP_COMPILED) $(TESTDIR)/script_test.c
TEST_BIN_SCRIPT = $(BUILD_TMPDIR)/script_test

TEST_SRC_STREAM = $(SRCDIR)/stream.c $(TESTDIR)/stream_test.c
TEST_BIN_STREAM = $(BUILD_TMPDIR)/stream_test

TEST_SRC_LOAD = $(TEST_COMMON_SRC) $(SRCDIR)/process.c $(SRCDIR)/za.c $(SRCDIR)/eval.c $(SRCDIR)/reader.c $(SRCDIR)/stream.c $(SRCDIR)/load.c $(TESTDIR)/load_test.c
TEST_BIN_LOAD = $(BUILD_TMPDIR)/load_test

TEST_SRC_STREAM_LISP = $(TEST_COMMON_SRC) $(SRCDIR)/process.c $(SRCDIR)/reader.c $(SRCDIR)/stream.c $(SRCDIR)/stream_lisp.c $(SRCDIR)/za.c $(SRCDIR)/eval.c $(TESTDIR)/stream_lisp_test.c
TEST_BIN_STREAM_LISP = $(BUILD_TMPDIR)/stream_lisp_test

TEST_SRC_FORMAT = $(TEST_COMMON_SRC) $(SRCDIR)/process.c $(SRCDIR)/reader.c $(SRCDIR)/stream.c $(SRCDIR)/stream_lisp.c $(SRCDIR)/print.c $(SRCDIR)/format.c $(SRCDIR)/za.c $(SRCDIR)/eval.c $(TESTDIR)/format_test.c
TEST_BIN_FORMAT = $(BUILD_TMPDIR)/format_test

# interrupt.c/kernel.cはリンクせず、clock_test.cがget_tick_counter/
# kernel_get_boot_epoch_secondsをテスト用固定値で置き換える
TEST_SRC_CLOCK = $(TEST_COMMON_SRC) $(SRCDIR)/process.c $(SRCDIR)/clock.c $(SRCDIR)/za.c $(SRCDIR)/eval.c $(SRCDIR)/reader.c $(SRCDIR)/stream.c $(TESTDIR)/clock_test.c
TEST_BIN_CLOCK = $(BUILD_TMPDIR)/clock_test

TEST_SRC_P9 = $(SRCDIR)/p9.c $(TESTDIR)/p9_test.c
TEST_BIN_P9 = $(BUILD_TMPDIR)/p9_test

# virtio9p_test.cが独自にos_transport_virtio9p_instance()をfake定義するため、
# 本物のtransport_virtio9p.cはリンクしない
TEST_SRC_VIRTIO9P = $(SRCDIR)/p9.c $(SRCDIR)/virtio9p.c $(TESTDIR)/virtio9p_test.c
TEST_BIN_VIRTIO9P = $(BUILD_TMPDIR)/virtio9p_test

TEST_SRC_LISP_COMPILED = $(TEST_COMMON_SRC) $(SRCDIR)/process.c $(SRCDIR)/za.c $(SRCDIR)/eval.c $(SRCDIR)/reader.c $(SRCDIR)/stream.c $(SRCDIR)/stream_lisp.c $(SRCDIR)/format.c $(SRCDIR)/print.c $(LISP_COMPILED) $(TESTDIR)/lisp_compiled_test.c
TEST_BIN_LISP_COMPILED = $(BUILD_TMPDIR)/lisp_compiled_test


.PHONY: all setup image transpile build compile run test test-qemu clean

all: build


image:
	docker build --build-arg USER_UID="$$(id -u)" --build-arg USER_GID="$$(id -g)" -t isiki-builder .

# ros(roswell)はイメージのENTRYPOINTが/usr/local/bin/sbclのため、
# --entrypoint bashで明示的に上書きしないと引数がsbclへ直接渡ってしまう。
# imageターゲットでホストのUID/GIDと同じbuilderユーザーを作成し、
# そのユーザーにroswellをインストール済みのため、--userを指定しても
# $HOMEが正しく解決され動作する。
transpile:
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint bash -v "$(PWD)":/workspace isiki-builder \
		-c 'ros run --load src/lisp/transpile.lisp --eval "(main)" --quit'

$(LISP_COMPILED): transpile

build: $(SRC) $(HDR)
	mkdir -p esp_dir/EFI/BOOT
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint x86_64-w64-mingw32-gcc -v "$(PWD)":/workspace isiki-builder \
		-nostdlib -mno-red-zone -O1 -shared \
		-mno-stack-arg-probe \
		-DISIKIOS_BUILD_HASH=\"$(GIT_HASH)\" \
		-DISIKIOS_BUILD_DATE=\"$(BUILD_DATE)\" \
		-Wl,--subsystem,10 \
		-Wl,--entry,EfiMain \
		-o $(TARGET) $(SRC)

# ファイル単体のコンパイルチェック用。リンクは行わず、生成した .o は tmp/ に捨てる
compile: $(OBJ)

$(BUILD_TMPDIR):
	mkdir -p $(BUILD_TMPDIR)

$(BUILD_TMPDIR)/%.o: $(SRCDIR)/%.c $(HDR) | $(BUILD_TMPDIR)
	mkdir -p $(dir $@)
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint x86_64-w64-mingw32-gcc -v "$(PWD)":/workspace isiki-builder \
		-nostdlib -mno-red-zone -O1 -c \
		-mno-stack-arg-probe \
		-DISIKIOS_BUILD_HASH=\"$(GIT_HASH)\" \
		-DISIKIOS_BUILD_DATE=\"$(BUILD_DATE)\" \
		-o $@ $<

# ネイティブgccでビルドし、そのままコンテナ内で実行するユニットテスト
test: $(TEST_SRC_RUNTIME) $(TEST_SRC_LISP) $(TEST_SRC_PROCESS) $(TEST_SRC_READER) $(TEST_SRC_EVAL) $(TEST_SRC_PRINT) $(TEST_SRC_REPL) $(TEST_SRC_SUBPRIMITIVE) $(TEST_SRC_SCRIPT) $(TEST_SRC_STREAM) $(TEST_SRC_LOAD) $(TEST_SRC_STREAM_LISP) $(TEST_SRC_FORMAT) $(TEST_SRC_P9) $(TEST_SRC_VIRTIO9P) $(TEST_SRC_CLOCK) $(TEST_SRC_LISP_COMPILED) $(HDR) | $(BUILD_TMPDIR)
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint gcc -v "$(PWD)":/workspace isiki-builder \
		-std=c11 -Wall -Wextra \
		-DISIKIOS_UNIT_TEST \
		-I$(SRCDIR) \
		-o $(TEST_BIN_RUNTIME) $(TEST_SRC_RUNTIME) -lm
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint gcc -v "$(PWD)":/workspace isiki-builder \
		-std=c11 -Wall -Wextra \
		-DISIKIOS_UNIT_TEST \
		-I$(SRCDIR) \
		-o $(TEST_BIN_LISP) $(TEST_SRC_LISP) -lm
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint gcc -v "$(PWD)":/workspace isiki-builder \
		-std=c11 -Wall -Wextra \
		-DISIKIOS_UNIT_TEST \
		-I$(SRCDIR) \
		-o $(TEST_BIN_PROCESS) $(TEST_SRC_PROCESS) -lm
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint gcc -v "$(PWD)":/workspace isiki-builder \
		-std=c11 -Wall -Wextra \
		-DISIKIOS_UNIT_TEST \
		-I$(SRCDIR) \
		-o $(TEST_BIN_READER) $(TEST_SRC_READER) -lm
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint gcc -v "$(PWD)":/workspace isiki-builder \
		-std=c11 -Wall -Wextra \
		-DISIKIOS_UNIT_TEST \
		-I$(SRCDIR) \
		-o $(TEST_BIN_EVAL) $(TEST_SRC_EVAL) -lm
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint gcc -v "$(PWD)":/workspace isiki-builder \
		-std=c11 -Wall -Wextra \
		-DISIKIOS_UNIT_TEST \
		-I$(SRCDIR) \
		-o $(TEST_BIN_PRINT) $(TEST_SRC_PRINT) -lm
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint gcc -v "$(PWD)":/workspace isiki-builder \
		-std=c11 -Wall -Wextra \
		-DISIKIOS_UNIT_TEST \
		-I$(SRCDIR) \
		-o $(TEST_BIN_REPL) $(TEST_SRC_REPL) -lm
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint gcc -v "$(PWD)":/workspace isiki-builder \
		-std=c11 -Wall -Wextra \
		-DISIKIOS_UNIT_TEST \
		-I$(SRCDIR) \
		-o $(TEST_BIN_SUBPRIMITIVE) $(TEST_SRC_SUBPRIMITIVE) -lm
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint gcc -v "$(PWD)":/workspace isiki-builder \
		-std=c11 -Wall -Wextra \
		-DISIKIOS_UNIT_TEST \
		-I$(SRCDIR) \
		-o $(TEST_BIN_SCRIPT) $(TEST_SRC_SCRIPT) -lm
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint gcc -v "$(PWD)":/workspace isiki-builder \
		-std=c11 -Wall -Wextra \
		-DISIKIOS_UNIT_TEST \
		-I$(SRCDIR) \
		-o $(TEST_BIN_STREAM) $(TEST_SRC_STREAM)
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint gcc -v "$(PWD)":/workspace isiki-builder \
		-std=c11 -Wall -Wextra \
		-DISIKIOS_UNIT_TEST \
		-I$(SRCDIR) \
		-o $(TEST_BIN_LOAD) $(TEST_SRC_LOAD) -lm
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint gcc -v "$(PWD)":/workspace isiki-builder \
		-std=c11 -Wall -Wextra \
		-DISIKIOS_UNIT_TEST \
		-I$(SRCDIR) \
		-o $(TEST_BIN_STREAM_LISP) $(TEST_SRC_STREAM_LISP) -lm
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint gcc -v "$(PWD)":/workspace isiki-builder \
		-std=c11 -Wall -Wextra \
		-DISIKIOS_UNIT_TEST \
		-I$(SRCDIR) \
		-o $(TEST_BIN_FORMAT) $(TEST_SRC_FORMAT) -lm
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint gcc -v "$(PWD)":/workspace isiki-builder \
		-std=c11 -Wall -Wextra \
		-DISIKIOS_UNIT_TEST \
		-I$(SRCDIR) \
		-o $(TEST_BIN_CLOCK) $(TEST_SRC_CLOCK) -lm
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint gcc -v "$(PWD)":/workspace isiki-builder \
		-std=c11 -Wall -Wextra \
		-DISIKIOS_UNIT_TEST \
		-I$(SRCDIR) \
		-o $(TEST_BIN_P9) $(TEST_SRC_P9)
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint gcc -v "$(PWD)":/workspace isiki-builder \
		-std=c11 -Wall -Wextra \
		-DISIKIOS_UNIT_TEST \
		-I$(SRCDIR) \
		-o $(TEST_BIN_VIRTIO9P) $(TEST_SRC_VIRTIO9P)
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint gcc -v "$(PWD)":/workspace isiki-builder \
		-std=c11 -Wall -Wextra \
		-DISIKIOS_UNIT_TEST \
		-I$(SRCDIR) \
		-o $(TEST_BIN_LISP_COMPILED) $(TEST_SRC_LISP_COMPILED) -lm
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
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint /workspace/$(TEST_BIN_FORMAT) -v "$(PWD)":/workspace isiki-builder
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint /workspace/$(TEST_BIN_CLOCK) -v "$(PWD)":/workspace isiki-builder
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint /workspace/$(TEST_BIN_P9) -v "$(PWD)":/workspace isiki-builder
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint /workspace/$(TEST_BIN_VIRTIO9P) -v "$(PWD)":/workspace isiki-builder
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint /workspace/$(TEST_BIN_LISP_COMPILED) -v "$(PWD)":/workspace isiki-builder

clean:
	rm -rf esp_dir $(BUILD_TMPDIR)
	rm -f .qemu-test-trigger test-results.txt qemu.log

run:
	qemu-system-x86_64 \
		-m 256M \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive format=raw,file=fat:rw:./esp_dir \
		-fsdev local,id=fsdev9p,path=$(PWD),security_model=none,readonly=off \
		-device virtio-9p-pci,fsdev=fsdev9p,mount_tag=hostshare

debug:
	qemu-system-x86_64 \
		-m 256M \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive format=raw,file=fat:rw:./esp_dir \
		-fsdev local,id=fsdev9p,path=$(PWD),security_model=none,readonly=off \
		-device virtio-9p-pci,fsdev=fsdev9p,mount_tag=hostshare \
		-monitor stdio -serial null \
		-d cpu_reset,int -D qemu.log

# isiki_test.lispをQEMU上で全自動実行し、test-results.txtの結果でpass/failを判定する。
# .qemu-test-triggerの存在をkernel.cが検知し、qemu_boot_test.lispをloadしてから
# ResetSystemでQEMUを電源断する
test-qemu: build
	mkdir -p $(BUILD_TMPDIR)
	rm -f .qemu-test-trigger test-results.txt
	touch .qemu-test-trigger
	qemu-system-x86_64 \
		-m 256M \
		-display none \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive format=raw,file=fat:rw:./esp_dir \
		-fsdev local,id=fsdev9p,path=$(PWD),security_model=none,readonly=off \
		-device virtio-9p-pci,fsdev=fsdev9p,mount_tag=hostshare \
		-no-reboot
	rm -f .qemu-test-trigger
	test -f test-results.txt
	cat test-results.txt
	grep -q " 0 failed" test-results.txt

# za_test.lisp(拡張1/4/6)のGC誘発を伴う大量ループ(N=50000)をローカルでのみ実行する。
# GitHub ActionsはKVM無しでQEMUがTCG(ソフトウェアエミュレーション)にフォールバック
# するため、この負荷テストはCIのmilestone 2(qemu_boot_m2_za.lisp)からは外してある
test-qemu-stress:
	$(MAKE) test-qemu-milestone MILESTONE=test/lisp/qemu_boot_m2_za_stress.lisp

# test-qemuと同様だが、MILESTONE変数(boot-entryスクリプトのパス)を
# .qemu-test-triggerの内容として書き込み、指定したmilestoneのみを実行する
# (GitHub Actions側でハングと正常進行の区別をつけるためのmilestone分割用)
test-qemu-milestone: build
	mkdir -p $(BUILD_TMPDIR)
	test -n "$(MILESTONE)"
	rm -f .qemu-test-trigger test-results.txt
	echo "$(MILESTONE)" > .qemu-test-trigger
	qemu-system-x86_64 \
		-m 256M \
		-display none \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive format=raw,file=fat:rw:./esp_dir \
		-fsdev local,id=fsdev9p,path=$(PWD),security_model=none,readonly=off \
		-device virtio-9p-pci,fsdev=fsdev9p,mount_tag=hostshare \
		-no-reboot
	rm -f .qemu-test-trigger
	test -f test-results.txt
	cat test-results.txt
	grep -q " 0 failed" test-results.txt
