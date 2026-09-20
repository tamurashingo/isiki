# 次期マイルストン: 呼び出し規約(ABI)をcons-list方式から固定引数レジスタ渡し方式へ

## 背景

`jit-arithmetic-cons-elimination-instructions.md`で対応した
`primitive_add2`等9個の問題は、**`isiki-os`全体の呼び出し規約が抱える
より大きな構造的コストの、一部だけを局所的に潰したもの**に過ぎない。

現状、`isiki-os`の呼び出し可能オブジェクトはすべて
`fn(evaluated_args, env)`という統一ABI(CLAUDE.mdにも明記、AOT/JIT/
インタプリタ/ネイティブCプリミティブを`apply_function`が区別せず呼べる
ようにするための設計)を使っており、`args`は常にconsリストである。

- JITは呼び出し箇所ごとに引数の個数を静的に知っている場合が多いにも
  関わらず、呼び出し規約自体が「可変長引数はconsリストで渡す」ことを
  前提にしているため、**2引数固定の演算という最も頻出するパターンでさえ、
  本来ならレジスタ渡しで完結できるところをconsセル経由に変換していた**。
  今回の修正はこの9関数に限定した対症療法であり、他の頻出する固定引数
  関数呼び出し(ユーザー定義関数の呼び出し全般を含む)には、同様の問題が
  依然として残っている可能性が高い。
- インタプリタ経由の呼び出し(`eval.c`の`eval_args`)も同様にconsリストを
  都度組み立てており、これは可変長引数の統一的な扱いという設計上の
  制約からほぼ不可避なコストになっている。

SBCL(Steel Bank Common Lisp)のような成熟した処理系は、これに対して
**固定引数はレジスタで直接渡し、可変長引数(`&rest`)が必要な場合のみ
consリストを組み立てる**という呼び出し規約を採用している。具体的には
おおむね以下のような設計になっている(処理系のバージョン・アーキテク
チャによって詳細は異なるため、実装時に個別に調査・確認する)。

- 引数の個数(arity)によって呼び出し規約が分岐する。少数の固定引数
  (1〜3個程度)はレジスタ渡し、それを超える場合や`&rest`がある場合の
  みスタック/ヒープを介する。
  - x86-64版SBCLでは、fixed-argの関数呼び出しにおいて、引数はレジスタ
    (`rdx`, `rdi`, `rsi`等、SBCLの内部ABIに従う)で渡され、呼び出し先の
    関数エントリポイントは実際の引数個数(`rcx`等に格納されるnargs)に
    応じて複数のエントリ(0引数用、1引数用、2引数用...)を持ち、
    ジャンプテーブル的にディスパッチする。
  - `&rest`引数がある場合のみ、その部分だけがヒープ上のリストとして
    構築される。固定部分の引数はレジスタ渡しのまま。
  - 型が静的に分かる場合(数値型の宣言、`the fixnum`等)は、ボックス化
    (タグ付きポインタ化)を回避した直接計算(unboxed演算)を行う最適化
    パスも持つが、これは今回のマイルストンのスコープを超える可能性が
    高いため、まずは「consリスト経由をやめて固定引数をレジスタ渡しに
    する」部分に絞ることを推奨する。

この変更はJIT(`za.c`)・AOTトランスパイラ(`transpile.lisp`)双方の
コード生成ロジック、および呼び出し規約に依存する全てのネイティブC
プリミティブに影響する、**プロジェクト全体を横断する大規模な変更**である。
今回のセッションで扱ってきたIDE/9P性能問題とは独立した、別マイルストン
として計画・着手する。

---

## 最終的にやりたいこと(マイルストンのゴール)

1. **固定引数の関数呼び出し(ユーザー定義関数・ネイティブCプリミティブ
   問わず)について、consリストを経由せず、レジスタ(または呼び出し規約上
   の固定スロット)で直接引数を渡す呼び出し規約を設計・導入する。**
2. **`&rest`/可変長引数が必要なケースのみ、従来通りconsリストを構築する
   経路を残す。**
3. **JIT(`za.c`)・AOTトランスパイラ(`transpile.lisp`)・インタプリタ
   (`eval.c`)・既存のネイティブCプリミティブ(`os_register_subprimitives`
   等で登録されているもの)のすべてが、新しい呼び出し規約と後方互換性
   (あるいは明確な移行パス)を持つようにする。**
4. **この変更により、`primitive_add2`等で対応した問題が、算術演算に
   限らずユーザー定義関数の呼び出し全般で解消されることを確認する。**

### 確定している設計方針: 引数の順序を `env` 優先に入れ替える

現行ABIは `fn(evaluated_args, env)` の順(第1引数がconsリスト、第2引数が
`env`)だが、新ABIでは**`env` を第1引数に固定し、以降のスロットに実引数を
並べる**方針とする。

```c
// 現行
lisp_val_t fn(lisp_val_t evaluated_args, lisp_val_t env);

// 新ABI(固定引数版、例: 2引数関数)
lisp_val_t fn(lisp_val_t env, lisp_val_t arg0, lisp_val_t arg1);
```

理由: `env` はどの呼び出しでも省略できない「必ず渡す値」であるのに対し、
`evaluated_args` は本来「0個から可変長」の実引数であり、性質が異なる。
`env` を固定の第1引数に据えることで、System V AMD64 ABI の引数レジスタ
(`rdi`, `rsi`, `rdx`, `rcx`, `r8`, `r9`、その先はスタック)にそのまま
自然にマッピングできる。

```
rdi = env
rsi = arg0
rdx = arg1
rcx = arg2
r8  = arg3
r9  = arg4
(それ以上はスタック渡し)
```

`&rest` 部分を持つ関数は、固定引数分をレジスタ渡しにしたまま、`&rest`
にまとめるべき残りの実引数だけを従来通りconsリストとして構築し、
レジスタ渡しの最後のスロット(または専用のスロット)に渡す設計とする
(詳細は調査事項2で検討する)。

この方針を踏まえ、調査事項2(SBCLの呼び出し規約調査)では、SBCLが
`env`(あるいはSBCLにおけるクロージャ環境/FDEFN相当の固定引数)を
どのレジスタ位置に固定しているか、実引数個数(nargs)をどのレジスタで
呼び出し先に伝えているかを重点的に確認し、`isiki-os`の
`env`優先レイアウトとの異同を整理する。

このマイルストンは大規模なため、**一度に全て実装するのではなく、
複数の小さなマイルストンに分割して段階的に進める**ことを前提とする
(具体的な分割案は「調査事項」を踏まえて別途起こす)。

---

## 調査事項(設計着手前に必ず実施すること)

### 1. 現状の呼び出し規約の全利用箇所の洗い出し

- `apply_function`(`eval.c`)が、AOT生成コード・JIT生成コード・
  インタプリタ・ネイティブCプリミティブの4種類をどう区別なく呼んで
  いるか、現状の統一ABI(`fn(evaluated_args, env)`)への依存箇所を
  全て洗い出す。
- `os_register_subprimitives`/`os_register_za_primitives`/
  `os_register_aot_init_functions`で登録されている関数群が、それぞれ
  何個の関数を持ち、うち固定引数のものと可変長引数(`&rest`)のものの
  比率を把握する(固定引数が大多数であれば、このマイルストンの効果が
  大きいことの裏付けになる)。

### 2. SBCLの呼び出し規約の詳細調査

- SBCLのソースコード(`src/compiler/x86-64/call.lisp`、
  `src/runtime/x86-64-arch.c`等)、またはSBCL Internals関連のドキュメント
  を参照し、固定引数呼び出し・可変長引数呼び出し・多値(multiple values)
  の扱いを具体的に調査する。
- 特に、「呼び出し先の引数個数(arity)チェックをどこで行っているか」
  (呼び出し元が個数を保証する静的ディスパッチか、呼び出し先が実行時に
  検査するか)を明確にする。`isiki-os`はJIT/AOT/インタプリタが混在する
  ため、静的に個数が分からない呼び出し(例えばインタプリタから
  JITコンパイル済み関数を呼ぶ場合)への対処方針を検討する必要がある。
- `env`を第1引数に固定した場合、実引数の個数(nargs)を呼び出し先へ
  どう伝えるかを設計する。候補として、(a) SBCL同様に専用レジスタ
  (`rax`/`rcx`等の未使用スロット)にnargsを積む、(b) 関数ごとにarity別の
  エントリポイントを複数用意し呼び出し元が個数に応じて呼び分ける、
  (c) 固定引数関数は個数チェック自体を省略し呼び出し元の静的な保証に
  委ねる(不正な個数の呼び出しはコンパイルエラーとして検出する)、の
  3案を比較する。(c)は最も高速だが、インタプリタ経由での動的な誤呼び出し
  (引数個数の実行時ミスマッチ)を検出できなくなるため、エラーハンドリング
  の方針(現状`apply_function`が返しているエラーメッセージ等)との整合性を
  確認する。
- SBCL以外の処理系(CMUCL、Clozure CL等、SBCLの祖先または同系統)の
  呼び出し規約も参考までに調査し、`isiki-os`のSASOS・タグ付きポインタ
  設計との親和性を比較検討する。

### 3. `isiki-os`独自制約との整合性確認

- **Immobilized Space・コピーGCとの相性**: レジスタ渡しの引数は、GCの
  ルートセット(shadow stack)としてどう追跡されるかを設計する必要が
  ある。現状「スタックフレーム上のローカル変数やレジスタ内の値を安全に
  ルーツとして追従できるセル構造」を採用しているとのことなので、この
  既存の仕組みをレジスタ渡し引数にも適用できるか確認する。
- **3層の実行モデル(AOT/JIT/インタプリタ)全てで一貫した規約にできるか**:
  インタプリタ(`eval.c`の木構造評価器)は、コンパイル時に引数個数が
  分からない場合があるため、固定引数レジスタ渡しの恩恵を受けにくい
  可能性がある。インタプリタは現状のconsリスト方式のまま残し、
  JIT/AOTコンパイル済みコード同士の呼び出しだけを新方式にする、という
  部分的な適用も選択肢として検討する。
- **`documents/jit.md`/`documents/transpiler.md`との整合**: 既存の
  JIT対応構文一覧・AOTトランスパイラのマイルストン計画ドキュメントとの
  関係を整理し、今回の呼び出し規約変更がこれらにどう影響するかを
  明記する。

### 4. 影響範囲とリスクの見積もり

- この変更は`za.c`(JITコンパイラ)・`transpile.lisp`(AOTトランスパイラ)
  ・`eval.c`(インタプリタ)・既存の全ネイティブCプリミティブに影響する。
  影響範囲の広さから、**段階的な移行が可能な設計**(新旧呼び出し規約を
  一定期間共存させる、あるいは関数ごとに移行できる仕組み)が現実的か、
  それとも一斉切り替えが必要かを検討する。
- 既存の`test/c/`・`test/lisp/`のテスト資産が、この変更でどれだけ
  影響を受けるかを見積もる。

---

## 実装後のテスト方針(将来の実装マイルストンに向けた指針)

このマイルストン自体はまだ設計段階のため、具体的なテスト項目は実装
マイルストンが具体化した時点で個別に定めるが、最低限以下の観点を
テスト設計に含めることを推奨する。

1. **新旧呼び出し規約での計算結果の完全一致**(固定引数・可変長引数
   いずれも)を確認する回帰テスト。
2. **`jit-arithmetic-cons-elimination-instructions.md`のN-スケーリング
   ベンチマークを再計測し、`primitive_add2`等のピンポイント修正時より
   さらに`futex回数/N`が改善するか(あるいはIDE/9P等の実運用シナリオでの
   改善幅がどれだけ広がるか)を確認する。** 今回のマイルストンの効果を
   定量的に検証する主要な指標として位置づける。
3. **GCとの整合性テスト**: レジスタ渡し引数を含む状態でGCが発火した
   場合に、正しくルートとして追跡され、値化け・クラッシュが起きない
   ことを確認する(過去に経験した`os_gc_register_root`の内部ポインタ
   バグのような、GCとの整合性に関する不具合パターンを踏まえ、特に
   注意深くテストする)。

---

## 調査結果と設計確定(2026-09-09)

### 訂正: レジスタ割り付けはSystem V AMD64 ABIではなくMicrosoft x64 ABI

冒頭「確定している設計方針」節ではSystem V AMD64 ABI(`rdi`, `rsi`, `rdx`, `rcx`, `r8`,
`r9`)を前提にレジスタマッピングを記述しているが、実装調査の結果これは誤りだった
ことが判明した。`Makefile`のメインビルドターゲット(`build`/`all`)は
`x86_64-w64-mingw32-gcc`(MinGW-w64、Windows x64向けクロスコンパイラ)を使用しており、
実際のCコンパイラABIは**Microsoft x64呼び出し規約**(整数/ポインタ引数は
`rcx`, `rdx`, `r8`, `r9`の順、5個目以降はスタック)である。これは`za.c`が既に
`za_compile_fold`(za.c:1736-1806)や各種ランタイム呼び出しヘルパー
(`za_emit_gc_link_slot`等、za.c:1101-1116)で一貫して`ZA_REG_RCX`=第1引数、
`ZA_REG_RDX`=第2引数として機械語を生成していることとも整合する。

したがって、新ABIのレジスタマッピングは以下の通り訂正する:
```
rcx = env (第1引数)
rdx = arg0 (第2引数)
r8  = arg1 (第3引数)
r9  = arg2 (第4引数)
(それ以上はスタック渡し)
```
AOT側(`transpile.lisp`が生成する通常のC関数)はコンパイラがMicrosoft x64 ABIで
自動的にレジスタ割り付けを行うため、この訂正はAOT生成コード自体には影響しない
(Cソースコードの記述は変わらず、Cコンパイラの生成する機械語がSystem VではなくMS ABI
になるだけ)。手動でレジスタ配置を書くza.cのみ、このマッピングを明示的に踏襲する
必要がある(ABI-M1以降、既にこの規約で実装している)。


