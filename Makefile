
PWD = $(shell pwd)

# OVMFのcode pflashイメージ。Homebrew版qemuでの既定値。CI等では
# `make test-qemu OVMF_CODE=/usr/share/OVMF/OVMF_CODE.fd` のように上書きする
OVMF_CODE ?= /opt/homebrew/opt/qemu/share/qemu/edk2-x86_64-code.fd

# [測定] ゲストに与えるメモリ量。Lispヒープは起動時にUEFIの最大
# EfiConventionalMemory領域から取るので、これを下げるとGC頻度が上がる。
# **生成コードを一切変えずにGC圧を上げられる**のが要点で、
# 「GC_DEBUGを有効にするとcc_carのインライン化が消えて観測できない」という
# 壁を迂回できる(%%DIAG-GC-STRESSはGC_DEBUG専用)。
#   make test-qemu QEMU_MEM=96M
# 下げすぎるとinit.lispのload前に落ちる。ヒープ先頭アドレスも動くので、
# os_heap_initのheap_base > MAGIC_MUST_BE_BELOW 検査も併せて効く。
QEMU_MEM ?= 256M

# [測定] カーネル本体へ渡す追加フラグ。未初期化スタック読み出しの追跡に使う:
#   make build EXTRA_CFLAGS=-ftrivial-auto-var-init=pattern
# スタックフレームをパターンで埋めるので、未初期化のまま読んだ値がそのパターンなら
# その場で分かる。レイアウトが変わっても**ゴミはゴミのまま**読まれるので、
# 「無関係な変更で症状が消える」形の問題が摂動で消えなくなる。
EXTRA_CFLAGS ?=

# 命令数ベースの性能計測(documents/performance-measurement.md参照)で使う
# TCGプラグインのビルドに必要な、ホストにインストール済みのqemu-system-x86_64
# バージョン(`qemu-system-x86_64 --version`で確認)。プラグインABI
# (QEMU_PLUGIN_VERSION)はヘッダのバージョンとホストのQEMU本体バージョンが
# 一致している必要があるため、`.envrc`等で実際のホスト側バージョンに
# 合わせて上書きすること
QEMU_VERSION_FOR_PLUGIN ?= 10.2.1

TARGET = esp_dir/EFI/BOOT/BOOTX64.EFI
SRCDIR = src/c
# トランスパイラ(transpileターゲット)の生成物。git管理対象外で、これらの
# 入力Lispファイルが変更された時だけ再生成される(下記$(LISP_COMPILED)ルール)。
# test/lisp/transpile_fixture.lispはトランスパイラの動作確認用フィクスチャ
# (テスト専用、global_environmentへは登録されない)で、本番のカーネルバイナリには
# 含めず$(LISP_COMPILED_FIXTURE)という別ファイルへコンパイルする(transpile.lispの
# main参照)
TRANSPILE_LISP_SRC = src/lisp/transpile.lisp test/lisp/transpile_fixture.lisp src/lisp/init_aot.lisp src/lisp/utility.lisp src/lisp/device.lisp src/lisp/ide.lisp src/lisp/partition.lisp src/lisp/mount.lisp src/lisp/file-node.lisp src/lisp/fat16.lisp src/lisp/fat32.lisp src/lisp/file-cmd.lisp src/lisp/bench_aot.lisp
LISP_COMPILED = $(SRCDIR)/lisp_compiled.c
LISP_COMPILED_FIXTURE = $(TESTDIR)/lisp_compiled_fixture.c
SRC = $(SRCDIR)/main.c $(SRCDIR)/kernel.c $(SRCDIR)/interrupt.c $(SRCDIR)/framebuffer.c $(SRCDIR)/process.c $(SRCDIR)/runtime.c $(SRCDIR)/lisp.c $(SRCDIR)/reader.c $(SRCDIR)/za.c $(SRCDIR)/disasm.c $(SRCDIR)/disasm_symtab.c $(SRCDIR)/disasm_symtab_lookup.c $(SRCDIR)/disasm_lisp.c $(SRCDIR)/eval.c $(SRCDIR)/print.c $(SRCDIR)/repl.c $(SRCDIR)/subprimitive.c $(SRCDIR)/drivers/pci.c $(SRCDIR)/drivers/virtio.c $(SRCDIR)/drivers/virtqueue.c $(SRCDIR)/drivers/ide.c $(SRCDIR)/block_device.c $(SRCDIR)/ide_subprimitive.c $(SRCDIR)/bench_subprimitive.c $(SRCDIR)/p9.c $(SRCDIR)/transport_virtio9p.c $(SRCDIR)/virtio9p.c $(SRCDIR)/stream.c $(SRCDIR)/stream_lisp.c $(SRCDIR)/mount.c $(SRCDIR)/format.c $(SRCDIR)/load.c $(SRCDIR)/clock.c $(LISP_COMPILED)
# $(TARGET) の依存には生成物(disasm_symtab.c)を含めない。含めると自分が作る
# ファイルに依存して毎回作り直しになる
SRC_NO_SYMTAB = $(filter-out $(SRCDIR)/disasm_symtab.c,$(SRC))
HDR = $(SRCDIR)/kernel.h $(SRCDIR)/interrupt.h $(SRCDIR)/framebuffer.h $(SRCDIR)/process.h $(SRCDIR)/version.h $(SRCDIR)/font8x16.h $(SRCDIR)/runtime.h $(SRCDIR)/lisp.h $(SRCDIR)/reader.h $(SRCDIR)/za.h $(SRCDIR)/za_jit_tags.h $(SRCDIR)/disasm.h $(SRCDIR)/disasm_lisp.h $(SRCDIR)/eval.h $(SRCDIR)/print.h $(SRCDIR)/repl.h $(SRCDIR)/subprimitive.h $(SRCDIR)/drivers/pci.h $(SRCDIR)/drivers/virtio.h $(SRCDIR)/drivers/virtqueue.h $(SRCDIR)/drivers/ide.h $(SRCDIR)/block_device.h $(SRCDIR)/ide_subprimitive.h $(SRCDIR)/bench_subprimitive.h $(SRCDIR)/p9.h $(SRCDIR)/p9_transport.h $(SRCDIR)/transport_virtio9p.h $(SRCDIR)/virtio9p.h $(SRCDIR)/stream.h $(SRCDIR)/stream_lisp.h $(SRCDIR)/mount.h $(SRCDIR)/format.h $(SRCDIR)/load.h $(SRCDIR)/clock.h

GIT_HASH := $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)
BUILD_DATE := $(shell date -u +"%Y-%m-%dT%H:%M:%SZ")

# [GCデバッグ] documents/pitfalls.md 原則4/原則6。GC_DEBUG=1でstale領域の
# staleポインタのデリファレンス検査を有効にする(%%DIAG-GC-STRESSで実行時に
# 強制GCの間隔も設定できる)。GC_PAINT=1を併用すると旧From空間をトラップパターンで
# 塗り潰すが、GC非管理の生データ(os_stream_t等)を壊すため既定では無効。
# 通常ビルドでは空なので影響しない
GC_DEBUG_FLAGS = $(if $(GC_DEBUG),-DISIKIOS_GC_DEBUG,)$(if $(GC_PAINT), -DISIKIOS_GC_PAINT,)

# [調査] 16byte境界監査(documents/alignment-survey-report.md)。ALIGN_AUDIT=1 の
# ときだけ各アロケータとgc_copy_valueに計数を入れ、QEMU試験なら電源断の直前に、
# ネイティブのユニットテストならatexitでヒストグラムを出す。
# 既定では空なので通常ビルドの挙動・性能は変わらない(runtime.hのマクロが
# ((void)0)に畳まれる)
ALIGN_AUDIT_FLAGS = $(if $(ALIGN_AUDIT),-DISIKIOS_ALIGN_AUDIT,)

# [検証] JIT_DUMP=1 で、JITがコンパイルした関数ごとに
# 「出力サイズ・マスクしたバイト数・バイト列のハッシュ」をシリアルへ出す。
# タグ定数の整理が出力を変えていないことを確かめるための計測専用フラグで、
# 既定では空なので通常ビルドには1命令も入らない
# (documents/jit-tag-constants.md Step 1)
JIT_DUMP_FLAGS = $(if $(JIT_DUMP),-DISIKIOS_JIT_DUMP,)

BUILD_TMPDIR = tmp

# [GCデバッグ] $(TARGET)はSRC/HDRのファイル依存で追跡するため、**フラグだけ変えても
# 再ビルドされない**。以前はrm -f $(TARGET)を手で打つ運用にしていたが、忘れると
# 「塗り潰し有効のつもりで無効のバイナリを測る」ことになり、しかも無言で成立する
# (documents/pitfalls.md 原則6)。使用したフラグをスタンプに残し、変わったときだけ
# スタンプを更新して$(TARGET)の再ビルドを促す
#
# EXTRA_CFLAGSもスタンプに含める。これは「計測のための一時的なフラグ」を渡す口で、
# まさに**同じセッション中に付けたり外したりする**使われ方をする。含めていなかった
# ために実際に踏んだ(2026-09-17、16byte境界化の前後比較):
#   make ... EXTRA_CFLAGS=-DOS_HEAP_ALIGN=8ULL   # 比較用のバイナリを作る
#   make ...                                     # 戻したつもり
# 2回目はSRC/HDRが更新されていないので再ビルドされず、**8byte版のバイナリのまま
# 測って heap-align=8 と報告された**。ALIGN_AUDITが使用値を出力に混ぜていたので
# 気づけたが、出していなければそのまま誤った比較表になっていた。
GC_DEBUG_STAMP = $(BUILD_TMPDIR)/.gc-debug-flags
BUILD_FLAG_SIGNATURE = $(GC_DEBUG_FLAGS) $(ALIGN_AUDIT_FLAGS) $(JIT_DUMP_FLAGS) $(EXTRA_CFLAGS)
.PHONY: FORCE
FORCE:
$(GC_DEBUG_STAMP): FORCE
	@mkdir -p $(BUILD_TMPDIR)
	@echo '$(BUILD_FLAG_SIGNATURE)' | cmp -s - $@ 2>/dev/null || echo '$(BUILD_FLAG_SIGNATURE)' > $@
