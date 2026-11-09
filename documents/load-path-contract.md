# 読み込み命令のパスの契約と、アプリの置き場所

- 対象: `load`(`src/c/load.c` の `cc_load`)
- 対応する issue: #193(開き方)/ #194(隠れていた前提)/ #195(置き場所と試験)
- 前の調査: `docs/investigation/m2-load-from-disk.logic.md` / `.refs.md`
- 前の作業: #196 / PR #198(読み取りと読み込みを記憶域の整理に耐える形へ直した)

---

## 1. 契約

`load` に渡す名前は、**先頭の区切りで経路が分かれる。**

| 形 | 経路 | 例 |
|---|---|---|
| **先頭が `/` でない** | **ホスト共有(9P)のエクスポートルートからの相対指定。マウント表は引かない** | `(load "test/lisp/x_test.lisp")` |
| **先頭が `/`** | **マウント表(`*mounts*`)で解決する。** `/9p` 配下は登録が無くても常にホスト共有へ解決される | `(load "/app/hello.lisp")` / `(load "/9p/tmp/x.lisp")` |
| 先頭が `/` で、どのマウントにも一致しない | **`no such mount` で通知する。ホスト共有へ落とさない** | `(load "/no-such-mount/x.lisp")` |

> **心覚え: 相対 = 母艦(開発時)、絶対 = 機械の中。**

### なぜこの形なのか

- `load` に渡されている名前は **2 件を除いて全部「先頭に区切りが無い相対指定」**で、
  マウント表の解決は絶対パス前提である。素直に載せ替えると**全部が解決不能になり、
  試験は「落ちる」より先に「何も走らなくなる」**(`logic.md` §8-1/§8-2)。
- **「相対に `/` を補う」案は採らない。** 根(`"/"`)を割り当てる試験が実在し
  (`test/lisp/partition_test.lisp`)、**同じ起動の後続の `load` がディスクへ流れて落ちる**
  (`"/"` マウントは何にでも一致する)。
- **「解決できないときホスト共有へ落とす」案も採らない。**
  既存は無傷になるが、**割り当て名の綴り間違いがホスト共有の文面で失敗して
  原因が分からなくなる**(`logic.md` §9-5)。

### 決めなかったこと

- **`load` に作業ディレクトリ(`*cwd*`)を効かせない。** `ls` / `cd` / `cat` は効かせているので
  揃えたくなるが、**「相対 = ホスト共有」の契約と正面からぶつかる。**
  将来その契約を変えるときに一緒に考える。**効いていないことは試験で固定してある**
  (`test/lisp/load_disk_test.lisp` の §4)。
- **打ち切りの報告が母艦に残らない件は #197。** `print_load_abort` は現在のプロセスの
  画面バッファへ出すので、`-display none` の試験では誰も読まない(ここでは直していない)。

---

## 2. 変わったもの / 変わらなかったもの(#194)

| # | 前提 | どうなったか | どこで固定したか |
|---|---|---|---|
| 1 | **相対パスの扱い** | **変わらない。** 先頭に区切りが無い名前はホスト共有のまま。マウント表も引かない(根を割り当てても変わらない) | `test/c/load_test.c` の `test_load_relative_path_stays_on_9p_even_with_root_mount`(9P の open が呼ばれた回数と**渡されたパスそのもの**を見る) / `test/lisp/io_signal_test.lisp` の `*io-ok-load*` |
| 2 | **絶対パスの扱い** | **変わった。** `/9p/...` はマウント表で `/9p` を剥がしてから開く(以前は剥がさずそのまま 9P のパスにしていた) | `test/c/load_test.c` の `test_load_absolute_9p_path_strips_prefix` / `io_signal_test.lisp` の `*io-ok-path*` |
| 3 | **解決できない絶対パス** | **変わった。** 以前は 9P へ直行して 9P の文面で落ちていた。いまは `no such mount` | `test/c/load_test.c` の `test_load_unresolvable_absolute_path_does_not_fall_back_to_9p`(**9P の open が 1 度も呼ばれないこと**を見る) / `io_signal_test.lisp` / `load_disk_test.lisp` §2 |
| 4 | `LOAD_PATH_MAX` | **変わらない**(256)。`os_mount_resolve` へ渡す相対パスのバッファだけ、規約側の名前 `STREAM_PATH_MAX`(同じ 256)を使う。超過が黙って切れる性質も変わらない | — (値が同じなので挙動の差が無い) |
| 5 | **エラー文面**(`os_signal_io_error("load", ...)`) | **先頭は変わらない**(`"load: " + path + ": " + 理由`)。**理由の語が増えた**: `no such mount` / `cannot read file` | `io_signal_test.lisp`(`"load: "` の 6 文字 + 理由の全文)/ `load_disk_test.lisp` §2・§3 |
| 6 | **`print_load_abort` の出力先** | **変わらない**(現在のプロセスの画面バッファ)。**母艦には残らない。#197** | `test/c/load_test.c` の打ち切りの段(キャプチャして文面を見る) |
| 7 | **マウントが無い状態の `load`** | **変わらない。** `*mounts*` は起動直後は空だが、`/9p` 配下は組み込みで解決でき、相対指定はマウント表を見ない | `test/c/load_test.c`(`reset_mounts()` した状態で相対・`/9p` の両方を通す) |
| 8 | **FAT 経路の記憶域の安全性** | **新しく要るようになった。** 包む・包んだ以上は反復ごとに取り直す・`env` を守る・運転手の非局所脱出を拾う。**守りは #196 で先に入れてある** | `test/c/load_test.c` の `test_load_from_wrapped_stream_survives_gc` / `test_load_protects_env_across_open` / `test/lisp/load_gc_stress_test.lisp` §4 |

