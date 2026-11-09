# 参照マップ: M2 load をマウント経由にする(実装担当・レビュー担当向け)

- 作成日: 2026-11-09
- 対応する論理要約: `docs/investigation/m2-load-from-disk.logic.md`
- 対象 issue: #174(マイルストン) / #193(開き方) / #194(隠れた前提) / #195(置き場所と試験)
- **読み取りのみ。QEMU も `make build` も実行していない**
- 事実には行番号を添えた。実測していない推論は **★ 未検証** と付けた
- 前調査(`docs/investigation/m1-init-lisp-embed.refs.md`)と重複する「読み込みの輪の構造」は再掲しない

---

## 0. 論理要約の項目 ↔ 実装箇所 対応表

| 論理要約の項 | 実装箇所 |
|---|---|
| 2. 読み込み作業 | `src/c/load.c:109-128`(`cc_load`)、`src/c/load.c:43-107`(`os_load_from_stream`)、`src/c/load.h:28-29` |
| 2. 開く口(マウント経由) | `src/c/stream_lisp.c:143-197`(`cc_open_input_stream`)、`src/c/mount.c:42-111`(`os_mount_resolve`) |
| 2. マウント表 | `src/lisp/mount.lisp:24-33`(`*mounts*` / `mount`)、`src/c/mount.c:49`(C 側の読み出し) |
| 2. 組み込みのホスト共有の接頭辞 | `src/c/mount.c:15`(`MOUNT_9P_PREFIX = "/9p"`)、`:89-95` |
| 3. 責務 | §A-1 の表 |
| 4. 流れ | §A-2 / §A-3 |
| 5. 業務ルール | §B-1 / §B-2 / §B-5 |
| 6. 状態遷移 | §A-4(ストリームの後始末)、§A-5(GC による再配置) |
| 7. 外部連携 | §C-1 / §C-2 / §D-1 |
| 8. 制約 | §A-5 / §B-3 / §C-3 / §E |
| 9. 問題点 | §A-5 / §B-4 / §E |
| 10. 不明点 | §F |

---

## A. マウント経由の開き方(#193 の材料)

### A-1. 現状の責務の分担(事実)

| 層 | 場所 | 役割 |
|---|---|---|
| 読んで評価する輪 | `src/c/load.c:43-107` `os_load_from_stream(stream, name, env, out_aborted_count)` | 開いたストリームを受け取り、読み切るまで評価。**閉じるのもこの関数** |
| 9p の開き方 | `src/c/load.c:109-128` `cc_load` | `os_stream_open_9p_file` を**直呼び**(`:115`)。**ここが差し替え点** |
| 同梱の開き方 | `src/c/kernel.c:160-182` `load_embedded_lisp` | C のローカル `os_stream_t` + `os_stream_open_string_input`(`:161-162`) |
| マウント経由の開き方(手本) | `src/c/stream_lisp.c:143-197` `cc_open_input_stream` | `os_mount_resolve` → 9p / FAT で分岐 |

### A-2. `os_mount_resolve` の契約(事実)

宣言と doxygen: `src/c/mount.h:14-29`。実装: `src/c/mount.c:42-111`。

| 項目 | 事実 | 行 |
|---|---|---|
| 引数 | `path`(NUL 終端の**絶対パス**)、`out_relative`、`relative_cap`、`out_device` | `mount.h:22-25` |
| 戻り値 | `MOUNT_KIND_NONE` / `_9P` / `_FAT32` / `_FAT16` | `mount.h:6-12` |
| マウント表の読み出し | `*MOUNTS*` を動的変数から取る。**未登録なら `nil` = 空とみなす** | `mount.c:49`、`mount.h:15` |
| 一致の規則 | **最長一致**。接頭辞の直後が終端か `/` であることを要求(`"/mnt"` は `"/mnt2/x"` に一致しない) | `mount.c:25-40`、`:74-84` |
| `"/"` マウントの特例 | 常に一致(長さ 1)。相対パスは**元のパスそのまま** | `mount.c:33-35`、`:101` |
| 組み込みの 9p | `"/9p"` は `*mounts*` に登録が無くても**常に解決できる**。長さ 3 なので `"/"` マウントより優先される | `mount.c:15`、`:89-95` |
| 一致しないとき | `MOUNT_KIND_NONE` を返す。`out_relative` / `out_device` には**何も書かない** | `mount.c:97-99` |
| 相対パスの作り方 | 一致した長さぶんを削る(`"/"` マウントのときだけ削らない)。`relative_cap` で**黙って切り詰める** | `mount.c:101-107` |
| **確保を伴う** | `os_make_symbol` を 3 回呼ぶ(`:44`/`:46`/`:49`)+ マウントパスの `os_string_to_cstr`。**= GC が走りうる** | `mount.c:44-52`、`:76` |

ユニット試験の既存の形(`test/c/mount_test.c:99-160`)。`reset_mounts`(`:99-101`)で `*MOUNTS*` を空にできる。

### A-3. 9p / FAT の分岐と、その後の扱いの違い(事実)

`src/c/stream_lisp.c:143-197` を基準に:

| | 9p(`stream_lisp.c:158-172`) | FAT(`stream_lisp.c:174-193`) |
|---|---|---|
| 開く手段 | `os_stream_open_9p_file(raw, relative, ...)`(`:161`) | `os_mount_fat_read_file(kind, device, relative, &data, &len, &transfer)`(`:182`)で**ファイル全体を一括で読む** |
| Lisp の呼び戻し | **無い** | **ある**(`%DEVICE-HANDLE` と `FATxx-READ-FILE` を `os_apply_function`。`mount.c:136-174`) |
| 非局所脱出 | 起きない | **起きる。** `transfer != nil` ならそれをそのまま Lisp へ返す(`stream_lisp.c:180-187`、`mount.c:126-134`) |
| 必要な前提 | 9p セッション | **FAT ドライバの Lisp 関数が登録済みであること。** 無ければ 0 を返す(`mount.c:140-143`、`:152-155`) |
| ファイル無しと空ファイル | 区別できる | **区別できない**(どちらも `nil`)。既知の制約(`mount.c:170-174`) |
| 出来上がるストリーム種別 | `STREAM_9P_FILE_READ` | `STREAM_STRING_INPUT`(`stream_lisp.c:191`) |
| 解決できない場合 | — | `os_signal_io_error("open-input-file", path, "no such mount", global_environment)`(`stream_lisp.c:195-196`) |

`os_mount_fat_read_file` の doxygen: `src/c/mount.h:31-48`。
バッファは `os_alloc_raw` 済みの**専有バッファ**で、Lisp vector からは既に切り離されている(`stream_lisp.c:188-189`、`mount.c:176-180` 付近)。

### A-4. 後始末(`close`)の違い(事実)

`os_stream_close`: `src/c/stream.c:536-550`。

| 種別 | close で何が起きるか |
|---|---|
| 9p 読み | `os_virtio9p_close(fid)`(`stream.c:540-543`) |
| 文字列入力(= FAT 経由の入力もこれ) | **`closed = 1` を立てるだけ**(`stream.c:549`)。解放は無い(バッファは GC 管理) |
| FAT 書き | `flush_write_buf_fat` を呼び、**戻り値でポインタを更新してから** `closed` を書く(`stream.c:544-548`) |

→ **入力に限れば、9p と FAT の間に後始末の差は実質無い**(`os_stream_close` 1 回で足りる)。
`os_load_from_stream` は正常終了・構文エラー・I/O エラーのすべてで閉じる(`load.c:56`・`:67`・`:105`)。

### A-5. 【最重要】GC 安全性(#49 の経緯と、そこから出る要件)

経緯のコメント(事実):

| 行 | 内容 |
|---|---|
| `src/c/stream_lisp.c:152-157` | **`os_stream_t`(raw)は `MAGIC_STREAM` に包まれるまで GC から到達不能。** 確保してから包むまでの間に他の確保を挟むと、2 回以上の GC を跨いだ時に raw の領域が別オブジェクトの複製先として**上書きされる**。だから raw の確保は**包む直前まで遅らせる** |
| `src/c/stream.h:189-203` | `os_stream_open_string_input` は**自分では一切確保しない**。バッファは呼び出し側が raw より**先に**確保し、raw の確保から包むまでに確保を挟まないこと |
| `src/c/stream_lisp.c:129-137` | `self_handle` は「まだ誰も参照していない」この瞬間だけ生ポインタ経由で書ける |
| `src/c/runtime.c:3295-3340` | `gc_relocate_stream`: **`os_stream_t` 本体は GC のたびに別番地へ複製される。** 文字列入力/出力は `str_cap` ぶんのバッファも**丸ごと複製**(`:3304-3311`)。`mount_file_node` / `self_handle` / `pending_transfer` は `gc_copy_value` し直す |
| `src/c/reader.h:67-70` | 「readerが先読みした 1 文字を stream 自身には書き戻さない。**読み取り中の確保で GC が走ると `os_stream_t` は別番地へ再配置されているため**」 |
| `src/c/stream_lisp.c:284-293` | `cc_read` は読み取り後に**ハンドルから生ポインタを取り直して**から先読み文字を戻す |
| `src/c/runtime.c:544-577` | `os_alloc_bytes`: 空きが足りなければ `os_alloc_bytes_slow`(= GC)。**つまり任意の確保点で GC が走る** |

ここから出る要件(**設計で必ず決めること**):

| # | 要件 | 根拠 |
|---|---|---|
| 1 | **FAT 経路のストリームは言語側の値(`MAGIC_STREAM`)に包み、輪の全体にわたって `GC_PROTECT` する。** 包まないとバッファごと死ぬ(複製先として上書きされる) | `stream_lisp.c:152-157`、`runtime.c:3295-3311` |
| 2 | **包むと、輪が持っている `os_stream_t *` は GC のたびに無効になる。** 各反復でハンドルから取り直す必要がある。`os_load_from_stream` はいま**生ポインタを引数で受けて輪の全体で使い回す**(`load.c:43`・`:53`・`:66`・`:105`)ので、**この形のままでは FAT 経路に使えない** | `load.c:43-107`、`runtime.c:3295-3303` |
| 3 | **さらに深い窓がある。** `os_read_stream_ex` は読み取りの間ずっと生ポインタを保持する(`reader.c:917-919` が `stream_source_ctx_t.stream` へ入れ、`:100-127` の peek/advance がそれを使う)。**1 つの S 式を読む途中で GC が走ると、以後その式の残りは古い複製から読まれ、新しい実体の読取位置は進まない**(= 同じ式を二度読む / 途中から化ける)。`cc_read` も同じ窓を持つ(1 式だけなので表に出にくい) | `reader.c:917-943`、`reader.c:90-127` |
| 4 | **`cc_load` は `env` を `GC_PROTECT` していない**(`load.c:109-128`)。いまは安全(9p の open は確保しない)が、`os_mount_resolve` は `os_make_symbol` を呼び、FAT 経路は Lisp を呼び戻すので**確保する**。`env` は輪へ渡す値であり、`cc_open_input_stream` のように `global_environment` で代用できない(読んだ式は呼び出し元の `env` で評価する契約: `load.h:40`) | `load.c:109-128`、`mount.c:44-52`、`stream_lisp.c:144`・`:162-169` |
| 5 | **FAT ドライバの非局所脱出を拾って返す。** 拾わないと「そのAPI経由のエラーだけが静かに消える」(`stream_lisp.c:9-15` の omission list) | `stream_lisp.c:180-187` |
| 6 | 同梱の読み込み(`kernel.c:160-182`)は**C のローカル + 静的配列**なので包んではいけない(L3。`str_cap` ぶんが GC ごとに複製される) | `kernel.c:140-144` |