調査事項1〜4を実施した結果と、それを踏まえて確定した設計判断を記録する。

### 調査事項1の結果: 呼び出し規約の全利用箇所

- `apply_function`(`eval.c:127-156`)は`MAGIC_FUNCTION_NATIVE`と
  `MAGIC_FUNCTION_INTERPRETED`の2種類しか区別しない。AOTコンパイル済み関数も
  JITコンパイル済み関数も両方`MAGIC_FUNCTION_NATIVE`として登録され、同一の
  Cポインタシグネチャ`lisp_val_t (*)(lisp_val_t evaluated_args, lisp_val_t env)`
  で区別なく呼ばれる。
- 登録関数の内訳: `os_register_subprimitives`(subprimitive.c)=12個(全て固定
  引数)、`os_register_za_primitives`=1個、`os_register_aot_init_functions`
  (lisp_compiled.c)=357個(うち約349個が固定引数、約14個が可変長/`&rest`)。
  `os_bootstrap`(runtime.c)のコア約115個のプリミティブも大半が固定引数。
  → **全体の9割以上が固定引数であり、このマイルストンの効果は大きい**ことが
  裏付けられた。
- `lisp_compiled.c`には819箇所の`os_make_cons(__call_arg_...`があり、AOT生成
  コードの全呼び出し箇所が、既知の固定引数呼び出しに対してもconsリストを構築
  している。これが最大の影響範囲。
- 動的呼び出し経路(`primitive_funcall`/`primitive_apply`(eval.c:812-829)、
  `primitive_funcall_by_name`(runtime.c:5691)、`os_apply_via_cell`
  (runtime.c:2170)、`bind_params`(eval.c:96-118)、`mount.c`/`stream.c`の
  コールバック呼び出し多数)は全て`evaluated_args`consリストを前提にしている。

### 調査事項2の結果: nargs伝達方式の決定

SBCL等は実行時arityディスパッチ(専用レジスタでのnargs受け渡し、またはarity別
エントリポイント)を持つが、`isiki-os`では**(c) 固定引数関数は個数チェックを
省略し、呼び出し元の静的保証に委ねる**方式を採用する。根拠は調査事項1の通り
静的に個数がわかる呼び出しが大多数であるため。動的呼び出し経路(funcall/apply/
インタプリタ/コールバック)には固定引数レジスタ渡しエントリポイントを使わせず、
既存のconsリストABIへ常に委ねることで、(c)案の弱点(動的な誤呼び出しの検出
不能)を実質的に無効化する。

### 調査事項3の結果: `isiki-os`独自制約との整合性

- **GCとの整合性**: Cheneyコピーコレクタ(`os_gc_collect`, runtime.c:942〜)の
  ルートは安定したメモリ位置(`GC_PROTECT`ローカル、または`za_emit_gc_link_slot`
  でリンクされた`[rsp+off]`フレームスロット)でなければならない。**レジスタの
  みに保持された値はGCスキャンから不可視**であり、GCトリガーとなり得るアロケ
  ーションを跨いで生存する場合は必ずリンク済みフレームスロットへのスピルが
  必要。`za_compile_fold`が2番目のオペランド評価前に1番目をスピルしているのと
  同じ手法を新ABIでもそのまま踏襲すればよく、新しいGC機構は不要。
- **3層実行モデルとの整合**: インタプリタ(`eval.c`)は呼び出し先/arityが実行時
  にしか決まらない木構造評価器であり、新ABIの恩恵(静的な引数個数決定)を原理
  的に受けにくい。**インタプリタ本体・`&rest`関数は当面スコープ外**とし、
  JIT/AOTコンパイル済みコード同士の静的呼び出しにのみ新ABIを適用する部分適用
  方針を採る。
- **重要な既存制約(コード確認済み)**: `tco_result_t`(lisp_compiled.c:11-16)は
  `step_fn_t fn`(`(lisp_val_t,lisp_val_t)->tco_result_t`型)と`lisp_val_t args`
  という単一consスロットを持つ構造体であり、AOTの末尾呼び出しトランポリンは
  本質的にconsリスト前提。za.cの末尾呼び出しトランポリン(`za_ensure_trampoline`)
  も同様。→ **末尾呼び出し(トランポリン)は当面スコープ外**とし、非末尾呼び出し
  のみを対象にする。
- 関数オブジェクトは`os_make_instance(MAGIC_FUNCTION_NATIVE, fnptr, word2, word3)`
  の4ワード固定長で、word2は「nil(組み込み)/fixnum1(JITコンパイル済み)/
  fixnum2(リフトクロージャ)」の3値タグとして既に使われている
  (`os_register_aot_init_functions`が`os_make_native_function`をそのまま使用)。

### 調査事項4の結果: 影響範囲と移行戦略(dual-entry設計)

段階的移行が現実的であり、一斉切り替えは行わない。

- 動的呼び出し経路(`apply_function`, `primitive_funcall`/`apply`,
  `os_apply_via_cell`, 各種コールバック)は**現状のconsリストABIのまま変更
  しない**。
- 各関数オブジェクトに「新ABI固定引数エントリポイント」と「旧ABIconsリスト
  エントリポイント」を両方持たせるdual-entry設計を、関数オブジェクトの
  4ワード制約を踏まえてword1を「メタデータ構造体へのポインタ」に転用する形で
  将来のABI-M4以降に導入する。
- 関数再定義との整合性: JIT/AOTの静的呼び出しコードは常にFunction Cell経由の
  間接呼び出しを維持し、実行時にarity不一致を検出したらconsリストABIへ
  フォールバックする設計とする(ABI-M5で詳細化)。

### マイルストン命名方針

`documents/transpiler.md`のM0〜M13(実態はfeature/file-ioブランチ上の別ナンバ
リングでM15相当まで進行しており、この表自体が既にstale)とは完全に独立した
番号系統として`ABI-M0`, `ABI-M1`, ... を採用する。本ドキュメントをマイルストン
計画・進捗記録のmaster documentとして育てる。コミットメッセージは
`[ABI刷新] ABI-Mn: ...`の形式(既存の`[ファイルI/O] M9: ...`慣習を踏襲)。

### マイルストンロードマップ

| ID | スコープ | リスク | 状態 |
|---|---|---|---|
| ABI-M0 | ベンチマーク計測基盤の整備(N-スケーリング、futex/N計測をtest/配下に定着) | 低 | 完了 |
| ABI-M1 | za.c: 未対応プリミティブ(NOT/CONSP/LISTP)の固定引数直接呼び出し化(非末尾) | 低〜中 | 完了 |
| ABI-M2 | 未対応プリミティブへの固定引数ラッパー拡充(車輪の横展開) | 低 | 完了 |
| ABI-M3 | AOT(transpile.lisp)側でのネイティブプリミティブ呼び出しのcons回避(非末尾) | 中 | 完了 |
| ABI-M4 | 関数オブジェクトのdual-entryレイアウト導入(振る舞い不変のリファクタリング) | 高 | 完了 |
| ABI-M5 | JIT-to-JITユーザー定義関数の静的呼び出し新ABI化(非末尾、za.c側のみ) | 高 | 完了 |
| ABI-M6 | AOT-to-AOTユーザー定義関数の静的呼び出し新ABI化(非末尾、transpile.lisp側) | 中 | 完了 |
| ABI-M7 | JIT-to-JITユーザー定義関数の静的呼び出し新ABI化(末尾、za.c側のみ) | 高 | 完了 |
| ABI-M8 | AOT-to-AOTユーザー定義関数の静的呼び出し新ABI化(末尾、tco_result_t拡張) | 高 | 完了 |
| ABI-M9+ | `&rest`ハイブリッド化、インタプリタ本体は当面スコープ外 | 未定 | 未着手 |

#### ABI-M0詳細: ベンチマーク計測基盤の整備(完了)

`jit-arithmetic-cons-elimination-instructions.md`(前回セッションの一時的な設計
メモ、`git log --all`にもリポジトリにも存在しないことを確認済み)が言及していた
「futex回数/N」計測は、Linux上でstrace等を使う前提の手法でありisiki-osの実機
ターゲット(mingw-gcc/QEMU、Linuxシステムコールを持たないbare-metalカーネル)
には適用できない。代わりに、isiki-os自身が既に持つ以下2つのカウンタを
Lispから読める形にして計測基盤とした:

- `%%HEAP-USED-BYTES`(既存): From空間の使用バイト数。
- `%%GC-COLLECT-COUNT`(ABI-M0で新設、runtime.h/runtime.c、既存の
  `os_gc_collect_count()`アクセサをLispへ露出しただけ): 累積GC発火回数。

`test/lisp/abi_bench.lisp`に、自己再帰ホットループ(za.cのTCOトランポリンで
JITコンパイルされる)の前後でこの2値の差分を取る`isiki-abi-bench-measure`
ヘルパーと、3つの比較シナリオ(baseline/not/usercall)を実装した。
`test/lisp/qemu_boot_abi_bench.lisp`から`make test-qemu-milestone
MILESTONE=test/lisp/qemu_boot_abi_bench.lisp`で単独実行できる(デフォルトの
`test-qemu`/CIには含めていない。観測ログを取るためのツールであり、pass/fail
判定を伴う回帰テストではないため)。

**n=500での実測ベースライン(2026-09-09、feature/abi-redesignブランチ、
コミット38c4d7d時点)**:
```
baseline (自己再帰のみ、呼び出しなし):      82 bytes/call
not (ABI-M1で高速パス化済み、非allocating): 82 bytes/call  (baselineと同一)
usercall (ユーザー定義関数呼び出し、未対応): 98 bytes/call  (baseline+16byte/call)
```
`not`がbaselineと完全に同一になったことは、ABI-M1の「非allocatingな固定引数
直接call」が実際にヒープ確保ゼロを達成していることの直接的な裏付けになる。
一方`usercall`はbaselineに対し**+16byte/call**(cons 1個分ちょうど)の超過が
あり、これは`za_compile_call`の一般呼び出し経路が固定引数1個のユーザー定義
関数呼び出しに対してさえconsリスト(1要素)を毎回組み立てているコストを直接
定量化したものである。ABI-M5(JIT-to-JITユーザー定義関数の静的呼び出し
新ABI化)の目標は、この+16byte/callをbaseline水準(0超過)まで削減すること
であり、実装後は本ベンチマークを再実行してこの数値が実際に0に近づくことを
確認する。

#### ABI-M1詳細: NOT/CONSP/LISTPの固定引数レジスタ渡し化(完了)

**重要な訂正(実装時に判明)**: 当初案では対象を`CAR`/`CDR`/`CONS`としていたが、
実装着手時にza.c(`za_compile_expr`、za.c:2479-2502付近)を確認したところ、
`CAR`/`CDR`/`CONS`(および`NULL`/`ATOM`/`EQ`/`+`/`-`/`*`/`<`/`=`/`>`/`<=`/`>=`)は
**既に`za_compile_unary`/`za_compile_binary`経由で`cc_car`/`cc_cdr`/`os_make_cons`を
直接callする固定引数高速パスが実装済み**であることが判明した(`za_compile_fold`と
同系統の、より一般化された既存ヘルパー)。したがって対象をこの既存の高速パスに
まだ乗っていないプリミティブへ変更した:

- **`NOT`**: `NULL`とISLisp仕様上完全に同一(`os_bootstrap`が`primitive_null`実体を
  共有登録している)にも関わらず、za.c側の判定は`NULL`シンボルのみを見ていたため
  `(not x)`は高速パスに乗っていなかった。既存の`primitive_null1`ラッパーをそのまま
  再利用し、`za_compile_unary`へのディスパッチを追加するだけで対応できた
  (新規Cコードの追加は不要)。
- **`CONSP`**: `primitive_atom1`と対になる非allocatingな核ロジックが無かったため、
  `primitive_atom`/`primitive_atom1`と同じ構造(n項版が1引数の核ロジックへ委譲する形)
  で`primitive_consp1`を新設し、`primitive_consp`をその薄いラッパーへリファクタリング。
- **`LISTP`**: 同様に`primitive_listp1`を新設し、`primitive_listp`を委譲形に
  リファクタリング。

`za_syms_t`(za.c:810-822付近)に`notsym`/`consp`/`listp`の3フィールドを追加し、
`za_try_compile_defun`内の初期化(既存の`syms.nullsym = os_make_symbol("NULL")`等の
並び)に3行追加、`za_compile_expr`のディスパッチ(既存の`nullsym`/`atom`判定の並び)に
3つの`if (head == syms->X) return za_compile_unary(..., wrapper_fn);`を追加、という
既存パターンへの機械的な追加のみで完結した(za_compile_unary/za_compile_binary自体は
変更不要)。

検証は`test/lisp/za_test.lisp`(ckpt-25〜27)に`%%za-compiled-p`によるJITコンパイル
判定+新旧ABI結果一致の回帰テストを追加し、`make test`(ネイティブgcc単体テスト、
8135アサーション)と`make test-qemu-milestone MILESTONE=test/lisp/qemu_boot_m2_za.lisp`
(実機ビルドをQEMU上で起動、181アサーション)、および`make test-qemu`(default、
init/isiki/za/za_ext5,7-19/environment系の全回帰、za.cが生成する機械語の実行を含む)
の3種すべてで0failedを確認した。`primitive_car1`/`primitive_cdr1`/`primitive_cons2`
という当初案のラッパーは、既存の`cc_car`/`cc_cdr`/`os_make_cons`直接callより
迂遠(かつ未使用)になるため追加しなかった。

#### ABI-M2詳細: 未対応プリミティブへの固定引数ラッパー拡充(完了)

