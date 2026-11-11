# 全画面対話アプリの土台 — 参照マップ

> 実装担当・レビュー担当向け。`app-terminal-io.logic.md` の節番号に対応する実装箇所。
> 基準: main = `f8ff0e2`。**読み取りのみ。何も実行していない。**
> 行番号は上記コミット時点のもの。★ = 実行して確かめていない推定。

---

## 0. 件数の数え方(規則 13。数字は記録せず導出する)

本文に出した件数は、すべて下のコマンドで出した。**本文に写した数字を信じないこと。**

```sh
# 変換表に載っているキー番号の個数と、載っていないキーの名前(logic §9-3)
python3 - <<'EOF'
import re
src = open('src/c/interrupt.c').read()
for name in ('SCANCODE_NORMAL','SCANCODE_SHIFT'):
    m = re.search(r'static const char %s\[128\] = \{(.*?)\n\};' % name, src, re.S)
    codes = sorted(int(x,16) for x in re.findall(r'\[0x([0-9A-Fa-f]{2})\]', m.group(1)))
    print(name, len(codes), ' '.join('%02X'%c for c in codes))
    special = {0x01:'ESC',0x0F:'Tab',0x1D:'Ctrl',0x38:'Alt',0x3A:'Caps',
               0x47:'Home',0x48:'Up',0x49:'PgUp',0x4B:'Left',0x4D:'Right',
               0x4F:'End',0x50:'Down',0x51:'PgDn',0x52:'Ins',0x53:'Del'}
    print('  missing:', [n for c,n in sorted(special.items()) if c not in codes])
EOF
# → 両表とも 50 個。missing は ESC/Tab/Ctrl/Alt/Caps/矢印4/Home/End/PgUp/PgDn/Ins/Del

# Lisp から呼べる関数の登録箇所(ファイル別)
grep -rc 'os_set_function(os_make_symbol("' src/c/*.c | grep -v ':0'

# そのうち os_bootstrap の中にあるもの(= 小さいヒープの掃引に効く分。logic §8-6)
awk 'NR>=3722 && NR<=4430' src/c/runtime.c | grep -c 'os_set_function(os_make_symbol("'
grep -c 'os_set_function(os_make_symbol("' src/c/runtime.c   # 同数なら全部が中にある

# 診断用の関数(%% で始まるもの)の一覧
grep -rh 'os_set_function(os_make_symbol("' src/c/*.c \
  | sed 's/.*os_make_symbol("\([^"]*\)").*/\1/' | sort -u | grep '^%%'
```

---

## logic §1 調査範囲 — 読んだファイル

| 層 | ファイル |
|---|---|
| 表示面 | `src/c/framebuffer.h`(207行), `src/c/framebuffer.c`(480行) |
| プロセス | `src/c/process.h`(185行), `src/c/process.c`(480行) |
| 割り込み・キー | `src/c/interrupt.c`(979行), `src/c/interrupt.h`(148行) |
| 読み取り | `src/c/reader.c` |
| 対話ループ | `src/c/repl.c`(80行) |
| 流れ(C) | `src/c/stream.h`, `src/c/stream.c` |
| 流れ(Lisp 境界) | `src/c/stream_lisp.c` |
| 下位の口 | `src/c/subprimitive.c` |
| 起動 | `src/c/kernel.c`, `src/c/main.c` |
| ファイル(Lisp) | `src/lisp/file-cmd.lisp`, `src/lisp/fat16.lisp`, `src/lisp/fat32.lisp` |
| マウント境界 | `src/c/mount.h`, `src/c/mount.c` |
| 標準入出力(Lisp) | `src/lisp/init.lisp` |
| アプリの前例 | `apps/hello.lisp` |
| 既存の設計記録 | `documents/console-process-current-state.md`, `documents/console-scrollback.md` |

**読んでいない**: 9P 側の削除・改名(`src/c/p9.c`, `src/c/virtio9p.c`)。

---

## logic §2 ドメイン概念 → 実装

| 概念 | 実装 |
|---|---|
| プロセス 4 つ固定 | `process.h:8` `#define PROCESS_COUNT VBUF_COUNT` / `framebuffer.h:7` `VBUF_COUNT 4` |
| 実行時に増やせない | `process.c` の `spawn` は `static`、呼び出しは `process_scheduler_start` のみ。`documents/console-process-current-state.md` §3 |
| 表示面 | `frame_buffer` 構造体 `framebuffer.h:23-112`、実体 `framebuffer.c:6` `static frame_buffer frame_buffers[VBUF_COUNT]` |
| プロセスと 1 対 1、起動時固定 | `process.c:421` `proc->stdout_buffer = &buffers[i]` |
| 文字の格子 = 履歴つきの環 | `framebuffer.h:44` `UINT8 *content`、環の規約は `framebuffer.c:120-128` のコメント |
| 表示窓 | `framebuffer.h:65` `view_offset`、`framebuffer.c:148-155` `os_vbuf_visible_row` |
| カーソル | `framebuffer.h:29-33` `cursor_position{x,y}`。**y は画面内の行**(同 29 行のコメント) |
| 入力の溜め場 | `process.h:27-28` `stdin_buf[PROCESS_STDIN_BUF_SIZE]` / `stdin_len`、`process.h:10` = 256 |
| 行の確定 | `process.h:36` `volatile UINT32 ready` |
| 表示の焦点 | `framebuffer.c:9` `active_index` / `process.h:118` `g_current_process_index` |
| 走っているプロセス | 時計の割り込み `interrupt.c:522` `c_timer_switch`。`*CURRENT-PROCESS*` / `*RUN-QUEUE*` 側で切り替わる |
| 3 つが 1 つの番号に潰れている | `documents/console-process-current-state.md` §8-1、`runtime.c:445-448` のコメント |
| 動的変数はプロセス間で共有 | `runtime.c:222` の `g_dynamic_bindings` が単一。`documents/console-process-current-state.md` §4 |

