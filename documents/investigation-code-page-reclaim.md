# 調査報告: コードページのフリーリスト接続と回収経路

> 調査日: 2026-09-15 / ブランチ: `feature/compiler-optimization`(`6da82cb`)
> **調査のみ。恒久的なコード変更はしていない。A の測定にのみ一時パッチを当て、
> 測定後に `git checkout` で戻した(パッチは末尾に添付)。**

## 測定の再現手順(実行前に記録)

QEMU の出力は `-serial` で必ず捕捉する。

```bash
# 1. 一時パッチを当てる(末尾の「A 測定用の一時パッチ」)
# 2. qemu_boot_test.lisp と同内容 + レポート行のブートファイルを作る
python3 - <<'PY'
s=open("test/lisp/qemu_boot_test.lisp",encoding="utf-8").read()
rep='(format *isiki-test-stream* "#STAT ~S~%" (%%diag-defun-stats))(finish-output *isiki-test-stream*)\n'
open("test/lisp/tmp_boot_stat.lisp","w",encoding="utf-8").write(s.replace("(isiki-test-report)", rep+"(isiki-test-report)"))
PY
make test-qemu-milestone MILESTONE=test/lisp/tmp_boot_stat.lisp QEMU_EXTRA_FLAGS='-serial stdio'
# 3. git checkout -- src/c/runtime.c で戻し、tmp ファイルを消す
```

D(実行中コードの回収)を踏ませる測定は、**先にこの節へ手順を追記してから**行う。

### D の再現手順(実行前に記録)

回収機構はまだ存在しないので、**回収による破壊は再現できない**。
ここで確かめるのは「その状況が到達可能か」だけ。

```lisp
;; 1. 自分自身を再定義する関数
(defun dsel () (progn (defun dsel () 1) 2))
(dsel)          ; 実行中に自分を再定義する
(dsel)          ; 2回目は新しい定義

;; 2. 再定義を含むループ
(defun dloop (n) (let ((i 0)) (while (< i n) (progn (defun dtgt () i) (setq i (+ i 1))))))

;; 3. 深い呼び出しの途中で下位フレームの関数を再定義
(defun dinner () (progn (defun douter () 9) 3))
(defun douter () (dinner))
```

`make test-qemu-milestone MILESTONE=<boot> QEMU_EXTRA_FLAGS='-serial stdio'` で実行し、
シリアルを捨てないこと。落ちたら深追いせず手順とログを残す。


---

## A の結果(再定義 vs 新規定義)

**回収経路を作っても CI の消費は 3.1% しか減らない。**

一時パッチで `os_set_function` の新規/再定義と、`os_imm_pages_alloc_contiguous` の
実績を数えた(パッチは末尾に添付、測定後 `git checkout` で戻した)。

```
起動直後 (new redef contig_calls contig_pages contig_1page redef_jit redef_jit_pages)
       = (733    0    85           122          64           0         0)
完走後   = (2813  512  1062         1158         992          33        37)
```

| | 起動直後 | フルテスト完走後 | テスト実行中のみ |
|---|---|---|---|
| 新規の関数登録 | 733 | 2,813 | 2,080 |
| **再定義** | **0** | **512** | **512** |
| コードページ確保の回数 | 85 | 1,062 | 977 |
| コードページ総数 | 122 | 1,158 | 1,036 |
| **再定義で捨てられた JIT 関数** | **0** | **33** | **33** |
| **そのコードページ数(= 回収可能量)** | **0** | **37** | **37** |

### 決定的な数字

- **再定義 512 回のうち、JIT コードを捨てたのは 33 回(6.4%)だけ。**
  残り 479 回はインタプリタ関数や native の再登録で、コードページを持たない
- **回収可能なページは 37 / 1,158 = 3.2%。**
  バイトでは 151,552 / 4,856,352 = **3.1%**

つまり **29% の内訳は「異なる名前の定義の蓄積」が支配的**で、
**回収経路を作っても CI の余裕は生まれない。**

