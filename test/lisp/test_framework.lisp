;; test/lisp/test_framework.lisp
;;
;; QEMU実機上でLispテストを走らせるための共通フレームワーク(assert-equal等)。
;; isiki_test.lisp/za_test.lisp/za_test_ext5.lispはこのファイルが定義するマクロ・
;; 関数を前提とするが、自分自身ではこのファイルをloadしない。boot-entryスクリプト
;; (qemu_boot_test.lisp、qemu_boot_m*.lisp)が最初に1回だけloadする決まりにする
;; (内容ファイル側にloadを持たせると、1ブートで複数の内容ファイルを連続loadする
;; ケースで*isiki-test-stream*が再オープンされてtest-results.txtの上書き・
;; カウンタリセットが起きるため)。

(defglobal *isiki-test-stream* (open-output-file "/9p/test-results.txt"))
(defglobal *isiki-test-pass* 0)
(defglobal *isiki-test-fail* 0)

;; ---------------------------------------------------------------------------
;; 進捗マーカーと経過時間。
;;
;; **合計だけでは、ハングしたときに何も分からない。** test-results.txtは
;; isiki-test-reportが最後に書くので、途中で止まると「空のファイル」しか残らず、
;; どのファイルのどこまで進んだのかを外から知る手段が無い。
;;
;; そこでloadの単位で1行ずつ、開始からの経過時間つきで書き出す。
;; **1ファイル1行**なので通常実行でも負担にならず、監査モード(*isiki-audit*、
;; 全アサーションを1件ずつ出す)を有効にしなくても効く。
;;
;; 時間の単位はtick。get-internal-real-timeはPITの分周設定(約100Hz)の
;; tick数をそのまま返すので、**100 tick = 1秒**(1 tick = 10ms)である
;; (src/c/clock.c の TICKS_PER_SECOND)。
;; **随時出力するか、結果だけにするかの切り替え。**
;; #at の行は 9p 経由のファイルへ finish-output まで行うため、**1行あたりの
;; コストが無視できない**(test-results.txt はホスト側の共有ディレクトリにあり、
;; QEMU の 9p は msize <= 8192 で動いている)。
;; ハングの切り分けをするときは t、時間を計測するときは nil にする。
;; nil でも #elapsed(最後に1回)は出るので、総時間の比較はできる。
(defglobal *isiki-test-progress* t)

(defglobal *isiki-test-time-start* (get-internal-real-time))
(defglobal *isiki-test-mark-prev* 0)   ; 直前のマーク時点の経過tick

;; frameworkをloadしてからの経過tick
(defun isiki-test-elapsed ()
  (- (get-internal-real-time) *isiki-test-time-start*))

;; 進捗を1行書いてフラッシュする。t=は開始からの経過tick、+=は直前のマークからの差分。
;; **フラッシュまでやること。** バッファに溜めたままだとハング時に消える。
(defun isiki-test-mark (label)
  (if *isiki-test-progress*
      (let ((now (isiki-test-elapsed)))
        (format *isiki-test-stream* "#at ~A t=~D += ~D~%"
                label now (- now *isiki-test-mark-prev*))
        (setq *isiki-test-mark-prev* now)
        (finish-output *isiki-test-stream*))
    nil))

;; 試験ファイルを進捗マーカーつきでloadする。boot-entryスクリプトは
;; (load "test/lisp/xxx_test.lisp") の代わりにこれを使う。
;; **loadの前に印をつける**ので、ハングしたときは最後に出ている行の
;; ファイルが犯人になる(通り抜けていれば次の行が出る)。
(defun isiki-test-load (path)
  (isiki-test-mark path)
  (isiki-audit-begin path)
  (load path))
;; ---------------------------------------------------------------------------