**件数を数えたいときは導出する**(規則 13)。パスの契約を固めている C 側の試験は:

```
grep -n 'test_load_relative_path\|test_load_absolute_9p_path\|test_load_unresolvable_absolute' test/c/load_test.c
```

ゲスト側は:

```
grep -rn 'no such mount\|cannot read file' test/lisp/
```

---

## 3. アプリの置き場所(決定 D2b。#195)

**アプリ専用の FAT32 ディスクを 1 枚足す。**

| | 決めたこと |
|---|---|
| ホスト側の原本 | `apps/*.lisp` |
| イメージ | `images/app_fat32.img`(`Makefile` の `APP_DISK_IMG`、40MB、FAT32) |
| 依存 | **`apps/*.lisp` だけ。** カーネルの再ビルドから独立している |
| 焼き込み | **母艦側で `cp`**(既存の作法)。ゲスト内で書いてから読む形にすると**書き込みの遅さを踏む** |
| 接続 | **Primary の slave(`bus=0,unit=1`)に固定。どの QEMU 起動にも付く**(9P を外した起動にも) |
| ゲストでの割り当て | `(mount "/app" (%device-named "ide-primary-slave") ':fat32)` |
| ディスク上の置き場所 | ディスクの**根**(`/app/hello.lisp`) |

### なぜ専用ディスクなのか

| 案 | 判断 |
|---|---|
| データ用ディスク(`QEMU_DISK_IMG`) | ✗ **9P を外した起動に付いていない** — アプリが要る場所で使えない |
| 起動用ディスク(ESP) | ✗ 常にあるが `$(TARGET)/$(SRC)/$(HDR)` 依存で**ソースを触るたび作り直される** → **アプリだけ入れ替えられない**。これは M2 が解消しようとしている結合そのもの |
| **アプリ専用の新しいディスク** | ✅ **カーネルの再ビルドから独立。** FAT32 なので **`.lisp`(拡張子 4 文字)が置ける** |

**FAT32 を選ぶのは `.lisp` を使うため。** 旧方式(FAT16)は 8.3 名しか扱えないので、
既存のフィクスチャは `TEST.LSP` になっている。

### 装置の名前は構成で動く

探査順は **secondary-master → secondary-slave → primary-master → primary-slave** で、
検出できたものだけを詰めて `blk0, blk1, ...` と振る(`src/c/block_device.c`)。
したがって**同じディスクの `blk` 番号が構成で動く。**

- Primary の slave に固定すると、**チャネルの名前は常に `"ide-primary-slave"`** になる
- 名前引きは `(%device-named "ide-primary-slave")`(`src/lisp/device.lisp`)。
  **既存の試験が各自に書いていた名前引きの共通化である**
- **自動の割り当ては 1 つも無い**ので、読む前に `mount` を呼ぶ前段が必ず要る

---

## 4. 試験の段

