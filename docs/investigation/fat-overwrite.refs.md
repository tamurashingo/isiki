# 参照マップ — ディスク上のファイルの上書き保存(#199)

対象: main `800b736`。実装担当・レビュー担当向け。
論理要約(`fat-overwrite.logic.md`)の節番号に対応させてある。
**このファイルは設計担当には渡さない。**

---

## 0. 結論に直接対応する 3 行

**この 3 行を見れば決まる。**

| # | 場所 | 中身 |
|---|---|---|
| 1 | `src/c/stream_lisp.c:362-368` | `cc_open_output_file` が `os_mount_fat_resolve_file_node(..., 1 /* truncate */, 1 /* create_if_missing */, ...)` を呼び、偽なら `os_signal_io_error("open-output-file", path, "cannot resolve file", ...)` |
| 2 | `src/c/mount.c:294-320` | 切り詰めの実体。`node != nil && truncate` のとき `FAT16-WRITE-FILE` / `FAT32-WRITE-FILE` を**空の vector** で呼び、`:310` の `if (truncated == nil) return 0;` で失敗にする |
| 3 | `src/lisp/fat16.lisp:966` / `src/lisp/fat32.lisp:1162` | `fat16-write-file` / `fat32-write-file` の `cond` 末尾 **`(t nil)`** = 必要クラスタ数 < 現クラスタ数(縮小)のとき**何もせず nil**。**これが真因** |

連鎖: `(t nil)` → `mount.c:310` が 0 を返す → `stream_lisp.c:367` が `"cannot resolve file"` を signal。

---

## 1. 「cannot resolve file」の全出現(§1 / §4-2)

`grep -rn "cannot resolve file" src/` の結果は **2 箇所のみ**:

| 場所 | 関数 | 出る条件 |
|---|---|---|
| `src/c/stream_lisp.c:367` | `cc_open_output_file`(`:336` から) | `os_mount_fat_resolve_file_node(kind, device, relative, **truncate=1**, create=1)` が 0 を返し、かつ `transfer == nil` |
| `src/c/stream_lisp.c:412` | `cc_open_io_file`(`:381` から) | 同じ関数を **truncate=0** で呼んで 0。切り詰めを要求しないので、**実際に札が無い/作れないときだけ** |

近縁(別文面。混同しないこと):
- `src/c/stream_lisp.c:196` `"no such mount"`(`*mounts*` に無い道)
- `src/c/stream_lisp.c:186` `"cannot read file"`(読み込み側)
- `src/c/load.c:375` `"cannot read file"` / `:394` `"no such mount"`

文面の組み立て: `src/c/runtime.c:5743` `os_signal_io_error`
→ `<SIMPLE-ERROR>`、`format-string` は `"~A"` 固定、本文は `format-arguments` の car
(`"<what>: <path>: <detail>"`)。Lisp からの取り出しは `SIMPLE-ERROR-FORMAT-ARGUMENTS`
(`src/c/lisp_compiled.c:20407` で登録、定義は `src/lisp/init.lisp` 付近の `<simple-error>`)。

---

## 2. open-output-file の FAT 経路(§3 / §4)

| 段 | 場所 |
|---|---|
| 道の割り振り | `src/c/mount.c:41-...` `os_mount_resolve`(`mount_path_match_len` は `:25`) |
| 入口(書き込み用) | `src/c/stream_lisp.c:336-378` `cc_open_output_file` |
| 入口(読み書き両用) | `src/c/stream_lisp.c:381-423` `cc_open_io_file` |
| 把握 → 切り詰め → 新規作成の**順番** | `src/c/mount.c:258-351` `os_mount_fat_resolve_file_node`。`:294` 切り詰め / `:321` 新規作成 / `:347` 最終判定 |
| 把握の実体 | `src/lisp/fat16.lisp:413` `fat16-resolve-node` / `src/lisp/fat32.lisp:650` `fat32-resolve-node` |
| 流れの生成 | `src/c/stream.c:340` `os_stream_open_fat_file_write` / `:345` `os_stream_open_fat_file_io` |
| 流れ → 下回り(部分書き換え) | `src/c/stream.c:120-163` `flush_write_buf_fat` 付近、`src/c/stream.c:165-230` `refill_read_buf_fat` |
| 高い層の上被せ | `src/lisp/file-cmd.lisp:288-297` `write-vector-to-file`(= `open-output-file` + `write-byte`)。読み側は `:270` |
| Lisp 側の別名 | `open-input-file` は `src/lisp/init.lisp:278`。`open-output-file` / `open-io-file` は C の組み込みそのまま(`src/c/stream_lisp.c:733` 付近の `os_register_streams`) |

