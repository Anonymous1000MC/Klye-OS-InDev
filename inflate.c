/* inflate.c - DEFLATE decompression, as PNG needs it.
 *
 * PNG's image data is a zlib stream: a two byte header, then DEFLATE, then an
 * Adler-32 checksum.  DEFLATE is a compressed bit stream with three block
 * types and Huffman coded literals and lengths, and there is no implementation
 * of it anywhere in this tree, so this is one.
 *
 * Scope is DEFLATE as PNG uses it: stored, fixed Huffman and dynamic Huffman
 * blocks.  That is all of it that a conforming encoder may emit, so this is a
 * complete decoder, not a subset.  What is deliberately not here is anything
 * outside DEFLATE -- no gzip wrapper, no ZIP.
 *
 * The output is a callback rather than a buffer.  A PNG's rows are defiltered
 * as they arrive, since a row cannot be filtered until the row above it is
 * known, and buffering the whole decompressed image to filter afterwards would
 * be several megabytes for a wallpaper sized like the screen.
 */

#include "inflate.h"

static const char *inflate_error_text = "";

/* One Huffman table: counts of codes per bit length, and the symbols ordered
 * by code length then by symbol.  Decoding is a walk rather than a lookup
 * table, which is slower per bit but needs 2 KiB instead of 100 KiB for the
 * fast variant, and a wallpaper decompresses a few hundred kilobytes once. */
struct huffman {
    uint16_t counts[16];
    uint16_t symbols[INFLATE_MAX_SYMBOLS];
};

struct inflate_state {
    const uint8_t *input;
    size_t input_length;
    size_t input_position;
    uint32_t bit_buffer;
    int bit_count;
    bool overrun;
};

/* Read `count` bits, least significant bit first, which is the order DEFLATE
 * packs them in.  Bits past the end of the input read as zero and set the
 * overrun flag rather than returning an error immediately: a truncated stream
 * has to be detected, but only once the caller has consumed what there was,
 * because a caller that stops early on a short read would report a corrupt
 * file for a stream it simply did not need the end of. */
static uint32_t inflate_bits(struct inflate_state *state, int count)
{
    uint32_t value;

    while (state->bit_count < count) {
        uint8_t byte = 0U;

        if (state->input_position < state->input_length) {
            byte = state->input[state->input_position++];
        } else {
            state->overrun = true;
        }
        state->bit_buffer |= (uint32_t)byte << state->bit_count;
        state->bit_count += 8;
    }
    value = state->bit_buffer & ((1U << count) - 1U);
    state->bit_buffer >>= count;
    state->bit_count -= count;
    return value;
}

static void huffman_build(struct huffman *table, const uint8_t *lengths,
                          int count)
{
    uint16_t offsets[16];
    int index;

    for (index = 0; index < 16; ++index) {
        table->counts[index] = 0U;
    }
    for (index = 0; index < count; ++index) {
        table->counts[lengths[index]]++;
    }
    /* the one-code case: a table with a single symbol of length 1 is how an
     * "all zeroes" or "all ones" distance table is written, and it is not a
     * complete prefix code, so it is given a second code of the other value
     * to make it one */
    if (table->counts[0] == (uint16_t)count) {
        table->counts[0] = 0U;
        if (count > 0) {
            table->counts[1] = (uint16_t)count;
        }
    }
    table->counts[0] = 0U;
    offsets[0] = 0U;
    offsets[1] = 0U;
    for (index = 1; index < 15; ++index) {
        offsets[index + 1] = (uint16_t)(offsets[index] + table->counts[index]);
    }
    for (index = 0; index < count; ++index) {
        if (lengths[index] != 0U) {
            table->symbols[offsets[lengths[index]]++] = (uint16_t)index;
        }
    }
}

static int huffman_decode(struct inflate_state *state, const struct huffman *table)
{
    int code = 0;
    int first = 0;
    int index = 0;

    for (int length = 1; length < 16; ++length) {
        code |= (int)inflate_bits(state, 1);
        {
            int count = (int)table->counts[length];

            if (code - first < count) {
                return (int)table->symbols[index + (code - first)];
            }
            index += count;
            first = (first + count) << 1;
            code <<= 1;
        }
        if (state->overrun) {
            return -1;
        }
    }
    return -1;
}

static const uint16_t length_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
    67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const uint8_t length_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
    4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const uint16_t distance_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
    769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const uint8_t distance_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
    9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

/* One length/distance pair: a back reference into what has been output. */
struct backref {
    uint32_t length;
    uint32_t distance;
};