OBJ = $(patsubst $(SRCDIR)/%.c,$(BUILD_TMPDIR)/%.o,$(SRC))

# fat16_test.img/fat32_test.imgのような固定テストフィクスチャはtmp/に置くが、
# ブート用イメージ(boot_fat32.img)はビルド成果物なのでimages/に分けて置く
IMAGES_DIR = images

TESTDIR = test/c
TEST_COMMON_SRC = $(SRCDIR)/runtime.c $(SRCDIR)/lisp.c

TEST_SRC_RUNTIME = $(TEST_COMMON_SRC) $(SRCDIR)/process.c $(SRCDIR)/za.c $(SRCDIR)/eval.c $(SRCDIR)/reader.c $(SRCDIR)/stream.c $(SRCDIR)/mount.c $(TESTDIR)/runtime_test.c
TEST_BIN_RUNTIME = $(BUILD_TMPDIR)/runtime_test

TEST_SRC_LISP = $(TEST_COMMON_SRC) $(SRCDIR)/process.c $(SRCDIR)/za.c $(SRCDIR)/eval.c $(SRCDIR)/reader.c $(SRCDIR)/stream.c $(SRCDIR)/mount.c $(TESTDIR)/lisp_test.c
TEST_BIN_LISP = $(BUILD_TMPDIR)/lisp_test

TEST_SRC_PROCESS = $(TEST_COMMON_SRC) $(SRCDIR)/process.c $(SRCDIR)/za.c $(SRCDIR)/eval.c $(SRCDIR)/reader.c $(SRCDIR)/stream.c $(SRCDIR)/mount.c $(TESTDIR)/process_test.c
TEST_BIN_PROCESS = $(BUILD_TMPDIR)/process_test

TEST_SRC_READER = $(TEST_COMMON_SRC) $(SRCDIR)/process.c $(SRCDIR)/reader.c $(SRCDIR)/stream.c $(SRCDIR)/mount.c $(SRCDIR)/za.c $(SRCDIR)/eval.c $(TESTDIR)/reader_test.c
TEST_BIN_READER = $(BUILD_TMPDIR)/reader_test

TEST_SRC_EVAL = $(TEST_COMMON_SRC) $(SRCDIR)/process.c $(SRCDIR)/za.c $(SRCDIR)/eval.c $(SRCDIR)/reader.c $(SRCDIR)/stream.c $(SRCDIR)/mount.c $(TESTDIR)/eval_test.c
TEST_BIN_EVAL = $(BUILD_TMPDIR)/eval_test

TEST_SRC_PRINT = $(TEST_COMMON_SRC) $(SRCDIR)/process.c $(SRCDIR)/print.c $(SRCDIR)/za.c $(SRCDIR)/eval.c $(SRCDIR)/reader.c $(SRCDIR)/stream.c $(SRCDIR)/mount.c $(TESTDIR)/print_test.c
TEST_BIN_PRINT = $(BUILD_TMPDIR)/print_test

TEST_SRC_REPL = $(TEST_COMMON_SRC) $(SRCDIR)/process.c $(SRCDIR)/reader.c $(SRCDIR)/stream.c $(SRCDIR)/mount.c $(SRCDIR)/za.c $(SRCDIR)/eval.c $(SRCDIR)/print.c $(SRCDIR)/repl.c $(TESTDIR)/repl_test.c
TEST_BIN_REPL = $(BUILD_TMPDIR)/repl_test

TEST_SRC_SUBPRIMITIVE = $(TEST_COMMON_SRC) $(SRCDIR)/process.c $(SRCDIR)/subprimitive.c $(SRCDIR)/za.c $(SRCDIR)/eval.c $(SRCDIR)/reader.c $(SRCDIR)/stream.c $(SRCDIR)/mount.c $(TESTDIR)/subprimitive_test.c
TEST_BIN_SUBPRIMITIVE = $(BUILD_TMPDIR)/subprimitive_test

TEST_SRC_SCRIPT = $(TEST_COMMON_SRC) $(SRCDIR)/process.c $(SRCDIR)/reader.c $(SRCDIR)/stream.c $(SRCDIR)/mount.c $(SRCDIR)/stream_lisp.c $(SRCDIR)/za.c $(SRCDIR)/disasm.c $(SRCDIR)/disasm_symtab_lookup.c $(SRCDIR)/disasm_lisp.c $(SRCDIR)/eval.c $(SRCDIR)/print.c $(SRCDIR)/format.c $(SRCDIR)/subprimitive.c $(SRCDIR)/drivers/ide.c $(SRCDIR)/block_device.c $(SRCDIR)/ide_subprimitive.c $(LISP_COMPILED) $(TESTDIR)/script_test.c
TEST_BIN_SCRIPT = $(BUILD_TMPDIR)/script_test

TEST_SRC_STREAM = $(SRCDIR)/stream.c $(TESTDIR)/stream_test.c
TEST_BIN_STREAM = $(BUILD_TMPDIR)/stream_test

TEST_SRC_LOAD = $(TEST_COMMON_SRC) $(SRCDIR)/process.c $(SRCDIR)/za.c $(SRCDIR)/eval.c $(SRCDIR)/reader.c $(SRCDIR)/stream.c $(SRCDIR)/mount.c $(SRCDIR)/load.c $(TESTDIR)/load_test.c
TEST_BIN_LOAD = $(BUILD_TMPDIR)/load_test

TEST_SRC_STREAM_LISP = $(TEST_COMMON_SRC) $(SRCDIR)/process.c $(SRCDIR)/reader.c $(SRCDIR)/stream.c $(SRCDIR)/mount.c $(SRCDIR)/stream_lisp.c $(SRCDIR)/za.c $(SRCDIR)/eval.c $(TESTDIR)/stream_lisp_test.c
TEST_BIN_STREAM_LISP = $(BUILD_TMPDIR)/stream_lisp_test

TEST_SRC_FORMAT = $(TEST_COMMON_SRC) $(SRCDIR)/process.c $(SRCDIR)/reader.c $(SRCDIR)/stream.c $(SRCDIR)/mount.c $(SRCDIR)/stream_lisp.c $(SRCDIR)/print.c $(SRCDIR)/format.c $(SRCDIR)/za.c $(SRCDIR)/eval.c $(TESTDIR)/format_test.c
TEST_BIN_FORMAT = $(BUILD_TMPDIR)/format_test

# interrupt.c/kernel.cはリンクせず、clock_test.cがget_tick_counter/
# kernel_get_boot_epoch_secondsをテスト用固定値で置き換える
TEST_SRC_CLOCK = $(TEST_COMMON_SRC) $(SRCDIR)/process.c $(SRCDIR)/clock.c $(SRCDIR)/za.c $(SRCDIR)/eval.c $(SRCDIR)/reader.c $(SRCDIR)/stream.c $(SRCDIR)/mount.c $(TESTDIR)/clock_test.c
TEST_BIN_CLOCK = $(BUILD_TMPDIR)/clock_test

TEST_SRC_P9 = $(SRCDIR)/p9.c $(TESTDIR)/p9_test.c
TEST_BIN_P9 = $(BUILD_TMPDIR)/p9_test

# virtio9p_test.cが独自にos_transport_virtio9p_instance()をfake定義するため、
# 本物のtransport_virtio9p.cはリンクしない
TEST_SRC_VIRTIO9P = $(SRCDIR)/p9.c $(SRCDIR)/virtio9p.c $(TESTDIR)/virtio9p_test.c
TEST_BIN_VIRTIO9P = $(BUILD_TMPDIR)/virtio9p_test

TEST_SRC_LISP_COMPILED = $(TEST_COMMON_SRC) $(SRCDIR)/process.c $(SRCDIR)/za.c $(SRCDIR)/eval.c $(SRCDIR)/reader.c $(SRCDIR)/stream.c $(SRCDIR)/mount.c $(SRCDIR)/stream_lisp.c $(SRCDIR)/format.c $(SRCDIR)/print.c $(SRCDIR)/subprimitive.c $(SRCDIR)/drivers/ide.c $(SRCDIR)/block_device.c $(SRCDIR)/ide_subprimitive.c $(LISP_COMPILED) $(LISP_COMPILED_FIXTURE) $(TESTDIR)/lisp_compiled_test.c
TEST_BIN_LISP_COMPILED = $(BUILD_TMPDIR)/lisp_compiled_test

# interrupt.cはリンクせず、ide_test.cが独自にinb/outb/inw/outwをfake定義して
# レジスタ操作の順序・値を検証する(subprimitive_test.cと同じ方針)
TEST_SRC_IDE = $(TEST_COMMON_SRC) $(SRCDIR)/process.c $(SRCDIR)/za.c $(SRCDIR)/eval.c $(SRCDIR)/reader.c $(SRCDIR)/stream.c $(SRCDIR)/mount.c $(SRCDIR)/drivers/ide.c $(SRCDIR)/block_device.c $(SRCDIR)/ide_subprimitive.c $(TESTDIR)/ide_test.c
TEST_BIN_IDE = $(BUILD_TMPDIR)/ide_test

# framebuffer.cもランタイムに依存しない(contentの確保は呼び出し側の責任)ので
# 単体でリンクできる。履歴リングと表示窓の境界条件を検証する
TEST_SRC_FRAMEBUFFER = $(SRCDIR)/framebuffer.c $(TESTDIR)/framebuffer_test.c
TEST_BIN_FRAMEBUFFER = $(BUILD_TMPDIR)/framebuffer_test

