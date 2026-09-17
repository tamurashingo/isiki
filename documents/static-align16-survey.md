# 静的領域（BSS / DATA / RODATA 等）のオブジェクトを16byte境界に置けるか

調査日: 2026-09-17 / ブランチ: `feature/static-align-survey`
派生元: **`feature/heap-align16`（PR #73、2026-09-17時点で未マージ）**。
PR #73 で Lispヒープ側が16byte境界になった状態を前提にするため、main ではなく
そちらから派生している。PR #73 → PR #72 の順に先行マージが要る。

本書は判定材料をそろえる調査であり、**本採用の実装は行っていない**。
Step 3 の試験的な変更はこのブランチ上にのみ存在する（第4章に明記）。

---

## 1. 結論

### 問1: Lisp の値として現れうるヒープ外オブジェクトは、どこに・何個・どういう形であるか

**5種類だけ。** ALIGN_AUDIT で実測したヒープ外ポインタ 1,095,850 回（延べ）を
**100% 分類できた**（未分類 0 件、表の overflow 0 件）。

| オブジェクト | 置き場所 | 形 | distinct | 延べ | 境界外れ |
|---|---|---|---|---|---|
| `g_nil_cell`（nil） | `.bss` 静的 | 単体変数 | 1 | 796,906 | **100%** |
| `g_za_fn_cell_cache_slots[i]` | `.bss` 静的 | 配列要素 | 957 | 67,335 | **50.0%** |
| `g_za_quote_slots[i]` | `.bss` 静的 | 配列要素 | 323 | 22,750 | **50.0%** |
| `g_za_number_slots[i]` | `.bss` 静的 | 配列要素 | 116 | 8,234 | **50.0%** |
| `g_za_lambda_slots[i]` | `.bss` 静的 | 配列要素 | 77 | 5,329 | **49.4%** |
| `block_device_t*`（2台） | boot allocator | 動的（ヒープ外） | 2 | 216 | **100%** |
| Immobilized Space（Function Cell / 環境ページ） | 専用静的領域 | 動的（専用アロケータ） | — | 195,080 | **0%** |

「静的」と言えるのは実質 **`g_nil_cell` と4本のスロットプールだけ**で、
それ以外はアロケータ経由である。`.rodata` / `.data` に置かれた Lisp オブジェクトは
**存在しない**（第2章2.6）。

### 問2: 16byte境界に置く手段はあるか。どの段階で保証されるか

**ある。全段階で保証できる。**

| 段階 | 状況 |
|---|---|
| コンパイラ | GCC 12（mingw）。`__attribute__((aligned(16)))` がシンボルとセクションの両方に反映される。`-fno-common` が GCC 10 以降の既定なので COMMON シンボルの穴も無い |
| 配列要素 | **`aligned` 属性だけでは揃わない。要素サイズを16の倍数にする必要がある**（現在 `lisp_val_t` = 8byte） |
| リンカ | **リンカスクリプトを使っていない**（mingw-gcc が PE を直接出力）。`.bss` / `.data` / `.rdata` はいずれもセクションアライン `2**4` = 16 |
| EFI 変換 | **ELF→PE の変換段階が無い**。`x86_64-w64-mingw32-gcc` が PE32+ を直接出力する |
| ロード | `SectionAlignment = 0x1000`。実測でロードのずれ（slide）は **4096 の倍数**で、3つの異なるシンボルで同一。したがって **リンク時のアライン mod 16 はロード後も保たれる** |
| ヒープ外の動的確保 | `os_boot_alloc(size, align)` の `align` は呼び出し側指定。実呼び出しは2箇所だけで、どちらも `8`。UEFI の `AllocatePool` / `AllocatePages` は**一度も呼んでいない** |

### 問3: 手段がない（割に合わない）ものはどれか

**現時点で「手段が無い」ものは1つも無い。** 費用の順に:

| 対象 | 変更 | 規模 |
|---|---|---|
| `g_nil_cell` | `aligned(8)` → `aligned(16)` | **1行** |
| `block_device_t*` | `os_boot_alloc(..., 8)` → `..., 16` | **2行**（`block_device.c:79,84`） |
| JITリテラルスロット4本 | 要素を16byteにする（`lisp_val_t` → 8byteパディング付き構造体） | **約24行**、BSS +30,720 byte |

### 問4: 監査の強制対象を「例外リスト以外すべて」に広げられるか

**広げられる。** 上の3つを直せば、**例外リストが空のまま**「Lisp値として現れる
ポインタはすべて16byte境界」を強制できる（第6章の案を比較）。

「ヒープ外だが正当なアドレス」の判別は、**そもそも不要になる**のが要点である。
現在の `align_addr_is_in_lisp_heap` は「保証範囲が狭いから範囲で絞る」ための
仕掛けであって、範囲外にも保証が及べば、タグだけで判定してよい。

---

## 2. Step 1: ヒープ外オブジェクトの棚卸し

### 2.1 収集方法

`gc_copy_value` の入口で、From/To 空間の外を指すタグ付き値を採取した
（`ALIGN_AUDIT=1` のときだけ有効。`runtime.c` の `align_ext_note`）。
`gc_copy_value` は GC のたびに全ルートと全生存オブジェクトの全フィールドを通るので、
**Lisp値として流通しているポインタ**はここで網羅できる。

採取は2段構えにしてある。

1. **領域別の延べ件数**: `os_addr_region` の戻り値（2 = Immobilized Space、
   4 = それ以外）ごとに、延べ回数と境界外れ回数を数える。
2. **アドレス別の表**: region 4 だけ、オープンアドレッシングのハッシュ表
   （容量8192、負荷率0.5で打ち切り）でアドレスごとに数える。`nm` と突き合わせて
   シンボルへ逆引きするので、値そのものを残す必要があるのはこちらだけ。

当初は全領域を単一の線形表（512件）で持っていたが、Immobilized Space のスロットで
埋まって **23.8% が overflow** になった。見たい対象で表を分けることで overflow を
**0 件**にできた。