;; ---------------------------------------------------------------------------
;; [GC監査] 塗り潰し(ISIKIOS_GC_PAINT)下で全試験を流すための逐次出力モード。
;; documents/pitfalls.md 原則6。
;;
;; 監査で要るのは「最後にまとめて出す合計」ではなく「1件ごとの記録」である。
;; 塗り潰し下ではゲストがハングしたりCPU例外で止まったりしうるため、まとめて
;; 出す形だとそこまでの情報がすべて失われる。1件ごとにfinish-outputまで行い、
;; ホスト側のtest-results.txtへ即座に反映させる。
;;
;; *isiki-audit*がnilの間は既存の振る舞いを変えない(assert-*の展開形に
;; 実行時のifが1つ増えるだけで、出力も合計値も従来どおり)。
(defglobal *isiki-audit* nil)          ; 監査モードの有効・無効
(defglobal *isiki-audit-index* 0)      ; 通番(何件目のアサーションか)
(defglobal *isiki-audit-label* "-")    ; いま流している試験ファイル名
(defglobal *isiki-audit-gc-prev* 0)    ; 直前のアサーション時点のGC回数
(defglobal *isiki-audit-time-prev* 0)  ; 直前のアサーション時点の時刻(tick)
(defglobal *isiki-audit-first-ng* 0)   ; 最初にNGが出た通番(0なら未発生)
;; %%DIAG-GC-STALE-HITSはISIKIOS_GC_DEBUGビルドにしか存在しない。通常ビルドでの
;; 基準時間取得にも同じ経路を使うため、呼んでよいかをboot-entry側から明示する
(defglobal *isiki-audit-stale-available* nil)

;; 試験ファイルの切り替わりを記録する。boot-entryスクリプトがloadの直前に呼ぶ
(defun isiki-audit-begin (label)
  (setq *isiki-audit-label* label)
  (if *isiki-audit*
      (progn
        (format *isiki-test-stream* "#file ~A~%" label)
        (finish-output *isiki-test-stream*))
    nil))

(defun isiki-audit-stale-hits ()
  (if *isiki-audit-stale-available* (%%diag-gc-stale-hits) 0))

;; 塗り潰しのトラップを読んだ延べ回数。範囲検査(stale)と違いGC世代の偶奇に
;; 依存しないので、監査で実際に見るのはこちら
(defun isiki-audit-trap-hits ()
  (if *isiki-audit-stale-available* (%%diag-gc-trap-hits) 0))

;; 検出箇所の一覧。**件数は「回数」であって「箇所数」ではない**ので、
;; 箇所ごとにdedupeした表をここで出す。addrはtools/bench/locate_rip.shで
;; 関数名へ逆引きする(anchorが実行時の基準アドレス)
(defun isiki-audit-trap-report ()
  (if *isiki-audit-stale-available*
      (let ((k (%%diag-gc-trap-sites)) (i 0))
        (progn
          (format *isiki-test-stream* "#trap hits=~D sites=~D painted-fields=~D anchor=~D~%"
                  (%%diag-gc-trap-hits) k (%%diag-gc-painted-fields) (%%diag-image-anchor))
          (while (< i k)
            (progn
              (format *isiki-test-stream* "#trapsite ~D addr=~D hits=~D~%"
                      i (%%diag-gc-trap-site-addr i) (%%diag-gc-trap-site-hits i))
              (setq i (+ i 1))))
          (finish-output *isiki-test-stream*)))
    nil))

;; アサーション1件ぶんの記録。OK/NGにかかわらず記録し、**検出しても中断しない**
;; (最初の1件で止まると以後が全部隠れるため)。最初のNGの通番を覚えておき、
;; それ以降の結果は独立に再確認が必要であることをホスト側が判定できるようにする。
;; 行はアサーションの**後**に出るので、ハングしたときは
;; 「最後に出ている通番 + 1」がハングしたアサーションになる
(defun isiki-audit-record (ok)
  (if *isiki-audit*
      (let ((gc (%%gc-collect-count)) (tm (get-internal-real-time)))
        (setq *isiki-audit-index* (+ *isiki-audit-index* 1))
        (if (and (not ok) (= *isiki-audit-first-ng* 0))
            (setq *isiki-audit-first-ng* *isiki-audit-index*)
          nil)
        (format *isiki-test-stream* "#t ~D ~A ~A gc=~D tick=~D t=~D trap=~D stale=~D~%"
                *isiki-audit-index* *isiki-audit-label*
                (if ok "OK" "NG")
                (- gc *isiki-audit-gc-prev*)
                (- tm *isiki-audit-time-prev*)
                (isiki-test-elapsed)
                (isiki-audit-trap-hits)
                (isiki-audit-stale-hits))
        (setq *isiki-audit-gc-prev* gc)
        (setq *isiki-audit-time-prev* tm)
        (finish-output *isiki-test-stream*))
    nil))
;; ---------------------------------------------------------------------------