**9p 経路は従来どおり C のローカル `os_stream_t` で安全**(`os_stream_open_9p_file` は Lisp を呼ばず確保もしない)。
同じ前例が `cc_probe_file`(`stream_lisp.c:601`)と `cc_file_length`(`stream_lisp.c:692`)にある(どちらも `os_stream_t tmp` をローカルに取る)。

### A-6. `os_load_from_stream` の署名と、そこへ渡すまでに必要なこと(事実)

```
lisp_val_t os_load_from_stream(os_stream_t *stream, const char *name, lisp_val_t env,
                               int *out_aborted_count);
```
宣言 `src/c/load.h:28-29`、doxygen `:7-27`、実装 `src/c/load.c:43-107`。

| 引数 | 契約 | 行 |
|---|---|---|
| `stream` | **開いた状態**で渡す。**この関数が閉じる** | `load.h:19`、`load.c:56`・`:67`・`:105` |
| `name` | エラー表示用の名前。**開く用途には使わない** | `load.h:20-21`、`load.c:57`・`:68`・`:101` |
| `env` | 読んだ各式をこの環境で評価する | `load.h:22` |
| `out_aborted_count` | 打ち切り件数を**加算**する先。不要なら 0 | `load.h:23-24`、`load.c:98-100` |

呼び出し例 2 つ: `load.c:127`(9p、件数は受け取らない)、`kernel.c:165`(同梱、件数を見て `os_panic`)。

**渡すまでに必要なこと(FAT 経路)**: §A-5 の 1〜5。具体的な順序は
`cc_open_input_stream`(`stream_lisp.c:174-193`)の並びがそのまま手本になる
(一括読み → **確保を伴う処理をすべて終えてから** raw を確保 → `os_stream_open_string_input` → `os_make_stream`)。

---

## B. 隠れていた前提(#194 の材料)

### B-1. いま `load` に渡されている path の形(**全数**)

収集方法: `grep -rn '(load "' --include=*.lisp --include=*.sh --include=*.py --include=*.md --include=Makefile .`
(`docs/` を除く)。および `.qemu-test-trigger` を書く箇所。

| 形 | 件数 | 代表 |
|---|---|---|
| **先頭の `/` が無い相対パス** | **上記 grep の全件(2 件の例外を除く)** | `test/lisp/test_framework.lisp`(53 回)、`test/lisp/*_test.lisp`、`src/lisp/*.lisp` |
| **`/9p/` 始まり** | **2 件のみ** | `test/lisp/io_signal_test.lisp:65`・`:66` `(load "/9p/io-no-such-file.lisp")` |

**`.qemu-test-trigger` の中身**(= `cc_load` へ渡る唯一の C 側の呼び出し):

| 行 | 事実 |
|---|---|
| `src/c/kernel.c:91` | 既定値 `"test/lisp/qemu_boot_test.lisp"`(**先頭に `/` が無い**) |
| `src/c/kernel.c:400-414` | トリガーの 1 行目を読んで上書き。空なら既定値のまま |
| `src/c/kernel.c:95` | `cc_load(os_make_cons(os_make_string(g_qemu_test_boot_script), nil), global_environment)` |
| `Makefile:1153` | `touch .qemu-test-trigger`(空 = 既定値) |
| `Makefile:1378` | `echo "$(MILESTONE)" > .qemu-test-trigger`。`MILESTONE` の値は `Makefile:1196-1213` 等で `test/lisp/qemu_boot_*.lisp` |
| `tools/run_qemu_milestone.sh:105` | 同じ形(再試行の輪の中) |

**Lisp 側から `load` を呼ぶ共通の口**: `test/lisp/test_framework.lisp:69-74` `isiki-test-load`。
ここが `(load path)` を呼ぶ(`:74`)。**ほぼ全部の試験ファイルがこの 1 か所を通る。**

**ツール・計測が生成する boot-entry**(前調査 §E-1 の一覧がそのまま該当):
`tools/bench/run_jit_bench.sh:127`、`run_interp_bench.sh:118`、`run_construct_bench.sh:88`、
`run_paint_audit.sh:72`・`:105`、`tools/gen_macro_parity_drivers.py:42`、
`tools/gen_jit_nest_probe.py:41`、`tools/mandel_run.lisp:24`。
いずれも**相対パス**を書く形(`src/lisp/init.lisp` 自体は決定 D2 で外されたが、`test/lisp/...` 系は残っている)。

文書に出てくるもの: `documents/inline-arith.md:133`(`(load "sub.lisp")`)、
`documents/known-bug-frame-cell-gc.md:117-118`、`documents/measurement-callsite-breakdown.md:123`。
`src/lisp/device.lisp:4`、`ide.lisp:5-6`、`partition.lisp:4-5`、`fat16.lisp:5-6`、`fat32.lisp:5-6` は
**コメント**(「REPL から順に load する」という古い手引き。既に AOT 済みで不要と明記)。

