;;;; Secondary IDEチャネル(0x170-0x177)のIDE/ATA HDDをPIOモードで読み書きするための
;;;; Lisp側API。ファイルシステムは実装しない(セクタ単位の読み書きのみ)。
;;;;
;;;; utility.lisp/init_aot.lispとは異なりAOTトランスパイル対象外の、通常のload形式
;;;; の(インタプリタ実行専用)ファイル。REPLから(load "src/lisp/device.lisp")の後に
;;;; (load "src/lisp/ide.lisp")で明示的に読み込む。文字リテラル(#\X)はAOT
;;;; トランスパイラでは未対応だが、このファイルはインタプリタ実行のみなので
;;;; 使用できる。動作確認が完了したら、utility.lisp/init_aot.lispと同様にAOT
;;;; トランスパイル対象へ移行しImmobilized Spaceを節約する(transpile.lispの制約=
;;;; 単一式body・仮引数はシンボルのみ、に合わせて書き直しが必要な後続タスクとして
;;;; 別途行う)。

;; os_block_device_probe_all(C側、kernel_main内でLisp起動前に実行済み)が検出した
;; Secondary IDEチャネルのデバイスを、*devices*(device.lisp)へblk0,blk1,...として
;; 登録する。個々のデバイスへは*devices*/(%device-handle 'blk0)経由でアクセスする
;; (*ide-device*のような単一デバイス専用のグローバルは持たない)。
(defun %ide-register-devices ()
  (%ide-register-devices-loop 0 (%%ide-device-count)))

;; (%ide-register-devices-loop i count) : C側レジストリのindex iからcountまでを
;; 順に%device-register-blkへ渡す再帰ヘルパー。デバイス数は現状最大2台なので
;; 素の再帰で十分(eval.cはTCO非対応だが、この程度の深さはスタックに問題ない)。
(defun %ide-register-devices-loop (i count)
  (if (>= i count)
      nil
      (progn
        (%device-register-blk
          (let ((dev (%%ide-device-at i)))
            (list ':type 'blk
                  ':handle dev
                  ':name (%%ide-device-name dev)
                  ':model (%%ide-device-model dev)
                  ':total-sectors (%%ide-total-sectors dev))))
        (%ide-register-devices-loop (+ i 1) count))))

(%ide-register-devices)

;; (%ide-bytes-from-addr addr offset count) : addr+offsetを先頭にcount byte分を
;; %%peekで読み、0番目が先頭になるfixnum(0-255)のリストを返す。
(defun %ide-bytes-from-addr (addr offset count)
  (for ((i (- count 1) (- i 1))
        (result nil (cons (%%peek (+ addr (+ offset i))) result)))
      ((< i 0) result)))

;; (%ide-bytes-to-addr addr offset bytes) : bytesの各要素(fixnum 0-255)を
;; addr+offsetから順に%%pokeで書き込む。
(defun %ide-bytes-to-addr (addr offset bytes)
  (progn
    (for ((b bytes (cdr b))
          (i offset (+ i 1)))
        ((null b) nil)
      (%%poke (+ addr i) (car b)))
    nil))

;; (read-sector device lba) : deviceからlba番目の1セクタ(512byte)を読み込み、
;; 先頭バイトが先頭要素になる512要素のfixnumリストを返す。失敗時はnil。
(defun read-sector (device lba)
  (if (%%ide-read-sector device lba)
      (%ide-bytes-from-addr (%%ide-sector-buffer-address device) 0 512)
      nil))

;; (write-sector device lba bytes) : 512要素のfixnumリストbytesをdeviceのlba番目の
;; セクタへ書き込む(内部でCACHE FLUSHまで実行する)。成功時t、失敗時nil。
;; bytesの要素数が512でない場合はnil。
(defun write-sector (device lba bytes)
  (if (= (length bytes) 512)
      (progn
        (%ide-bytes-to-addr (%%ide-sector-buffer-address device) 0 bytes)
        (%%ide-write-sector device lba))
      nil))

;; (%hex-nibble-code n) : 0-15の値をASCII文字コード(fixnum)にする(0-9->'0'-'9', 10-15->'A'-'F')。
(defun %hex-nibble-code (n)
  (if (< n 10)
      (+ n 48)
      (+ (- n 10) 65)))

;; (%hex-byte-write out byte) : byte(0-255)を2桁の16進文字としてoutへ書き出す。
;; format.cの~Xはゼロ埋め(,'0)に未対応なため、ニブルごとに手動で変換して~Cで出力する。
(defun %hex-byte-write (out byte)
  (format out "~C~C" (%hex-nibble-code (div byte 16)) (%hex-nibble-code (mod byte 16))))

;; (%hex-offset-write out offset) : offset(0-65535程度)を4桁の16進数としてoutへ書き出す。
(defun %hex-offset-write (out offset)
  (progn
    (%hex-byte-write out (div offset 256))
    (%hex-byte-write out (mod offset 256))))

;; (%hex-dump-row out bytes offset) : bytes(16要素のfixnumリスト)を
;; "OFFS: XX XX ... XX" の1行としてoutへ書き出す。
(defun %hex-dump-row (out bytes offset)
  (progn
    (%hex-offset-write out offset)
    (format out ": ")
    (for ((b bytes (cdr b)))
        ((null b) nil)
      (progn
        (%hex-byte-write out (car b))
        (format out " ")))
    (format out "~%")))

;; (%hex-dump-rows out bytes offset) : bytesを16byteずつ%hex-dump-rowへ渡す再帰ヘルパー。
(defun %hex-dump-rows (out bytes offset)
  (if (null bytes)
      nil
      (progn
        (%hex-dump-row out (%ide-take bytes 16) offset)
        (%hex-dump-rows out (%ide-drop bytes 16) (+ offset 16)))))

;; (%ide-take list n) : listの先頭n要素からなるリストを返す。
(defun %ide-take (list n)
  (if (or (null list) (= n 0))
      nil
      (cons (car list) (%ide-take (cdr list) (- n 1)))))

;; (%ide-drop list n) : listの先頭n要素を取り除いた残りを返す。
(defun %ide-drop (list n)
  (if (or (null list) (= n 0))
      list
      (%ide-drop (cdr list) (- n 1))))

;; (%ide-output-stream) : *standard-output*が動的束縛されていれば(with-standard-output
;; 経由等)それを使い、なければ画面への新規出力ストリームを開く(utility.lispのroomと
;; 同じ慣習)。
(defun %ide-output-stream ()
  (if (dynamic *standard-output*)
      (dynamic *standard-output*)
      (open-output-stream)))

;; (hex-dump-bytes bytes) : bytes(512要素程度のfixnumリスト)を16byte/行で
;; 標準出力へヘキサダンプする。
(defun hex-dump-bytes (bytes)
  (progn
    (%hex-dump-rows (%ide-output-stream) bytes 0)
    nil))