# disasm.cはランタイムに依存しない純粋なデコーダ(Lispからの入口はdisasm_lisp.c側)
# なので、stream_testと同じく単体でリンクしてバイト列を直接食わせて検証できる
TEST_SRC_DISASM = $(SRCDIR)/disasm.c $(TESTDIR)/disasm_test.c
TEST_BIN_DISASM = $(BUILD_TMPDIR)/disasm_test

TEST_SRC_MOUNT = $(TEST_COMMON_SRC) $(SRCDIR)/process.c $(SRCDIR)/za.c $(SRCDIR)/eval.c $(SRCDIR)/reader.c $(SRCDIR)/stream.c $(SRCDIR)/mount.c $(TESTDIR)/mount_test.c
TEST_BIN_MOUNT = $(BUILD_TMPDIR)/mount_test


.PHONY: all setup image transpile build compile run test test-qemu test-qemu-all clean

all: build


image:
	docker build --build-arg USER_UID="$$(id -u)" --build-arg USER_GID="$$(id -g)" -t isiki-builder .

# ros(roswell)はイメージのENTRYPOINTが/usr/local/bin/sbclのため、
# --entrypoint bashで明示的に上書きしないと引数がsbclへ直接渡ってしまう。
# imageターゲットでホストのUID/GIDと同じbuilderユーザーを作成し、
# そのユーザーにroswellをインストール済みのため、--userを指定しても
# $HOMEが正しく解決され動作する。$(TRANSPILE_LISP_SRC)への依存により、
# トランスパイラ自体または入力Lispファイルが変更された時だけ再生成される
# (以前はphonyなtranspileへの無条件依存だったため、buildをファイル依存化しても
# lisp_compiled.cが毎回再生成されmtimeが更新され続け、結局$(TARGET)/
# BOOT_FAT32_IMGが常に再ビルドされてしまっていた)。
$(LISP_COMPILED): $(TRANSPILE_LISP_SRC)
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint bash -v "$(PWD)":/workspace isiki-builder \
		-c 'ros run --load src/lisp/transpile.lisp --eval "(main)" --quit'

# (main)は$(LISP_COMPILED)と$(LISP_COMPILED_FIXTURE)を1回の実行で両方生成するが、
# 独立したルールにしているため、両方が古い状態から`make test`のように両方を
# 要求するターゲットを叩くと(main)が2回走ることがある(生成内容は毎回supersedeで
# 上書きするため冪等、無駄ではあるが壊れはしない)。GNU Make 4.3+のグループ
# ターゲット(&:)を使えば1回にできるが、古いmake(例: macOS標準の3.81)との
# 互換性を優先しここでは使わない
$(LISP_COMPILED_FIXTURE): $(TRANSPILE_LISP_SRC)
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint bash -v "$(PWD)":/workspace isiki-builder \
		-c 'ros run --load src/lisp/transpile.lisp --eval "(main)" --quit'

transpile: $(LISP_COMPILED) $(LISP_COMPILED_FIXTURE)

# $(TARGET)をファイル依存(SRC/HDR)で追跡することで、BOOT_FAT32_IMG(後述)が
# 「実際にソース/ヘッダが変更された時だけ」再生成されるようにする土台になる
# (buildをphonyのままにしていると、buildを経由するあらゆる後続ターゲットが
# 常に再実行されてしまう)。
# [シンボル解決] 逆アセンブル用のシンボル表は**2 パス**で作る
# (documents/disasm-symbols.md §3)。
#
#   1. 空のテーブルでリンク      → アドレスが確定する
#   2. nm で抽出してテーブル生成
#   3. テーブルを含めて再リンク
#   4. **再抽出して 1 と照合**   → ずれていたらビルドを止める
#
# 実測では .rdata が 80KB 増えても .text のシンボルは 1 つも動かなかった
# (.text が最初のセクションで、テーブルは .rdata へ載るため)。
# それでも 4 を入れてあるのは、**将来リンカの挙動が変わったときに、
# 黙って古いテーブルを積んだイメージができるのを防ぐ**ためである。
DISASM_SYMTAB = $(SRCDIR)/disasm_symtab.c
KERNEL_LINK = docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint x86_64-w64-mingw32-gcc -v "$(PWD)":/workspace isiki-builder \
		-nostdlib -mno-red-zone -O1 -shared \
		-mno-stack-arg-probe \
		-DISIKIOS_BUILD_HASH=\"$(GIT_HASH)\" \
		-DISIKIOS_BUILD_DATE=\"$(BUILD_DATE)\" \
		$(GC_DEBUG_FLAGS) $(ALIGN_AUDIT_FLAGS) $(JIT_DUMP_FLAGS) $(EXTRA_CFLAGS) \
		-Wl,--subsystem,10 \
		-Wl,--entry,EfiMain \
		-o $(TARGET) $(SRC)
NM_IN_DOCKER = docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint x86_64-w64-mingw32-nm -v "$(PWD)":/workspace isiki-builder
OBJDUMP_IN_DOCKER = docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint x86_64-w64-mingw32-objdump -v "$(PWD)":/workspace isiki-builder

$(TARGET): $(SRC_NO_SYMTAB) $(HDR) $(GC_DEBUG_STAMP)
	mkdir -p esp_dir/EFI/BOOT $(BUILD_TMPDIR)
	@echo '/* placeholder */' > $(DISASM_SYMTAB)
	@printf '#include "runtime.h"\n#include "disasm_symtab.h"\n' >> $(DISASM_SYMTAB)
	@printf 'const UINT64 g_disasm_link_image_base = 0;\nconst UINT64 g_disasm_sym_count = 0;\n' >> $(DISASM_SYMTAB)
	@printf 'const UINT64 g_disasm_sym_addr[1] = {0};\nconst UINT32 g_disasm_sym_name_off[1] = {0};\nconst char g_disasm_sym_names[1] = {0};\n' >> $(DISASM_SYMTAB)
	$(KERNEL_LINK)
	@$(NM_IN_DOCKER) $(TARGET) > $(BUILD_TMPDIR)/nm_pass1.txt
	@$(OBJDUMP_IN_DOCKER) -p $(TARGET) | awk '/^ImageBase/ { print $$2 }' > $(BUILD_TMPDIR)/imagebase.txt
	@sh tools/gen_disasm_symtab.sh $(BUILD_TMPDIR)/nm_pass1.txt "$$(cat $(BUILD_TMPDIR)/imagebase.txt)" > $(DISASM_SYMTAB)
	$(KERNEL_LINK)
	@$(NM_IN_DOCKER) $(TARGET) > $(BUILD_TMPDIR)/nm_pass2.txt
	@sh tools/check_disasm_symtab.sh $(BUILD_TMPDIR)/nm_pass1.txt $(BUILD_TMPDIR)/nm_pass2.txt

# [測定の落とし穴] `build`は以前 $(TARGET)(= esp_dir/.../BOOTX64.EFI)だけを作って
# いた。QEMUが実際に起動するのは $(BOOT_FAT32_IMG) の中にコピーされたEFIなので、
# `make build` の後に手でqemuを起動すると**古いカーネルを測ってしまう**。
# しかもEFIとイメージのmtimeが同じ秒に収まると make は「最新」と判断して
# 作り直さないため、`make images/boot_fat32.img` を明示しても取りこぼす。
# 実測で、cc_carインライン化の可否を2回とも誤判定しかけた。
# buildにイメージまで含めて、`make build`だけで起動可能な状態が揃うようにする。
build: $(TARGET)

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
		$(GC_DEBUG_FLAGS) $(ALIGN_AUDIT_FLAGS) $(JIT_DUMP_FLAGS) \
		-DISIKIOS_BUILD_HASH=\"$(GIT_HASH)\" \
		-DISIKIOS_BUILD_DATE=\"$(BUILD_DATE)\" \
		-o $@ $<

