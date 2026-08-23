# isiki-os Lisp→Cトランスパイラ 実装マイルストン

このドキュメントは、`src/lisp/init.lisp`(let/and/or/cond/ILOS/条件系/Environment API
などのISLisp拡張層一式、1530行)を、実行時ロード+za JITコンパイルではなく、
初回ビルド時にCへ変換して他のCソースと同じ領域にリンクする「トランスパイラ」を
組み込んでいくためのマイルストンです。トランスパイラ自体はCommon Lisp(SBCL、
Roswellの`ros run`)で実装し、既存のDockerビルド環境内で動かします。

## 動機: なぜAOTトランスパイルが必要か

現在`init.lisp`は`cc_load()`(`load.c`)経由で実行時にロードされ、その中の`defun`は
za JIT(`src/c/za.c`)によって機械語へコンパイルされると、生成された機械語・
Function Cell・リテラルスロットが"Immobilized Space"(`g_imm_space`、4MB静的領域、
`runtime.c`/`za.c`)にピン留めされる。このImmobilized Spaceは、GCで移動しない
安定アドレスを提供する代わりに手動でのリロケーション・パッチ・回収(`destroy-environment`)
を必要とし、`documents/environment.md`に記録されている通り、self-reference movabs
パッチ・トランポリンrel32パッチ・tagbody/goのjmp衝突という3件の個別バグを
すでに生んでいる。`init.lisp`の内容を実行時JITではなく初回ビルド時にCとして
直接組み込めれば、この一連の脆弱な仕組みへの依存を段階的に減らせる。

## 前提: 既存アーキテクチャ

トランスパイラが生成するCコードは、以下の既存の制約に合わせる必要がある
(za.cが既に同じ制約の中で動いているため、za.cと同じ答えを再利用できる部分が多い)。

- **オブジェクト表現**は64bit `lisp_val_t`、下位3bitタグ: `TAG_FIXNUM`(即値)/
  `TAG_CONS`(16B: car/cdr)/`TAG_SYMBOL`(32B)/`TAG_CHAR`(即値)/`TAG_STRING`
  (ヒープ)/`TAG_INSTANCE`(32B、magicタグ付き: ネイティブ関数/インタプリタ関数/
  bignum/vector/float等)/`TAG_FORWARD`(GC内部用)/`TAG_RAW_POINTER`(即値、
  GC対象外、Immobilized Spaceアドレス等に使用)。`documents/utility.md`・
  `src/c/runtime.h`と整合させる。
- **呼び出し規約はすべてのcallableで統一されている**: `lisp_val_t fn(lisp_val_t
  evaluated_args, lisp_val_t env)`。`apply_function`(`eval.c`)は
  `MAGIC_FUNCTION_NATIVE`(手書きprimitive_*とza生成コードを区別せず直接call)と
  `MAGIC_FUNCTION_INTERPRETED`を分岐するだけで、呼び出し元からは手書きC関数・
  JIT生成コード・トランスパイラ生成コードのいずれも区別できない。トランスパイラが
  生成する`defun`相当のCコードも、この同じABIに合わせることで`apply_function`から
  区別されない自然な統合が可能になる(za.cの既存実装をそのまま参考にできる)。
- **GCはCheney方式のコピーGC**。シャドウスタックは`GC_PROTECT`マクロ(`runtime.h`)
  として既に実装済みで、プロセスごとのintrusive linked list(`process_t.gc_roots`)に
  Cのローカル変数を指す`gc_rootnode`をpushし、`__attribute__((cleanup(...)))`で
  スコープを抜ける際に自動popする。ヒープ確保を伴う操作(`cons`等)を含むコードを
  生成する場合は、この既存の仕組みに同じ形で参加する必要がある。参考実装
  (`/Users/shingo.tamura/prog/c/mini-os`)は独自のshadow-stack配列を発明しているが、
  isiki-osには既にGC_PROTECTがあるため、そちらを流用する(新規のGC連携機構は作らない)。
- **既存のビルド導線は未配線**。Makefileに`.PHONY: transpile`は宣言済みだが
  レシピが空。`init.lisp`は現在`SRC`リストに含まれておらず、完全に実行時ロードのみ。
  Dockerfileは`FROM fukamachi/sbcl`で、roswellがプリインストールされている前提
  (gcc-mingw-w64も導入済み)。
