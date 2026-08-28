;; test/lisp/ide_test.lisp
;;
;; src/lisp/ide.lispのSecondary IDEチャネル(0x170-0x177)動作確認。
;; QEMU側はMakefileの$(IDE_DISK_IMG)ターゲットが作成する16MBの使い捨てディスクを
;; bus=ide.1,unit=0でアタッチしており、先頭にIDE_TEST_MAGICを書き込んでいる。
;;
;; 本ファイルはtest/lisp/test_framework.lisp(assert-equal等)とsrc/lisp/ide.lisp
;; (*ide-device*/read-sector/write-sector/%ide-take)を、本ファイルより先にboot-entry
;; スクリプトがloadしている前提で書かれている。

;;; --- 1. Secondary IDEチャネルにデバイスが検出されている ---

(assert-equal t (if *ide-device* t nil))

;; IDENTIFYが報告する総セクタ数は0より大きい(16MBイメージなのでかなり大きいはず)
(assert-equal t (> (%%ide-total-sectors *ide-device*) 0))

;;; --- 2. セクタ0はMakefileが書き込んだIDE_TEST_MAGICで始まる ---

;; "ISIKIOS-IDE-TEST-SECTOR0-MAGIC!" の各文字のASCIIコード(先頭31byte分)
(defglobal ide-test-magic-bytes
  '(73 83 73 75 73 79 83 45 73 68 69 45 84 69 83 84 45 83 69 67 84 79 82 48 45 77 65 71 73 67 33))

(assert-equal ide-test-magic-bytes (%ide-take (read-sector *ide-device* 0) 31))

;;; --- 3. sector1への書き込み→読み込みの往復一致 ---

(defglobal ide-test-pattern-bytes
  (for ((i 511 (- i 1))
        (result nil (cons (mod i 256) result)))
      ((< i 0) result)))

(assert-equal t (if (write-sector *ide-device* 1 ide-test-pattern-bytes) t nil))
(assert-equal ide-test-pattern-bytes (read-sector *ide-device* 1))