;; ---------------------------------------------------------------------------
;; エラーで中断したアサーションの検出。
;;
;; assert-*の展開形の中でformの評価がエラー(handlerの無いsignal-conditionによる
;; %abort-top-level、あるいはprimitiveのeval-error)で中断すると、そのトップレベル
;; フォーム全体が捨てられ、passにもfailにも数えられずに「消える」。これでは
;; 「0 failed」でも全件通ったとは言えないので、各assert-*はformを評価する**前**に
;; *isiki-test-attempt*を進めておき、次のassert-*の入口(とisiki-test-report)で
;; attempt > pass + fail なら直前のフォームが中断したと判定してfailに数える。
;; 中断したフォームは*isiki-test-last-form*に残っているので[ABORT]として記録できる
(defglobal *isiki-test-attempt* 0)
(defglobal *isiki-test-last-form* nil)

(defun isiki-test-begin (form)
  (if (> *isiki-test-attempt* (+ *isiki-test-pass* *isiki-test-fail*))
      (progn
        (setq *isiki-test-fail* (+ *isiki-test-fail* 1))
        (isiki-audit-record nil)
        (format *isiki-test-stream* "[ABORT] ~S => aborted by error~%" *isiki-test-last-form*))
    nil)
  (setq *isiki-test-attempt* (+ *isiki-test-attempt* 1))
  (setq *isiki-test-last-form* form))
;; ---------------------------------------------------------------------------