- **マクロは素のCommon Lisp互換の`defmacro`**。`and`/`or`/`cond`/`let`/`let*`/
  `case`/`for`/`while`等は、Cの特殊形式ではなくすべて`init.lisp`内の`defmacro`で
  定義されている(例: `and`/`or`は`init.lisp:44-59`)。C側インタプリタ(`eval.c`)が
  直接理解する核となる特殊形式は`quote`/`if`/`progn`/`setq`/`defun`/`lambda`/
  `defmacro`/`quasiquote`/`block`/`return-from`/`unwind-protect`(および恐らく
  `tagbody`/`go`)程度に絞られる。

## 現状: `transpile.lisp`/`init2.lisp`の状態

`src/lisp/transpile.lisp`はすでに着手済みだが、ロード不可能な壊れたスケルトン
(約125行)である:

- `lisp-name-to-c-name`が未定義の`c-perfix`を参照している(`c-prefix`のタイプミス)。
- `transpile-expr`の`os-defun`ケースが、未定義のヘルパー`captured-names`/
  `transpile-binding-decls`を呼んでいる。
- `case`式の途中(`os-defun`ケースの直後)で閉じ括弧が欠落しており、ファイルが
  構文的に完結していない。
- `main`が未定義の`*runtime-lisp-path*`を参照している。

`src/lisp/init2.lisp`は3行の動作確認用スタブ(`(defun add3 (x) (+ x 3))`)で、
実質的な内容はまだない。

以降のマイルストンは、ゼロから作るのではなく、この壊れた雛形を直しながら
段階的に育てていく形になる。

## マイルストン一覧

| ID | スコープ | 状態 | コミット |
|---|---|---|---|
| M0 | `transpile`ターゲットの実配線(Docker内`ros run`の動作確認) | 未着手 | — |
| M1 | 手書きCファイルによる生成ファイル経路の確立(build/QEMU起動への影響確認) | 未着手 | — |
| M2 | `transpile.lisp`が固定fixnumを返す関数を1つ生成できるようにする | 未着手 | — |
| M3 | リテラル(fixnum/string/symbol/nil/t)と`quote`のコード生成 | 未着手 | — |
| M4 | `if`/`progn`/`setq`(ローカル変数、クロージャなし)のコード生成 | 未着手 | — |
| M5 | マクロ展開方式の検証(ホストSBCLへの`load`+`macroexpand`) | 完了(検証失敗・フォールバック採用) | #20 |
| M6 | `defun`相当のコード生成(za.cのネイティブABIに合わせる) | 未着手 | — |
| M7 | ヒープ確保を伴うコード生成箇所への`GC_PROTECT`統合 | 未着手 | — |
| M8 | `and`/`or`のエンドツーエンド動作(最初の実マイルストン) | 未着手 | — |
| M9 | 自己/相互再帰・末尾呼び出し | 未着手 | — |
| M10 | `lambda`のリフティングと自由変数捕捉 | 未着手 | — |
| M11 | 動的変数(`defdynamic`/`dynamic`) | 未着手 | — |
| M12 | ILOS(クラス・generic function)とコンディションシステム | 未着手 | — |
| M13 | 統合: init.lispの一部をAOTリンク経路へ切り替え | 未着手 | — |

init.lispは巨大なので、M8で対応範囲を「まず`and`/`or`が通る」程度の小さなスライスに
絞り、以降のマイルストンで段階的に広げていく方針をとる。ILOS・コンディション
システム(M12)は最終的にはトランスパイル対象に含める前提とし、恒久的に
インタプリタ実行のまま、という前提は置かない(規模・複雑さの理由から着手順は
最後になる見込み)。

## 各マイルストン詳細

### M0: ビルド導線の実配線

**やること**

- Makefileの`.PHONY: transpile`に実際のレシピを追加する。既存の`build`/`compile`/
  `test`ターゲットと同じパターン(`docker run --rm --user "$(id -u):$(id -g)"
  --entrypoint bash -v "$(PWD)":/workspace isiki-builder -c 'ros run --load
  src/lisp/transpile.lisp --eval "(main)" --quit'`)を踏襲する。