### 2.2 逆引きの方法

UEFI のロードアドレスは起動ごとに変わるので、既知シンボルの実行時アドレスを
報告に混ぜ、`nm` の空間へ戻してから探索している。

```
[ALIGN] ext-anchor os_heap_used_ratio=0x275f6ff g_nil_cell=0x3c56b08 imm_space=0x2c26000
```

実測（`QEMU_MEM=96M`、GC 72回、`tmp/sa/map_96M.serial` と同じ起動の `nm`）:

```
runtime anchor=0x275f6ff  nm anchor=0x2446c36ff  slide=0xfffffffdbe09c000
slide % 4096 = 0
g_nil_cell : runtime=0x3c56b08  nm=0x245bbab08  slide 一致
g_imm_space: runtime=0x2c26000  nm=0x244b8a000  slide 一致
```

**3つの異なるシンボルで slide が完全に一致し、かつ 4096 の倍数**である。
これが問2の「ロード段階」の根拠になる（第3章3.4）。

### 2.3 棚卸し表（`QEMU_MEM=96M`、全試験、GC 72回）

延べ観測 1,095,850 回。

| # | オブジェクト | 所属 | 形 | 現在のアライン | distinct | 延べ回数 | 境界外れ | タグ |
|---|---|---|---|---|---|---|---|---|
| 1 | `g_nil_cell` | `.bss`（静的） | 単体変数 `UINT8[16]` | `aligned(8)` | 1 | 796,906 | 796,906（100%） | `CONS` |
| 2 | `g_za_fn_cell_cache_slots[i]` | `.bss`（静的） | `lisp_val_t[2048]` の要素 | 配列先頭16 / 要素stride **8** | 957 | 67,335 | 33,666（50.0%） | `RAWPTR` |
| 3 | `g_za_quote_slots[i]` | `.bss`（静的） | `lisp_val_t[1024]` の要素 | 同上 | 323 | 22,750 | 11,374（50.0%） | `RAWPTR` |
| 4 | `g_za_number_slots[i]` | `.bss`（静的） | `lisp_val_t[512]` の要素 | 同上 | 116 | 8,234 | 4,116（50.0%） | `RAWPTR` |
| 5 | `g_za_lambda_slots[i]` | `.bss`（静的） | `lisp_val_t[256]` の要素 | 同上 | 77 | 5,329 | 2,635（49.4%） | `RAWPTR` |
| 6 | `block_device_t*` | boot allocator | 動的（ヒープ外） | `os_boot_alloc(...,8)` | 2 | 216 | 216（100%） | `RAWPTR` |
| 7 | Immobilized Space | 専用静的領域 | `os_imm_slot_alloc` / `os_imm_page_alloc` | **16 / 4096** | —（表対象外） | 195,080 | **0** | `RAWPTR` |

**未分類: 0 件（0.0%）。** region 4 の延べ 900,770 回の内訳は
796,906 + 67,335 + 22,750 + 8,234 + 5,329 + 216 = 900,770 で完全に一致する。

内訳の出し方は2段階である。

1. `nm` への逆引きで解決: 1,474 / 1,476 アドレス（延べ 900,554 回、**99.976%**）
2. `nm` に当たらなかったもの: **2 アドレス（延べ 216 回、0.024%）**。
   これは第2.4節で boot allocator の領域内と特定し、`block_device_t` であることを
   オフセットのシミュレーションで確定させた

したがって**最終的に分類できなかったものは 0 件**である。
表の overflow も 0 件なので、観測を取りこぼした分も無い。

配列要素がちょうど 50% ずれるのは、配列先頭が16境界で要素 stride が 8 だから
（偶数添字が16境界、奇数添字が 8 mod 16）。`g_za_lambda_slots` だけ 49.4% なのは、
観測された添字の偶奇がたまたま完全に半々でなかったため。

### 2.4 `block_device_t*` の2件を特定した根拠

逆引きで `nm` のシンボルに当たらなかった2件は、boot allocator の領域内だった。

| アドレス | boot allocator 先頭からのオフセット | low4 |
|---|---|---|
| `0x1b8c048` | 72（`0x48`） | 8 |
| `0x1b8c1a8` | 424（`0x1a8`） | 8 |

この起動の UEFI 領域先頭は `OS Heap Target Address: 0x0000000001B8C000`（シリアル出力）。
`os_boot_alloc(size, 8)` を `ide_probe_one` の順に適用したシミュレーションと一致する:

| probe | slot オフセット | `block_device_t` オフセット | low4 |
|---|---|---|---|
| secondary-master | 0 | **72 (0x48)** ← 観測 | 8 |
| secondary-slave | 176 | 248 (0xf8) | 8 |
| primary-master | 352 | **424 (0x1a8)** ← 観測 | 8 |
| primary-slave | 528 | 600 (0x258) | 8 |

`sizeof(ide_bd_slot_t)` = 68、`sizeof(block_device_t)` = 104。QEMU が与える
ドライブは2台（`bus=0,unit=0` と `bus=1,unit=0`）なので、secondary-slave の
確保（オフセット248）は検出失敗で捨てられている（`block_device.c` のコメントどおり）。

**4スロットすべてが low4 = 8 になる**。`align=8` と 68/104 というサイズの組み合わせ上、
どのデバイスが検出されても100%外れる構造になっている。

### 2.5 ビルド依存であることの実測

同じソースでも `g_nil_cell` の配置はビルドで変わる。

| ビルド | `g_nil_cell` 実行時アドレス | low4 | region 4 の境界外れ率 |
|---|---|---|---|
| `ALIGN_AUDIT=1`（既定） | `0x3c56b08` | **8** | 848,913 / 900,770 = **94.2%** |
| `ALIGN_AUDIT=1 GC_DEBUG=1` | `0xdb57890` | **0** | 30,629 / 1,168,121 = **2.6%** |