ABI-M1で確立したパターン(`primitive_atom1`型の「非allocatingな核ロジック+n項版
はそれへ委譲」でruntime.h/runtime.cに`_1`/`_2`ラッパーを追加し、`za_syms_t`へ
シンボルを追加、`za_compile_expr`のディスパッチへ`za_compile_unary`/
`za_compile_binary`呼び出しを1行追加)を、`os_bootstrap`登録済みの型述語・
非allocatingな2引数プリミティブのうちまだ高速パスに乗っていなかった11個へ
機械的に展開した:

- 1引数の型述語(`za_compile_unary`): `NUMBERP`/`FIXNUMP`/`BIGNUMP`/`FLOATP`/
  `SYMBOLP`/`STRINGP`/`FUNCTIONP`/`CHARACTERP`/`STREAMP`(9個。いずれも
  タグ/magicナンバーの直接比較のみで非allocating)。
- 2引数の破壊的更新(`za_compile_binary`): `SET-CAR`/`SET-CDR`(2個。
  `cc_set_car`/`cc_set_cdr`の直接呼び出しのみで非allocating)。

いずれも既存の`primitive_XXX(args,env)`本体を`primitive_XXX1(cc_car(args))`
(または`primitive_XXX2(...)`)への委譲へ書き換えるだけで、ロジック自体は
変更していない。`test/lisp/za_test.lisp`(ckpt-28〜30)に各プリミティブの
`%%za-compiled-p`判定+新旧ABI結果一致の回帰テストを追加し、`make test`
(8135アサーション)・`make test-qemu-milestone MILESTONE=test/lisp/qemu_boot_m2_za.lisp`
(220アサーション、ABI-M1の181から+39)・`make test-qemu`(default)の3種
すべてで0failedを確認した。

未対応のまま残した候補(将来のABI-M2継続ラウンド向け): `EQL`/`EQUAL`(比較
ロジックが複雑またはconsリスト/配列の再帰比較を伴う)、`CHAR=`等の文字比較
(n-ary、`za_compile_fold`型のchain対応が別途必要)、`BASIC-ARRAY-P`等の配列
述語群(優先度が低いため保留)。

#### ABI-M3詳細: AOT側でのネイティブプリミティブ呼び出しのcons回避(完了)

**調査で判明した重要な前提**: `transpile-tail-call`(末尾呼び出しのTCOトランポリン
経路)は`*known-function-names*`(このファイル内でdefunされた関数)宛の呼び出し
にしか使われず、`transpile-tail-stmt`(transpile.lisp:1550-1575)のディスパッチ上、
ネイティブプリミティブ呼び出しは(末尾位置に書かれていても)常に`transpile-call`
経由で`transpile-expr`→`tail-return-final`で結果をreturnするだけであり、
`tco_result_t.args`のconsリスト前提(0-2で指摘した制約)には一切触れない。
つまり**`transpile-call`だけを変更すればプリミティブ呼び出しの非末尾・末尾
両方が改善され、`transpile-tail-call`自体(ユーザー定義関数の自己/相互再帰
トランポリン)には触れる必要がない**ことが確認できた。

**実装**: `*primitive-fixed-arity-c-names*`という新しい対応表
(`(name arity . c-name)`)を追加し、`*primitive-c-names*`に載っている
呼び出し先のうちABI-M1/M2でruntime.h/runtime.cへ追加した固定引数版
(`car`/`cdr`は`cc_car`/`cc_cdr`を直接、`cons`は`os_make_cons`を直接、
他は`_1`/`_2`サフィックス版)へ、実引数個数がちょうど一致する場合にだけ
使う21エントリを登録した。`transpile-call`は、`primitive-fixed-arity-c-name`
がマッチを返した場合、`transpile-cons-chain`(consリスト構築)を経由せず
`transpile-c-arg-list`(新設、カンマ区切り整形のみ)で引数一時変数を直接
渡す。既存の`*known-function-names*`優先順位(ユーザーが同名でdefunして
プリミティブを再定義した場合はそちらを優先する)を壊さないよう、
`(member name *known-function-names*)`の場合はこの高速パスを使わないガード
を明示的に入れた。引数の評価自体は既存の`transpile-call-args-guarded`
(GC_PROTECT・非局所脱出の短絡処理)をそのまま再利用しており、GC安全性に
関する変更は無い(評価済みの一時変数は既にGC_PROTECTされているため、
consリストへ包む代わりに直接渡しても安全)。

**生成コードでの確認**: `make transpile`で再生成した`lisp_compiled.c`を
確認したところ、例えば`member`のAOT実装が`(null list)`→`primitive_null1(...)`、
`(eq item (car list))`→`primitive_eq2(item, cc_car(...))`のように、
consリスト構築を経由しない直接呼び出しへ変わっていることを確認した。一方
`(append list1 (cdr list2))`のような**ユーザー定義AOT関数(append2)への
呼び出しは従来通り`os_make_cons(...)`でconsリストを構築**しており、
意図通りプリミティブ呼び出しのみが変更されている(ABI-M5がこちらを
改善する対象として残る)。

**検証**: `make test`(ネイティブgcc単体テスト、AOT変換結果をそのまま
実行、8135アサーション)、実機ビルドをQEMU上で起動する`make test-qemu`
(default、1589アサーション)、および`make test-qemu-all`のうち
`qemu_boot_m5_ide.lisp`(5)・`qemu_boot_m6_fat16.lisp`(152)・
`qemu_boot_fat32.lisp`(135)——いずれもM15でAOTトランスパイル対象へ
移動したdevice/ide/partition/fat16/fat32ドライバコードを実際に実行する
ため、ABI-M3の影響を最も強く受ける経路——の3マイルストンすべてで0failedを
確認した。

**誤検知だったハングの切り分け**: `test-qemu-all`の`qemu_boot_fat32_primary_boot.lisp`
マイルストンが単体実行でも590秒でタイムアウトしたため、当初ABI-M3による
回帰を疑い、`git stash`で`transpile.lisp`の変更を退避してABI-M2時点の
コードでも同じマイルストンを実行したところ、**変更前のベースラインでも
同一のタイムアウトが再現**した。これは本セッションの環境(KVM無しの
QEMU/TCG)がこのマイルストンには単純に遅すぎるという、`test-qemu-stress`/
`test-qemu-perf`が既にCI対象外とされているのと同種の既存の特性であり、
ABI-M3による回帰ではないと判断した(`test-qemu-all`/`test-qemu-perf`同様、
このマイルストンおよびそれに続く`qemu_boot_partition.lisp`系はローカルで
時間をかけて確認するか、KVMが使える環境で確認する必要がある。今回のセッションの
サンドボックスでは確認できなかった)。

#### ABI-M4詳細: 関数オブジェクトのdual-entryレイアウト導入(完了)

**実装**: 関数オブジェクト(`MAGIC_FUNCTION_NATIVE`)は4ワード固定長のため、
案A(word1を「メタデータ構造体へのポインタ」に転用)を採用した。
`za_fn_meta_t { UINT64 cons_entry; UINT64 fixed_entry; UINT64 arity; }`を
runtime.hに新設し(`cons_entry`をoffset 0固定、za.cの手書き機械語から
シンボルオフセット無しで直接dereferenceできるようにするため)、
Immobilized Space上のバンプカーソル(`g_fn_meta_cursor`、Function Cellの
`g_function_cell_cursor`と同じ仕組み)から`os_fn_meta_alloc`で確保する。
- `os_make_native_function`/`os_make_jit_function`/`os_make_lifted_closure`
  (3コンストラクタ)を、fnptrを直接word1へ渡す形から、`os_fn_meta_alloc(fnptr)`
  でmetaを確保しその生ポインタをword1へ渡す形に変更(`fixed_entry`/`arity`は
  常に0=未対応のまま、ABI-M5まで振る舞いに影響しない)。
- `eval.c`の`apply_function`(MAGIC_FUNCTION_NATIVE分岐)を、word1を直接
  関数ポインタとしてcastする形から、word1をmetaへのポインタとしてcastし
  `meta->cons_entry`を関数ポインタとして呼ぶ形に変更。
- `za.c`の`za_ensure_trampoline`(JIT関数呼び出しの共有トランポリン)を、
  `obj[1]`への直接jmpから、`obj[1]`(meta)を経由して`meta->cons_entry`
  (offset 0)へjmpする2段dereferenceに変更(手書き機械語、`jit_mov_reg_from_mem_disp8`
  を1命令追加しただけ)。
- `os_reset_runtime_state_for_test`(ネイティブ単体テストがヒープを
  再確保するたびに呼ぶリセット関数)に`g_fn_meta_cursor`のリセットを追加
  (追加し忘れるとstaleなポインタがテスト間で残留するクラスのバグになる、
  `g_function_cell_cursor`と同じ理由)。

`gc_scan_instance`/`os_make_instance`のGC_PROTECT判断ロジック自体は変更
不要だった(word1は「タグ無しの生ポインタ」という性質自体は変わらず、
指す先がコード領域からImmobilized Space上のメタデータに変わっただけで、
どちらもGCの移動・スキャン対象外という点は同じため)。

**検証で判明した誤検知(重要)**: 初回の`make test-qemu-milestone
MILESTONE=qemu_boot_m6_fat16.lisp`実行で32〜39件の失敗が発生し、しかも
「テスト冒頭のディレクトリ一覧が最終状態を含んでいる」という不可解な
パターンだった。`git stash`でのベースライン比較では再現しなかったため
(ベースラインは別の失敗要因である`test-qemu-all`のプロセス管理問題で
汚染されていた)、ディスクイメージを独立してマウントし中身がpristineな
初期状態であることを直接確認した上で、`make test-qemu-milestone`を経由
せず`qemu-system-x86_64`を直接起動して検証したところ、FAT16は152件、
FAT32は135件、いずれも0failedで完全に一致した。原因は`make
test-qemu-milestone`のディスクイメージ準備(Dockerコンテナ内でのloop
マウント書き込み)とQEMU起動によるホスト側読み取りの間のタイミング競合
(本セッション中に大量のQEMUプロセスを連続起動したことによる高負荷が
誘発した、ABI-M4と無関係な既存のテストインフラ側の問題)であり、
ABI-M4のコード自体には問題が無いことを確認した。

**検証結果**: `make test`(8135)、`make test-qemu`(default、1589)、
`qemu_boot_m5_ide.lisp`(5)、`qemu_boot_m6_fat16.lisp`(152、直接起動で
検証)、`qemu_boot_fat32.lisp`(135、直接起動で検証)——いずれもABI-M3
時点と全く同じアサーション数・0failedで一致し、「振る舞い不変」の
要件を満たしていることを確認した。

#### ABI-M5詳細: JIT-to-JITユーザー定義関数の静的呼び出し新ABI化(非末尾、完了)

**スコープ**: ABI-M4のdual-entryレイアウトを実際に活用し、za.c(JIT)側の
非末尾呼び出し(`za_compile_call`)で、呼び出し先が固定引数(`&rest`無し・
fixed_count<=3)のJIT関数であることが実行時に確認できた場合、consリストを
一切構築せずレジスタ渡しで直接callする。末尾呼び出し(トランポリン経由)は
ABI-M3/M4同様スコープ外のまま。transpile.lisp(AOT)側は本マイルストンでは
未着手。

**設計(パラメータスロット方式)**: 「double-compile(本体を2回コンパイルする)」
ではなく、「呼び出し規約が違うだけで本体は共有する」方式を採用した。
- 新フレームオフセット`ZA_OFF_PARAM_BASE`以降に`ZA_MAX_FIXED_ENTRY_PARAMS`(=3)
  個分のパラメータスロット(`za_param_val_off(i)`/`za_param_node_off(i)`)を
  追加。
- コンパイル対象の関数本体に`lambda`/`flet`/`labels`が1つも現れない
  (`za_form_disables_param_slots`による事前スキャン)場合のみ、グローバル
  フラグ`g_za_use_param_slots`を立て、`za_emit_operand`のパラメータ参照
  (`is_literal==0`)がconsリストを都度辿る代わりにこのスロットを直接読む
  ようにする。lambda/flet/labelsを含む関数は保守的に対象外とする(エスケープ
  するクロージャ/flet/labels束縛関数は別のparams/fixed_countコンテキストで
  `param_index`を解決するため、外側関数のスロットと取り違える危険があるため)。
- `za_try_compile_defun`は、対象関数の場合のみ「固定引数レジスタ渡し
  エントリポイント(`rcx=env, rdx=arg0, r8=arg1, r9=arg2`を一度だけ
  パラメータスロットへ展開してから共有本体へjmp)」を「consリストエントリ
  ポイント(こちらも一度だけconsリストをパラメータスロットへ展開してから
  共有本体へ自然に流れ落ちる)」より前に出力する。両アドレスは
  `os_make_jit_function_dual(cons_entry, fixed_entry, arity)`でmetaへ登録する。
- `za_compile_call`の非末尾呼び出し経路(`argc<=3`)は、Function Cellの
  現在の中身に対し実行時に4条件を判定してから分岐する: (1)cellがnilでない、
  (2)`obj[0]==MAGIC_FUNCTION_NATIVE`、(3)`meta->arity==argc`、
  (4)`meta->fixed_entry!=0`。全て満たせば`fixed_entry(env, arg0, ...)`を
  レジスタ渡しで直接call、1つでも満たさなければ従来の
  `os_apply_via_cell`(consリストABI)へフォールバックする。関数再定義は
  Function Cellの中身を差し替えるだけなので、コンパイル時ではなく
  実行時判定が必須(cellのアドレス自体は静的に確定するが、中身は変わりうる)。

**実装中に発見・修正した2つのバグ(いずれも重要な教訓)**:

1. **je/jne反転バグ**: `meta->fixed_entry != 0`のチェックに誤って
   `jit_emit_jne_rel32_placeholder()`(不一致ならfallbackへ)を使っており、
   本来は`jit_emit_je_rel32_placeholder()`(0と一致=未対応ならfallbackへ)が
   正しかった。この1文字レベルの条件反転により、高速pathが実行時に絶対に
   選ばれない状態になっていた。QEMU実機上の`%%GC-COLLECT-COUNT`マーカー
   手法や新規プリミティブ追加によるデバッグではこのバグ自体を特定できず
   (後述のnative probe手法で初めて可視化できた)、いったん修正したが
   ベンチマークに変化が出ず、根本原因は2にもう1つあることが判明した。