### B-2. `os_mount_resolve` はそれらをどう解決するか(事実。**ここが核心**)

`mount_path_match_len`(`mount.c:25-40`)は `path` の先頭から素朴に比較する。

| 渡される path | 結果 | 理由 |
|---|---|---|
| `test/lisp/qemu_boot_test.lisp` | **`MOUNT_KIND_NONE`** | `"/9p"` と比べて 1 文字目(`t` vs `/`)で不一致(`mount.c:27-31`)。`"/mnt"` も同様。**`"/"` マウントがあっても一致しない**(`"/"` の比較も 1 文字目で落ちる) |
| `/9p/io-no-such-file.lisp` | `MOUNT_KIND_9P`、相対 `/io-no-such-file.lisp` | `mount.c:89-95`、`:101` |
| `/mnt/APP.LSP`(`"/mnt"` を FAT16 でマウント済み) | `MOUNT_KIND_FAT16`、相対 `/APP.LSP` | `mount.c:74-84`、`:101` |
| 任意の絶対パス(`"/"` を FAT でマウント済み) | その FAT。相対は**元のパスのまま** | `mount.c:33-35`、`:101` |

> **したがって `load` を素直に `os_mount_resolve` に載せ替えると、
> いまの全部の path が「どのマウントにも解決できない」になる。**
> 9p のままにはならず、FAT にも行かず、**解決不能**である。
> 影響範囲: `.qemu-test-trigger` 経由の台本(`kernel.c:95`)と `isiki-test-load`(`test_framework.lisp:74`)
> = **QEMU 試験の全部**。

**互換の取り方は 2 通りしかない**(設計判断):

| 案 | 内容 | 既存への影響 |
|---|---|---|
| (i) 先頭が `/` でなければ従来どおり 9p ルート相対として開く | `cc_load` 内で分岐。`os_mount_resolve` は絶対パスだけに使う | **既存の path は 1 文字も変えずに通る。** `"/"` マウントの影響も受けない |
| (ii) 相対パスに `/` を補ってから解決する | 書き換え不要に見えるが… | **危険。** `test/lisp/partition_test.lisp:86` が `(mount "/" 'blk0s1 ':fat32)` を実行し、**その後** `test/lisp/qemu_boot_partition.lisp:7` が `(load "test/lisp/file_cmd_test.lisp")` を呼ぶ。(ii) だとこの load が FAT32 スライスへ行って失敗する |
| (iii) 全部の path を `/9p/...` へ書き換える | 一貫するが、`grep` で 100 行超(`test/lisp/` 45 本 + `tools/` 7 か所 + `kernel.c:91` + 文書) | 書き換え漏れが**その試験だけの失敗**として出る |

9p 側は**先頭の `/` を 1 つだけ読み飛ばす**(`src/c/virtio9p.c:61-63`)ので、
`"/9p"` を取り除いた `/test/lisp/x.lisp` はそのまま開ける(= (iii) は技術的には成立する)。
ただし **9p のパス要素は最大 8 個**(`virtio9p.c:9`)、パスバッファ 256 byte(`:11`)で、
超過は**黙って切れる**(`:65` の `count < P9_MAX_PATH_COMPONENTS`)。

### B-3. `LOAD_PATH_MAX` と FAT のパス(事実)

| 定数 | 値 | 場所 |
|---|---|---|
| `LOAD_PATH_MAX` | **256** | `src/c/load.c:11` |
| `STREAM_PATH_MAX` | **256** | `src/c/stream.h:10`(「`stream_lisp.c`/`load.c`/`mount.c` で共有する規約」と明記) |
| `LOAD_ERR_MSG_MAX` | 128 | `src/c/load.c:13` |
| `LOAD_REPORT_MAX` | 256 | `src/c/load.c:16` |

- **容量としては足りる**(マウント接頭辞は `"/mnt"` 程度、FAT の相対パスはむしろ短くなる)。
- ただし `os_string_to_cstr` は**黙って切り詰める**(`src/c/runtime.c:4494-4496`)。
  256 byte を超える path は切れた先で**別の実在ファイルに当たりうる**(既存の性質。`load` も `open-input-stream` も同じ)。
- `mount.c:102-107` の相対パス書き出しも `relative_cap` で黙って切り詰める。
- **★ 未検証(判断)**: `load` 側を `STREAM_PATH_MAX` に揃えるか `LOAD_PATH_MAX` を残すかは設計判断。
  値は同じなので挙動は変わらない。揃えるなら「規約の単一の真実源」(`stream.h:8-10`)に寄せる形になる。

### B-4. エラー文面と `print_load_abort` の出力先(事実)

| # | 場所 | 文面 |
|---|---|---|
| 1 | `load.c:122` | open 失敗: `os_signal_io_error("load", path, err_msg, env)` → 本文は `"load: " + path + ": " + 9p の下位層のメッセージ` |
| 2 | `load.c:57` | 構文エラー: `"load: " + name + ": syntax error"` |
| 3 | `load.c:68` | I/O エラー: `"load: " + name + ": I/O error"` |
| 4 | `load.c:28-41` | 打ち切り 1 件ごとに 1 行 |

- 文面の組み立ては `os_signal_io_error`(`src/c/runtime.c:5743-5768`)。
  クラスは `<simple-error>`、`format-string` は `"~A"` 固定、本文は `format-arguments` 側
  (`~` を含むパスが指示子に解釈されないようにするため。`test/lisp/io_signal_test.lisp:44-45`)。
