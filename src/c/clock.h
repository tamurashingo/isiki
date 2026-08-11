#ifndef _CLOCK_H_
#define _CLOCK_H_

#include "types.h"

/**
 * 組み込み関数GET-UNIVERSAL-TIME。UEFI GetTimeで取得した起動時UTC
 * (kernel_get_boot_epoch_seconds)にPIT tickカウンタによる起動後経過秒数を
 * 加算したUniversal Time Formatの値を返す
 * @param args 未使用(引数無し)
 * @param env 未使用
 * @return 現在時刻(Universal Time Format, 1900-01-01からの経過秒数)のfixnum
 */
lisp_val_t primitive_get_universal_time(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数GET-INTERNAL-REAL-TIME。起動後のPIT tick数をそのまま返す
 * @param args 未使用(引数無し)
 * @param env 未使用
 * @return 起動後のtick数のfixnum
 */
lisp_val_t primitive_get_internal_real_time(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数GET-INTERNAL-RUN-TIME。プロセス単位のCPU時間計測が本OSに無いため、
 * GET-INTERNAL-REAL-TIMEと同じPIT tickカウンタを流用する簡略実装
 * @param args 未使用(引数無し)
 * @param env 未使用
 * @return 起動後のtick数のfixnum
 */
lisp_val_t primitive_get_internal_run_time(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数INTERNAL-TIME-UNITS-PER-SECOND。PITの分周設定(約100Hz)に対応する
 * 1秒あたりのinternal time unit数を返す
 * @param args 未使用(引数無し)
 * @param env 未使用
 * @return 100のfixnum
 */
lisp_val_t primitive_internal_time_units_per_second(lisp_val_t args, lisp_val_t env);

/**
 * GET-UNIVERSAL-TIME/GET-INTERNAL-REAL-TIME/GET-INTERNAL-RUN-TIME/
 * INTERNAL-TIME-UNITS-PER-SECONDをglobal_environmentに登録する
 */
void os_register_clock(void);

#endif /* _CLOCK_H_ */