2. **consリスト構築(手順4)が実行時分岐より前に無条件実行されていた
   (根本原因)**: `za_compile_call`の元の実装は、手順4(consリストを
   os_make_consでfold構築)を手順6a(高速path/fallbackの実行時分岐)より
   *前*に無条件で出力していた。そのため高速pathを選んでも、その前段階で
   毎回consセル(16byte)を確保してしまい、「consリストを一切構築しない」
   という設計意図が実装に反映されていなかった。修正として、4条件の実行時
   判定をconsリスト構築より前に移動し、判定に成功した場合は手順4を完全に
   スキップして`fn`/引数のリンクだけを外してから直接callするよう
   `za_compile_call`を再構成した(新設のstaticヘルパー
   `za_emit_call_build_acc_and_unlink`に手順4+5をまとめ、高速path失敗時・
   `argc>3`の場合・末尾呼び出しの3箇所からそれぞれ呼ぶ形にした)。

**診断手法として有効だった「nativeプローブ」**: QEMU実機上での
`%%GC-COLLECT-COUNT`マーカーや新規プリミティブ追加によるデバッグは
原因不明のまま行き詰まったため、ユーザーの提案で「za.cをネイティブgccで
`-DISIKIOS_UNIT_TEST`無しでビルドし(`za_try_compile_defun`冒頭の
`#ifdef ISIKIOS_UNIT_TEST return nil;`を回避)、`za_try_compile_defun`を
直接呼んで生成された機械語をファイルへダンプ、`objdump`で逆アセンブル
確認する」手法に切り替えたところ、QEMU起動サイクル無しで数秒で
再コンパイル・再実行でき、劇的に診断が高速化した。この過程で以下も
副次的に発見・修正した:

- **プローブ自体のGC安全性バグ**: プローブの`main()`内でconsセル/シンボルを
  構築する際、通常のruntime.c/za.cコードと同様`GC_PROTECT`マクロで
  shadow stackに登録しないと、自動GC発火時(`os_alloc_bytes`がFrom空間
  枯渇時に`os_gc_collect()`を自動発火する)にダングリングポインタになる。
  今回のクラッシュの直接原因ではなかったが(下記`za_form_disables_param_slots`
  バグの方が先に踏まれた)、プローブの正しさとして必須の修正だった。
- **`za_form_disables_param_slots`の無限再帰バグ**: 新設した事前スキャン
  関数のループ条件が`(rest & TAG_MASK) == TAG_CONS`のままで、`rest != nil`
  のチェックが抜けていた。isiki-osでは`nil`自体が`TAG_CONS`(`g_nil_cell`
  への自己参照セル)としてタグ付けされているため、リスト終端`nil`に到達
  しても停止条件を満たしてしまい、`cc_cdr(nil)==nil`のまま停止しない
  (かつ`elem`もnilなので`za_form_disables_param_slots(nil)`への無限再帰)、
  スタックオーバーフローでセグフォルトしていた。本体が裸のシンボル1個
  だけの関数(`(defun identity (x) x)`)はこの関数呼び出し自体を素通り
  するため気づかず、`progn`等ネストしたconsを含む本体(呼び出しを含む
  実用的などんな関数でも該当)で初めて踏む形だった。`za.c`の他の全ての
  コンスリスト走査箇所(`za_compile_progn`等)が一貫して`rest != nil`を
  ループ条件に使っている既存の慣習に合わせて修正した。

**検証結果**: `make test`(8135)、`make test-qemu-milestone
MILESTONE=qemu_boot_m2_za.lisp`(220)、`make test-qemu`(default、1589)——
いずれも0failedで、ABI-M4時点と同じアサーション数のまま一致した。
`abi_bench.lisp`(n=500)は、修正前は`usercall(slow-path)`が
`baseline=82`に対し`98`(+16byte/call、consセル1個分)のままだったが、
修正後は`baseline=81`/`not(fast-path)=81`/`usercall=81`と完全に一致し、
「JIT-to-JITユーザー定義関数の非末尾呼び出しがconsリストを一切構築しない」
という設計目標を実測で確認できた。

#### ABI-M6詳細: AOT側ユーザー定義関数の静的呼び出し新ABI化(非末尾、完了)

**スコープ**: ABI-M5(za.c/JIT側)の対になる、transpile.lisp(AOT)側での
非末尾呼び出しのconsリスト回避。ABI-M3の検証時点で「`append2`のような
ユーザー定義AOT関数への呼び出しは従来通り`os_make_cons`でconsリストを構築
しており、これは別マイルストンの対象として残る」と明記していた項目に
対応する。末尾呼び出し(`transpile-tail-call`/トランポリン)は引き続き
スコープ外(ABI-M3同様、`tco_result_t.args`のconsリスト前提に触れない)。

**za.c(ABI-M5)との重要な設計上の違い**: AOTの既知関数呼び出しは
`call-target-c-name`によりリンク時に確定するC関数名への直接呼び出しであり、
za.cのようなFunction Cell経由の間接呼び出しではない。そのため関数再定義は
呼び出し元のC関数名解決に影響せず(既存の`call-target-c-name`が
`*known-function-names*`優先で解決する仕組みそのものが再定義安全性を担保
済み)、**ABI-M5が必要とした実行時4条件判定(nil/MAGIC/arity/fixed_entry
チェック)は不要で、コンパイル時(トランスパイル時)にarity一致を確認する
だけで高速pathを選べる**。また、za.cはレジスタ渡し(rcx/rdx/r8/r9)という
物理的な制約からパラメータ数を`ZA_MAX_FIXED_ENTRY_PARAMS`(=3)に制限した
のに対し、AOTは通常のC関数呼び出し(コンパイラが必要に応じてスタックへ
自動的にスピルする)であるため、パラメータ数に上限を設けていない。

**実装**: 1つのdefunにつき、既存の2つのC関数(`name__step`/consリストABI
公開ラッパー`name`)に加え、`&rest`を持たずパラメータ数が1個以上の場合のみ
2つのC関数を追加する: `static name__step_fixed(env, arg0, ...)`
(直接受け取ったCパラメータをそのまま束縛するプロローグのみが`__step`と
異なり、本体のC文テキスト自体は`transpile-tail-stmt`が1度生成したものを
そのまま再利用する。za.cのABI-M5が「パラメータスロット方式」で採用した
「エントリの前段だけを複製し本体を共有する」設計と同じ考え方をCソース
レベルで実現したもの)と、それをトランポリンループで包む公開ラッパー
`name__fixed(env, arg0, ...)`(`__step_fixed`はこのファイル内の`__fixed`
からしか呼ばれないため`static`にでき、`__step`自身が末尾呼び出し
トランポリンのためファイルを跨いでstaticにできないのとは対照的)。
`*known-function-arities*`(mainが`*known-function-names*`と同時に束縛する
`(name . fixed-arity)`のalist、`&rest`を持つ関数はnil)と
`known-function-fixed-arity`ヘルパーで、`transpile-prototype`/
`transpile-defun`/`transpile-call`のいずれからも対象関数を判定できるように
した。`transpile-call`は、呼び出し先が`*known-function-names*`に載っており
実引数個数がその固定arityとちょうど一致する場合、consチェーン構築
(`transpile-cons-chain`)を経由せず`name__fixed(env, arg0, ...)`を直接
callする(ABI-M3の`*primitive-fixed-arity-c-names*`と同じ
`transpile-call-args-guarded`/GC_PROTECT機構をそのまま再利用しており、
GC安全性に関する変更は無い)。

**生成コードでの確認**: `make transpile`で再生成した`lisp_compiled.c`
(20498行)を確認したところ、`__fixed`呼び出し1152箇所・`__fixed`関数定義
723個が生成された。ABI-M3の検証時に「consリストを構築したまま」と記録
した`append2`自身の非末尾自己呼び出し(`(append (car list1) (append2 (cdr
list1) list2))`)が`lisp_ll_append2__fixed(env, __call_arg_21,
__call_arg_22)`という直接callに変わっていることを実際に確認した。また
`&rest`パラメータを持つ`list`(`(defun list (&rest items) items)`)には
`__fixed`が一切生成されないことも確認し、対象外判定が意図通り機能して
いることを確認した。

**検証結果**: `make test`(8135)、`make test-qemu`(default、1589)、
`qemu_boot_m5_ide.lisp`(5)、`qemu_boot_m6_fat16.lisp`(152)、
`qemu_boot_fat32.lisp`(135)——いずれもABI-M5時点と同じアサーション数の
まま0failedで一致した。1152箇所という実際のAOTコードベース全体
(`utility.lisp`/`fat16.lisp`/`fat32.lisp`/`mount.lisp`等)の呼び出しが
この検証を通っているため、ABI-M0のような合成ベンチマーク(N回ループの
heap-delta測定)は追加していない(ABI-M3の前例と同じ判断: AOT側は
生成コードの直接確認+実際の呼び出しを大量に含む既存テストスイート全体の
0failedで十分な検証になる。テストコード側から新規にAOT関数を追加できない
制約もあり、za.cのabi_bench.lispのような独立した計測基盤を作るコストに
見合わないと判断した)。

#### ABI-M7詳細: JIT-to-JITユーザー定義関数の静的呼び出し新ABI化(末尾、za.c側のみ、完了)

**スコープ**: ABI-M5の非末尾呼び出し高速pathを、末尾呼び出し(自己/相互再帰の
トランポリン経由の呼び出し)にも拡張する。za.c側のみ(AOT側のtco_result_t
再設計を伴う末尾呼び出し対応は別マイルストンとしてスコープ外のまま)。
着手前にユーザーへ選択肢を提示し、「末尾呼び出し対応(za.c側、当初はargc<=2
を想定)」を選んでもらった。

**当初の懸念(register制約)は実装過程で解消**: 当初、共有トランポリン
(`za_ensure_trampoline`)へ`cell+env+argc個`を全てレジスタで渡す設計を想定し、
MS x64の4本の整数引数レジスタでは`cell+env+arg0+arg1`(=4本)までしか収まらず
argc<=2に制限されると見積もっていた。しかし実装時に、ABI-M5の非末尾高速path
と全く同じ構造(実行時4条件判定を呼び出しサイトの機械語に直接埋め込み、
チェック通過後は判定に使ったscratchレジスタを使い回して`env`/`arg0..2`だけを
`rcx/rdx/r8/r9`へ積み直してfixed_entryへ直接call/jmpする)を末尾呼び出しにも
そのまま採用できることに気づいた。共有トランポリンを経由せず呼び出しサイト
自身が判定するため、`cell`はレジスタに残す必要がなく(fixed_entryのアドレスを
取り出した後は不要)、ABI-M5の非末尾pathと全く同じ`ZA_MAX_FIXED_ENTRY_PARAMS`
(=3)がそのまま使える。この発見によりargc<=2という当初の制約は撤廃した。

**実装**: `za_compile_call`の6b(末尾)分岐に、6a(非末尾、ABI-M5)と同一の
4条件判定(nil/`MAGIC_FUNCTION_NATIVE`/arity一致/`fixed_entry`非0)を、
consリスト構築(手順4)より前に追加した。判定に成功した場合:
1. fixed_entryのアドレス(判定直後は`rax`)を、直後の`za_gc_unlink`呼び出し
   (volatileレジスタを破壊しうる通常のC呼び出し規約)を跨いで生き残らせる
   ため、callee-savedな`r13`へ退避する。
2. `za_call_saved_head_off(call_depth)`経由でfn/引数分のリンクを外し、続けて
   `ZA_OFF_ENV_SAVED_HEAD`経由でenv分のリンクも外す(consリスト版と同じく
   2段階。accは未構築のため、この時点でリンクされているのはfn/引数/envの
   みで、他に外すものは無い)。
3. `env`/`arg0..2`を値スロットからレジスタ(`rcx`/`rdx`/`r8`/`r9`)へ読み出し、
   `r13`(fixed_entryアドレス)を`r11`へ移してから自分のフレームを完全に畳み
   (`add rsp`+`pop r13`+`pop rbx`、consリスト版と同じ手順)、`jmp r11`で
   fixed_entryへ直接tail-jmpする。これは素のJMP命令(callではない)であり、
   jmp先がさらに末尾再帰してもCスタックは一切伸びない(既存の共有トランポリン
   と同じくJMPチェーンとして動作する)。
判定に失敗した場合(または`argc>ZA_MAX_FIXED_ENTRY_PARAMS`で判定自体を試みない
場合)は、従来通り手順4(consリスト構築)+手順5(unlink)を経て共有トランポリン
経由(consリストABI)へフォールバックする。手順4/5を出力するコードは、6a/6bの
どちらからも呼べるよう`za_emit_call_build_acc_and_unlink`という共通static
ヘルパーへ抽出した(元は呼び出し前に無条件で1回だけ出力していたが、ABI-M5で
6aが先に「consリスト構築より前に判定」という構造に変わった際、6bのために
そのまま残しておいた箇所を今回6bにも同じ構造を導入するために抽出した)。