- **`print_load_abort` の出力先は「現在のプロセスの画面バッファ」**
  (`load.c:30-40` の `proc->stdout_buffer`)。**`test-results.txt`(9p)には出ない。**
  したがって打ち切りの行は QEMU 試験のホスト側成果物には残らない(`-display none` では誰も読まない)。
- 文面を固定している試験: `test/lisp/io_signal_test.lisp:65-81`
  (`"load: "` の 6 文字を `subseq` で見る: `:67`)。
- **`test/lisp/io_signal_test.lisp:69-74` のコメントが「load と open-\*-file ではパスの基準が違う」
  と明記している。M2 でここが変わるので、コメントと 2 つのグローバル
  (`*io-broken-path*` / `*io-broken-load*`、`*io-ok-path*` / `*io-ok-load*`)の存在理由が消える。**
  この試験は `test/lisp/qemu_boot_m1_base.lisp:19` 経由で `test-qemu` に入っている。
- `src/c/load.h:32-42` の doxygen が「`path` は 9P エクスポートルートから見た相対パス」と書いている(`:39`)。**直す対象。**

### B-5. マウントが無い状態で `load` を呼ぶと何が起きるか(事実)

| 条件 | 現状 | マウント経由にした後 |
|---|---|---|
| `*mounts*` が空(= 通常のブート直後。登録は `src/lisp/mount.lisp:24-33` の `mount` を**誰かが呼ぶまで無い**) | 9p を直接開く。9p が有れば**成功する** | `"/9p/..."` は**組み込みで解決できる**(`mount.c:15`・`:89-95`)。それ以外の絶対パスは `MOUNT_KIND_NONE` |
| 9p 自体が無い(実機 / `test-qemu-no-9p`) | 開けず `<simple-error>`。下位層の文面は `os_virtio9p_ensure_session` の失敗理由(`virtio9p.c:109-117`) | 同じ(9p へ行けばそうなる)。FAT へ行けば FAT の結果 |
| **自動マウントは 1 つも無い** | — | `*mounts*` の初期値は `nil`(`src/lisp/mount.lisp:24` は「既存値を書き戻す」形)。**ディスクの `.lisp` を読むには、誰かが先に `mount` を呼んでいる必要がある** |

**★ 未検証(判断)**: 「マウントが無い」ときの返し方を
`cc_open_input_stream` と揃えて `"no such mount"`(`stream_lisp.c:196`)にするか、
9p へ落とす(§B-2 (i))かは設計判断。**(i) を採ると、綴り間違いの `/mnt2/x.lisp` が
9p へ流れて 9p の文面で失敗する**(「マウント名を間違えた」と分からない)。

---

## C. アプリの置き場所の材料(#195 の材料)

### C-1. FAT イメージをビルドで作っている箇所と、中へファイルを入れる手順(事実)

| イメージ | 定義 | 作り方 | 中へ入れる手順 |
|---|---|---|---|
| ブート用 ESP(GPT + FAT32、64MB) | `Makefile:1015`(`BOOT_FAT32_IMG = images/boot_fat32.img`) | `Makefile:1037-1050`。`sgdisk` → `losetup` → `mkfs.vfat -F 32` → `mount` → `cp` → `umount`。**`--privileged` の docker 内** | `Makefile:1048` が `esp_dir/EFI/BOOT/BOOTX64.EFI` を `cp` する**1 行だけ**。依存は `$(TARGET) $(SRC) $(HDR)`(`:1037`)なので**ソースを触るたび作り直される** |
| FAT16 試験用(16MB) | `Makefile:845` | `Makefile:848-868`。`mkfs.vfat -F 16` → `mount -o loop` → `touch`/`printf`/`mkdir` → `umount` → ホスト側で `dd`/`printf` で FAT 表に合成チェインを書く | `Makefile:855-864`。**`printf ... > /mnt/.../TEST.LSP` の形で中身を書く** |
| FAT32 試験用(40MB) | `Makefile:889` | `Makefile:892-914`。同じ作法 | `Makefile:899-910` |
| GPT 2 分割 / MBR 2 分割(64MB) | `Makefile:928` / `:965` | `Makefile:933-955` / `:970-990` | 各パーティションに `TEST.LSP` を 1 つ置く(`Makefile:922-924` の説明) |
| **読み込み計測用(ホスト事前配置の手本)** | `Makefile:1302`(FAT16) / `:1316`(FAT32) | `Makefile:1304-1314` / `:1318-1328` | **`cp $(PERF_READ_DATA_100K) /mnt/.../READ100K.BIN` の形**。データはホストの `python3` で先に生成(`:1293-1300`) |

> **フィクスチャを「ホスト側で事前に焼き込む」作法は既に確立していて、手本がある**
> (`Makefile:1271-1281` の説明。「ゲスト内での書き込みコスト・JIT でのテストデータ生成コストの両方をゼロにできる」)。
> **#195 の `.lisp` フィクスチャはこの形をそのまま真似られる。**

QEMU への繋ぎ:

| 変数 | 既定 | 場所 |
|---|---|---|
| `QEMU_DISK_IMG` | `IDE_DISK_IMG`(素の 16MB + magic) | `Makefile:1056` |
| 接続 | ブート用 = `if=ide,bus=0,unit=0`、データ = `if=ide,bus=1,unit=0` | `Makefile:1158-1159` |
| 上書きの形 | `$(MAKE) test-qemu-milestone MILESTONE=... QEMU_DISK_IMG=tmp/fat16_test.img` | `Makefile:1197-1199` |
| **9p を外した起動はデータディスクを渡さない** | ブート用 1 本だけ | `Makefile:1104-1113`(`:1102-1103` のコメントが「FAT 層も使わないことを示すため」と明記) |

