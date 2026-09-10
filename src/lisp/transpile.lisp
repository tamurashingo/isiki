;;;; ホスト(CommonLisp)側で実行するトランスパイラ
;;;;
;;;; M9までにサポートしたのは、fixnum/string/symbol/nil/tのリテラルとquote、
;;;; defunパラメータ(クロージャなしのローカル変数)の参照・setq、
;;;; if/progn/and/orを組み合わせた単一の本体式、このファイル内でdefunされた
;;;; 関数同士の自己/相互再帰呼び出し(za.cのような都度のシンボル名解決は行わず、
;;;; AOTでリンクされるC関数を直接呼び出す)、および生成する関数のパラメータを
;;;; GC_PROTECTでshadow stackへ登録するコード生成。
;;;;
;;;; M10ではlambdaによる第一級(エスケープ可能)クロージャを追加する。lambda式は
;;;; トップレベルのC関数(defunと同じ__step/公開ラッパーの2関数構成)へリフトし、
;;;; 自由変数(パラメータでも他のdefun/プリミティブ名でもない裸の変数参照)だけを
;;;; 含む最小限の環境(os_make_environment)に捕捉する。捕捉方式はza.cの拡張4と
;;;; 同じSBCL方式の変数単位box昇格: setqされ、かつ何らかのネストしたlambdaに
;;;; 捕捉される変数だけをbox((val . 実値)のcons、os_setcdrで書き換え、cc_cdrで
;;;; 読み出す)として扱い、それ以外は値コピーで捕捉する。boxは複数のクロージャが
;;;; 同一のcons自体を共有することで、どちらから書き換えても他方から見える
;;;; (za_emit_build_capture_envと同じ考え方)。専用のshadow-stack配列は新設せず、
;;;; 既存のGC_PROTECTベースの仕組みで捕捉環境自体を保護する。let/cond/case等は
;;;; 後続のマイルストンで拡張する。
;;;;
;;;; 末尾位置の既知関数呼び出しは、64KB(ガード無し)のプロセススタックを
;;;; 実測した結果、素朴なC再帰では約410段で隣接プロセスのスタックを破壊する
;;;; ことが分かったため、トランポリン方式で定数スタックに変換する(ユーザー
;;;; 確認済み: 自己再帰だけでなく相互再帰も含む一般的な末尾呼び出しに対応)。
;;;; 各defunは「1手だけ進めるstep関数(tco_result_tを返す)」と「stepをループで
;;;; 回し切る公開ラッパー(ABI互換のlisp_val_tを返す)」の2つのC関数に分解する。
;;;; 末尾位置の既知関数呼び出しはstep関数を実際には呼ばず、呼び出し先の関数
;;;; ポインタと引数だけをtco_result_tに詰めてreturnする。ラッパーのwhileループが
;;;; それを受け取って次のstepを呼ぶため、Cの呼び出しは常にreturnしてから次の
;;;; 呼び出しが起きる「フラットな」形になり、再帰段数に関わらずCスタック消費は
;;;; 一定になる(GC_PROTECTのshadow stack登録もcleanup属性でC関数のスコープに
;;;; 束縛されているため、ループが何回回ってもリークしない)。プリミティブ呼び出し
;;;; は再帰しないstep関数を持たないため、末尾位置でも即時評価して確定値を返す。

(defparameter *runtime-lisp-path* "test/lisp/transpile_fixture.lisp"
  "トランスパイラの動作確認用フィクスチャ(transpile_fixture.lisp参照、全関数が
   %%transpile-fixture-接頭辞を持ちglobal_environmentへ登録もされない、テスト
   専用コード)。本番のAOTコードと一切混ざらないよう、生成先も*output-c-path*
   ではなく*fixture-output-c-path*という別ファイルにする")