---

## logic §3 責務の分担

| 担い手 | 実装 |
|---|---|
| 表示面の層 | `framebuffer.c` 全体 |
| プロセスの層 | `process.c:450-466` `switch_active_process`、`process.c:473-480` `process_stdin_push` |
| 割り込みの層 | `interrupt.c:293-355` `c_keyboard_handler`、`interrupt.c:261-273` `backspace` |
| 読み取りの層 | `reader.c:62-72` `ensure_data`、`reader.c:917` `os_read`、`reader.c:987-995` `os_process_stdin_read_char` |
| 対話ループ | `repl.c:37-80` `os_repl_step` |
| 流れ(出) | `stream.c:306-309` `os_stream_open_screen_output`、`stream.c:467-471`(画面への 1 文字) |
| 流れ(入) | `stream.c:311-314` `os_stream_open_keyboard_input`、`stream.c:427-433`(キーボードからの 1 文字) |
| ファイルの層 | `src/lisp/file-cmd.lisp`, `src/lisp/fat16.lisp`, `src/lisp/fat32.lisp`, `src/c/mount.c` |

### 責務の歪み 2 つ

1. **エコーが割り込みの層にある** → `interrupt.c:352-353`
   ```c
   frame_buffer *fb = current->stdout_buffer;
   fb->write_char(fb, (UINT8)c);          /* ← 無条件。切る口が無い */
   process_stdin_push(current, (UINT8)c);
   ```
2. **「行を 1 つ読む」が読み取りの層に埋まっている** → `reader.c:62-72`
   ```c
   static void ensure_data(process_t *proc) {
       if (proc->read_pos < proc->stdin_len) { return; }
       reset_line(proc);
       if (!prompt_shown) { print_prompt(proc); prompt_shown = 1; }
       os_wait_for_more_input(proc);        /* ← ready が立つまで hlt */
   }
   ```

---

## logic §4 処理の流れ

### §4-1 出力の 4 経路

| 経路 | 実装 |
|---|---|
| Lisp の出力 | `stream_lisp.c:199-209` `cc_open_output_stream` → `stream.c:306-309` → `stream.c:467-471` → `framebuffer.c:305` `write_char`。書式付き出力は `src/c/format.c`、表示は `src/c/print.c` |
| カーネル内側の直書き | 版表示 `kernel.c:44-61` / 読み込みの進捗 `load.c:144-152` / プロンプト `reader.c:47-55` / キーのエコー `interrupt.c:352-353` / 結果表示 `repl.c:26,67,70,72` |
| 診断(いま見えている面へ) | `interrupt.c:263`, `interrupt.c:751`, `runtime.c:1097,1466,1705,1872,2914,2944,3000,4321`(いずれも `get_active_frame_buffer()` 経由) |
| 直列線(母艦) | `interrupt.c:149-205`。公開口は `interrupt.c:191` `os_diag_serial_write`。**Lisp への登録は無い**(上の `grep` に出てこない) |

### §4-1 1 文字出すときの手順 → `framebuffer.c:305-340` `write_char`

| 手順 | 行 |
|---|---|
| さかのぼり中なら最下部へ飛んで全面描き直し | `framebuffer.c:308-313` |
| カーソルを消す | `framebuffer.c:315-317` |
| 改行なら `newline` | `framebuffer.c:318-324` → `framebuffer.c:287-297` |
| 格子へ書く | `framebuffer.c:326` → `framebuffer.c:179-186` `content_put` |
| 字形を描く | `framebuffer.c:328` → `framebuffer.c:50-64` `draw_char` |
| 桁を進め、桁数で折り返す | `framebuffer.c:330-336` |
| カーソルを描く | `framebuffer.c:337-339` |

**前進しかできないことの根拠**: `framebuffer.c` に「カーソルを任意位置へ置く」関数が無い。
`framebuffer.h:70-112` の関数ポインタ表にも無い。
唯一の後退は `interrupt.c:267-270`(`backspace` が `cursor_position.x -= 1`)で、
**割り込みの層からしか呼ばれない。**

### §4-2 出力の例外系

| 事象 | 行 |
|---|---|
| 格子の範囲外は黙って捨てる | `framebuffer.c:180-182` `if (row >= self->rows \|\| col >= self->cols) return;` |
| 履歴が取れなければ無言で履歴なしに落ちる | `kernel.c:391-397` |
| 128 文字を超える字は疑問符 | `framebuffer.c:52-54` `if (code > 127) code = '?';` |

### §4-3 入力の手順 → `interrupt.c:293-355` `c_keyboard_handler`

