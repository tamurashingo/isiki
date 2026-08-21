;; test/lisp/za_test_ext19.lisp
;;
;; za.c(defunをx86-64機械語へJITコンパイルするコンパイラ)の拡張19
;; (defglobal変数・未束縛シンボルの読み込みおよびsetq)を検証するテスト。
;; za_test_ext5.lisp〜za_test_ext18.lispと同様に独立ファイルとして切り出す。
;; za.cのグラマー全体・za_test.lisp(拡張0〜4)については test/lisp/za_test.lisp の
;; コメントを参照。let-localへのsetq(拡張9)については za_test_ext9.lisp を参照。
;;
;; 本ファイルは test/lisp/test_framework.lisp が定義する assert-equal 等をそのまま
;; 使う(boot-entryスクリプトが本ファイルより先にそれをloadしている前提)。他のext系
;; ファイルへの依存は無い(自己完結、iz19-プレフィクスで独自にヘルパーを定義する)。
;;
;; 読み込みはos_get_variable、setqはos_setq_variable_checked(os_setq_variableに
;; defconstant保護を加えたラッパー、runtime.c)をそのまま呼び出すだけで、インタプリタ
;; と同じ「envの親チェーンを辿って解決する」セマンティクスを再現する。コンパイル時に
;; 「本当にdefglobalか」を判定する必要は無い(local/param/&restのいずれでもない裸
;; シンボルは全てこの経路になる)。

;;; --- 1. defglobal変数の読み込みのみ ---

(defglobal *iz19-foo* 42)

(defun iz19-read-foo (x)
  (+ *iz19-foo* x))
(assert-equal t (%%za-compiled-p (function iz19-read-foo)))
(assert-equal 43 (iz19-read-foo 1))

;;; --- 2. defglobal変数へのsetq(ユーザー提示の元コードと同じ形) ---

(defglobal *iz19-bar* 0)

(defun iz19-set-bar (x)
  (let ((y x))
    (setq *iz19-bar* y)))
(assert-equal t (%%za-compiled-p (function iz19-set-bar)))
(assert-equal 7 (iz19-set-bar 7))
;; setq後、グローバル変数の値は別の関数からの再読み込みでも実際に変わっている
;; (トップレベルからの読み込みも含む、インタプリタと同じ「envの共有」)
(assert-equal 7 *iz19-bar*)

(defun iz19-read-bar ()
  *iz19-bar*)
(assert-equal t (%%za-compiled-p (function iz19-read-bar)))
(assert-equal 7 (iz19-read-bar))

;;; --- 3. 未束縛(defglobalしていない)シンボルへのsetq ---
;;
;; os_setq_variableは既存の束縛が見つからない場合、setq呼び出し時に渡されたenvへ
;; os_set_variableで新規束縛を作る(インタプリタの既存動作そのもの、runtime.c参照)。
;; ただしJITコンパイル済み関数の呼び出しは、インタプリタのapply_function(eval.c、
;; MAGIC_FUNCTION_INTERPRETED分岐)が毎回作る使い捨てのCALL-ENVを経由しない
;; ——za.cはパラメータ・let-localを一切lispの環境フレームへ束縛せずレジスタ/スタック
;; スロットで直接保持するため、JIT関数はos_make_jit_function経由でMAGIC_FUNCTION_NATIVE
;; 扱いとなり、apply_functionはネイティブ関数と同じく呼び出し元のenvをそのまま
;; 引き渡す(eval.c:130-133)。よってトップレベル(env=global_environment)から呼んだ
;; JITコンパイル済み関数内でのsetqは、CALL-ENVという使い捨てフレームが無いため
;; 呼び出し元のenv——ここではglobal_environment自身——へ直接新規束縛を作ってしまい、
;; 事実上defglobalしたのと同じように呼び出しを越えて値が残る。これは今回の拡張で
;; 導入したものではなくJITの呼び出し規約が元々持っていた性質であり(is_literal==6の
;; `(function sym)`解決など既存コードも同じくenvをそのまま伝播させている)、新しい
;; 環境表現を導入しない今回のスコープでは解消しない既知の相違点として、その通りの
;; 挙動をテストする。

;; za_try_compile_defunはdefun本体がトップレベルで複数formに分かれている場合、
;; 常にfallbackする既存の制限(cc_cdr(body)!=nilチェック、za.c)があるため、
;; setqと直後の読み込みという2つのformをprognで1つにまとめる必要がある。
(defun iz19-set-then-read-unbound (v)
  (progn
    (setq *iz19-unbound-2* v)
    *iz19-unbound-2*))
(assert-equal t (%%za-compiled-p (function iz19-set-then-read-unbound)))
(assert-equal 99 (iz19-set-then-read-unbound 99))
;; トップレベル呼び出しなので上のsetqはglobal_environmentへ直接束縛される。よって
;; 別の(JITコンパイル済みの)呼び出しからの読み込みでも99が見え続ける
;; (defglobalした場合と区別が無い、上記コメント参照)。
(defun iz19-read-unbound-only ()
  *iz19-unbound-2*)
(assert-equal t (%%za-compiled-p (function iz19-read-unbound-only)))
(assert-equal 99 (iz19-read-unbound-only))
;; トップレベルの裸シンボル読み込みでも同じenvを見るため99が見える
(assert-equal 99 *iz19-unbound-2*)

;;; --- 4. defconstant変数へのsetq(g_sym_eval_error相当、defconstant保護) ---

(defconstant *iz19-const* 123)

(defun iz19-set-const (v)
  (setq *iz19-const* v))
(assert-equal t (%%za-compiled-p (function iz19-set-const)))
(assert-equal 'eval-error (iz19-set-const 456))
;; 保護されているため値自体は変わっていない
(defun iz19-read-const ()
  *iz19-const*)
(assert-equal t (%%za-compiled-p (function iz19-read-const)))
(assert-equal 123 (iz19-read-const))

;;; --- 5. ローカル変数・固定引数によるシャドーイング(同名グローバルより優先) ---

(defglobal *iz19-shadow* 1000)

(defun iz19-shadow-by-param (*iz19-shadow*)
  (+ *iz19-shadow* 1))
(assert-equal t (%%za-compiled-p (function iz19-shadow-by-param)))
(assert-equal 6 (iz19-shadow-by-param 5))
;; 固定引数はコピーであり、グローバル本体には影響しない
(assert-equal 1000 *iz19-shadow*)

(defun iz19-shadow-by-let (x)
  (let ((*iz19-shadow* x))
    (setq *iz19-shadow* (+ *iz19-shadow* 1))
    *iz19-shadow*))
(assert-equal t (%%za-compiled-p (function iz19-shadow-by-let)))
(assert-equal 11 (iz19-shadow-by-let 10))
;; let-localへのsetqであり、同名グローバルは変更されない
(assert-equal 1000 *iz19-shadow*)