### 推測・未確認

- **(未確認)** `redef_jit` は「`os_set_function` が既存名を上書きした時点で
  旧関数が JIT 済みだった」回数である。旧関数が他から参照されていない保証は
  取っていない(C-2 参照)ので、**37 ページは回収可能量の上限**であり、
  実際に安全に回収できる量はこれ以下になる
- **(推測)** テスト中の再定義 512 回の大半は、テストファイルが同名のヘルパー
  (`say` / `force-gc` 等)を定義し直しているものと考えられる。
  個々の名前までは集計していない

---

## サマリ

**判断ゲートの「別名の蓄積が支配的 → パッキング一択」。**
回収可能量は 3.1% で、回収経路を作っても CI の枯渇は防げない。
フリーリスト接続も、返るものが 3.2% しかないので単独では意味がない。
さらに **F の結果として、パッキングを入れるとページ単位の回収は原理的に成立しなくなる**
(トップレベル `defun` の owner はすべて `global_environment` なので全員が同居する)。
**回収は作らず、パッキングに集中するのがよいと考える。**

---

## 調査項目 B: フリーリストと連続確保

### 確認したこと

**B-1. 単一ページの鎖。範囲は持たない**

`runtime.c:1101-1102`:

> `/** os_imm_page_freeで返却されたページのフリーリスト(各ページの先頭8byteをnextポインタとして使う) */`
> `static void *g_imm_free_list = 0;`

`os_imm_page_free`(`runtime.c:1417-1419`)は 1 ページを鎖の先頭へ繋ぐだけ。
`os_imm_page_alloc`(`runtime.c:1172-1177`)は先頭を 1 枚外すだけ。
**連続した複数ページを切り出す手段は無い。** アドレス順にも並んでいないので、
隣接ページの合体(coalescing)もできない。

**B-2. 1ページ要求が 93.4%(実測)**

| | 確保回数 | 総ページ数 | 1ページ要求 | 平均 |
|---|---|---|---|---|
| 起動時 | 85 | 122 | 64(**75.3%**) | 1.44 ページ |
| テスト実行中 | 977 | 1,036 | 928(**95.0%**) | 1.06 ページ |
| 合計 | 1,062 | 1,158 | 992(**93.4%**) | 1.09 ページ |

起動時(`init.lisp` の関数群)は大きめで 4 分の 1 が複数ページ。
テスト中の小さい関数はほぼ 1 ページ。

**B-3/B-4. 「1ページだけフリーリスト、複数ページは bump」で 93.4% を賄える**

ただし **A の結果と掛け合わせると意味が薄い。** 返るページが 3.2% しかないので、
フリーリストはほとんど常に空になる。断片化が問題になる以前に、在庫が無い。

**ページ用途の入れ替えに問題は無い(確認)**

- **実行権限の管理が存在しない。** Immobilized Space は BSS 上の静的配列
  (`runtime.c:1098`)で、カーネル全体でページテーブルを触るのは
  スタックガードの `unmap_range`(`process.c:120, 142`)だけ。
  NX ビットや W^X の設定箇所は無い
- **キャッシュのフラッシュは既に済んでいる。** `jit_serialize_icache()`
  (`za.c:462-466`、`cpuid` 1発)をコードのコピー直後に呼んでいる(`za.c:6168`)。
  slot だったページがコードになる場合もこの経路を通る
- アラインは 4096 境界で揃っており、コードの要求(16 byte 境界)を満たす

**なお、現状でも解放されたコードページは無駄ではない。** `os_imm_page_alloc`
(cell / meta)はフリーリストを見るので、`destroy-environment` で返ったコードページは
**slot として再利用される。** 「返しても使われない」のはコードとしての再利用だけである。

### 推測・未確認

- **(未確認)** 断片化の実測はしていない。A の結果から在庫がほぼ無いため測る意味が薄いと判断した

---

## 調査項目 C: コードページの到達不能性をどう判定するか

