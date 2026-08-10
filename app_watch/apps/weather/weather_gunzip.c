/* tinf - tiny inflate library - public domain
 * Based on Joergen Ibsen's tinf algorithm.
 * Supports raw deflate and gzip wrapper.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

static uint32_t tinf_read_bits(const uint8_t *src, int bitpos, int nbits)
{
    uint32_t val = 0;
    for (int i = 0; i < nbits; i++)
        val |= (uint32_t)((src[(bitpos + i) >> 3] >> ((bitpos + i) & 7)) & 1) << i;
    return val;
}

typedef struct {
    const uint8_t *src;
    int bitpos;
    uint8_t *dst;
    size_t dstlen;
    size_t dstpos;
} tinf_state;

static uint32_t tinf_bits(tinf_state *s, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        int bp = s->bitpos++;
        v |= (uint32_t)((s->src[bp >> 3] >> (bp & 7)) & 1) << i;
    }
    return v;
}

#define TINF_TREE_SIZE 16

typedef struct {
    uint8_t counts[TINF_TREE_SIZE];
    uint8_t *symbols;
} tinf_tree;

static int tinf_build_tree(tinf_tree *t, const uint8_t *lengths, int n, uint16_t *sym_buf)
{
    int offsets[TINF_TREE_SIZE];
    t->symbols = sym_buf;
    memset(t->counts, 0, sizeof(t->counts));
    for (int i = 0; i < n; i++) if (lengths[i]) t->counts[lengths[i]]++;
    int total = 0;
    for (int i = 1; i < TINF_TREE_SIZE; i++) {
        offsets[i] = total;
        total += t->counts[i];
    }
    for (int i = 0; i < n; i++)
        if (lengths[i]) sym_buf[offsets[lengths[i]]++] = (uint16_t)i;
    return total;
}

static int tinf_decode_symbol(tinf_state *s, tinf_tree *t)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        code = (code << 1) | (int)tinf_bits(s, 1);
        int cnt = (len < TINF_TREE_SIZE) ? t->counts[len] : 0;
        if (code < first + cnt) return t->symbols[index + code - first];
        index += cnt;
        first = (first + cnt) << 1;
    }
    return -1;
}

static const int tinf_lbase[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258
};
static const int tinf_lext[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const int tinf_dbase[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,
    1537,2049,3073,4097,6145,8193,12289,16385,24577
};
static const int tinf_dext[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
};

static int tinf_inflate_block(tinf_state *s, tinf_tree *lt, tinf_tree *dt)
{
    for (;;) {
        int sym = tinf_decode_symbol(s, lt);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (s->dstpos >= s->dstlen) return -1;
            s->dst[s->dstpos++] = (uint8_t)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            int length = tinf_lbase[sym] + (int)tinf_bits(s, tinf_lext[sym]);
            sym = tinf_decode_symbol(s, dt);
            if (sym < 0 || sym >= 30) return -1;
            int dist = tinf_dbase[sym] + (int)tinf_bits(s, tinf_dext[sym]);
            if ((size_t)dist > s->dstpos) return -1;
            if (s->dstpos + length > s->dstlen) return -1;
            for (int i = 0; i < length; i++) {
                s->dst[s->dstpos] = s->dst[s->dstpos - dist];
                s->dstpos++;
            }
        }
    }
}

static int tinf_inflate_uncompressed(tinf_state *s)
{
    s->bitpos = (s->bitpos + 7) & ~7;
    int len = (int)tinf_bits(s, 16);
    tinf_bits(s, 16);
    for (int i = 0; i < len; i++) {
        if (s->dstpos >= s->dstlen) return -1;
        s->dst[s->dstpos++] = s->src[s->bitpos >> 3];
        s->bitpos += 8;
    }
    return 0;
}

static int tinf_inflate_fixed(tinf_state *s)
{
    uint8_t *flens = malloc(288), *fdens = malloc(32);
    uint8_t *ftree = malloc(288), *dtree = malloc(32);
    if (!flens || !fdens || !ftree || !dtree) {
        free(flens); free(fdens); free(ftree); free(dtree);
        return -1;
    }
    for (int i =   0; i < 144; i++) flens[i] = 8;
    for (int i = 144; i < 256; i++) flens[i] = 9;
    for (int i = 256; i < 280; i++) flens[i] = 7;
    for (int i = 280; i < 288; i++) flens[i] = 8;
    for (int i = 0; i < 32; i++) fdens[i] = 5;
    tinf_tree lt, dt;
    tinf_build_tree(&lt, flens, 288, ftree);
    tinf_build_tree(&dt, fdens, 32, dtree);
    int ret = tinf_inflate_block(s, &lt, &dt);
    free(flens); free(fdens); free(ftree); free(dtree);
    return ret;
}

static int tinf_inflate_dynamic(tinf_state *s)
{
    static const int clen_order[19] = {
        16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
    };
    int hlit  = (int)tinf_bits(s, 5) + 257;
    int hdist = (int)tinf_bits(s, 5) + 1;
    int hclen = (int)tinf_bits(s, 4) + 4;

    uint8_t clens[19] = {0};
    for (int i = 0; i < hclen; i++) clens[clen_order[i]] = (uint8_t)tinf_bits(s, 3);
    uint16_t ctree_buf[19];
    tinf_tree ct;
    tinf_build_tree(&ct, clens, 19, ctree_buf);

    uint8_t lengths[286 + 30] = {0};
    int n = hlit + hdist, idx = 0;
    while (idx < n) {
        int sym = tinf_decode_symbol(s, &ct);
        if (sym < 0) return -1;
        if (sym < 16) {
            lengths[idx++] = (uint8_t)sym;
        } else {
            int rep = 0;
            uint8_t val = 0;
            if (sym == 16) { rep = (int)tinf_bits(s, 2) + 3; if (idx == 0) return -1; val = lengths[idx-1]; }
            else if (sym == 17) { rep = (int)tinf_bits(s, 3) + 3; }
            else if (sym == 18) { rep = (int)tinf_bits(s, 7) + 11; }
            else return -1;
            for (int i = 0; i < rep && idx < n; i++) lengths[idx++] = val;
        }
    }

    uint16_t *ltree_buf = malloc(sizeof(uint16_t) * 286);
    uint16_t *dtree_buf = malloc(sizeof(uint16_t) * 30);
    if (!ltree_buf || !dtree_buf) {
        free(ltree_buf); free(dtree_buf);
        return -1;
    }
    tinf_tree lt, dt;
    tinf_build_tree(&lt, lengths, hlit, ltree_buf);
    tinf_build_tree(&dt, lengths + hlit, hdist, dtree_buf);
    int ret = tinf_inflate_block(s, &lt, &dt);
    free(ltree_buf); free(dtree_buf);
    return ret;
}

int weather_gunzip(const uint8_t *in, size_t inlen, uint8_t *out, size_t *outlen)
{
    if (inlen < 18 || in[0] != 0x1f || in[1] != 0x8b || in[2] != 8) return -1;

    int flg = in[3];
    size_t pos = 10;
    if (flg & 0x04) {
        if (pos + 4 > inlen) return -1;
        pos += 4 + (size_t)(in[pos] | (in[pos+1] << 8));
    }
    if (flg & 0x08) { while (pos < inlen && in[pos]) pos++; pos++; }
    if (flg & 0x10) { while (pos < inlen && in[pos]) pos++; pos++; }
    if (flg & 0x02) pos += 2;
    if (pos >= inlen) return -1;

    tinf_state s;
    s.src = in + pos;
    s.bitpos = 0;
    s.dst = out;
    s.dstlen = *outlen;
    s.dstpos = 0;

    /* Strip trailing CRC32 + ISIZE (8 bytes) */
    size_t srclen = inlen - pos;
    if (srclen < 8) return -1;
    srclen -= 8;

    /* Inflate deflate stream */
    int last;
    do {
        if ((s.bitpos >> 3) >= srclen) return -1;
        last = (int)tinf_bits(&s, 1);
        int type = (int)tinf_bits(&s, 2);
        int ret;
        if (type == 0) ret = tinf_inflate_uncompressed(&s);
        else if (type == 1) ret = tinf_inflate_fixed(&s);
        else if (type == 2) ret = tinf_inflate_dynamic(&s);
        else return -1;
        if (ret != 0) return -1;
    } while (!last);

    *outlen = s.dstpos;
    return 0;
}