(defparameter *aot-lisp-path* "src/lisp/init_aot.lisp"
  "init.lispから移動した、AOTトランスパイル対象の本番関数を置くファイル(M13)。
   transpile_fixture.lispと違い、ここでdefunされた関数はos_register_aot_init_functions
   経由でglobal_environmentへ登録され、init.lisp側からもインタプリタで定義された
   関数と同じシンボル名で呼び出せる")
(defparameter *utility-lisp-path* "src/lisp/utility.lisp"
  "init_aot.lispと同じくAOTトランスパイル対象だが、init.lispからの移動ではなく
   新規に追加するアプリケーション関数(roomコマンド等)を置くファイル。
   init_aot.lispのdefunと同じ制約(パラメータはシンボルのみ、本体は単一の
   トップレベル式)に従う必要がある。init_aot.lispのdefunと合わせて同じ
   os_register_aot_init_functions経由でglobal_environmentへ登録される")
(defparameter *fs-lisp-paths*
  '("src/lisp/device.lisp"
    "src/lisp/ide.lisp"
    "src/lisp/partition.lisp"
    "src/lisp/mount.lisp"
    "src/lisp/file-node.lisp"
    "src/lisp/fat16.lisp"
    "src/lisp/fat32.lisp"
    "src/lisp/file-cmd.lisp")
  "M15(documents/fs.md): IDE/FAT16/FAT32/partition/mount/file-cmdレイヤーの
   AOTトランスパイル対象ファイル。このレイヤー自体がファイルシステムの実装で
   あるため、実機起動時にはファイルシステムからLispソースをloadする手段が
   無く、init_aot.lisp/utility.lispと同じくos_register_aot_init_functions経由で
   カーネルバイナリへ埋め込む必要がある。*aot-lisp-path*等と異なりdefun以外の
   トップレベルフォーム(defclass/defdynamic/起動時に一度だけ実行する副作用の
   ある呼び出し)も持つため、mainではdefunとそれ以外を別々に集める。依存関係の
   順(device->ide->partition->mount->fat16/fat32->file-cmd)で1ファイルずつ
   追加・検証したため、この順序のまま保つ。file-node.lisp([ファイルI/O]#45)は
   fat16.lisp/fat32.lispの<fat16-file-node>/<fat32-file-node>が継承する
   <file-node>を定義するため、この2ファイルより前に置く必要がある(defclassの
   スーパークラス解決は%register-class経由の実行時ルックアップなので、
   *fs-lisp-paths*内での並び順=実際の登録順を守らないと未登録エラーになる)。
   このリストの並び順とMakefileのTRANSPILE_LISP_SRCの並び順は同期を保つ共通の
   ソースが無いため、どちらかにファイルを追加する際は両方を手動で更新すること")
(defparameter *output-c-path* "src/c/lisp_compiled.c")
(defparameter *fixture-output-c-path* "test/c/lisp_compiled_fixture.c"
  "*runtime-lisp-path*(テスト専用フィクスチャ)のコンパイル結果の出力先。
   本番のカーネルバイナリ(*output-c-path*)には一切含めず、テストビルド時だけ
   このファイルを追加でコンパイル・リンクする")

(defparameter *known-function-names* nil
  "現在のトランスパイル対象ファイル内でdefunされている関数名の一覧。mainが
   全defunを読み終えた時点で束縛し、transpile-callが呼び出し先を解決する際に
   参照する(自己/相互再帰が定義順に関係なく解決できるようにするため)")

(defparameter *known-function-arities* nil
  "ABI-M6: *known-function-names*と対になる(name . fixed-arity)のalist。
   &restパラメータを持たないdefunのみ、その固定パラメータ数をfixed-arityとして
   持つ(&restを持つ、または未知の関数はassocがnilを返す)。mainが
   *known-function-names*と同時に束縛する。known-function-fixed-arity参照")

(defparameter *primitive-c-names*
  ;; 自己再帰の停止条件(カウントダウン等)を書くために最低限必要な算術/比較
  ;; プリミティブと、M10で導入するfuncall(lambdaが生成するクロージャ値を
  ;; 呼び出す唯一の手段)に対応する。これらはruntime.c/eval.cのprimitive_*が
  ;; 生成関数と同じABI(lisp_val_t fn(lisp_val_t args, lisp_val_t env))で
  ;; 既に実装済みのC関数で、呼び出し先アドレスはリンク時に確定するため、
  ;; defunされた関数と同じ「直接呼び出し」方式で扱える。primitive_funcallは
  ;; apply_function経由で汎用的にディスパッチするため、呼び出し先が
  ;; os_make_lifted_closureで作ったクロージャであってもこの1エントリで
  ;; そのまま対応できる(eval.c参照)
  '((- . "primitive_subtract")
    (eq . "primitive_eq")
    (funcall . "primitive_funcall")
    ;; M13: init.lispから移動するリスト操作関数(member/assoc/%append2/...)が
    ;; 使うcar/cdr/cons/nullに対応する。runtime.cのprimitive_car等はos_bootstrap内で
    ;; global_environmentへ既に登録済みのコア関数で、生成関数と同じABIを持つため
    ;; defunされた関数と同じ「直接呼び出し」方式で扱える
    (null . "primitive_null")
    (car . "primitive_car")
    (cdr . "primitive_cdr")
    (cons . "primitive_cons")
    ;; M14: create-list(%create-list-helper)が停止条件に使う数値等価比較
    (= . "primitive_num_equal")
    ;; M14: nreverse(%nreverse-helper)が破壊的な反転に使う
    (set-cdr . "primitive_set_cdr")
    ;; M14: setfの展開先(set-car/set-aref/set-elt)と、slot-value/set-slot-value
    ;; (%slot-index)が使う+・%%class-slots・%%instance-class・%%instance-slots
    (set-car . "primitive_set_car")
    (set-aref . "primitive_set_aref")
    (set-elt . "primitive_set_elt")
    (+ . "primitive_add")
    (%%class-slots . "primitive_class_slots")
    (%%instance-class . "primitive_instance_class")
    (%%instance-slots . "primitive_instance_slots")
    ;; M14: apply(&restの実引数リストを展開してfnを呼ぶ、eval.c側の組み込み関数)
    (%%apply . "primitive_apply")
    ;; M14: map-into(%map-into-min-length/%map-into-loop)が使う
    (length . "primitive_length")
    (elt . "primitive_elt")
    (< . "primitive_less_than")
    (>= . "primitive_greater_equal")
    ;; M14基盤D: for/whileの停止条件(test-and-result/test)が使う
    (> . "primitive_greater_than")
    ;; M14基盤E: with-open-input-stream/with-open-input-fileの展開先(バインディング
    ;; の初期値式、およびunwind-protectのcleanup)が使う
    (open-input-stream . "cc_open_input_stream")
    (close . "cc_close")
    ;; M14基盤E: with-open-input-streamがunwind-protect経由で必ずcloseすることを
    ;; テストfixtureから検証するために使う(cc_open_stream_p、stream_lisp.c:100)
    (open-stream-p . "cc_open_stream_p")
    ;; M14: with-open-output-fileの展開先(バインディングの初期値式)が使う
    (open-output-file . "cc_open_output_file")
    ;; [ファイルI/O]#50(M7): read-file-into-vector/write-vector-to-file
    ;; (file-cmd.lisp)が使う。いずれもstream_lisp.cのos_register_streamsで
    ;; 登録される生のC関数(AOT側にdefunが無い)ため、*known-function-names*では
    ;; 解決できずここへの追加が必要だった(open-input-stream等と同じ理由)
    (read-byte . "cc_read_byte")
    (write-byte . "cc_write_byte")
    (file-length . "cc_file_length")
    ;; M12基盤C(#27): %register-classが*classes*を更新するのに使う。defdynamic
    ;; はシンボル名キーの動的束縛なので、変数ごとの追加登録は不要
    (%%set-dynamic . "primitive_set_dynamic")
    ;; M12基盤E(#27): signal-conditionがcatchタグ用の一意なシンボルを作るのに使う
    (gensym . "primitive_gensym")
    ;; M12基盤F(#27): cerrorがcontinue-stringをobjs(&restの実引数、可変長)で
    ;; formatした文字列を作るのに使う。formatは可変長引数を取るため、cerrorの
    ;; 呼び出しは#'format(function特殊形式、下記transpile-expr参照)で関数値を
    ;; 取得し%%apply経由で(cons str (cons continue-string objs))を渡す
    (format . "cc_format")
    (create-string-output-stream . "cc_create_string_output_stream")
    (get-output-stream-string . "cc_get_output_stream_string")
    ;; M12基盤B(#27): %register-builtin-classがbootstrap用クラスオブジェクトを
    ;; 生成するのに使う(メタクラスは<built-in-class>)
    (%%make-builtin-class-raw . "primitive_make_builtin_class_raw")
    ;; M12基盤B(#27、計画通りだがPhase6まで未使用だったため実装時に追加): defclass
    ;; マクロは常にインタプリタ実行だが、AOT側のテストfixtureがmake-instanceの
    ;; 結線検証用にテストクラスを直接生成するのに使う
    (%%make-class-raw . "primitive_make_class_raw")
    ;; M12(#27): slot-valueがインスタンスのスロットベクタから値を読み出すのに使う
    (aref . "primitive_aref")
    ;; M12 Phase6(#27): make-instanceがインスタンスのスロットベクタを確保するのに使う
    (make-array . "primitive_make_array")
    ;; M12 Phase6(#27): make-instanceがクラス+スロットベクタから生インスタンスを
    ;; 生成するのに使う
    (%%make-instance-raw . "primitive_make_instance_raw")
    ;; M12(#27): subclasspがクラスの直接の親クラス一覧を辿るのに使う
    (%%class-supers . "primitive_class_supers")
    ;; M12基盤D(#27): class-ofが値の種別に応じてpredefinedクラスを解決するのに
    ;; 使う型判定8個。integerp(既存defun、本Phaseで移動)が使うfixnump/bignumpも
    ;; 含む
    (numberp . "primitive_numberp")
    (fixnump . "primitive_fixnump")
    (bignump . "primitive_bignump")
    (floatp . "primitive_floatp")
    (symbolp . "primitive_symbolp")
    (consp . "primitive_consp")
    (characterp . "primitive_characterp")
    (stringp . "primitive_stringp")
    (functionp . "primitive_functionp")
    (general-array*-p . "primitive_array_star_p")
    (general-vector-p . "primitive_general_vector_p")
    (streamp . "primitive_streamp")
    ;; M12(#27): class-ofがインスタンス/クラスオブジェクトの種別を判定するのに使う
    (%%standard-classp . "primitive_standard_classp")
    (%%builtin-classp . "primitive_builtin_classp")
    (%%class-instance-p . "primitive_class_instance_p")
    ;; M12(#27): typepがclass-designatorがクラスオブジェクトそのものかどうかを
    ;; 判定するのに使う
    (%%classp . "primitive_classp")
    ;; M12 Phase5(#27): %invoke-method-chain/call-next-methodが「no applicable
    ;; method」「no next method」を報告するのに使う。両者はerrorを呼びたいが、
    ;; errorはPhase9で移動予定でまだAOT側から名前解決できない(call-target-c-name
    ;; には*known-function-names*/*primitive-c-names*どちらにも載らない呼び出しは
    ;; 未対応エラーになる、フォールバック無し)。%%funcall-by-nameはos_get_function+
    ;; os_apply_functionで対象関数を実行時に名前解決して呼ぶため、Phaseの前後関係に
    ;; 関係なくinit.lisp常駐の関数を正しいセマンティクスのまま呼べる
    (%%funcall-by-name . "primitive_funcall_by_name")
    ;; room(utility.lisp)用: 実装済みだがこれまでAOT側から呼ばれていなかった
    ;; ランタイムプリミティブと、room自身が使う4つのバイト数アクセサ
    (create-string . "primitive_create_string")
    (string-elt . "primitive_string_elt")
    (string-append . "primitive_string_append")
    (div . "primitive_div")
    (mod . "primitive_mod")
    (open-output-stream . "cc_open_output_stream")
    (%%heap-total-bytes . "primitive_heap_total_bytes")
    (%%heap-used-bytes . "primitive_heap_used_bytes")
    (%%imm-space-total-bytes . "primitive_imm_space_total_bytes")
    (%%imm-space-used-bytes . "primitive_imm_space_used_bytes")
    (%%boot-alloc-used-bytes . "primitive_boot_alloc_used_bytes")
    ;; FAT16-M0(0)(documents/fs.md): logand/logior/logxor/ashが使う。subprimitive.c
    ;; にcc_in_8等と同じパターンで実装されたビット演算プリミティブ
    (%%logand . "cc_logand")
    (%%logior . "cc_logior")
    (%%logxor . "cc_logxor")
    (%%ash . "cc_ash")
    ;; FAT16-M0(1)(documents/fs.md): char-code/code-charが使う。文字とFIXNUMの
    ;; タグ付け替えのみを行うsubprimitive.cの関数
    (%%char-code . "cc_char_code")
    (%%code-char . "cc_code_char")
    ;; M15(documents/fs.md): device.lisp/fat16.lisp/fat32.lisp/file-cmd.lispが
    ;; 使う文字列/シンボル系プリミティブ。runtime.cのprimitive_*として既に
    ;; 実装済みでos_bootstrap内でglobal_environmentへ登録済み
    (subseq . "primitive_subseq")
    (char-index . "primitive_char_index")
    (string= . "primitive_string_equal")
    (symbol-name . "primitive_symbol_name")
    (string-to-symbol . "primitive_string_to_symbol")
    ;; M15: ide.lispがセクタバッファの読み書きに使う生メモリアクセス
    ;; (subprimitive.cにcc_in_8等と同じパターンで実装済み)
    (%%peek . "cc_peek")
    (%%poke . "cc_poke")
    ;; M15: ide.lispがIDEデバイスの列挙/セクタ読み書きに使う
    ;; (ide_subprimitive.cにcc_ide_*として実装済み、os_register_ide_subprimitives
    ;; 経由でglobal_environmentへ登録済み)
    (%%ide-device-count . "cc_ide_device_count")
    (%%ide-device-at . "cc_ide_device_at")
    (%%ide-device-name . "cc_ide_device_name")
    (%%ide-device-model . "cc_ide_device_model")
    (%%ide-sector-buffer-address . "cc_ide_sector_buffer_address")
    (%%ide-read-sector . "cc_ide_read_sector")
    (%%ide-write-sector . "cc_ide_write_sector")
    (%%ide-total-sectors . "cc_ide_total_sectors")
    ;; M15: device.lispがVolume ID(4byte little-endian)の合成に使う
    (* . "primitive_multiply")
    ;; M15: fat16.lisp/fat32.lispが削除済みエントリ(0xE5)判定等に使う
    (/= . "primitive_num_not_equal")
    (<= . "primitive_less_equal")
    ;; M15: ide.lispが%ide-bytes-to-addrのwhile条件に使う(NOTはnullと同じ
    ;; primitive_nullとしてos_bootstrap内で登録済み)
    (not . "primitive_null")
    ;; [ファイルI/O]#46(M3): %fat16-read-lba-list/%fat32-read-lba-list相当が
    ;; ファイル全体のバイト列バッファをconsリストではなくgeneral-vectorとして
    ;; 確保するのに使う(elt/set-elt/length/subseqは既に登録済み)
    (create-vector . "primitive_create_vector")))

(defun str->fn (fn-str)
  "文字列から関数オブジェクトを得る。関数が未定義だが alist に登録するための処置"
  (symbol-function (intern (string-upcase fn-str))))

(defparameter *macro-expanders*
  (list (cons 'let "expand-let")
        (cons 'let* "expand-let*")
        (cons 'cond "expand-cond")
        (cons 'case "expand-case")
        (cons 'case-using "expand-case-using")
        (cons 'setf "expand-setf")
        (cons 'for "expand-for")
        (cons 'while "expand-while")
        (cons 'with-open-input-stream "expand-with-open-input-stream")
        (cons 'with-open-input-file "expand-with-open-input-file")
        (cons 'with-open-output-stream "expand-with-open-output-stream")
        (cons 'with-open-output-file "expand-with-open-output-file")
        (cons 'defclass "expand-defclass")
        (cons 'defmethod "expand-defmethod"))
  "マクロ名(シンボル)から展開関数への alist。展開関数は元のフォーム全体
   (car=マクロ名を含む)を受け取り、展開後のフォームを返す。macroexpand-allが
   これを見てディスパッチする。各オペレータの実装コミットでここへ追加していく")

(defun sanitize-c-ident (name)
  "MEM-REF-64 -> mem_ref_64 (Cの識別子として使える形にする)。M14基盤D: for/while
   展開が使う%FOR-NEXT-VALUES等の%接頭辞(Lisp側の内部変数マーカーで、Cの識別子
   として不正な文字)も、!と同様に読み捨てる。M15: %fat16-8.3-field-bytes等、
   名前に'.'を含む関数名(Cの識別子として不正な文字)は'_'に変換し(-と同じ扱い)、
   %fat32-name-equal?等、名前に'?'を含む関数名は!と同様に読み捨てる"
  (remove-if (lambda (c) (member c '(#\! #\% #\?)))
             (substitute #\_ #\. (substitute #\_ #\- (string-downcase name)))))

(defun lisp-name-to-c-name (symbol)
  "MEMBER -> lisp_ll_member / %%mem-ref-64 -> lisp_ll_mem_ref_64。
   Lisp名の先頭に%系のマーカーが無いM13以降のような通常のトップレベル関数名
   (member/assoc/reverse等)も、必ずlisp_ll_を付けてCのグローバル名前空間へ
   出す(接頭辞無しのままだとlibc等の同名識別子と衝突しうるため。過去は
   全ファイルのdefunが%%transpile-fixture-接頭辞付きだったため露見しなかった)"
  (let* ((name (symbol-name symbol))
         (prefix-len (cond
                       ((and (>= (length name) 2)
                             (string= (subseq name 0 2) "%%"))
                        2)
                       ((and (plusp (length name))
                             (char= (char name 0) #\%))
                        1)
                       (t 0)))
         (body (sanitize-c-ident (subseq name prefix-len))))
    (concatenate 'string "lisp_ll_" body)))

(defparameter *c-reserved-words*
  '("auto" "break" "case" "char" "const" "continue" "default" "do" "double"
    "else" "enum" "extern" "float" "for" "goto" "if" "inline" "int" "long"
    "register" "restrict" "return" "short" "signed" "sizeof" "static" "struct"
    "switch" "typedef" "union" "unsigned" "void" "volatile" "while" "_Bool"
    "_Complex" "_Imaginary" "env" "nil")
  "Cの予約語(+このファイルが生成するコードが暗黙に使うenv/nil)。M15:
   %plist-getのdefaultパラメータのように、defunパラメータ名がそのままでは
   Cの識別子として使えない場合があるため、param-symbol-to-c-nameがこれと
   衝突する場合に接尾辞を付けて回避する")

(defun param-symbol-to-c-name (symbol)
  "defunパラメータ名 -> Cのローカル変数名。プレフィックスは付けないが、
   Cの予約語(*c-reserved-words*)と衝突する場合のみ_paramを付けて回避する"
  (let ((name (sanitize-c-ident (symbol-name symbol))))
    (if (member name *c-reserved-words* :test #'string=)
        (concatenate 'string name "_param")
        name)))

(defun c-string-literal (s)
  "CommonLisp文字列からCの文字列リテラル(ダブルクオート込み)を作る。
   \\と\"のみエスケープする(現時点のfixtureはASCII識別子文字列のみのため十分)"
  (with-output-to-string (out)
    (write-char #\" out)
    (loop for ch across s
          do (when (or (char= ch #\\) (char= ch #\"))
               (write-char #\\ out))
             (write-char ch out))
    (write-char #\" out)))

(defun c-char-literal (ch)
  "CommonLisp文字からCのchar literal(シングルクオート込み)を作る。\\と'の
   みエスケープする(M15: fat16.lisp/fat32.lisp/file-cmd.lispが使うのは
   '/'や'.'等のASCII印字可能文字のみのため十分)"
  (cond
    ((char= ch #\\) "'\\\\'")
    ((char= ch #\') "'\\''")
    (t (format nil "'~A'" ch))))

;;; M10: lambdaの自由変数解析。let等の追加束縛フォームが無い現時点では、
;;; 式木の中で新たに変数を束縛できるのはlambda自身のパラメータだけなので、
;;; BOUND(既に束縛済みのシンボルのリスト)を引数で辿るだけでスコープを追跡できる。
;;; quoteはリテラルデータなので中身を式として解釈しない。呼び出し形式
;;; (name arg*)/if/progn/setq/and/or はいずれも(car expr)がシンボルで
;;; (cdr expr)が処理すべき部分式のリストという共通の形をしているため、
;;; lambda/quote以外は特別扱いせず(cdr expr)を再帰的に処理するだけでよい
;;; (carのシンボル自身は呼び出し先名やsetqの対象決定など別の場所で解決するため、
;;; ここでは変数参照として扱わない=自然にプリミティブ名/defun名を除外できる)

(defun tagbody-tag-p (elem)
  "tagbodyの本体要素のうち、goの飛び先になるタグ(nilでない裸シンボル)かどうかを
   判定する。インタプリタのis_tagbody_tag(eval.c)は実行時の値の型で判定するが、
   トランスパイラはtagbodyの本体をコンパイル時のリストとして持っているため、
   構文上の形(consでない、かつnilでないシンボル)だけで静的に判定できる"
  (and elem (symbolp elem)))

(defun free-variables (expr bound)
  "EXPR中の裸シンボル参照のうち、BOUND(シンボルのリスト)に含まれないものを
   重複なく集めて返す。lambda式に出会った場合はそのパラメータをBOUNDに加えて
   本体を再帰的に走査するため、ネストしたlambdaがさらに内側でしか使わない
   変数は自然に除外され、外側からの捕捉が必要な自由変数だけが伝播する。
   M14基盤D: go/return-from/block/tagbodyのタグ名・block名は変数参照ではなく
   ラベルなので、汎用の(cdr expr)走査に落とすと誤って自由変数と見なされてしまう
   ため、これらは専用の分岐でタグ/名前自身を素通しし、値・本体だけを再帰的に
   走査する。M12基盤F(#27): (function name)の第二要素も同様に変数参照ではなく
   呼び出し先名(call-target-c-nameが解決する)なので専用の分岐で素通しする"
  (cond
    ((integerp expr) nil)
    ((stringp expr) nil)
    ((null expr) nil)
    ((eq expr t) nil)
    ((symbolp expr) (if (member expr bound) nil (list expr)))
    ((and (consp expr) (eq (car expr) 'quote)) nil)
    ((and (consp expr) (eq (car expr) 'function) (= (length expr) 2) (symbolp (second expr))) nil)
    ((and (consp expr) (eq (car expr) 'dynamic) (= (length expr) 2)) nil)
    ((and (consp expr) (eq (car expr) 'defdynamic) (= (length expr) 3))
     (free-variables (third expr) bound))
    ((and (consp expr) (eq (car expr) 'lambda) (= (length expr) 3))
     (free-variables (third expr) (append (second expr) bound)))
    ((and (consp expr) (eq (car expr) 'go) (= (length expr) 2)) nil)
    ((and (consp expr) (eq (car expr) 'return-from))
     (if (cddr expr) (free-variables (third expr) bound) nil))
    ((and (consp expr) (eq (car expr) 'block))
     (reduce #'union (mapcar (lambda (e) (free-variables e bound)) (cddr expr))
             :initial-value nil))
    ((and (consp expr) (eq (car expr) 'tagbody))
     (reduce #'union
             (mapcar (lambda (e) (if (tagbody-tag-p e) nil (free-variables e bound)))
                     (cdr expr))
             :initial-value nil))
    ((consp expr)
     ;; let/let*の展開((lambda (vars) body) inits)のように演算子位置に直接
     ;; lambda式が来る即時呼び出しでは、(car expr)自体も自由変数を持ちうる
     ;; (このケースだけexpr全体を、通常の(name arg*)呼び出しでは関数名を除いた
     ;; (cdr expr)だけを走査する)
     (reduce #'union
             (mapcar (lambda (e) (free-variables e bound))
                     (if (consp (car expr)) expr (cdr expr)))
             :initial-value nil))
    (t nil)))

(defun setq-targets (expr bound)
  "EXPR中でsetqの対象になっている変数のうち、BOUNDに含まれないものを重複なく
   集めて返す(free-variablesと同じBOUND伝播規則でlambdaのネストを辿るため、
   ネストしたlambdaの内部で行われるsetqも見つかる)。この関数はboxに昇格すべき
   パラメータを判定するために、あるパラメータが自分の本体のどこか(直接でも
   ネストしたlambda経由でも)でsetqされるかどうかを調べる目的で使う"
  (cond
    ((atom expr) nil)
    ((eq (car expr) 'quote) nil)
    ((and (eq (car expr) 'dynamic) (= (length expr) 2)) nil)
    ((and (eq (car expr) 'defdynamic) (= (length expr) 3))
     (setq-targets (third expr) bound))
    ((and (eq (car expr) 'lambda) (= (length expr) 3))
     (setq-targets (third expr) (append (second expr) bound)))
    ((and (eq (car expr) 'setq) (= (length expr) 3))
     (union (if (member (second expr) bound) nil (list (second expr)))
            (setq-targets (third expr) bound)))
    (t (reduce #'union
               (mapcar (lambda (e) (setq-targets e bound))
                       (if (consp (car expr)) expr (cdr expr)))
               :initial-value nil))))

(defun find-lambda-forms (expr)
  "EXPR中に直接現れるlambda式を集める。見つけたlambdaの内部(そのbody)までは
   辿らない: ネストしたlambdaが外側の変数を必要とする場合、それはfree-variables
   がそのネストしたlambdaを直接囲むlambdaの自由変数として再帰的に伝播させるため、
   ここで別途掘り進める必要が無い(captured-params参照)。let/let*展開の
   ((lambda (vars) body) inits)のように演算子位置に直接lambda式が来る場合は
   (car expr)も含めて走査しないと、その場に直接ネストしたlambdaを見落とす"
  (cond
    ((atom expr) nil)
    ((eq (car expr) 'quote) nil)
    ((and (eq (car expr) 'lambda) (= (length expr) 3)) (list expr))
    (t (reduce #'append
               (mapcar #'find-lambda-forms (if (consp (car expr)) expr (cdr expr)))
               :initial-value nil))))

(defun captured-params (body params)
  "BODY中のどこかにネストして現れるlambdaが自由変数として必要とする変数のうち、
   PARAMS(この関数自身のパラメータ)に含まれるものを集めて返す(=このパラメータは
   何らかのネストしたlambdaに捕捉される、という判定)。find-lambda-formsで見つけた
   直接ネストのlambdaそれぞれについて、そのlambda自身のパラメータだけをBOUNDとした
   free-variablesを取ることで、より深くネストしたlambdaの要求も(free-variables自身の
   lambda特別扱いにより)自動的に伝播して含まれる"
  (intersection params
                (reduce #'union
                        (mapcar (lambda (l) (free-variables (third l) (second l)))
                                (find-lambda-forms body))
                        :initial-value nil)))

(defun boxed-params (body params)
  "PARAMSのうち、za.cの拡張4と同じ基準(setqされ、かつエスケープするlambdaに
   捕捉される変数だけをbox化する)でboxへ昇格すべきものを返す"
  (intersection (setq-targets body nil) (captured-params body params)))

;;; M14: マクロ展開。init.lispのlet/cond/case/for/while/setf/with-open-*は全て
;;; defmacroだが、init.lispには(defpackage/in-package)によるパッケージ分離が無いため、
;;; これらをそのままホストSBCLへ(defmacro let ...)としてloadするとCL:LET等の
;;; 特殊形式/標準マクロと名前衝突し"The special operator LET can't be redefined
;;; as a macro."になる(M5/issue #20で検証済み・documents/transpiler.md参照)。
;;; そのため、マクロ名と衝突しない通常の関数(expand-let等)としてマクロ本体を
;;; ポートし、以下のmacroexpand-allという自前の再帰コードウォーカーから
;;; *macro-expanders*(マクロ名->展開関数のalist)経由で呼び出す方式を採る。
;;; gensymはホスト側SBCL標準のものを使う(展開時にのみ使う一意シンボルなので、
;;; ランタイム側のgensymと意味的に完全に等価)。

(defun macroexpand-all (form)
  "FORM中に現れるマクロ呼び出し(*macro-expanders*に載っているもの)を再帰的に
   展開して、コアフォーム(if/progn/setq/and/or/lambda/dynamic/defdynamic+呼び出し)
   だけになるまで畳み込む。quoteは不透過データなので中身を展開しない。lambdaは
   パラメータリストに触れずbodyのみ展開する。dynamicは第2引数(未評価のシンボル
   リテラル)に触れず、defdynamicは第3引数(値を計算する式)のみ展開する(いずれも
   free-variables/setq-targetsの特別扱いと同じ理由)。マクロ呼び出しが見つかった
   場合は展開関数を1回呼んだ後、展開結果を再度macroexpand-allに通す(let*が
   letへ展開するなど、展開結果がさらにマクロ呼び出しを含みうるため)"
  (cond
    ((atom form) form)
    ((eq (car form) 'quote) form)
    ((and (eq (car form) 'dynamic) (= (length form) 2)) form)
    ((and (eq (car form) 'defdynamic) (= (length form) 3))
     (list (first form) (second form) (macroexpand-all (third form))))
    ((and (eq (car form) 'lambda) (= (length form) 3))
     (list (first form) (second form) (macroexpand-all (third form))))
    ((and (symbolp (car form)) (assoc (car form) *macro-expanders*))
     (macroexpand-all (funcall (str->fn (cdr (assoc (car form) *macro-expanders*))) form)))
    (t (cons (macroexpand-all (car form)) (mapcar #'macroexpand-all (cdr form))))))


(defun call-target-c-name (name)
  "呼び出し先シンボル名からC関数名を解決する。このファイル内でdefunされた
   関数名(*known-function-names*、mainがdefun走査後に束縛する)を優先し、
   次に算術/比較プリミティブのホワイトリスト(*primitive-c-names*)を見る。
   za.cのようなランタイム上のシンボル->Function-Cell解決は行わず、AOTで
   リンクされるC関数を名前で直接呼び出す(呼び出し先アドレスはリンク時に確定
   するため、定義順に関わらず相互再帰も解決できる)"
  (cond
    ((member name *known-function-names*) (lisp-name-to-c-name name))
    ((assoc name *primitive-c-names*) (cdr (assoc name *primitive-c-names*)))
    (t (error "transpile-call: 未対応の呼び出し先です: ~S" name))))

(defparameter *primitive-fixed-arity-c-names*
  ;; ABI-M3: *primitive-c-names*に載っているn項/可変長版の呼び出し先のうち、
  ;; ABI-M1/M2(documents/abi-redesign.md)でza.c向けにruntime.h/runtime.cへ
  ;; 追加した固定引数版(_1/_2サフィックス、またはcar/cdr/consのように
  ;; cc_car/cc_cdr/os_make_consを直接使えるもの)へ、実引数の個数がちょうど
  ;; 一致する呼び出しに限って直接callできるようにする対応表。(name arity . c-name)
  ;; の形。個数が一致しない呼び出し(例: (+ a b c)というn項和や単項の(- x))は
  ;; ここに載らず*primitive-c-names*側のn項版へ従来通りフォールバックする
  ;; (transpile-call参照)。固定引数版はいずれも「argsをconsリストとして
  ;; 受け取らない」ため、呼び出し側もconsリスト構築(transpile-cons-chain)を
  ;; 経由しない
  '((car 1 . "cc_car")
    (cdr 1 . "cc_cdr")
    (cons 2 . "os_make_cons")
    (eq 2 . "primitive_eq2")
    (null 1 . "primitive_null1")
    (= 2 . "primitive_num_equal2")
    (< 2 . "primitive_less_than2")
    (> 2 . "primitive_greater_than2")
    (>= 2 . "primitive_greater_equal2")
    (+ 2 . "primitive_add2")
    (- 2 . "primitive_subtract2")
    (set-car 2 . "primitive_set_car2")
    (set-cdr 2 . "primitive_set_cdr2")
    (numberp 1 . "primitive_numberp1")
    (fixnump 1 . "primitive_fixnump1")
    (bignump 1 . "primitive_bignump1")
    (floatp 1 . "primitive_floatp1")
    (symbolp 1 . "primitive_symbolp1")
    (consp 1 . "primitive_consp1")
    (characterp 1 . "primitive_characterp1")
    (stringp 1 . "primitive_stringp1")
    (functionp 1 . "primitive_functionp1")
    (streamp 1 . "primitive_streamp1")
    ;; read-file-into-vector(file-cmd.lisp)のような1byteずつread-byte/set-eltを
    ;; 呼ぶホットループが、バイトごとにconsセルを構築するコストを避けるために追加
    ;; (性能調査で判明。ABI-M1/M2で車輪の横展開をした際にはvector/streamの
    ;; プリミティブまでは対象にしていなかった)。
    (elt 2 . "primitive_elt2")
    (set-elt 3 . "primitive_set_elt3")
    (read-byte 1 . "cc_read_byte1")))

(defun primitive-fixed-arity-c-name (name argc)
  "NAMEが*primitive-fixed-arity-c-names*に載っており、かつ実引数個数ARGCが
   その固定arityと一致する場合、対応するC関数名を返す。一致しなければnil
   (transpile-callがconsチェーン経由の従来呼び出しへフォールバックする合図)"
  (let ((entry (assoc name *primitive-fixed-arity-c-names*)))
    (if (and entry (= (cadr entry) argc))
        (cddr entry)
        nil)))

(defun known-function-fixed-arity (name)
  "ABI-M6: NAMEが*known-function-names*内で&restパラメータを持たないdefunの
   場合そのパラメータ数、&restを持つ場合・未知の場合・パラメータ0個の場合は
   nilを返す(0個は元々consチェーンを構築しない0引数呼び出しに対し新たな
   エントリを増やす利点が無いため対象外とする)。transpile-call/
   transpile-prototype/transpile-defunがname__fixedエントリの有無を判定する
   のに使う。*known-function-arities*はmainが束縛する"
  (let ((arity (cdr (assoc name *known-function-arities*))))
    (and arity (> arity 0) arity)))

(defun fixed-entry-param-c-names (arity)
  "ARITY個の`lisp_val_t`引数名(__arg0, __arg1, ...)のリストを返す。
   name__fixedのCシグネチャ生成(transpile-prototype/emit-function-body)と
   呼び出し側(transpile-call)のいずれからも参照する共通の命名規則"
  (loop for i from 0 below arity collect (format nil "__arg~D" i)))


;;; let/let*: init.lispのdefmacro let/let*(%let-vars/%let-inits)と同じ展開規則。
;;; bodyは&restで複数式を許すが、transpile-lambda/transpile-defunは単一の本体式
;;; のみ対応するため、init.lispの`,@body`をそのまま展開先に置く代わりに
;;; (progn ,@body)で1式に畳み込む(prognは既存サポート済みで、複数式のうち
;;; 最後の式の値を残す評価順もletのbody評価順と一致する)。

(defun %%let-vars (bindings)
  (if (null bindings) nil (cons (car (car bindings)) (%%let-vars (cdr bindings)))))

(defun %%let-inits (bindings)
  (if (null bindings) nil (cons (car (cdr (car bindings))) (%%let-inits (cdr bindings)))))

(defun expand-let (form)
  (destructuring-bind (let-kw bindings &rest body) form
    (declare (ignore let-kw))
    `((lambda ,(%%let-vars bindings) (progn ,@body)) ,@(%%let-inits bindings))))

(defun expand-let* (form)
  (destructuring-bind (let*-kw bindings &rest body) form
    (declare (ignore let*-kw))
    (if (null bindings)
        `(progn ,@body)
        `(let (,(car bindings))
           (let* ,(cdr bindings) ,@body)))))

;;; cond: init.lispのdefmacro cond(%%case-expandとは別物)と同じ展開規則。
;;; 1段展開すると結果に(cond ,@(cdr clauses))というcond自身の再帰呼び出しが
;;; 残るが、macroexpand-allが展開結果を再度macroexpand-allに通す(let*と同じ
;;; 理由)ため、clausesが尽きるまで自動的に繰り返し展開される

(defun expand-cond (form)
  (let ((clauses (cdr form)))
    (if (null clauses)
        nil
        `(if ,(car (car clauses))
             (progn ,@(cdr (car clauses)))
             (cond ,@(cdr clauses))))))

;;; case: init.lispのdefmacro case(%case-expand)と同じ展開規則。t節以外の
;;; clauseはkeylistをquoteしたまま(member key '(...))へ、t節は素通しでprognへ
;;; 展開するif連鎖。keylistのquoteはtranspile-quotedがリスト(cons)対応する
;;; 必要があるため、このコミットでtranspile-quotedへcons分岐を追加する。

(defun %%case-expand (key clauses)
  (if (null clauses)
      nil
      (if (eq (car (car clauses)) t)
          `(progn ,@(cdr (car clauses)))
          `(if (member ,key ',(car (car clauses)))
               (progn ,@(cdr (car clauses)))
               ,(%%case-expand key (cdr clauses))))))

(defun expand-case (form)
  (destructuring-bind (case-kw keyform &rest clauses) form
    (declare (ignore case-kw))
    (let ((key (gensym)))
      `(let ((,key ,keyform))
         ,(%%case-expand key clauses)))))

;;; case-using: init.lispのdefmacro case-using(%case-using-expand)と同じ展開
;;; 規則。実行時の述語呼び出しは%case-using-match(init_aot.lispへ移動する既知
;;; 関数)へ委譲するため、caseと違いmemberを直接埋め込むのではなく関数呼び出しを
;;; 生成する。

(defun %%case-using-expand (pred key clauses)
  (if (null clauses)
      nil
      (if (eq (car (car clauses)) t)
          `(progn ,@(cdr (car clauses)))
          `(if (%case-using-match ,pred ,key ',(car (car clauses)))
               (progn ,@(cdr (car clauses)))
               ,(%%case-using-expand pred key (cdr clauses))))))

(defun expand-case-using (form)
  (destructuring-bind (case-using-kw predform keyform &rest clauses) form
    (declare (ignore case-using-kw))
    (let ((pred (gensym)) (key (gensym)))
      `(let ((,pred ,predform) (,key ,keyform))
         ,(%%case-using-expand pred key clauses)))))

;;; setf: init.lispのdefmacro setfと同じ展開規則。placeの形に応じてsetq/
;;; set-car/set-cdr/set-aref/set-elt/set-slot-value/set-propertyへ展開する。
;;; このうちset-property(%check-symbol-arg経由でerrorを呼ぶ)はILOSの
;;; condition system(M12)が無いと呼び出し先を解決できずtranspile時に
;;; エラーとなるため、(setf (property ...) ...)は現時点では未対応のまま
;;; (呼び出し先解決時にcall-target-c-nameが明示的なエラーで検出する)。
;;; car/cdr/aref/elt/slot-valueの5形式は本コミットで対応する。

(defun expand-setf (form)
  (destructuring-bind (setf-kw place value) form
    (declare (ignore setf-kw))
    (cond ((symbolp place) `(setq ,place ,value))
          ((eq (car place) 'car) `(set-car ,(car (cdr place)) ,value))
          ((eq (car place) 'cdr) `(set-cdr ,(car (cdr place)) ,value))
          ((eq (car place) 'aref) `(set-aref ,@(cdr place) ,value))
          ((eq (car place) 'elt) `(set-elt ,value ,@(cdr place)))
          ((eq (car place) 'slot-value) `(set-slot-value ,@(cdr place) ,value))
          ((eq (car place) 'property) `(set-property ,value ,@(cdr place))))))

;;; for/while: init.lispのdefmacro for/while(%for-vars/%for-inits/%for-next/
;;; %for-nexts/%for-let-bindings/%for-setqs)と同じ展開規則。init.lisp側の同名
;;; ヘルパーと衝突しないよう%%接頭辞でポートする(%%let-vars等と同じ命名規則)。
;;; 展開結果はlet(基盤A)・if/progn/setqと、本コミットで追加するblock/
;;; return-from/tagbody/go(基盤D)のみに帰着する。tagbody/goのタグ
;;; (%for-loop/%while-loop)はこのform内だけで解決される局所的な識別子であり、
;;; go/tagbody自体はマクロではないため、forが複数ネストしても新たなシンボルは
;;; 増えない(init.lisp:159-163のコメントと同じ理由)。

(defun %%for-vars (bindings)
  (if (null bindings) nil (cons (car (car bindings)) (%%for-vars (cdr bindings)))))

(defun %%for-inits (bindings)
  (if (null bindings) nil (cons (car (cdr (car bindings))) (%%for-inits (cdr bindings)))))

(defun %%for-next (binding)
  "step式を省略したbinding (var init) は、次の値も現在値のまま(変化なし)とする"
  (if (null (cdr (cdr binding)))
      (car binding)
      (car (cdr (cdr binding)))))

(defun %%for-nexts (bindings)
  (if (null bindings) nil (cons (%%for-next (car bindings)) (%%for-nexts (cdr bindings)))))

(defun %%for-let-bindings (bindings)
  (if (null bindings)
      nil
      (cons (list (car (car bindings)) (car (cdr (car bindings))))
            (%%for-let-bindings (cdr bindings)))))

(defun %%for-setqs (bindings list-expr)
  (if (null bindings)
      nil
      (cons `(setq ,(car (car bindings)) (car ,list-expr))
            (%%for-setqs (cdr bindings) `(cdr ,list-expr)))))

(defun expand-for (form)
  (destructuring-bind (for-kw bindings test-and-result &rest body) form
    (declare (ignore for-kw))
    `(let ,(%%for-let-bindings bindings)
       (block nil
         (tagbody
          %for-loop
          (if ,(car test-and-result)
              (return-from nil (progn ,@(cdr test-and-result)))
              (progn
                ,@body
                (let ((%for-next-values (list ,@(%%for-nexts bindings))))
                  ,@(%%for-setqs bindings '%for-next-values))
                (go %for-loop))))))))

(defun expand-while (form)
  (destructuring-bind (while-kw test &rest body) form
    (declare (ignore while-kw))
    `(block nil
       (tagbody
        %while-loop
        (if ,test
            (progn ,@body (go %while-loop))
            nil)))))

;;; with-open-input-stream/with-open-input-file: init.lispのdefmacro
;;; with-open-input-stream/with-open-input-file(init.lisp:195-210)と同じ展開規則。
;;; 展開結果はlet(基盤A)・unwind-protect(基盤E、本コミットで追加)・
;;; open-input-stream/closeへの呼び出し(*primitive-c-names*)のみに帰着する。
;;; with-open-input-fileはopen-input-stream呼び出し(open-input-fileという
;;; 別名ではなく、init.lispのマクロ本体と同じくopen-input-stream自身)を
;;; with-open-input-streamへ差し込むだけの薄いラッパー。

(defun expand-with-open-input-stream (form)
  (destructuring-bind (kw binding &rest body) form
    (declare (ignore kw))
    `(let ((,(car binding) ,(car (cdr binding))))
       (unwind-protect
           (progn ,@body)
         (close ,(car binding))))))

(defun expand-with-open-input-file (form)
  (destructuring-bind (kw binding &rest body) form
    (declare (ignore kw))
    `(with-open-input-stream (,(car binding) (open-input-stream ,(car (cdr binding))))
       ,@body)))

;;; with-open-output-stream/with-open-output-file: init.lispのdefmacro
;;; with-open-output-stream/with-open-output-file(init.lisp:223-235)と同じ展開規則。
;;; with-open-input-stream/with-open-input-fileと同型(基盤Eは既に導入済みのため
;;; 新規基盤は不要)。with-open-output-fileはopen-output-file呼び出しを
;;; with-open-output-streamへ差し込むだけの薄いラッパー。

(defun expand-with-open-output-stream (form)
  (destructuring-bind (kw binding &rest body) form
    (declare (ignore kw))
    `(let ((,(car binding) ,(car (cdr binding))))
       (unwind-protect
           (progn ,@body)
         (close ,(car binding))))))

(defun expand-with-open-output-file (form)
  (destructuring-bind (kw binding &rest body) form
    (declare (ignore kw))
    `(with-open-output-stream (,(car binding) (open-output-file ,(car (cdr binding))))
       ,@body)))

;;; defclass: init.lispのdefmacro defclass(init.lisp参照)と同じ展開規則。
;;; supers/slot-specsはマクロなので未評価のまま渡される。%slot-spec-form(s)/
;;; %plist-getは展開結果の(list ,@...)部分をunquote-spliceで組み立てるための
;;; 「マクロ展開時に(このホストSBCL上で)呼ばれる」ヘルパーなので、%%let-vars等と
;;; 同じくホスト関数として移植する(init.lisp常駐の同名関数とは別物、衝突しない)。
;;; 一方、展開結果に直接埋め込まれる%defclass-supers/%register-class/
;;; %merge-superclass-slots/%%make-class-rawは「実行時に呼ばれる」コードなので、
;;; init_aot.lispへ移動済みの関数/プリミティブとして解決される(call-target-c-name)。

(defun %%defclass-plist-get (plist key default)
  (if (null plist)
      default
      (if (eq (car plist) key)
          (car (cdr plist))
          (%%defclass-plist-get (cdr (cdr plist)) key default))))

(defun %%defclass-slot-spec-form (spec)
  (list 'list (list 'quote (car spec))
              (list 'quote (%%defclass-plist-get (cdr spec) :initarg nil))
              (list 'lambda nil (%%defclass-plist-get (cdr spec) :initform nil))))

(defun %%defclass-slot-spec-forms (specs)
  (if (null specs)
      nil
      (cons (%%defclass-slot-spec-form (car specs)) (%%defclass-slot-spec-forms (cdr specs)))))

(defun expand-defclass (form)
  (destructuring-bind (defclass-kw name supers slot-specs &rest options) form
    (declare (ignore defclass-kw options))
    `(let ((%supers (%defclass-supers ',supers)))
       (%register-class ',name
         (%%make-class-raw ',name %supers
           (append (%merge-superclass-slots %supers)
                   (list ,@(%%defclass-slot-spec-forms slot-specs))))))))


;;; defgeneric/defmethod: init.lispのdefmacro defgeneric/defmethod(init.lisp
;;; 436-437行目/474-477行目)と同じ展開規則をホスト側に移植する。総称関数
;;; ディスパッチの実行時基盤(%register-method/%generic-call/%applicable-methods/
;;; %order-methods/%invoke-method-chain/next-method-p/call-next-method)は
;;; 既にinit_aot.lispへ移動済み(M12 Phase5、#27)の普通のdefunであり、AOT側からも
;;; 呼べる状態にある。そのためここで新規に追加するのはマクロ展開のグルーだけで
;;; よい。interpreter側の(class ClassName)マクロは移植せず、既にAOT defunの
;;; %find-classを直接呼ぶ(%find-class (quote ClassName))という形をemitする点だけが
;;; interpreter版のdefmethod展開と異なる。
;;;
;;; defgenericの展開結果は(defclassの%register-class呼び出しと違って)ただの式では
;;; なく普通のdefunなので、main側でtoplevel-defun-p判定(car formが'defunかどうかの
;;; 生form判定)を行う前に先行展開しておく必要がある(下記%%expand-defgeneric-form/
;;; %%expand-defgenerics-in-forms、mainから呼ぶ)。defmethodの展開結果は
;;; %register-methodへのトップレベル呼び出し(defclassの%register-class呼び出しと
;;; 同じ形の、ただの式)なので、*macro-expanders*経由でtranspile-toplevel-form/
;;; emit-toplevel-forms-runnerに素直に乗る(fat16.lisp等、本番ファイルでの
;;; 想定用途)。

(defun %%first-param-specializer (param)
  (if (consp param) (car (cdr param)) nil))

(defun %%first-param-var (param)
  (if (consp param) (car param) param))

(defun %%lambda-list-specializers (lambda-list)
  (if (null lambda-list)
      nil
      (cons (%%first-param-specializer (car lambda-list))
            (%%lambda-list-specializers (cdr lambda-list)))))

(defun %%method-plain-params (lambda-list)
  (if (null lambda-list)
      nil
      (cons (%%first-param-var (car lambda-list))
            (%%method-plain-params (cdr lambda-list)))))

(defun %%specializer-forms (names)
  (if (null names)
      nil
      (cons (if (car names) (list '%find-class (list 'quote (car names))) nil)
            (%%specializer-forms (cdr names)))))

(defun expand-defgeneric (form)
  (destructuring-bind (defgeneric-kw name lambda-list &rest options) form
    (declare (ignore defgeneric-kw lambda-list options))
    `(defun ,name (&rest %generic-args) (%generic-call ',name %generic-args))))

(defun expand-defmethod (form)
  (destructuring-bind (defmethod-kw name lambda-list &rest body) form
    (declare (ignore defmethod-kw))
    `(%register-method ',name
       (list ,@(%%specializer-forms (%%lambda-list-specializers lambda-list)))
       (lambda ,(%%method-plain-params lambda-list)
         ,(if (= (length body) 1) (car body) `(progn ,@body))))))

(defun %%expand-defgeneric-form (form)
  "formがdefgenericならこの1formをdefunへ展開し、それ以外はformをそのまま返す
   (main側でtoplevel-defun-p判定を行う前に、ファイル内の全formへmapcarで適用する
   プリパス用のヘルパー)"
  (if (and (consp form) (eq (car form) 'defgeneric))
      (expand-defgeneric form)
      form))

(defun %%expand-defgenerics-in-forms (forms)
  (mapcar #'%%expand-defgeneric-form forms))


(declaim (ftype function transpile-quoted))
(declaim (ftype function transpile-if))
(declaim (ftype function transpile-progn))
(declaim (ftype function transpile-setq))
(declaim (ftype function transpile-and))
(declaim (ftype function transpile-or))
(declaim (ftype function transpile-dynamic-read))
(declaim (ftype function transpile-defdynamic))
(declaim (ftype function transpile-call))
(declaim (ftype function transpile-lambda))
(declaim (ftype function transpile-tail-stmt))
(declaim (ftype function transpile-tail-and))
(declaim (ftype function transpile-tail-or))
(declaim (ftype function transpile-tail-call-args-guarded))
(declaim (ftype function transpile-tail-call))
(declaim (ftype function transpile-go))
(declaim (ftype function transpile-return-from))
(declaim (ftype function transpile-block))
(declaim (ftype function transpile-tagbody))
(declaim (ftype function transpile-unwind-protect))
(declaim (ftype function transpile-catch))
(declaim (ftype function transpile-throw))


(defun transpile-expr (expr &optional scope)
  "fixnum/string/nil/tの裸リテラルと、それらに対するquote、シンボルのquote、
   および束縛済みパラメータの参照(scope内のシンボル)に対応する。GCで移動しうる
   値(symbol/string)は、za.cのT/quoteシンボルリテラル対応と同じく、生ポインタを
   埋め込まずos_make_symbol/os_make_stringを都度呼んで解決する。nilはランタイムが
   GCで移動しない固定センチネルなのでexternグローバルnilをそのまま参照してよい。
   scopeはシンボルからCのローカル変数の情報(c-name . boxed-p)へのalist。
  boxed-pがnon-nilの変数は、値そのものではなくbox((捨て値 . 実値)のcons)を
  c-nameが指すため、読み出しはcc_cdrを経由する(M10のbox化パラメータ/自由変数、
  transpile-defun/transpile-lambda参照)"
  (cond
    ((integerp expr)
     ;; M15調査で発覚: 負の整数リテラル(例: (ash n -8)の-8)を単純に
     ;; os_make_fixnum(-8ULL)と出力すると、Cの単項マイナスがunsigned long long
     ;; リテラルへ適用され2の補数の巨大な正の値になり、符号無しのos_make_fixnumへ
     ;; そのまま渡ると本来の値と無関係なタグ付き値になる(runtime.hのos_make_fixnumは
     ;; 非負専用、符号付きの値はos_make_fixnum_signedを使う契約)。負の場合のみ
     ;; magnitude(絶対値)を渡すos_make_fixnum_signedへ切り替える
     (if (< expr 0)
         (format nil "os_make_fixnum_signed(1, ~AULL)" (- expr))
         (format nil "os_make_fixnum(~AULL)" expr)))
    ((stringp expr)
     (format nil "os_make_string(~A)" (c-string-literal expr)))
    ((null expr) "nil")
    ((eq expr t) "g_sym_t")
    ((characterp expr)
     ;; M15: fat16.lisp/fat32.lisp/file-cmd.lispが使う#\/等の文字リテラル。
     ;; リーダは既にTAG_CHARの値へパース済み(reader.c)なので、os_make_charで
     ;; そのままタグ付けするだけでよい
     (format nil "os_make_char(~A)" (c-char-literal expr)))
    ((and (symbolp expr) (assoc expr scope))
     (let ((binding (cdr (assoc expr scope))))
       (if (cdr binding)
           (format nil "cc_cdr(~A)" (car binding))
           (car binding))))
    ((and (consp expr) (eq (car expr) 'quote) (= (length expr) 2))
     (transpile-quoted (second expr)))
    ((and (consp expr) (eq (car expr) 'function) (= (length expr) 2) (symbolp (second expr)))
     ;; M12基盤F(#27): #'format(cerrorが%%apply経由でformatを可変長引数付きで
     ;; 呼ぶのに使う)。call-target-c-nameで解決したC関数を、呼び出さずに
     ;; os_make_native_functionで第一級の関数値として包む。defunされた関数
     ;; 名/プリミティブのホワイトリストいずれも標準ABI(evaluated_args, env)を
     ;; 持つため、lambdaと違い自由変数の捕捉は不要
     (format nil "os_make_native_function((lisp_addr_t)(void *)~A)" (call-target-c-name (second expr))))
    ((and (consp expr) (eq (car expr) 'if))
     (transpile-if expr scope))
    ((and (consp expr) (eq (car expr) 'progn))
     (transpile-progn expr scope))
    ((and (consp expr) (eq (car expr) 'setq) (= (length expr) 3))
     (transpile-setq expr scope))
    ((and (consp expr) (eq (car expr) 'and))
     (transpile-and (cdr expr) scope))
    ((and (consp expr) (eq (car expr) 'or))
     (transpile-or (cdr expr) scope))
    ((and (consp expr) (eq (car expr) 'lambda) (= (length expr) 3))
     (transpile-lambda expr scope))
    ((and (consp expr) (eq (car expr) 'dynamic) (= (length expr) 2))
     (transpile-dynamic-read expr))
    ((and (consp expr) (eq (car expr) 'defdynamic) (= (length expr) 3))
     (transpile-defdynamic expr scope))
    ((and (consp expr) (eq (car expr) 'go) (= (length expr) 2))
     (transpile-go expr))
    ((and (consp expr) (eq (car expr) 'return-from))
     (transpile-return-from expr scope))
    ((and (consp expr) (eq (car expr) 'block))
     (transpile-block expr scope))
    ((and (consp expr) (eq (car expr) 'tagbody))
     (transpile-tagbody expr scope))
    ((and (consp expr) (eq (car expr) 'unwind-protect))
     (transpile-unwind-protect expr scope))
    ((and (consp expr) (eq (car expr) 'catch))
     (transpile-catch expr scope))
    ((and (consp expr) (eq (car expr) 'throw) (= (length expr) 3))
     (transpile-throw expr scope))
    ((and (consp expr) (symbolp (car expr)))
     (transpile-call expr scope))
    ((and (consp expr) (consp (car expr)) (eq (car (car expr)) 'lambda) (= (length (car expr)) 3))
     ;; let/let*の展開((lambda (vars) body) inits)のような、演算子位置に直接
     ;; lambda式が来る即時呼び出し形式。funcallプリミティブ(primitive_funcall、
     ;; fnを第一引数、残りを実引数として受け取りapply_functionへ渡す)への
     ;; 呼び出しへ書き換えることで、既存のtranspile-call/transpile-lambdaを
     ;; そのまま再利用できる
     (transpile-call (cons 'funcall expr) scope))
    ((symbolp expr)
     (error "transpile-expr: 未束縛の変数参照です: ~S" expr))
    (t (error "transpile-expr: 未対応の式です: ~S" expr))))


(defparameter *ct-temp-counter* 0
  "M14基盤D: block/return-from/tagbody/go導入に伴い、testの結果や中間式の値を
   一時変数へ受けてos_is_control_transferで判定するif/progn/and/setqが使う
   一意なCローカル変数名のカウンタ(*or-temp-counter*と同じ役割の専用版)")

(defun transpile-if (expr scope)
  "(if test then [else])。真偽値は「nil以外はすべて真」の規約に従い、
   testをnilと比較した結果でCの三項演算子に分岐する(za.cの拡張0と同じ設計)。
   elseを省略した場合はnilがデフォルト(destructuring-bindの&optionalが
   nilにデフォルトし、transpile-exprがnil式を\"nil\"に変換するのでそのまま流れる)。
   M14基盤D: eval_if(eval.c)がtestの評価結果を真偽判定の前にos_is_control_transfer
   でチェックしているのと同じく、testがblock/tagbodyのgo/return-from非局所脱出
   シグナルであれば真偽判定を行わずそのまま式全体の値として伝播させる(then/else
   どちらも評価しない)"
  (destructuring-bind (if-kw test then &optional else) expr
    (declare (ignore if-kw))
    (let ((temp (format nil "__if_test_~A" (incf *ct-temp-counter*))))
      (format nil "({ lisp_val_t ~A = (~A); os_is_control_transfer(~A) ? ~A : ((~A) != nil ? (~A) : (~A)); })"
              temp (transpile-expr test scope) temp temp temp
              (transpile-expr then scope)
              (transpile-expr else scope)))))

(defun transpile-progn-forms (forms scope)
  "formのリストを左から順に評価し、最後の式の値を残す(transpile-prognの本体、
   および同じ評価順を必要とするblockの本体からも共有される)。M14基盤D:
   eval_progn(eval.c)がform評価ごとにos_is_control_transferをチェックし
   即座に伝播させているのと同じく、途中のformがblock/tagbodyの非局所脱出
   シグナルを返した場合は残りのformを評価せずそのシグナルを式全体の値として
   返す(forの本体に埋め込まれたreturn-from/goが後続のformを実行してしまう
   のを防ぐために必須)。formが1つも無い場合はnilを返す"
  (cond
    ((null forms) "nil")
    ((null (cdr forms)) (transpile-expr (car forms) scope))
    (t (let ((temp (format nil "__progn_val_~A" (incf *ct-temp-counter*))))
         (format nil "({ lisp_val_t ~A = (~A); os_is_control_transfer(~A) ? ~A : (~A); })"
                 temp (transpile-expr (car forms) scope) temp temp
                 (transpile-progn-forms (cdr forms) scope))))))

(defun transpile-progn (expr scope)
  "(progn form*)。transpile-progn-forms参照"
  (transpile-progn-forms (cdr expr) scope))

(defun transpile-setq (expr scope)
  "(setq var val)。varはscope内の束縛済みローカル変数(defun/lambdaパラメータ、
   またはlambdaが捕捉した自由変数)のみ対応する。box化されていない変数への
   代入は、Cの代入式が代入後の値そのものに評価される性質を使い、追加の処理
   無くLispのsetqの戻り値規約と一致する。box化された変数(za.cの拡張4と同じ
   基準でsetqされエスケープするlambdaに捕捉されたもの)への代入はos_setcdrで
   box(cons)のcdrを直接書き換える(このboxを共有する他のクロージャや外側の
   スコープからも変更後の値が見える)。os_setcdrはval自身を返す規約なので、
   ここでもCの代入式と同じく戻り値の扱いを変える必要がない。M14基盤D:
   eval_setq(eval.c)がvalの評価結果をos_setq_variable呼び出しの前に
   os_is_control_transferでチェックし、非局所脱出シグナルであれば代入自体を
   行わずそのまま返しているのと同じく、valがそのシグナルの場合は変数/boxへの
   代入を実行せずシグナルをそのまま式全体の値として返す"
  (let* ((var (second expr))
         (val (third expr))
         (binding (assoc var scope)))
    (unless binding
      (error "transpile-setq: setqの対象が未束縛のローカル変数です: ~S" var))
    (let ((c-name (car (cdr binding)))
          (boxed-p (cdr (cdr binding)))
          (temp (format nil "__setq_val_~A" (incf *ct-temp-counter*))))
      (format nil "({ lisp_val_t ~A = (~A); os_is_control_transfer(~A) ? ~A : ~A; })"
              temp (transpile-expr val scope) temp temp
              (if boxed-p
                  (format nil "os_setcdr(~A, ~A)" c-name temp)
                  (format nil "(~A = ~A)" c-name temp))))))

(defun transpile-and (forms scope)
  "(and form*)。init.lispのdefmacro andが展開する(if a (and b...) nil)の
   ネストと同じ形をCの三項演算子で直接生成する(=transpile-ifと同じパターン)。
   formが1つも無い場合はt、1つだけの場合はその式自身をそのまま返す
   (末尾のif展開に頼らずここで打ち切ることで、不要なネストを避ける)。M14基盤D:
   transpile-ifと同じ理由で、各formの評価結果が非局所脱出シグナルであれば
   真偽判定を行わずそのまま伝播させる"
  (cond
    ((null forms) "g_sym_t")
    ((null (cdr forms)) (transpile-expr (car forms) scope))
    (t (let ((temp (format nil "__and_val_~A" (incf *ct-temp-counter*))))
         (format nil "({ lisp_val_t ~A = (~A); os_is_control_transfer(~A) ? ~A : ((~A) != nil ? (~A) : nil); })"
                 temp (transpile-expr (car forms) scope) temp temp temp
                 (transpile-and (cdr forms) scope))))))

(defparameter *or-temp-counter* 0)

(defun transpile-or (forms scope)
  "(or form*)。init.lispのdefmacro orは(let ((temp a)) (if temp temp (or ...)))
   に展開し、これはaを二重評価しないための一時変数である。このマイルストンでは
   letが未対応(M10でlambda lifting/closuresが入るまで見送り)なため、GNU Cの
   文(ステートメント)式({ ...; expr; })でCレベルの一時変数を導入し、同じ
   単一評価の性質を実現する(一時変数名はネストしたブロックスコープにより
   衝突しないが、可読性と将来の-Wshadow対策のため呼び出しごとに一意な名前を
   振る)。formが1つも無い場合はnil、1つだけの場合はその式自身をそのまま返す"
  (cond
    ((null forms) "nil")
    ((null (cdr forms)) (transpile-expr (car forms) scope))
    (t (let ((temp (format nil "__or_tmp_~A" (incf *or-temp-counter*))))
         (format nil "({ lisp_val_t ~A = (~A); ~A != nil ? ~A : (~A); })"
                 temp
                 (transpile-expr (car forms) scope)
                 temp
                 temp
                 (transpile-or (cdr forms) scope))))))

(defun transpile-quoted (val)
  "(quote val)のval側。fixnum/string/nil/tはtranspile-exprと同じ扱いで、
   それ以外のシンボルはos_make_symbol_cachedで名前から解決する。consは要素ごとに
   再帰的にtranspile-quotedしたものをos_make_consで組み立てる(caseのkeylist
   '(1 2 3)等、リテラルなリストのquote対応。transpile-cons-chainと同じ
   「引数を評価してから呼び出す」C評価順のため、途中でGCが起きても未保護の
   中間値が上位32bit破壊等に晒される窓は無い)。
   [ABI刷新] AOT改善B: シンボル解決はos_make_symbolを直接呼ぶ代わりに、この
   呼び出し箇所専用のC static局所変数(GNU文式`({ static int idx = -1; ...; })`
   で包む)にg_symbol_table上の添字をキャッシュするos_make_symbol_cachedを使う。
   2回目以降の実行はハッシュ計算・文字列比較を一切行わずg_symbol_table[idx]を
   返すだけになる(quoteシンボルリテラルはslot-value/dynamic/tagbodyのタグ名
   比較等でも使われ、B'適用後もwhile/dotimesの各反復で複数回実行される
   ホットパスだったため、documents/abi-redesign.md 2026-09-10調査参照)。
   staticローカル変数はCの言語仕様上ブロックスコープごとに独立した実体を持つ
   ため、transpile-quotedの呼び出し箇所ごとに(このtranspile-quoted関数自身が
   何度呼ばれても、生成されるC上の字句的ブロックはその都度別物なので)
   衝突なく機能する——一意なカウンタ付き変数名を振る必要が無い"
  (cond
    ((symbolp val)
     (cond
       ((null val) "nil")
       ((eq val t) "g_sym_t")
       (t (format nil "({ static int __quote_sym_idx = -1; os_make_symbol_cached(&__quote_sym_idx, ~A); })"
                  (c-string-literal (if (keywordp val)
                                         (concatenate 'string ":" (symbol-name val))
                                         (symbol-name val)))))))
    ((consp val)
     (format nil "os_make_cons(~A, ~A)" (transpile-quoted (car val)) (transpile-quoted (cdr val))))
    (t (transpile-expr val))))

(defun transpile-dynamic-read (expr)
  "(dynamic name)。nameは未評価のシンボルリテラルとして扱い(quoteと同様)、
   os_get_dynamicで動的変数の現在値を取得する式を生成する"
  (format nil "os_get_dynamic(~A)" (transpile-quoted (second expr))))

(defparameter *defdynamic-temp-counter* 0)

(defun transpile-defdynamic (expr scope)
  "(defdynamic name value-form)。value-formをscope内で評価し、GCで移動しうる
   その結果を一旦Cローカル変数へGC_PROTECTしてから、name(未評価のシンボル
   リテラル)をos_make_symbolで解決してos_set_dynamicへ渡す。eval_defdynamicと
   同じくnameそのものを式全体の値として返す(za.cの拡張6と同じ意味論)。
   M14基盤D: eval_defdynamicがvalueの評価結果をos_set_dynamic呼び出しの前に
   is_control_transferでチェックし、非局所脱出シグナルであれば設定を行わず
   そのまま返しているのと同じく、valueがblock/tagbodyのシグナルであれば
   os_set_dynamicを呼ばずシグナルをそのまま式全体の値として返す"
  (let ((temp (format nil "__defdynamic_val_~A" (incf *defdynamic-temp-counter*)))
        (name-c (transpile-quoted (second expr))))
    (format nil "({ lisp_val_t ~A = ~A; GC_PROTECT(~A); os_is_control_transfer(~A) ? ~A : (os_set_dynamic(~A, ~A), ~A); })"
            temp (transpile-expr (third expr) scope) temp temp temp name-c temp name-c)))

(defparameter *enclosing-tagbodies* nil
  "現在transpile中のC関数内で、直接Cのgotoでジャンプできるtagbodyのタグ→
   Cラベル名のalist(内側のtagbodyのタグほど前に来る=assocが自然に最も近い
   tagbodyを優先する、CLの字句的スコープと同じ結果になる)。transpile-tagbody
   が自分のタグをpushしながら本体をtranspileし、emit-function-body(新しいC
   関数=defun/lambdaの境界)とtranspile-unwind-protect(protected-form/
   cleanup-formsの実行を飛び越えてはいけない境界)がnilへ再束縛することで、
   Cのgotoが届かない/届いてはいけない場合には自動的にこのリストが空になり、
   transpile-goが安全に(従来の)シグナル方式へフォールバックする")

(defun transpile-go (expr)
  "(go tag)。tagは未評価のシンボルリテラル(quoteと同様)。
   [ABI刷新] AOT改善A: tagが*enclosing-tagbodies*(直接Cのgotoで届く範囲=
   同一C関数内かつunwind-protectのprotected-form/cleanup-formsを跨がない)
   に見つかれば、シグナル値の構築(os_make_instance+os_make_symbol、
   ホットループの毎反復で線形/ハッシュ探索を伴う)を一切せず直接C の
   gotoへコンパイルする(while/dotimes/for等、tagbodyの外へエスケープ
   しない大多数のケースがこれに当たる)。届かない場合(lambdaへエスケープした
   クロージャからの参照等、documents/abi-redesign.md 2026-09-10調査の
   「cross function boundary」参照)は、eval_go(eval.c)と同じくMAGIC_GO_EXIT
   のシグナル値を作って返すだけの従来方式のまま(実際のジャンプはこの
   シグナルを受け取ったtagbody側=transpile-tagbodyの%%tagbody-dispatch-chain
   が行う)。goto先はCの文であってCの式ではないため、GNU文式({ goto L; nil; })
   で包んで式コンテキストへ埋め込む(gotoの後のnilは実際には評価されない
   到達不能コード=goto自体が既に制御を移すため)"
  (let* ((tag (second expr))
         (direct (assoc tag *enclosing-tagbodies*)))
    (if direct
        (format nil "({ goto ~A; nil; })" (cdr direct))
        (format nil "os_make_instance(MAGIC_GO_EXIT, ~A, nil, nil)" (transpile-quoted tag)))))

(defparameter *block-temp-counter* 0)

(defun transpile-return-from (expr scope)
  "(return-from name [value-form])。value-formの評価結果がeval_return_from
   (eval.c)と同じく既に非局所脱出シグナルであればそのまま二重にラップせず
   伝播させ、そうでなければMAGIC_BLOCK_EXITのシグナル値(name+value)を作って
   返す。value-formを省略した場合はnilを返す規約(eval_return_fromと同じ)。
   GC_PROTECTは、nameのos_make_symbol呼び出し(未internなら新規確保が起きうる)
   評価中にGCで移動しうるtempをshadow stackへ繋いでおくためのもの"
  (let* ((has-value (cddr expr))
         (value-form (if has-value (third expr) nil))
         (temp (format nil "__return_from_val_~A" (incf *block-temp-counter*))))
    (format nil "({ lisp_val_t ~A = (~A); GC_PROTECT(~A); os_is_control_transfer(~A) ? ~A : os_make_instance(MAGIC_BLOCK_EXIT, ~A, ~A, nil); })"
            temp
            (if has-value (transpile-expr value-form scope) "nil")
            temp temp temp
            (transpile-quoted (second expr))
            temp)))

(defun transpile-block (expr scope)
  "(block name form*)。formをtranspile-progn-formsと同じ規則(非局所脱出
   シグナルなら即座に伝播)で評価し、その結果がこのblockのMAGIC_BLOCK_EXIT
   (nameが一致するもの)であればos_control_transfer_valueで包みを外し、
   それ以外(通常の値、または一致しないタグ/goシグナル、他のblockのシグナル)
   はそのまま伝播させる(eval_block/eval.cと同じ意味論)。GC_PROTECTは、
   name比較用のos_make_symbol呼び出し中にGCで移動しうるtempを保護する"
  (destructuring-bind (block-kw name &rest body) expr
    (declare (ignore block-kw))
    (let ((temp (format nil "__block_val_~A" (incf *block-temp-counter*)))
          (name-c (transpile-quoted name)))
      (format nil "({ lisp_val_t ~A = (~A); GC_PROTECT(~A); (os_is_control_transfer(~A) && os_control_transfer_magic(~A) == MAGIC_BLOCK_EXIT && os_control_transfer_name(~A) == (~A)) ? os_control_transfer_value(~A) : ~A; })"
              temp (transpile-progn-forms body scope)
              temp temp temp temp name-c temp temp))))

(defparameter *tagbody-temp-counter* 0)

(defun %%tagbody-c-label (tag suffix)
  (format nil "__tagbody_label_~A_~A" (sanitize-c-ident (symbol-name tag)) suffix))

(defun %%tagbody-dispatch-chain (signal-temp tags suffix result-temp end-label)
  "signal-tempがMAGIC_GO_EXITでtags中のいずれかのタグ名と一致すれば対応する
   Cラベルへgotoし、どれにも一致しなければ(GO_EXIT以外の非局所脱出シグナルの
   場合も含む)result-tempへsignal-tempを保存してend-labelへgotoする(伝播)"
  (if (null tags)
      (format nil "~A = ~A; goto ~A;" result-temp signal-temp end-label)
      (format nil "if (os_control_transfer_magic(~A) == MAGIC_GO_EXIT && os_control_transfer_name(~A) == (~A)) { goto ~A; } else { ~A }"
              signal-temp signal-temp (transpile-quoted (car tags))
              (%%tagbody-c-label (car tags) suffix)
              (%%tagbody-dispatch-chain signal-temp (cdr tags) suffix result-temp end-label))))

(defun %%tagbody-form-stmt (form tags suffix result-temp end-label scope)
  "tagbody本体中の1つのform(タグでない要素)をCの文へ変換する。評価結果が
   非局所脱出シグナルでなければ単に捨てて次のformへフォールスルーする
   (tagbodyは中間値を無視する、eval_tagbodyと同じ規約)。GC_PROTECTは、
   ディスパッチ先のタグ名比較(os_make_symbol呼び出しを含む)の間、tempが
   GCで移動しうる場合に備えて保護する"
  (let ((temp (format nil "__tagbody_form_~A" (incf *tagbody-temp-counter*))))
    (format nil "{ lisp_val_t ~A = (~A); if (os_is_control_transfer(~A)) { GC_PROTECT(~A); ~A } }"
            temp (transpile-expr form scope) temp temp
            (%%tagbody-dispatch-chain temp tags suffix result-temp end-label))))

(defun transpile-tagbody (expr scope)
  "(tagbody form*)。formの間に挟まる裸シンボルはgoの飛び先タグとして扱う
   (tagbody-tag-p、マクロ展開済みのフォームを扱うため静的に判定できる)。
   タグはこのtagbody内だけで解決される局所的なCラベルへ直接コンパイルする
   (通常のgoto/labelはC関数全体のスコープを持つため、GNU文式でネストしていても
   問題ない)。各formはeval_tagbodyと同じくos_is_control_transferで判定し、
   GO_EXITでこのtagbodyのいずれかのタグへ一致すればgotoでジャンプし、それ以外の
   非局所脱出シグナル(このtagbody内で一致しないgo、またはblockのreturn-from等)
   はそのままtagbody式全体の値として伝播させる。通常の値は捨てて次のformへ進む
   (tagbody自体は最後まで実行した場合は常にnilを返す)"
  (let* ((body (cdr expr))
         (tags (remove-if-not #'tagbody-tag-p body))
         (suffix (incf *tagbody-temp-counter*))
         (result-temp (format nil "__tagbody_result_~A" suffix))
         (end-label (format nil "__tagbody_end_~A" suffix))
         ;; [ABI刷新] AOT改善A: このtagbody自身のタグをCラベルへ対応付け、
         ;; 内側の(=より新しくpushされた)ものがassocで先に見つかるよう手前へ
         ;; 追加する。これにより同名タグを持つネストしたtagbody(例: whileの
         ;; ネスト)でもCLの字句的スコープ通り最も内側のtagbodyが優先される。
         ;; body(=以下でtranspile-goに到達する再帰)のtranspile中だけ有効な
         ;; 動的束縛にすることで、emit-function-body/transpile-unwind-protectが
         ;; nilへ再束縛した外側では自動的に見えなくなる
         (*enclosing-tagbodies*
           (append (mapcar (lambda (tag) (cons tag (%%tagbody-c-label tag suffix))) tags)
                   *enclosing-tagbodies*)))
    (format nil "({ lisp_val_t ~A = nil; ~{~A~}~A: ~A; })"
            result-temp
            (mapcar (lambda (elem)
                      (if (tagbody-tag-p elem)
                          (format nil "~A: ; " (%%tagbody-c-label elem suffix))
                          (format nil "~A " (%%tagbody-form-stmt elem tags suffix result-temp end-label scope))))
                    body)
            end-label
            result-temp)))

(defparameter *unwind-protect-temp-counter* 0)

(defun transpile-unwind-protect (expr scope)
  "(unwind-protect protected-form cleanup-form*)。eval_unwind_protect(eval.c)と
   同じ意味論: protected-formを1回だけ評価し、その結果(通常の値、または
   block/tagbodyの非局所脱出シグナルのいずれでもよい)をGC_PROTECTしたCローカル
   変数へ保存する。cleanup-formはtranspile-progn-formsで評価するが、その戻り値は
   捨てる——cleanup-form自身が新たな非局所脱出を起こした場合もその脱出は無視して
   protected-formの結果を優先する、eval_unwind_protectに明記された既知の簡略化と
   同じ。式全体の値としては常にprotected-formの結果(GC_PROTECTしたtemp)を返す。
   [ABI刷新] AOT改善A: protected-form/cleanup-formsの中から外側のtagbodyのタグへ
   goする場合、Cのgotoで直接ジャンプしてしまうとcleanup-formsの実行(またはまだ
   実行していないcleanup-forms自体)を飛び越えてしまい、unwind-protectの契約
   (保護対象の脱出経路に関わらずcleanup-formsを必ず実行する)を破る。そのため
   *enclosing-tagbodies*をnilへ再束縛してから両方をtranspileし、この境界を
   跨ぐgoは必ず(従来通り)シグナル方式にフォールバックさせる(protected-form/
   cleanup-forms自身の中で完結するtagbody/goはこの再束縛の影響を受けず、
   通常通り直接gotoが使われる)"
  (destructuring-bind (uwp-kw protected-form &rest cleanup-forms) expr
    (declare (ignore uwp-kw))
    (let* ((temp (format nil "__unwind_protect_val_~A" (incf *unwind-protect-temp-counter*)))
           (protected-c (let ((*enclosing-tagbodies* nil)) (transpile-expr protected-form scope)))
           (cleanup-c (let ((*enclosing-tagbodies* nil)) (transpile-progn-forms cleanup-forms scope))))
      (format nil "({ lisp_val_t ~A = (~A); GC_PROTECT(~A); (void)(~A); ~A; })"
              temp protected-c temp cleanup-c temp))))

(defparameter *catch-temp-counter* 0)

(defun transpile-catch (expr scope)
  "(catch tag-form form*)。eval_catch(eval.c)と同じ意味論: tag-formを1回
   評価し(非局所脱出シグナルならそのまま伝播)、その値をGC_PROTECTしたCローカル
   変数(タグ比較のeq判定に使う実行時値、block/return-fromの静的quoteシンボルとは
   違い動的な値なので毎回tag-form自身を評価する)へ保存する。bodyは
   transpile-progn-formsで評価し、その結果がMAGIC_CATCH_EXITでタグがeq
   (Cの==、tagはlisp_val_tなポインタ/即値エンコーディングなので値比較で
   eqと等価)であればos_control_transfer_valueで包みを外し、それ以外(通常の値、
   タグが一致しないthrow、block/tagbodyの他シグナル)はそのまま伝播させる"
  (destructuring-bind (catch-kw tag-form &rest body) expr
    (declare (ignore catch-kw))
    (let ((tag-temp (format nil "__catch_tag_~A" (incf *catch-temp-counter*)))
          (body-temp (format nil "__catch_body_~A" (incf *catch-temp-counter*))))
      (format nil "({ lisp_val_t ~A = (~A); GC_PROTECT(~A); os_is_control_transfer(~A) ? ~A : ({ lisp_val_t ~A = (~A); GC_PROTECT(~A); (os_is_control_transfer(~A) && os_control_transfer_magic(~A) == MAGIC_CATCH_EXIT && os_control_transfer_name(~A) == ~A) ? os_control_transfer_value(~A) : ~A; }); })"
              tag-temp (transpile-expr tag-form scope) tag-temp
              tag-temp tag-temp
              body-temp (transpile-progn-forms body scope) body-temp
              body-temp body-temp body-temp tag-temp
              body-temp body-temp))))

(defparameter *throw-temp-counter* 0)

(defun transpile-throw (expr scope)
  "(throw tag-form result-form)。eval_throw(eval.c)と同じ意味論: tag-form・
   result-formをこの順に1回だけ評価し(いずれかが非局所脱出シグナルならそのまま
   伝播、GC_PROTECTは各値のos_make_instance呼び出しまでGCで移動しうるtempを
   保護するため)、両方が通常値ならMAGIC_CATCH_EXITのシグナル値(tag+result)を
   作って返す。対応するcatchが動的に外側に無い場合、シグナルはそのまま最上位まで
   伝播する(eval_throwのコメントと同じ既知の簡略化)"
  (destructuring-bind (throw-kw tag-form result-form) expr
    (declare (ignore throw-kw))
    (let ((tag-temp (format nil "__throw_tag_~A" (incf *throw-temp-counter*)))
          (val-temp (format nil "__throw_val_~A" (incf *throw-temp-counter*))))
      (format nil "({ lisp_val_t ~A = (~A); GC_PROTECT(~A); os_is_control_transfer(~A) ? ~A : ({ lisp_val_t ~A = (~A); GC_PROTECT(~A); os_is_control_transfer(~A) ? ~A : os_make_instance(MAGIC_CATCH_EXIT, ~A, ~A, nil); }); })"
              tag-temp (transpile-expr tag-form scope) tag-temp
              tag-temp tag-temp
              val-temp (transpile-expr result-form scope) val-temp
              val-temp val-temp
              tag-temp val-temp))))

(defparameter *lambda-name-counter* 0)
(defparameter *lifted-lambda-decls* nil
  "現在transpile-defunがdefun本体を走査している間に見つかったlambda式を
   トップレベルのC関数(__step/公開ラッパー)へリフトした結果の文字列を蓄積する
   リスト。transpile-lambdaが自身の生成物をpushする。ネストしたlambdaほど先に
   push されるため、出力時はreverseして定義順(内側から外側)に並べる(Cは
   使用箇所より前に定義または宣言されている必要があるため)。transpile-defunが
   1つのdefunを処理する間だけletで新しい束縛を張る")
(defparameter *closure-temp-counter* 0)

(defun emit-param-binding-stmt (c-var boxed-p)
  "step関数の先頭で、パラメータ1つをevaluated_argsから読み出しCローカル変数
   c-varへ束縛するC文を作る。boxed-pがnon-nilの場合(za.cの拡張4と同じ基準で
   setqされエスケープするlambdaに捕捉されるパラメータ)は、値そのものではなく
   (捨て値 . 実値)のconsをc-varへ束縛する。このconsが以後の全参照(このパラメータ
   自身への読み書きと、ネストしたlambdaが捕捉する際に共有するオブジェクト)で
   共有される「box」そのものになる"
  (if boxed-p
      (format nil "lisp_val_t ~A = os_make_cons(nil, cc_car(evaluated_args)); evaluated_args = cc_cdr(evaluated_args); GC_PROTECT(~A);"
              c-var c-var)
      (format nil "lisp_val_t ~A = cc_car(evaluated_args); evaluated_args = cc_cdr(evaluated_args); GC_PROTECT(~A);"
              c-var c-var)))

;;; M14 基盤B: &restパラメータ。list/append/create-list/apply/mapcar/map-into等が
;;; 使う。呼び出し側(transpile-call/transpile-tail-call)はパラメータの個数に
;;; 関わらず常に実引数を1つのconsチェーン(evaluated_args)として渡すだけなので、
;;; 変更が必要なのは呼び出される側(param-scope-and-preamble)だけでよい:
;;; 固定パラメータをcc_car/cc_cdrで1つずつ剥がした後に残るevaluated_argsは、
;;; 既にそのまま&restパラメータが指すべきリストそのものになっている

(defun split-rest-param (params)
  "PARAMS(defun/lambdaのパラメータリスト)を(values fixed-params rest-param)に
   分割する。&restシンボルが無ければrest-paramはnil。&rest name(nameは
   シンボル1つ)の形式のみ対応する"
  (let ((pos (position '&rest params)))
    (if (null pos)
        (values params nil)
        (let ((rest-tail (nthcdr (1+ pos) params)))
          (unless (and (= (length rest-tail) 1) (symbolp (car rest-tail)))
            (error "split-rest-param: &restの後はパラメータ名1つのみ対応です: ~S" params))
          (values (subseq params 0 pos) (car rest-tail))))))

(defun emit-rest-param-binding-stmt (c-var boxed-p)
  "step関数の先頭で、&restパラメータへ残りのevaluated_args全部をそのまま
   束縛するC文を作る(cc_car/cc_cdrで1つずつ剥がす通常パラメータとは異なり、
   この時点のevaluated_args自体が既に欲しいリストそのものになっている)。
   boxed-pの意味・box化の形はemit-param-binding-stmtと同じ"
  (if boxed-p
      (format nil "lisp_val_t ~A = os_make_cons(nil, evaluated_args); GC_PROTECT(~A);"
              c-var c-var)
      (format nil "lisp_val_t ~A = evaluated_args; GC_PROTECT(~A);"
              c-var c-var)))

(defun emit-capture-fetch-stmt (c-var symbol-name-string)
  "リフトしたlambdaのstep関数の先頭で、自由変数1つを捕捉環境(envパラメータ、
   apply_function/za_ensure_trampolineが定義時の捕捉環境へ差し替え済み)から
   symbol-name-stringという名前のシンボルで検索し、Cローカル変数c-varへ束縛
   するC文を作る。この変数がboxか値コピーかは、捕捉元の外側スコープでの
   boxed-pがそのまま伝播する(呼び出し元のtranspile-lambda参照)ため、ここでは
   os_get_variableが返した値をそのままc-varへ入れるだけでよい。
   [ABI刷新] AOT改善B: このstep関数は捕捉した各クロージャの呼び出しのたび
   (defmethodの本体はM10のクロージャリフティング経由でコンパイルされるため、
   メソッド呼び出しのたびにこれが実行される)に毎回実行されるホットパスの
   ため、transpile-quotedと同じos_make_symbol_cached+static局所変数の
   キャッシュ化を適用する"
  (format nil "lisp_val_t ~A = os_get_variable(({ static int __capture_sym_idx = -1; os_make_symbol_cached(&__capture_sym_idx, ~A); }), env); GC_PROTECT(~A);"
          c-var (c-string-literal symbol-name-string) c-var))

(defun param-scope-and-preamble (params body)
  "PARAMSとBODY(このパラメータ群だけをスコープに持つLisp式)から、za.cの拡張4と
   同じSBCL方式の変数単位box昇格の判定を行い、(values scope preamble-stmts)を
   返す。scopeはこの関数自身のパラメータ分のalist(シンボル -> (c-name . boxed-p))、
   preamble-stmtsはstep関数の先頭でevaluated_argsから読み出すC文のリスト
   (固定パラメータの並び順、末尾に&restパラメータがあればさらにその後)。
   defun/lambdaのどちらのパラメータ束縛にも共通して使う。&restパラメータ
   (基盤B)はsplit-rest-paramで固定パラメータと切り分け、box昇格判定
   (boxed-params)には固定パラメータと同じ1つの変数として扱わせる"
  (multiple-value-bind (fixed-params rest-param) (split-rest-param params)
    (let* ((all-params (if rest-param (append fixed-params (list rest-param)) fixed-params))
           (boxed (boxed-params body all-params)))
      (values
       (mapcar (lambda (p) (cons p (cons (param-symbol-to-c-name p) (and (member p boxed) t))))
               all-params)
       (append
        (mapcar (lambda (p) (emit-param-binding-stmt (param-symbol-to-c-name p) (and (member p boxed) t)))
                fixed-params)
        (when rest-param
          (list (emit-rest-param-binding-stmt (param-symbol-to-c-name rest-param) (and (member rest-param boxed) t)))))))))

(defun emit-direct-preamble-stmts (params scope arg-c-names)
  "ABI-M6: PARAMS(&restを持たない固定パラメータのみ。呼び出し元がarityの
   一致で保証する)を、ARG-C-NAMES(name__fixedが直接受け取るCパラメータ名)から
   束縛するC文のリストを作る。emit-param-binding-stmt(evaluated_argsをcc_car/
   cc_cdrで剥がす版)に対応する直接束縛版で、box化するかどうか(boxed-p)は
   呼び出し規約に依存しないためparam-scope-and-preambleが返したSCOPEをそのまま
   再利用する"
  (mapcar (lambda (p arg-c-name)
            (let* ((entry (cdr (assoc p scope)))
                   (c-var (car entry))
                   (boxed-p (cdr entry)))
              (if boxed-p
                  (format nil "lisp_val_t ~A = os_make_cons(nil, ~A); GC_PROTECT(~A);" c-var arg-c-name c-var)
                  (format nil "lisp_val_t ~A = ~A; GC_PROTECT(~A);" c-var arg-c-name c-var))))
          params arg-c-names))

(defun emit-trampoline-drive-loop (var-name env-c-name)
  "tco_result_tを繰り返しトランポリンし切って最終値をVAR-NAMEに残すCの文列を作る
   (defun/lambdaの公開ラッパー(name(evaluated_args,env))、ABI-M6のname__fixed
   公開ラッパーのいずれも、この同じ文列を共有する)。
   ABI-M8: is_tail_call==2(固定引数ABI継続、transpile-tail-call-fixed-args-guarded
   参照)の場合は、fixed_fn(void*、実際の呼び出し先ごとに引数個数が異なる
   name__step_fixedへの生ポインタ)をfixed_argc(0〜3)に応じた関数ポインタ型へ
   キャストしてenv+arg0..2で直接call(consリストを一切構築しない)。
   is_tail_call==1(従来のconsリストABI継続)の場合は従来通りfn(args, env)を呼ぶ"
  (with-output-to-string (out)
    (format out "    while (~A.is_tail_call) {~%" var-name)
    (format out "        if (~A.is_tail_call == 2) {~%" var-name)
    (format out "            switch (~A.fixed_argc) {~%" var-name)
    (format out "            case 0: ~A = ((tco_result_t (*)(lisp_val_t))~A.fixed_fn)(~A); break;~%"
            var-name var-name env-c-name)
    (format out "            case 1: ~A = ((tco_result_t (*)(lisp_val_t, lisp_val_t))~A.fixed_fn)(~A, ~A.arg0); break;~%"
            var-name var-name env-c-name var-name)
    (format out "            case 2: ~A = ((tco_result_t (*)(lisp_val_t, lisp_val_t, lisp_val_t))~A.fixed_fn)(~A, ~A.arg0, ~A.arg1); break;~%"
            var-name var-name env-c-name var-name var-name)
    (format out "            default: ~A = ((tco_result_t (*)(lisp_val_t, lisp_val_t, lisp_val_t, lisp_val_t))~A.fixed_fn)(~A, ~A.arg0, ~A.arg1, ~A.arg2); break;~%"
            var-name var-name env-c-name var-name var-name var-name)
    (format out "            }~%")
    (format out "        } else {~%")
    (format out "            ~A = ~A.fn(~A.args, ~A);~%" var-name var-name var-name env-c-name)
    (format out "        }~%")
    (format out "    }~%")))

(defun emit-function-body (c-name params preamble-stmts body-form scope &optional fixed-arity)
  "defun/lambdaのどちらにも共通のstep関数+公開ラッパーのC関数定義を組み立てる。
   PARAMSはevaluated_argsを消費するパラメータの並び(空ならevaluated_args自体が
   未使用になるため(void)キャストで警告を抑止する)、PREAMBLE-STMTSはstep関数の
   先頭で実行するC文(パラメータ束縛+lambdaの場合は自由変数の捕捉環境からの
   読み出し)、BODY-FORMは末尾位置として処理する本体式、SCOPEはPREAMBLE-STMTSが
   束縛した全変数(パラメータ+自由変数)を含むalist。
   ABI-M6: FIXED-ARITYが非nil(&restを持たないdefunのみ、transpile-defun参照。
   lambdaは常にnilのまま渡さない)の場合、consリストABI(__step/公開ラッパー)に
   加えて固定引数レジスタ渡し版(__step_fixed/__fixed)も出力する。za.cの
   ABI-M5(パラメータスロット方式)と同じ「エントリの前段(パラメータ束縛)だけを
   複製し、tail-stmt(本体)のテキスト自体は1度生成したものを両エントリで
   共有する」設計。__step_fixedは当初「このファイル内のname__fixedからしか
   呼ばれない」という想定でstaticにしていたが、ABI-M8(tco_result_tの固定引数
   ABI継続、transpile-tail-call-fixed-args-guarded参照)により、他の(同一
   ファイル内で前方参照になる、またはファイルを跨ぐ)関数の末尾呼び出しからも
   fixed_fnとして直接参照されるようになったため、__step自身と同じ理由で
   staticにできない(transpile-prototypeが両ファイルへ共通で前方宣言を出力する)。
   [ABI刷新] AOT改善A: ここが常に新しいC関数(__step)の先頭であり、Cのgotoは
   他の関数のラベルへは届かない。そのため*enclosing-tagbodies*をnilへ再束縛
   してからbody-formをtranspileし、この関数の外(呼び出し元がlambdaなら
   捕捉元のtagbody等)のタグへのgoが誤って直接gotoにコンパイルされないように
   する(このケースは実際にはdefun/lambda境界を跨ぐ通常のLispコードでは
   起こらないはずだが、万一書かれた場合も安全に従来のシグナル方式へ
   フォールバックさせるための防御)"
  (let* ((*enclosing-tagbodies* nil)
         (tail-stmt (transpile-tail-stmt body-form scope)))
    (with-output-to-string (out)
      (format out "tco_result_t ~A__step(lisp_val_t evaluated_args, lisp_val_t env) {~%" c-name)
      (when (null params)
        (format out "    (void)evaluated_args;~%"))
      (dolist (stmt preamble-stmts)
        (format out "    ~A~%" stmt))
      (format out "    (void)env;~%")
      (format out "    ~A~%" tail-stmt)
      (format out "}~%~%")
      (format out "lisp_val_t ~A(lisp_val_t evaluated_args, lisp_val_t env) {~%" c-name)
      (format out "    tco_result_t __r = ~A__step(evaluated_args, env);~%" c-name)
      (format out "~A" (emit-trampoline-drive-loop "__r" "env"))
      (format out "    return __r.value;~%")
      (format out "}~%")
      (when fixed-arity
        (let* ((arg-c-names (fixed-entry-param-c-names fixed-arity))
               (direct-preamble (emit-direct-preamble-stmts params scope arg-c-names)))
          (format out "~%tco_result_t ~A__step_fixed(lisp_val_t env~{, lisp_val_t ~A~}) {~%"
                  c-name arg-c-names)
          (dolist (stmt direct-preamble)
            (format out "    ~A~%" stmt))
          (format out "    (void)env;~%")
          (format out "    ~A~%" tail-stmt)
          (format out "}~%~%")
          (format out "lisp_val_t ~A__fixed(lisp_val_t env~{, lisp_val_t ~A~}) {~%" c-name arg-c-names)
          (format out "    tco_result_t __r = ~A__step_fixed(env~{, ~A~});~%" c-name arg-c-names)
          (format out "~A" (emit-trampoline-drive-loop "__r" "env"))
          (format out "    return __r.value;~%")
          (format out "}~%"))))))

(defun emit-closure-creation (c-name free-vars outer-scope)
  "リフトしたlambda本体(c-name)を、自由変数だけを含む最小限の捕捉環境と共に
   os_make_lifted_closureでラップするC式を作る。自由変数が1つも無ければ捕捉用の
   環境オブジェクトを新たに確保する必要は無いが、捕捉環境としてnilを渡すのは
   誤り(M12 #27 Phase6のQEMU限定regressionで発見): lambda本体が%%funcall-by-name
   経由でまだAOT化されていない(init.lisp常駐の)関数を呼ぶと、呼ばれた側の
   インタプリタ本体評価はここで渡したenvをそのまま使うため、envがnilだと
   os_get_functionが親を辿れずglobal_environmentへ到達できず、本体中の通常の
   関数呼び出し(interpreted defun同士の呼び出し)がことごとく未定義関数扱いに
   なりg_sym_eval_errorを返す。global_environmentは全ての環境チェーンの根なので
   代わりにこれを渡す(自由変数を捕捉しないため意味的にも安全)。自由変数がある
   場合は、
   os_make_environment(親を持たない、この捕捉専用の環境)を作りGC_PROTECTしたのち、
   各自由変数についてOUTER-SCOPE(このlambda式が出現した時点の外側のscope)での
   現在の値(box化されていればboxそのもの、そうでなければ値そのもの)を
   os_env_add_binding_pairで(sym . 値)ペアとして連結する。boxそのものを共有
   することで、複数のクロージャが同じboxを捕捉した場合に一方の書き換えが
   他方からも見える(za.cの拡張4と同じ設計)。シンボル/consの確保がGCを
   誘発しても既存のOUTER-SCOPEの変数は呼び出し元でGC_PROTECT済みなので安全。
   [ABI刷新] AOT改善B: このクロージャ生成式はループ内で毎回新規クロージャを
   作る箇所(defmethodのメソッド呼び出しのたび等)で繰り返し実行されるため、
   環境名/各自由変数名のシンボル解決をos_make_symbol_cached+static局所変数の
   キャッシュ化に置き換える(transpile-quoted/emit-capture-fetch-stmtと同じ
   パターン)"
  (if (null free-vars)
      (format nil "os_make_lifted_closure((lisp_addr_t)(void *)~A, global_environment)" c-name)
      (let ((env-temp (format nil "__closure_env_~A" (incf *closure-temp-counter*))))
        (format nil "({ lisp_val_t ~A = os_make_environment(({ static int __closure_env_name_idx = -1; os_make_symbol_cached(&__closure_env_name_idx, ~A); }), nil); GC_PROTECT(~A); ~{~A~}os_make_lifted_closure((lisp_addr_t)(void *)~A, ~A); })"
                env-temp
                (c-string-literal c-name)
                env-temp
                (mapcar (lambda (v)
                          (let* ((sym-temp (format nil "__closure_sym_~A" (incf *closure-temp-counter*)))
                                 (pair-temp (format nil "__closure_pair_~A" (incf *closure-temp-counter*)))
                                 (outer-c-name (car (cdr (assoc v outer-scope)))))
                            (format nil "lisp_val_t ~A = ({ static int __closure_free_sym_idx = -1; os_make_symbol_cached(&__closure_free_sym_idx, ~A); }); GC_PROTECT(~A); lisp_val_t ~A = os_make_cons(~A, ~A); GC_PROTECT(~A); os_env_add_binding_pair(~A, ~A); "
                                    sym-temp (c-string-literal (symbol-name v)) sym-temp
                                    pair-temp sym-temp outer-c-name
                                    pair-temp
                                    pair-temp env-temp)))
                        free-vars)
                c-name
                env-temp))))

(defun transpile-lambda (expr scope)
  "(lambda (param*) <本体1式>)。M10で導入する第一級(エスケープ可能)クロージャ。
   自由変数(パラメータでも他のdefun名/プリミティブ名でもない裸の変数参照)を
   free-variablesで解析し、外側のSCOPEで解決できなければ未束縛変数として
   エラーにする(transpile-exprの規約と同じ)。本体はdefunと同じ__step/公開
   ラッパーの2関数構成でトップレベルのC関数へリフトし(*lifted-lambda-decls*へ
   push)、使用箇所にはリフト済み関数と捕捉環境をos_make_lifted_closureで
   ラップする式(emit-closure-creation)を返す。パラメータ自身のbox昇格判定は
   defunと同じparam-scope-and-preambleを使い、捕捉した自由変数のbox/値コピーの
   区別は捕捉元(outer scope)のboxed-pをそのまま継承する(boxはこのlambdaが
   新たに決めるものではなく、そのbox化された変数を最初に持つdefun/lambdaが
   一度だけ決める性質だから)"
  (destructuring-bind (lambda-kw params raw-body) expr
    (declare (ignore lambda-kw))
    (unless (every #'symbolp params)
      (error "transpile-lambda: パラメータはシンボルのみ対応です: ~S" expr))
    (let* ((c-name (format nil "__lisp_lambda_~A" (incf *lambda-name-counter*)))
           (body (macroexpand-all raw-body))
           (free-vars (free-variables body params)))
      (dolist (v free-vars)
        (unless (assoc v scope)
          (error "transpile-lambda: 自由変数が外側のスコープで未束縛です: ~S" v)))
      (multiple-value-bind (own-scope own-preamble) (param-scope-and-preamble params body)
        (let* ((capture-scope
                 (mapcar (lambda (v)
                           (let* ((binding (cdr (assoc v scope)))
                                  (outer-boxed-p (cdr binding)))
                             (cons v (cons (format nil "__captured_~A" (param-symbol-to-c-name v))
                                           outer-boxed-p))))
                         free-vars))
               (capture-preamble
                 (mapcar (lambda (v)
                           (emit-capture-fetch-stmt (car (cdr (assoc v capture-scope))) (symbol-name v)))
                         free-vars))
               (body-scope (append own-scope capture-scope))
               (fn-text (emit-function-body c-name params (append own-preamble capture-preamble) body body-scope))
               (closure-expr (emit-closure-creation c-name free-vars scope)))
          (push fn-text *lifted-lambda-decls*)
          closure-expr)))))


(defun transpile-cons-chain (temps)
  "Cの一時変数名のリストから、末尾がnilのconsチェーンを組み立てるC式を作る
   (evaluated_argsとして呼び出し先に渡す引数リストの構築)"
  (if (null temps)
      "nil"
      (format nil "os_make_cons(~A, ~A)" (car temps) (transpile-cons-chain (cdr temps)))))

(defun transpile-c-arg-list (temps)
  "Cの一時変数名のリストをカンマ区切りの引数並びへ整形する(ABI-M3の固定引数
   直接呼び出し用。transpile-cons-chainと異なりconsチェーンを組み立てない)"
  (format nil "~{~A~^, ~}" temps))

(defparameter *call-temp-counter* 0)

(defun transpile-call-args-guarded (all-temps remaining-temps remaining-args scope final-c-expr)
  "ALL-TEMPSを1つずつGC-safeに評価し、いずれかが非局所脱出シグナル
   (os_is_control_transfer)であれば残りの引数評価とFINAL-C-EXPR(呼び出し本体)を
   一切実行せずそのシグナル自身を式全体の値として返す。M14基盤D:
   transpile-if/transpile-and/transpile-progn-formsと同じ短絡規則を、通常の
   関数呼び出しの引数評価にも適用する(funcall経由でreturn-from/throwする
   エスケープするクロージャの結果が、letの脱糖((lambda (result) body) init)の
   ように別の呼び出しの引数として渡された場合、この規則が無いと非局所脱出
   シグナルが素通しされずただの値として本体に渡ってしまう不具合があった)"
  (if (null remaining-temps)
      (funcall final-c-expr all-temps)
      (let ((temp (car remaining-temps)))
        (format nil "({ lisp_val_t ~A = (~A); GC_PROTECT(~A); os_is_control_transfer(~A) ? ~A : (~A); })"
                temp (transpile-expr (car remaining-args) scope) temp temp temp
                (transpile-call-args-guarded all-temps (cdr remaining-temps) (cdr remaining-args) scope final-c-expr)))))

(defun transpile-call (expr scope)
  "(name arg*)。nameはdefunされた関数名またはプリミティブのホワイトリストに
   限る(call-target-c-name参照)。各引数はeval.cのeval_argsと同じ考え方で、
   1つずつ評価してすぐGC_PROTECTしてから次の引数を評価する(引数式自体の評価が
   GCを誘発しても、既に評価済みの前の引数がコピーGCで移動済みの古いアドレスを
   指したままにならないようにするため)。引数が無い場合は一時変数もconsチェーンも
   不要なため、直接nilを渡す単純な呼び出し式にする。M12 Phase 9(#27):
   transpile-call-args-guarded参照、いずれかの引数が非局所脱出シグナルなら
   呼び出し自体を行わずそのシグナルを伝播させる。
   ABI-M3: nameの実引数個数が*primitive-fixed-arity-c-names*の固定arityと
   一致する場合は、consチェーン構築(transpile-cons-chain)を経由せず、
   GC_PROTECT済みの引数一時変数をそのままカンマ区切りで固定引数版のC関数へ
   渡す(primitive-fixed-arity-c-name参照)。ただしcall-target-c-nameと同じ
   優先順位(*known-function-names*、つまりこのファイル内でユーザーが同名の
   defunで再定義した場合はそちらを優先する)を保つため、nameが
   *known-function-names*に載っている場合はこの高速パスを使わない。
   ABI-M6: nameが*known-function-names*に載っており(defunされた関数)、かつ
   その関数が&restを持たず実引数個数argcとちょうどarityが一致する場合、
   name__fixed(env, arg0, ...)を直接callする(consチェーン構築を経由しない)。
   za.cのABI-M5(JIT側)と異なり、AOTの既知関数呼び出しはFunction Cell経由の
   間接呼び出しではなく元々リンク時に確定するC関数名の直接呼び出しのため
   (call-target-c-name参照)、実行時のarity/redefinition判定は不要でコンパイル
   時にこの高速パスを選べる。いずれの高速パスにも一致しなければ従来通り
   call-target-c-nameが解決するconsチェーン渡しのn項/可変長版へフォールバック
   する"
  (let* ((name (car expr))
         (args (cdr expr))
         (argc (length args))
         (known-p (member name *known-function-names*))
         (known-arity (and known-p (known-function-fixed-arity name)))
         (fixed-c-name (if known-p
                            nil
                            (primitive-fixed-arity-c-name name argc))))
    (cond
      ((and known-arity (= known-arity argc))
       (let ((temps (mapcar (lambda (arg)
                               (declare (ignore arg))
                               (format nil "__call_arg_~A" (incf *call-temp-counter*)))
                             args)))
         (transpile-call-args-guarded temps temps args scope
           (lambda (all-temps)
             (format nil "~A__fixed(env, ~A)" (lisp-name-to-c-name name) (transpile-c-arg-list all-temps))))))
      (fixed-c-name
       (let ((temps (mapcar (lambda (arg)
                               (declare (ignore arg))
                               (format nil "__call_arg_~A" (incf *call-temp-counter*)))
                             args)))
         (transpile-call-args-guarded temps temps args scope
           (lambda (all-temps) (format nil "~A(~A)" fixed-c-name (transpile-c-arg-list all-temps))))))
      (t
       (let ((c-name (call-target-c-name name)))
         (if (null args)
             (format nil "~A(nil, env)" c-name)
             (let ((temps (mapcar (lambda (arg)
                                     (declare (ignore arg))
                                     (format nil "__call_arg_~A" (incf *call-temp-counter*)))
                                   args)))
               (transpile-call-args-guarded temps temps args scope
                 (lambda (all-temps) (format nil "~A(~A, env)" c-name (transpile-cons-chain all-temps)))))))))))

(defun tail-return-final (c-expr)
  "末尾位置で、既に確定したC式c-exprの値をそのままtco_result_tとしてreturnする
   Cの文を作る(トランポリンを継続させず、この時点で呼び出し元のwhileループを
   終了させる)"
  (format nil "return (tco_result_t){.is_tail_call = 0, .value = (~A)};" c-expr))

(defparameter *tco-max-fixed-argc* 3
  "ABI-M8: tco_result_tの固定引数ABI継続(fixed_fn/fixed_argc/arg0..2)が
   保持できる引数個数の上限。za.cのABI-M5/M7のZA_MAX_FIXED_ENTRY_PARAMSと
   同じ値に揃えた(tco_result_tは全呼び出し先で共有する固定サイズのCの構造体
   のため、フィールド数の上限をどこかで決める必要があり、za.c側との対応関係を
   分かりやすくする目的でこの値を選んだ。AOTの非末尾呼び出し(ABI-M6の__fixed)
   自体には technicalなレジスタ制約は無いが、末尾呼び出しのトランポリン継続は
   1つの共有構造体を経由するためこの上限が必要になる)")

(defun tco-fixed-arg-inits (temps)
  "TEMPSの各要素を、tco_result_tの.arg0/.arg1/.arg2フィールド初期化子の
   カンマ区切り文字列にする(transpile-tail-call-fixed-args-guarded参照)"
  (format nil "~{~A~^, ~}"
          (loop for i from 0 for temp in temps collect (format nil ".arg~D = ~A" i temp))))

(defun transpile-tail-call (expr scope)
  "末尾位置の(name arg*)で、nameが*known-function-names*に含まれる(=この
   ファイル内でdefunされた)場合にのみ呼ばれる。transpile-callと同様にGC-safeな
   引数一時変数を組み立てるが、実際にはstep関数を呼ばずtco_result_tへ関数
   ポインタと引数consチェーンを詰めてreturnする。呼び出し元のCフレームはここで
   returnして消えるため、何段トランポリンが続いてもCスタックは伸びない。
   M12 Phase 9(#27): transpile-call-args-guardedと同じ短絡規則で、いずれかの
   引数が非局所脱出シグナルならトランポリン継続を組み立てず、そのシグナルを
   確定値としてreturnする。
   ABI-M8: nameが&restを持たない既知関数で、実引数個数がそのarityとちょうど
   一致し、かつ*tco-max-fixed-argc*以下の場合、consチェーン構築を経由せず
   tco_result_tの固定引数ABI継続(fixed_fn=name__step_fixed、arg0..2に評価済みの
   引数)を詰めてreturnする(emit-trampoline-drive-loop参照)。ABI-M6の非末尾
   呼び出しと異なり実行時判定は不要(理由はABI-M6/M7の設計判断と同じ:
   AOTの既知関数呼び出しはリンク時確定の直接呼び出しであり、za.cのような
   Function Cell経由の間接呼び出しではないため)"
  (let* ((name (car expr))
         (args (cdr expr))
         (argc (length args))
         (c-name (lisp-name-to-c-name name))
         (fixed-arity (known-function-fixed-arity name)))
    (cond
      ((and fixed-arity (= fixed-arity argc) (<= argc *tco-max-fixed-argc*))
       (let ((temps (mapcar (lambda (arg)
                               (declare (ignore arg))
                               (format nil "__call_arg_~A" (incf *call-temp-counter*)))
                             args)))
         (transpile-tail-call-fixed-args-guarded temps temps args scope c-name)))
      ((null args)
       (format nil "return (tco_result_t){.is_tail_call = 1, .fn = ~A__step, .args = nil};" c-name))
      (t
       (let ((temps (mapcar (lambda (arg)
                               (declare (ignore arg))
                               (format nil "__call_arg_~A" (incf *call-temp-counter*)))
                             args)))
         (transpile-tail-call-args-guarded temps temps args scope c-name))))))

(defun transpile-tail-call-fixed-args-guarded (all-temps remaining-temps remaining-args scope c-name)
  "transpile-tail-call-args-guardedのABI-M8版。consチェーンを組み立てず、
   評価済みの引数一時変数をtco_result_tの.arg0/.arg1/.arg2へ直接詰める"
  (if (null remaining-temps)
      (format nil "return (tco_result_t){.is_tail_call = 2, .fixed_fn = (void *)~A__step_fixed, .fixed_argc = ~D, ~A};"
              c-name (length all-temps) (tco-fixed-arg-inits all-temps))
      (let ((temp (car remaining-temps)))
        (format nil "{ lisp_val_t ~A = (~A); GC_PROTECT(~A); if (os_is_control_transfer(~A)) { return (tco_result_t){.is_tail_call = 0, .value = (~A)}; } else { ~A } }"
                temp (transpile-expr (car remaining-args) scope) temp temp temp
                (transpile-tail-call-fixed-args-guarded all-temps (cdr remaining-temps) (cdr remaining-args) scope c-name)))))

(defun transpile-tail-call-args-guarded (all-temps remaining-temps remaining-args scope c-name)
  "transpile-call-args-guardedの末尾呼び出し版。式ではなく完結したC文を返す
   (呼び出し元のtranspile-tail-callは既にreturn文の中で使われないため、ここで
   自分でreturn文を組み立てる)"
  (if (null remaining-temps)
      (format nil "return (tco_result_t){.is_tail_call = 1, .fn = ~A__step, .args = ~A};"
              c-name (transpile-cons-chain all-temps))
      (let ((temp (car remaining-temps)))
        (format nil "{ lisp_val_t ~A = (~A); GC_PROTECT(~A); if (os_is_control_transfer(~A)) { return (tco_result_t){.is_tail_call = 0, .value = (~A)}; } else { ~A } }"
                temp (transpile-expr (car remaining-args) scope) temp temp temp
                (transpile-tail-call-args-guarded all-temps (cdr remaining-temps) (cdr remaining-args) scope c-name)))))

(defun transpile-tail-and (forms scope)
  "transpile-andの末尾位置版。最後の式だけが本当の末尾位置(そこに到達した場合の
   値がand全体の値になる)で、それより前の式は真偽判定のためだけに通常評価する
   (nilで短絡した場合の戻り値は常にnilそのものなので、確定値としてreturnする)。
   M14基盤D: transpile-andと同じ理由で、各formの評価結果が非局所脱出シグナル
   であれば真偽判定を行わずそのまま確定値としてreturnする"
  (cond
    ((null forms) (tail-return-final "g_sym_t"))
    ((null (cdr forms)) (transpile-tail-stmt (car forms) scope))
    (t (let ((temp (format nil "__and_val_~A" (incf *ct-temp-counter*))))
         (format nil "{ lisp_val_t ~A = (~A); if (os_is_control_transfer(~A)) { ~A } else if (~A != nil) { ~A } else { ~A } }"
                 temp (transpile-expr (car forms) scope) temp (tail-return-final temp) temp
                 (transpile-tail-and (cdr forms) scope)
                 (tail-return-final "nil"))))))

(defun transpile-tail-progn (forms scope)
  "transpile-progn-formsの末尾位置版。最後のformだけが本当の末尾位置で、
   それより前のformは副作用のためだけに通常評価する。M14基盤D:
   transpile-progn-formsと同じ理由で、途中のformの評価結果が非局所脱出
   シグナルであれば残りのformを評価せずそのシグナルを確定値としてreturnする
   (旧実装は(void)キャストで単純に捨てて次のformへ進んでいたため、for/while
   本体でreturn-from/goが早期脱出しても後続のformが実行されてしまう不具合が
   あった)"
  (cond
    ((null forms) (tail-return-final "nil"))
    ((null (cdr forms)) (transpile-tail-stmt (car forms) scope))
    (t (let ((temp (format nil "__progn_val_~A" (incf *ct-temp-counter*))))
         (format nil "{ lisp_val_t ~A = (~A); if (os_is_control_transfer(~A)) { ~A } else { ~A } }"
                 temp (transpile-expr (car forms) scope) temp (tail-return-final temp)
                 (transpile-tail-progn (cdr forms) scope))))))

(defun transpile-tail-or (forms scope)
  "transpile-orの末尾位置版。二重評価を避けるための一時変数はtranspile-orと同じ
   考え方だが、値を返すのがCの式ではなく文になるため、GNUの文(ステートメント)式
   ではなく普通のブロック内if文で構成する。最後の式だけが本当の末尾位置になる"
  (cond
    ((null forms) (tail-return-final "nil"))
    ((null (cdr forms)) (transpile-tail-stmt (car forms) scope))
    (t (let ((temp (format nil "__or_tmp_~A" (incf *or-temp-counter*))))
         (format nil "{ lisp_val_t ~A = (~A); if (~A != nil) { ~A } else { ~A } }"
                 temp
                 (transpile-expr (car forms) scope)
                 temp
                 (tail-return-final temp)
                 (transpile-tail-or (cdr forms) scope))))))

(defun transpile-tail-stmt (expr scope)
  "式exprが末尾位置にあるときの、tco_result_tをreturnするCの文を作る。
   if/progn/and/orは末尾位置を最後の分岐/式へ伝播し、それ以外(リテラル・quote・
   パラメータ参照・setq・プリミティブ呼び出し・未知の呼び出し)はtranspile-expr
   にそのまま委譲して確定値としてreturnする。*known-function-names*に載っている
   関数への呼び出しだけが、transpile-tail-call経由でトランポリン継続になる
   (quoteとsetqは(consp expr)かつ(symbolp (car expr))を満たすが呼び出し先の
   関数名ではないため、known-function-namesに入り得ずここでは自然に除外される)"
  (cond
    ((and (consp expr) (eq (car expr) 'if))
     (destructuring-bind (if-kw test then &optional else) expr
       (declare (ignore if-kw))
       (let ((temp (format nil "__if_test_~A" (incf *ct-temp-counter*))))
         (format nil "{ lisp_val_t ~A = (~A); if (os_is_control_transfer(~A)) { ~A } else if (~A != nil) { ~A } else { ~A } }"
                 temp (transpile-expr test scope) temp (tail-return-final temp) temp
                 (transpile-tail-stmt then scope)
                 (transpile-tail-stmt else scope)))))
    ((and (consp expr) (eq (car expr) 'progn))
     (transpile-tail-progn (cdr expr) scope))
    ((and (consp expr) (eq (car expr) 'and))
     (transpile-tail-and (cdr expr) scope))
    ((and (consp expr) (eq (car expr) 'or))
     (transpile-tail-or (cdr expr) scope))
    ((and (consp expr) (symbolp (car expr)) (member (car expr) *known-function-names*))
     (transpile-tail-call expr scope))
    (t (tail-return-final (transpile-expr expr scope)))))

(defun transpile-prototype (form)
  "(defun name (param*) body) から、mainがbody生成前に出力する前方宣言を作る。
   相互再帰時、defunの並び順に関わらずどちらのC関数からも他方を呼べるようにする
   (za.cのシンボル解決に頼らない代わりに、AOTのC前方宣言で解決する)。M15:
   *output-c-path*/*fixture-output-c-path*の2ファイルに分かれた後も、フィクスチャ
   側からAOT側の関数を末尾呼び出しする場合に.fn = 対象__stepという形でファイルを
   跨いで関数ポインタを取る必要があるため、__step版もstaticにはしない(以前は
   ファイル内実装詳細としてstaticにしていたが、ファイル分割に伴い外部リンケージが
   必須になった)。全既知関数の前方宣言を両ファイルへ共通で出力する(自分の
   ファイルで定義されない関数は本体無しの宣言のみになるが、Cとして正しい)。
   ABI-M6: known-function-fixed-arityが非nilを返す(&restを持たない)関数のみ、
   name__fixedの前方宣言も追加する。ABI-M8: __step_fixedもname__fixedと同じく
   ファイルを跨いで(またはファイル内で前方参照として)tco_result_tの
   fixed_fnから直接参照されるようになったため、__stepと同様前方宣言する
   (emit-function-body参照)"
  (let* ((name (second form))
         (c-name (lisp-name-to-c-name name))
         (arity (known-function-fixed-arity name)))
    (format nil "tco_result_t ~A__step(lisp_val_t evaluated_args, lisp_val_t env);~%lisp_val_t ~A(lisp_val_t evaluated_args, lisp_val_t env);~%~A"
            c-name c-name
            (if arity
                (format nil "tco_result_t ~A__step_fixed(lisp_val_t env~{, lisp_val_t ~A~});~%lisp_val_t ~A__fixed(lisp_val_t env~{, lisp_val_t ~A~});~%"
                        c-name (fixed-entry-param-c-names arity) c-name (fixed-entry-param-c-names arity))
                ""))))


(defun transpile-defun (form)
  "(defun name (param*) <本体1式>) に対応する。パラメータはシンボルのみ
   (末尾の&rest nameのみ許容、基盤B/param-scope-and-preamble参照)、
   bodyは単一式のみ(fixnum/string/nil/tリテラル・quote・
   パラメータの裸参照・M10のlambda等)。za.cのネイティブABI(lisp_val_t fn(
   lisp_val_t evaluated_args, lisp_val_t env))に合わせ、パラメータ束縛はza.cと
   同様にevaluated_argsをcc_car/cc_cdrで辿って行う(param-scope-and-preamble/
   emit-function-body参照。setqされ、かつ本体中のlambdaに捕捉されるパラメータは
   box化される=M10でdefunパラメータもlambdaと同じbox昇格の対象になる)。

   1つのdefunから2つのC関数を生成する: 本体を1手だけ進めるstep関数
   (末尾位置の既知関数呼び出しをトランポリン継続としてreturnする)と、
   ABI互換の公開ラッパー(stepをwhileループで回し切ってlisp_val_tを返す)。
   これにより自己/相互再帰の末尾呼び出しがCの再帰呼び出しにならず、再帰段数に
   関わらずCスタック消費が一定になる(ファイル先頭のコメント参照)。

   本体の中にlambdaが直接またはネストして現れる場合、それらはこのdefunより先に
   トップレベルのC関数として出力する必要があるため、*lifted-lambda-decls*を
   このdefun専用にletで束縛し、transpile-lambdaが積んだ結果を(内側から外側の
   順で)このdefun自身のC関数定義の前に連結する"
  (destructuring-bind (defun-kw name params &rest body) form
    (declare (ignore defun-kw))
    (unless (every #'symbolp params)
      (error "transpile-defun: パラメータはシンボルのみ対応です: ~S" name))
    (unless (= (length body) 1)
      (error "transpile-defun: bodyは単一式のみ対応です: ~S" name))
    (let* ((c-name (lisp-name-to-c-name name))
           (*lifted-lambda-decls* nil)
           (expanded-body (macroexpand-all (first body)))
           ;; ABI-M6: &restを持たない(=paramsがそのまま固定パラメータ列と一致する)
           ;; 場合のみ非nil。known-function-fixed-arity参照
           (fixed-arity (known-function-fixed-arity name)))
      (multiple-value-bind (scope preamble) (param-scope-and-preamble params expanded-body)
        (let ((fn-text (emit-function-body c-name params preamble expanded-body scope fixed-arity)))
          (format nil "~{~A~%~}~A" (reverse *lifted-lambda-decls*) fn-text))))))

(defun read-all-forms (path)
  (with-open-file (in path)
    (loop for form = (read in nil :eof)
          until (eq form :eof)
          collect form)))

(defun toplevel-defun-p (form)
  (and (consp form) (eq (car form) 'defun)))

(defun transpile-toplevel-form (form)
  "M15: device.lisp等のdefun以外のトップレベルフォーム(defclass/defdynamic/
   起動時に一度だけ実行する副作用のある呼び出し)を1つのC文へ変換する。
   マクロ展開後、パラメータもscopeも持たない非末尾位置の式としてtranspile-expr
   に評価させ、戻り値は捨てる(呼び出し元のos_run_aot_toplevel_formsは副作用の
   ためだけに1回呼ぶ)。本体中に直接またはネストしてlambdaが現れる場合は
   *lifted-lambda-decls*へその定義をpushする(transpile-defunと同じ仕組みを
   このフォーム単位で再利用する)"
  (format nil "    (void)(~A);~%" (transpile-expr (macroexpand-all form) nil)))

(defun emit-toplevel-forms-runner (forms)
  "FORMS(main がファイル読み込み順に集めたdefun以外のトップレベルフォーム、
   *fs-lisp-paths*の並び順=依存関係の順を保つ)から、起動時に一度だけ実行する
   C関数os_run_aot_toplevel_formsを生成する。kernel_mainからos_register_aot_init_
   functions(全defunの登録)の直後に呼ばれる想定で、defdynamicの初期化・
   defclassのクラス登録・%ide-register-devices等の起動時副作用をソース上の
   順序のまま実行する。formsが1つも無くても空の関数を出力する(fs-lisp-pathsが
   空の間もビルドが壊れないように)"
  (let* ((*lifted-lambda-decls* nil)
         (stmts (mapcar #'transpile-toplevel-form forms)))
    ;; transpile-call/transpile-expr(function特殊形式等)が生成するコードは、
    ;; defun本体と同じくローカル変数envの存在を前提にしている(defunのstep関数
    ;; ではパラメータとして渡ってくる)。ここではdefun呼び出しの連鎖の起点となる
    ;; 環境が無いため、global_environment(全ての環境チェーンの根、
    ;; emit-closure-creationの自由変数無しクロージャと同じ考え方)を束縛する
    (format nil "~{~A~%~}void os_run_aot_toplevel_forms(void) {~%    lisp_val_t env = global_environment;~%    (void)env;~%~{~A~}}~%"
            (reverse *lifted-lambda-decls*) stmts)))

(defun emit-aot-registration (aot-defuns)
  "init_aot.lisp由来のdefun群それぞれについて、os_set_function/
   os_make_native_functionでglobal_environmentへ登録するC関数
   os_register_aot_init_functionsを生成する(M13)。init.lispから該当defunの
   テキストを取り除いた後も、インタプリタ側から同じシンボル名でこれらの
   AOTコンパイル済み関数を呼び出せるようにするための配線"
  (format nil "void os_register_aot_init_functions(void) {~%~{    os_set_function(os_make_symbol(\"~A\"), os_make_native_function((lisp_addr_t)(void *)~A), global_environment);~%~}}~%"
          (mapcan (lambda (form)
                    (list (symbol-name (second form)) (lisp-name-to-c-name (second form))))
                  aot-defuns)))

(defun emit-c-file (out-path prototypes bodies tail-text)
  "prototypes(全既知関数分、この呼び出しのファイルに定義が無いものも含む)と
   bodies(このファイルが実際に定義する関数の本体)を、共通のヘッダ(#include/
   tco_result_t型定義)と共に1つのCファイルout-pathへ書き出す。tail-textは
   bodiesの後に追加でそのまま書き出す文字列(registration/toplevel-runner等、
   不要なら空文字列)。M15: *output-c-path*(本番AOT)と*fixture-output-c-path*
   (テスト専用フィクスチャ)の2ファイルから共通で呼ばれる"
  (with-open-file (out out-path :direction :output :if-exists :supersede)
    ;; funcall(primitive_funcall)はeval.hで宣言されているため、runtime.h/lisp.hだけでは
    ;; 暗黙のint宣言(実体はlisp_val_t=64bitを返すため上位32bitが失われ得る)になってしまう。
    ;; M14基盤E: open-input-stream/close(cc_open_input_stream/cc_close)はstream_lisp.hで
    ;; 宣言されているため、同じ理由でstream_lisp.hも必要
    ;; FAT16-M0(0): logand/logior/logxor/ash(cc_logand等)はsubprimitive.hで
    ;; 宣言されているため、同じ理由でsubprimitive.hも必要
    ;; M15: %%ide-*(cc_ide_*)はide_subprimitive.hで宣言されているため、
    ;; 同じ理由でide_subprimitive.hも必要
    (format out "#include \"runtime.h\"~%#include \"lisp.h\"~%#include \"eval.h\"~%#include \"stream_lisp.h\"~%#include \"format.h\"~%#include \"subprimitive.h\"~%#include \"ide_subprimitive.h\"~%~%")
    ;; 末尾呼び出しのトランポリン継続を表す型。is_tail_call=0ならvalueが確定値、
    ;; 1ならfn/argsが「次にこのstep関数をこの引数(consリスト)で呼ぶ」ことを表す
    ;; (実際の呼び出しは各defunの公開ラッパーのwhileループが行う。ファイル先頭の
    ;; コメント参照)。
    ;; ABI-M8: is_tail_call=2はfixed_fn/fixed_argc/arg0..2が「次にこのstep_fixed
    ;; 関数を(env, arg0, ...)で直接呼ぶ(consリストを構築しない)」ことを表す。
    ;; fixed_fnは実際の呼び出し先ごとに引数個数が異なる(name__step_fixedの
    ;; シグネチャはfixed_argc個)ため、汎用のvoid*として持ち、呼び出し側
    ;; (emit-trampoline-drive-loop)がfixed_argcに応じたステップ関数ポインタ型へ
    ;; キャストしてから呼ぶ。fixed_argcは3(ABI-M5/M7のZA_MAX_FIXED_ENTRY_PARAMSと
    ;; 揃えた上限、known-function-fixed-arity/transpile-tail-callが保証)まで。
    ;; M15: 2ファイルそれぞれが独立してこの型定義を持つ(Cにtypeのリンケージは
    ;; 無く、同じレイアウトの型を各翻訳単位が個別に定義するだけなので問題ない)
    (format out "typedef struct tco_result tco_result_t;~%")
    (format out "typedef tco_result_t (*step_fn_t)(lisp_val_t, lisp_val_t);~%")
    (format out "struct tco_result {~%    int is_tail_call;~%    lisp_val_t value;~%    step_fn_t fn;~%    lisp_val_t args;~%    void *fixed_fn;~%    UINT64 fixed_argc;~%    lisp_val_t arg0;~%    lisp_val_t arg1;~%    lisp_val_t arg2;~%};~%~%")
    (dolist (p prototypes)
      (format out "~A~%" p))
    (format out "~%")
    (dolist (b bodies)
      (format out "~A~%" b))
    (format out "~%~A" tail-text)))

(defun main ()
  (let* ((fixture-defuns (remove-if-not #'toplevel-defun-p
                            (%%expand-defgenerics-in-forms (read-all-forms *runtime-lisp-path*))))
         (aot-all-forms (%%expand-defgenerics-in-forms (read-all-forms *aot-lisp-path*)))
         (aot-defuns (remove-if-not #'toplevel-defun-p aot-all-forms))
         (aot-toplevel-forms (remove-if #'toplevel-defun-p aot-all-forms))
         (utility-all-forms (%%expand-defgenerics-in-forms (read-all-forms *utility-lisp-path*)))
         (utility-defuns (remove-if-not #'toplevel-defun-p utility-all-forms))
         (utility-toplevel-forms (remove-if #'toplevel-defun-p utility-all-forms))
         (fs-all-forms (%%expand-defgenerics-in-forms (mapcan #'read-all-forms *fs-lisp-paths*)))
         (fs-defuns (remove-if-not #'toplevel-defun-p fs-all-forms))
         (fs-toplevel-forms (remove-if #'toplevel-defun-p fs-all-forms))
         ;; M15: main-defunsが本番のカーネルバイナリ(*output-c-path*)へ実際に
         ;; 定義を出力する関数群。fixture-defuns(テスト専用)はここに含めない
         (main-defuns (append aot-defuns utility-defuns fs-defuns))
         (all-defuns (append fixture-defuns main-defuns))
         (*known-function-names* (mapcar #'second all-defuns))
         ;; ABI-M6: known-function-fixed-arity(transpile-call/transpile-prototype/
         ;; transpile-defunが参照)が使う、名前->固定パラメータ数の対応表。
         ;; &restを持つ関数はnilのまま(assocに現れず、known-function-fixed-arityが
         ;; nilを返す)にする
         (*known-function-arities*
           (mapcar (lambda (form)
                     (multiple-value-bind (fixed-params rest-param) (split-rest-param (third form))
                       (cons (second form) (if rest-param nil (length fixed-params)))))
                   all-defuns))
         ;; フィクスチャ側からAOT関数を(直接/末尾)呼び出すケースがあるため、
         ;; 前方宣言だけは両ファイルへ全既知関数分を共通で出力する(定義が無い
         ;; 側ではただのextern宣言になるだけで、Cとして問題ない)
         (all-prototypes (mapcar #'transpile-prototype all-defuns))
         (main-bodies (mapcar #'transpile-defun main-defuns))
         (fixture-bodies (mapcar #'transpile-defun fixture-defuns))
         (registration (emit-aot-registration main-defuns))
         ;; init_aot.lisp(*classes*のdefdynamic+21個の%register-builtin-class呼び出し)
         ;; -> utility.lisp -> fs-lisp-paths(依存関係の順)の順序で実行する必要がある
         ;; (fat16.lisp/fat32.lispのdefclassが<standard-object>等の組み込みクラスに
         ;; 依存するため)。この3ファイル群の読み込み順がそのまま実行順になる
         (toplevel-runner (emit-toplevel-forms-runner (append aot-toplevel-forms utility-toplevel-forms fs-toplevel-forms))))
    (emit-c-file *output-c-path* all-prototypes main-bodies
                 (format nil "~A~%~A" registration toplevel-runner))
    ;; M15: フィクスチャは登録(emit-aot-registration)もtoplevel-runnerも不要
    ;; (テスト専用でglobal_environmentへは登録しない、transpile_fixture.lisp
    ;; 自体もdefun以外のトップレベルフォームを持たない)
    (emit-c-file *fixture-output-c-path* all-prototypes fixture-bodies "")))