**切り詰めの意味はここにしか無い**: `src/c/mount.c:294-320`。
下回り(`fat16.lisp` / `fat32.lisp`)に truncate という語は 1 つも無い
(`grep -n "truncate\|縮小" src/lisp/fat16.lisp src/lisp/fat32.lisp` はコメントのみ)。

---

## 3. FAT16 / FAT32 の差(§0 / §7)

**この件に関する差は無い。** 双子の対応表:

| 役目 | FAT16 | FAT32 |
|---|---|---|
| 全体書き換え | `src/lisp/fat16.lisp:935-966` | `src/lisp/fat32.lisp:1130-1162` |
| **縮小を拒む行** | `src/lisp/fat16.lisp:966` `(t nil)` | `src/lisp/fat32.lisp:1162` `(t nil)` |
| 必要クラスタ数 | `src/lisp/fat16.lisp:765` `%fat16-cluster-count-for-bytes` | `src/lisp/fat32.lisp:957` |
| 拡張(塊を足す) | `src/lisp/fat16.lisp:896-918` `%fat16-extend-file` | `src/lisp/fat32.lisp:1089-1107` |
| 塊の確保 | `src/lisp/fat16.lisp:848` `%fat16-allocate-clusters` | `src/lisp/fat32.lisp:1027` |
| 鎖の 1 要素の書き換え | `src/lisp/fat16.lisp:825` `%fat16-set-fat-entry` | `src/lisp/fat32.lisp:1004` |
| 鎖の控えの書き戻し | `src/lisp/fat16.lisp:212` `%fat16-flush-fat-cache` | `src/lisp/fat32.lisp:90` |
| 札の更新 | `src/lisp/fat16.lisp:920` `%fat16-finish-directory-update` | `src/lisp/fat32.lisp:1114`(**開始クラスタが 2 フィールドに分かれる**のが唯一の構造差) |
| 部分書き換え | `src/lisp/fat16.lisp:564-653` `%fat16-write-into-impl` | `src/lisp/fat32.lisp:765-852` |
| **大きさを縮めない行** | `src/lisp/fat16.lisp:639` `new-size = max(end-offset, node.size)` | `src/lisp/fat32.lisp:840` |
| 新規作成 | `src/lisp/fat16.lisp:1076` | `src/lisp/fat32.lisp:1632` |

**解放の処理は両方に無い**: `grep -rn "shrink\|free-chain\|release-cluster" src/lisp/fat16.lisp src/lisp/fat32.lisp` → 0 件。
取りこぼしを許容する決まりは `src/lisp/fat16.lisp:843-847` のコメント。

母艦の共有側(対照): `src/c/stream.c:280-291` `os_stream_open_9p_file_write` は
`P9_OWRITE | P9_OTRUNC` で開く(切り詰めは 9P サーバ側)。
`:293-304` `os_stream_open_9p_file_io` も **`P9_OTRUNC` 付き**
→ **読み書き両用の意味が FAT 側(`src/c/stream.c:345`、切り詰めなし)と食い違う**(§7)。

---

## 4. 「塊が減る書き換え」の 3 通り(§5 / §6)

判定箇所は `src/lisp/fat16.lisp:958-966`(FAT32 は `1154-1162`):

```
required == current  -> %fat16-write-file-data + %fat16-finish-directory-update   (通る)
required >  current  -> %fat16-extend-file     + %fat16-finish-directory-update   (通る)
t                    -> nil                                                       (拒む)
```

- `required` は**バイト長から切り上げた塊数**(`:765`)。**バイト数そのものは見ていない**
  → 同じ塊数に収まる縮小(例 100→50)は 1 本目の枝で通り、札の大きさも小さくなる
- `current` は札の開始クラスタが 0 なら 0、さもなくば鎖の長さ(`:953-956`、鎖は `:270`)
- 0 バイトの既存ファイル(開始クラスタ 0)は `required=0 == current=0` で 1 本目の枝
  → `%fat16-write-file-data`(`:776`)は `(null bytes)` で早期 t …**ではない**。
  空 vector は `nil` ではないので let\* 側へ入るが、`%fat16-split-into-chunks`(`:699`)が
  空リストを返し、`%fat16-write-lba-list`(`:716`)の while が 1 周も回らず t になる。
  **不正な LBA への書き込みは起きない**(実測で裏が取れている)

---

## 5. 既存の試験が当たっていない範囲(§9-5)

### 当たっているもの