GC_DEBUG ビルドでは nil がたまたま16境界に落ちるため、残る境界外れは
JITリテラルスロットだけになる。**「揃っているように見えるビルド」が実在する**
ことの直接の証拠であり、`aligned(8)` のまま放置してはいけない理由でもある。

### 2.6 ソースからの静的な探索（表に現れないもの）

| 探索対象 | 結果 |
|---|---|
| タグ付けマクロ（`\| TAG_*`）の全箇所 | 17箇所。うちヒープ外を生むのは `runtime.c:2527`（`g_nil_cell \| TAG_CONS`）、`3871`（Function Cell）、`3948`（環境ページ）、`3981`（リテラルスロット）、`ide_subprimitive.c:38`（`block_device_t*`）の5箇所のみ。残り12箇所はすべて `os_alloc_bytes` 由来 |
| `static` / グローバルの Lisp オブジェクト型変数 | `g_nil_cell` のみ |
| AOT ジェネレータが出す静的定数 | `static int __*_idx`（シンボル添字キャッシュ、2,651個）と **`static za_fn_meta_t __closure_meta`（286個）**。前者は `int`、後者は下記 |
| `.rodata` の Lisp オブジェクト | **無し**。`const` な Lisp オブジェクトはソース中に存在しない |
| アセンブリソース（`.S` / `.s` / `.asm`） | **1本も無い** |
| JIT がコードページへ埋めるデータ | 無し。`.quad` / `.byte` / `.align` 等のディレクティブは C ソース中に存在しない（第3章3.5） |

#### AOT の `static za_fn_meta_t __closure_meta` について

`transpile.lisp:1714,1716` が lifted closure ごとに出力する静的構造体で、
`lisp_compiled.c` に **286個**ある。`os_make_lifted_closure_with_meta`
（`runtime.c:3511`）へ `&__closure_meta` として渡され、
`MAGIC_FUNCTION_NATIVE` の **word1 に *タグを付けずに* 格納される**。

- **タグ付き Lisp 値ではない**ので、本調査の対象（「Lisp の値として現れるポインタ」）
  には該当しない。実測でも `gc_copy_value` の観測に現れない。
- ただし「Lisp から到達できる静的オブジェクト」ではあるので、将来 word1 を
  タグ付き表現へ変える場合は対象に入る。`sizeof(za_fn_meta_t)` = 24 で、
  単体変数なので `aligned(16)` を付ければ揃う（配列ではないので stride 問題は無い）。
  出力箇所はジェネレータの2行だけ。

---

## 3. Step 2: 置き場所ごとのアライン手段

### 3.1 コンパイラ段階

- ツールチェーン: `x86_64-w64-mingw32-gcc (GCC) 12-win32`、`-nostdlib -mno-red-zone -O1 -shared`
- `-fcommon` / `-fno-common` の明示指定は**無い**。GCC 10 以降は `-fno-common` が既定なので、
  COMMON シンボルが `.bss` でアラインを落とす経路は無い
- `-fpack-struct` / `-fdata-sections` / `-mcmodel` の指定も無い
- 既存の `__attribute__((aligned(N)))` は12箇所あり、`aligned(4096)` の
  `g_imm_space` / `g_stack_area` / `g_pt_pool` / `g_p9_*_buf` が**実測でロード後も
  4096境界を保っている**（`imm_space=0x2c26000`）。属性がバイナリまで通ることの
  実地の裏付けになる

### 3.2 配列要素

**`aligned` 属性は配列の *先頭* にしか効かない。** 要素のアドレスは
「先頭 + 要素サイズ × i」なので、要素サイズが16の倍数でなければ一部の要素がずれる。
実測の 50.0% はこれそのものである。

| プール | 型 | 要素サイズ | 上限 | 現在のBSS | 16byte化後 | 増分 |
|---|---|---|---|---|---|---|
| `g_za_quote_slots` | `lisp_val_t` | 8 | 1024 | 8,192 | 16,384 | +8,192 |
| `g_za_number_slots` | `lisp_val_t` | 8 | 512 | 4,096 | 8,192 | +4,096 |
| `g_za_lambda_slots` | `lisp_val_t` | 8 | 256 | 2,048 | 4,096 | +2,048 |
| `g_za_fn_cell_cache_slots` | `lisp_val_t` | 8 | 2048 | 16,384 | 32,768 | +16,384 |
| **合計** | | | 3,840 | 30,720 | 61,440 | **+30,720 byte** |

`.bss` は現在 0x13653c0 = 20.3MB（大半は `IMM_SPACE_SIZE` 16MB と
プロセススタック）なので、+30KB は **+0.15%** にすぎない。

**JIT の出力は stride に依存していない。** これが重要で、スロットのアドレスは
`&g_za_quote_slots[op->param_index]` として **C 側で計算した絶対値**を
`jit_movabs_reg` で即値として埋めている（`za.c:1470,1478,5558`）。
生成コード側に `base + 8*i` のようなインデックス計算は無いので、
要素サイズを変えても**出力される機械語の形は変わらない**。

アドレス → 添字の逆算は `za_free_literal_slot`（`za.c:1121-1147`）の4本の
範囲検査だけで、いずれも C のポインタ演算（`addr - g_za_quote_slots`）なので
型を変えれば自動で追随する。

触る行は全部で約24行（宣言4 + 添字アクセス/アドレス取得16 + 範囲検査4）。

### 3.3 リンカ段階

- **リンカスクリプトは使っていない**。`x86_64-w64-mingw32-gcc` に
  `-Wl,--subsystem,10 -Wl,--entry,EfiMain` を渡すだけで、`-T` は無い。
  したがって「出力セクション定義がアラインを落とす」経路は存在しない
- 最終バイナリのセクション（`objdump -h`）:

```
Idx Name          Size      VMA               Algn
  0 .text         00208ca0  00000002446c1000  2**4
  1 .data         00002ad0  00000002448ca000  2**4
  2 .rdata        00007290  00000002448cd000  2**4
  3 .pdata        00007938  00000002448d5000  2**2
  4 .xdata        00008c58  00000002448dd000  2**2
  5 .bss          013653c0  00000002448e6000  2**4
```

`.bss` / `.data` / `.rdata` はいずれも `2**4` = **16**。VMA もすべて 0x1000 の倍数。

### 3.4 EFI 変換・ロード段階

- **ELF→PE の変換段階は存在しない**。mingw-gcc が PE32+ を直接出力する
  （`file` の出力: `PE32+ executable for EFI (application), x86-64`）
- PE ヘッダ: `SectionAlignment = 0x1000`、`FileAlignment = 0x200`、
  `ImageBase = 0x2446c0000`、`DllCharacteristics = 0x160`
  （`HIGH_ENTROPY_VA | DYNAMIC_BASE | NX_COMPAT`）。`.reloc` が存在するので
  **再配置されうる**
- 実測: 3つのシンボル（`os_heap_used_ratio` / `g_nil_cell` / `g_imm_space`）で
  slide が完全に一致し、**`slide % 4096 == 0`**。UEFI の `LoadImage` が
  ページ単位で確保することと整合する
- したがって **リンク時のアドレス mod 16 は、ロード後の実アドレスでもそのまま保たれる**。
  「`nm` で16の倍数なら実行時も16の倍数」と言ってよい（第4章で実地に確認）

### 3.5 JIT のコードページ

指示書は「JIT は AT&T 構文のアセンブリ文字列として命令を出力する」を前提にしているが、
**現在の `za.c` はそうなっていない**。命令は `jit_emit8` / `jit_emit32` / `jit_emit64`
（71箇所）で**生バイト列として直接組み立てて**おり、`__asm__` は3箇所だけ、
しかもすべて `cpuid`（命令キャッシュのシリアライズ）で、コード生成とは無関係である。

したがって `.quad` / `.align` といったアセンブラディレクティブは存在せず、
**コードページにデータを置く literal pool も無い**。コードページ自体は
`os_imm_pages_alloc_contiguous`（4096境界）から取る。

この前提の違いは PR #72 の調査でも確認済みで、そちらの報告書2.3節にも記録がある。

### 3.6 ヒープ外の動的確保

- `os_boot_alloc(size, align)`（`runtime.c:1478`）は呼び出し側指定のアライン。
  実呼び出しは **2箇所だけ**で、どちらも `align=8`:
  - `block_device.c:79` `os_boot_alloc(sizeof(ide_bd_slot_t), 8)`
  - `block_device.c:84` `os_boot_alloc(sizeof(block_device_t), 8)`
  領域の先頭は UEFI の `EfiConventionalMemory` 記述子の `PhysicalStart`
  （`main.c:337`）で、UEFI 仕様上ページ境界。**先頭は問題なく、`align` 引数だけが原因**
- `AllocatePool` / `AllocatePages` は `main.c` の EFI Boot Services 構造体の
  **フィールド宣言としてのみ存在し、一度も呼ばれていない**
  （`grep` で呼び出し 0 件）。UEFI プールメモリが Lisp 値になる経路は無い
- Immobilized Space（`os_imm_slot_alloc` / `os_imm_page_alloc` /
  `os_imm_pages_alloc_contiguous`）は PR #73 で `OS_HEAP_ALIGN` 経由になっており、
  実測でも境界外れ **0 件**

---

## 4. Step 3: 試験結果

**このブランチ（`feature/static-align-survey`）にのみ存在する試験的な変更である。**
本採用の判断は行っていない。実験は3つを順に積み上げ、`QEMU_MEM=96M` の全試験で
ヒープ外ポインタの境界外れ件数を測った。

### 4.1 実験A: `g_nil_cell` を `aligned(16)` にする

変更は1行（`runtime.c:266`）。

| 段階 | 変更前 | 変更後 |
|---|---|---|
| ソース | `__attribute__((aligned(8)))` | `__attribute__((aligned(16)))` |
| `nm`（リンク後） | `0000000245bbab08` → low4 = **8** | `0000000245bbab10` → low4 = **0** |
| ロード後の実アドレス | `0x3c56b08` → low4 = **8** | `0x3c56b10` → low4 = **0** |
| region 4 の境界外れ | 848,913 | **52,007** |
| 試験結果 | 3269 passed / 0 failed | 3269 passed / 0 failed |

減った 796,906 件は `g_nil_cell` の観測回数とちょうど一致する。
**ソースの属性 → `nm` → ロード後の実アドレス → 監査の計数**が
1本につながっていることの実地の確認になっている。

### 4.2 実験B: JITリテラルスロットの要素を16byteにする

```c
typedef struct {
    lisp_val_t v;
    UINT64     _pad;   /* 要素strideを16byteにするためだけのパディング */
} za_slot_t;
static za_slot_t g_za_quote_slots[ZA_MAX_QUOTE_SLOTS] __attribute__((aligned(16)));
```

4本のプールすべてに適用。変更したのは:

| 種類 | 箇所数 |
|---|---|
| 宣言（`lisp_val_t[]` → `za_slot_t[]` + `aligned(16)`） | 4 |
| 代入・アドレス取得（`arr[i]` → `arr[i].v`、`&arr[i]` → `&arr[i].v`） | 16 |
| `za_free_literal_slot` の範囲検査とアドレス→添字の逆算 | 4ブロック |
| 合計 | **約24行、1ファイル（`za.c`）** |

範囲検査は `lisp_val_t*` と `za_slot_t*` の型が食い違うので、byte 単位の比較へ
書き換えたうえで `sizeof(za_slot_t)` で割って添字に戻している。