| 段 | 何を見るか |
|---|---|
| `make test`(`test/c/load_test.c`) | パスの契約の 3 分岐。**ホスト共有へ落ちていないことは「9P の open が呼ばれた回数」で見る**(戻り値だけでは 9P の open 失敗と区別が付かない) |
| `make test-qemu-load-disk`(`test/lisp/load_disk_test.lisp`) | ディスクの `.lisp` が `load` できる / 相対が無傷(同じ中身をホスト共有からも読む)/ 解決できない絶対パスの文面 / 割り当てはしたがファイルが無い場合の文面 / `*cwd*` が効かないこと |
| `make test-qemu-no-9p` | **9P を外した起動でディスクのアプリが読めること。** 判定は `src/c/kernel.c` の `run_boot_smoke` がシリアルへ出す。**件数を増やしたら `Makefile` の `grep` の数字も直す** |
| `make test-qemu-load-gc-stress` | **記憶域の整理を強制しても、ディスク経由の `load` が 1 回ずつしか評価しないこと**(#196 からの申し送り)。材料は `apps/load-gc-stress.lisp` |
| `make test-qemu-all` の全段 | **陰性対照として一番強い。** 台本自身の読み出しと `isiki-test-load` が相対指定の `load` に乗っているので、**相対が壊れたら全段が落ちる** |

### 題材の形は意図である(#196 の実測)

ディスク経由の `load` の検出器は、**題材が素直だと穴が開いていても気づけない。**

- **入れ子のリストが要る。** 平らなリストは要素を全部読んでから cons を積むので、
  読んでいる途中に確保が入らない(= 整理が読取の途中に挟まらない)
- **生存集合を膨らませるフォームを間に挟まないと再現しない。**
  生存集合が小さいと、複製先が古い複製の位置まで届かないので、
  古い複製を読み続けても中身が壊れない

`apps/load-gc-stress.lisp` はこの 2 つを両方持っている。**どちらかを抜くと検出器は鳴らなくなる。**

### 陽性対照(規則 8。2026-11-09 に実施)

**この検出器が対象を見られることを先に示した。** `os_load_from_stream` の
`lisp_val_t handle = stream->self_handle;` を `nil` に変えて #196 より前の形
(生ポインタを輪の全体で使い回す)に戻し、`make test-qemu-load-gc-stress` を走らせた:

```
[NG] (LOAD "/app/load-gc-stress.lisp") => EVAL-ERROR (expected T)
[NG] *APP-LGS-COUNT* => 1 (expected 2)
[NG] *APP-LGS-NESTED* => NIL (expected ((1 2) (3 (4 5)) ((6) 7)))
[NG] *APP-LGS-STRINGS* => NIL (expected ("alpha" "beta" "gamma"))
[NG] (LENGTH *APP-LGS-LIVE*) => 0 (expected 5)
```

**節 3(ホスト共有経由の `load`)は通ったまま**で、節 4 だけが落ちた
(9P のストリームは C のローカルにあって整理で動かないので、
ここが落ちないことがディスク経路との切り分けになっている)。
`*APP-LGS-COUNT*` が 2 ではなく **1** になるのが「古い複製から読み続けた」の印である。

### この段が黙って縮んでいた(2026-11-09 に見つけた)

`make test-qemu-load-gc-stress` を**同じ FAT16 イメージに対して 2 回続けて走らせると**、
2 回目の `open-output-file "/lgs/GCSTRES.LSP"` が `cannot resolve file` で失敗する。
試験本体は `(if (require-gc-debug-build ...) (progn ...))` という**1 つのトップレベル
フォーム**なので、そこで打ち切られると**以後の節が全部飛ぶ。**
打ち切りの理由は画面バッファにしか出ない(#197)ため、ホスト側に残るのは

```
#count test/lisp/load_gc_stress_test.lisp 3
==== isiki tests: 5 passed, 0 failed ====
```

で、**`grep " 0 failed"` は通る。** 本来は `#count 19` である。
**pristine な `main` でも同じ**だったので、PR #198 以降ずっとこの形だった
(`test-qemu-all` は先頭でイメージを `rm` するので、通しで走らせる限り当たらない)。

対処: `test-qemu-load-gc-stress` が**イメージを毎回作り直す**ようにした
(段が「繰り返し走らせても同じ」になる)。

**残っている問題(直していない)**: 既にあるファイルへの `open-output-file` が
FAT16 で `cannot resolve file` になること自体。再現は上のとおり
(同じパスへの `open-input-file` は成功し、中身も読める)。
**打ち切りの理由が母艦に残らない件は #197。** この 2 つが重なると
「段が黙って縮む」が起きるので、**節の件数(`#count`)を見る習慣が要る。**
