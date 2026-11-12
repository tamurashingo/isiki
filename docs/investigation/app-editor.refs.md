# エディタ(#218)— 参照マップ

> 実装担当・レビュー担当向け。`app-editor.logic.md` の節番号に対応する実装箇所。
> 基準: main = `42d4a3a`(#216 / #199 / #217 がすべて入ったあと)。行番号はこの時点のもの。
> **製品のコードは 1 行も変えていない。** 実機は **2 周**だけ回した(§0-2 に内訳)。
> ★ = 実行して確かめていない推定。★ の無いものは 2 周のどちらかで実測した。

---

## 0-1. 件数・数値の導出の仕方(規則 13。本文に写した数字を信じないこと)

```sh
# 画面の大きさ(起動時の解像度から決まる。固定値ではない)
#   framebuffer.c:439-440 相当 … cols = width / VBUF_GLYPH_WIDTH, rows = height / VBUF_GLYPH_HEIGHT
grep -n 'VBUF_GLYPH_WIDTH\|VBUF_GLYPH_HEIGHT' src/c/framebuffer.h

# 1 文字あたりの画素書き込み回数(logic §8-2)。
#   erase_cursor 128 + draw_char の立っているビット数 + draw_cursor 128
python3 - <<'EOF'
import re
vals=[int(x,16) for x in re.findall(r'0x([0-9A-Fa-f]{2})', open('src/c/font8x16.h').read())]
tot=sum(bin(v).count('1') for v in vals[:128*16])
print('glyph avg =', tot/128, '/ per char =', 128+tot/128+128)
EOF

# 取れるキーの一覧(logic §14-1)
grep -n '\[0x' src/c/keymap.c | wc -l            # 変換表 + 特殊キー表の登録数
grep -n 'KEY_CODE_' src/c/keymap.h                # 変換表に無いキーの値

# 制御キー + 英字が既存の文字と衝突する組(logic §14-2)
python3 -c "print([(chr(ord('a')+i-1), i) for i in (8,9,10,13)])"
#   → 8=C-h は Backspace、9=C-i は Tab、10=C-j は改行、13=C-m(改行は 10 なので衝突しない)

# ファイルに対してできること(消す・名前を変えるが無いこと)
grep -n 'defun fat16-\|defun fat32-' src/lisp/fat16.lisp src/lisp/fat32.lisp
grep -rn 'Tremove\|Twstat' src/c/p9.c            # 0 件 = 9P 側にも無い
```

## 0-2. 実機を回した 2 周(予算: 単一段 2 回。使い切った)

| 周 | 段 | 何を測ったか | 結果 |
|---|---|---|---|
| 1 | `make test-qemu-milestone MILESTONE=tmp/qemu_boot_editor_probe.lisp` | 1 文字ずつ流す道の費用 / 折り返し / 巻き上げ / 文字列の書き換え / 鎖の繋ぎ替え / 行の組み立て | 16 passed, 0 failed |
| 2 | `make test-qemu-milestone MILESTONE=tmp/qemu_boot_editor_probe2.lisp` | **文字列を 1 回で流す道との比較**(同じ周の中で両方)/ ホスト共有への上書き保存 | 16 passed, 0 failed |

台本は `tmp/editor_probe.lisp` / `tmp/editor_probe2.lisp` と、
それぞれの段 `tmp/qemu_boot_editor_probe{,2}.lisp`。**調査の後に消した**(使い捨て)。
測る前の宣言は `docs/investigation/app-editor.predictions.md`(**回す前に書いた**。規則 11)。

### 実測値(そのまま)

```
1 周目:
#ed size rows=50 cols=160 jit-write-at=T jit-set-elt=NIL
#ed floor rows=49 bytes=6720 ticks=0 gc=0
#ed one-line chars=159 bytes=10704 ticks=0 gc=0
#ed one-char bytes=608 ticks=0 gc=0
#ed full-screen-1 rows=49 chars=7791 bytes=505344 ticks=8 gc=0
#ed full-screen-2 bytes=505344 ticks=12 gc=0
#ed clear bytes=352 ticks=0
#ed wrap last-col=#\y next-row-0=#\Z next-row-1=#\space
#ed scroll row-3=#\U row-2=#\B row-1=#\space
#ed build append-bytes=20880 setelt-bytes=8288
#ed insert copy-bytes=1024 shift-bytes=6944
#ed literal before="abc" after="Zbc"

2 周目:
#e2 size rows=50 cols=160 chars-per-screen=7791
#e2 screen write-at bytes=505344 ticks=12 gc=0 | 2nd bytes=505360 ticks=12
#e2 screen format bytes=7504  ticks=4  gc=0 | 2nd bytes=7520  ticks=4
#e2 one-line write-at bytes=10720 format bytes=560 blank bytes=560 one-char bytes=576
#e2 save first bytes=4896 ticks=0 | overwrite bytes=2640 ticks=4 | lines-after=3
```

### 宣言との突き合わせ(規則 6: 外れたものを消さない)

| 宣言 | 結果 |
|---|---|
| P1 `term-write-at` は JIT 済み | **当たり**(`jit-write-at=T`) |
| P2 1 行 ≤16KB / 1 文字 ≤100byte | **当たり**(10,704 byte / 66.5 byte) |
| P3 全画面 = 1 行 ×49 ±20% | **当たり**(505,344 / 10,704 = 47.2) |
| P4 全画面 10〜1000 tick | **下限が外れた。** 1 周目は **8 tick**(宣言は 10 以上)。2 周目は 12 tick。分解能 10ms |
| P5 最終桁で折り返す | **当たり**(`next-row-0=#\Z`) |
| P6 最下行の行送りで巻き上がる | **当たり**(`row-3=#\U row-2=#\B`) |
| P7 リテラルは書き換わって残る | **当たり**(`before="abc" after="Zbc"`) |
| P8 連結は器 + 書き込みの 10 倍以上 | **外れた。** 2.5 倍(20,880 / 8,288) |
| Q1 format は 1/10 以下 | **当たり**(7,504 / 505,344 = 1/67) |
| Q2 format は tick でも遅くない | **当たり**(4 対 12) |
| Q3 1 行の format ≤1,000 byte | **当たり**(560) |
| Q4 ホスト共有の短縮上書き → 3 行 | **当たり**(`lines-after=3`) |
| Q5 `term-write-at` 全画面は 1 周目と同値 | **当たり**(505,344 で一致) |

> **「器を使い回すほうが安い」という直感は外れた**(logic §11-1)。
> `#ed insert copy-bytes=1024 shift-bytes=6944`。**作り直しが 6.8 倍安い。**

---

## logic §1 調査範囲 — 読んだもの

| 層 | ファイル |
|---|---|
| 端末の口(#216) | `src/c/terminal.h`(62行), `src/c/terminal.c`(118行) |
| 表示面 | `src/c/framebuffer.h`, `src/c/framebuffer.c`(480行) |
| キーの判定(#216) | `src/c/keymap.h`, `src/c/keymap.c`(156行), `test/c/keymap_test.c`(307行) |
| キーの配達 | `src/c/interrupt.c:265-300`, `src/c/process.h:10-61`, `src/c/process.c:492-540` |
| 1 打鍵の読み | `src/c/reader.c:987-1015` |
| Lisp 側の包み | `src/lisp/init.lisp:1050-1124` |
| 書式付き出力 | `src/c/format.c:158-230` |
| 列の操作 | `src/c/runtime.c:11240-11256`(`ELT`/`LENGTH`), `:11353-11396`(`SET-ELT`) |
| ファイル | `src/c/mount.c:259-340`, `src/c/stream.c:70-140`, `src/lisp/file-cmd.lisp:270-312` |
| FAT | `src/lisp/fat16.lisp`, `src/lisp/fat32.lisp`(**消す・名前を変えるは無い**) |
| アプリ | `apps/pager.lisp`(240行), `apps/inspect.lisp`(580行), `apps/filer.lisp`(490行) |
| 試験 | `test/lisp/term_test.lisp`, `test/lisp/filer_test.lisp`, `test/lisp/filer_app_disk_test.lisp`, `test/lisp/fat16_test.lisp:651-705` |
| 前回の調査 | `docs/investigation/app-terminal-io.{logic,refs}.md`, `docs/investigation/fat-overwrite.{logic,refs}.md` |

---

## logic §2 概念 → 実装

| logic の言葉 | 実装 |
|---|---|
| 文字の格子 | `frame_buffer.content`(`src/c/framebuffer.h:44`)。**`UINT8` 1 バイト/マス。属性の場所が無い** |
| 表示窓 | `rows`/`cols`/`view_offset`(`framebuffer.h:47-64`)。実測 50 行 ×160 桁 |
| カーソル | `frame_buffer.cursor_position`(`framebuffer.h:29-33`)。**読み出しの口は無い** |
| 素の打鍵 | `process_t.raw_input`(`src/c/process.h:54`) |
| 打鍵の受け皿 | `process_t.raw_keys` / `PROCESS_RAW_KEY_BUF_SIZE = 16`(`process.h:22,57`) |
| 行入力の溜め場 | `PROCESS_STDIN_BUF_SIZE = 256`(`process.h:10`) |

---

## logic §3 責務 → 実装

| 担い手 | 実装 |
|---|---|
| 表示面の層 | `src/c/framebuffer.c`: `write_char`:309, `newline`:291, `content_put`:185, `content_screen_row`:179, `scroll_up`:105, `content_scroll_up`:163, `clear_screen`:362, `render_frame_buffer`:375, `os_vbuf_set_cursor`:434, `os_vbuf_cell`:459, `os_vbuf_clear`:472 |
| 打鍵を決める層 | `src/c/keymap.c:91-156` `os_keymap_decode` |
| 打鍵を届ける層 | `src/c/interrupt.c:265-300`、`src/c/process.c:492-540` |
| 1 打鍵の読み | `src/c/reader.c:987-1015` `os_process_stdin_read_char`(先頭が素の打鍵の分岐) |
| 読みやすい名前の層 | `src/lisp/init.lisp:1066`(rows) `:1067`(cols) `:1072`(set-cursor) `:1077`(clear) `:1081`(read-cell) `:1086`(raw-input) `:1092-1101`(**write-at**) `:1104`(read-key) `:1115-1123`(キーの値) |
| Lisp の口の登録 | `src/c/terminal.c:112-118`。**`os_bootstrap` の外**(`terminal.h:14-24` の注記。小ヒープ試験に触らない) |

### 再利用できるもの(logic §3 の 1〜6)

| # | 実装 |
|---|---|
| 1 核と皮 | `apps/pager.lisp:164`(`pager-run`)/`:209`(`pager`)、`apps/filer.lisp:371`(`filer-run`)/`:481`(`filer`)、`apps/inspect.lisp:537`/`:579` |
| 2 素の打鍵を必ず戻す | `apps/filer.lisp:487-490`(`unwind-protect`)。対照は `test/lisp/filer_app_disk_test.lisp` §7、踏んだ記録は `apps/pager.lisp:229-236` |
| 3 一面ずつ見せる核 | `apps/pager.lisp:164-192`。ファイラからの呼び方は `apps/filer.lisp:338-342`(**皮ではなく核を呼ぶ**) |
| 4 小さい部品 | `apps/filer.lisp:170`(`%filer-clamp`)、`:175`(`%filer-floor1`)、`:160`(`%filer-truncate`)、`:223`(`%filer-window-top`)、`:142`(`%filer-number-text`)、`:184`(`%filer-nth`)、`:192`(`%filer-length`) |
| 5 画面の検査 | `test/lisp/filer_app_disk_test.lisp:120-126`(`%fd-row`)。作法は同ファイル `:25-32` |
| 6 切り下げて上書きしない | `apps/filer.lisp:481-486`、`apps/pager.lisp:209-240` |

---

## logic §4 流れ

| 流れ | 実装 |
|---|---|
| 位置を指定して書く | `init.lisp:1092-1101` → `terminal.c:63-73` → `framebuffer.c:434-457`、文字は `stream_lisp.c:737`(`WRITE-CHAR`)→ `framebuffer.c:309-344` |
| 1 文字ごとの消し描き | `framebuffer.c:320`(erase)/`:332`(draw_char)/`:342`(draw_cursor) |
| 折り返し | `framebuffer.c:336-340` → `newline`:291 |
| 巻き上げ | `framebuffer.c:294-303`(`content_scroll_up` + `scroll_up`) |
| 1 打鍵を受け取る | `interrupt.c:292-299` → `process.c:492-504` → `reader.c:990-1014` |
| 横取り | `keymap.c:116-130`(F1〜F4 / 上位修飾 + 前頁・次頁) |
| 書き込み用に開く(= 空にする) | `stream_lisp.c:742` → `mount.c:309-340`(FAT は**空ベクタでの全体書き換え**)/ `stream.c:282`(9P は `P9_OTRUNC`) |
| 書き出し | `stream.c:81-140`(512 バイトごとに `write-from!` を Lisp へ呼び戻す) |

---

## logic §5 不変条件 → 根拠

| # | 根拠 |
|---|---|
| 1 1 マス 1 文字 | `framebuffer.h:44`(`UINT8 *content`)、色は `framebuffer.c:332` で `0x00FFFFFF` 固定 |
| 2 書くのは常に最下部 | `framebuffer.c:179-183`(`content_screen_row` は `view_offset` を見ない)、`:310-315` |
| 3 右下は使えない | `framebuffer.c:336-340` + `:294-303`。**実測**(`#ed wrap` / `#ed scroll`) |
| 4 カーソルは読めない | `src/c/terminal.c:112-118` に読み出しの登録が無い。`apps/filer.lisp:70-83` が同じことを書いている |
| 5 範囲外は断る | `framebuffer.h:206-208`、`framebuffer.c:435-437`/`:460-462`、`terminal.c:30-43` |
| 6 素の打鍵は必ず戻す | `process.c:521-540`(戻り値が切り替え前の状態) |
| 7 開く = 空にする | `mount.c:309-340`、`stream.c:282`。試験は `test/lisp/fat16_test.lisp:687-705` |
| 8 文字列はその場で書ける | `runtime.c:11367-11375`(`TAG_STRING` 枝が `bytes[idx]` を直書き)。試験は `test/lisp/init_test.lisp:1168` |
| 9 リテラルは書き換わって残る | **実測**(`#ed literal before="abc" after="Zbc"`) |
| 10 鎖は繋ぎ替えられる | `SET-CDR`。**引数の順は `(cons 新しい値)`**(`init.lisp:322-323` の `setf` 展開)。**実測** |
| 11 費用は呼んだ回数に比例 | **実測**(`#e2 one-line ... format bytes=560` 対 `one-char bytes=576`) |

---

## logic §6 状態

| 状態 | 実装 |
|---|---|
| 素の打鍵の入切 | `process.c:532-540`。入れると受け皿を空にする(`:534-537`) |
| 保存の途中で空になる窓 | `mount.c:309-340`。**元に戻す経路は無い** |
| エディタが自分で持つもの | 実装は無い(新規) |

---

## logic §7 出入り口

| 出入り口 | 実装 | 無いもの |
|---|---|---|
| 画面 | `terminal.c:112-118` の 5 個 + `init.lisp:1066-1104` の 7 つの包み | 1 行消去 / 色 / カーソル位置の読み出し |
| 打鍵 | `keymap.c:91-156`、`reader.c:987-1015` | — |
| ディスク | `stream_lisp.c:736-756`、`src/lisp/file-cmd.lisp:270-312` | **消す / 名前を変える**(`fat16.lisp` / `fat32.lisp` のどちらにも無い) |
| ホスト共有 | `src/c/stream.c:275-300` | 同上(`p9.c` に `Tremove`/`Twstat` が無い) |

---

## logic §8 制約

| # | 根拠 |
|---|---|
| 8-1 費用の構造 | **実測**(§0-2 の表)。1 文字ずつの道は `init.lisp:1095-1099` の `while` + `write-char`、
   1 回で流す道は `format.c:164-209`(`~A` は `os_print_to_sink` が **C の中で**流す) |
| 8-2 画素の回数 | `framebuffer.c:50-63`(draw_char)、`:71-79`(draw_cursor)、`:88-96`(erase_cursor)。★ 数えただけ |
| 8-3 受け皿 16 個 | `process.h:22`、溢れの破棄は `process.c:493-498` |
| 8-4 右下と最下行 | `framebuffer.c:336-340`、`:291-305`。既存の回避は `apps/filer.lisp:84-91` |
| 8-5 色は構造的に無い | `framebuffer.h:44`、確保は `framebuffer.h:132-142`(`os_vbuf_content_bytes`)+ `kernel.c` の boot allocator。**4 面共有** |
| 8-6 開いたら空 | `mount.c:309-340` |
| 8-7 書式付き出力はディスクに安全でない | **`src/c/format.c:158-163` の `[既知の未修正バグ]` 注記**。★ 再現は見ていない |

---

## logic §10 残っている不足 — 当たった場所

| 不足 | 当たった場所 | 判定 |
|---|---|---|
| 位置指定 | `terminal.c:63-73` + `framebuffer.c:434` | 在る |
| 1 打鍵待ち | `reader.c:990-1014` + `init.lisp:1104` | 在る |
| エコー停止 | `process.c:532-540` + `interrupt.c:292-299` | 在る |
| キーの種類 | `keymap.c:55-68`(特殊キー表)、`:143-153`(制御キー + 英字) | 在る |
| 上書き保存 | `mount.c:309-340` + `src/lisp/fat16.lisp` / `fat32.lisp` の縮小処理(#199 で追加)。試験 `test/lisp/fat16_test.lisp:651-705` | 在る |
| **1 行消去** | 無い。代替は `init.lisp:1092` か `format` で空白を流す。**実測 560 byte** | Lisp で足りる |
| **色・属性** | 無い。入れるなら `framebuffer.h:44` の格子から | C が要る(勧めない) |
| **差分再描画の材料** | `terminal.c:112-118` の 5 個で足りる | Lisp で足りる |
| カーソル位置の読み出し | 無い。`apps/filer.lisp:70-83` が同じ欠落を記録している | 無くても作れる |

---

## logic §11 行の持ち方 — 根拠

| 主張 | 根拠 |
|---|---|
| 文字列は長さ固定・中身は可変 | `runtime.c:11367-11375`、`runtime.c:11246-11248`(`LENGTH` は O(1)) |
| 鎖の長さは O(n) | `runtime.c:11240-11244`(`LENGTH` の `TAG_CONS` 枝が数え上げ) |
| 鎖の添字引きは O(n) | `runtime.c` の `ELT` `TAG_CONS` 枝。`apps/filer.lisp:184-190` が同じ理由で自前の走査を持っている |
| 繋ぎ替えは O(1) | `SET-CDR`。**実測**(`*ep-splice*` / `*ep-unsplice*`) |
| 作り直しが 6.8 倍安い | **実測**(`#ed insert copy-bytes=1024 shift-bytes=6944`) |
| 1 行あたりの記憶量 | `runtime.c:3121`(文字列は `8 + 長さ`)+ cons 16 byte。★ 合計は測っていない |

---

## logic §12 描き直し — 実装の勧め

| 段 | 書き方 | 費用(実測) |
|---|---|---|
| 文字の挿入・削除 | その行だけ `(term-set-cursor r 0)` + `(format out "~A" line)` | 560 byte / 1 刻み未満 |
| 状態表示だけ | 同上を最下行へ。**行送りを出さないこと**(`framebuffer.c:291-305`) | 560 byte |
| 窓が動いたとき | 全画面を同じ道で | 7,504 byte / 4 刻み |

**`term-write-at`(`init.lisp:1092-1101`)をそのまま使わないこと。** 1 文字ずつ `write-char` を
呼ぶので 1 文字あたり 66.5 byte 確保する(実測)。
**ただし包みを書き換えないこと** —— `apps/filer.lisp` と `test/lisp/term_test.lisp` /
`filer_app_disk_test.lisp` が現在の振る舞い(位置が取れなければ何も書かない)に依存している。
エディタ側に「位置を決めて文字列を 1 回で流す」自前の小さな関数を置くのが安全である
(2 周目の `e2-line-at` がその形。`term-set-cursor` が nil なら何も書かない点まで同じにすること)。

---

## logic §13 保存

| 主張 | 根拠 |
|---|---|
| その場で上書きできる | `test/lisp/fat16_test.lisp:651-705`、`test/lisp/fat32_test.lisp` の対応箇所(#199)。ホスト共有側は**実測**(`#e2 save ... lines-after=3`) |
| 名前を変える・消すが無い | `grep 'defun fat16-\|defun fat32-'` に無い。`p9.c` に `Tremove`/`Twstat` が無い |
| 開いた時点で空 | `mount.c:309-340`、`stream.c:282` |
| 保存は 1 文字ずつの道で | `format.c:158-163` の注記 ★ |

---

## logic §14 キー

| 事項 | 根拠 |
|---|---|
| 変換表(素/上位修飾) | `keymap.c:15-44` |
| 変換表に無いキー | `keymap.c:55-68` + `keymap.h:72-84`(値)。母艦の試験 `test/c/keymap_test.c:191-249` が値を固定 |
| 制御キー + 英字 = 1〜26 | `keymap.c:143-153`。試験 `test/c/keymap_test.c:255-270` |
| 衝突(8/9/10) | `keymap.h:74-76`。`C-h`=Backspace、`C-i`=Tab、`C-j`=改行 |
| F1〜F4 は横取り | `keymap.c:116-122`。試験 `test/c/keymap_test.c:120-135` |
| 上位修飾 + 前頁/次頁 は横取り | `keymap.c:124-130`。試験 `:142-156` |
| 無反応のキー | `test/c/keymap_test.c:175-183`(固定キー / もう 1 つの修飾キー / 5 番目の機能キー) |
| Lisp 側の値の写し | `src/lisp/init.lisp:1106-1123`。**C 側(`keymap.h`)と二重管理。片方を動かすと母艦の試験が落ちる** |

---

## logic §15 試験

| 作法 | 手本 |
|---|---|
| 核に 3 つの流れを渡す | `test/lisp/filer_test.lisp`、`test/lisp/pager_test.lisp` |
| 格子を読み返す | `test/lisp/filer_app_disk_test.lisp:120-126` + `:150-170` |
| 描いて読むまでを 1 式で | 同 `:25-32`、`test/lisp/term_test.lisp:30-40` |
| 読み返した値を記録へ残す | `test/lisp/filer_app_disk_test.lisp:172-178` |
| 実機でしか言えない 2 つ | 同 `:5-12` |
| 保存の確かめ方 | `test/lisp/fat16_test.lisp:651-705`(短縮 / 同じ / 拡大 / 2 回続けて) |
| 段の登録 | `Makefile:1463-1500`(並び)+ `tools/check_test_count_ratchet.py` の `COUNTS` と `where_it_runs`。**段を足したら名簿にも足すこと**(`make check-test-file-wiring`) |
| CI にも載せる | `.github/workflows/ci.yml` 側にも段がある(#215/#216/#217 がいずれもそうしている) |

---

## 置き場所(エディタを書くときの約束)

- 原本は `apps/editor.lisp`。ビルドがアプリ専用 FAT32(`Makefile` の `APP_DISK_IMG`)へ焼く
- **同梱にも AOT にも入れない** → 起動イメージ・起動時間・不動領域は 1 バイトも増えない
  (`python3 tools/gen_embedded_lisp.py --print-inputs` と `Makefile` の `TRANSPILE_LISP_SRC` で導出)
- 使ってよいのはカーネル同梱(`src/lisp/init.lisp`)+ AOT 済み(`file-cmd.lisp` など)+
  同じ円盤の `apps/pager.lisp` だけ。**ホスト共有を `load` してはならない**
- `(format t ...)` は使えない(第 1 引数が `MAGIC_STREAM` 必須。`format.c:164-168`)。
  `~nA` の桁揃えも効かない(`format.c:204-215`)
- キーワードは必ずクォートする(`':dir` の形)
- 主ループは関数の中に書く(トップレベルの繰り返しはスタックを壊す)