### 確認したこと

**C-1. 関数オブジェクトからコードページを辿れる**

`fn`(TAG_INSTANCE)→ `word1` = `za_fn_meta_t *` → `code_base` / `code_len`。
`za_fn_meta_t` は 5 フィールドで、offset 24 = `code_base`、offset 32 = `code_len`
(`src/c/runtime.h` の定義、`os_fn_set_code_range` が `runtime.c:3261-3275` で設定する)。
**A の測定でこの経路を実際に使って code_len を読み、ページ数を数えた。動く。**

**C-2. cell の書き換えだけでは到達不能とは言えない(確認)**

`os_set_function` が cells の中身を書き換えても、**旧関数オブジェクトへの他の参照は
そのまま生きている。** `(function foo)` の戻り値を変数に入れていれば、
その関数は呼べる状態のまま残る。したがって
**「再定義した = 旧コードは死んだ」とは断定できない。**

**C-3. GC への相乗りは構造上は可能(確認)+ 設計は推測**

- `gc_scan_instance`(`runtime.c` の `MAGIC_FUNCTION_NATIVE` ケース)は
  **生きている関数オブジェクトを必ず訪れる。** word1(meta への生ポインタ)は
  「素通し」とコメントされているが、**訪れてはいる**ので、
  ここで meta のアドレスを記録すればマーク相当になる**(推測)**
- 全コードページの一覧は `global_environment` の `pages` スロットにある
  (`os_environment_register_pages`、`runtime.c:3685-3702`)。
  スイープ対象の列挙はできる**(確認)**
- したがって「GC のマーク中に生きている meta を集め、`pages` のうち
  どの meta の `[code_base, code_base+code_len)` にも入らないページを返す」
  という二相スイープは**構造上は組める(推測)**

**C-4. meta 自体の生存判定は逆向きで、上記のマークでしか取れない(確認)**

meta は Immobilized Space 上で GC 管理外。関数オブジェクト → meta の一方向しかない
(`gc_scan_instance` のコメント「word1は…Lispヒープ外。素通し」)。
**meta から関数オブジェクトへ戻る参照は無い。**

### 報告: どれか

**「GC まで待つ必要がある」。** 同名再定義の時点では C-2 のとおり到達不能性を
判定できない。GC のマークに相乗りする形なら判定できる**(推測)**が、
**それでも D の「実行中でないこと」は別途保証が要る。**

---

## 調査項目 D: 実行中のコードを回収しない保証

**危険が確定したのでここで止めた。**

### 確認したこと

**D-1. 3つの状況はすべて到達可能(実測)**

回収機構が無いので破壊は起きないが、状況そのものが成立することを確かめた:

```
#D 1回目 dsel=2 compiled=T          ; 自分を実行中に自分を再定義
#D 2回目 dsel=1 compiled=T
#D ループ内再定義 dtgt=5 compiled=T   ; ループ本体が動いている最中に再定義
#D 呼び出し中に下位を再定義 douter=3  ; douter のフレームが積まれたまま douter を再定義
#D その後 douter=9
```

**D-2. GC はコールスタックから実行中のコードページを割り出せない(確認)**

GC のスタック走査は **precise(シャドースタック)**であって conservative ではない:

```c
for (UINT32 i = 0; i < PROCESS_COUNT; i++) {
    for (gc_rootnode *node = get_process(i)->gc_roots; node != 0; node = node->next) {
        *node->var_ptr = gc_copy_value(*node->var_ptr);
    }
}
```
(`runtime.c:2193-2198`)

見ているのは `GC_PROTECT` で登録された `lisp_val_t` のアドレスだけで、
**C スタック上の生の戻り番地は一切見ない。**
JIT 生成コードの戻り番地はコードページを指すが、GC はそれを知らない。

### 判定