**JIT の出力は1バイトも変わらない。** スロットのアドレスは
`&g_za_quote_slots[i].v` として C 側で計算した絶対値を `jit_movabs_reg` の即値に
埋めているだけで、生成コードに `base + 8*i` のようなインデックス計算が無いため。
事前に `.quad` / `.align` を含むアセンブリ文字列が無いことも確認済み（第3章3.5）。

`nm`（リンク後、実験B適用時）:

```
0000000245be0190 b g_za_fn_cell_cache_slots
0000000245be89b0 b g_za_lambda_slots
0000000245bea9d0 b g_za_number_slots
0000000245bee9f0 b g_za_quote_slots
```

4本とも low4 = 0。要素 stride が16なので、**全要素が16境界**になる。

### 4.3 実験C: `block_device_t` を `align=16` で確保する

`block_device.c:79,84` の `os_boot_alloc(..., 8)` を `..., 16` にする（2行）。
`slot` 側も16にしないと `bd` の位置がずれるので両方変える。

### 4.4 A+B+C を適用した結果

| | region 2（Immobilized Space） | region 4（静的・ブート時確保） | 試験 |
|---|---|---|---|
| 変更前 | 195,080 / 境界外れ **0** | 900,770 / 境界外れ **848,913**（94.2%） | 3269 passed / 0 failed |
| 実験A のみ | 195,080 / **0** | 900,770 / **52,007**（5.8%） | 3269 passed / 0 failed |
| **A+B+C** | 195,079 / **0** | 900,658 / **0**（**0%**） | **3269 passed / 0 failed** |

**ヒープ外の Lisp ポインタの境界外れが 0 件になった。**
ヒープ内（PR #73 で保証済み）と合わせて、この時点で
「Lisp の値として現れるポインタはすべて16byte境界」が実測で成立している。

### 4.5 実験D: 回帰と、JIT を厚く踏む条件

| 検証 | 結果 |
|---|---|
| `make test`（**既定ビルド**、`ALIGN_AUDIT` 無し） | **8170 OK / 0 NG**（PR #73 と同じ。`za_slot_t` 化で壊れていない） |
| 強制GC + JIT（`gc-stress 1000`、`GC_DEBUG=1`、GC 231回） | **733 passed / 0 failed**、region 4 の境界外れ **0**（変更前は 30,629） |

強制GC条件は `init_test` + `za_test` を回すのでリテラルスロットを実際に多く作る。
ここで 30,629 → 0 になったことが、実験B が机上でなく実使用で効いていることの確認になる。
`violations=0`（ヒープ内の強制判定）も維持されている。

---

## 5. 例外リストに残すべきもの

**現時点で、例外リストに残さなければならないものは無い。**

第4章のとおり、3つの変更（合計 約27行）で既知のヒープ外オブジェクトはすべて
16byte境界に乗る。したがって「例外リスト以外すべて強制」は、
**例外リストが空のまま**達成できる。

将来 Lisp 値になりうるが、現在は対象外のもの:

| 対象 | 現状 | 対象外の理由 |
|---|---|---|
| `za_fn_meta_t`（AOT の `__closure_meta` 286個、`os_fn_meta_alloc` 経由の分） | `MAGIC_FUNCTION_NATIVE` の word1 に**タグ無し**で格納 | タグ付き Lisp 値ではないので、タグのビットを消費しない。タグ付き表現へ変えるなら、単体変数なので `aligned(16)` で足りる（`sizeof` = 24、配列ではないため stride 問題も無い） |
| `os_stream_t`（`MAGIC_STREAM` の word1） | 同上（タグ無しの生ポインタ） | 同上。実体は `os_alloc_raw` 経由なのでヒープ内で、PR #73 で既に16境界 |
| bignum の limb 配列（word3） | 同上 | 同上 |

いずれも「word1/word3 に生ポインタを置く」という現在の設計が続く限り、
タグのアラインとは無関係である。

---

## 6. Step 4: 監査の強制範囲を広げる案の比較材料

現在の判定は「タグが該当し、**かつ From/To 空間内**」（`align_addr_is_in_lisp_heap`）。
これは PR #73 の時点で保証範囲がヒープに限られていたための絞りで、
保証が範囲外へ及べば不要になる。

| | 案A: 既知領域を列挙して範囲検査 | 案B: 専用セクションにまとめる | 案C: 範囲では絞らず、例外リストだけ除外 |
|---|---|---|---|
| 判定方法 | From/To + Immobilized Space + 静的 Lisp オブジェクトの範囲をすべて列挙 | `.lisp_static` にまとめ、`__lisp_static_start/end` で範囲を得る | タグが該当すれば無条件に強制。例外は個別アドレス/タグで除外 |
| 現行コードの変更箇所 | `align_addr_is_in_lisp_heap` に静的シンボルの範囲を追加（5シンボル分＝5範囲。プールが増えるたびに追記が要る） | 5シンボルへ `__attribute__((section(".lisp_static")))` + リンカへの範囲シンボル追加 + 判定1箇所 | `align_addr_is_in_lisp_heap` の呼び出しを**外すだけ**（1箇所） |
| 追加の前提 | 無し | **リンカスクリプトが無い**ため、範囲シンボルをどう得るかを新規に決める必要がある（mingw/PE で `__start_SECNAME` は ELF の機能で、そのままは使えない） | 第4章の3変更が入っていること |
| AOT ジェネレータへの影響 | 無し | `__closure_meta` を将来タグ付きにするならセクション指定の出力が要る | 無し |
| JIT への影響 | 無し（スロットプールの範囲を監査側に列挙するだけ） | プール4本へセクション属性 | 無し |
| 漏れ方 | **列挙漏れが「見逃し」になる**（新しい静的 Lisp オブジェクトを足した人が監査に追記し忘れると、揃っていなくても通る） | セクション指定忘れが「誤検出」になる（揃っていても範囲外として弾かれる → 気づける） | 揃っていないものを足すと**即座に失敗する**（気づける） |
| 備考 | 領域が増えるほど判定コストが線形に増える。`gc_copy_value` は全フィールドを通るのでホットパス | ソースに `section(` の前例が1つも無い。PE での挙動は未確認 | いちばん単純。ただし第4章の3変更が前提 |