| 手順 | 行 | 備考 |
|---|---|---|
| 修飾キーの押し下げ・離し | `interrupt.c:294-301` | `0x2A/0x36` 押下、`0xAA/0xB6` 離し |
| **上位ビットが立っていれば捨てる** | `interrupt.c:302-304` | **`0xE0` もここで消える** |
| プロセス切り替えキー | `interrupt.c:306-313` | `0x3B`〜`0x3E`。**変換表より前**(`interrupt.c:308-310` のコメントが順序の理由を書いている) |
| さかのぼり表示 | `interrupt.c:320-324` | 修飾キー + `0x49`/`0x51`。**変換表より前で番号を拾う前例** |
| 変換表を引く | `interrupt.c:326` | |
| **引いて 0 なら捨てる** | `interrupt.c:327-329` | **矢印が落ちるのはここ** |
| 最下部へ戻す | `interrupt.c:333` | |
| **読み切り待ちなら捨てる** | `interrupt.c:337-340` | `if (current->ready) return;` |
| 1 文字消し | `interrupt.c:342-349` | |
| エコー + 積み込み | `interrupt.c:351-354` | |

割り込みの入口: `interrupt.c:423-424`(`inb(0x60)` → `c_keyboard_handler`)。

### §4-3 読み手の側

| 手順 | 行 |
|---|---|
| 未読があればそれを返す | `reader.c:63-64` |
| 無ければ空にして眠る | `reader.c:65-72` |
| 眠りの実体 | `interrupt.c:282-286` `os_wait_for_more_input`(`while (!proc->ready) hlt`) |
| Lisp 側の 1 文字読み | `stream_lisp.c:227-247` `cc_read_char` → `stream.c:427-433` → `reader.c:987-995` |

### §4-4 入力の例外系

| 事象 | 行 |
|---|---|
| 溢れを黙って捨てる | `process.c:473-480`。`if (proc->stdin_len < PROCESS_STDIN_BUF_SIZE - 1)` で**else が無い** |
| エコーは先に済んでいる | `interrupt.c:353` が `interrupt.c:354` より先 → 画面と中身が食い違う |
| 読み切り待ちで捨てる | `interrupt.c:337-340` |
| 変換表に無いキーは無反応 | `interrupt.c:327-329` |

---

## logic §5 業務ルール・不変条件

| # | 根拠 |
|---|---|
| 1 桁数・行数は解像度から | `framebuffer.c:439-440` `cols = width / VBUF_GLYPH_WIDTH; rows = height / VBUF_GLYPH_HEIGHT;`。字の大きさ `framebuffer.h:10-11`(8×16)。解像度は `main.c:384` で GOP から拾う(`SetMode` は呼んでいない) |
| 2 履歴は画面行数を下回らない | `framebuffer.c:416-419` `os_vbuf_effective_hist_rows`。**確保側と初期化側の両方が同じ関数を通る**(`framebuffer.h:113-122` が理由を書いている) |
| 3 環で、最古を捨て、詰め直さない | `framebuffer.c:163-170` `content_scroll_up` |
| 4 書き込みは常に最下部の 1 画面 | `framebuffer.c:183-185` のコメントと式 `logical_row(self, self->count - self->rows + row)` |
| 5 カーソルは窓の中の位置 | `framebuffer.h:29` のコメント |
| 6 さかのぼり中はカーソルを描かない | `framebuffer.c:72-74`, `framebuffer.c:89-91` |
| 7 さかのぼり位置は面ごと | `framebuffer.c:408-414` `switch_active_frame_buffer` が `view_offset` に触らない |
| 8 さかのぼり中の表示は格子に書かない | `framebuffer.c:210-216` のコメント、`framebuffer.c:218-247` `draw_scroll_indicator` |
| 9 色は固定、属性なし | 前景 `framebuffer.c:328,383` で `0x00FFFFFF` 固定、背景 `framebuffer.c:361` で `0x00000000` 固定。`draw_char` は色を引数に取るが(`framebuffer.c:50`)、**呼び出し側が白しか渡さない** |
| 10 面の切り替えは全面描き直し | `framebuffer.c:413` → `framebuffer.c:371-393` `render_frame_buffer` |
| 11 行の確定は改行だけ | `process.c:477-479` |
| 12 1 行 255 文字 | `process.h:10`(256)と `process.c:474`(`- 1`) |
| 13 切り替えキーは変換表より前 | `interrupt.c:306-313`。**順序の理由がコメントにある**(`interrupt.c:308-310`) |
| 14 キーの宛先は表示の焦点 | `interrupt.c:335` `get_current_process()` → `process.h:120-122`(`g_current_process_index` を引く) |
| 15 確定〜読み切りの間は受け付けない | `interrupt.c:337-340` |
| 16 書き込み用は切り詰め、読み書き両用は保つ | `stream_lisp.c:361`(`1 /* truncate */`)と `stream_lisp.c:406`(`0 /* truncate */`) |
| 17 塊が減る書き換えは受け付けない | `fat16.lisp:952-966` の `cond`(`966` が `(t nil)`)、`fat32.lisp:1152-1162`(`1162` が `(t nil)`) |
| 18 一覧は 3 つ組 | `fat16.lisp:156-167` `%fat16-dir-entries-to-display-list`、`file-cmd.lisp:212` `ls` のコメント |
| 19 マウントの根は常に存在 | `file-cmd.lisp:212-237` `ls` のコメント(根は `.`/`..` を持たないので空と区別できない) |

