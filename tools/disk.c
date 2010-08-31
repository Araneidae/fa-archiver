/* Common routines for disk access. */

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <errno.h>

#include "error.h"
#include "sniffer.h"
#include "mask.h"
#include "transform.h"

#include "disk.h"


static uint32_t round_to_page(uint32_t block_size)
{
    uint32_t page_size = sysconf(_SC_PAGESIZE);
    return page_size * ((block_size + page_size - 1) / page_size);
}

static bool page_aligned(uint64_t offset, const char *description)
{
    uint32_t page_size = sysconf(_SC_PAGESIZE);
    return TEST_OK_(offset % page_size == 0,
        "Bad page alignment for %s", description);
}


bool initialise_header(
    struct disk_header *header,
    filter_mask_t archive_mask,
    uint64_t file_size,
    uint32_t input_block_size,
    uint32_t output_block_size,
    uint32_t first_decimation,
    uint32_t second_decimation)
{
    uint32_t archive_mask_count = count_mask_bits(archive_mask);

    /* Header signature. */
    memset(header, 0, sizeof(*header));
    strncpy(header->signature, DISK_SIGNATURE, sizeof(header->signature));
    header->version = DISK_VERSION;

    /* Capture parameters. */
    copy_mask(header->archive_mask, archive_mask);
    header->archive_mask_count = archive_mask_count;
    header->first_decimation = first_decimation;
    header->second_decimation = second_decimation;
    header->input_block_size = input_block_size;

    /* Compute the fixed size parameters describing the data layout. */
    header->major_sample_count = output_block_size / FA_ENTRY_SIZE;
    header->d_sample_count = header->major_sample_count / first_decimation;
    header->dd_sample_count = header->d_sample_count / second_decimation;
    header->major_block_size =
        archive_mask_count * (
            header->major_sample_count * FA_ENTRY_SIZE +
            header->d_sample_count * sizeof(struct decimated_data));

    /* Computing the total number of samples (we count in major blocks) is a
     * little tricky, as we have to fit everything into file_size including
     * all the auxiliary data structures.  What makes things more tricky is
     * that both the index and DD data areas are rounded up to a multiple of
     * page size, so simple division won't quite do the trick. */
    uint64_t data_size = file_size - DISK_HEADER_SIZE;
    uint32_t index_block_size = sizeof(struct data_index);
    uint32_t dd_block_size =
        header->dd_sample_count * archive_mask_count *
        sizeof(struct decimated_data);
    /* Start with a simple estimate by division. */
    uint32_t major_block_count =
        data_size / (
            index_block_size + dd_block_size + header->major_block_size);
    uint32_t index_data_size =
        round_to_page(major_block_count * index_block_size);
    uint32_t dd_data_size = round_to_page(major_block_count * dd_block_size);
    /* Now incrementally reduce the major block count until we're good.  In
     * fact, this is only going to happen once at most. */
    while (index_data_size + dd_data_size +
           major_block_count * header->major_block_size > data_size)
    {
        major_block_count -= 1;
        index_data_size = round_to_page(major_block_count * index_block_size);
        dd_data_size = round_to_page(major_block_count * dd_block_size);
    }

    /* Finally we can compute the data layout. */
    header->index_data_start = DISK_HEADER_SIZE;
    header->index_data_size = index_data_size;
    header->dd_data_start = header->index_data_start + index_data_size;
    header->dd_data_size = dd_data_size;
    header->dd_total_count = header->dd_sample_count * major_block_count;
    header->major_data_start = header->dd_data_start + dd_data_size;
    header->major_block_count = major_block_count;
    header->total_data_size =
        header->major_data_start +
        (uint64_t) major_block_count * header->major_block_size;

    header->current_major_block = 0;

    errno = 0;      // Suppresses invalid errno report from TEST_OK_ failures
    return
        TEST_OK_(output_block_size % sysconf(_SC_PAGESIZE) == 0,
            "Output block size must be a multiple of page size")  &&
        TEST_OK_(
            output_block_size % FA_ENTRY_SIZE == 0,
            "Output block size must be a multiple of FA entry size")  &&
        validate_header(header, file_size);
}