**診断で判明した重要な教訓(ネイティブプローブの限界)**: 当初、ABI-M5/M6で
有効だった「za.cをネイティブgccでビルドし、実際に`za_try_compile_defun`が
生成した関数を関数ポインタとして直接呼んで実行する」プローブ手法を試みた
ところ、深い自己末尾再帰が無限ループし、浅い再帰でも不正な値を返した。
signalハンドラでフォルト位置を、C側のprintfマーカーで実行経路を丹念に
追跡した結果、コンパイル時のC生成ロジック自体は意図通り(4条件判定・
レジスタ読み出し・スタック整合すべて手計算で検証して正しいことを確認済み)
だったが、**実行時にza.cのJIT生成コード(常にMS x64 ABI = rcx/rdx/r8/r9で
C関数を呼ぶ前提)が、ネイティブLinux gcc(既定でSystem V AMD64 ABI = 
rdi/rsi/rdx/rcxで受け取る)でビルドしたコールバック先C関数(cc_car・
primitive_subtract2・za_gc_unlink等)を直接呼んでいたためABI不一致が
発生し、全ての引数がずれて渡っていた**ことが根本原因と判明した。ABI-M5/M6の
ネイティブプローブはこの不一致を踏んでいなかった(M5は生成バイト列を
objdumpで目視確認するのみで実行しなかった、M6はAOT/C側でJIT機械語を
一切生成しない)ため今回初めて顕在化した。この問題はプローブ環境固有の
制約であり、za.c自身が対象とする実機ビルド(`x86_64-w64-mingw32-gcc`、
呼び出し元・呼び出し先どちらもMS x64 ABIで一貫)には存在しない。ネイティブ
プローブでの実行検証はここで断念し、実機ABIが一貫して保証されるQEMU実行
検証に切り替えた(デバッグ計装は全てza.cから削除済み)。

**QEMU実行での実証(既存テストの再活用)**: 幸い`test/lisp/za_test.lisp`
(拡張3節)には、ABI-M7のfast pathをまさに直接exerciseする既存テストが
既に存在していた: `isiki-za-test-fact-iter`/`isiki-za-test-sum-iter`
(いずれも2引数のアキュムレータ渡し自己末尾再帰、`sum-iter(200000, 0)`で
20万段の深い末尾再帰を行い`20000100000`という結果を検証)と、
`isiki-za-test-even-p`/`isiki-za-test-odd-p`(1引数の相互末尾再帰)。
いずれも実機ABIが一貫するQEMU上で実行され、20万段の深い自己再帰でも
クラッシュせず(=真のTCOが機能している証拠)、計算結果も完全に一致した
(=レジスタ渡しの値が破損していない証拠)。

**検証結果**: `make test`(8135)、`make test-qemu-milestone
MILESTONE=qemu_boot_m2_za.lisp`(220、上記の深い自己/相互末尾再帰テストを
含む)、`make test-qemu`(default、1589)、`qemu_boot_m5_ide.lisp`(5)、
`qemu_boot_m6_fat16.lisp`(152)、`qemu_boot_fat32.lisp`(135)——いずれも
ABI-M6時点と同じアサーション数のまま0failedで一致し、「振る舞い不変」を
満たしていることを確認した。

#### ABI-M8詳細: AOT-to-AOTユーザー定義関数の静的呼び出し新ABI化(末尾、完了)

**スコープ**: ABI-M6(AOT非末尾)の対になる、transpile.lisp(AOT)側での
末尾呼び出しのconsリスト回避。`tco_result_t`の再設計を伴う、当初ロードマップで
「高リスク」と位置付けられていた項目。za.c側は既にABI-M5(非末尾)/ABI-M7
(末尾)の両方が完了済みのため、これでza.c/AOT双方が非末尾・末尾とも新ABIに
対応したことになる。

**設計(tco_result_tの拡張)**: `struct tco_result`に`void *fixed_fn`
(実際の呼び出し先ごとに引数個数が異なる`name__step_fixed`への生ポインタ、
`step_fn_t`のような単一の関数ポインタ型で表現できないためvoid*で持つ)、
`UINT64 fixed_argc`、`lisp_val_t arg0/arg1/arg2`を追加した。`is_tail_call`の
意味を0(確定値)/1(従来のconsリストABI継続、fn/args)/2(ABI-M8の固定引数ABI
継続、fixed_fn/fixed_argc/arg0..2)の3値に拡張した。`fixed_argc`は
`*tco-max-fixed-argc*`(=3、za.cのABI-M5/M7の`ZA_MAX_FIXED_ENTRY_PARAMS`と
揃えた値)までに制限する(tco_result_tは全呼び出し先で共有する固定サイズの
構造体のため、AOTの非末尾呼び出し(ABI-M6)自体には無い上限がここでは必要に
なる)。

**実装**: `transpile-tail-call`に、呼び出し先が`known-function-fixed-arity`で
判定できる固定引数関数(&restを持たず、実引数個数がちょうどarityと一致し
`*tco-max-fixed-argc*`以下)の場合の高速pathを追加した。既存の
`transpile-tail-call-args-guarded`と同じGC-safe評価+非局所脱出短絡規則
(`transpile-tail-call-fixed-args-guarded`)で引数を一時変数へ評価した後、
consチェーンを組み立てず`return (tco_result_t){.is_tail_call = 2, .fixed_fn =
(void *)name__step_fixed, .fixed_argc = N, .arg0 = ..., ...};`を返す。
両方の公開ラッパー(`name(evaluated_args, env)`・ABI-M6の`name__fixed`)が
共有するトランポリン駆動ループを`emit-trampoline-drive-loop`という1つの
ヘルパーへ集約し(以前は2箇所にほぼ同じ`while`ループを重複して出力していた)、
`is_tail_call==2`の場合は`fixed_argc`に応じた関数ポインタ型へ`fixed_fn`を
キャストしてcallするよう拡張した。AOTの既知関数呼び出しはリンク時確定の
直接呼び出しのため、ABI-M6/M7と同じ理由でza.cのような実行時判定は不要。

**発見・修正したリンクエラー**: 実装当初、ABI-M6が追加した`name__step_fixed`
を`static`(「このファイル内の`name__fixed`からしか呼ばれない」という
当時の前提)のままにしていたところ、`transpile-tail-call`が他の(同一
ファイル内で前方参照になる、またはファイルを跨ぐ)関数の末尾呼び出しから
`fixed_fn`として直接参照するようになったため、`__step`自身が既に踏んでいた
のと全く同じ理由(M15のファイル分割で相互末尾呼び出しのためstatic化を
撤廃した経緯)で大量のリンクエラー(`undeclared`)が発生した。`__step_fixed`
を`__step`と同様非staticにし、`transpile-prototype`にも前方宣言を追加して
解決した。

**生成コードでの確認**: `make transpile`で再生成した`lisp_compiled.c`を
確認したところ、`is_tail_call = 2`を使う末尾呼び出し継続が240箇所
(フィクスチャ側に35箇所)生成された。ABI-M6で確認した`member`の非末尾
呼び出しに続き、`member`自身の末尾自己再帰(`(member item (cdr list))`)も
`fixed_fn = (void *)lisp_ll_member__step_fixed, fixed_argc = 2`という
直接ディスパッチに変わっていることを確認した。

**検証結果**: `make test`(8135、AOTコンパイル済みCコードをネイティブで
実際に実行するため、`is_tail_call==2`のswitch/case分岐も含めトランポリン
駆動ループ自体が実地で検証される)、`make test-qemu-milestone
MILESTONE=qemu_boot_m2_za.lisp`(220)、`make test-qemu`(default、1589)、
`qemu_boot_m5_ide.lisp`(5)、`qemu_boot_m6_fat16.lisp`(152)、
`qemu_boot_fat32.lisp`(135)——いずれもABI-M7時点と同じアサーション数の
まま0failedで一致し、「振る舞い不変」を確認した。

### 実測による重要な追加知見: 新ABIは常に速くなるとは限らない(2026-09-09)

ABI-M0〜M8完了後、`main`(ABI刷新着手前)と本ブランチとで実際の壁時計時間を
比較する追加調査を行った。

**FAT16ファイル読み込み(`read-file-into-vector`)は無関係と判明**:
`test/lisp/perf_fat16_test.lisp`の#41(カーネルバイナリ読み込み)相当を
20000〜100000byte規模に縮小して計測したところ、`main`とfeature/abi-redesign
で読み込み速度に有意差は無かった(いずれも約0.55ms/byte)。副次的に
`STREAM_READ_CHUNK`(9P/FAT共有のリフレッシュバッファサイズ、既定1024byte)を
安易に4096byteへ拡張すると`stream.h`の`buf_data[1024]`固定配列を超えて
書き込むバッファオーバーフローになることを発見した(4096byteに拡張する場合は
`buf_data`のサイズも同時に変更する必要がある)。バッファサイズを正しく
拡張して再測定しても速度は変わらず、`read-byte`/`elt`/`set-elt`を
`*primitive-fixed-arity-c-names*`に追加してconsチェーン構築を無くしても
速度は変わらなかった。ボトルネックはQEMU(TCG、KVM無し)環境のIDE PIO転送
そのもの(1セクタ512byteにつき256回の16bit `IN`命令ポーリングが必要で、
そのエミュレーションオーバーヘッドが支配的)であり、Lisp/C層のいかなる
変更でも改善しないハードウェアエミュレーション特性と判明した。

**算術プリミティブ(+/-/=)を多用する自己末尾再帰は、むしろ10〜13%遅くなった**:
`(defun bench-add-loop (n acc) (if (= n 0) acc (bench-add-loop (- n 1) (+ acc n))))`
という極小の自己末尾再帰ループ(n=200000)を、AOT側(`utility.lisp`に追加)と
JIT側(test/lisp配下、za.cでコンパイル)の両方で用意し、QEMU実機上で
`get-internal-real-time`により計測した。

| 実行方式 | main | feature/abi-redesign | 差 |
|---|---|---|---|
| AOT | 336 ticks(3.36秒) | 372 ticks(3.72秒) | +11% |
| JIT | 2484 ticks(24.84秒) | 2820 ticks(28.20秒) | +13% |

ネイティブ実行(QEMUを介さず`lisp_compiled.c`を直接リンクして計測)でも
同じ方向の結果(main 0.317秒 vs feature/abi-redesign 0.336秒)が出ており、
測定誤差ではなく実際の傾向と考えられる。

`*primitive-fixed-arity-c-names*`による`+`/`-`のcons回避自体は
`primitive_add(os_make_cons(...), env)`→`primitive_add2(a, b)`という
生成コードの変化として確実に起きていること(209箇所→8箇所、ABI-M3以降の
コミット参照)を`lisp_compiled.c`の差分で直接確認済みだが、それでも
このマイクロベンチマークでは遅くなった。理由として、以下が有力と考えられる:

- consセル確保はbump allocator(ポインタ加算+2語書き込み)で数命令程度と
  非常に軽量であり、削減できる絶対コストがそもそも小さい
- 一方、ABI-M5/M7(JIT、末尾呼び出し)は呼び出しのたびにnil/MAGIC_FUNCTION_
  NATIVE/arity一致/fixed_entry非0の4条件判定+unlink呼び出し2回を追加し、
  ABI-M8(AOT、末尾呼び出し)はトランポリン駆動ループに`fixed_argc`に応じた
  `switch`分岐を追加している。これらの判定コストが、削減したconsセル
  1〜2個分のコストを上回ってしまったと考えられる

**教訓**: 関数呼び出しの新ABI化(特に実行時判定を伴う末尾呼び出し高速path、
ABI-M5/M7/M8)は、`member`/`append2`のように**引数が多い・呼び出しあたりの
本体処理量も大きい**関数では判定コストを相殺して余りある削減が見込める一方、
本ベンチマークのような**極小の呼び出し(2引数の算術のみ)を高頻度に繰り返す
ケースでは判定コストがcons削減コストを上回り、正味で悪化しうる**。新ABI化は
無条件に有益とは限らず、GC発火回数・heap使用量の観点では引き続き改善で
あることは事実だが、壁時計時間で見た場合はワークロード依存であることが
判明した。ABI-M9以降で同種の最適化を追加する場合は、この種のマイクロ
ベンチマークでの検証も併せて行うべきである。

**strace(futex回数)による再検証**: 過去セッションで使われていた
「futex回数/N」計測手法(`abi_bench.lisp`冒頭コメント参照)を、`strace -c -f`で
QEMUプロセス自体をトレースする形で再現した(ネイティブ実行では
`os_make_cons`がbump allocatorでmalloc/pthreadを一切使わないためfutex呼び出しが
0件になることをまず確認済み。QEMU自身がマルチスレッド(vCPU/IOスレッド間の
futexベース同期)であるため、ゲスト側の命令実行量に応じてfutex回数が変動する)。
`isiki-bench-jit-add-loop`(n=200000)を両ブランチで完全に同一条件で実行した
ところ、futex回数はmain 19881回に対しfeature/abi-redesign 19185回
(-3.5%、総syscall数も-4.4%)と、**syscallベースの指標ではわずかに改善**
していた。これは上記の壁時計時間(+13%、悪化)と逆方向であり、両者は
別のものを計測していると考えられる: futex回数はQEMU内部のスレッド間
同期イベント数(consリスト構築等でコード密度・実行パスが変わることの
間接的な影響を受ける)を反映するのに対し、壁時計時間はABI-M5/M7が
追加した実行時判定(nil/MAGIC/arity/fixed_entryチェック等、syscallを
一切伴わないユーザー空間内の純粋なCPU命令)のコストを直接反映する。
「cons回避によりQEMU側のスレッド同期オーバーヘッドは減ったが、その分を
上回る判定コストがCPU時間に乗った」という、より完全な説明になる。

### 真因の発見と修正: primitive_add2等が実はconsを回避していなかった(2026-09-10)

上記の「壁時計時間が悪化した」という結果を、ヒープサイズを変えてGC発生回数を
意図的に変化させる実験(N=2,000,000、heap=16MB/64MB/1024MB)で直接検証した
ところ、**GC発生回数と実行時間に相関は無かった**(heap=1024MB・GC0回が
0.38秒で最も遅く、heap=16MB・GC30回が0.31秒で最も速かった)。これは
Cheney方式のコピーGCのコストが生存データ量に比例しゴミの量には比例しない
ため、本ベンチマークのように生存データがn/accの2値のみ(呼び出しのたびに
使い捨てられるconsセルは即座にゴミになる)の場合、GCが何度発火しても
コピーするものがほぼ無くGC自体のコストは無視できるほど小さいことに
起因する。