(defmacro assert-equal (expected form)
  `(let ((%isiki-expected (progn (isiki-test-begin ',form) ,expected)) (%isiki-actual ,form))
     (if (equal %isiki-expected %isiki-actual)
         (progn
           (setq *isiki-test-pass* (+ *isiki-test-pass* 1))
           (isiki-audit-record t))
         (progn
           (setq *isiki-test-fail* (+ *isiki-test-fail* 1))
           ;; 監査記録を先に出す。塗り潰し下では%isiki-actualがトラップパターン
           ;; (タグ=TAG_FORWARD)になりうるので、~Sでの印字が暴走する可能性がある。
           ;; 先に通番を確定させておけば、印字で落ちても何件目かは残る
           (isiki-audit-record nil)
           (format *isiki-test-stream* "[NG] ~S => ~S (expected ~S)~%"
                   ',form %isiki-actual %isiki-expected)))))

(defmacro assert-float-close (expected form)
  `(let ((%isiki-expected (progn (isiki-test-begin ',form) ,expected)) (%isiki-actual ,form))
     (if (< (abs (- %isiki-expected %isiki-actual)) 1.0e-6)
         (progn
           (setq *isiki-test-pass* (+ *isiki-test-pass* 1))
           (isiki-audit-record t))
         (progn
           (setq *isiki-test-fail* (+ *isiki-test-fail* 1))
           (isiki-audit-record nil)
           (format *isiki-test-stream* "[NG] ~S => ~S (expected ~~ ~S)~%"
                   ',form %isiki-actual %isiki-expected)))))

;; (assert-error form) : formの評価が何らかのconditionをsignalすること
;; (仕様の "an error shall be signaled" の例)を検証する。handlerは
;; return-fromで脱出するので、continuableかどうかにかかわらず捕捉した時点で
;; passになる。formが正常に値を返した場合はfailとして、その値を記録する
(defmacro assert-error (form)
  `(let ((%isiki-actual
          (block %isiki-assert-error
            (isiki-test-begin ',form)
            (with-handler (lambda (%isiki-c) (return-from %isiki-assert-error '%isiki-signaled))
              (list '%isiki-no-error ,form)))))
     (if (eq %isiki-actual '%isiki-signaled)
         (progn
           (setq *isiki-test-pass* (+ *isiki-test-pass* 1))
           (isiki-audit-record t))
         (progn
           (setq *isiki-test-fail* (+ *isiki-test-fail* 1))
           (isiki-audit-record nil)
           (format *isiki-test-stream* "[NG] ~S => ~S (expected an error to be signaled)~%"
                   ',form (car (cdr %isiki-actual)))))))

;; (assert-error-class class-designator form) : formの評価が
;; class-designator(またはそのサブクラス)のコンディションをsignalすることを
;; 検証する。assert-errorが「何かがsignalされた」までしか見ないのに対し、
;; こちらは**どのクラスか**まで固定する。
;;
;; EVAL-ERROR返しをsignalへ寄せていく作業(documents/error-unwind-survey.md
;; §A-4 / §6-3 B)では、「エラーになること」だけでなく「仕様のerror-idどおりの
;; クラスであること」までが検証対象になる。assert-errorで通してしまうと、
;; 0除算が<simple-error>でsignalされても誰も気づかない。
;;
;; class-designatorは**評価する**(typepと同じクラスdesignator: クラス名symbolでも
;; クラスオブジェクトでもよい)。呼び出し側はquoteして渡す:
;;   (assert-error-class '<division-by-zero> (div 1 0))
;;
;; ハンドラは仕様どおり必ず制御を転送する(spec:6927-6930)。continuableかどうかは
;; 問わない。assert-errorと同じくisiki-test-beginを先に呼ぶので、中断検出
;; (*isiki-test-attempt*)にも正しく乗る。
;;
;; 結果は3通りに分けてNGメッセージに出す:
;;   signalされてクラスも一致     -> pass
;;   signalされたがクラスが違う   -> NG(実際のクラス名を出す)
;;   signalされず値を返した       -> NG(その値を出す。EVAL-ERRORもここに落ちる)
(defmacro assert-error-class (class-designator form)
  `(let ((%isiki-class ,class-designator))
     (let ((%isiki-actual
            (block %isiki-assert-error-class
              (isiki-test-begin ',form)
              (with-handler
                  (lambda (%isiki-c)
                    (return-from %isiki-assert-error-class
                      (list '%isiki-signaled
                            (typep %isiki-c %isiki-class)
                            (%%class-name (class-of %isiki-c)))))
                (list '%isiki-no-error ,form)))))
       (if (eq (car %isiki-actual) '%isiki-signaled)
           (if (car (cdr %isiki-actual))
               (progn
                 (setq *isiki-test-pass* (+ *isiki-test-pass* 1))
                 (isiki-audit-record t))
             (progn
               (setq *isiki-test-fail* (+ *isiki-test-fail* 1))
               (isiki-audit-record nil)
               (format *isiki-test-stream* "[NG] ~S => signaled ~S (expected ~S)~%"
                       ',form (car (cdr (cdr %isiki-actual))) %isiki-class)))
         (progn
           (setq *isiki-test-fail* (+ *isiki-test-fail* 1))
           (isiki-audit-record nil)
           (format *isiki-test-stream* "[NG] ~S => ~S (expected ~S to be signaled)~%"
                   ',form (car (cdr %isiki-actual)) %isiki-class))))))

;; (assert-output (result-var output-var) form body...) : formを
;; *standard-output*が文字列出力ストリーム(create-string-output-stream)に
;; 束縛された状態で評価し、その戻り値をresult-var、出力された文字列を
;; output-varへ束縛してbodyを評価する。room_test.lisp等で使っていた
;; with-standard-output+create-string-output-stream+get-output-stream-stringの
;; 定型コードをまとめたヘルパー。bodyの中で通常のassert-equal等を呼んで
;; result-var/output-varを検証する(出力内容が完全一致でなくprefix/suffix等
;; での検証が必要な場合にも対応できるよう、単一のexpected値との比較には
;; 固定しない)。
(defmacro assert-output (vars form &rest body)
  (let ((result-var (car vars)) (output-var (car (cdr vars))))
    `(with-standard-output (create-string-output-stream)
       (let ((,result-var ,form))
         (let ((,output-var (get-output-stream-string (standard-output))))
           ,@body)))))

(defun isiki-test-report ()
  ;; 最後のassert-*が中断していた場合を拾う(isiki-test-beginと同じ判定)
  (if (> *isiki-test-attempt* (+ *isiki-test-pass* *isiki-test-fail*))
      (progn
        (setq *isiki-test-fail* (+ *isiki-test-fail* 1))
        (format *isiki-test-stream* "[ABORT] ~S => aborted by error~%" *isiki-test-last-form*))
    nil)
  (if *isiki-audit*
      (format *isiki-test-stream* "#audit total=~D first-ng=~D~%"
              *isiki-audit-index* *isiki-audit-first-ng*)
    nil)
  ;; 総経過時間。100 tick = 1秒(src/c/clock.c の TICKS_PER_SECOND)
  (let ((e (isiki-test-elapsed)))
    (format *isiki-test-stream* "#elapsed ~D ticks = ~D sec~%" e (div e 100)))
  (format *isiki-test-stream* "~%==== isiki tests: ~D passed, ~D failed ====~%"
          *isiki-test-pass* *isiki-test-fail*))