**危険。** 到達不能性が判定できても、**そのページが今まさに実行中でないことは
現在の機構では分からない。** D-1 の3ケースはいずれも
「旧コードの戻り番地がスタックに載ったまま、その関数が再定義される」形で、
即時回収でもGC時回収でも、そのページを再利用すれば**戻り先が別の関数の機械語になる。**

保証を作るには、コールスタックを保守的に走査して戻り番地がコードページ範囲に
入るものを pin する仕組みが要る**(推測)**。これは GC の設計を precise から
一部 conservative へ変える話になる。

**依頼書の指示どおり、ここで止めた。**

---

## 調査項目 E: 焼き込まれた参照との関係

### 確認したこと

**E-1. JIT の呼び出し先キャッシュはコードページを指さない**

キャッシュスロットが持つのは Function Cell のアドレス(`za.c:1064-1070`)。
生成コードに焼き込まれるのは**スロット自身のアドレス**
(逆アセンブルで確認: `movabs r14, 0xdc77ac0` → `mov rax, [r14]`)で、
どちらも Immobilized Space のコードページではない。**影響を受けない。**

**E-2. AOT から JIT コードを直接呼ぶ経路は無い**

`src/c/lisp_compiled.c` に `cons_entry` / `fixed_entry` の参照は無い。
AOT の `name__fixed` はリンク時に確定する C 関数(`transpile.lisp:588, 595`)で、
カーネルイメージの `.text` にある。**Immobilized Space を指さない。**

**E-3. fnptr で一意化する表は見つからなかった**

`os_fn_meta_alloc`(`runtime.c:1449`)は呼ばれるたびに新しい meta を切り出すだけで、
fnptr をキーにした検索・再利用はしていない。**同じ fnptr に複数の meta ができうる**が、
回収済みアドレスを保持し続ける表は存在しない。

**E-4. 逆アセンブラは回収済みページを指しうる(推測)**

`meta->code_base` / `code_len` は回収時に無効化されない。
ただし **meta に到達するには生きた関数オブジェクトが要る**ので、
「回収されたのに参照できる」状況は、C-2 の「旧関数オブジェクトがまだ生きている」
ケースと同じ問題に帰着する。**逆アセンブラ固有の追加リスクは無いと考える(推測)。**

---

## 調査項目 F: パッキングとの設計上の関係

### 確認したこと

**F-1/F-2. 「同じ環境の関数だけを同じページに」は事実上の制約にならない**

トップレベル `defun` の owner は `os_definition_env(env)` = `global_environment`
(`src/c/eval.c:399-402`)。**したがって全員が同じページ群に同居する。**
依頼書の指摘どおり、この制約ではページ単位の回収は成立しない。

**F-3. パッキングを入れると B の連続確保問題は自然に縮む**

1 コールサイト 762 byte、ネスト 1 段 691 byte という測定値から、
小さい関数は 1 ページの数分の一に収まる。実測でも 1 ページ要求が 93.4%。
**ただし起動時は平均 1.44 ページなので、複数ページを要する関数は残る。**

**F-4. 判定: 実質的に排他**

- ページ単位の回収を残したままパッキングを入れると、
  **同居する全員が死ぬまで 1 ページも返せない。** global_environment では
  実質「永久に返らない」になる
- 回収の粒度をページより細かくするには、可変長ブロックのフリーリストが要る。
  現在の「先頭 8 byte を next に使う単一ページの鎖」(`runtime.c:1101`)は
  そのままでは使えない

**A の結果(回収可能 3.1%)と合わせると、回収を捨ててパッキングを取るのが合理的。**

---

## 所見

**(所見)判断ゲートは「別名の蓄積が支配的 → パッキング一択」。**
回収可能量 3.1% は測定誤差に近い。しかも C-2 により実際に安全に回収できる量は
それ以下で、D により実行中でないことの保証まで要る。
**費用と危険に対して得るものが小さすぎる。**