---

## logic §6 状態と遷移

| 状態 | 実装 |
|---|---|
| 履歴の伸び(`count` は戻らない) | `framebuffer.c:164-168`、初期値は `framebuffer.c:462` `fb->count = rows` |
| 表示窓(両端でクランプ) | `framebuffer.c:249-265` `os_vbuf_scroll`、上限 `framebuffer.c:144-146` `os_vbuf_max_view_offset` |
| 最下部へ戻る引き金 2 つ | 文字を出したとき `framebuffer.c:308-313`、キーを打ったとき `interrupt.c:333` |
| 表示の焦点 | `process.c:450-466` のみが動かす |
| 入力の 3 状態(空 / 溜まり / 確定) | 空へ戻すのは `reader.c:24-28` `reset_line` **だけ**。呼ばれるのは `reader.c:66`(`ensure_data`)と `reader.c:924`(`os_read`) |
| 1 キーずつの状態は無い | `process.h:17-48` に該当フィールドが無い |
| カーソル位置を外から設定する操作が無い | `framebuffer.h:70-112` の関数ポインタ表、および `framebuffer.h:113-199` の公開関数宣言のどちらにも無い |

---

## logic §7 外部連携・データの出入り口

| 口 | 実装 | Lisp への登録 |
|---|---|---|
| 画面 | `framebuffer.c` | **1 文字出力のみ**(流れ経由。`stream_lisp.c:725` `OPEN-OUTPUT-STREAM`, `727` 付近の `WRITE-CHAR`) |
| 母艦のログ線 | `interrupt.c:191` | **無し** |
| キーボード | `interrupt.c`, `reader.c` | `stream_lisp.c:736-737` 付近の `READ-CHAR` 等(行単位) |
| 生のメモリ | `subprimitive.c:39-44` `cc_peek`, `subprimitive.c:52-58` `cc_poke`(**1 バイト単位**) | `subprimitive.c:175-176` `%%PEEK` / `%%POKE` |
| 入出力ポート | `subprimitive.c:14-18, 26-31, 61-69, 72-83` | `subprimitive.c:173-174, 177-178` `%%IN-8` / `%%OUT-8` / `%%IN-16` / `%%OUT-16` |
| ディスクのファイル | `src/lisp/fat16.lisp`, `src/lisp/fat32.lisp`, `src/c/mount.c` | 多数(下表) |

### 生のメモリ経由で画面を触れない根拠

- 表示面の実体は `framebuffer.c:6` で **`static`**。`get_active_frame_buffer()`
  (`framebuffer.c:399-401`)は C からしか呼べず、**Lisp への登録が無い**
  (§0 の `grep` に `get_active_frame_buffer` を返す口が出てこない)
- **記号名 → 番地の逆引きが無い。** あるのは番地 → 記号名だけで、
  しかも `.text` の範囲マッチである: `src/c/disasm_symtab.h:28-29`
  `os_disasm_lookup_kernel_symbol(UINT64 runtime_addr, UINT64 *out_offset)`。
  `src/c/disasm_symtab.c` には `"get_active_frame_buffer"`(:4936)や
  `"os_vbuf_visible_row"`(:4929)が**名前としては**入っているが、引ける向きが逆
- → **画面を触るには新しい口が最低 1 つ必要。**

### ファイル操作の対応表

| 操作 | 実装 | Lisp の名前 |
|---|---|---|
| 一覧 | `fat16.lisp:168` `fat16-read-dir` / `fat32.lisp` 同等 / `file-cmd.lisp:158` 振り分け | `ls`(`file-cmd.lisp:212`。**3 つ組のリストを返しつつ表示もする**) |
| 全体の読み | `fat16.lisp:386` `fat16-read-file` / `file-cmd.lisp:270` `read-file-into-vector` | `read-file-into-vector` |
| 新規作成 | `fat16.lisp:1076` `fat16-create-file` / `fat32.lisp` 同等 | `open-output-file`(新規時)、`fat*-create-file` |
| ディレクトリ作成 | `fat16.lisp:1269` `fat16-create-directory` / `fat32.lisp:1711` | 同名 |
| 位置指定の上書き | `stream_lisp.c:745-746` | `FILE-POSITION` / `SET-FILE-POSITION` + `open-io-file` |
| 大きさ | `fat16.lisp:430` `fat16-file-size` | `file-length` |
| 読み書き両用で開く | `stream_lisp.c:381-423` `cc_open_io_file` | `open-io-file` / `with-open-io-file`(`init.lisp:311`) |
| **削除** | **無し** | — |
| **改名** | **無し** | — |
| **ディレクトリの削除** | **無し** | — |

削除・改名が無いことの根拠:
```sh
grep -rn "delete\|remove\|rename\|unlink" src/lisp/fat16.lisp src/lisp/fat32.lisp
# → 一致なし(fat32 側の "create-dir-entries" 等が別件で引っかかるだけ)
```

### 上書き保存が壊れていると読める経路(★ 未実行)

