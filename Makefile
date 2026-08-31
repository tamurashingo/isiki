
PWD = $(shell pwd)

# OVMFのcode pflashイメージ。Homebrew版qemuでの既定値。CI等では
# `make test-qemu OVMF_CODE=/usr/share/OVMF/OVMF_CODE.fd` のように上書きする
OVMF_CODE ?= /opt/homebrew/opt/qemu/share/qemu/edk2-x86_64-code.fd

TARGET = esp_dir/EFI/BOOT/BOOTX64.EFI
SRCDIR = src/c
# トランスパイラ(transpileターゲット)の生成物。git管理対象外で、
# buildやtest実行時にtranspileターゲット経由で都度生成される
LISP_COMPILED = $(SRCDIR)/lisp_compiled.c
SRC = $(SRCDIR)/main.c $(SRCDIR)/kernel.c $(SRCDIR)/interrupt.c $(SRCDIR)/framebuffer.c $(SRCDIR)/process.c $(SRCDIR)/runtime.c $(SRCDIR)/lisp.c $(SRCDIR)/reader.c $(SRCDIR)/za.c $(SRCDIR)/eval.c $(SRCDIR)/print.c $(SRCDIR)/repl.c $(SRCDIR)/subprimitive.c $(SRCDIR)/drivers/pci.c $(SRCDIR)/drivers/virtio.c $(SRCDIR)/drivers/virtqueue.c $(SRCDIR)/drivers/ide.c $(SRCDIR)/block_device.c $(SRCDIR)/ide_subprimitive.c $(SRCDIR)/p9.c $(SRCDIR)/transport_virtio9p.c $(SRCDIR)/virtio9p.c $(SRCDIR)/stream.c $(SRCDIR)/stream_lisp.c $(SRCDIR)/format.c $(SRCDIR)/load.c $(SRCDIR)/clock.c $(LISP_COMPILED)
HDR = $(SRCDIR)/kernel.h $(SRCDIR)/interrupt.h $(SRCDIR)/framebuffer.h $(SRCDIR)/process.h $(SRCDIR)/version.h $(SRCDIR)/font8x16.h $(SRCDIR)/runtime.h $(SRCDIR)/lisp.h $(SRCDIR)/reader.h $(SRCDIR)/za.h $(SRCDIR)/eval.h $(SRCDIR)/print.h $(SRCDIR)/repl.h $(SRCDIR)/subprimitive.h $(SRCDIR)/drivers/pci.h $(SRCDIR)/drivers/virtio.h $(SRCDIR)/drivers/virtqueue.h $(SRCDIR)/drivers/ide.h $(SRCDIR)/block_device.h $(SRCDIR)/ide_subprimitive.h $(SRCDIR)/p9.h $(SRCDIR)/p9_transport.h $(SRCDIR)/transport_virtio9p.h $(SRCDIR)/virtio9p.h $(SRCDIR)/stream.h $(SRCDIR)/stream_lisp.h $(SRCDIR)/format.h $(SRCDIR)/load.h $(SRCDIR)/clock.h

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

TEST_SRC_SCRIPT = $(TEST_COMMON_SRC) $(SRCDIR)/process.c $(SRCDIR)/reader.c $(SRCDIR)/stream.c $(SRCDIR)/stream_lisp.c $(SRCDIR)/za.c $(SRCDIR)/eval.c $(SRCDIR)/print.c $(SRCDIR)/format.c $(SRCDIR)/subprimitive.c $(LISP_COMPILED) $(TESTDIR)/script_test.c
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

TEST_SRC_LISP_COMPILED = $(TEST_COMMON_SRC) $(SRCDIR)/process.c $(SRCDIR)/za.c $(SRCDIR)/eval.c $(SRCDIR)/reader.c $(SRCDIR)/stream.c $(SRCDIR)/stream_lisp.c $(SRCDIR)/format.c $(SRCDIR)/print.c $(SRCDIR)/subprimitive.c $(LISP_COMPILED) $(TESTDIR)/lisp_compiled_test.c
TEST_BIN_LISP_COMPILED = $(BUILD_TMPDIR)/lisp_compiled_test

# interrupt.cはリンクせず、ide_test.cが独自にinb/outb/inw/outwをfake定義して
# レジスタ操作の順序・値を検証する(subprimitive_test.cと同じ方針)
TEST_SRC_IDE = $(TEST_COMMON_SRC) $(SRCDIR)/process.c $(SRCDIR)/za.c $(SRCDIR)/eval.c $(SRCDIR)/reader.c $(SRCDIR)/stream.c $(SRCDIR)/drivers/ide.c $(SRCDIR)/block_device.c $(SRCDIR)/ide_subprimitive.c $(TESTDIR)/ide_test.c
TEST_BIN_IDE = $(BUILD_TMPDIR)/ide_test