**(所見)前回「フリーリスト接続を先に直すべき」と書いたが、撤回する。**
前回は「返しても使われない / 使えるようにしても返らない」という構造の問題として
見ていたが、今回の実測で**そもそも返るものが 3.2% しかない**ことが分かった。
接続しても在庫が無い。**構造としては歪んでいるが、直しても数字は動かない。**

**(所見)D は、回収を将来検討する場合の本命の障害になる。**
GC が precise なので「実行中のコードページ」を知る手段が無い。
これは回収機構だけの問題ではなく、**コードページを動かす/捨てるあらゆる最適化
(将来のコード圧縮、再配置)に共通の前提**になる。記録しておく価値があると思う。

**(所見)パッキングの効果見積もりの精度を上げるには、関数ごとのコード長分布が要る。**
平均 1.09 ページ(テスト中)は「ほとんどが 1 ページ以内」を意味するが、
その 1 ページの中を何 byte 使っているかは測っていない。
**そこが分からないとパッキングで何倍になるか言えない。** 次に測るならここ。

---

## 未調査・積み残し

- **関数ごとのコード長の分布は未測定。** パッキングの効果を数字で出すには必須。
  `%%DIAG-ZA-CODE-LEN` は直近 1 件しか返さないので、
  定義のたびに読む一時パッチか、`pages` スロットからの集計が要る
- **再定義 512 回の名前別内訳は取っていない。** どのテストが何を定義し直しているか
- **回収可能 37 ページのうち、C-2 の意味で本当に到達不能なものが何ページか**は未確認。
  37 は上限
- **D の保守的スタック走査の実現可能性**は検討していない(危険が分かった時点で止めた)
- **複数ページを要する関数(起動時の 25%)がパッキング後にどうなるか**は未検討

---

## A 測定用の一時パッチ

測定後 `git checkout -- src/c/runtime.c` で戻済み。作業ツリーはクリーン。