```
stream_lisp.c:336 cc_open_output_file
  └ stream_lisp.c:360-362  os_mount_fat_resolve_file_node(..., 1 /* truncate */, 1, ...)
      └ mount.c:294-317    truncate 分岐: 空 vector を FAT*-WRITE-FILE へ渡す
          └ fat16.lisp:935 fat16-write-file
              ├ fat16.lisp:955  required-cluster-count = %fat16-cluster-count-for-bytes(0, …)
              │                 → fat16.lisp:765-770 で byte-length==0 なら 0
              ├ fat16.lisp:953  (= required current)  … 元が 0 byte のときだけ成立
              ├ fat16.lisp:957  (> required current)  … 成立しない
              └ fat16.lisp:966  (t nil)  ★ ここに落ちる
          └ mount.c:310-312    truncated == nil → return 0
      └ stream_lisp.c:363-368  → os_signal_io_error("open-output-file", …, "cannot resolve file")
```
FAT32 側も同形: `fat32.lisp:1151-1162`(`1162` が `(t nil)`)。

**試験で守られていない根拠**: FAT 経路で `open-output-file` を使う試験は
`test/lisp/fat16_test.lisp:446`、`test/lisp/fat32_test.lisp:390`、
`test/lisp/fat_callback_transfer_test.lisp:40,60,86` だけで、
**いずれも新規ファイル**(ディスクイメージはビルドで作り直される)。
`test/lisp/io_signal_test.lisp:103,114,166` も新規。
`write-vector-to-file`(`file-cmd.lisp:288`)も `open-output-file` を使うので同じ罠を踏む。
9P 経路(`stream_lisp.c:349-350`)は `P9_OTRUNC` 相当で**別の実装**なので影響しない。

---

## logic §8 制約

| # | 根拠 |
|---|---|
| 1 切り替えキーが奪われる | `interrupt.c:306-313` |
| 2 さかのぼりキーも使用済み | `interrupt.c:320-324` |
| 3 動的変数はプロセス間共有 | `runtime.c:222` |
| 4 開いた流れは面の差し替えに追随しない | `stream.c:308` `stream->out_fb = fb`(開いた時点で焼き付く)。`documents/console-process-current-state.md` §4 |
| 5 3 つの番号が潰れている | `documents/console-process-current-state.md` §8-1 |

### §8-6 組み込みを足すと小さいヒープの試験が固まる

- 罠の記述: `test/c/runtime_test.c:2265-2274`(**通る窓が 5KB 未満**、
  症状は NG ではなくハング)。掃引の履歴は `test/c/runtime_test.c:2231-2254`
- つまみ: `Makefile:33` `SMALL_HEAP_SIZE ?=`、`Makefile:111` `SMALL_HEAP_FLAGS`。
  `BUILD_FLAG_SIGNATURE` に入っているので値を変えれば再ビルドされる
  (`test/c/runtime_test.c:2259`)
- 陽性対照: `Makefile:738-760` `check` 系(`SMALL_HEAP_KNOB_SIZE`)
- 既知の回避策(監査ビルドだけに登録して既定の個数を変えない):
  `runtime.c:3937-3944`(`#ifdef ISIKIOS_ALLOC_AUDIT`)、理由は `runtime.h:908-914`

#### ★ 推定: 「起動の後半で登録する」なら掃引は要らない

根拠の連鎖(**走らせていない**):

1. 小さいヒープの試験が呼ぶのは `os_bootstrap()` だけ — `test/c/runtime_test.c:2276-2282`
   `setup_small_heap`。`grep -n "os_register" test/c/runtime_test.c` → **一致なし**
2. `os_bootstrap()` は `runtime.c:3722`〜`4430`。その範囲の登録数と
   `runtime.c` 全体の登録数が**同じ 165**(§0 の `awk`/`grep`)→ 全部が中にある
3. 他の登録関数は**すべて `kernel.c` からしか呼ばれない**:
   `kernel.c:418` `os_register_subprimitives()`, `:419` `os_register_ide_subprimitives()`,
   `:420` `os_register_mount_native_subprimitives()`, `:429` `os_register_eval_primitives()`,
   `:431` `os_register_format()`, `:433` `os_register_za_primitives()`,
   `:435` `os_register_aot_init_functions()`
4. 小さいヒープの試験のリンク集合は `Makefile:166` `TEST_SRC_RUNTIME`
   = `TEST_COMMON_SRC`(`Makefile:164` = `runtime.c` + `lisp.c`)+ `process.c za.c eval.c
   reader.c stream.c mount.c runtime_test.c`。
   **`subprimitive.c` も `stream_lisp.c` も `framebuffer.c` も入っていない**
5. 過去に掃引し直しが必要になった例(`%%SINGLE-FLOAT-P` と浮動小数の境界 4 定数)は
   **`os_bootstrap` の中**にある: `runtime.c:3928-3933`

→ **新しい口を `subprimitive.c`(`os_register_subprimitives`)か新規ファイルの
同種の登録関数に置けば、`os_bootstrap` の恒久消費は変わらない。** ★
着手時の確認手順: 口を足したあと `make test` が通ることだけを見る
(固まるなら `SMALL_HEAP_SIZE` の掃引へ)。**ただし QEMU は起動しない運用**なので
`make test`(母艦の単体試験)で足りる。

### §8-7/8-8 確保の話(規則 9: 時間より確保量)