bool validate_header(struct disk_header *header, uint64_t file_size)
{
    COMPILE_ASSERT(sizeof(struct disk_header) <= DISK_HEADER_SIZE);

    uint32_t input_sample_count = header->input_block_size / FA_FRAME_SIZE;
    errno = 0;      // Suppresses invalid error report from TEST_OK_ failures
    return
        /* Basic header validation. */
        TEST_OK_(
            strncmp(header->signature, DISK_SIGNATURE,
                sizeof(header->signature)) == 0,
            "Invalid header signature")  &&
        TEST_OK_(header->version == DISK_VERSION,
            "Invalid header version %u", header->version)  &&

        /* Data capture parameter validation. */
        TEST_OK_(
            count_mask_bits(header->archive_mask) ==
                header->archive_mask_count,
            "Inconsistent archive mask")  &&
        TEST_OK_(header->archive_mask_count > 0, "Empty capture mask")  &&
        TEST_OK_(header->total_data_size <= file_size,
            "Data size in header larger than file size")  &&

        /* Data parameter validation. */
        TEST_OK_(
            header->d_sample_count * header->first_decimation ==
            header->major_sample_count,
            "Invalid first decimation")  &&
        TEST_OK_(
            header->dd_sample_count * header->second_decimation ==
            header->d_sample_count,
            "Invalid second decimation")  &&
        TEST_OK_(
            header->archive_mask_count * (
                header->major_sample_count * FA_ENTRY_SIZE +
                header->d_sample_count * sizeof(struct decimated_data)) ==
            header->major_block_size,
            "Invalid major block size")  &&
        TEST_OK_(
            header->major_block_count * sizeof(struct data_index) <=
            header->index_data_size,
            "Invalid index block size")  &&
        TEST_OK_(
            header->dd_sample_count * header->major_block_count ==
                header->dd_total_count,
            "Invalid total DD count")  &&
        TEST_OK_(
            header->dd_total_count * header->archive_mask_count *
                sizeof(struct decimated_data) <= header->dd_data_size,
            "DD area too small")  &&

        /* Check page alignment. */
        page_aligned(header->index_data_size, "index size")  &&
        page_aligned(header->dd_data_size, "DD size")  &&
        page_aligned(header->major_block_size, "major block")  &&
        page_aligned(header->index_data_start, "index area")  &&
        page_aligned(header->dd_data_start, "DD data area")  &&
        page_aligned(header->major_data_start, "major data area")  &&

        /* Check data areas. */
        TEST_OK_(header->index_data_start >= DISK_HEADER_SIZE,
            "Unexpected index data start")  &&
        TEST_OK_(
            header->dd_data_start >=
            header->index_data_start + header->index_data_size,
            "Unexpected DD data start")  &&
        TEST_OK_(
            header->major_data_start >=
            header->dd_data_start + header->dd_data_size,
            "Unexpected major data start")  &&
        TEST_OK_(
            header->total_data_size >=
            header->major_data_start +
            header->major_block_count * header->major_block_size,
            "Data area too small for data")  &&
        TEST_OK_(
            header->index_data_size >=
            header->major_block_count * sizeof(struct data_index),
            "Index area too small")  &&

        /* Major data layout validation. */
        TEST_OK_(
            header->first_decimation > 1  &&  header->second_decimation > 1,
            "Decimation too small")  &&
        TEST_OK_(
            header->major_sample_count > 1, "Output block size too small")  &&
        TEST_OK_(header->major_block_count > 1, "Data file too small")  &&
        TEST_OK_(
            header->input_block_size % FA_FRAME_SIZE == 0,
            "Input block size must be a multiple of FA frame size")  &&
        TEST_OK_(
            header->major_sample_count % input_sample_count == 0,
            "Input and output block sizes don't match properly")  &&
        TEST_OK_(
            header->major_sample_count % header->first_decimation == 0,
            "Invalid first decimation")  &&
        TEST_OK_(
            header->major_sample_count % (
                header->first_decimation * header->second_decimation) == 0,
            "Decimation must fit into a complete major block")  &&

        TEST_OK_(header->current_major_block < header->major_block_count,
            "Invalid current index");
}


void print_header(FILE *out, struct disk_header *header)
{
    char mask_string[RAW_MASK_BYTES+1];
    format_raw_mask(header->archive_mask, mask_string);
    fprintf(out,
        "FA sniffer archive: %.7s, v%d.\n"
        "Archiving: %s\n"
        "Decimation %"PRIu32", %"PRIu32" => %"PRIu32", "
            "recording %u BPMs\n"
        "Input block size = %"PRIu32" bytes, %"PRIu32" frames\n"
        "Output block size = %"PRIu32" bytes, %"PRIu32" samples\n"
        "Total size = %"PRIu32" major blocks = %"PRIu32" samples"
            " = %"PRIu64" bytes\n"
        "Index data from %"PRIu64" for %"PRIu32" bytes\n"
        "DD data starts %"PRIu64" for %"PRIu32" bytes, %"PRIu32" samples\n"
        "FA+D data from %"PRIu64", %"PRIu32" decimated samples per block\n"
        "Current index: %"PRIu32"\n",
        header->signature, header->version,
        mask_string,
        header->first_decimation, header->second_decimation,
            header->first_decimation * header->second_decimation,
            header->archive_mask_count,
        header->input_block_size, header->input_block_size / FA_FRAME_SIZE,
        header->major_block_size, header->major_sample_count,
        header->major_block_count,
            header->major_block_count * header->major_sample_count,
            header->total_data_size,
        header->index_data_start, header->index_data_size,
        header->dd_data_start, header->dd_data_size, header->dd_total_count,
        header->major_data_start, header->d_sample_count,
        header->current_major_block);
}


bool get_filesize(int disk_fd, uint64_t *file_size)
{
    /* First try blocksize, if that fails try stat: the first works on a
     * block device, the second on a regular file. */
    if (ioctl(disk_fd, BLKGETSIZE64, file_size) == 0)
        return true;
    else
    {
        struct stat st;
        return
            TEST_IO(fstat(disk_fd, &st))  &&
            DO_(*file_size = st.st_size)  &&
            TEST_OK_(*file_size > 0,
                "Zero file size.  Maybe stat failed?");
    }
}


void dump_binary(FILE *out, void *buffer, size_t length)
{
    uint8_t *dump = buffer;

    for (size_t a = 0; a < length; a += 16)
    {
        fprintf(out, "%08zx: ", a);
        for (int i = 0; i < 16; i ++)
        {
            if (a + i < length)
                fprintf(out, " %02x", dump[a+i]);
            else
                fprintf(out, "   ");
            if (i % 16 == 7)
                fprintf(out, " ");
        }

        fprintf(out, "  ");
        for (int i = 0; i < 16; i ++)
        {
            uint8_t c = dump[a+i];
            if (a + i < length)
                fprintf(out, "%c", 32 <= c  &&  c < 127 ? c : '.');
            else
                fprintf(out, " ");
            if (i % 16 == 7)
                fprintf(out, " ");
        }
        fprintf(out, "\n");
    }
    if (length % 16 != 0)
        fprintf(out, "\n");
}