**漏れたときにどちら側へ倒れるか**が案ごとに違う点が、比較の要点である。
案A は列挙漏れが見逃しになり（安全側に倒れない）、案B・案C は漏れが検出側に倒れる。

---

## 7. 未確認事項

| 項目 | 理由 |
|---|---|
| `.rodata` に Lisp オブジェクトを置いた場合のマージ挙動 | 現在 `const` な Lisp オブジェクトが存在しないため、確認する対象が無い。将来置く場合は `-fmerge-constants` 相当の結合でアラインが落ちないかを別途確認する必要がある |
| 案B（専用セクション）の PE での実挙動 | ソースに `section(` の前例が無く、リンカスクリプトも無いため、`__start_SECNAME` 相当の範囲シンボルを mingw/PE でどう得るかは未調査。案の比較材料としてのみ挙げている |
| ASLR が実際に効いた場合の挙動 | `DllCharacteristics = 0x160` に `DYNAMIC_BASE` が立っているが、今回の OVMF では毎回同じ slide（4096の倍数）だった。別のファームウェアでページ境界でないロードが起きないことは、仕様（UEFI の `LoadImage` がページ単位で確保する）に依拠しており、実機での確認はしていない |
| 実機（非QEMU）での確認 | 環境が無いため未実施。静的領域の配置はリンカとビルドフラグに依存するので、実機ビルドでの再確認が要る |
| `AllocatePool` を将来使い始めた場合 | 現在1度も呼んでいないため未確認。UEFI 仕様は `AllocatePool` に8byte境界しか要求していないので、使い始めるなら Lisp 値にする前にアラインの確認が要る |
| 実験A/B/C の性能への影響 | 本調査では境界の成立だけを見ており、BSS が +30,720 byte 増えること以外の性能測定はしていない。JIT の出力バイト列が変わらないことは確認済みなので、実行時コストの増分は無いと見込まれるが、計測はしていない |

---

## 8. 本採用（2026-09-17）

第4章の実験A・B・Cを本採用し、16byte境界が**後から崩れたときに必ず検出される**
状態にした。以降「Lispの値として現れるポインタは、例外なく16byte境界」が
コンパイル時・ブート時・監査の3段階で検査される。

### 8.1 rebase / base の確認

**PR #73 の内容は main に入っていなかった。** 指示書は「PR #72・#73 はマージ済み」を
前提にしていたが、実際には:

| PR | マージ先 | 時刻 |
|---|---|---|
| #72 | `main` | 15:38:36 |
| #73 | **`feature/alignment-audit`** | 15:38:48 |

`feature/alignment-audit` は #72 で main へマージされた**後**に #73 を受け取ったため、
#73 のコミット `9e6822b`（ヒープの16byte境界化）は main に到達していない。

```
$ git merge-base --is-ancestor 9e6822b origin/main; echo $?
1                      # = 入っていない
$ git show origin/main:src/c/runtime.h | grep -c OS_HEAP_ALIGN
0                      # main には OS_HEAP_ALIGN が無い
$ git log --oneline origin/main..origin/feature/alignment-audit
1911cd6 Merge pull request #73 from tamurashingo/feature/heap-align16
9e6822b feat(gc): Lispヒープのバンプアロケータを16byte境界にする
```

このため **main へ rebase していない**。rebase すると PR #74 の差分に #73 の内容
（`Makefile` / `runtime.h` / `runtime_test.c` / `heap-align16-report.md`）が混ざり、
指示書が求める「PR #74 の変更だけになっていること」という確認自体が成立しないうえ、
#73 の作業が #74 経由で再配達される形になる。

現在の base（`feature/heap-align16`）に対する差分は、本採用分を含めても
**PR #74 自身の変更だけ**である:

```
$ git diff --stat origin/feature/alignment-audit..HEAD
 documents/static-align16-survey.md | ...
 src/c/block_device.c               | ...
 src/c/runtime.c                    | ...
 src/c/za.c                         | ...
```

**必要な対処**: `feature/alignment-audit` を main へマージし直すと #73 が main に入る
（`main` は `feature/alignment-audit` の祖先ではないので fast-forward ではなく
通常のマージになる）。それが済んだ時点で、PR #74 の base を main へ切り替えられる。

### 8.2 Step 1: 本採用した変更

実験用の `#ifdef` やフラグ切り替えは**一切残していない**（常に有効）。
アライン値はすべて `OS_HEAP_ALIGN` 経由で、`16` の直書きはしていない。
`-DOS_HEAP_ALIGN=8ULL` の比較ビルドでは、ヒープと静的領域が**一緒に**8へ戻る。

| # | 対象 | 変更 | 箇所 |
|---|---|---|---|
| A | `g_nil_cell` | `aligned(8)` → `aligned(OS_HEAP_ALIGN)` | `runtime.c:271` |
| B | JITリテラルスロット4本 | 要素を `za_slot_t`（`union { lisp_val_t v; UINT8 _pad[OS_HEAP_ALIGN]; }`、型に `aligned(OS_HEAP_ALIGN)`）にし、配列にも `aligned(OS_HEAP_ALIGN)` | `za.c:578-590` ほか約24行 |
| C | `block_device_t` / `ide_bd_slot_t` | `os_boot_alloc(..., 8)` → `..., OS_HEAP_ALIGN` | `block_device.c:79,88` |

`za_slot_t` を **struct + パディング配列ではなく union** にしたのは、
`OS_HEAP_ALIGN == sizeof(lisp_val_t)` のとき（= `-DOS_HEAP_ALIGN=8ULL` の比較ビルド）に
パディング配列がサイズ0になって壊れるのを避けるため。union なら素直に8byteへ縮む。