```diff
diff --git a/src/c/runtime.c b/src/c/runtime.c
index 3e5f74f..f493ef0 100644
--- a/src/c/runtime.c
+++ b/src/c/runtime.c
@@ -1165,6 +1165,27 @@ lisp_val_t primitive_fn_cell_count(lisp_val_t args, lisp_val_t env) {
     return os_make_fixnum(g_function_cell_count);
 }
 
+/* [一時測定パッチ] defun の再定義/新規の内訳と、コードページ確保の実績を数える。
+   調査 documents/investigation-code-page-reclaim.md の項目A専用。恒久化しないこと。 */
+static UINT64 g_diag_fn_new = 0;      /* os_set_function: その env に無い名前 */
+static UINT64 g_diag_fn_redef = 0;    /* os_set_function: 既存名の上書き */
+static UINT64 g_diag_contig_calls = 0;/* os_imm_pages_alloc_contiguous の成功呼び出し数 */
+static UINT64 g_diag_contig_pages = 0;/* 同、確保した総ページ数 */
+static UINT64 g_diag_contig_p1 = 0;   /* 同、1ページ要求だったもの */
+static UINT64 g_diag_redef_jit = 0;   /* 再定義で捨てられた旧関数がJIT済みだった回数 */
+static UINT64 g_diag_redef_jit_pages = 0; /* 同、そのコードページ数の合計(=回収可能量) */
+
+lisp_val_t primitive_diag_defun_stats(lisp_val_t args, lisp_val_t env) {
+    (void)args; (void)env;
+    lisp_val_t l7 = os_make_cons(os_make_fixnum(g_diag_redef_jit_pages), nil);
+    lisp_val_t l6 = os_make_cons(os_make_fixnum(g_diag_redef_jit), l7);
+    lisp_val_t l5 = os_make_cons(os_make_fixnum(g_diag_contig_p1), l6);
+    lisp_val_t l4 = os_make_cons(os_make_fixnum(g_diag_contig_pages), l5);
+    lisp_val_t l3 = os_make_cons(os_make_fixnum(g_diag_contig_calls), l4);
+    lisp_val_t l2 = os_make_cons(os_make_fixnum(g_diag_fn_redef), l3);
+    return os_make_cons(os_make_fixnum(g_diag_fn_new), l2);
+}
+
 static imm_slot_cursor_t g_fn_meta_cursor;
 /** 直近にImmobilized Spaceへ要求された確保サイズ(枯渇時の診断表示用) */
 static UINT64 g_imm_last_request_bytes = 0;
@@ -1407,6 +1428,9 @@ void *os_imm_pages_alloc_contiguous(UINT64 count) {
     }
     void *pages = g_imm_bump;
     g_imm_bump += needed;
+    g_diag_contig_calls++;              /* [一時測定パッチ] */
+    g_diag_contig_pages += count;       /* [一時測定パッチ] */
+    if (count == 1) { g_diag_contig_p1++; } /* [一時測定パッチ] */
     return pages;
 }
 
@@ -2434,6 +2458,7 @@ void os_bootstrap() {
         os_set_function(os_make_symbol("%%HEAP-USED-BYTES"), os_make_native_function((lisp_addr_t)(void *)primitive_heap_used_bytes), global_environment);
         os_set_function(os_make_symbol("%%GC-COLLECT-COUNT"), os_make_native_function((lisp_addr_t)(void *)primitive_gc_collect_count), global_environment);
         os_set_function(os_make_symbol("%%DIAG-FN-CELL-COUNT"), os_make_native_function((lisp_addr_t)(void *)primitive_fn_cell_count), global_environment);
+        os_set_function(os_make_symbol("%%DIAG-DEFUN-STATS"), os_make_native_function((lisp_addr_t)(void *)primitive_diag_defun_stats), global_environment);  /* [一時測定パッチ] */
         os_set_function(os_make_symbol("%%IMM-SPACE-TOTAL-BYTES"), os_make_native_function((lisp_addr_t)(void *)primitive_imm_space_total_bytes), global_environment);
         os_set_function(os_make_symbol("%%IMM-SPACE-USED-BYTES"), os_make_native_function((lisp_addr_t)(void *)primitive_imm_space_used_bytes), global_environment);
         os_set_function(os_make_symbol("%%BOOT-ALLOC-USED-BYTES"), os_make_native_function((lisp_addr_t)(void *)primitive_boot_alloc_used_bytes), global_environment);
@@ -3575,9 +3600,27 @@ lisp_val_t os_set_function(lisp_val_t sym, lisp_val_t fn_obj, lisp_val_t env) {
     lisp_val_t existing_pair = cc_assoc_eq(sym, alist);
 
     if (existing_pair != nil) {
+        g_diag_fn_redef++;   /* [一時測定パッチ] */
+        /* [一時測定パッチ] 捨てられる旧関数がJIT済みなら、そのコードページは
+           この時点で(cells経由の参照を除けば)到達不能になる。何ページ分かを数える。
+           za_fn_meta_t: offset24=code_base, offset32=code_len */
+        {
+            lisp_val_t old_fn = cc_cdr(existing_pair);
+            if ((old_fn & TAG_MASK) == TAG_INSTANCE) {
+                UINT64 *o = (UINT64 *)(old_fn & ~TAG_MASK);
+                if (o[0] == MAGIC_FUNCTION_NATIVE && o[2] != nil && o[1] != 0) {
+                    UINT64 code_len = ((UINT64 *)o[1])[4];
+                    if (code_len != 0) {
+                        g_diag_redef_jit++;
+                        g_diag_redef_jit_pages += (code_len + IMM_PAGE_SIZE - 1) / IMM_PAGE_SIZE;
+                    }
+                }
+            }
+        }
         // すでに存在する場合は cdr を破壊的に書き換える
         ((lisp_val_t *)(existing_pair & ~TAG_MASK))[1] = fn_obj;
     } else {
+        g_diag_fn_new++;     /* [一時測定パッチ] */
         // 新規追加
         lisp_val_t new_pair = os_make_cons(sym, fn_obj);
         // (push new-pair alist)
```