- `fukamachi/sbcl`ベースのDockerイメージ内で本当に`ros`コマンドが素の状態で
  動くかを実機検証する(前提として動くはずだが、確認済みではない)。
  参考実装(mini-os)のDockerfileは明示的にRoswellをインストールしているため、
  もし`fukamachi/sbcl`のプリインストールだけでは不十分だった場合、同様の
  インストールステップの追加が必要になる可能性がある。

**検証方法**

- `make transpile`をコンテナ内で実行し、Docker/Roswell起動エラーが出ないことを
  確認する(この時点では`transpile.lisp`自体がロード不可能でも構わない。
  ビルド導線そのものが動くかの確認に限定する)。

**見送ってよい範囲**

- `transpile.lisp`の内容修正はM2以降で行う。ここでは導線のみ。

### M1: 生成ファイル経路の確立

**やること**

- 手書きの trivial な`.c`ファイル(例: 固定fixnumを返す関数1つ)を`src/c/`配下に
  仮に置き、Makefileの`SRC`リストへ追加する。
- 既存の`build`/`test-qemu`が、この追加ファイルの存在によって壊れないことを
  確認する。

**検証方法**

- `make build && make test-qemu`が従来通り成功することを確認する。

**実装のポイント**

- 「生成されたCファイルをリンクできるか」と「トランスパイラが正しいCを
  生成できるか」を分離して検証するためのステップ。この手書きファイルは
  M2で`transpile.lisp`の実際の出力に置き換える。

### M2: 最初の実生成物

**やること**

- `transpile.lisp`の既知バグ(`c-perfix`タイプミス、未定義ヘルパー呼び出し、
  `case`式の閉じ括弧欠落、未定義の`*runtime-lisp-path*`)を修正し、固定fixnumを
  返す関数を1つ生成できるようにする。
- M1で仮置きした手書きCファイルを、この生成結果に置き換える。

**検証方法**

- `test/c`のユニットテスト、またはQEMU起動スクリプトから生成された関数を呼び、
  戻り値のタグ・値を検証する。

### M3: リテラルと`quote`

**やること**

- fixnum/string/symbol/nil/tのリテラルと`quote`のコード生成に対応する。

**実装のポイント**

- fixnumは`make_fixnum(n)`相当の即値構築でよいが、string/symbolはヒープ上の
  静的データ+構築呼び出し(`os_make_string`/`os_make_symbol`相当)が必要になる。
  za.cの`quote`シンボルリテラル対応(拡張7、`za_classify_operand`)が
  参考になる。

### M4: `if`/`progn`/`setq`

**やること**

- ローカル変数(クロージャなし)を対象に、`if`/`progn`/`setq`のコード生成に
  対応する。

**実装のポイント**

- 真偽値は「nil以外はすべて真」という規約に従い、`nil`との比較+条件分岐で
  `if`を実装する(za.cの拡張0と同じ設計)。

### M5: マクロ展開方式の検証

これは検証マイルストンであり、結果によって以降のマイルストンの設計が変わる。

**やること**

- `init.lisp`の`defmacro`定義(`and`/`or`など)を、トランスパイラのホストである
  SBCLへそのまま`load`し、`macroexpand`/`macroexpand-1`で目的の形へ展開できるかを
  検証する。成立すれば、トランスパイラのコード生成本体はM3/M4で用意した
  核となる特殊形式(`quote`/`if`/`progn`/`setq`/`defun`/`lambda`/`block`/
  `return-from`/`unwind-protect`)だけを相手にすればよくなり、`and`/`or`/`cond`/
  `let`等それぞれを個別に実装する必要がなくなる。

**検証すべき懸念**

- ISLispとCommon Lispのgensym・パッケージ・reader差異により、`load`した
  マクロ定義がSBCL上でそのまま正しく展開されない可能性がある。

**見送ってよい範囲(フォールバック)**

- 検証が失敗した場合、対象範囲を絞った専用の最小マクロ展開器を自前で実装する
  方針に切り替える。この判断はこのマイルストンの結果として明記し、
  以降のマイルストン(特にM8)の前提を更新する。

**検証結果**

