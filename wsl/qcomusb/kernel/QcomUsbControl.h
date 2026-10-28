/* SPDX-License-Identifier: MIT
 * Bounded, allocation-free control framing shared by the kernel control
 * worker and its host-side conformance tests. This module never performs I/O
 * and contains no memory, function, or arbitrary register operations.
 */
#ifndef QCOM_USB_CONTROL_H
#define QCOM_USB_CONTROL_H

#include <stddef.h>
#include <stdint.h>

/* The newline is part of the 128-byte request-frame ceiling. */
#define SXNU_CONTROL_FRAME_BYTES 128u
#define SXNU_CONTROL_LINE_BYTES 127u
#define SXNU_CONTROL_REPLY_BYTES 512u
#define SXNU_CONTROL_WORK_BYTES 256u
#define SXNU_CONTROL_SESSION_HEX 16u
#define SXNU_CONTROL_SEQ_HEX 8u
#define SXNU_CONTROL_CRC_HEX 8u

enum sxnu_control_verb {
    SXNU_CONTROL_VERB_INVALID = 0,
    SXNU_CONTROL_VERB_HELLO,
    SXNU_CONTROL_VERB_PING,
    SXNU_CONTROL_VERB_STATUS,
    SXNU_CONTROL_VERB_REBOOT,
    SXNU_CONTROL_VERB_SCREENSHOT,
    SXNU_CONTROL_VERB_SHOTNEXT
};

struct sxnu_control_frame {
    uint64_t session;
    uint32_t seq;
    uint32_t crc;
    enum sxnu_control_verb verb;
};

struct sxnu_control_parser {
    uint8_t line[SXNU_CONTROL_FRAME_BYTES];
    uint16_t used;
    uint8_t discarding;
    uint32_t overlong;
};

struct sxnu_control_state {
    uint64_t session;
    uint32_t last_seq;
    uint32_t generation;
};

struct sxnu_control_writer {
    char *out;
    size_t capacity;
    size_t used;
    uint8_t failed;
};