この結果を踏まえ、ABI-M1以前から存在する`primitive_add2`/
`primitive_subtract2`/`primitive_num_equal2`/`primitive_less_than2`/
`primitive_greater_than2`/`primitive_greater_equal2`(2引数固定版、
za.c向けの対症療法として本セッション開始前から存在)の実装を確認した
ところ、以下の重大な見落としが判明した:

```c
lisp_val_t primitive_add2(lisp_val_t a, lisp_val_t b) {
    GC_PROTECT(a);
    GC_PROTECT(b);
    lisp_val_t args = os_make_cons(a, os_make_cons(b, nil));  // ここでconsしている
    return primitive_add(args, global_environment);
}
```

**呼び出しサイト側のconsリスト構築(ABI-M3で除去)は無くなったが、呼び出し先
であるこの`_2`関数自体が内部で全く同じconsを構築し、汎用のn項版へ委譲する
だけだった。** つまり`+`/`-`/`=`/`<`/`>`/`>=`の6個については、ABI-M3以前と
以降でヒープ確保量に**正味の変化が無く**、むしろ関数呼び出しが1段増えて
いた(このためGC発生回数がmain/feature/abi-redesignでほぼ同じだった)。
`car`/`cdr`/`cons`/`eq`/`null`/`set-car`/`set-cdr`/型述語/ABI検証で追加した
`elt`/`set-elt`/`read-byte`は直接実装(cons非経由)になっており、この問題は
無かった。

**修正**: 比較演算子4つ(`num_equal2`/`less_than2`/`greater_than2`/
`greater_equal2`)は、元々2値を直接取りconsを構築しない`number_compare`
static関数へ直接委譲するだけで済むため差し替えた。`add2`/`subtract2`は、
両オペランドが非負FIXNUMで結果がオーバーフロー/アンダーフローしない場合
(=n項版`primitive_add`/`primitive_subtract`自身のfast-path条件と全く同じ)
にはconsを一切構築しない高速pathを追加し、それ以外の稀なケース(負数・
float・bignum昇格)のみ従来通りconsチェーン経由でn項版へフォールバックする
よう修正した。

**検証結果**: ネイティブ実行(N=2,000,000)でGC発生回数が0になり(修正前は
heap=64MBで5回)、実行時間が約3倍改善した(heap=64MB: 0.344秒→0.116秒、
heap=1024MB: 0.395秒→0.127秒)。QEMU実機(N=200,000、`%%bench-add-loop`)でも
136 ticks(1.36秒)となり、main(336 ticks、3.36秒)比で**約60%高速化**、
修正前のfeature/abi-redesign(372 ticks、3.72秒)比では**約63%高速化**を
確認した。`make test`(8135)/`make test-qemu`(default、1589)/za_test(220)
いずれも0failedで振る舞い不変を確認済み。

**教訓**: 「呼び出しサイトでconsリストを組まなくなった」ことと「実際に
ヒープ確保が減った」ことは別物であり、呼び出し先の実装まで追わないと
誤った結論に至る。今回はABI-M3の生成コード確認(`primitive_add(os_make_cons
(...), env)`→`primitive_add2(a, b)`という呼び出し表記の変化)だけを見て
「cons削減が達成された」と誤認していた。今後同種の固定引数ラッパーを追加・
検証する際は、ラッパー自身の実装本体まで確認し、必要なら`os_gc_collect_count`
のようなランタイム計測で実際のヒープ確保量を直接確認すべきである。

### 他のbuilt-in関数への横展開監査(2026-09-10)

`primitive_add2`等で見つかった「呼び出しサイトのcons構築は消えたが、呼び出し先の
`_N`関数自身が内部でconsを組んで汎用n項版へ委譲するだけ」というwrap-delegate
アンチパターンが、他の固定引数ラッパーにも潜んでいないか横展開で監査した。
対象は`*primitive-fixed-arity-c-names*`(AOT用テーブル)の全エントリと、za.cが
`(void *)primitive_xxx`/`(void *)cc_xxx`で直接ポインタ参照している全関数
(za.c自身のJIT高速パス、AOTテーブルとは独立)の両方。

**監査結果**:
- AOTテーブルの型述語9個(`bignump`/`floatp`/`symbolp`/`characterp`/`stringp`/
  `streamp`/`numberp`/`fixnump`/`consp`/`functionp`)、`eq2`/`null1`/`set-car2`/
  `set-cdr2`、`elt2`/`set-elt3`/`cc_read_byte1`は全て直接タグ/マジックナンバー
  判定またはconsセル直接ミューテーションのみで、cons確保なし。問題なし。
- za.c専用(AOTテーブル未登録)の`primitive_atom1`(`atom`用)/`primitive_listp1`
  (`listp`用)も、`(val & TAG_MASK) == TAG_CONS`の直接判定のみ。問題なし。
- **`primitive_multiply2`(`*`用)と`primitive_less_equal2`(`<=`用)に、
  `add2`/`subtract2`と全く同じwrap-delegateバグを発見**。両方ともza.c専用で
  AOTテーブルには未登録(=AOT側の`*`/`<=`はABI-M3当時から今日まで、そもそも
  固定引数化されておらず常に汎用n項版へconsリスト経由で渡っていた。これは
  wrap-delegateバグとは別種の「ABI-M3の対象漏れ」であり、今回は対象外として
  記録のみ行う)。

**修正**: `less_equal2`は比較演算子4つと同型で、`number_compare(a, b) <= 0`へ
直接差し替え。`multiply2`は`add2`/`subtract2`と同型で、両オペランドが非負
FIXNUMかつ積がオーバーフローしない場合(=`primitive_multiply`自身のfast-path
条件、`FIXNUM_MAGNITUDE_MASK / mag_a`によるオーバーフロー事前判定)のみconsを
一切構築せず、それ以外(負数・float・bignum昇格)は従来通りconsチェーン経由で
n項版へフォールバックする。

**検証**: `make test`(8135)/`make test-qemu-milestone`(za_test、220)/
`make test-qemu`(default、1589)いずれも0failedで振る舞い不変を確認済み。

**教訓**: 今回の監査で「AOTテーブルに載っている=ABI-M3で検証済み」と誤解せず、
za.c側にしか登録されていない関数も同じ手口で漏れなく洗い出したことで発見
できた。片方の呼び出し経路(AOT/JIT)だけを見ていると、もう片方専用の同種
バグを見落とす。

## 次のアクション

1. ~~本ドキュメントの調査事項1〜4を実施し、設計メモとして`documents/abi-redesign.md`
   にまとめる。~~ **完了(上記「調査結果と設計確定」節)。**
2. ~~調査結果を踏まえ、`documents/transpiler.md`のマイルストン計画との関係を
   整理した上で、このABI刷新を何回かのマイルストンに分割する計画を別途起こす。~~
   **完了(上記「マイルストンロードマップ」表、`transpiler.md`とは独立した
   `ABI-Mn`系統として管理)。**
3. ~~分割後の最初のマイルストン(ABI-M1)から実装に着手する。~~ **完了(上記
   「ABI-M1詳細」節。NOT/CONSP/LISTPをza_compile_unary経由の固定引数直接呼び出しへ
   追加し、`make test`/`make test-qemu-milestone`/`make test-qemu`全てで0failedを
   確認済み)。**
4. ~~ABI-M0(ベンチマーク計測基盤の整備)に着手する。~~ **完了(上記「ABI-M0詳細」
   節。`%%GC-COLLECT-COUNT`新設+`test/lisp/abi_bench.lisp`。usercallがbaseline比
   +16byte/call、ABI-M5がこれを解消すべき目標値として記録済み)。**
5. ~~ABI-M2(未対応プリミティブへの固定引数ラッパー拡充)に着手する。~~ **完了(上記
   「ABI-M2詳細」節。型述語9個+SET-CAR/SET-CDRの計11個を追加、`make test`
   /`make test-qemu-milestone`/`make test-qemu`全てで0failedを確認済み)。**
6. ~~ABI-M3(AOT/transpile.lisp側でのネイティブプリミティブ呼び出しのcons回避)に
   着手する。~~ **完了(上記「ABI-M3詳細」節。`*primitive-fixed-arity-c-names*`
   新設+`transpile-call`改修。`make test`/`make test-qemu`/IDE・FAT16・FAT32
   マイルストン全てで0failedを確認済み。`qemu_boot_fat32_primary_boot.lisp`は
   ABI-M3以前から本セッションのKVM無し環境では完走できないことを確認済みで
   回帰ではない)。**
7. ~~ABI-M4(関数オブジェクトのdual-entryレイアウト導入)に着手する。~~ **完了
   (上記「ABI-M4詳細」節。`za_fn_meta_t`+`os_fn_meta_alloc`新設、3コンストラクタ/
   `apply_function`/`za_ensure_trampoline`を改修。全検証(make test/test-qemu/
   IDE/FAT16/FAT32)がABI-M3と完全に同じアサーション数・0failedで一致し、
   「振る舞い不変」を確認済み)。**
8. ~~ABI-M5(JIT-to-JITユーザー定義関数の静的呼び出し新ABI化、非末尾)に着手する。~~
   **完了(上記「ABI-M5詳細」節。za.c側の非末尾呼び出しのみ、パラメータスロット
   方式のdual-entry+実行時4条件判定を実装。je/jne反転バグとconsリスト構築が
   実行時分岐より前に無条件実行されていたバグの2つを発見・修正。
   `abi_bench.lisp`でusercallのbytes/callがbaseline+16→baselineと完全一致に
   改善したことを実測確認済み)。**
9. ~~ABI-M6(AOT-to-AOTユーザー定義関数の静的呼び出し新ABI化、非末尾)に着手する。~~
   **完了(上記「ABI-M6詳細」節。transpile.lisp側に`name__fixed`/`__step_fixed`
   のdual-entryを追加し、`transpile-call`がarity一致時に直接callするよう改修。
   AOTはリンク時確定の直接呼び出しのためza.cのABI-M5と異なり実行時判定が
   不要と判明し、コンパイル時判定のみのシンプルな実装で済んだ。生成コードで
   `append2`の自己呼び出しがconsリスト構築無しの直接callに変わったことを
   確認、`make test`/`make test-qemu`/IDE/FAT16/FAT32全てで0failedを確認済み)。**
10. ~~ABI-M7(JIT-to-JITユーザー定義関数の静的呼び出し新ABI化、末尾)に着手する。~~
    **完了(上記「ABI-M7詳細」節。za.cの6b(末尾)分岐に6a(ABI-M5)と同一の
    4条件判定を追加し、成功時はconsリスト構築も共有トランポリンも経由せず
    fixed_entryへ直接tail-jmpするよう改修。当初argc<=2の register制約を
    想定していたが、6aと同じ「呼び出しサイト自身が判定しcellをレジスタに
    残さない」設計により制約が実在しないと判明しargc<=3(6aと同じ上限)に
    拡張。ネイティブプローブでの実行検証はMS x64/SysV ABI不一致という
    プローブ環境固有の限界に突き当たり断念し、実機ABIが一貫するQEMU実行
    (`sum-iter(200000, 0)`等の既存の深い自己/相互末尾再帰テスト)で
    真のTCOと計算結果の正しさを実証。`make test`/`make test-qemu`/za_test/
    IDE/FAT16/FAT32全てで0failedを確認済み)。**
11. ~~ABI-M8(AOT-to-AOTユーザー定義関数の静的呼び出し新ABI化、末尾)に着手する。~~
    **完了(上記「ABI-M8詳細」節。`tco_result_t`にfixed_fn/fixed_argc/arg0..2を
    追加しis_tail_call=2(固定引数ABI継続)を新設、`transpile-tail-call`が
    arity一致時にconsチェーンを組み立てず直接ディスパッチするよう改修。
    実装当初`__step_fixed`をstaticのままにしていたためリンクエラーが多発し、
    `__step`と同じ理由で非staticにして解決。za.c(ABI-M5/M7)・AOT(ABI-M6/M8)
    双方で非末尾・末尾とも新ABI化が完了した。`make test`(トランポリン駆動
    ループのswitch/case分岐を実地で検証)/`make test-qemu`/za_test/IDE/FAT16/
    FAT32全てで0failedを確認済み)。**
12. `&rest`ハイブリッド化(固定引数+`&rest`混在関数の固定部分だけ高速化する等)は、
    当初のロードマップ通りABI-M9以降として個別に詳細設計・実装する。
    インタプリタ本体(eval.c)のconsリストABIは引き続き当面スコープ外とする。

## za.c: JIT生成コードのGC保護コスト削減(3段階計画、Phase1実施・結果を踏まえ再検討中、2026-09-10)

上記のprimitive_add2等の修正でAOTは大幅に高速化したが、JITは約11%の改善に
留まった件について、`za_compile_fold`/`za_compile_binary`(`+`/`-`/`*`/比較演算子/
`eq`/`cons`/`set-car`/`set-cdr`)が二項演算1回につき「アキュムレータをGCルートへ
link(movabs+call)→wrapper_fnへ間接call→unlink(movabs+call)」という3点セットを
必ず発行しており、`%%bench-add-loop-jit`のようなループでは1イテレーションあたり
10回以上の間接callが発生していることが判明した。これに対しPhase1〜3の3段階
対策を計画し、Phase1を実施した。

### Phase1: leaf判定によるlink/unlink省略(実施・コミット済み)