- ホストのSBCL(2.6.7)へ`init.lisp`を素の`common-lisp-user`パッケージへ
  `(load "src/lisp/init.lisp")`したところ、`defmacro let`の評価時点で
  `The special operator LET can't be redefined as a macro.`というエラーで
  即座に失敗した。`init.lisp`には`defpackage`/`in-package`が一切無く、
  `let`/`let*`/`and`/`or`/`cond`/`case`/`the`/`setf`等、標準Common Lispの
  特殊形式・マクロ名と衝突する名前のまま`defmacro`しているため、これらの名前は
  `common-lisp`パッケージのシンボルを指しており、SBCLは特殊形式のリバインドを
  拒否する。`and`/`or`単体を個別パッケージへ切り出す、あるいは`shadow`で
  回避する手段も原理的にはあるが、`init.lisp`自体はISLisp処理系の起動時ロード
  対象であって本マイルストンのために書き換える対象ではないため、そうした
  回避策の追求は見送る。
- 結論: 検証は失敗。**フォールバック(対象範囲を絞った専用の最小マクロ展開器を
  自前で実装する方針)を採用する**。`init.lisp`の`defmacro`定義をそのまま
  ホストSBCLへ`load`して`macroexpand`する経路には依存しない。M8以降は
  `and`/`or`/`cond`等それぞれについて、トランスパイラ側で個別に展開規則を
  実装する(またはトランスパイラ自身のミニマルな`defmacro`/`macroexpand`相当の
  機構を用意し、`init.lisp`の定義本文をその機構にだけ通す)方針で進める。

### M6: `defun`相当のコード生成

**やること**

- za.cの既存ネイティブABI(`lisp_val_t fn(lisp_val_t evaluated_args, lisp_val_t
  env)`)に合わせて、`defun`相当のコード生成を実装する。パラメータ束縛は
  za.cと同様、`evaluated_args`を`cc_car`/`cc_cdr`で辿る形にする。

**実装のポイント**

- 生成した関数を`MAGIC_FUNCTION_NATIVE`として関数セルに登録すれば、
  `apply_function`から見て手書きprimitiveやza JIT生成コードと区別されない。
  これにより既存のディスパッチ機構への変更が不要になる。

### M7: `GC_PROTECT`統合

**やること**

- ヒープ確保(`cons`、文字列構築等)を伴うコード生成箇所で、既存の
  `GC_PROTECT`/`gc_rootnode`の仕組みに参加する。

**実装のポイント**

- za.cの拡張1は、この`GC_PROTECT`マクロがコンパイル時に展開する処理
  (シャドウスタックへのpush/pop)を、JIT生成の機械語として自前で再現している。
  トランスパイラはCソースを生成するので、za.cのように機械語で模倣する必要はなく、
  生成するC関数の本体に`GC_PROTECT`マクロそのものを直接埋め込めばよい
  (Cコンパイラの`cleanup`属性がそのまま効く)。この点はza.cより単純になる見込み。

**検証方法**

- ループ内で`cons`する生成関数を用意し、ループ中に`os_gc_collect`を強制発火させ、
  GC後もポインタが正しく指し示す先を追従していることを確認する。

### M8: `and`/`or`のエンドツーエンド動作

ユーザーが最初に指定した、最初の実マイルストン。init.lispが巨大なため、
まずこの小さなスライスで全体のパイプラインを通す。

**やること**