# ネイティブgccでビルドし、そのままコンテナ内で実行するユニットテスト
test: $(TEST_SRC_RUNTIME) $(TEST_SRC_LISP) $(TEST_SRC_PROCESS) $(TEST_SRC_READER) $(TEST_SRC_EVAL) $(TEST_SRC_PRINT) $(TEST_SRC_REPL) $(TEST_SRC_SUBPRIMITIVE) $(TEST_SRC_SCRIPT) $(TEST_SRC_STREAM) $(TEST_SRC_LOAD) $(TEST_SRC_STREAM_LISP) $(TEST_SRC_FORMAT) $(TEST_SRC_P9) $(TEST_SRC_VIRTIO9P) $(TEST_SRC_CLOCK) $(TEST_SRC_LISP_COMPILED) $(TEST_SRC_IDE) $(TEST_SRC_MOUNT) $(TEST_SRC_DISASM) $(TEST_SRC_FRAMEBUFFER) $(HDR) | $(BUILD_TMPDIR)
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint gcc -v "$(PWD)":/workspace isiki-builder \
		-std=c11 -Wall -Wextra \
		-DISIKIOS_UNIT_TEST $(ALIGN_AUDIT_FLAGS) \
		-I$(SRCDIR) \
		-o $(TEST_BIN_RUNTIME) $(TEST_SRC_RUNTIME) -lm
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint gcc -v "$(PWD)":/workspace isiki-builder \
		-std=c11 -Wall -Wextra \
		-DISIKIOS_UNIT_TEST $(ALIGN_AUDIT_FLAGS) \
		-I$(SRCDIR) \
		-o $(TEST_BIN_LISP) $(TEST_SRC_LISP) -lm
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint gcc -v "$(PWD)":/workspace isiki-builder \
		-std=c11 -Wall -Wextra \
		-DISIKIOS_UNIT_TEST $(ALIGN_AUDIT_FLAGS) \
		-I$(SRCDIR) \
		-o $(TEST_BIN_PROCESS) $(TEST_SRC_PROCESS) -lm
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint gcc -v "$(PWD)":/workspace isiki-builder \
		-std=c11 -Wall -Wextra \
		-DISIKIOS_UNIT_TEST $(ALIGN_AUDIT_FLAGS) \
		-I$(SRCDIR) \
		-o $(TEST_BIN_READER) $(TEST_SRC_READER) -lm
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint gcc -v "$(PWD)":/workspace isiki-builder \
		-std=c11 -Wall -Wextra \
		-DISIKIOS_UNIT_TEST $(ALIGN_AUDIT_FLAGS) \
		-I$(SRCDIR) \
		-o $(TEST_BIN_EVAL) $(TEST_SRC_EVAL) -lm
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint gcc -v "$(PWD)":/workspace isiki-builder \
		-std=c11 -Wall -Wextra \
		-DISIKIOS_UNIT_TEST $(ALIGN_AUDIT_FLAGS) \
		-I$(SRCDIR) \
		-o $(TEST_BIN_PRINT) $(TEST_SRC_PRINT) -lm
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint gcc -v "$(PWD)":/workspace isiki-builder \
		-std=c11 -Wall -Wextra \
		-DISIKIOS_UNIT_TEST $(ALIGN_AUDIT_FLAGS) \
		-I$(SRCDIR) \
		-o $(TEST_BIN_REPL) $(TEST_SRC_REPL) -lm
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint gcc -v "$(PWD)":/workspace isiki-builder \
		-std=c11 -Wall -Wextra \
		-DISIKIOS_UNIT_TEST $(ALIGN_AUDIT_FLAGS) \
		-I$(SRCDIR) \
		-o $(TEST_BIN_SUBPRIMITIVE) $(TEST_SRC_SUBPRIMITIVE) -lm
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint gcc -v "$(PWD)":/workspace isiki-builder \
		-std=c11 -Wall -Wextra \
		-DISIKIOS_UNIT_TEST $(ALIGN_AUDIT_FLAGS) \
		-I$(SRCDIR) \
		-o $(TEST_BIN_SCRIPT) $(TEST_SRC_SCRIPT) -lm
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint gcc -v "$(PWD)":/workspace isiki-builder \
		-std=c11 -Wall -Wextra \
		-DISIKIOS_UNIT_TEST $(ALIGN_AUDIT_FLAGS) \
		-I$(SRCDIR) \
		-o $(TEST_BIN_STREAM) $(TEST_SRC_STREAM)
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint gcc -v "$(PWD)":/workspace isiki-builder \
		-std=c11 -Wall -Wextra \
		-DISIKIOS_UNIT_TEST $(ALIGN_AUDIT_FLAGS) \
		-I$(SRCDIR) \
		-o $(TEST_BIN_LOAD) $(TEST_SRC_LOAD) -lm
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint gcc -v "$(PWD)":/workspace isiki-builder \
		-std=c11 -Wall -Wextra \
		-DISIKIOS_UNIT_TEST $(ALIGN_AUDIT_FLAGS) \
		-I$(SRCDIR) \
		-o $(TEST_BIN_STREAM_LISP) $(TEST_SRC_STREAM_LISP) -lm
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint gcc -v "$(PWD)":/workspace isiki-builder \
		-std=c11 -Wall -Wextra \
		-DISIKIOS_UNIT_TEST $(ALIGN_AUDIT_FLAGS) \
		-I$(SRCDIR) \
		-o $(TEST_BIN_FORMAT) $(TEST_SRC_FORMAT) -lm
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint gcc -v "$(PWD)":/workspace isiki-builder \
		-std=c11 -Wall -Wextra \
		-DISIKIOS_UNIT_TEST $(ALIGN_AUDIT_FLAGS) \
		-I$(SRCDIR) \
		-o $(TEST_BIN_CLOCK) $(TEST_SRC_CLOCK) -lm
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint gcc -v "$(PWD)":/workspace isiki-builder \
		-std=c11 -Wall -Wextra \
		-DISIKIOS_UNIT_TEST $(ALIGN_AUDIT_FLAGS) \
		-I$(SRCDIR) \
		-o $(TEST_BIN_P9) $(TEST_SRC_P9)
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint gcc -v "$(PWD)":/workspace isiki-builder \
		-std=c11 -Wall -Wextra \
		-DISIKIOS_UNIT_TEST $(ALIGN_AUDIT_FLAGS) \
		-I$(SRCDIR) \
		-o $(TEST_BIN_VIRTIO9P) $(TEST_SRC_VIRTIO9P)
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint gcc -v "$(PWD)":/workspace isiki-builder \
		-std=c11 -Wall -Wextra \
		-DISIKIOS_UNIT_TEST $(ALIGN_AUDIT_FLAGS) \
		-I$(SRCDIR) \
		-o $(TEST_BIN_LISP_COMPILED) $(TEST_SRC_LISP_COMPILED) -lm
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint gcc -v "$(PWD)":/workspace isiki-builder \
		-std=c11 -Wall -Wextra \
		-DISIKIOS_UNIT_TEST $(ALIGN_AUDIT_FLAGS) \
		-I$(SRCDIR) \
		-o $(TEST_BIN_IDE) $(TEST_SRC_IDE) -lm
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint gcc -v "$(PWD)":/workspace isiki-builder \
		-std=c11 -Wall -Wextra \
		-DISIKIOS_UNIT_TEST $(ALIGN_AUDIT_FLAGS) \
		-I$(SRCDIR) \
		-o $(TEST_BIN_MOUNT) $(TEST_SRC_MOUNT) -lm
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint gcc -v "$(PWD)":/workspace isiki-builder \
		-std=c11 -Wall -Wextra \
		-DISIKIOS_UNIT_TEST \
		-I$(SRCDIR) \
		-o $(TEST_BIN_DISASM) $(TEST_SRC_DISASM)
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint gcc -v "$(PWD)":/workspace isiki-builder \
		-std=c11 -Wall -Wextra \
		-DISIKIOS_UNIT_TEST \
		-I$(SRCDIR) \
		-o $(TEST_BIN_FRAMEBUFFER) $(TEST_SRC_FRAMEBUFFER)
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
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint /workspace/$(TEST_BIN_MOUNT) -v "$(PWD)":/workspace isiki-builder
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint /workspace/$(TEST_BIN_DISASM) -v "$(PWD)":/workspace isiki-builder
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint /workspace/$(TEST_BIN_FRAMEBUFFER) -v "$(PWD)":/workspace isiki-builder

clean:
	rm -rf esp_dir $(BUILD_TMPDIR) $(IMAGES_DIR)
	rm -f .qemu-test-trigger test-results.txt qemu.log $(LISP_COMPILED) $(LISP_COMPILED_FIXTURE)

# IDE/ATA PIO動作確認用の使い捨てディスクイメージ。qemu-imgはCI環境で未保証の
# パッケージのため使わず、dd/printfのみで作成する。先頭にIDE_TEST_MAGICを書き込み、
# read-sectorでのセクタ0読み込み確認に使う。Secondaryチャネル(bus=1,unit=0)の
# masterとして明示的にアタッチする(Primaryチャネルは起動用のBOOT_FAT32_IMG
# (images/boot_fat32.img)が占有する)
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

# FAT32読み書き実装用の使い捨てテストイメージ。FAT16_DISK_IMGと同じ構成・同じ理由
# (loopマウントでの内容配置、DELETED.TXTを最後に作成/削除して0xE5マーカーを作る、
# --privilegedでのみdocker runする)だが、以下の点がFAT16と異なる:
#   - イメージサイズは40MB(16MBだとmkfs.vfat -F 32が「クラスタ数が
#     推奨最小値未満」というwarningを出すため、実測で確認した安全マージン)。
#   - このフォーマットではsectors-per-cluster=1(512byte/cluster、FAT16は4=2048byte)
#     になるため、512byteを超えるファイルは自動的に複数クラスタへ分割される。
#     BIG.TXTは1000byte("0123456789"を100回)で3クラスタに、WRITE1.TXTは
#     512byte("A"を512回)でちょうど1クラスタになるようサイズを合わせている。
#   - FATエントリが4byte/entry(FAT16は2byte)のため、後述の合成チェイン用オフセット
#     計算がcluster-no*4になる。
#
# umount後、ホスト側でFATテーブル(1本目、reserved-sectors=32なのでバイトオフセット
# 32*512=16384から開始)にクラスタ40→41→44→終端という非連続なチェインを直接
# dd/printfで書き込む(fat32-cluster-chain確認用)。クラスタ40/41/44はmkfs.vfat後は
# 未使用で、TEST.LSP/BIG.TXT/WRITE1.TXT/SUBDIR以下が使う実クラスタとは十分な余裕を
# 持って重複しない(xxd -g1で目視確認済み)。オフセットは16384 + cluster-no*4で計算
# した固定値(16544/16548/16560)、値はLE u32(41=0x00000029/44=0x0000002C/
# 終端=0x0FFFFFFF)。FAT16と同じ理由でエスケープは\xHHではなく\0NNN(8進)を使う。
FAT32_DISK_IMG = $(BUILD_TMPDIR)/fat32_test.img
FAT32_TEST_STRING = Hello from FAT32!