static inline uint32_t
sxnu_control_crc32(const void *bytes, size_t count)
{
    const uint8_t *p = bytes;
    uint32_t crc = 0xffffffffu;
    size_t i;

    for (i = 0; i < count; i++) {
        unsigned int bit;
        crc ^= p[i];
        for (bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static inline int
sxnu_control_hex_value(uint8_t c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static inline int
sxnu_control_parse_hex(const uint8_t *text, size_t digits, uint64_t *value)
{
    uint64_t result = 0;
    size_t i;

    if (!value || !digits || digits > 16u) return 0;
    for (i = 0; i < digits; i++) {
        int digit = sxnu_control_hex_value(text[i]);
        if (digit < 0) return 0;
        result = (result << 4) | (uint64_t)digit;
    }
    *value = result;
    return 1;
}

static inline enum sxnu_control_verb
sxnu_control_parse_verb(const uint8_t *text, size_t bytes)
{
    if (bytes == 5u && text[0] == 'H' && text[1] == 'E' && text[2] == 'L' &&
        text[3] == 'L' && text[4] == 'O') return SXNU_CONTROL_VERB_HELLO;
    if (bytes == 4u && text[0] == 'P' && text[1] == 'I' && text[2] == 'N' &&
        text[3] == 'G') return SXNU_CONTROL_VERB_PING;
    if (bytes == 6u && text[0] == 'S' && text[1] == 'T' && text[2] == 'A' &&
        text[3] == 'T' && text[4] == 'U' && text[5] == 'S')
        return SXNU_CONTROL_VERB_STATUS;
    if (bytes == 6u && text[0] == 'R' && text[1] == 'E' && text[2] == 'B' &&
        text[3] == 'O' && text[4] == 'O' && text[5] == 'T')
        return SXNU_CONTROL_VERB_REBOOT;
    if (bytes == 10u && text[0] == 'S' && text[1] == 'C' && text[2] == 'R' &&
        text[3] == 'E' && text[4] == 'E' && text[5] == 'N' && text[6] == 'S' &&
        text[7] == 'H' && text[8] == 'O' && text[9] == 'T') return SXNU_CONTROL_VERB_SCREENSHOT;
    if (bytes == 8u && text[0] == 'S' && text[1] == 'H' && text[2] == 'O' &&
        text[3] == 'T' && text[4] == 'N' && text[5] == 'E' && text[6] == 'X' &&
        text[7] == 'T') return SXNU_CONTROL_VERB_SHOTNEXT;
    return SXNU_CONTROL_VERB_INVALID;
}

static inline const char *
sxnu_control_verb_name(enum sxnu_control_verb verb)
{
    switch (verb) {
    case SXNU_CONTROL_VERB_HELLO: return "HELLO";
    case SXNU_CONTROL_VERB_PING: return "PING";
    case SXNU_CONTROL_VERB_STATUS: return "STATUS";
    case SXNU_CONTROL_VERB_REBOOT: return "REBOOT";
    case SXNU_CONTROL_VERB_SCREENSHOT: return "SCREENSHOT";
    case SXNU_CONTROL_VERB_SHOTNEXT: return "SHOTNEXT";
    default: return "";
    }
}

static inline void
sxnu_control_parser_reset(struct sxnu_control_parser *parser)
{
    if (!parser) return;
    parser->used = 0;
    parser->discarding = 0;
}

/* Feed one byte. Returns 1 for a complete line, 0 while waiting, and -1 when
 * an overlong line was discarded through its newline. The parser has no
 * allocation and never interprets a partial frame. */
static inline int
sxnu_control_parser_feed(struct sxnu_control_parser *parser, uint8_t byte,
                         char *line, size_t line_capacity, size_t *line_bytes)
{
    if (!parser || !line || !line_bytes || line_capacity == 0u) return -1;
    if (byte == (uint8_t)'\n') {
        size_t bytes;
        if (parser->discarding) {
            parser->discarding = 0;
            parser->used = 0;
            parser->overlong++;
            return -1;
        }
        bytes = parser->used;
        parser->used = 0;
        if (!bytes) return 0;
        if (bytes >= line_capacity) {
            parser->overlong++;
            return -1;
        }
        for (size_t i = 0; i < bytes; i++) line[i] = (char)parser->line[i];
        line[bytes] = 0;
        *line_bytes = bytes;
        return 1;
    }
    if (parser->discarding) return 0;
    if (parser->used >= SXNU_CONTROL_LINE_BYTES) {
        parser->discarding = 1;
        parser->used = 0;
        return 0;
    }
    parser->line[parser->used++] = byte;
    return 0;
}

/* Strict grammar:
 * SXNU1 <16hex session> <8hex seq> <HELLO|PING|STATUS|REBOOT|SCREENSHOT|SHOTNEXT> <8hex crc>
 * CRC-32/IEEE covers every byte before the space immediately before the CRC. */
static inline int
sxnu_control_parse(const char *line, size_t bytes, struct sxnu_control_frame *frame)
{
    const uint8_t *p = (const uint8_t *)line;
    size_t cursor = 5u, verb_start, verb_bytes, crc_start;
    uint64_t session, seq, crc;
    enum sxnu_control_verb verb;

    if (!line || !frame || bytes < 6u || bytes > SXNU_CONTROL_LINE_BYTES) return 0;
    if (p[0] != 'S' || p[1] != 'X' || p[2] != 'N' || p[3] != 'U' || p[4] != '1' ||
        p[cursor++] != (uint8_t)' ') return 0;
    if (cursor > bytes || bytes - cursor < SXNU_CONTROL_SESSION_HEX) return 0;
    if (!sxnu_control_parse_hex(p + cursor, SXNU_CONTROL_SESSION_HEX, &session)) return 0;
    cursor += SXNU_CONTROL_SESSION_HEX;
    if (cursor >= bytes || p[cursor++] != (uint8_t)' ') return 0;
    if (cursor > bytes || bytes - cursor < SXNU_CONTROL_SEQ_HEX) return 0;
    if (!sxnu_control_parse_hex(p + cursor, SXNU_CONTROL_SEQ_HEX, &seq)) return 0;
    cursor += SXNU_CONTROL_SEQ_HEX;
    if (cursor >= bytes || p[cursor++] != (uint8_t)' ') return 0;

    verb_start = cursor;
    while (cursor < bytes && p[cursor] != (uint8_t)' ') cursor++;
    if (cursor == verb_start || cursor >= bytes) return 0;
    verb_bytes = cursor - verb_start;
    verb = sxnu_control_parse_verb(p + verb_start, verb_bytes);
    if (verb == SXNU_CONTROL_VERB_INVALID) return 0;
    cursor++;
    crc_start = cursor;
    if (crc_start > bytes || bytes - crc_start != SXNU_CONTROL_CRC_HEX) return 0;
    if (!sxnu_control_parse_hex(p + crc_start, SXNU_CONTROL_CRC_HEX, &crc)) return 0;
    if ((uint32_t)crc != sxnu_control_crc32(line, crc_start - 1u)) return 0;

    frame->session = session;
    frame->seq = (uint32_t)seq;
    frame->crc = (uint32_t)crc;
    frame->verb = verb;
    return 1;
}

static inline void
sxnu_control_state_reset(struct sxnu_control_state *state, uint64_t session,
                         uint32_t generation)
{
    if (!state) return;
    state->session = session;
    state->last_seq = 0;
    state->generation = generation;
}

/* HELLO is read-only and valid only with session zero and sequence zero. All
 * other commands require the current nonzero session and exactly the next
 * sequence number. This function has no side effects. */
static inline enum sxnu_control_verb
sxnu_control_accept(const struct sxnu_control_state *state,
                    const struct sxnu_control_frame *frame)
{
    if (!state || !frame) return SXNU_CONTROL_VERB_INVALID;
    if (frame->verb < SXNU_CONTROL_VERB_HELLO ||
        frame->verb > SXNU_CONTROL_VERB_SHOTNEXT) return SXNU_CONTROL_VERB_INVALID;
    if (frame->verb == SXNU_CONTROL_VERB_HELLO)
        return frame->session == 0 && frame->seq == 0 ?
               SXNU_CONTROL_VERB_HELLO : SXNU_CONTROL_VERB_INVALID;
    if (!state->session || frame->seq == 0 || frame->session != state->session ||
        state->last_seq == UINT32_MAX ||
        frame->seq != state->last_seq + 1u) return SXNU_CONTROL_VERB_INVALID;
    return frame->verb;
}

static inline int
sxnu_control_commit(struct sxnu_control_state *state,
                    const struct sxnu_control_frame *frame,
                    enum sxnu_control_verb accepted)
{
    if (!state || !frame || accepted < SXNU_CONTROL_VERB_HELLO ||
        accepted > SXNU_CONTROL_VERB_SHOTNEXT || frame->verb != accepted) return 0;
    if (accepted == SXNU_CONTROL_VERB_HELLO)
        return frame->session == 0 && frame->seq == 0;
    if (!state->session || frame->seq == 0 || frame->session != state->session ||
        state->last_seq == UINT32_MAX ||
        frame->seq != state->last_seq + 1u) return 0;
    state->last_seq = frame->seq;
    return 1;
}

static inline void
sxnu_control_writer_init(struct sxnu_control_writer *writer, char *out, size_t capacity)
{
    writer->out = out;
    writer->capacity = out ? capacity : 0;
    writer->used = 0;
    writer->failed = writer->capacity == 0u;
    if (out && capacity) out[0] = 0;
}

static inline int
sxnu_control_writer_put(struct sxnu_control_writer *writer, const char *text,
                        size_t bytes)
{
    size_t i;
    if (!writer || writer->failed || !text) return 0;
    if (bytes > writer->capacity - writer->used) {
        writer->failed = 1;
        return 0;
    }
    for (i = 0; i < bytes; i++) writer->out[writer->used + i] = text[i];
    writer->used += bytes;
    if (writer->used < writer->capacity) writer->out[writer->used] = 0;
    return 1;
}

static inline int
sxnu_control_writer_text(struct sxnu_control_writer *writer, const char *text)
{
    size_t bytes = 0;
    if (!text) return 0;
    while (text[bytes]) bytes++;
    return sxnu_control_writer_put(writer, text, bytes);
}

static inline int
sxnu_control_writer_char(struct sxnu_control_writer *writer, char value)
{
    return sxnu_control_writer_put(writer, &value, 1u);
}

static inline int
sxnu_control_writer_hex(struct sxnu_control_writer *writer, uint64_t value,
                        unsigned int digits)
{
    static const char alphabet[] = "0123456789abcdef";
    char text[16];
    unsigned int i;
    if (!writer || writer->failed || digits > 16u) return 0;
    for (i = 0; i < digits; i++) {
        text[digits - i - 1u] = alphabet[value & 15u];
        value >>= 4;
    }
    return sxnu_control_writer_put(writer, text, digits);
}

static inline int
sxnu_control_writer_decimal(struct sxnu_control_writer *writer, uint64_t value)
{
    char text[20];
    unsigned int used = 0;
    do {
        text[used++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value);
    for (unsigned int i = 0; i < used / 2u; i++) {
        char swap = text[i];
        text[i] = text[used - i - 1u];
        text[used - i - 1u] = swap;
    }
    return sxnu_control_writer_put(writer, text, used);
}

static inline int
sxnu_control_writer_signed(struct sxnu_control_writer *writer, int64_t value)
{
    uint64_t magnitude;
    if (!writer) return 0;
    if (value < 0) {
        if (!sxnu_control_writer_char(writer, '-')) return 0;
        magnitude = (uint64_t)(-(value + 1)) + 1u;
    } else {
        magnitude = (uint64_t)value;
    }
    return sxnu_control_writer_decimal(writer, magnitude);
}

static inline int
sxnu_control_reply_start(struct sxnu_control_writer *writer, uint64_t session,
                         uint32_t seq, enum sxnu_control_verb verb,
                         const char *status)
{
    if (!writer || verb == SXNU_CONTROL_VERB_INVALID || !status) return 0;
    return sxnu_control_writer_text(writer, "SXNU1R ") &&
           sxnu_control_writer_hex(writer, session, SXNU_CONTROL_SESSION_HEX) &&
           sxnu_control_writer_char(writer, ' ') &&
           sxnu_control_writer_hex(writer, seq, SXNU_CONTROL_SEQ_HEX) &&
           sxnu_control_writer_char(writer, ' ') &&
           sxnu_control_writer_text(writer, sxnu_control_verb_name(verb)) &&
           sxnu_control_writer_char(writer, ' ') &&
           sxnu_control_writer_text(writer, status);
}

/* Append the CRC and newline. Returns the complete reply byte count including
 * newline, or zero if the fixed 512-byte ceiling would be exceeded. */
static inline size_t
sxnu_control_reply_finish(struct sxnu_control_writer *writer)
{
    uint32_t crc;
    if (!writer || writer->failed) return 0;
    crc = sxnu_control_crc32(writer->out, writer->used);
    if (!sxnu_control_writer_char(writer, ' ') ||
        !sxnu_control_writer_hex(writer, crc, SXNU_CONTROL_CRC_HEX) ||
        !sxnu_control_writer_char(writer, '\n')) return 0;
    return writer->used;
}

#endif /* QCOM_USB_CONTROL_H */
