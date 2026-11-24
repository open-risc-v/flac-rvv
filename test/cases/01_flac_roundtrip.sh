#!/usr/bin/env bash
# flac 真实门禁：单声道 16bit 样本 → 流式编码（内存回调）→ 流式解码 → 比对 PCM。
# 被测路径是 libFLAC 的 LPC 残差计算（RVV 内核在这里生效），产物是编码流 + 解码 PCM。
# 编码与解码都是纯整数路径，A/B 必须逐字节一致。
set -euo pipefail

: "${STAGE:=run}" "${SIDE:=A}" "${BIN:=build/native}" "${DATA:=build/ab-data}" "${OUT:=build/ab-data/out}"
mkdir -p "$DATA" "$OUT"

case "$STAGE" in
  gen)
    # 输入由探针内部合成（固定 4096 点 44.1 kHz 单声道），不需要外部语料
    ;;
  run)
    if [ ! -x "$BIN/flac_harness" ]; then
      echo "缺少 $BIN/flac_harness（先编上游 libFLAC 再编探针）" >&2
      exit 1
    fi
    ${RUNNER:-} "$BIN/flac_harness" "$OUT/flac_stream.bin" > "$OUT/hash.txt"
    ;;
  *)
    echo "未知 STAGE=$STAGE" >&2; exit 2 ;;
esac