$(FAT32_DISK_IMG): | $(BUILD_TMPDIR)
	dd if=/dev/zero of=$@ bs=1M count=40 2>/dev/null
	docker run --rm --privileged --entrypoint bash -v "$(PWD)":/workspace -w /workspace isiki-builder \
		-c 'set -e; \
			mkfs.vfat -F 32 $@; \
			mkdir -p /mnt/fat32_test; \
			mount -o loop $@ /mnt/fat32_test; \
			touch /mnt/fat32_test/HELLO.TXT; \
			printf "%s\n" "$(FAT32_TEST_STRING)" > /mnt/fat32_test/TEST.LSP; \
			printf "0123456789%.0s" {1..100} > /mnt/fat32_test/BIG.TXT; \
			printf "A%.0s" {1..512} > /mnt/fat32_test/WRITE1.TXT; \
			printf "long name content" > /mnt/fat32_test/Long_File_Name.txt; \
			printf "x" > /mnt/fat32_test/This_Is_A_Very_Long_File_Name.txt; \
			mkdir /mnt/fat32_test/SUBDIR; \
			printf "nested file content" > /mnt/fat32_test/SUBDIR/NESTED.TXT; \
			mkdir /mnt/fat32_test/SUBDIR/DEEPER; \
			touch /mnt/fat32_test/SUBDIR/DEEPER/DEEP.TXT; \
			printf "will be deleted" > /mnt/fat32_test/DELETED.TXT; \
			rm /mnt/fat32_test/DELETED.TXT; \
			umount /mnt/fat32_test'
	printf '\051\000\000\000' | dd of=$@ bs=1 seek=16544 count=4 conv=notrunc 2>/dev/null
	printf '\054\000\000\000' | dd of=$@ bs=1 seek=16548 count=4 conv=notrunc 2>/dev/null
	printf '\377\377\377\017' | dd of=$@ bs=1 seek=16560 count=4 conv=notrunc 2>/dev/null

# src/lisp/partition.lisp(PART-M2)確認用、GPTで2パーティション(FAT16+FAT32)に
# 分割したディスクイメージ。パーティション1(FAT16、16MiB、開始セクタ2048=1MiB境界、
# FAT16_DISK_IMGと同じサイズ。8MiBではmkfs.vfat -F 16が総セクタ数の境界条件によって
# 「too small or too large filesystem」を不安定に報告することを実機確認したため避けた)・
# パーティション2(FAT32、40MiB、開始セクタ34816=17MiB境界、mkfs.vfat -F 32が
# 「クラスタ数が推奨最小値未満」を警告しない安全マージン、FAT32_DISK_IMGと同じ理由)。
# 各パーティションにTEST.LSPを1つだけ置き、%device-fat16-uuid/%device-fat32-uuidが
# パーティションハンドル経由でも機能することの確認に使う(ファイル読み込み自体の
# 確認はPART-M4でimages/boot_fat32.imgのESPを使う)。type GUIDはpartition.lispの
# GPT登録ロジックが特定のtypeでフィルタしない(全ゼロ=未使用スロットのみ判定)ため
# 汎用のLinux filesystem(8300)のままで良い。BOOT_FAT32_IMGと同様、sgdisk/losetupに
# CAP_SYS_ADMIN相当の権限を要するため--privilegedでdocker runする。
GPT_MULTI_DISK_IMG = $(BUILD_TMPDIR)/gpt_multi_test.img
GPT_MULTI_IMG_SIZE_MB = 64
GPT_MULTI_PART1_START_SECTOR = 2048
GPT_MULTI_PART2_START_SECTOR = 34816

$(GPT_MULTI_DISK_IMG): | $(BUILD_TMPDIR)
	dd if=/dev/zero of=$@ bs=1M count=$(GPT_MULTI_IMG_SIZE_MB) 2>/dev/null
	docker run --rm --privileged --entrypoint bash -v "$(PWD)":/workspace -w /workspace isiki-builder \
		-c 'set -e; \
			sgdisk -o $@; \
			sgdisk -n 1:$(GPT_MULTI_PART1_START_SECTOR):+16M -t 1:8300 -c 1:"ISIKI-P1" $@; \
			sgdisk -n 2:$(GPT_MULTI_PART2_START_SECTOR):+40M -t 2:8300 -c 2:"ISIKI-P2" $@; \
			LOOPDEV1=$$(losetup -o $$(($(GPT_MULTI_PART1_START_SECTOR)*512)) --sizelimit $$((16*1024*1024)) -f --show $@); \
			mkfs.vfat -F 16 $$LOOPDEV1; \
			mkdir -p /mnt/gpt_multi_p1; \
			mount $$LOOPDEV1 /mnt/gpt_multi_p1; \
			printf "%s\n" "Hello from GPT partition 1 (FAT16)!" > /mnt/gpt_multi_p1/TEST.LSP; \
			umount /mnt/gpt_multi_p1; \
			losetup -d $$LOOPDEV1; \
			LOOPDEV2=$$(losetup -o $$(($(GPT_MULTI_PART2_START_SECTOR)*512)) --sizelimit $$((40*1024*1024)) -f --show $@); \
			mkfs.vfat -F 32 $$LOOPDEV2; \
			mkdir -p /mnt/gpt_multi_p2; \
			mount $$LOOPDEV2 /mnt/gpt_multi_p2; \
			printf "%s\n" "Hello from GPT partition 2 (FAT32)!" > /mnt/gpt_multi_p2/TEST.LSP; \
			umount /mnt/gpt_multi_p2; \
			losetup -d $$LOOPDEV2'

# src/lisp/partition.lisp(PART-M3)確認用、レガシーMBR(DOSパーティションテーブル)で
# 2パーティション(FAT16+FAT32)に分割したディスクイメージ。サイズ・開始セクタ・
# ファイルシステムはGPT_MULTI_DISK_IMGと完全に同じレイアウトにして、GPT版・MBR版で
# パーティション検出以外の差異が出ないようにする。sgdisk(GPTのみ対応)ではなく
# sfdisk(util-linuxのfdiskパッケージ、Dockerfileへ追加)のスクリプト入力形式で
# MBRパーティションテーブルを作る。typeはFAT16(6)/W95 FAT32 LBA(c)を指定するが、
# partition.lispのMBR登録ロジックはtypeを0x00(未使用)/0xEE(GPTプロテクティブ)/
# 0x05・0x0F(拡張、スコープ外)以外は無条件に登録するため、typeの値自体は
# 検出結果に影響しない。losetup/mountはCAP_SYS_ADMIN相当の権限を要するため
# --privilegedでdocker runする。
MBR_MULTI_DISK_IMG = $(BUILD_TMPDIR)/mbr_multi_test.img
MBR_MULTI_IMG_SIZE_MB = 64
MBR_MULTI_PART1_START_SECTOR = 2048
MBR_MULTI_PART2_START_SECTOR = 34816

$(MBR_MULTI_DISK_IMG): | $(BUILD_TMPDIR)
	dd if=/dev/zero of=$@ bs=1M count=$(MBR_MULTI_IMG_SIZE_MB) 2>/dev/null
	docker run --rm --privileged --entrypoint bash -v "$(PWD)":/workspace -w /workspace isiki-builder \
		-c 'set -e; \
			printf "label: dos\nunit: sectors\nstart=$(MBR_MULTI_PART1_START_SECTOR), size=32768, type=6\nstart=$(MBR_MULTI_PART2_START_SECTOR), size=81920, type=c\n" | sfdisk $@; \
			LOOPDEV1=$$(losetup -o $$(($(MBR_MULTI_PART1_START_SECTOR)*512)) --sizelimit $$((16*1024*1024)) -f --show $@); \
			mkfs.vfat -F 16 $$LOOPDEV1; \
			mkdir -p /mnt/mbr_multi_p1; \
			mount $$LOOPDEV1 /mnt/mbr_multi_p1; \
			printf "%s\n" "Hello from MBR partition 1 (FAT16)!" > /mnt/mbr_multi_p1/TEST.LSP; \
			umount /mnt/mbr_multi_p1; \
			losetup -d $$LOOPDEV1; \
			LOOPDEV2=$$(losetup -o $$(($(MBR_MULTI_PART2_START_SECTOR)*512)) --sizelimit $$((40*1024*1024)) -f --show $@); \
			mkfs.vfat -F 32 $$LOOPDEV2; \
			mkdir -p /mnt/mbr_multi_p2; \
			mount $$LOOPDEV2 /mnt/mbr_multi_p2; \
			printf "%s\n" "Hello from MBR partition 2 (FAT32)!" > /mnt/mbr_multi_p2/TEST.LSP; \
			umount /mnt/mbr_multi_p2; \
			losetup -d $$LOOPDEV2'

# Primary IDE HDD(if=ide,bus=0,unit=0)から起動するための、実際にGPT+EFI System
# Partition(ESP)+FAT32でフォーマットした起動イメージ(documents/fat32.md
# FAT32-M9)。従来のQEMU vvfatドライバ(-drive format=raw,file=fat:rw:./esp_dir)は
# :floppy:を指定しない場合に内部で偽のMBRパーティションテーブルを合成しており、
# それによって初めてOVMFがfixed disk(removable=off)上のFATを認識してブート
# できていた。パーティションテーブルの無い生FAT32(FAT32_DISK_IMGと同じ構成)を
# そのままPrimary IDE HDDへアタッチしても、UEFI仕様上fixed diskはパーティション
# テーブルを要求するためOVMFがブートできない。そのため実機のUEFI起動メディアと
# 同じレイアウト(GPT + ESP用パーティション(タイプGUID C12A7328-...、sgdiskの
# type code EF00) + FAT32)を作る。
#
# パーティションは1MiB境界(セクタ2048、バイトオフセット1048576)から開始し、
# サイズは60MiB(バイト62914560)に固定する。イメージ全体は64MiBとし、末尾に
# 約3MiBの余裕(バックアップGPT用、規格上必要なのは末尾34セクタ=17KB程度)を
# 持たせている。パーティション部分だけをmkfs.vfat/mountの対象にするため、
# losetup -o <開始バイトオフセット> --sizelimit <サイズbyte>でパーティション専用の
# loopデバイスを作る(GPTパーティションテーブル自体はmkfs.vfatの対象に含めない)。
# ビルド済みのesp_dir/EFI/BOOT/BOOTX64.EFIをコピーするだけの内容。$(TARGET)
# ($(SRC)/$(HDR)に依存するファイルターゲット)と$(SRC)/$(HDR)自体を直接の依存に
# することで、ソース/ヘッダが実際に変更された時だけ再生成される(FAT16_DISK_IMG/
# FAT32_DISK_IMGはビルド結果と無関係な固定テストフィクスチャのためSRC/HDRに
# 依存しない、という違いがある)。tmp/はfat16_test.img/fat32_test.imgのような
# 使い捨てテストフィクスチャ用ディレクトリなので、ビルド成果物であるこのイメージは
# 区別してimages/(IMAGES_DIR)に置く。loopマウント/losetupはCAP_SYS_ADMIN相当の
# 権限を要するため、FAT16_DISK_IMG/FAT32_DISK_IMGと同様--privilegedでdocker runする。
BOOT_FAT32_IMG = $(IMAGES_DIR)/boot_fat32.img

