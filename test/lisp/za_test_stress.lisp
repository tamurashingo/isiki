;; test/lisp/za_test_stress.lisp
;;
;; za_test.lisp(拡張1/4/6)が定義するループ関数を大量N(50000)で呼び、GC(os_gc_collect)
;; を繰り返し誘発しても結果が破壊されないことを確認する負荷テスト。
;;
;; za_test_ext5.lispとは異なり本ファイルは自己完結していない: isiki-za-test-cons-chain/
;; isiki-za-test-adder-chain/isiki-za-test-sum-closure-chain/isiki-za-test-dyn-stress/
;; isiki-za-test-all-eq-sym はすべてza_test.lisp側の定義を再利用する前提であり、
;; boot-entryスクリプトが本ファイルより先にza_test.lispをloadしていることを要求する。
;;
;; GitHub Actions側のQEMU実行(milestone 2)はハードウェア支援仮想化(KVM)を欠き
;; TCG(ソフトウェアCPUエミュレーション)へフォールバックするため、この負荷テストは
;; ローカル実行専用とし、CIからはloadしない。通常の make test-qemu(qemu_boot_test.lisp)
;; からも意図的に外してあり、make test-qemu-stress(qemu_boot_m2_za_stress.lisp)から
;; のみ到達する。
;;
;; 本ファイルは test/lisp/test_framework.lisp が定義する assert-equal 等をそのまま
;; 使う(boot-entryスクリプトが本ファイルより先にそれをloadしている前提)。

;; 大量にconsを呼び続けてGC(os_gc_collect)を誘発しても、za生成コードが結果を破壊
;; しないことを確認する(os_make_cons自身のGC_PROTECTだけで安全という設計の裏付け)。
(close (open-output-file "tmp/ckpt-8-before-chain.txt"))

(assert-equal 50000 (length (isiki-za-test-cons-chain 50000)))
(close (open-output-file "tmp/ckpt-9-after-chain.txt"))
(assert-equal 49999 (car (isiki-za-test-cons-chain 50000)))
(close (open-output-file "tmp/ckpt-10-final.txt"))

;; 大量にクロージャを生成し続けてGC(os_gc_collect)を誘発しても、古いクロージャ・
;; 新しいクロージャどちらも正しい値を返すことを確認する
(close (open-output-file "tmp/ckpt-17-before-closure-chain.txt"))

(assert-equal 50000 (length (isiki-za-test-adder-chain 50000)))
(close (open-output-file "tmp/ckpt-18-after-closure-chain.txt"))

;; sum_{i=0}^{49999} (i+1) = sum_{i=0}^{49999} i + 50000 = 49999*50000/2 + 50000 = 1250025000
(assert-equal 1250025000 (isiki-za-test-sum-closure-chain (isiki-za-test-adder-chain 50000)))
(close (open-output-file "tmp/ckpt-19-final.txt"))

;; 大量にdefdynamicを呼び続けてGC(os_gc_collect)を誘発しても、戻り値(name)が
;; コンパイル時に埋め込んだシンボルとeqであり続けること(os_set_dynamic呼び出しを
;; 挟んだ後の再解決が正しいことの確認)、および動的値自体も最終的に正しいことを確認する
(assert-equal 50000 (length (isiki-za-test-dyn-stress 50000)))
(assert-equal t (isiki-za-test-all-eq-sym (isiki-za-test-dyn-stress 50000) '*iza-test-dyn2*))
(assert-equal 49999 (dynamic *iza-test-dyn2*))
(close (open-output-file "tmp/ckpt-20-defdynamic.txt"))
