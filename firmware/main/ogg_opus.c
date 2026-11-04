#include "ogg_opus.h"

#include <string.h>

#include "esp_random.h"

#define PAGE_PACKETS 50  // 50 x 20 ms: one page per second

#define FLAG_BOS 0x02
#define FLAG_EOS 0x04

static uint32_t s_crc_table[256];

// Ogg uses CRC-32 with polynomial 0x04C11DB7, no reflection, initial value 0.
static void crc_init(void)
{
    if (s_crc_table[1]) {
        return;
    }
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t r = i << 24;
        for (int b = 0; b < 8; b++) {
            r = (r & 0x80000000u) ? (r << 1) ^ 0x04C11DB7u : r << 1;
        }
        s_crc_table[i] = r;
    }
}

static uint32_t crc_update(uint32_t crc, const uint8_t *p, size_t n)
{
    while (n--) {
        crc = (crc << 8) ^ s_crc_table[((crc >> 24) ^ *p++) & 0xFF];
    }
    return crc;
}

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = v;
    p[1] = v >> 8;
}

static void put_le32(uint8_t *p, uint32_t v)
{
    put_le16(p, v);
    put_le16(p + 2, v >> 16);
}

static bool write_page(ogg_opus_t *o, uint8_t flags, uint64_t granule, const uint8_t *segs, int seg_count,
                       const uint8_t *body, size_t body_len)
{
    uint8_t hdr[27 + 255];
    memcpy(hdr, "OggS", 4);
    hdr[4] = 0;  // version
    hdr[5] = flags;
    put_le32(hdr + 6, (uint32_t)granule);
    put_le32(hdr + 10, (uint32_t)(granule >> 32));
    put_le32(hdr + 14, o->serial);
    put_le32(hdr + 18, o->page_seq++);
    put_le32(hdr + 22, 0);  // CRC, filled in below
    hdr[26] = seg_count;
    memcpy(hdr + 27, segs, seg_count);
    size_t hdr_len = 27 + seg_count;

    uint32_t crc = crc_update(0, hdr, hdr_len);
    crc = crc_update(crc, body, body_len);
    put_le32(hdr + 22, crc);

    return fwrite(hdr, 1, hdr_len, o->f) == hdr_len && fwrite(body, 1, body_len, o->f) == body_len;
}

// Writes a header packet (OpusHead/OpusTags) as a page of its own.
static bool write_header_page(ogg_opus_t *o, uint8_t flags, const uint8_t *packet, size_t len)
{
    uint8_t segs[255];
    int n = 0;
    size_t left = len;
    while (left >= 255) {
        segs[n++] = 255;
        left -= 255;
    }
    segs[n++] = left;
    return write_page(o, flags, 0, segs, n, packet, len);
}

bool ogg_opus_open(ogg_opus_t *o, const char *path, uint32_t input_rate, uint8_t channels, uint16_t pre_skip)
{
    crc_init();
    memset(o, 0, sizeof(*o));
    o->serial = esp_random();
    o->f = fopen(path, "wb");
    if (!o->f) {
        return false;
    }

    uint8_t head[19];
    memcpy(head, "OpusHead", 8);
    head[8] = 1;  // version
    head[9] = channels;
    put_le16(head + 10, pre_skip);
    put_le32(head + 12, input_rate);
    put_le16(head + 16, 0);  // output gain
    head[18] = 0;            // channel mapping family 0 (mono/stereo)

    static const char vendor[] = "recordPan";
    uint8_t tags[8 + 4 + sizeof(vendor) - 1 + 4];
    memcpy(tags, "OpusTags", 8);
    put_le32(tags + 8, sizeof(vendor) - 1);
    memcpy(tags + 12, vendor, sizeof(vendor) - 1);
    put_le32(tags + 12 + sizeof(vendor) - 1, 0);  // no user comments

    if (!write_header_page(o, FLAG_BOS, head, sizeof(head)) || !write_header_page(o, 0, tags, sizeof(tags))) {
        fclose(o->f);
        o->f = NULL;
        return false;
    }
    return true;
}

static bool flush_page(ogg_opus_t *o, uint8_t flags)
{
    bool ok = write_page(o, flags, o->granule, o->segs, o->seg_count, o->body, o->body_len);
    o->seg_count = 0;
    o->packet_count = 0;
    o->body_len = 0;
    return ok;
}

bool ogg_opus_write(ogg_opus_t *o, const uint8_t *packet, size_t len, uint32_t samples_48k)
{
    bool ok = true;
    int needed = len / 255 + 1;
    if (o->seg_count + needed > 255 || o->body_len + len > sizeof(o->body)) {
        ok = flush_page(o, 0);
    }
    size_t left = len;
    while (left >= 255) {
        o->segs[o->seg_count++] = 255;
        left -= 255;
    }
    o->segs[o->seg_count++] = left;
    memcpy(o->body + o->body_len, packet, len);
    o->body_len += len;
    o->granule += samples_48k;

    if (++o->packet_count >= PAGE_PACKETS) {
        ok = flush_page(o, 0) && ok;
    }
    return ok;
}

bool ogg_opus_flush(ogg_opus_t *o)
{
    return o->seg_count == 0 || flush_page(o, 0);
}

void ogg_opus_close(ogg_opus_t *o)
{
    if (!o->f) {
        return;
    }
    // An EOS page may carry no packets; it then just terminates the stream.
    flush_page(o, FLAG_EOS);
    fclose(o->f);
    o->f = NULL;
}