### C-2. 既存の `/mnt` の作法(事実)

| 事実 | 根拠 |
|---|---|
| **自動マウントは無い。** 各試験が自分で `(mount "/mnt" 'blk0 ':fat16)` を呼ぶ | `test/lisp/fat16_test.lisp:437`、`fat32_test.lisp:380`、`perf_fat16_test.lisp:21`、`perf_read_fat16_test.lisp:20`、`read_file_native_test.lisp:12`、`file_cmd_test.lisp:45`(`blk0s0`)、`fat_callback_transfer_test.lisp:25`(`/p1`)、`partition_test.lisp:86`(`"/"` に `blk0s1`) |
| `ls` / `cd` / `cat` は**作業ディレクトリ**を持つ。初期値は `"/"` | `src/lisp/file-cmd.lisp:37`(`*cwd*`)、`:101`(`%filecmd-absolute-path`)、`:197-199`(`pwd`)、`:212-214`(`ls`)、`:238-239`(`cd`)、`:320-321`(`cat`) |
| `ls`/`cd`/`cat` は **C 側の解決を使わず、Lisp 側に同じ最長一致を再実装**している。**9p は解決対象に含めない** | `src/lisp/file-cmd.lisp:11-16` |
| `cat` はファイルの読み出しに総称関数(`read-into!`)を使い、サイズはノードのスロットから取る | `src/lisp/file-cmd.lisp:320-338` |
| **`load` には作業ディレクトリの概念が無い** | `load.c:109-128` に `*cwd*` の参照は無い |
| デバイス名は**探査順の連番**。二次マスタが先に探査されるので、データディスクを付けると ESP は `blk1`、付けないと `blk0` になる | `src/c/block_device.c:118-122`、`src/lisp/device.lisp:26-27`・`:43-45`、`src/lisp/ide.lisp:14` |
| パーティションは `blk0s0` / `blk0s1` … として登録される(GPT の使用中エントリを前から詰めて番号を振る) | `src/lisp/partition.lisp:51-57`、`:102-122` |
| **FAT16 には長い名前の対応が無い。** FAT32 にはある | `src/lisp/fat16.lisp:297`・`:971-1013`(8.3 名のみ)、`src/lisp/fat32.lisp:294-330`(VFAT LFN) |

> **`.lisp` という拡張子は 8.3 名に収まらない**(拡張子が 4 文字)。
> だから既存のフィクスチャは `TEST.LSP` になっている(`Makefile:856`・`:900`)。
> **FAT16 に置くなら名前は `*.LSP` にするしかない。`*.lisp` を置きたいなら FAT32 が必要。**

### C-3. 置き場所の候補(**決めるのは設計担当**)

| 案 | 具体 | 利点 | 欠点 |
|---|---|---|---|
| **(A) データディスクの `/mnt` 配下** | 既存の `tmp/fat16_test.img` か専用の新イメージを `bus=1,unit=0` に付け、`(mount "/mnt" 'blk0 ':fat16)` の後 `(load "/mnt/APP.LSP")` | 既存の作法そのまま(7 本の試験が同じ形)。`ls`/`cd`/`cat` と同じ名前空間。イメージの作り方の手本が 2 つある(`Makefile:848` / `:1304`) | **FAT16 は 8.3 名のみ**なので `*.LSP` に限られる。既存の FAT16 イメージは書き込み試験が壊すので**順序の制約がある**(`Makefile:1187-1189`)→ 専用イメージが要る。**9p を外した起動にはデータディスクが付いていない**(`Makefile:1104-1113`)ので、そこで試すには結線も変える |
| **(B) ブートディスクの ESP 配下** | `images/boot_fat32.img` の ESP に `/LISP/APP.LISP` 等を `cp`(`Makefile:1048` の隣に 1 行足す)。ゲスト側はそのスライスを `mount` して `load` | **カーネルと一緒に配られる**(= アプリが機械に同伴する)。**どの QEMU 起動にも必ず付いている**(9p 無しの起動にも)。FAT32 なので**長い名前が使える** = `*.lisp` が置ける | ESP は UEFI の持ち物という建前を侵す。**デバイス名が接続構成で変わる**(`blk0s0` / `blk1s0`)→ 名前で引く必要がある(`test/lisp/fat32_primary_boot_test.lisp:35-45` が既にその形)。イメージは `$(SRC)`/`$(HDR)` 依存なので**ソースを触るたび作り直される**(アプリだけ入れ替えたい運用に向かない) |
| **(C) アプリ専用の新しいディスク** | 新しい `tmp/app_disk.img`(FAT32、長い名前可)を空いているスロット(`bus=1,unit=1` 等)に付け、`/app` にマウント | 試験用イメージと**完全に分離**できる(破壊的試験の順序制約から自由)。FAT32 なので `*.lisp` が置ける。将来「アプリを配る」話に素直に伸びる | 新しいイメージ生成ルール + QEMU の結線 + マウントの作法を新設することになる。探査順でデバイス名がまた動く(`block_device.c:118-122`) |

**★ 未検証(判断)**: (B) の「アプリがカーネルと同伴する」性質は M2 の目的
(「アプリをディスクに置いて読めるようにする」)に最も近いが、
**ESP を作り直す条件が広い**(`Makefile:1037`)ので、アプリを 1 本足すだけで
ブートイメージが毎回作り直される。(C) はそこだけが違う。

---

## D. 試験の形(#195 の材料)

### D-1. 真似るのが近い既存の段(事実)