`aligned(16)` の直書きが残っている3箇所（`g_ist_stack` / `g_fpu_default_state` /
`g_jit_code`）は**Lisp値ではない**。割り込みスタックとFXSAVE領域はx86-64 ABIが16を
要求するもの、`g_jit_code` はJITのステージングバッファで、いずれも `OS_HEAP_ALIGN` と
連動させるべきものではないため、意図的に16のままにしてある。

#### JIT 出力が変わらないことの確認

スロットのアドレスは `&g_za_*_slots[i].v` として **C 側で計算した絶対値**を
`jit_movabs_reg` の即値に埋めるだけで、生成コードに `base + 8*i` のような
インデックス計算は無い。読み書きは `jit_mov_reg_from_mem_disp8(..., 0)` /
`jit_mov_mem_disp8_from_reg(..., 0, ...)`、いずれも REX.W + `disp8=0` の
**64bitアクセス1回**で、オフセット0（= `.v`）しか触らない。

実地の裏付けとして、`make test`（既定ビルド）が **8170 OK / 0 NG** で
PR #73 と同数のまま通っており、JIT を厚く踏む強制GC条件（GC 231回）も
**733 passed / 0 failed** で変わっていない。

### 8.3 Step 2: コンパイル時の検査

| 検査 | 箇所 |
|---|---|
| `OS_HEAP_ALIGN >= 8` | `runtime.h:85` |
| `OS_HEAP_ALIGN` が2のべき乗 | `runtime.h:86` |
| `OS_HEAP_ALIGN > TAG_MASK` | `runtime.h:88` |
| `sizeof(g_nil_cell) % OS_HEAP_ALIGN == 0` | `runtime.c:273` |
| `__alignof__(g_nil_cell) >= OS_HEAP_ALIGN` | `runtime.c:278` |
| `sizeof(za_slot_t) % OS_HEAP_ALIGN == 0` | `za.c:585` |
| `_Alignof(za_slot_t) >= OS_HEAP_ALIGN` | `za.c:589` |

`g_nil_cell` は `aligned` を**変数**に付けているので、型を取る `_Alignof` では見られない。
GCC拡張の `__alignof__`（式を取れる）を使っている。

`block_device_t` については、指示書が挙げていた2つの検査は**該当しない**:

- **配列で持っていない**。`g_block_devices` は `block_device_t *` の配列（ポインタの配列）で、
  実体はすべて `os_boot_alloc` で個別に確保される。要素サイズの制約は生じない
- **型に `aligned` を付けていない**。確保のたびに `os_boot_alloc(..., OS_HEAP_ALIGN)` が
  開始位置を切り上げるので、型のアラインに依存しない。代わりに確保直後の
  実アドレスを検査している（8.4）

### 8.4 Step 3: ブート時の検査

`os_assert_lisp_aligned(name, addr)`（`runtime.c`、宣言は `runtime.h:112`）。
外れていれば対象名とアドレスを出して `os_panic` する。**既定ビルドでも常に有効**。

| 対象 | 呼び出し箇所 | 位置の根拠 |
|---|---|---|
| `g_nil_cell` | `runtime.c:2623`（`os_bootstrap` 内、nil を組み立てる直前） | nil が**初めて使われる前**。これより早い位置は無い |
| JITリテラルスロット4本の配列先頭 | `za.c:1116-1119`（`za_assert_slot_alignment`）、`kernel.c:109` から呼ぶ | 最初のJITコンパイルより前。要素サイズは 8.3 の `_Static_assert` が保証するので、先頭だけ見れば全要素が揃う |
| `block_device_t` | `block_device.c:92`（`os_boot_alloc` の直後） | `os_boot_alloc` は panic 経路を持たないので、確保した時点で見ないと「Lisp値になってから監査で気づく」ことになる |

**panic hook がまだ入っていない点について**: これらの検査は
`os_set_panic_hook(power_off)`（`kernel.c:171`）より前に走るので、発火すると
`os_panic` は電源断ではなく `for(;;) hlt` で止まる。QEMU からは**タイムアウト**
（終了コード124）として見える。ただし診断は**シリアルへ先に**出しているので、
何が外れたかは確実に読める（`os_panic_stack_overflow` と同じ方針）。
既存の `os_heap_init` のヒープ先頭検査もまったく同じ位置・同じ性質であり、
本採用で新たに持ち込んだ制約ではない。

### 8.5 Step 4: スロット配列のパディングとルート走査

| 確認項目 | 結果 | 根拠 |
|---|---|---|
| GC はスロット配列をどう走査するか | **要素の型単位ですらなく、登録された個々のポインタを辿るだけ**。バイト範囲の走査ではない | `runtime.c:2546-2548` `for (i...) *g_gc_extra_roots[i] = gc_copy_value(*g_gc_extra_roots[i]);`。`g_gc_extra_roots` は `lisp_val_t *` の配列（`runtime.c:1835`）で、`os_gc_register_root(&g_za_*_slots[i].v)` が登録した**そのアドレス**しか読まない |
| 直す必要があるか | **無し**。パディングはルートとして一度も読まれない | 登録されるのは `&arr[i].v`（union のオフセット0）だけ |
| パディングへの書き込みはあるか | **無し** | C 側の代入は9箇所すべて `arr[i].v = ...`。`_pad` はメンバ宣言（`za.c:579`）の1箇所にしか現れず、読み書きするコードは無い。JIT 側も `disp8=0` の64bitアクセスのみ |
| 塗り潰し監査が BSS / スロット配列を塗るか | **塗らない** | トラップパターンの書き込みは `runtime.c:2605` の1箇所だけで、範囲は `g_to_start` 〜 `gc_debug_old_used_end`、つまり**Lispヒープ内の旧From空間**に限られる。BSS は対象外 |

したがって Step 4 は**確認のみで、変更は不要**だった。

### 8.6 Step 5: 監査をタグだけの判定に切り替え

