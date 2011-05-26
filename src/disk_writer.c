/* Writes buffer to disk. */

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

#include "error.h"
#include "buffer.h"
#include "fa_sniffer.h"
#include "mask.h"
#include "disk.h"
#include "transform.h"
#include "locking.h"

#include "disk_writer.h"



/* Used to terminate threads. */
static bool writer_running;

/* File handle for writing to disk. */
static int disk_fd;


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Disk header and in-ram data.                                              */

/* Memory mapped pointers to in memory data stored on disk. */
static struct disk_header *header;      // Disk header with basic parameters
static struct data_index *data_index;   // Index of blocks
static struct decimated_data *dd_data;  // Double decimated data


/* Opens and locks the archive for direct IO and maps the three in memory
 * regions directly into memory.  Returns the configured input block size. */
bool initialise_disk_writer(const char *file_name, uint32_t *input_block_size)
{
    uint64_t disk_size;
    return
        TEST_IO_(
            /* I am told, eg http://lkml.org/lkml/2007/1/10/233, see also
             * http://kerneltrap.org/node/7563, to use madvise() and
             * posix_fadvise() instead of O_DIRECT.  However I'm not persuaded,
             * the pattern of access in this application is specialised enough
             * that I think O_DIRECT is appropriate. */
            disk_fd = open(file_name, O_RDWR | O_DIRECT | O_LARGEFILE),
            "Unable to open archive file \"%s\"", file_name)  &&
        lock_archive(disk_fd)  &&
        TEST_IO(
            header = mmap(NULL, DISK_HEADER_SIZE,
                PROT_READ | PROT_WRITE, MAP_SHARED, disk_fd, 0))  &&
        get_filesize(disk_fd, &disk_size)  &&
        validate_header(header, disk_size)  &&
        DO_(*input_block_size = header->input_block_size)  &&
        TEST_IO(
            data_index = mmap(NULL, header->index_data_size,
                PROT_READ | PROT_WRITE, MAP_SHARED, disk_fd,
                header->index_data_start))  &&
        TEST_IO(
            dd_data = mmap(NULL, header->dd_data_size,
                PROT_READ | PROT_WRITE, MAP_SHARED, disk_fd,
                header->dd_data_start))  &&
        DO_(initialise_transform(header, data_index, dd_data));
}

static void close_disk(void)
{
    ASSERT_IO(munmap(dd_data, header->dd_data_size));
    ASSERT_IO(munmap(data_index, header->index_data_size));
    ASSERT_IO(munmap(header, DISK_HEADER_SIZE));
    ASSERT_IO(close(disk_fd));
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Disk writing and read permission thread. */

/* This thread manages writing of blocks to the disk.  Requests for reads are
 * interlocked with this thread so that reading is blocked while a write is
 * currently in progress. */

DECLARE_LOCKING(writer_lock);

/* Current write request. */
static bool writing_active = false;
static off64_t writing_offset;
static void *writing_block;
static size_t writing_length;


/* Ensures entire block is written even if interrupted. */
static bool do_write(int file, void *buffer, size_t length)
{
    while (length > 0)
    {
        ssize_t tx;
        if (!TEST_OK(tx = write(file, buffer, length)))
            return false;
        length -= tx;
        buffer += tx;
    }
    return true;
}

static void * writer_thread(void *context)
{
    bool ok = true;
    while (ok  &&  writer_running)
    {
        LOCK(writer_lock);
        while (writer_running  &&  !writing_active)
            pwait(&writer_lock);
        UNLOCK(writer_lock);

        ok = writing_active  &&
            TEST_IO(lseek(disk_fd, writing_offset, SEEK_SET))  &&
            do_write(disk_fd, writing_block, writing_length);

        LOCK(writer_lock);
        writing_active = false;
        psignal(&writer_lock);
        UNLOCK(writer_lock);
    }
    return NULL;
}

static void stop_writer_thread(void)
{
    LOCK(writer_lock);
    writer_running = false;
    psignal(&writer_lock);
    UNLOCK(writer_lock);
}

void schedule_write(off64_t offset, void *block, size_t length)
{
    LOCK(writer_lock);
    while (writing_active)
        pwait(&writer_lock);
    writing_offset = offset;
    writing_block = block;
    writing_length = length;
    writing_active = true;
    psignal(&writer_lock);
    UNLOCK(writer_lock);
}

void request_read(void)
{
    LOCK(writer_lock);
    while (writing_active)
        pwait(&writer_lock);
    UNLOCK(writer_lock);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Data processing thread. */

static struct reader_state *reader;


static void * transform_thread(void *context)
{
    while (writer_running)
    {
        uint64_t timestamp;
        const void *block = get_read_block(reader, NULL, &timestamp);
        process_block(block, timestamp);
        if (block)
            release_read_block(reader);
    }
    return NULL;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Disk writing initialisation and startup.                                  */

static pthread_t transform_id;
static pthread_t writer_id;


bool start_disk_writer(struct buffer *buffer)
{
    reader = open_reader(buffer, true);
    writer_running = true;
    return
        TEST_0(pthread_create(&writer_id, NULL, writer_thread, NULL))  &&
        TEST_0(pthread_create(&transform_id, NULL, transform_thread, NULL));
}


void terminate_disk_writer(void)
{
    log_message("Waiting for writer");
    stop_writer_thread();
    stop_reader(reader);
    ASSERT_0(pthread_join(transform_id, NULL));
    ASSERT_0(pthread_join(writer_id, NULL));
    close_reader(reader);
    close_disk();

    log_message("Disk writer done");
}