/* Run one length/distance pair, calling emit once per byte.
 *
 * One byte at a time rather than in runs because the callback may need each
 * byte in order -- a PNG defilter needs the previous byte of the row, and
 * emitting a whole match at once would hand it bytes it has not filtered yet.
 * The callback is inlined by the compiler at -O2, so the per byte cost is a
 * call and a compare. */
static bool inflate_backref(struct inflate_state *state,
                            struct backref ref,
                            uint8_t *history, size_t history_length,
                            size_t *history_used, uint32_t before,
                            inflate_emit_fn emit, void *context)
{
    ptrdiff_t from = (ptrdiff_t)*history_used - (ptrdiff_t)ref.distance;

    if (ref.distance == 0U || ref.distance > *history_used) {
        return false;
    }
    /* A match is copied a byte at a time and each byte is appended to the
     * window as it goes, so a match that starts near the end of the window and
     * runs past it is fine: the bytes it is copying are the ones just written.
     * Refusing because the window would overflow is what made every image
     * larger than 32 KiB fail, which is every wallpaper. */
    for (uint32_t index = 0; index < ref.length; ++index) {
        uint8_t byte;

        if (from + (ptrdiff_t)index < 0 ||
            (size_t)(from + (ptrdiff_t)index) >= history_length) {
            return false;
        }
        byte = history[from + (ptrdiff_t)index];
        if (!emit(context, byte)) {
            return false;
        }
        if (*history_used < history_length) {
            history[(*history_used)++] = byte;
        } else {
            /* slide: drop the oldest byte so the window stays the most recent
             * history a later match can refer back to */
            for (size_t move = 0; move + 1U < history_length; ++move) {
                history[move] = history[move + 1U];
            }
            history[history_length - 1U] = byte;
            if (from == 0) {
                return false; /* nothing older left to refer to */
            }
            from--;
        }
    }
    (void)state;
    (void)before;
    return true;
}

static bool inflate_block(struct inflate_state *state,
                          const struct huffman *literals,
                          const struct huffman *distances,
                          uint8_t *history, size_t history_length,
                          size_t *history_used, uint32_t *before,
                          inflate_emit_fn emit, void *context)
{
    for (;;) {
        int symbol = huffman_decode(state, literals);

        if (symbol < 0) {
            return false;
        }
        if (symbol < 256) {
            uint8_t byte = (uint8_t)symbol;

            if (!emit(context, byte)) {
                return false;
            }
            if (*history_used < history_length) {
                history[(*history_used)++] = byte;
            }
            continue;
        }
        if (symbol == 256) {
            return true; /* end of block */
        }
        {
            int index = symbol - 257;
            struct backref ref;

            if (index < 0 || index >= 29) {
                return false;
            }
            ref.length = (uint32_t)length_base[index] +
                         inflate_bits(state, length_extra[index]);
            symbol = huffman_decode(state, distances);
            if (symbol < 0 || symbol >= 30) {
                return false;
            }
            ref.distance = (uint32_t)distance_base[symbol] +
                           inflate_bits(state, distance_extra[symbol]);
            if (!inflate_backref(state, ref, history, history_length,
                                 history_used, *before, emit, context)) {
                return false;
            }
        }
    }
}

static void build_fixed(struct huffman *literals, struct huffman *distances)
{
    uint8_t lengths[288];
    int index;

    for (index = 0; index < 144; ++index) {
        lengths[index] = 8;
    }
    for (; index < 256; ++index) {
        lengths[index] = 9;
    }
    for (; index < 280; ++index) {
        lengths[index] = 7;
    }
    for (; index < 288; ++index) {
        lengths[index] = 8;
    }
    huffman_build(literals, lengths, 288);
    for (index = 0; index < 30; ++index) {
        lengths[index] = 5;
    }
    huffman_build(distances, lengths, 30);
}

static bool read_dynamic(struct inflate_state *state, struct huffman *literals,
                         struct huffman *distances)
{
    static const uint8_t order[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };
    uint8_t code_lengths[19];
    uint8_t lengths[288 + 32];
    struct huffman code_table;
    int literal_count;
    int distance_count;
    int code_count;
    int index = 0;

    literal_count = (int)inflate_bits(state, 5) + 257;
    distance_count = (int)inflate_bits(state, 5) + 1;
    code_count = (int)inflate_bits(state, 4) + 4;
    if (literal_count > 286 || distance_count > 30) {
        return false;
    }
    for (index = 0; index < 19; ++index) {
        code_lengths[index] = 0U;
    }
    for (index = 0; index < code_count; ++index) {
        code_lengths[order[index]] = (uint8_t)inflate_bits(state, 3);
    }
    huffman_build(&code_table, code_lengths, 19);