# [測定の落とし穴] buildにブートイメージまで含める。
# QEMUが実際に起動するのは $(BOOT_FAT32_IMG) の中にコピーされたEFIであって
# $(TARGET) ではない。`make build` の後に手でqemuを起動すると**古いカーネルを
# 測ってしまう**。しかもEFIとイメージのmtimeが同じ秒に収まるとmakeは「最新」と
# 判断するので、`make images/boot_fat32.img` を明示しても取りこぼす。
# 実測で、cc_carインライン化の可否を2回とも誤判定しかけた
# (documents/performance-measurement.md「測定の落とし穴」参照)。
#
# この行は $(BOOT_FAT32_IMG) の定義より**後**に書く必要がある。
# 前置き部の `build: $(TARGET)` に足すと、prerequisiteはルール読み込み時に
# 展開されるため空になり、黙って効かない(実際に一度踏んだ)。
build: $(BOOT_FAT32_IMG)
BOOT_FAT32_IMG_SIZE_MB = 64
BOOT_FAT32_PART_SIZE_MB = 60
BOOT_FAT32_PART_START_BYTES = 1048576
BOOT_FAT32_PART_SIZE_BYTES = 62914560

$(IMAGES_DIR):
	mkdir -p $(IMAGES_DIR)

$(BOOT_FAT32_IMG): $(TARGET) $(SRC) $(HDR) | $(IMAGES_DIR)
	dd if=/dev/zero of=$@ bs=1M count=$(BOOT_FAT32_IMG_SIZE_MB) 2>/dev/null
	docker run --rm --privileged --entrypoint bash -v "$(PWD)":/workspace -w /workspace isiki-builder \
		-c 'set -e; \
			sgdisk -o $@; \
			sgdisk -n 1:2048:+$(BOOT_FAT32_PART_SIZE_MB)M -t 1:EF00 -c 1:"EFI System" $@; \
			LOOPDEV=$$(losetup -o $(BOOT_FAT32_PART_START_BYTES) --sizelimit $(BOOT_FAT32_PART_SIZE_BYTES) -f --show $@); \
			mkfs.vfat -F 32 $$LOOPDEV; \
			mkdir -p /mnt/boot_fat32; \
			mount $$LOOPDEV /mnt/boot_fat32; \
			mkdir -p /mnt/boot_fat32/EFI/BOOT; \
			cp esp_dir/EFI/BOOT/BOOTX64.EFI /mnt/boot_fat32/EFI/BOOT/BOOTX64.EFI; \
			umount /mnt/boot_fat32; \
			losetup -d $$LOOPDEV'

# Secondaryチャネル(bus=1,unit=0)にアタッチするディスクイメージ。IDE milestone(既存)
# はIDE_DISK_IMG(素の16MBイメージ+magic文字列)、FAT16milestone(documents/fs.md)は
# FAT16_DISK_IMGを、test-qemu-milestone呼び出し時にQEMU_DISK_IMG=...で上書きして
# 使う。既定値はIDE_DISK_IMGなので既存ターゲットの挙動は変わらない。
QEMU_DISK_IMG = $(IDE_DISK_IMG)

# run/debug(対話的なQEMU起動)はfat16.lisp/ide.lispを手元で試せるよう、既定で
# FAT16フォーマット済みのFAT16_DISK_IMGをアタッチする。QEMU_DISK_IMGとは別の変数に
# しているのは、test-qemu/test-qemu-milestoneの既定値(IDE_DISK_IMG、IDE milestoneの
# magic文字列前提)に影響を与えないため。`make run RUN_DISK_IMG=...`で個別に
# 上書きできる。FAT32を試す場合は`make run RUN_DISK_IMG=tmp/fat32_test.img`
# (FAT32_DISK_IMGの実体、無ければ自動生成される)。
RUN_DISK_IMG ?= $(FAT16_DISK_IMG)

run: $(RUN_DISK_IMG) $(BOOT_FAT32_IMG)
	qemu-system-x86_64 \
		-m $(QEMU_MEM) \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive id=hd_boot,file=$(BOOT_FAT32_IMG),format=raw,if=ide,bus=0,unit=0 \
		-drive id=hd0,file=$(RUN_DISK_IMG),format=raw,if=ide,bus=1,unit=0 \
		-fsdev local,id=fsdev9p,path=$(PWD),security_model=none,readonly=off \
		-device virtio-9p-pci,fsdev=fsdev9p,mount_tag=hostshare

debug: $(RUN_DISK_IMG) $(BOOT_FAT32_IMG)
	qemu-system-x86_64 \
		-m $(QEMU_MEM) \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive id=hd_boot,file=$(BOOT_FAT32_IMG),format=raw,if=ide,bus=0,unit=0 \
		-drive id=hd0,file=$(RUN_DISK_IMG),format=raw,if=ide,bus=1,unit=0 \
		-fsdev local,id=fsdev9p,path=$(PWD),security_model=none,readonly=off \
		-device virtio-9p-pci,fsdev=fsdev9p,mount_tag=hostshare \
		-monitor stdio -serial null \
		-d cpu_reset,int -D qemu.log

# isiki_test.lispをQEMU上で全自動実行し、test-results.txtの結果でpass/failを判定する。
# .qemu-test-triggerの存在をkernel.cが検知し、qemu_boot_test.lispをloadしてから
# ResetSystemでQEMUを電源断する
test-qemu: build $(QEMU_DISK_IMG) $(BOOT_FAT32_IMG)
	mkdir -p $(BUILD_TMPDIR)
	rm -f .qemu-test-trigger test-results.txt
	touch .qemu-test-trigger
	qemu-system-x86_64 \
		-m $(QEMU_MEM) \
		-display none \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive id=hd_boot,file=$(BOOT_FAT32_IMG),format=raw,if=ide,bus=0,unit=0 \
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
	rm -f $(IDE_DISK_IMG) $(FAT16_DISK_IMG) $(FAT32_DISK_IMG) $(BOOT_FAT32_IMG) $(GPT_MULTI_DISK_IMG) $(MBR_MULTI_DISK_IMG)
	$(MAKE) test-qemu
	$(MAKE) test-qemu-milestone MILESTONE=test/lisp/qemu_boot_m5_ide.lisp
	$(MAKE) test-qemu-milestone MILESTONE=test/lisp/qemu_boot_m6_fat16.lisp QEMU_DISK_IMG=tmp/fat16_test.img
	$(MAKE) test-qemu-milestone MILESTONE=test/lisp/qemu_boot_fat32.lisp QEMU_DISK_IMG=tmp/fat32_test.img
	$(MAKE) test-qemu-milestone MILESTONE=test/lisp/qemu_boot_fat32_primary_boot.lisp
	$(MAKE) test-qemu-milestone MILESTONE=test/lisp/qemu_boot_partition.lisp QEMU_DISK_IMG=tmp/gpt_multi_test.img
	$(MAKE) test-qemu-milestone MILESTONE=test/lisp/qemu_boot_partition.lisp QEMU_DISK_IMG=tmp/mbr_multi_test.img

# za_test.lisp(拡張1/4/6)のGC誘発を伴う大量ループ(N=50000)をローカルでのみ実行する。
# GitHub ActionsはKVM無しでQEMUがTCG(ソフトウェアエミュレーション)にフォールバック
# するため、この負荷テストはCIのmilestone 2(qemu_boot_m2_za.lisp)からは外してある
test-qemu-stress:
	$(MAKE) test-qemu-milestone MILESTONE=test/lisp/qemu_boot_m2_za_stress.lisp

# [性能測定] documents/performance-measurement.md「read-file-into-vector-native」
# 参照。READ-FILE-INTO-VECTOR-NATIVE(mount.c、命令数計測実験専用のFAT16
# ルート直下限定・素のC実装)がread-file-into-vectorと完全に同じ結果を返す
# ことを、0byte・クラスタサイズ未満・ちょうど1クラスタ・複数クラスタに
# またがるサイズの各パターンで確認する。test-qemu-allには含めない(実験専用
# プリミティブの検証であり、CIで恒常的に検証すべき正式APIではないため)
test-qemu-read-file-native:
	$(MAKE) test-qemu-milestone MILESTONE=test/lisp/qemu_boot_read_file_native.lisp QEMU_DISK_IMG=tmp/fat16_test.img