| 近さ | 段 | 形 |
|---|---|---|
| **最も近い** | `Makefile:1197`(`qemu_boot_m6_fat16.lisp` + `QEMU_DISK_IMG=tmp/fat16_test.img`) | 「ホストが焼いたフィクスチャ入り FAT イメージを `bus=1` に付け、台本がマウントして読む」。**`test-qemu-all` に入っている**(`Makefile:1190-1213`) |
| 次に近い | `Makefile:1333-1335`(`test-qemu-perf-read`) | **専用イメージ生成ルール + ホスト側 `cp` でフィクスチャを焼く**手本(`Makefile:1304-1314`)。ただし**ローカル専用で `test-qemu-all` には入っていない**(`:1330-1332`) |
| 9p 無しの判定 | `Makefile:1099-1119`(`test-qemu-no-9p`)、実体 `src/c/kernel.c:186-288` | 台本を読めないので**C に式を直接持ち、判定をシリアルへ出す**。**4 件の内訳を `grep` で照合している**(`Makefile:1116`) |
| 陰性対照の作り方の手本 | `src/c/kernel.c:253-256` | 「9p が本当に無いこと」を 1 件目に置く。**これが無いと何も確かめていない段になる**と明記 |

台本の形(`test/lisp/qemu_boot_m6_fat16.lisp:1-10`):
`test_framework.lisp` を読む → 内容ファイルを読む → `(isiki-test-report)` → `(close *isiki-test-stream*)`。
結果は **9p 経由の `test-results.txt`**(`test/lisp/test_framework.lisp:11`)に出る。
ホスト側は `" 0 failed"` を `grep`(`Makefile:1166`、`tools/run_qemu_milestone.sh:113-118`)。

### D-2. フィクスチャを用意する作法(事実)

1. ホストでデータを作る(必要なら)。`python3` を**docker の外**で使う前例: `Makefile:1293-1300`。
2. `dd` で空イメージ → `--privileged` の docker 内で `mkfs.vfat` → `mount -o loop` → `cp`/`printf` → `umount`
   (`Makefile:848-865`、`:1304-1314`)。**loop マウントは権限が要るのでこのルールだけ `--privileged`**(`Makefile:825-827`)。
3. 名前は FAT16 なら 8.3(`TEST.LSP`)。FAT32 なら長い名前も可(`Makefile:903-904` が
   `Long_File_Name.txt` / `This_Is_A_Very_Long_File_Name.txt` を置いている)。
4. 破壊的な試験と同じイメージを使うと**順序の制約**が生まれる(`Makefile:1187-1189`、`:1171-1173`)。
   `test-qemu-all` は先頭で該当イメージを `rm` する(`Makefile:1191`)。

### D-3. 陰性対照(9p 経由の `load` が無傷)の固め方(**筋**)

| # | 案 | 根拠・備考 |
|---|---|---|
| 1 | **何もしない形で既に強い。** `test-qemu-all` の**全段**が 9p 経由の `load` に乗っている(台本の読み出し `kernel.c:95` と `isiki-test-load` `test_framework.lisp:74`)。**9p の `load` が壊れたら全段が落ちる** | `Makefile:1190-1213` |
| 2 | ただし**文面と戻り値の契約**は別に固める価値がある。既存の `test/lisp/io_signal_test.lisp:61-99` が
「`"load: "` で始まる」「正常なら `t`」「構文エラーは `<simple-error>`」を見ている。**ここに「相対パスのまま通ること」を 1 件足すのが最小の固め方** | `io_signal_test.lisp:65-99`、`test/lisp/qemu_boot_m1_base.lisp:19` |
| 3 | C 側のユニット試験も使える。`test/c/load_test.c` は 9p を偽物で差し替える形(`:38-59`)で 7 本 + 輪の試験 4 本が走る(`:199`〜`:400`)。**`mount.c` は既に `TEST_SRC_LOAD` にリンクされている**(`Makefile:196`)ので、`*MOUNTS*` を空にした状態・`/mnt` を積んだ状態の両方を**ゲスト無しで**固定できる(`test/c/mount_test.c:99-101` の `reset_mounts` と同じ形) | `Makefile:196`、`test/c/load_test.c`、`test/c/mount_test.c:99-160` |
| 4 | **★ 未検証(判断)**: ディスクの `.lisp` を読む段を `test-qemu-no-9p` の中に置くと「9p が無くてもディスクから読める」という**一番強い主張**になるが、いまそこにはデータディスクが付いていない(`Makefile:1104-1113`)。ESP に置く案(C-3 (B))ならそのまま置ける。**その場合 `Makefile:1116` の `grep '4 passed, 0 failed'` の件数を直す義務がある**(直し忘れると判定が合わなくなる) | `Makefile:1104-1119`、`kernel.c:250-287` |

---

## E. 静かに壊れうるもの(名指し)