- M5の検証結果により、`init.lisp`の`defmacro`定義をホストSBCLへそのまま
  `load`して`macroexpand`する経路は採用しない(#20)。代わりに`and`/`or`の
  展開規則をトランスパイラ側で個別に用意し、M3/M4/M6/M7で用意したコード
  生成器で実際にCへ変換・リンク・実行する。

**検証方法**

- 短絡評価を含め、インタプリタ実行時と同じ結果になることをQEMU起動テストまたは
  `test/c`で確認する。

**見送ってよい範囲**

- `cond`/`let`はM5の展開方式が成立していれば、`and`/`or`とほぼ同じ仕組みで
  「ほぼ無償」で追従する見込みが高い。ただし`let`は展開後に即時`lambda`呼び出しの
  形になるため、実際にはM10(クロージャ・lambdaリフティング)が完了するまで
  対応できない可能性がある(za.cの拡張2でも同じ理由で`let`は恒久的に
  フォールバック対象として残っている)。`case`は展開後の分岐ロジックが
  M3/M4の範囲を超える可能性があり、個別確認が必要。

### M9: 自己/相互再帰・末尾呼び出し

**やること**

- 自己再帰・相互再帰に対応する。init.lispのリスト操作関数の多くは再帰的に
  書かれているため、`for`/`while`やリストユーティリティのトランスパイルに
  先立って必要になる。

**実装のポイント**

- za.cの拡張3(呼び出し先アドレスをキャッシュせず毎回解決する設計)と同じ方針を
  踏襲できるか検討する。末尾呼び出しの扱い(トランポリン方式を導入するか、
  素のCの再帰呼び出しでCスタックの余裕が足りるか)は未検証であり、
  実際にCスタックサイズを計測してから判断する。

### M10: `lambda`のリフティングと自由変数捕捉

**やること**

- `lambda`をトップレベルのC関数へリフトし、自由変数の捕捉を解析する。

**実装のポイント**

- 参考実装(mini-os)の自由変数解析(`captured-names`)は参考にするが、
  捕捉した変数の保持方式はisiki-osの既存`GC_PROTECT`ベースに作り直す
  (mini-os独自のshadow-stack配列は流用しない)。
- za.cの拡張4で導入された「setqされエスケープする変数だけをcell昇格させる」
  という変数単位box化の考え方(`documents/let.md`参照)も、生成Cコードの
  設計判断として参考になる。

### M11: 動的変数

**やること**

- `defdynamic`/`dynamic`のコード生成に対応する。

**実装のポイント**

- isiki-osの動的変数は単一のグローバルalist(`g_dynamic_bindings`)を
  `os_get_dynamic`/`os_set_dynamic`で読み書きするだけの仕組み(za.cの拡張6と同じ)
  なので、生成コードもこれらのC関数を直接callするだけでよく、独自の
  バインディングスタックは不要な見込み。

### M12: ILOSとコンディションシステム

**やること**

- `defclass`/`make-instance`/スロットアクセス/`defgeneric`/`defmethod`、および
  ISLispの条件システム(`signal-condition`/`with-handler`等)のコード生成に
  対応する。

**実装のポイント**

- 規模・複雑さの都合上、他のマイルストンがすべて完了した後の最終段階として
  着手する想定。za.cの拡張7が「quoteシンボルリテラル対応のみ」で部分完了に
  留まった経緯(`&rest`引数の値参照・`let`展開への依存という別々の未対応機能が
  残っていたため)を踏まえ、このマイルストンに着手する時点でM9(再帰)・M10
  (クロージャ)が完了していることを前提条件として明記する。

### M13: 統合

**やること**

- 実際に`init.lisp`の一部(例: M8-M11で対応済みの範囲)を、`cc_load()`+
  Immobilized Space経由の実行時ロードから、AOTコンパイル+リンクの経路へ
  切り替える。

**検証方法**

- 切り替え前後でQEMU起動時の挙動が変わらないことに加え、Immobilized Spaceの
  使用量(ページ数)が実際に減っていることを確認する。これがこのプロジェクト
  全体の目的そのものであるため、単なる機能追加ではなく既存の脆弱な仕組みへの
  依存を減らせているかを明示的に測定する。

## 未解決の論点

1. ~~M5のホストSBCLへの`load`+`macroexpand`方式が本当に成立するか~~
   → 解決(#20): 成立しない。`init.lisp`に`defpackage`/`in-package`が無く、
   `let`/`and`/`or`等が標準Common Lispの特殊形式・マクロ名と衝突するため、
   ホストSBCLへの直接`load`は`The special operator LET can't be redefined
   as a macro.`で即座に失敗する。フォールバック(対象範囲を絞った専用の
   最小マクロ展開器を自前で実装する方針)を採用する。
2. 生成コードのABIをza.cの既存ネイティブABIに完全一致させる方針でよいか
   (現状の調査では強く支持されるが、明文化して確定させておく)。
3. 生成するCファイルを1つの巨大ファイルにするか、フォーム単位/インクリメンタルな
   生成にするか(ビルド速度・マイルストン単位でのレビュー容易性に影響する)。
4. M9のCスタック余裕は未計測であり、素の再帰呼び出しで足りるか、
   za.cのようなトランポリン方式が必要になるかは実測してから判断する。