# [ファイルI/O]#52(M9): カーネル自身のブートバイナリ(約1.76MB)の読み込み(#41)と
# 65536byte超の書き込み(#39)を実データ規模で検証する。test-qemu-stressと同じ理由
# (KVM無しのQEMU/TCGでは1MB超のファイルI/Oが現実的な時間で終わらない)でローカル
# 専用とし、test-qemu-all/CIには含めない。fat16_test.lisp/fat32_test.lispは
# ディスクを共有しないため、それぞれ専用のディスクイメージを使う
# (QEMU_DISK_IMGを上書きしなければFAT16_DISK_IMGとFAT32_DISK_IMGが衝突する)。
test-qemu-perf:
	rm -f $(FAT16_DISK_IMG) $(FAT32_DISK_IMG)
	$(MAKE) test-qemu-milestone MILESTONE=test/lisp/qemu_boot_perf_fat16.lisp QEMU_DISK_IMG=tmp/fat16_test.img
	$(MAKE) test-qemu-milestone MILESTONE=test/lisp/qemu_boot_perf_fat32.lisp QEMU_DISK_IMG=tmp/fat32_test.img

# [性能測定] test-qemu-perfは#39(書き込み)と#41(読み込み)を同居させており、
# documents/performance-measurement.mdの調査(残り約14.4億命令のホットスポット
# 再探索)で「書き込み側には未解明の遅さ・再現する劣化がある」ことが判明した
# ため、読み込み単体の計測にはこのノイズを排除する必要がある。そのため、
# ゲスト内での書き込み・テストデータ生成を一切行わず、ホスト側で事前に
# mkfs.vfat+mount+cpして焼き込んだファイル(100KB/1MB/2MB、i mod 256の
# 決定論的パターン)をread-file-into-vectorで読むだけの専用ディスクイメージ・
# マイルストーンを用意する(documents/performance-measurement.mdで確立した
# 「ホスト側事前書き込み」手法の恒久化)。サイズはFAT16_DISK_IMG/FAT32_DISK_IMGと
# 同じ理由(FAT32はmkfs.vfat -F 32のクラスタ数警告を避けるための実測済み安全
# マージン)でFAT16=16MB/FAT32=40MBとする。
PERF_READ_SIZE_100K = 100000
PERF_READ_SIZE_1M = 1000000
PERF_READ_SIZE_2M = 2000000
PERF_READ_DATA_100K = $(BUILD_TMPDIR)/perf_read_100k.bin
PERF_READ_DATA_1M = $(BUILD_TMPDIR)/perf_read_1m.bin
PERF_READ_DATA_2M = $(BUILD_TMPDIR)/perf_read_2m.bin

# ホスト側でi mod 256を反復する決定論的パターンのバイナリを生成する
# (docker/gcc本体には依存しないため、isiki-builderコンテナ外・ホストの
# python3で直接生成する)。ゲスト側(perf_read_fat16_test.lisp/
# perf_read_fat32_test.lisp)は同じi mod 256パターンで期待値を再計算して照合する
$(PERF_READ_DATA_100K): | $(BUILD_TMPDIR)
	python3 -c "open('$@', 'wb').write(bytes([i % 256 for i in range($(PERF_READ_SIZE_100K))]))"

$(PERF_READ_DATA_1M): | $(BUILD_TMPDIR)
	python3 -c "open('$@', 'wb').write(bytes([i % 256 for i in range($(PERF_READ_SIZE_1M))]))"

$(PERF_READ_DATA_2M): | $(BUILD_TMPDIR)
	python3 -c "open('$@', 'wb').write(bytes([i % 256 for i in range($(PERF_READ_SIZE_2M))]))"

PERF_READ_FAT16_IMG = $(BUILD_TMPDIR)/perf_read_fat16.img

$(PERF_READ_FAT16_IMG): $(PERF_READ_DATA_100K) $(PERF_READ_DATA_1M) $(PERF_READ_DATA_2M) | $(BUILD_TMPDIR)
	dd if=/dev/zero of=$@ bs=1M count=16 2>/dev/null
	docker run --rm --privileged --entrypoint bash -v "$(PWD)":/workspace -w /workspace isiki-builder \
		-c 'set -e; \
			mkfs.vfat -F 16 $@; \
			mkdir -p /mnt/perf_read_fat16; \
			mount -o loop $@ /mnt/perf_read_fat16; \
			cp $(PERF_READ_DATA_100K) /mnt/perf_read_fat16/READ100K.BIN; \
			cp $(PERF_READ_DATA_1M) /mnt/perf_read_fat16/READ1M.BIN; \
			cp $(PERF_READ_DATA_2M) /mnt/perf_read_fat16/READ2M.BIN; \
			umount /mnt/perf_read_fat16'

PERF_READ_FAT32_IMG = $(BUILD_TMPDIR)/perf_read_fat32.img

$(PERF_READ_FAT32_IMG): $(PERF_READ_DATA_100K) $(PERF_READ_DATA_1M) $(PERF_READ_DATA_2M) | $(BUILD_TMPDIR)
	dd if=/dev/zero of=$@ bs=1M count=40 2>/dev/null
	docker run --rm --privileged --entrypoint bash -v "$(PWD)":/workspace -w /workspace isiki-builder \
		-c 'set -e; \
			mkfs.vfat -F 32 $@; \
			mkdir -p /mnt/perf_read_fat32; \
			mount -o loop $@ /mnt/perf_read_fat32; \
			cp $(PERF_READ_DATA_100K) /mnt/perf_read_fat32/READ100K.BIN; \
			cp $(PERF_READ_DATA_1M) /mnt/perf_read_fat32/READ1M.BIN; \
			cp $(PERF_READ_DATA_2M) /mnt/perf_read_fat32/READ2M.BIN; \
			umount /mnt/perf_read_fat32'

# 使い方: make test-qemu-perf-read。test-qemu-perfと同じくローカル専用
# (KVM無しのQEMU/TCGでは1MB超のファイルI/Oが現実的な時間で終わらないため)
# だが、書き込みを一切含まないため、test-qemu-perfよりも明確に短時間で完走する
test-qemu-perf-read: $(PERF_READ_FAT16_IMG) $(PERF_READ_FAT32_IMG)
	$(MAKE) test-qemu-milestone MILESTONE=test/lisp/qemu_boot_perf_read_fat16.lisp QEMU_DISK_IMG=$(PERF_READ_FAT16_IMG)
	$(MAKE) test-qemu-milestone MILESTONE=test/lisp/qemu_boot_perf_read_fat32.lisp QEMU_DISK_IMG=$(PERF_READ_FAT32_IMG)

# test-qemuと同様だが、MILESTONE変数(boot-entryスクリプトのパス)を
# .qemu-test-triggerの内容として書き込み、指定したmilestoneのみを実行する
# (GitHub Actions側でハングと正常進行の区別をつけるためのmilestone分割用)
# QEMU起動コマンドへ追加で渡す任意のフラグ。test-qemu-instcount/
# test-qemu-icountが命令数計測プラグイン・-icountを差し込むのに使う
# (documents/performance-measurement.md参照)。通常のtest-qemu-milestone
# 単体実行では空のままで既存の振る舞いを変えない
QEMU_EXTRA_FLAGS ?=

# [GC監査] 塗り潰し(GC_PAINT=1)下で試験を流すための実行ターゲット。
# test-qemu-milestoneとの違いは3点だけで、いずれも監査に必須のもの:
#   1. AUDIT_TIMEOUT秒でQEMU自体をtimeoutで打ち切る(makeを殺すとQEMUが孤児に
#      なるため、timeoutはQEMUに直接かける)。打ち切りは終了コード124で分かる。
#   2. " 0 failed" の検証をしない。監査では失敗も結果であり、ホスト側の
#      ドライバ(tools/bench/run_paint_audit.sh)が分類する。
#   3. QEMUの終了コードを$(BUILD_TMPDIR)/qemu-exit.txtへ残す。タイムアウトと
#      試験失敗を区別するのに使う(混同すると実在しないバグを追うことになる)。
# test-results.txtは9p越しにホストのファイルへ直接書かれるため、ゲストが
# ハングしてもそこまでの逐次出力は残る
AUDIT_TIMEOUT ?= 1800

# [性能測定] AOTのletインライナが発火していることを生成コードで確認する。
# 発火しなくなってもエラーにならずテストも通り、命令数だけが戻る(原則6の形)ため、
# 生成コードを直接見るテストとして常時回す
test-inline-expansion: $(LISP_COMPILED)
	tools/bench/check_inline_expansion.sh

test-qemu-audit-run: build $(QEMU_DISK_IMG) $(BOOT_FAT32_IMG)
	mkdir -p $(BUILD_TMPDIR)
	test -n "$(MILESTONE)"
	rm -f .qemu-test-trigger test-results.txt $(BUILD_TMPDIR)/qemu-exit.txt
	echo "$(MILESTONE)" > .qemu-test-trigger
	ec=0; timeout -k 10 $(AUDIT_TIMEOUT) qemu-system-x86_64 \
		-m $(QEMU_MEM) \
		-display none \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive id=hd_boot,file=$(BOOT_FAT32_IMG),format=raw,if=ide,bus=0,unit=0 \
		-drive id=hd0,file=$(QEMU_DISK_IMG),format=raw,if=ide,bus=1,unit=0 \
		-fsdev local,id=fsdev9p,path=$(PWD),security_model=none,readonly=off \
		-device virtio-9p-pci,fsdev=fsdev9p,mount_tag=hostshare \
		$(QEMU_EXTRA_FLAGS) \
		-no-reboot || ec=$$?; \
		echo $$ec > $(BUILD_TMPDIR)/qemu-exit.txt
	rm -f .qemu-test-trigger