    for (index = 0; index < literal_count + distance_count; ) {
        int symbol = huffman_decode(state, &code_table);

        if (symbol < 0) {
            return false;
        }
        if (symbol < 16) {
            lengths[index++] = (uint8_t)symbol;
            continue;
        }
        {
            uint8_t value = 0U;
            int repeat = 0;

            if (symbol == 16) {
                if (index == 0) {
                    return false; /* nothing to repeat */
                }
                value = lengths[index - 1];
                repeat = 3 + (int)inflate_bits(state, 2);
            } else if (symbol == 17) {
                repeat = 3 + (int)inflate_bits(state, 3);
            } else {
                repeat = 11 + (int)inflate_bits(state, 7);
            }
            if (index + repeat > literal_count + distance_count) {
                return false;
            }
            while (repeat-- > 0) {
                lengths[index++] = value;
            }
        }
    }
    if (lengths[256] == 0U) {
        return false; /* no end of block code */
    }
    huffman_build(literals, lengths, literal_count);
    huffman_build(distances, lengths + literal_count, distance_count);
    return true;
}

static bool inflate_stored(struct inflate_state *state, uint8_t *history,
                           size_t history_length, size_t *history_used,
                           inflate_emit_fn emit, void *context)
{
    uint32_t length;
    uint32_t check;

    /* a stored block starts on a byte boundary */
    state->bit_buffer = 0U;
    state->bit_count = 0U;
    if (state->input_position + 4U > state->input_length) {
        return false;
    }
    length = (uint32_t)state->input[state->input_position] |
             ((uint32_t)state->input[state->input_position + 1U] << 8);
    check = (uint32_t)state->input[state->input_position + 2U] |
            ((uint32_t)state->input[state->input_position + 3U] << 8);
    if ((length ^ 0xFFFFU) != check) {
        return false;
    }
    state->input_position += 4U;
    for (uint32_t index = 0; index < length; ++index) {
        uint8_t byte;

        if (state->input_position >= state->input_length) {
            return false;
        }
        byte = state->input[state->input_position++];
        if (!emit(context, byte)) {
            return false;
        }
        if (*history_used < history_length) {
            history[(*history_used)++] = byte;
        }
    }
    return true;
}

const char *inflate_error(void)
{
    return inflate_error_text;
}

/* The caller's emit, with a byte counter on top.
 *
 * How much has come out is a question about the bytes the caller was actually
 * given, not about how full the back reference window is: the window is 32 KiB
 * and saturates, so using its fill as a progress measure stops the stream after
 * 32 KiB however much more there was.  That is not a corner case -- it is every
 * image larger than 32 KiB, which is every wallpaper. */
struct emit_counter {
    inflate_emit_fn emit;
    void *context;
    size_t count;
};

static bool counted_emit(void *context, uint8_t byte)
{
    struct emit_counter *counter = (struct emit_counter *)(void *)context;

    counter->count++;
    return counter->emit(counter->context, byte);
}

bool inflate_stream(const uint8_t *input, size_t input_length,
                    size_t expected_output, uint8_t *history,
                    size_t history_length, inflate_emit_fn emit,
                    void *context)
{
    struct inflate_state state;
    struct huffman literals;
    struct huffman distances;
    struct emit_counter counter;
    size_t history_used = 0U;
    uint32_t before = 0U;
    int final = 0;

    inflate_error_text = "";
    counter.emit = emit;
    counter.context = context;
    counter.count = 0U;
    emit = counted_emit;
    context = &counter;
    state.input = input;
    state.input_length = input_length;
    state.input_position = 0U;
    state.bit_buffer = 0U;
    state.bit_count = 0;
    state.overrun = false;

    while (!final) {
        uint32_t type;

        final = (int)inflate_bits(&state, 1);
        type = inflate_bits(&state, 2);
        if (state.overrun) {
            inflate_error_text = "the compressed data ended early";
            return false;
        }
        if (type == 0U) {
            if (!inflate_stored(&state, history, history_length, &history_used,
                                emit, context)) {
                inflate_error_text = "a stored block is truncated or corrupt";
                return false;
            }
            continue;
        }
        if (type == 1U) {
            build_fixed(&literals, &distances);
        } else if (type == 2U) {
            if (!read_dynamic(&state, &literals, &distances)) {
                inflate_error_text = "the dynamic Huffman tables are corrupt";
                return false;
            }
        } else {
            inflate_error_text = "the stream names a block type that does not exist";
            return false;
        }
        if (!inflate_block(&state, &literals, &distances, history,
                           history_length, &history_used, &before, emit,
                           context)) {
            inflate_error_text = state.overrun
                                    ? "the compressed data ended early"
                                    : "a length or distance refers outside the output";
            return false;
        }
        if (counter.count >= expected_output && expected_output != 0U) {
            break;
        }
    }
    return true;
}