| 試験 | 場所 | 何を見ているか |
|---|---|---|
| 全体書き換え・同数 | `test/lisp/fat16_test.lisp:171` / `fat32_test.lisp:162` | 既存ファイルへの同クラスタ数上書き |
| 全体書き換え・増 | `fat16_test.lisp:186` / `fat32_test.lisp:177` | 塊が増える上書き |
| **全体書き換え・減 → nil を期待** | **`fat16_test.lisp:194`** / **`fat32_test.lisp:187`** | **いまの欠陥を「仕様」として固定している行。直すならここを書き換える** |
| 書き込み用に開く | `fat16_test.lisp:446` / `fat32_test.lisp:390` | **新規ファイル(`BIGWR.TXT`)のみ** |
| 読み書き両用で開く | `fat16_test.lisp:463` / `fat32_test.lisp:403` | 既存ファイルだが**切り詰めない**経路。1 バイト上書きで大きさが変わらないことを確認 |
| 高い層の往復 | `fat16_test.lisp:550` / `fat32_test.lisp:479` | **新規ファイル(`RFITV.BIN`)のみ** |
| 文面 | `test/lisp/io_signal_test.lisp:37,58-59` | `no such mount` 側だけ。`cannot resolve file` を見ている試験は**無い** |

### 当たっていないもの(**この Issue の範囲そのもの**)

- **中身のあるファイルへの `open-output-file`** … 両方の仕組みで 0 件
- **中身のあるファイルへの `write-vector-to-file`** … 0 件
- **0 バイトの既存ファイルへの `open-output-file`**(今は通っている側)… 0 件
- `cannot resolve file` の文面 … 0 件

### 2 周目で崩れる段(§9-6。★ 実行で確かめていない)