test-qemu-milestone: build $(QEMU_DISK_IMG) $(BOOT_FAT32_IMG)
	mkdir -p $(BUILD_TMPDIR)
	test -n "$(MILESTONE)"
	rm -f .qemu-test-trigger test-results.txt
	echo "$(MILESTONE)" > .qemu-test-trigger
	qemu-system-x86_64 \
		-m $(QEMU_MEM) \
		-display none \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive id=hd_boot,file=$(BOOT_FAT32_IMG),format=raw,if=ide,bus=0,unit=0 \
		-drive id=hd0,file=$(QEMU_DISK_IMG),format=raw,if=ide,bus=1,unit=0 \
		-fsdev local,id=fsdev9p,path=$(PWD),security_model=none,readonly=off \
		-device virtio-9p-pci,fsdev=fsdev9p,mount_tag=hostshare \
		$(QEMU_EXTRA_FLAGS) \
		-no-reboot
	rm -f .qemu-test-trigger
	test -f test-results.txt
	cat test-results.txt
	grep -q " 0 failed" test-results.txt

# 命令数ベースの性能計測基盤(documents/performance-measurement.md参照)。
# ゲストが実際に実行した命令数を、ホストの実行速度・スケジューリング
# ノイズと無関係にカウントするTCGプラグイン(tools/plugins/
# isiki_instcount.c)。プラグイン本体はisiki-builderコンテナ内で
# (QEMU_VERSION_FOR_PLUGINに対応する)qemu-plugin.hを取得してビルドする
# (ソースからの再ビルドを前提とし、.soはgit管理対象外)。stderrへ
# `[isiki_instcount] total_insns=<N>` の形式で報告する。
INSTCOUNT_PLUGIN = tools/plugins/isiki_instcount.so
# タイマー割り込み1回あたりの命令数。ゲート区間の中で割り込みが入ると、その
# ハンドラの命令も数えてしまうので差し引く。**実測で求めた値**で、同じ区間を
# 違う tick 数で 4 回測って傾きを取ったところ 4 回とも 914.0 だった
# (documents/performance-measurement.md「手法1の改良案」)。ハンドラを
# 変更したら測り直すこと
ISR_INSNS_PER_TICK ?= 914

$(INSTCOUNT_PLUGIN): tools/plugins/isiki_instcount.c
	docker run --rm --entrypoint bash -v "$(PWD)":/workspace -w /workspace isiki-builder -c '\
		set -e; \
		apt-get update -qq; \
		apt-get install -y -qq libglib2.0-dev pkg-config curl >/dev/null; \
		mkdir -p tools/plugins/build; \
		curl -sL https://raw.githubusercontent.com/qemu/qemu/v$(QEMU_VERSION_FOR_PLUGIN)/include/qemu/qemu-plugin.h -o tools/plugins/build/qemu-plugin.h; \
		gcc -shared -fPIC -Wall -Wextra -O2 -Itools/plugins/build $$(pkg-config --cflags glib-2.0) -o $(INSTCOUNT_PLUGIN) tools/plugins/isiki_instcount.c $$(pkg-config --libs glib-2.0)'

# [性能測定] 構文別ベンチマークスイート(documents/performance-measurement.md
# 「構文別ベンチマークスイート」節)。src/c/bench_subprimitive.cの素のC実装と
# src/lisp/bench_aot.lispの同等Lispコード(AOTトランスパイル済み)を、構文
# カテゴリごとに1対1で命令数比較する。ケースごとに別々のQEMU起動が必要
# (total_insnsはブート全体の合計しか得られないため)なので21回起動し、
# 10〜20分程度かかる。test-qemu-perf等と同じくローカル専用でCIには含めない。
# transpile.lisp/生成コードを変更した後に再実行して、構文単位の改善/退行を追う
BENCH_N_C ?= 10000000
BENCH_N_AOT ?= 1000000

test-qemu-construct-bench: $(INSTCOUNT_PLUGIN) build
	BENCH_N_C=$(BENCH_N_C) BENCH_N_AOT=$(BENCH_N_AOT) tools/bench/run_construct_bench.sh

# [性能測定] Phase1の受け入れ条件(documents/performance-measurement.md
# 「letのImmobilized Spaceリーク」節)。クロージャ生成が実行回数に比例して
# Immobilized Space(4MB固定・GC非対象)を消費しないことを回帰として確認する。
# 修正前はループ内の5変数let*が約24,300反復でOSを永久停止させていた
test-qemu-imm-leak:
	$(MAKE) test-qemu-milestone MILESTONE=test/lisp/qemu_boot_imm_leak.lisp

# [性能測定] forマクロの展開形変更(ループ本体のlet廃止)の意味論回帰テスト。
# 並列束縛・step省略・入れ子・GC併走を確認する
test-qemu-for-expansion:
	$(MAKE) test-qemu-milestone MILESTONE=test/lisp/qemu_boot_for_expansion.lisp

# 構文別ベンチマークのC版/AOT版が同じ計算をしていることの確認(比に意味を
# 持たせる前提条件)。計測と違い数秒で終わるため単独で実行できる
test-qemu-bench-construct-check:
	$(MAKE) test-qemu-milestone MILESTONE=test/lisp/qemu_boot_bench_construct.lisp

# [性能測定] float の型昇格(c-1)の効果。documents/float-contagion.md 6章。
# single-float はタグ0x4の即値なので結果を包むヒープ確保が要らない。
# double は MAGIC_FLOAT の instance なので1演算ごとに確保する。その差を測る。
# 同一ブート内で single/double/mixed/fixnum を2往復し、bytes/call と ticks を
# test-results.txt へ出す(確保量だけは回帰アサーションにしてある)
test-qemu-float-contagion-bench:
	$(MAKE) test-qemu-milestone MILESTONE=test/lisp/qemu_boot_float_contagion_bench.lisp

# 使い方: make test-qemu-instcount MILESTONE=test/lisp/qemu_boot_xxx.lisp
# [QEMU_DISK_IMG=...]。既存のtest-qemu-milestoneと同じ引数を受け付ける
test-qemu-instcount: $(INSTCOUNT_PLUGIN)
	$(MAKE) test-qemu-milestone QEMU_EXTRA_FLAGS="-plugin file=$(INSTCOUNT_PLUGIN)"

# ゲート版(documents/performance-measurement.md「手法1の改良案」)。
# **2パスで走る。** ロード先のアドレスはディスク構成でも変わるので、
# 同じQEMUコマンドラインでまず実行時アドレスを取り(パス1)、それを
# gate_start=/gate_end= に渡して測る(パス2)。
#
# MILESTONE の Lisp は、どこかで
#   (format *isiki-test-stream* "#gate ~A~%" (%%gate-plugin-args))
# を出力すること。パス1はその1行だけを取りに行く。
#
# 使い方: make test-qemu-instcount-gate MILESTONE=test/lisp/qemu_boot_xxx.lisp
test-qemu-instcount-gate: $(INSTCOUNT_PLUGIN)
	@test -n "$(MILESTONE)" || { echo "ERROR: MILESTONE= を指定すること"; exit 1; }
	@echo "=== パス1: ゲートの実行時アドレスを取る(プラグイン無し) ==="
	$(MAKE) test-qemu-milestone MILESTONE=$(MILESTONE)
	@GATE_ARGS=$$(sed -n 's/^#gate //p' test-results.txt | head -1); \
	 test -n "$$GATE_ARGS" || { echo "ERROR: test-results.txt に #gate 行が無い"; exit 1; }; \
	 echo "=== パス2: gate=$$GATE_ARGS ==="; \
	 $(MAKE) test-qemu-milestone MILESTONE=$(MILESTONE) \
	   QEMU_EXTRA_FLAGS="-plugin file=$(INSTCOUNT_PLUGIN),$$GATE_ARGS" 2>&1 | tee tmp/instcount-gate.out
	@echo ""
	@echo "=== 区間ごとの命令数(割り込み補正つき) ==="
	@echo "補正後 = insns - $(ISR_INSNS_PER_TICK) * ticks"
	@echo "  ISR_INSNS_PER_TICK はタイマー割り込み1回あたりの命令数。区間の中で"
	@echo "  割り込みが入るとそのハンドラの命令も数えてしまうので差し引く"
	@echo "  (documents/performance-measurement.md「手法1の改良案」§割り込みの補正)"
	@awk -v isr=$(ISR_INSNS_PER_TICK) '\
	  BEGIN { k=0; j=0 }   # **未初期化のまま添字に使うと ins[""] を引く**(awk の添字は文字列) \
	  FNR==NR { if ($$0 ~ /^\[isiki_instcount\] SEG [0-9]+ insns=/) { v=$$0; sub(/.*insns=/,"",v); ins[k++]=v } next } \
	  /^#seg / { label=$$2; ticks=0; \
	             for (i=1;i<=NF;i++) { if ($$i ~ /^ticks=/) { t=$$i; sub(/^ticks=/,"",t); ticks=t } } \
	             if (hdr==0) { printf "%-18s %14s %6s %14s\n","label","insns","ticks","corrected"; hdr=1 } \
	             printf "%-18s %14d %6d %14d\n", label, ins[j], ticks, ins[j]-isr*ticks; j++ }' \
	  tmp/instcount-gate.out test-results.txt

# 決定論的実行モード(-icount)での計測(documents/performance-measurement.md
# 参照)。1命令ごとに仮想時間を進めるため、ホストの実行速度と無関係に常に
# 同じ結果が再現される。ただしI/Oが絡む大きなワークロード(IDE PIO読み込み等)
# は実時間で非常に遅くなる(実測: 通常運転なら数秒で終わるN=1,000,000の
# JITループが3分強かかった)ため、小規模な決定論的A/B比較に用途を限定する
# こと。マルチスレッドTCGと非互換なのでaccelも合わせて上書きする
ICOUNT_SHIFT ?= 7

# 使い方: make test-qemu-icount MILESTONE=test/lisp/qemu_boot_xxx.lisp
# [QEMU_DISK_IMG=...] [ICOUNT_SHIFT=N]
test-qemu-icount:
	$(MAKE) test-qemu-milestone QEMU_EXTRA_FLAGS="-accel tcg,thread=single -icount shift=$(ICOUNT_SHIFT)"