.PHONY: all setup image transpile build compile run test test-qemu test-qemu-all clean

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
		-Wall -Wextra \
		-mno-stack-arg-probe \
		-DISIKIOS_BUILD_HASH=\"$(GIT_HASH)\" \
		-DISIKIOS_BUILD_DATE=\"$(BUILD_DATE)\" \
		-o $@ $<

# ネイティブgccでビルドし、そのままコンテナ内で実行するユニットテスト
test: $(TEST_SRC_RUNTIME) $(TEST_SRC_LISP) $(TEST_SRC_PROCESS) $(TEST_SRC_READER) $(TEST_SRC_EVAL) $(TEST_SRC_PRINT) $(TEST_SRC_REPL) $(TEST_SRC_SUBPRIMITIVE) $(TEST_SRC_SCRIPT) $(TEST_SRC_STREAM) $(TEST_SRC_LOAD) $(TEST_SRC_STREAM_LISP) $(TEST_SRC_FORMAT) $(TEST_SRC_P9) $(TEST_SRC_VIRTIO9P) $(TEST_SRC_CLOCK) $(TEST_SRC_LISP_COMPILED) $(TEST_SRC_IDE) $(HDR) | $(BUILD_TMPDIR)
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
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint gcc -v "$(PWD)":/workspace isiki-builder \
		-std=c11 -Wall -Wextra \
		-DISIKIOS_UNIT_TEST \
		-I$(SRCDIR) \
		-o $(TEST_BIN_IDE) $(TEST_SRC_IDE) -lm
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
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint /workspace/$(TEST_BIN_IDE) -v "$(PWD)":/workspace isiki-builder

clean:
	rm -rf esp_dir $(BUILD_TMPDIR)
	rm -f .qemu-test-trigger test-results.txt qemu.log

# IDE/ATA PIO動作確認用の使い捨てディスクイメージ。qemu-imgはCI環境で未保証の
# パッケージのため使わず、dd/printfのみで作成する。先頭にIDE_TEST_MAGICを書き込み、
# read-sectorでのセクタ0読み込み確認に使う。Secondaryチャネル(bus=1,unit=0)の
# masterとして明示的にアタッチする(Primaryは-drive ...,file=fat:rw:./esp_dirの
# ESP起動ドライブがif=指定無しでデフォルト占有しているため触らない)
IDE_DISK_IMG = $(BUILD_TMPDIR)/ide_disk.img
IDE_TEST_MAGIC = ISIKIOS-IDE-TEST-SECTOR0-MAGIC!

$(IDE_DISK_IMG): | $(BUILD_TMPDIR)
	dd if=/dev/zero of=$@ bs=1M count=16 2>/dev/null
	printf '%s' '$(IDE_TEST_MAGIC)' | dd of=$@ conv=notrunc bs=1 seek=0 2>/dev/null

# FAT16読み書き実装(documents/fs.md FAT16-M0(2)/FAT16-M2/FAT16-M3/FAT16-M4)用の
# 使い捨てテストイメージ。mkfs.vfat -F 16でFAT16フォーマットした後、loopマウント
# して既知内容のテストファイル(空のHELLO.TXT、"Hello from FAT16!"を書いたTEST.LSP、
# "0123456789"を250回(2500byte、クラスタサイズ2048byteを超えるので2クラスタに
# 分割される)繰り返したBIG.TXT、"A"を2048回(ちょうど1クラスタ)繰り返した
# WRITE1.TXT(FAT16-M6書き込みテスト専用、他のテストは読み込み専用のまま変更しない
# ため書き込み対象はこのファイルのみに限定する)を配置する。DELETED.TXTは
# 最後に作成してからrmする
# ことでルートディレクトリエントリの先頭バイトが0xE5(削除済みマーカー)になった
# 状態を作る(FAT16-M2の削除済みエントリスキップ確認用)。DELETED.TXTを最後に
# 作る/消すのは、それより前に作るとカーネルのvfatドライバが後続ファイル作成時に
# 空いた0xE5スロットを再利用してしまい(実機確認済み)、削除済みマーカーが
# 消えてしまうため。loopマウントはCAP_SYS_ADMIN相当の権限を要するため、この
# ルールのみ--privilegedでdocker run する(他のビルド用ルールは
# --user "$(id -u):$(id -g)"で非rootのまま)。
#
# umount後、ホスト側でFATテーブル(1本目、reserved-sectors=4なのでLBA4=バイト
# オフセット2048から開始)にクラスタ40→41→44→終端という非連続なチェインを直接
# dd/printfで書き込む(FAT16-M3のfat16-cluster-chain確認用)。クラスタ40/41/44は
# mkfs.vfat後は未使用(0x0000)で、TEST.LSP/BIG.TXT/WRITE1.TXT/SUBDIR以下が使う
# クラスタ(概ね3〜9番台、xxd -g1で目視確認済み)とは十分な余裕を持って重複しない
# ため、実ファイル/ディレクトリの内容を破壊せずに合成チェインを作れる
# (FAT16-M7aでSUBDIR以下を追加した際、元は10/11/14を使っていたが実クラスタ使用と
# 衝突したため、余裕を持って40/41/44へ変更した)。オフセットは
# reserved-sectors*bytes-per-sector(2048) + cluster-no*2で計算した固定値
# (2128/2130/2136)、値はLE u16(41=0x2900/44=0x2C00/終端=0xFFFF)。printfの
# エスケープは\xHH(16進)ではなく\0NNN(8進、POSIX printfが規定する形式)を使う。
# \xHHはbash等の拡張でしかなく、GitHub Actions runner(Ubuntu)のmakeレシピが使う
# /bin/sh=dashのprintf組み込みでは解釈されず、リテラル文字列"\x0b\x00"がそのまま
# 出力されてしまう(ddのcount=2はその先頭2byte、つまり'\','x'=0x5C,0x78を書き込む
# ことになり、意図した値と全く異なるゴミがFATに書かれる)。macOSの/bin/sh(bash)では
# \xHHが解釈されるため、ここはローカルとCIで結果が分かれる典型的なシェル差異だった。
FAT16_DISK_IMG = $(BUILD_TMPDIR)/fat16_test.img
FAT16_TEST_STRING = Hello from FAT16!

