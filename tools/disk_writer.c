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
#include "sniffer.h"
#include "mask.h"
#include "transform.h"
#include "disk.h"

#include "disk_writer.h"


static pthread_t writer_id;



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Disk header and in-ram data.                                              */

/* File handle for writing to disk. */
static int disk_fd;

/* Memory mapped pointers to in memory data stored on disk. */
static struct disk_header *header;      // Disk header with basic parameters
static struct data_index *data_index;   // Index of blocks
static struct decimated_data *dd_data;  // Double decimated data


static bool lock_disk(void)
{
    struct flock flock = {
        .l_type = F_WRLCK, .l_whence = SEEK_SET,
        .l_start = 0, .l_len = 0
    };
    return TEST_IO_(fcntl(disk_fd, F_SETLK, &flock),
        "Unable to lock archive for writing: already running?");
}


/* Opens and locks the archive for direct IO and maps the three in memory
 * regions directly into memory.  Returns the configured input block size. */
bool initialise_disk_writer(const char *file_name, uint32_t *input_block_size)
{
    uint64_t disk_size;
    return
        TEST_IO_(
            disk_fd = open(file_name, O_RDWR | O_DIRECT | O_LARGEFILE),
            "Unable to open archive file \"%s\"", file_name)  &&
        lock_disk()  &&
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
                header->dd_data_start));
}

static void close_disk(void)
{
    ASSERT_IO(munmap(dd_data, header->dd_data_size));
    ASSERT_IO(munmap(data_index, header->index_data_size));
    ASSERT_IO(munmap(header, DISK_HEADER_SIZE));
    ASSERT_IO(close(disk_fd));
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Disk writing thread.                                                      */

static struct reader_state *reader;
static bool writer_running;


static void * writer_thread(void *context)
{
    while (writer_running)
    {
        int backlog;
        const void *block = get_read_block(reader, &backlog, NULL);
        process_block(block);
        if (block)
            release_read_block(reader);
    }
    return NULL;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Disk writing initialisation and startup.                                  */


bool start_disk_writer(void)
{
    reader = open_reader(true);
    writer_running = true;
    return
        initialise_transform(disk_fd, header, dd_data)  &&
        TEST_0(pthread_create(&writer_id, NULL, writer_thread, NULL));
}


void terminate_disk_writer(void)
{
    log_message("Waiting for writer");
    writer_running = false;
    stop_reader(reader);
    ASSERT_0(pthread_join(writer_id, NULL));
    close_reader(reader);
    close_disk();

    log_message("Disk writer done");
}