- `Makefile:1926-1928` `test-qemu-load-gc-stress` は **`rm -f $(FAT16_DISK_IMG)` を入れて回避済み**
  (#199 の当面の対処)
- `Makefile:1425-1426` `test-qemu-all` は先頭で全イメージを `rm` する → **通し周回では当たらない**
- **`test/lisp/fat16_test.lisp:446`(`BIGWR.TXT`)と `:550`(`RFITV.BIN`)、
  `fat32_test.lisp:390` / `:479` は、同じ円盤で単独 2 周目を走らせると
  最初の `open-output-file` で中断すると読める。** イメージ生成規則
  (`Makefile:966` / `Makefile:1013` 付近)は phony でないので作り直されない。★
- `test/lisp/fat_callback_transfer_test.lisp:40,60,86` も `open-output-file` で新規作成
  → 同じ族。`:102,125` は読み書き両用なので影響なし。★

---

## 6. 直し方の候補に対応する実装箇所(§11)

### 候補 A(塊の鎖を縮める)

- 枝を足す場所: `src/lisp/fat16.lisp:966` / `src/lisp/fat32.lisp:1162` の `(t nil)` を
  `(t (%fat16-shrink-file ...))` 相当へ
- 使える道具は**既にある**: `%fat16-set-fat-entry`(`:825`、鎖の 1 要素を 0 にできる)、
  `%fat16-flush-fat-cache`(`:212`)、`fat16-cluster-chain`(`:270`)、
  `%fat16-finish-directory-update`(`:920`、開始クラスタ 0 と大きさ 0 を書ける)
- **順番の制約**(§11 の「危うい点」): 札を先に書き換え(`:920`)、
  そのあとで余った塊の鎖の項目を 0 にし、最後に控えを書き戻す。
  逆順だと二重使用の表ができる
- 繰り返しは `while` で書くこと(`src/lisp/fat16.lisp:270` のコメント、再帰は不可)
- `defmethod` の本体には複雑な処理を書かない約束(`src/lisp/fat16.lisp:488-499`)。
  新しい処理は素の `defun` に切り出す
- **AOT 対象**なので変更すると変換が走る: `Makefile:63` `TRANSPILE_LISP_SRC`、
  `src/lisp/transpile.lisp:58-60`

### 候補 B(切り詰めを独立した意味にする)

- `src/c/mount.c:294-320` を置き換え、下回りに切り詰め専用の入口を足す
- 署名を変えるなら `src/c/mount.h:95` も

### 候補 C(文面と区別だけ)

- `src/c/stream_lisp.c:367` の detail 文字列、および
  `src/c/mount.c:258-351` の戻り値を 1 ビットから理由付きへ(`out_transfer` と同じ要領)
- 文面を固定する試験は `test/lisp/io_signal_test.lisp` に足す場所がある(`:37-59`)

### 候補 D(読み書き両用で代替)— 採ってはいけない

- 理由の実装箇所: `src/lisp/fat16.lisp:639` / `src/lisp/fat32.lisp:840`(大きさが縮まない)

---

## 7. 実測の再現手順(規則 1。**使った QEMU は 2 周**)

使い捨ての材料は `tmp/` に置いて**消した**(git 管理外)。再現するには以下を作り直す。

```sh
make build                                   # 前景で先に通す(実施済み)
cp tmp/fat16_test.img tmp/ovw16.img
cp tmp/fat32_test.img tmp/ovw32.img
make test-qemu-milestone MILESTONE=tmp/ovw_boot16.lisp QEMU_DISK_IMG=tmp/ovw16.img
make test-qemu-milestone MILESTONE=tmp/ovw_boot32.lisp QEMU_DISK_IMG=tmp/ovw32.img
```

結果(2 周とも): `==== isiki tests: 34 passed, 0 failed ====` / 打ち切り 0 件 /
`#count tmp/ovw_probe.lisp 32`。

- 使い捨ての円盤を**写しで用意するのが肝心**。`tmp/fat16_test.img` を直接渡すと
  常設の試験が期待している目録の中身を書き換えてしまう
- `blk0` は `bus=1,unit=0`(`Makefile:1736`)= `QEMU_DISK_IMG`。
  探査順は `src/c/block_device.c:118-123`
- 台本は 9P(ホスト共有)経由で読まれる(`src/c/kernel.c:461-472` の `.qemu-test-trigger`)
  ので、`tmp/` 配下でも `MILESTONE=` に渡せる

### 呼び出し台本(`tmp/ovw_boot16.lisp`。32 は `':fat32` に変えるだけ)

```lisp
(load "test/lisp/test_framework.lisp")
(defglobal *ovw-fs* ':fat16)
(isiki-test-load "tmp/ovw_probe.lisp")
(isiki-test-report)
(close *isiki-test-stream*)
```

### 本体(`tmp/ovw_probe.lisp`。**期待値は走らせる前に書いた**。規則 11)

```lisp
(defglobal *ovw-dev* (%device-handle 'blk0))
(defun ovw-write-file (path bytes)
  (if (eq *ovw-fs* ':fat16) (fat16-write-file *ovw-dev* path bytes)
      (fat32-write-file *ovw-dev* path bytes)))
(defun ovw-read-file (path)
  (if (eq *ovw-fs* ':fat16) (fat16-read-file *ovw-dev* path)
      (fat32-read-file *ovw-dev* path)))
(defun ovw-catch (thunk)
  (block ovw (with-handler (lambda (c) (return-from ovw c)) (funcall thunk))))
;; 開けてしまった場合に二次的に落ちないよう、先にストリームかを見る
(defun ovw-msg (c) (if (open-stream-p c) 'opened (car (simple-error-format-arguments c))))
;; 繰り返しは必ず関数の中(トップレベルの while は 1 反復ごとに入れ子で評価される)
(defun ovw-fill (stream value n)
  (let ((i 0)) (while (< i n) (write-byte value stream) (setq i (+ i 1)))))

(mount "/mnt" 'blk0 *ovw-fs*)

;; P1 新規は通る(負の対照)
(defglobal *ovw-s1* (open-output-file "/mnt/OVWF.TXT"))
(assert-equal t (if *ovw-s1* t nil))
(ovw-fill *ovw-s1* 65 100)
(close *ovw-s1*)
(assert-equal 100 (file-length "/mnt/OVWF.TXT"))
(assert-equal 100 (length (ovw-read-file "/OVWF.TXT")))

;; P2 中身のあるファイルを開き直すと落ちる(本体)
(defglobal *ovw-c1* (ovw-catch (lambda () (open-output-file "/mnt/OVWF.TXT"))))
(assert-equal t (stringp (ovw-msg *ovw-c1*)))
(assert-equal "open-output-file: /mnt/OVWF.TXT: cannot resolve file" (ovw-msg *ovw-c1*))

;; P3 落ちても無傷
(assert-equal 100 (file-length "/mnt/OVWF.TXT"))
(assert-equal 100 (length (ovw-read-file "/OVWF.TXT")))
(assert-equal 65 (elt (ovw-read-file "/OVWF.TXT") 99))

;; P4 0 バイトの既存ファイルなら開き直せる(切り分け。仮説 1 を原因から外す根拠)
(defglobal *ovw-s2* (open-output-file "/mnt/OVWE.TXT"))
(close *ovw-s2*)
(assert-equal 0 (file-length "/mnt/OVWE.TXT"))
(defglobal *ovw-c2* (ovw-catch (lambda () (open-output-file "/mnt/OVWE.TXT"))))
(assert-equal t (if (open-stream-p *ovw-c2*) t nil))
(ovw-catch (lambda () (close *ovw-c2*)))
(assert-equal 0 (file-length "/mnt/OVWE.TXT"))

;; P5-P8 下回りの全体書き換え 3 通り
(defglobal *ovw-s3* (open-output-file "/mnt/OVWG.TXT"))
(ovw-fill *ovw-s3* 65 100)
(close *ovw-s3*)
(assert-equal 100 (file-length "/mnt/OVWG.TXT"))
(assert-equal t (if (ovw-write-file "/OVWG.TXT" (create-vector 50 66)) t nil))   ; 同塊数の縮小は通る
(assert-equal 50 (file-length "/mnt/OVWG.TXT"))
(assert-equal 50 (length (ovw-read-file "/OVWG.TXT")))
(assert-equal t (if (ovw-write-file "/OVWG.TXT" (create-vector 3000 67)) t nil)) ; 増は通る
(assert-equal 3000 (file-length "/mnt/OVWG.TXT"))
(assert-equal nil (ovw-write-file "/OVWG.TXT" (create-vector 100 68)))           ; 塊が減ると nil
(assert-equal 3000 (file-length "/mnt/OVWG.TXT"))
(assert-equal 3000 (length (ovw-read-file "/OVWG.TXT")))
(assert-equal 67 (elt (ovw-read-file "/OVWG.TXT") 2999))

;; P9 読み書き両用は成功するが大きさが縮まず、末尾に古い中身が残る(静かな破壊)
(defglobal *ovw-s4* (open-io-file "/mnt/OVWG.TXT"))
(ovw-fill *ovw-s4* 90 5)
(close *ovw-s4*)
(assert-equal 3000 (file-length "/mnt/OVWG.TXT"))
(assert-equal 90 (elt (ovw-read-file "/OVWG.TXT") 0))
(assert-equal 90 (elt (ovw-read-file "/OVWG.TXT") 4))
(assert-equal 67 (elt (ovw-read-file "/OVWG.TXT") 5))
(assert-equal 67 (elt (ovw-read-file "/OVWG.TXT") 2999))

;; P10 高い層は 3 通りとも同じ文面で落ちる(書く量に依存しない)
(defglobal *ovw-c3* (ovw-catch (lambda () (write-vector-to-file "/mnt/OVWG.TXT" (create-vector 10 69)))))
(assert-equal "open-output-file: /mnt/OVWG.TXT: cannot resolve file" (ovw-msg *ovw-c3*))
(defglobal *ovw-c4* (ovw-catch (lambda () (write-vector-to-file "/mnt/OVWG.TXT" (create-vector 3000 70)))))
(assert-equal "open-output-file: /mnt/OVWG.TXT: cannot resolve file" (ovw-msg *ovw-c4*))
(defglobal *ovw-c5* (ovw-catch (lambda () (write-vector-to-file "/mnt/OVWG.TXT" (create-vector 5000 71)))))
(assert-equal "open-output-file: /mnt/OVWG.TXT: cannot resolve file" (ovw-msg *ovw-c5*))
(assert-equal 3000 (file-length "/mnt/OVWG.TXT"))
(assert-equal 67 (elt (ovw-read-file "/OVWG.TXT") 2999))

;; 同じ道を読み込み用に開くのは通る(解決そのものは出来ている)
(defglobal *ovw-s5* (open-input-file "/mnt/OVWG.TXT"))
(assert-equal 90 (read-byte *ovw-s5*))
(close *ovw-s5*)
```

**この台本を常設の試験へ移すときの注意**: いまの `test/lisp/fat16_test.lisp:194` /
`fat32_test.lisp:187` と**期待値が正面から衝突する**(あちらは「減る書き換えは nil」を
期待している)。直しと同じ PR で両方を動かすこと。
配線の名簿(`tools/check_test_count_ratchet.py --wiring`)と件数の照合も更新対象。

---

## 8. 関連文書

- `documents/load-path-contract.md:154-175` … 今回の症状の既出の記録(「残っている問題」の節)
- `Makefile:1203-1204` … 「#199(上書き保存の壊れ)と同じ層を踏む」という申し送り
- `docs/investigation/app-terminal-io.logic.md` §7 … 仮説 2 の出どころ(★ のまま置かれていた)
- `docs/investigation/app-terminal-io.refs.md:289` … そのときに辿った同じ連鎖
- `docs/investigation/fat-overwrite.predictions.md` … **測る前の宣言**(規則 11)