| # | 対象 | どう壊れるか | 根拠 |
|---|---|---|---|
| **1** | **`.qemu-test-trigger` 経由の台本の読み出し** | 相対パスが解決できなくなると、**どの試験も 1 行目に到達しない**。`os_signal_io_error` は上がるが、試験モードでは電源断フックが働く(`kernel.c:421`)ので、ホストからは「`test-results.txt` が無い」= `tools/run_qemu_milestone.sh:112-125` の**再試行の輪**に入る(本物の失敗なのに再試行する) | `kernel.c:91`・`:95`、`run_qemu_milestone.sh:102-125` |
| **2** | **`isiki-test-load`(1 か所)** | ここが落ちると**その台本の以後のファイルが全部読まれない**。`isiki-test-report` は「0 passed, 0 failed」に近い数字を出す。`grep " 0 failed"` は**通ってしまう**(= 静かに合格する) | `test/lisp/test_framework.lisp:69-74`、`Makefile:1166` |
| **3** | **`"/"` マウントを張る試験と同じブートの後続の `load`** | `test/lisp/partition_test.lisp:86` が `"/"` を FAT32 スライスへマウントし、**その後** `qemu_boot_partition.lisp:7` が相対パスで `load` する。相対パスに `/` を補う実装(§B-2 (ii))にすると、ここが FAT32 へ流れて落ちる。**`"/"` マウントは何にでも一致する**(`mount.c:33-35`) | `partition_test.lisp:86`、`qemu_boot_partition.lisp:6-7`、`mount.c:33-35` |
| **4** | **`test/lisp/io_signal_test.lisp:65-81` の 2 本立てのパス** | `(load "/9p/io-no-such-file.lisp")` は**いま 9p のルート直下に `9p` という名の入口が無いから**失敗している。マウント経由にすると「9p 配下の存在しないファイル」として失敗する。**結果は同じ(両方 `<simple-error>`)なので試験は通り続け、意図が変わったことに誰も気づかない。** `:69-74` のコメントと `*io-broken-load*` / `*io-ok-load*` の存在理由は**嘘になる** | `io_signal_test.lisp:65-81`、`mount.c:89-95` |
| **5** | **GC 中の読み取り(§A-5 要件 3)** | FAT 経路で、1 つの S 式を読む途中に GC が走ると、以後その式は**古い複製から読まれる**。症状は「同じ式が二度評価される」「途中から化ける」。**どちらも `load` は成功(`t`)を返す**(打ち切りにならない)。**静かな破壊の典型** | `reader.c:917-919`・`:100-127`、`runtime.c:3295-3311`、`runtime.c:565-567` |
| **6** | **`cc_load` の `env` の保護漏れ(§A-5 要件 4)** | 保護せずに `os_mount_resolve` / FAT 読み出しを跨ぐと、`env` が再配置前の番地を指す。以後の**全式が古い環境に書き込まれる**。`load.c:45-50` に「同型の事故」が既に記録されている | `load.c:109-128`、`mount.c:44-52` |
| **7** | **FAT の書き込みの遅さ** | **`load` は読むだけなので直接は無関係。** 読みは速い: 100KB で約 0 秒、1MB で約 5〜6 秒(`documents/performance-measurement.md:3417-3419`)。**関係するのは試験のフィクスチャの作り方**で、ゲスト内で `.lisp` を書いてから読む形にすると書き込みの遅さ(2MB 一括で約 15〜18 秒、`:3421`)を踏む。**ホスト側で焼き込む作法(§D-2)を使えば回避できる**(`Makefile:1271-1281` がまさにその理由で作られている) | `documents/performance-measurement.md:3417-3422`、`Makefile:1271-1281` |
| **8** | **ユニット試験のリンク一覧** | **足す必要は無い。** `mount.c` は既に `TEST_SRC_LOAD` に入っている | `Makefile:196` |
| **9** | **二重読み込みの検出器** | `tools/check_no_double_init_load.py` は `test/` `tools/` の `.lisp`/`.sh`/`.py` を走査し、同梱済みソースの `load` を禁じる(`:28`・`:31-32`・`:44`)。**パスの書き換え方を変えると、この正規表現の形(引用符で囲んだ素の相対パス)から外れうる** | `tools/check_no_double_init_load.py:28-44` |
| **10** | **`load.h` の doxygen** | `:39` が「`path` は STRING(9P エクスポートルートから見た相対パス)」と書いている。原則 12(説明書きを合わせる)の対象 | `load.h:32-42` |
| **11** | **9p のパス要素数の上限** | 全部を `/9p/...` へ書き換える案(§B-2 (iii))では、`"/9p"` を剥がした後の要素数が 8 を超えると**黙って切れる**。いまの `test/lisp/x_test.lisp` は 2 要素なので余裕はある | `virtio9p.c:9`・`:65` |

---

## F. 不明な点(読み取りでは決められなかったこと)

| # | 内容 |
|---|---|
| 1 | **GC が「1 つの S 式を読む途中」で実際に走る頻度を測っていない。** §E-5 の窓が実機で踏まれるかは未測定(踏む条件は「読み取り中の確保がヒープの空きを越える」こと。`runtime.c:565`)。**★ 未検証** |
| 2 | FAT 経由の入力ストリームで `read` を繰り返す既存の試験があるか(= §E-5 を既に踏んでいる経路があるか)を網羅確認していない。`test/lisp/partition_test.lisp:90-100` が `read-byte` を再帰で回しているが、これは文字列入力ストリームに対する読み出しで、**1 回の呼び出しが短い** |
| 3 | `os_stream_t` の実寸(`stream.h:43` の 1024 byte + `:48` の 512 byte + フィールド)を数えていない。GC ごとの複製費用の見積りに要るなら測る必要がある |
| 4 | ブートの ESP に置いたファイルを `*mounts*` へ**自動で**登録する仕掛けは無い(`src/lisp/mount.lisp:24-33` は手で呼ぶ API のみ)。自動マウントを足すかは設計判断で、足すなら探査順によるデバイス名の揺れ(`block_device.c:118-122`)を名前引きで吸収する形が要る |
| 5 | `load` に作業ディレクトリ(`*cwd*`)を効かせるかどうか。`ls`/`cd`/`cat` は効かせている(`file-cmd.lisp:321`)が `load` は効かせていない。**揃えると `cd` の後の `load` の意味が変わる**ので、相対パスの互換(§B-2)と直接ぶつかる |
