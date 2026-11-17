// flac A/B 探针：单声道 16bit 样本 → FLAC 流式编码（内存回调）→ 流式解码 → 比对 PCM。
// flac 是纯整数变换（residual 预测 + Rice 编码），编码与解码都应 bit-exact。
#include <FLAC/stream_decoder.h>
#include <FLAC/stream_encoder.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NSAMP 4096
#define SRATE 44100

typedef struct { unsigned char *buf; size_t cap, len; } MemBuf;

static uint64_t fnv1a(const void *p, size_t n)
{
    const uint8_t *b = p;
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
    return h;
}

static FLAC__StreamEncoderWriteStatus enc_write(const FLAC__StreamEncoder *enc,
        const FLAC__byte buf[], size_t bytes, unsigned samples, unsigned ch, void *cd)
{
    MemBuf *m = cd;
    (void)enc; (void)samples; (void)ch;
    if (m->len + bytes > m->cap) return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
    memcpy(m->buf + m->len, buf, bytes);
    m->len += bytes;
    return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
}

/* 解码器所有回调共用一个 client_data，读写状态必须放在同一个结构里 */
typedef struct {
    const unsigned char *buf; size_t len, pos;
    int32_t *pcm; size_t pcm_cap, pcm_len;
} DecCtx;

static FLAC__StreamDecoderReadStatus dec_read(const FLAC__StreamDecoder *d,
        FLAC__byte buf[], size_t *bytes, void *cd)
{
    DecCtx *c = cd;
    (void)d;
    if (*bytes <= 0) return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
    size_t n = c->len - c->pos;
    if (n == 0) return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
    if (n > *bytes) n = *bytes;
    memcpy(buf, c->buf + c->pos, n);
    c->pos += n;
    *bytes = n;
    return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

static FLAC__StreamDecoderWriteStatus dec_write(const FLAC__StreamDecoder *d,
        const FLAC__Frame *frame, const FLAC__int32 *const buffer[], void *cd)
{
    DecCtx *c = cd;
    (void)d;
    for (unsigned i = 0; i < frame->header.blocksize; i++) {
        if (c->pcm_len >= c->pcm_cap) return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
        c->pcm[c->pcm_len++] = buffer[0][i];
    }
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void dec_meta(const FLAC__StreamDecoder *d, const FLAC__StreamMetadata *m, void *cd)
{ (void)d; (void)m; (void)cd; }

static void dec_error(const FLAC__StreamDecoder *d, FLAC__StreamDecoderErrorStatus s, void *cd)
{
    (void)cd;
    fprintf(stderr, "解码错误：%s\n", FLAC__StreamDecoderErrorStatusString[s]);
}

int main(int argc, char **argv)
{
    const char *outp = argc > 1 ? argv[1] : NULL;
    static int32_t pcm_in[NSAMP], pcm_out[NSAMP];

    /* 确定性样本：混合渐变 + 锯齿，让预测器与 Rice 参数都动起来 */
    for (int i = 0; i < NSAMP; i++) {
        unsigned int r = (unsigned int)i * 1103515245u + 12345u;
        int v = (i % 512) * 40 - 10240 + (int)((r >> 18) & 0x3ff) - 512;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        pcm_in[i] = v;
    }

    MemBuf enc = { 0 };
    enc.cap = NSAMP * 4 + 4096;
    enc.buf = malloc(enc.cap);
    if (!enc.buf) return 2;

    FLAC__StreamEncoder *se = FLAC__stream_encoder_new();
    if (!se) return 2;
    FLAC__stream_encoder_set_channels(se, 1);
    FLAC__stream_encoder_set_bits_per_sample(se, 16);
    FLAC__stream_encoder_set_sample_rate(se, SRATE);
    FLAC__stream_encoder_set_compression_level(se, 5);
    FLAC__StreamEncoderInitStatus ist =
        FLAC__stream_encoder_init_stream(se, enc_write, NULL, NULL, NULL, &enc);
    if (ist != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
        fprintf(stderr, "编码器 init 失败：%s\n", FLAC__StreamEncoderInitStatusString[ist]);
        return 3;
    }
    if (!FLAC__stream_encoder_process_interleaved(se, pcm_in, NSAMP)) return 4;
    FLAC__stream_encoder_finish(se);
    FLAC__stream_encoder_delete(se);

    DecCtx ctx = { 0 };
    ctx.pcm = pcm_out;
    ctx.pcm_cap = NSAMP;
    /* 落在临时文件上解码：内存回调的 read/seek 语义容易出 LOST_SYNC，文件路径最稳 */
    {
        FILE *tf = fopen("/tmp/_flac_ab.flac", "wb");
        if (!tf) return 5;
        fwrite(enc.buf, 1, enc.len, tf);
        fclose(tf);
    }
    FLAC__StreamDecoder *sd = FLAC__stream_decoder_new();
    if (!sd) return 2;
    FLAC__stream_decoder_set_metadata_respond_all(sd);
    FLAC__StreamDecoderInitStatus dst = FLAC__stream_decoder_init_file(
        sd, "/tmp/_flac_ab.flac", dec_write, dec_meta, dec_error, &ctx);
    if (!FLAC__stream_decoder_process_until_end_of_stream(sd)) return 4;
    FLAC__stream_decoder_finish(sd);
    FLAC__stream_decoder_delete(sd);

    if (outp) {
        FILE *f = fopen(outp, "wb");
        if (!f) { fprintf(stderr, "打不开 %s\n", outp); return 5; }
        fwrite(enc.buf, 1, enc.len, f);
        fwrite(ctx.pcm, sizeof(int32_t), ctx.pcm_len, f);
        fclose(f);
    }

    printf("samples=%d flac=%zu dec_samples=%zu fnv_stream=%016llx fnv_pcm=%016llx\n",
           NSAMP, enc.len, ctx.pcm_len,
           (unsigned long long)fnv1a(enc.buf, enc.len),
           (unsigned long long)fnv1a(ctx.pcm, ctx.pcm_len * sizeof(int32_t)));
    free(enc.buf);
    return 0;
}