| 主張 | 根拠 |
|---|---|
| 描画は GC ヒープを確保しない | `framebuffer.c:305-340` `write_char` は `content` と `buffer` にしか書かない。`os_alloc_*` の呼び出しが無い |
| 「標準出力を取り直す」が毎回確保する | `init.lisp:1027-1029` `standard-output` → 未束縛なら `(open-output-stream)` → `stream_lisp.c:205` 付近 `os_alloc_raw(sizeof(os_stream_t))`。`device.lisp:88-91` `%device-output-stream` も同形 |
| 「引数無しで 1 文字読む」も毎回確保する | `stream_lisp.c:54-80` `resolve_input_args` → 未束縛なら `stream_lisp.c:30-34` `make_keyboard_input_stream` → `os_alloc_raw(sizeof(os_stream_t))` |
| 1 回あたり約 1.6 KB ★ | `stream.h:34-121` のフィールドから算出: `buf_data[1024]` + `write_buf[512]` + 数十バイト。**`sizeof` を実行して確かめていない** |

### §8-9 ゴミ集めの安全点

`repl.c:74-79`。**1 周の最後にしかない**(コメントが理由を書いている)。
アプリは `repl.c:58` `os_eval_top_level_ex` の内側で走るので、その間は来ない。

### §8-10 トップレベルの繰り返しはスタックを壊す

`test/lisp/fat16_test.lisp:425-438` のコメントが実例を書いている
(「数万回級の反復でスタックオーバーフローし、隣接メモリを破損しうる」/
「`defun` 本体は機械語化されて本物のループになる」)。

### §8 描画の重さ ★(すべて推定。測っていない)

導出はループの上下限から。桁数 `cols = width/8`、行数 `rows = height/16`。

| 経路 | 回数 | 行 |
|---|---|---|
| 全面の黒塗り | `height × width` 回の 32bit ストア | `framebuffer.c:358-364` `clear_screen`(二重ループ) |
| 全面の描き直し | 上記 + 空白でないマスごとに `16 × 8 = 128` 回 | `framebuffer.c:371-393` `render_frame_buffer` + `framebuffer.c:56-62` |
| 1 文字出す(見えている面) | カーソル消去 128 + 字形 128 + カーソル描画 128 = **384** | `framebuffer.c:88-99` + `framebuffer.c:50-64` + `framebuffer.c:71-82` |
| 物理画面を 1 行押し上げる | 画面全体の画素コピー | `framebuffer.c:105-118` `scroll_up` |

既存の記録の数値(**引用であり、私は測り直していない** ★):
`documents/console-scrollback.md:195-198` —
「出力全体で見ると支配的なのは物理画面のピクセルスクロール(`scroll_up`、
1280x800x4 = 約 4MB のコピー)」。
解像度 1280×800(→ 160 桁 × 50 行)も `documents/console-process-current-state.md` §2 の
記述を引いたもので、**実機で再確認していない**。

→ **1 文字ずつ全面を埋めると `8000 × 384 = 3,072,000` 画素操作で、
全面の描き直し(約 2,048,000)の約 1.5〜3 倍。** ★
**カーソルの消去・再描画を 1 文字ごとにやめられる口を入れると、ここが約 1/3 になる。** ★

---

## logic §9 既存の問題点・矛盾・リスク

| # | 根拠 |
|---|---|
| 9-1(a) 行確定まで眠る | `reader.c:62-72` + `interrupt.c:282-286` |
| 9-1(b) エコーを切れない | `interrupt.c:352-353`。条件分岐が無い |
| 9-1(c) 読み切り待ちで捨てる | `interrupt.c:337-340` |
| **9-2 前提の訂正** | `interrupt.c:302-304` が `0xE0` を落とす。**しかし矢印が落ちるのは `interrupt.c:327-329`**(変換表が 0)。`interrupt.c:316-319` のコメントが「`0xE0` は上で破棄済みなので、テンキーとナビゲーションキーの PgUp/PgDn は区別されない」と明言している = **前置き無しで番号が届いている**。前例は `interrupt.c:320-324` |
| 9-3 変換表 50 個 | `interrupt.c:56-68`(通常)、`interrupt.c:71-83`(修飾キー併用)。数え方は §0 |
| 9-4 修飾キーは 1 種類 | `interrupt.c:53` `key_shift_pressed` だけ。`0x1D`(Ctrl)/`0x38`(Alt)は §0 の `missing` に出る |
| 9-5 溢れが黙る | `process.c:473-480`(else が無い)+ `interrupt.c:353` が先 |
| 9-6 ★ 拡張キーで修飾キーが乱れる恐れ | `interrupt.c:294-301` が `0x2A/0xAA` を素のまま見る。拡張キーに付く偽の修飾キー列が前置きを剥がされて届けば一致する。**手順の知識からの推定。未確認** |
| 9-7 位置指定が無い | `framebuffer.h:70-199` に該当なし |
| 9-8 消去が外に無い | `framebuffer.c:358-364` `clear_screen` は**物理画面だけ**(`content` に触らない。`kernel.c:403-405` のコメントが食い違いの危険を書いている)。`framebuffer.c:137-142` `clear_logical_row` は `static` |
| 9-9 格子を読み返せない | `framebuffer.c:148-155` `os_vbuf_visible_row` は公開されているが**C 向け**。Lisp への登録が無い。使っているのは `test/c/framebuffer_test.c:42` だけ |
| 9-10 再描画は全か無か | `framebuffer.c:371-393`。差分の記録を持つフィールドが `framebuffer.h:23-112` に無い |
| 9-11 反転表示ができない | §5-9 と同じ |
| 9-12 1 文字消しだけ「見えている面」を触る | `interrupt.c:262` `get_active_frame_buffer()` 対 `interrupt.c:352` `current->stdout_buffer` |
| 9-13 上書き保存 ★ | §7 の経路 |
| 9-14 削除・改名が無い | §7 の `grep` |