$(FAT16_DISK_IMG): | $(BUILD_TMPDIR)
	dd if=/dev/zero of=$@ bs=1M count=16 2>/dev/null
	docker run --rm --privileged --entrypoint bash -v "$(PWD)":/workspace -w /workspace isiki-builder \
		-c 'set -e; \
			mkfs.vfat -F 16 $@; \
			mkdir -p /mnt/fat16_test; \
			mount -o loop $@ /mnt/fat16_test; \
			touch /mnt/fat16_test/HELLO.TXT; \
			printf "%s\n" "$(FAT16_TEST_STRING)" > /mnt/fat16_test/TEST.LSP; \
			printf "0123456789%.0s" {1..250} > /mnt/fat16_test/BIG.TXT; \
			printf "A%.0s" {1..2048} > /mnt/fat16_test/WRITE1.TXT; \
			mkdir /mnt/fat16_test/SUBDIR; \
			printf "nested file content" > /mnt/fat16_test/SUBDIR/NESTED.TXT; \
			mkdir /mnt/fat16_test/SUBDIR/DEEPER; \
			touch /mnt/fat16_test/SUBDIR/DEEPER/DEEP.TXT; \
			printf "will be deleted" > /mnt/fat16_test/DELETED.TXT; \
			rm /mnt/fat16_test/DELETED.TXT; \
			umount /mnt/fat16_test'
	printf '\051\000' | dd of=$@ bs=1 seek=2128 count=2 conv=notrunc 2>/dev/null
	printf '\054\000' | dd of=$@ bs=1 seek=2130 count=2 conv=notrunc 2>/dev/null
	printf '\377\377' | dd of=$@ bs=1 seek=2136 count=2 conv=notrunc 2>/dev/null

# Secondaryチャネル(bus=1,unit=0)にアタッチするディスクイメージ。IDE milestone(既存)
# はIDE_DISK_IMG(素の16MBイメージ+magic文字列)、FAT16milestone(documents/fs.md)は
# FAT16_DISK_IMGを、test-qemu-milestone呼び出し時にQEMU_DISK_IMG=...で上書きして
# 使う。既定値はIDE_DISK_IMGなので既存ターゲットの挙動は変わらない。
QEMU_DISK_IMG = $(IDE_DISK_IMG)

# run/debug(対話的なQEMU起動)はfat16.lisp/ide.lispを手元で試せるよう、既定で
# FAT16フォーマット済みのFAT16_DISK_IMGをアタッチする。QEMU_DISK_IMGとは別の変数に
# しているのは、test-qemu/test-qemu-milestoneの既定値(IDE_DISK_IMG、IDE milestoneの
# magic文字列前提)に影響を与えないため。`make run RUN_DISK_IMG=...`で個別に
# 上書きできる。
RUN_DISK_IMG ?= $(FAT16_DISK_IMG)