`za_compile_fold`/`za_compile_binary`の2個目以降の全オペランドが
`za_operand_is_safe_leaf`(fixnum/charリテラル、または非box化local/paramスロットの
直接参照、いずれもCALLを一切伴わない)と判定できる場合、アキュムレータのGCルート
link/unlinkを丸ごと省略するようにした。`wrapper_fn`(`primitive_add2`等)自身が
自分の引数を`GC_PROTECT`してから確保する実装になっているため、za.c側の外部保護は
「後続オペランド評価中の生存」を守るためだけに必要であり、後続オペランドの評価が
一切の呼び出しを含まなければ不要になる、という考察に基づく。当初案(「+/-/*/=等の
プリミティブ名で対象を決める」)は、2個目以降のオペランドが非leaf(任意の式)の
場合に不安全(evaluation中のGCでオペランド0の値が古いアドレスのまま使われる)と
判明したため、「オペランドの構文的形が呼び出しを伴わないか」を基準に修正した。

安全性は、bignum昇格を伴う加算/減算をGCが複数回発火する条件下(2^60のbignumを
アキュムレータに繰り返し加減算)で実行し、multiplyによる独立した経路で計算した
期待値とcrossチェックするテスト(za_test.lisp、ckpt-31)で検証した。

**計測結果(N=200000、`%%bench-add-loop-jit`)**: 診断カウンタでza_gc_link呼び出し
回数を直接計測したところ、Phase1適用前9回/iteration→適用後6回/iteration(-33%、
3個の算術演算子分のlinkが正しく消えたことを確認)。しかし壁時計時間は2416 ticks→
2360 ticks程度と、約2.3%の改善に留まった。

**結論**: GCルート保護のためのlink/unlink呼び出し自体は、JIT/AOT速度差の主要因
ではないことが判明した。呼び出し回数を1/3近く減らしても体感速度がほぼ変わらない
ということは、コスト予測(「間接callがTCG環境で不釣り合いに高コスト」という仮説)
は過大評価であったか、link/unlink以外の要素(呼び出し規約の分岐判定・トランポリン
そのもの・命令数そのもの等)がより支配的であることを示唆する。

### Phase2: +/-のfixnum高速pathインライン化(実施・コミット済み)

Phase1の結果を踏まえてもなお、「wrapper_fn呼び出し自体(primitive_add2/
primitive_subtract2への間接call)を除去すればより大きな改善が見込める」という
仮説は依然検証する価値があると判断し、Phase2として実施した。`za_compile_fold`が
生成する`+`/`-`の二項演算について、両オペランドが非負fixnumかつ結果がオーバー
フローしない場合は、`primitive_add2`/`primitive_subtract2`それぞれのC実装が持つ
fixnum高速pathと完全に同一の条件・結果になるインラインアセンブリ(タグ+符号bit
判定→加減算→オーバーフロー検出)に置き換え、条件を満たさない場合(オーバーフロー・
負数・非fixnum)のみ従来通りwrapper_fnへの間接callにフォールバックするようにした。
`primitive_multiply2`はオーバーフロー判定に除算を要し単純なインライン化が困難な
ため対象外とした。正当性はfixnum境界値・オーバーフロー・負数オペランドを網羅した
テスト(za_test.lisp、ckpt-32)で検証済み。

**計測結果(N=200000、`%%bench-add-loop-jit`)**: 診断カウンタで直接計測したところ、
インライン高速pathが100%(400000/400000回、フォールバック0回)使われている
ことを確認した。つまり`+`/`-`の間接callは完全にゼロになった。にもかかわらず
壁時計時間は約2340 ticksと、Phase1適用後(約2360 ticks)からほぼ変化しなかった。

**結論(重要)**: Phase1(GCルート保護のためのcall削減、-33%)・Phase2(算術
wrapper呼び出し自体の除去、-100%)という、性質の異なる2種類の間接call削減を
それぞれ独立に実施・検証したが、いずれも壁時計時間の体感速度にはほぼ寄与
しなかった。これは「間接callの実行コストがJIT/AOT速度差の主要因である」という
当初の作業仮説(documents/abi-redesign.md冒頭のfutex観測に基づく仮説)そのものが
誤りであったことを強く示唆する。JIT/AOT速度差の真因は、間接call/直接callの違い
ではなく、以下のような別の要素にあると考えられる:
- za.cの値の保持方式(スタックスロットへの都度read/write、`za_load_slot`/
  `za_store_slot`)がAOT(GCCが最適化したレジスタ常駐)より本質的に命令数が多い。
- 自己末尾再帰1回あたりのトランポリン/4条件判定(ABI-M5/M7)自体のオーバーヘッド
  (fn取得・magic判定・arity判定・fixed_entry判定を都度行う)が支配的で、
  中身の算術演算の実装方法(call有無)は誤差の範囲でしかない。
- QEMU TCGの命令実行コストは「call/jmpかどうか」ではなく素の命令数に概ね比例し、
  間接call1回のコストは(za_gc_link/za_gc_unlink/primitive_add2のようなごく
  短い関数に対しては)想定したほど高くない。

**Phase3(JITフレーム単位でのGCルート一括管理)は、Phase1と同種の「call削減」
施策であり、Phase1・Phase2の結果からは効果が見込めない可能性が高い。** 着手前に
一度立ち止まり、実際のボトルネックを特定するための別のプロファイリング手法
(トランポリン自体を単独で計測する、za_load_slot/za_store_slotの回数を数える等)
を検討することを推奨する。

### トランポリン単独計測による仮説の直接検証(2026-09-10)

Phase1・Phase2がいずれも効果薄だった原因として、「自己末尾呼び出し(ABI-M7高速
path)自体のオーバーヘッド(fn/引数のlink3回+高速path内のunlink2回、および
4条件判定)が支配的で、算術演算の実装方法(call有無)は誤差でしかない」という
仮説を、実装前にまず最小構成で検証した。

**方法**: 算術演算を一切含まない自己末尾再帰ループ(`car`/`cdr`/`null`によるリスト
走査。いずれも`za_compile_unary`経由でlink/unlinkが元々ゼロの単項呼び出し)を
用意し、`%%bench-add-loop-jit`(Phase1+2適用後、`=`が1回のnumber_compare呼び出し
+`-`/`+`は完全インライン化済み)と同一N=200000で比較した。

```lisp
(defun %%bench-loop-only (lst)
  (if (null lst)
      0
      (%%bench-loop-only (cdr lst))))
```

**計測結果**: `%%bench-loop-only`(算術ゼロ)が2288 ticks、`%%bench-add-loop-jit`
(Phase1+2適用後)が2344 ticksと、**差はわずか2.4%**だった。

**結論**: 仮説は正しかった。自己末尾呼び出し自体のトランポリン/4条件判定
(fn/引数のfetch・MAGIC_FUNCTION_NATIVE判定・arity判定・fixed_entry判定・
GCルートのlink/unlink)が、ループ1回あたりのコストの大半(97%超)を占めており、
ループ本体の算術演算の実装方法(consベースのwrapper呼び出しか、Phase1のGC保護
削減か、Phase2のインライン化か)は、全体から見れば誤差の範囲でしかなかった。
これでPhase1・Phase2それぞれの「効果薄」という結果に、直接的な説明がついた。

**今後の方針**: Phase3(GCルート一括管理によるさらなるcall削減)は、算術演算側の
call削減と同じ層に手を入れる施策であり、今回の検証結果からは大きな効果は
期待できないため、**現時点では見送る**。JIT/AOT速度差を本質的に縮めるには、
自己末尾呼び出しのトランポリン自体(4条件判定・fn/引数の毎回のfetch・
GCルートのlink/unlink)を軽量化する、より踏み込んだ再設計が必要になる
(例: 関数再定義が起きていないことが分かっている場合に4条件判定自体を
省略する仕組み、fn/引数を毎回linkし直さずレジスタに保持したまま次の
イテレーションへ持ち越す設計等)。これは当初のABI-M0〜M8および本ドキュメントの
Phase1〜3計画のいずれとも異なる、新規のマイルストンとして別途詳細設計すべき
規模の変更であり、本セッションのスコープはここで一区切りとする。

### トランポリン内訳の追加切り分け(2026-09-10、決定的な結果)

上記の結論を受け、「トランポリン軽量化」の設計に着手する前に、トランポリンを
構成する要素(fn/引数のfetch、MAGIC判定、arity判定、fixed_entry判定、GCルートの
link/unlink)のうちどれが支配的かを、`%%bench-loop-only`(2308 ticks、N=200000)を
ベースラインに、各要素を個別に無効化・ショートカットした診断ビルドで直接計測した。

| 要素 | 診断ビルドのtick数 | ベースラインからの削減率 |
|---|---|---|
| ベースライン(`%%bench-loop-only`) | 2308 | - |
| GCルートlink/unlink無効化(fn/引数のlink3回・6b内のunlink2回を除去) | 2268 | -1.7% |
| MAGIC判定・arity判定・fixed_entry判定ショートカット(4条件のjcc全撤去) | 2288 | -0.9% |
| **fn/引数fetchのキャッシュ化(コンパイルサイトごとの専用スロットに初回だけ`os_make_symbol`+`os_get_function_cell`を実行し、以降は再利用)** | **44** | **-98.1%** |

**結論**: 自己末尾呼び出しのたびに行われる`os_make_symbol`(シンボル名文字列から
シンボルオブジェクトへのinterning再解決)+`os_get_function_cell`(envの親チェーンを
辿ってFunction Cellを検索)という、fn解決のための2回の間接call**だけ**が、
トランポリン全体のコストの98%超を占めていた。GCルート保護(link/unlink)も
4条件判定も、合計しても3%に満たない誤差の範囲だった。これはPhase1・Phase2で
検証した「算術演算部分のcall削減が効果薄だった」という結果とも整合する
(算術演算のcall削減は、そもそも支配的でないトランポリンの、さらに支配的でない
部分にすぎなかった)。

診断は`za_compile_call`のfn解決コード(コンパイルサイトごとに専用の静的スロットを
割り当て、初回呼び出し時にのみ解決結果をキャッシュする簡易メモ化)で実施し、
`make test-qemu`(1607、0failed)で他への悪影響が無いことも確認した(このキャッシュ
機構自体は関数再定義に対応していない診断専用コードのため、正式な実装では
別途対応が必要)。

**重要な技術的示唆**: 現在の実装は、自己末尾再帰であっても「呼び出し先が
自分自身である」という事実を一切利用せず、毎回シンボル名からの再解決を行っている
(flet/labels束縛関数はgensymスロットのアドレスをキャッシュする専用パスを持つが、
top-levelのdefunによる自己再帰にはこの最適化が及んでいない)。この非対称性自体が
今回発見した支配的コストの直接の原因であり、次の新マイルストンが最優先で
取り組むべき対象である。

**次の新マイルストンへの提言**: 「トランポリン軽量化」ではなく、より的を絞った
**「fn解決結果のキャッシュ化」**を新マイルストンの中心に据えるべきである。設計時の
論点:
- 関数再定義(`defun`の再実行によるFunction Cellの中身の変化)が起きた場合に、
  キャッシュされた古い解決結果を使い続けてしまう不整合をどう防ぐか(例:
  Function Cellのアドレス自体は再定義後も不変であるため、キャッシュするのは
  「シンボル→Function Cellアドレス」の対応のみに限定し、Cellの中身(現在の関数
  オブジェクト)は毎回読み直す設計にすれば、再定義に対して安全になる可能性が
  高い。今回の診断コードは「Cellの中身」まで丸ごとキャッシュしていた点で
  正式な実装より踏み込みすぎている)。
- flet/labels束縛関数が既に持つ「gensymスロットのアドレスをコンパイル時に
  埋め込む」方式を、top-levelのdefun(グローバルシンボル)にも拡張できないか
  (シンボルオブジェクト自体はGCで再配置されうるため、シンボルの名前ではなく
  Function Cellのアドレス、またはシンボルオブジェクトへの安定した参照を
  コンパイル時に取得・埋め込める仕組みが必要)。
- 自己末尾再帰以外の一般の関数呼び出し(6a、非末尾)にも同じ問題があるため、
  対象は自己末尾再帰に限定せず、za_compile_call全体のfn解決ロジックを見直す
  べきである。

## fn解決コストの追加切り分けと「fn解決結果のキャッシュ化」実装(2026-09-10、完了)

上記の提言を受け、`os_make_symbol`(シンボル解決、全シンボルの線形走査+
文字列比較)と`os_get_function_cell`(Cell解決、envのalist探索、EQ比較)の
どちらが98.1%削減の中でより支配的かを個別に切り分けた上で、正式な実装
(fn解決結果のキャッシュ化)を行った。

### 個別切り分け計測

`%%bench-loop-only`(ベースライン2308 ticks)に対し、以下の2パターンを
それぞれ計測した。

| パターン | tick数 | 削減率 |
|---|---|---|
| ベースライン | 2308 | - |
| A: シンボル解決のみキャッシュ(Cell解決は毎回) | 96 | -95.8% |
| B: Cell解決のみキャッシュ(シンボル解決は毎回) | 2256 | -2.3% |

`os_make_symbol`の線形走査(g_symbol_table、全シンボルに対して文字列比較)
が支配的コストであり、`os_get_function_cell`(alistのEQ比較走査)の寄与は
誤差レベルだったことが確定した。

### 実装: fn解決結果(Function Cellアドレス)のキャッシュ化

`za_compile_call`の一般呼び出し(flet/labels以外、シンボル名によるグローバル
関数呼び出し)について、コンパイルサイトごとに専用スロット(`g_za_fn_cell_
cache_slots`、他の3リテラルスロットプールと同型、GC追跡は不要)を1個割り
当て、初回実行時だけ`os_make_symbol`+`os_get_function_cell`で解決した
Function Cellのアドレスを格納し、2回目以降はスロットの値をそのまま再利用
する(シンボル名からの再解決を省略)。プール枯渇時はそのcall1箇所のみ
キャッシュを諦めて従来通り毎回解決する(コンパイル自体は失敗させない)。

**安全性の根拠**(実装前に確認・検証済み):
- Function Cellは`os_imm_slot_alloc`でImmobilized Spaceに確保される
  非移動アドレスであり(`os_set_function`参照)、GCによる再配置を追跡する
  必要がない。
- 関数再定義(`defun`の再実行)時、`os_set_function`は既存セルの**アドレスは
  維持したまま中身だけを書き換える**(`*(lisp_val_t *)cell_addr = fn_obj;`)。
  キャッシュしているのはセルの**アドレス**であり、セルの**中身**は呼び出しの
  たびに既存コードが毎回デリファレンスするため、再定義後も新しい定義が
  正しく呼ばれる。
- 関数再定義への安全性(REPL上でコードを汚しながら試行錯誤する開発体験の
  根幹)とGC整合性(bignum確保でGCを複数回誘発する条件下)の両方を、
  za_test.lispへの回帰テスト(ckpt-33)で直接検証した。

**実装中に踏んだ重大なバグ(教訓として記録)**: 未キャッシュを表すセンチネル
値として`nil`を使ったところ、全JIT関数がコンパイルされているのに何を
呼んでもNILを返すという壊滅的な回帰(za_test.lisp 33passed/205failed)を
引き起こした。原因は、`nil`が実際には`g_nil_cell`のアドレス`|TAG_CONS`という
**非ゼロの実行時値**(`os_bootstrap`で設定)であり、生成コード側の
「非ゼロならキャッシュ済み」というTEST+JNE判定が常に真になってしまい、
一度も実際の解決が行われないままnilをFunction Cellアドレスとして誤用して
いたため。GCルート保護の無効化・条件判定の無効化・プールサイズの変更・
関数分割によるスタックフレーム縮小など、あらゆる周辺要因を1つずつ切り分けて
ようやく特定した。修正はセンチネルを生の0に変更するだけで済んだが、
「`nil`は`C`の`0`ではない」という、このコードベースの最も基本的でありながら
見落としやすい前提を再確認する結果となった。

### 最終計測結果

N=200000での比較(diagnostic計装なしの本実装):

| ベンチマーク | Phase1+2適用後 | fn解決キャッシュ化後 | 削減率 |
|---|---|---|---|
| `%%bench-loop-only`(算術なし) | 2308 ticks | 60 ticks | -97.4% |
| `%%bench-add-loop-jit`(算術込み) | 2340〜2360 ticks | 96 ticks | -95.9% |

Phase1(GCルート保護削減、-2.3%)・Phase2(算術wrapper呼び出し除去、
効果なし)を大きく上回る、これまでの一連の調査で最も支配的だった真因への
直接的な対処となった。`make test`(8135)/`test-qemu-milestone`(za_test、
245)/`test-qemu`(default、1614)いずれも0failedで確認済み。

### 残課題(将来のマイルストン候補)

- `za_compile_call`の6a(非末尾呼び出し)は6bと同じfn解決コードパスを共有
  しているため今回の修正で同時に恩恵を受けているはずだが、6a単体の
  ベンチマーク(非末尾の一般呼び出しが主体のコード)での効果は未計測。
- `flet`/`labels`束縛関数(`fn_binding`が非NULLの分岐)は今回の対象外
  (既存のgensymスロット方式が既に「毎回os_get_function_cellのみ、
  シンボル解決は不要」という準最適な状態になっており、今回発見した
  ボトルネック(シンボル解決の線形走査)を元々持たない)。
- プールサイズ(`ZA_MAX_FN_CELL_CACHE_SLOTS=2048`)を超えるコンパイルサイト
  数になった場合の実地での挙動(自然にキャッシュなしへ縮退するのみで
  破綻はしないはずだが、実運用規模での枯渇有無は未確認)。


## FAT16/FAT32読み込みが遅い原因(2026-09-10 調査)

結論: IDE PIOでもFATレイヤのアルゴリズムでもなく、**AOT生成コードが実行時に
`os_make_symbol`(シンボルテーブルの線形走査)をホットループ内で呼んでいる**こと。
JITの`za_compile_call`で見つけたのと同じ「シンボル解決の線形走査」がAOT側にも
別の形で存在している。main/feature共通(両ブランチでネイティブ計測結果が一致)。

### 計測(QEMU TCG、ticks=10ms、feature/abi-redesign、diag worktree)
| 計測 | 結果 | 換算 |
|---|---|---|
| `%%ide-read-sector`(C PIO)×200 | 4 ticks | **0.2ms/セクタ** |
| `read-sector`(=PIO+`%ide-bytes-from-addr` 512反復のAOT while)×200 | 1904 ticks | 95ms/セクタ |
| `fat16-fat-entry`×200(FATキャッシュヒット) | 92 ticks | 4.6ms/回 |
| `%fat16-read-lba-list` 8セクタ | 140 ticks | 175ms/セクタ |
| `read-file-into-vector` PERFBIG.TXT(100KB, FAT16) | 5632 ticks | 563µs/byte |
| `(intern "%WHILE-LOOP")`×5120(JITループ内) | 84 ticks | **164µs/回** |
| AOT while 512反復(`%ide-bytes-from-addr`)×10 | 124 ticks | **242µs/反復** |
| JIT while 同一本体(`%%peek`+`set-elt`)×10 | 12 ticks | 23µs/反復 |
| JIT while(`set-elt`のみ)×10 | 8 ticks | 16µs/反復 |

### ネイティブプローブ(tmp/bench_fat16_probe.c、`-Wl,--wrap=os_make_symbol`で回数計測)
- `read-sector` 1回 = `os_make_symbol` **1031回**(=512反復×2+7)、3.1ms/セクタ(ネイティブ)
  → 1呼び出し≈3µs(ネイティブ)。QEMU上では≈150µs。
- main/featureとも同一回数・同一時間(ブランチ差なし)。
- lisp_compiled.c中の`os_make_symbol("...")`サイト数: 4467(ユニーク名969)。
  上位: BPB 305 / BYTES 212 / DEVICE 200+140+79 / %WHILE-LOOP 192 / PATH 62。

### 発生源(transpile.lisp)
1. `transpile-go`: `os_make_instance(MAGIC_GO_EXIT, os_make_symbol("%WHILE-LOOP"), nil, nil)`
   を毎反復生成(ヒープ確保+線形走査)。
2. `%%tagbody-dispatch-chain`: `os_control_transfer_name(sig) == os_make_symbol("%WHILE-LOOP")`
   の比較でもう1回線形走査。→ while/for 1反復あたり2回。
3. `transpile-quoted`のシンボル分岐: `'size`等のスロット名・キーワード・`dynamic`の
   変数名など、全quoteシンボルリテラルが評価のたびに`os_make_symbol`。
4. クロージャリフティング(1502/1510行付近): 捕捉変数ごとに`os_make_symbol("BPB")`等で
   環境alistを構築(クロージャ生成のたび)。`os_get_variable(os_make_symbol(..), env)`も同様。

9P読み込み(`read-file-into-vector`自身の1byteループ)が2.03MBで340秒かかるのも同じ原因
(2回/byte×~85µs)。IDE PIOはFAT16読み込み時間の0.1%未満。

### 修正案(未着手)
- B': `os_make_symbol`をハッシュ索引化(大文字小文字を畳み込んだハッシュ→
  g_symbol_table添字)。reader/eval/JIT/AOT全経路に効く。最小リスク。
- A: `go`の飛び先が同一C関数内のtagbodyに字句的に含まれる場合は直接`goto`
  (シグナル生成・シンボル解決・比較を全廃)。while/forのホットパス。
- B: AOTのquoteシンボルリテラルを静的スロットにキャッシュ(za.cの
  `g_za_quote_slots`と同じ、GCルート登録は1配列で1回。番兵は0、pitfalls.md参照)。
  `GC_MAX_EXTRA_ROOTS=160`はサイト単位登録には足りない。

## perf_fat16_test.lisp/perf_fat32_test.lispの9P依存除去 + 未解決の書き込み遅延調査(2026-09-10)

### 実施したこと
- テストデータを9P経由の実カーネルバイナリ読み込みから、JITコンパイルされる
  whileループでのその場生成(`%%perf-make-fill-vector`、2,000,000byte)へ変更。
  9Pはベアメタル実機では使われない暫定経路であり、その遅さがテストの完走
  時間・合否に影響すべきでないため。
- 生成ループが当初`(mod i 256)`を使っていたが、`mod`はza.cのfixnum高速path
  対象外(+/-のみインライン化)で、2,000,000反復のコストの大半を占めていた
  (57.44秒)。`(if (>= b 255) 0 (+ b 1))`という、インライン化済みの`+`と
  ラッパー呼び出し版`>=`だけで組む方式に変更し23.32秒へ短縮。

### mainブランチとの直接比較(検証済み)
同じ(9P非依存・mod非依存)テストファイルをmainブランチのworktreeにコピーして
実行し、featureブランチと比較した:
- `#39`(write-char 1文字ずつ、100KB): feature 31秒 / main **244秒**(約8倍)
- `#41`のfill(JIT、2MB): feature 23秒 / main **9分以上経過しても未完了**
  (20倍以上)

mainにはAOT改善B'/A/B(本ドキュメントの他セクション参照)が無いことに加え、
JIT側の関数解決キャッシュ化(前回セッションの成果、documents/abi-redesign.md
の別セクション参照)も無いため、AOT/JIT両方の未最適化コストを負っている。
この比較により、B'/A/Bの効果が実環境で確実に効いていることを確認した。

### 未解決: #39の後にfat16-create-fileの書き込みが単独計測より大幅に遅い
`perf_fat16_test.lisp`をチェックポイント計測すると:
- `#41`単独(#39を経由しない、fillのみ先行): fill 23.32秒、write(fat16-
  create-file、2MB)67-69秒、read(read-file-into-vector、2MB)60-62秒、
  equal 0.16秒。合計約152秒(2回の独立測定で再現性あり)。
- `#39`(100KB write-char+読み戻し、31秒)の直後に同じ`#41`を実行すると、
  fillは23秒程度で正常に終わるが、その後のwrite(fat16-create-file)が
  数分(最低でも8分以上、4回の試行で毎回未完了のままタイムアウト)かかる。

`fat16.lisp`/`fat32.lisp`はmain/feature間で差分ゼロ(本セッションでは一切
変更していない)ため、この現象はAOT改善B'/A/Bとは無関係にfat16.lispの
既存の性質と考えられる。原因の仮説(いずれも未検証):
- `#39`のPERFBIG.TXT(100,000要素のvector、`defglobal`で永続的にGCルートとして
  生存し続ける)が、後続の2,000,000要素vector確保時のGCコストを増加させている。
- `%fat16-write-file-data`が`%fat16-allocate-clusters`で確保したクラスタ
  リストを再利用せず、`fat16-cluster-chain`でFATを再度辿り直しており、
  ディスク上に既に別ファイル(PERFBIG.TXT)が存在する状態でのFATキャッシュ
  (`*fat16-fat-cache-*`系dynamic var)の挙動が影響している可能性。
- 単なるTCG/ホスト側のタイミング分散ではない(4回連続で同じパターンが
  再現しているため)。

次のアクション(未着手): `%fat16-write-file-data`/`%fat16-allocate-clusters`
に処理内容ごとのtick計測を仕込み、#39の有無で分岐する箇所を特定する。

## read-file-into-vector単体のサイズ別スケーリング計測(2026-09-10)

`fat16-create-file`/`fat32-create-file`によるゲスト内書き込みを介さず、ホスト側で
`mkfs.vfat`+loopマウントしてファイルを事前配置したディスクイメージを使い、
`read-file-into-vector`だけを計測した(書き込み側の未解明の遅延、後述の別節参照、
の影響を受けないようにするため)。

| サイズ | FAT16 | FAT32 |
|---|---|---|
| 100KB | 3.20秒(32.00秒/MB) | 3.88秒(38.80秒/MB) |
| 1MB | 31.28秒(31.28秒/MB) | 33.32秒(33.32秒/MB) |
| 2MB | 62.16秒(31.08秒/MB) | 66.60秒(33.30秒/MB) |

100KB→2MB(20倍)で所要時間もほぼ20倍(FAT16: 19.4倍、FAT32: 17.2倍)であり、
**ファイルサイズにほぼ線形**に振る舞う(B'/A/B適用後、超線形な悪化=#41本文が
懸念していたGCコストの悪化は再現していない)。FAT16/FAT32の差は約1.08〜1.21倍
と小さい(読み込みは両者ともread-sector単位のI/Oが支配的で、書き込みほど
クラスタサイズ差の影響を受けない)。

### 副産物の発見: fat32-create-file(書き込み)の2MBが著しく遅い
上記の計測の過程で、`fat32-create-file`でゲスト内書き込みしてから読む方式
(前節の計測方法)だと、FAT32の2MBだけ9分以上かかっても完了しないことが
判明した(FAT16の2MB書き込みは67-69秒)。原因の有力な仮説:
- テストイメージのクラスタサイズがFAT16は2048byte/cluster、FAT32は
  **512byte/cluster**(Makefile FAT32_DISK_IMGのコメントに明記、意図的な設定)。
  2MBファイルはFAT16で約977クラスタ、FAT32で約3907クラスタ(4倍)必要。
- `os_ide_write_sectors`は1回の呼び出し(=1セクタ)ごとに個別のCACHE FLUSHコマンドを
  発行する(block_device.c、ide_bd_flush調べ)。write-sectorはLisp側で1セクタ単位の
  呼び出しのため、セクタ数に比例してCACHE FLUSH回数も増える。TCGエミュレーション
  下でのCACHE FLUSHは比較的重いと見られ、4倍のセクタ数が単純な4倍を超える
  (9分以上/67秒 = 8倍以上)悪化として表れている可能性が高い。
- 未検証: `%fat32-allocate-clusters`のクラスタ探索が本当にO(総クラスタ数)に
  収まっているか(FAT32ディスクの総クラスタ数はFAT16よりずっと多い、40MB/512B
  ≈78,000 vs 16MB/2048B≈7,800)。

対策案(未着手): 複数セクタをまとめて1回のPIO転送+1回のCACHE FLUSHで書く
バルク版のwrite-sectors相当をLisp側に用意する(現状write-sectorは常に1セクタ)。