### 9-15/9-16 試験の在りか

| 対象 | 試験 | 走る段 |
|---|---|---|
| 表示面 | `test/c/framebuffer_test.c`(境界条件まで。`documents/console-scrollback.md:222-236` が内容を列挙) | **`make test`**(母艦)。`Makefile:230-231` `TEST_SRC_FRAMEBUFFER` / `TEST_BIN_FRAMEBUFFER`、`Makefile:773` の `test` ターゲットの依存に入っている |
| 入力の溜め場 | `test/c/process_test.c:149-174`(`process_stdin_push` と改行で `ready` が立つこと) | **`make test`**。`Makefile:172-173` |
| **キーの取り扱い** | **無し** | — |

**「無し」の根拠(機械的)**:
```sh
grep -n "interrupt.c" Makefile
# → 79(カーネルの SRC)、207・223(「interrupt.c はリンクせず」というコメント)の 3 行だけ。
#    TEST_SRC_* のどれにも入っていない
grep -rn "c_keyboard_handler" test/
# → 一致なし
ls test/c/ | grep -i interrupt
# → 一致なし
python3 tools/check_test_count_ratchet.py --wiring-derive | grep -i interrupt
# → 一致なし(配線の名簿にも無い)
```
→ **変換表・拡張キーの扱い・プロセス切り替えキー・修飾キーは全体が無試験。**

### キーの流し込み

| 手段 | 在りか | 自動化されているか |
|---|---|---|
| C の単体試験から直接積む | `process_stdin_push`。使用例: `test/c/repl_test.c:146`, `test/c/script_test.c:189`, `test/c/reader_test.c:141`, `test/c/process_test.c:153` | **母艦の単体試験だけ。** キーの番号ではなく**文字**を積むので、変換表は通らない |
| 仮想機械の管理口から送る | `documents/console-scrollback.md:205-212`(`-monitor unix:...` + `sendkey` + `screendump`)。**一度だけ手でやった記録** | **されていない。** `Makefile:1238` の `-monitor stdio` は `debug` ターゲット(対話用)のみ。`grep -rn "sendkey\|screendump" Makefile tools/` → 一致なし |
| **標準入力の差し替え** | `init.lisp:1035-1036` `with-standard-input` → `init.lisp:1024-1026` `standard-input` → `stream_lisp.c:54-80` `resolve_input_args` が `*standard-input*` を見る。入力源は `stream_lisp.c:736` 付近 `CREATE-STRING-INPUT-STREAM`(`stream.c:316-322` `os_stream_open_string_input`) | **できる。ゲストを動かす段の Lisp 試験から使える。** 既に揃っている |

→ **アプリのキー入力を「標準入力から 1 文字読む」に通せば、流し込みは既存の仕組みだけで書ける。**
通さない(むき出しの新しい関数にする)と**試験できない。**

---

## logic §11 3 つのアプリ

### 11-1 ページャ — 新しい口ゼロで作れる根拠

| 必要 | 既にあるもの |
|---|---|
| 全体を読む | `file-cmd.lisp:270` `read-file-into-vector` / `fat16.lisp:386` |
| 一面ぶん出す | `stream_lisp.c:725` 付近 `OPEN-OUTPUT-STREAM` + `WRITE-CHAR`、`src/c/format.c` |
| 進む合図を待つ | `stream_lisp.c:227` `cc_read_char`(**行確定まで眠る**)。改行キーで頁送りにすれば成立 |
| さかのぼり(既存) | `interrupt.c:320-324` + `framebuffer.c:249-275` |
| ディスクから読み込む | `apps/hello.lisp` の約束(アプリ専用 FAT32 ディスクの根に置く)、`Makefile` の `APP_DISK_IMG` |

★ 注意(未確認): アプリの中で `read-char` を呼んだときにプロンプトが出ないのは
`reader.c:35` `prompt_shown` が立ったままだからである(`reader.c:918` が 0 に戻すのは
`os_read` の入口のみ)。**呼ばれた文脈によっては 1 度プロンプトが出る可能性がある。**

### 11-2 ファイラ

| 必要 | 在りか |
|---|---|
| 一覧(3 つ組) | `file-cmd.lisp:158` `%filecmd-read-dir` / `fat16.lisp:168` / `fat32.lisp` 同等 |
| 移動 | `file-cmd.lisp:238` `cd` |
| 中身の表示 | `file-cmd.lisp:342` `cat` |
| ディレクトリ作成 | `fat16.lisp:1269` / `fat32.lisp:1711` |
| **削除・改名** | **無し**(§7) |
| **矢印** | **無し**(§9-2。`interrupt.c:320-324` の形で足せる) |
| **反転表示** | **無し**(§5-9) |