run: $(RUN_DISK_IMG)
	qemu-system-x86_64 \
		-m 256M \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive format=raw,file=fat:rw:./esp_dir \
		-drive id=hd0,file=$(RUN_DISK_IMG),format=raw,if=ide,bus=1,unit=0 \
		-fsdev local,id=fsdev9p,path=$(PWD),security_model=none,readonly=off \
		-device virtio-9p-pci,fsdev=fsdev9p,mount_tag=hostshare

debug: $(RUN_DISK_IMG)
	qemu-system-x86_64 \
		-m 256M \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive format=raw,file=fat:rw:./esp_dir \
		-drive id=hd0,file=$(RUN_DISK_IMG),format=raw,if=ide,bus=1,unit=0 \
		-fsdev local,id=fsdev9p,path=$(PWD),security_model=none,readonly=off \
		-device virtio-9p-pci,fsdev=fsdev9p,mount_tag=hostshare \
		-monitor stdio -serial null \
		-d cpu_reset,int -D qemu.log

# isiki_test.lispをQEMU上で全自動実行し、test-results.txtの結果でpass/failを判定する。
# .qemu-test-triggerの存在をkernel.cが検知し、qemu_boot_test.lispをloadしてから
# ResetSystemでQEMUを電源断する
test-qemu: build $(QEMU_DISK_IMG)
	mkdir -p $(BUILD_TMPDIR)
	rm -f .qemu-test-trigger test-results.txt
	touch .qemu-test-trigger
	qemu-system-x86_64 \
		-m 256M \
		-display none \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive format=raw,file=fat:rw:./esp_dir \
		-drive id=hd0,file=$(QEMU_DISK_IMG),format=raw,if=ide,bus=1,unit=0 \
		-fsdev local,id=fsdev9p,path=$(PWD),security_model=none,readonly=off \
		-device virtio-9p-pci,fsdev=fsdev9p,mount_tag=hostshare \
		-no-reboot
	rm -f .qemu-test-trigger
	test -f test-results.txt
	cat test-results.txt
	grep -q " 0 failed" test-results.txt

# test-qemu(デフォルトのqemu_boot_test.lisp)はdevice.lisp/ide.lisp/fat16.lispを
# loadしないため、IDE/FAT16milestoneはtest-qemuだけでは検証されない。この3つを
# 順に実行してまとめて検証する(いずれかが失敗すればmakeはそこで停止する)。
# fat16_test.lispはディスク上にファイルを作成・書き込みする破壊的なテストのため、
# 前回実行分のディスクイメージが残っているとpristineな状態を前提にしたアサーション
# (ディレクトリ一覧やファイル内容の期待値)が失敗する。毎回作り直すため事前にrmする
test-qemu-all:
	rm -f $(IDE_DISK_IMG) $(FAT16_DISK_IMG)
	$(MAKE) test-qemu
	$(MAKE) test-qemu-milestone MILESTONE=test/lisp/qemu_boot_m5_ide.lisp
	$(MAKE) test-qemu-milestone MILESTONE=test/lisp/qemu_boot_m6_fat16.lisp QEMU_DISK_IMG=tmp/fat16_test.img

# za_test.lisp(拡張1/4/6)のGC誘発を伴う大量ループ(N=50000)をローカルでのみ実行する。
# GitHub ActionsはKVM無しでQEMUがTCG(ソフトウェアエミュレーション)にフォールバック
# するため、この負荷テストはCIのmilestone 2(qemu_boot_m2_za.lisp)からは外してある
test-qemu-stress:
	$(MAKE) test-qemu-milestone MILESTONE=test/lisp/qemu_boot_m2_za_stress.lisp

# test-qemuと同様だが、MILESTONE変数(boot-entryスクリプトのパス)を
# .qemu-test-triggerの内容として書き込み、指定したmilestoneのみを実行する
# (GitHub Actions側でハングと正常進行の区別をつけるためのmilestone分割用)
test-qemu-milestone: build $(QEMU_DISK_IMG)
	mkdir -p $(BUILD_TMPDIR)
	test -n "$(MILESTONE)"
	rm -f .qemu-test-trigger test-results.txt
	echo "$(MILESTONE)" > .qemu-test-trigger
	qemu-system-x86_64 \
		-m 256M \
		-display none \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive format=raw,file=fat:rw:./esp_dir \
		-drive id=hd0,file=$(QEMU_DISK_IMG),format=raw,if=ide,bus=1,unit=0 \
		-fsdev local,id=fsdev9p,path=$(PWD),security_model=none,readonly=off \
		-device virtio-9p-pci,fsdev=fsdev9p,mount_tag=hostshare \
		-no-reboot
	rm -f .qemu-test-trigger
	test -f test-results.txt
	cat test-results.txt
	grep -q " 0 failed" test-results.txt