| 変更前 | 変更後 |
|---|---|
| `align_tag_is_enforced(tag) && align_addr_is_in_lisp_heap(addr) && OFFENDS(addr)` | `align_tag_is_enforced(tag) && OFFENDS(addr)` |
| 対象タグ: CONS / SYMBOL / STRING / INSTANCE / FORWARD | **`tag != TAG_FIXNUM && tag != TAG_CHAR`**（= RAW_POINTER も含む全ポインタ型タグ） |
| 確保サイト: 6/10 サイトのみ強制 | **全10サイト**（例外なし） |

- `align_addr_is_in_lisp_heap` は**失敗判定から外した**。削除はせず、
  ヒープ外ポインタをアドレス別に採取して `nm` へ逆引きする **分類用**として残し、
  その用途をコメントに明記した（`runtime.c:558-566`）
- 確保サイトの「サイズ」検査は、**バンプアロケータ3サイトのみ**に限定する
  `align_site_stride_is_enforced` を新設した。`os_boot_alloc` は確保ごとに開始位置を
  切り上げるし、リテラルスロット/`block_device` ハンドルの登録は「既にあるアドレスを
  記録するだけ」で、そこで渡す size は次のアドレスを決めない。全サイトに
  サイズ検査をかけると `sizeof(block_device_t)=104` のような**正当な値で誤検出**する
- **例外リストは空**である

### 8.7 Step 6: ネガティブテスト（コミットしていない）

3段階の検査が実際に崩れを捕まえることを確認した。いずれも一時的な変更で行い、
確認後にソースを復元してある（`grep -c "NEGATIVE TEST" src/c/*.c` = 0）。

| # | 崩し方 | どの検査が | どう失敗したか |
|---|---|---|---|
| NT1 | `g_nil_cell` を `aligned(8)` に戻す | **コンパイル時** | `runtime.c:278: error: static assertion failed: "alignof(g_nil_cell) < OS_HEAP_ALIGN: nil would not be aligned"` |
| NT2 | `za_slot_t` を 8byte（`union { lisp_val_t v; }`）に戻す | **コンパイル時** | `za.c:584` と `za.c:588` の2本が同時に失敗（サイズとアラインの両方） |
| NT3 | nil のアドレスを `g_nil_cell + 8` にする | **ブート時** | シリアルに `PANIC: not 16-byte aligned: g_nil_cell addr=63242296 low bits=8`。`test-results.txt` は**書かれず**、試験に到達する前に停止した（qemu-exit=124 = 8.4 のとおりタイムアウト） |
| NT4 | NT3 に加えてブート時検査だけ無効化 | **監査** | `[ALIGN] VIOLATION alloc g_nil_cell value=63240984 low4bit=8` / `violations=1`。**監査だけでも単独で検出できる** |

NT4 が `alloc` サイト（`ALIGN_SITE_STATIC_NIL`）で捕まえている点は、8.6 で
確保サイトの例外を無くしたことがそのまま効いている。

#### 副産物: `_Static_assert` のメッセージを ASCII にした

NT1/NT2 で、メッセージを日本語にすると GCC が非ASCIIを8進エスケープで出力し、
**読めない**ことが分かった:

```
error: static assertion failed: "g_nil_cell\37777777743\37777777601\37777777656...
```

読めない失敗メッセージは無いのとほぼ同じなので、本採用した `_Static_assert` の
メッセージはすべて ASCII にした（日本語の説明は直前のコメントに残してある）。

### 8.8 Step 7: 検証結果

#### 正しさ（すべて PR #74 の実験時と同じ）

| 検証 | 結果 |
|---|---|
| `make test`（既定ビルド、`ALIGN_AUDIT` 無し） | **8170 OK / 0 NG**、`[ALIGN]` 行 **0行** |
| QEMU全試験 `QEMU_MEM=96M`（GC 72回） | **3269 passed / 0 failed** |
| QEMU全試験 `QEMU_MEM=256M`（GC 14回） | **3269 passed / 0 failed** |
| 強制GC（`gc-stress 1000` + `GC_DEBUG=1`、GC 231回） | **733 passed / 0 failed** |
| 塗り潰し監査22試験 | **21 OK / 1 FAIL**、全22グループが `heap-align=16 violations=0`、`VIOLATION` 行 0件 |

`aot_leaf_gc_test` の1 FAIL は
`(> (- LEAF-GC-STRESS-GC-AFTER LEAF-GC-STRESS-GC-BEFORE) 1)`、`gc=0` の記録つきで、
PR #72 以来の「単独起動するとGCが1回も走らない」既存の性質。**同条件・同内容**である。

#### 新しい判定（タグのみ）での違反件数

| 条件 | region 2（Immobilized Space） | region 4（静的・ブート時確保） | `violations=` |
|---|---|---|---|
| 96M | 195,079 / **0** | 900,658 / **0** | **0** |
| 256M | 39,035 / **0** | 259,340 / **0** | **0** |
| 強制GC（GC 231回） | 210,517 / **0** | 1,168,121 / **0** | **0** |
| 22試験（各グループ） | — | — | **全22グループで 0** |

範囲の絞りを外した状態で、延べ **270万回超**のポインタ観測に対して違反 0 件。

#### `.bss` の増加量

`feature/alignment-audit`（= main + PR #73）を同条件でビルドして比較した。

| | `.bss` サイズ |
|---|---|
| 本採用前 | `0x013613c0` = 20,321,216 byte |
| 本採用後 | `0x013683c0` = 20,349,888 byte |
| **差分** | **+28,672 byte（+0.141%）** |

PR #74 の見積もりは +30,720 byte（スロット 3,840個 × 8byte）だった。実測が
2,048 byte 少ないのは、BSS のシンボル配置が変わって既存の詰め物が一部吸収された
ため（`nm` でスロット配列の間隔を見ると、本採用前後でどちらも配列間に他の
シンボルと余白が入っており、その並びが変わっている）。
**理論値の内訳ではなく実測値を採る。**

