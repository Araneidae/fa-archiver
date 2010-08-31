/* Filter mask routines. */

/* The filter mask is used to specify a list of PVs.  The syntax of a filter
 * mask can be written as:
 *
 *      mask = id [ "-" id ] [ "," mask]
 *
 * Here each id identifies a particular BPM and must be a number in the range
 * 0 to 255 and id1-id2 identifies an inclusive range of BPMs.
 */


#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "error.h"
#include "sniffer.h"
#include "parse.h"

#include "mask.h"


#define WRITE_BUFFER_SIZE       (1 << 16)


unsigned int count_mask_bits(filter_mask_t mask)
{
    unsigned int count = 0;
    for (int bit = 0; bit < FA_ENTRY_COUNT; bit ++)
        if (test_mask_bit(mask, bit))
            count ++;
    return count;
}


int format_raw_mask(filter_mask_t mask, char *buffer)
{
    for (int i = sizeof(filter_mask_t); i > 0; i --)
        buffer += sprintf(buffer, "%02X", ((unsigned char *) mask)[i - 1]);
    return 2 * sizeof(filter_mask_t);
}

void print_raw_mask(FILE *out, filter_mask_t mask)
{
    char buffer[2 * sizeof(filter_mask_t) + 1];
    fwrite(buffer, format_raw_mask(mask, buffer), 1, out);
}



static bool parse_id(const char **string, int *id)
{
    return
        parse_int(string, id)  &&
        TEST_OK_(0 <= *id  &&  *id < FA_ENTRY_COUNT, "id %d out of range", *id);
}


bool parse_mask(const char **string, filter_mask_t mask)
{
    memset(mask, 0, sizeof(filter_mask_t));

    bool ok = true;
    do {
        int id;
        ok = parse_id(string, &id);
        if (ok)
        {
            int end_id = id;
            if (parse_char(string, '-'))
                ok = parse_id(string, &end_id)  &&
                    TEST_OK_(id <= end_id, "Range %d-%d is empty", id, end_id);
            for (int i = id; ok  &&  i <= end_id; i ++)
                set_mask_bit(mask, i);
        }
    } while (parse_char(string, ','));

    return ok;
}


bool parse_raw_mask(const char **string, filter_mask_t mask)
{
    memset(mask, 0, sizeof(filter_mask_t));
    int count = FA_ENTRY_COUNT / 4;                 // 4 bits per nibble
    for (int i = count - 1; i >= 0; i --)
    {
        char ch = *(*string)++;
        int nibble;
        if ('0' <= ch  &&  ch <= '9')
            nibble = ch - '0';
        else if ('A' <= ch  &&  ch <= 'F')
            nibble = ch - 'A' + 10;
        else
            return TEST_OK_(false, "Unexpected character in mask");
        mask[i / 8] |= nibble << (4 * (i % 8));     // 8 nibbles per word
    }
    return true;
}


int copy_frame(void *to, const void *from, filter_mask_t mask)
{
    const int32_t *from_p = from;
    int32_t *to_p = to;
    int copied = 0;
    for (size_t i = 0; i < sizeof(filter_mask_t) / 4; i ++)
    {
        uint32_t m = mask[i];
        for (int j = 0; j < 32; j ++)
        {
            if ((m >> j) & 1)
            {
                *to_p++ = from_p[0];
                *to_p++ = from_p[1];
                copied += 8;
            }
            from_p += 2;
        }
    }
    return copied;
}


bool write_frames(int file, filter_mask_t mask, const void *frame, int count)
{
    int out_frame_size = count_mask_bits(mask) * FA_ENTRY_SIZE;
    while (count > 0)
    {
        char buffer[WRITE_BUFFER_SIZE];
        size_t buffered = 0;
        while (count > 0  &&  buffered + out_frame_size <= WRITE_BUFFER_SIZE)
        {
            copy_frame(buffer + buffered, frame, mask);
            frame = frame + FA_FRAME_SIZE;
            buffered += out_frame_size;
            count -= 1;
        }

        size_t written = 0;
        while (buffered > 0)
        {
            size_t wr;
            if (!TEST_IO(wr = write(file, buffer + written, buffered)))
                return false;
            written += wr;
            buffered -= wr;
        }
    }
    return true;
}