★ 注意: `file-cmd.lisp:182-189` `%filecmd-print-entries` は**再帰**である
(`file-cmd.lisp:306-313` `%filecmd-write-vector` は `while` にしてある)。
件数が多いディレクトリでスタックに効く可能性がある。未確認。

### 11-3 エディタ

| 不足 | 足す先 |
|---|---|
| カーソル位置指定 | **C**(`framebuffer.c` に設定操作、`framebuffer.h` に宣言、Lisp への登録) |
| 1 キー待ち | **C**(`interrupt.c` + `process.h` に受け皿、`reader.c`/`stream.c` の読み口) |
| エコー停止 | **C**(`interrupt.c:352-353` を素の入力のときだけ飛ばす) |
| 制御キー・脱出キー・削除キー・矢印 | **C**(`interrupt.c:53` に状態、`interrupt.c:56-83` の前に番号の分岐) |
| 保存の修理 | **Lisp**(`fat16.lisp:966` / `fat32.lisp:1162` の `(t nil)` を「塊を解放して縮める」に) |

---

## logic §12 答え — 変更箇所の見積もり

### 答え 1 の内訳

| やること | 触る場所 |
|---|---|
| 素の入力という状態を持つ | `src/c/process.h`(`process_t` にフィールド追加。`framebuffer.h:23-112` の表と同じく**GC が触る値を増やさないこと**) |
| キーの分岐 | `src/c/interrupt.c:293-355`。**`interrupt.c:320-324` と同じ位置**(変換表より前)に番号の分岐。`interrupt.c:302-304` は**変えない** |
| 1 キーずつの受け皿 | `src/c/process.h` + `src/c/process.c:473-480` の隣 |
| カーソルを置く操作 | `src/c/framebuffer.c` + `src/c/framebuffer.h`。消去は **`content` と物理画面の両方**(`framebuffer.c:137-142` と `framebuffer.c:358-364` の両方を使う) |
| Lisp への登録(**1 個**。選択子つき) | `src/c/subprimitive.c:171-180` `os_register_subprimitives`(★ §8-6 の理由でここが安い) |
| 読み口を標準入力に通す | `src/c/stream.c:427-433`(キーボードの読み)と `src/c/reader.c:987-995` に素の入力の分岐 |

**同時に入れるべき試験**:

| 試験 | 置き場 | 配線 |
|---|---|---|
| キーの取り扱い(**いま無い**) | 新規 `test/c/interrupt_test.c`、または `c_keyboard_handler` を切り出した新ファイル | `Makefile` に `TEST_SRC_*` / `TEST_BIN_*` を足し、`Makefile:773` の `test` の依存に入れ、**`python3 tools/check_test_count_ratchet.py --wiring-derive` で名簿を刷り直す**(`Makefile:437` `check-test-file-wiring` が落ちる) |
| カーソル位置指定・消去 | `test/c/framebuffer_test.c` に追加 | 既に配線済み |
| アプリのキー入力(文字列から流し込む) | `test/lisp/` に新規 | `Makefile` の該当する `test-qemu*` の台本に `load` を足す。名簿も刷り直す |

★ **`interrupt.c` を単体試験にリンクするのは今の形では難しい**(GDT/IDT/TSS と手書き
アセンブラを抱えている。`interrupt.c:15-50`, `interrupt.c:225-258`, `interrupt.c:357-480`)。
`c_keyboard_handler` を別ファイルへ切り出すのが現実的だが、
`Makefile:79` の `SRC` と `TEST_SRC_*` の両方に効くので**別の仕事として切る**のがよい。

### 答え 2 の根拠(再掲)

| 主張 | 根拠 |
|---|---|
| ページャは位置指定を要求しない | `framebuffer.c:305-340`(前進のみで足りる) |
| 改行で頁送りにすれば 1 キー待ちを迂回できる | `stream_lisp.c:227` + `reader.c:62-72`(改行が確定の合図) |
| 書き込みが無いので保存の壊れに当たらない | §7 の経路を通らない |
| 土台の一部は動いている | `interrupt.c:320-324` + `framebuffer.c:249-275` + `test/c/framebuffer_test.c` |

---

## 付録: 規則との対応

| 規則 | この調査での扱い |
|---|---|
| 1(R-claim) | 「確認した」と書いた箇所はすべて同じ行にファイル:行を置いた。測っていないものは ★ |
| 6(R-negative) | 否定の結果を消していない: 「生のメモリ経由では画面を触れない」「記号→番地の逆引きは無い」「キーの流し込みの自動化は無い」「削除・改名は無い」 |
| 9(R-instrument) | **時間を一切測っていない。**代わりにループの回数と確保量(約 1.6 KB/回、描画は確保ゼロ)を書いた。回数も実行していないので ★ |
| 13(R-count) | 件数(変換表 50 個、登録 165 個ほか)は読んで数えず、§0 のコマンドで導出した。**コマンドを本文に置いたので再導出できる** |
| 16(R-rebase) | 既存の基準値を取り直していない(測っていないため)。新しい口を入れたあとに掃引し直す場合は、**鳴ることを先に宣言すること** |
