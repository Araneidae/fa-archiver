/* Code to interface to fa_sniffer device. */

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <math.h>
#include <limits.h>

#include "error.h"
#include "buffer.h"

#include "fa_sniffer.h"
#include "sniffer.h"
#include "replay.h"


/* This is where the sniffer data will be written. */
static struct buffer *fa_block_buffer;


/* Abstraction of sniffer device interface so we can implement debug versions of
 * the sniffer. */
struct sniffer_context
{
    bool (*initialise)(const char *source_name);
    void (*reset)(void);
    bool (*read)(struct fa_row *block, size_t block_size);
    bool (*status)(struct fa_status *status);
};

/* This will be initialised with the appropriate context to use. */
static struct sniffer_context *sniffer_context;


static void * sniffer_thread(void *context)
{
    const size_t fa_block_size = buffer_block_size(fa_block_buffer);
    bool in_gap = false;    // Only report gap once
    while (true)
    {
        while (true)
        {
            void *buffer = get_write_block(fa_block_buffer);
            if (buffer == NULL)
            {
                /* Whoops: the archiver thread has fallen behind. */
                log_message("Sniffer unable to write block");
                break;
            }
            bool gap = !sniffer_context->read(buffer, fa_block_size);

            /* Get the time this block was written.  This is close enough to the
             * completion of the FA sniffer read to be a good timestamp for the
             * last frame. */
            struct timespec ts;
            ASSERT_IO(clock_gettime(CLOCK_REALTIME, &ts));
            release_write_block(fa_block_buffer, gap, ts_to_microseconds(&ts));
            if (gap)
            {
                if (!in_gap)
                    log_message("Unable to read block");
                in_gap = true;
                break;
            }
            else if (in_gap)
            {
                log_message("Block read successfully");
                in_gap = false;
            }
        }

        /* Pause before retrying.  Ideally should poll sniffer card for
         * active network here. */
        sleep(1);
        sniffer_context->reset();
    }
}


bool get_sniffer_status(struct fa_status *status)
{
    return sniffer_context->status(status);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Standard sniffer using true sniffer device. */

static const char *fa_sniffer_device;
static int fa_sniffer;
static bool ioctl_ok;

static bool initialise_sniffer_device(const char *device_name)
{
    fa_sniffer_device = device_name;
    bool ok = TEST_IO_(
        fa_sniffer = open(fa_sniffer_device, O_RDONLY),
        "Can't open sniffer device %s", fa_sniffer_device);
    int version;
    ioctl_ok =
        TEST_IO_(version = ioctl(fa_sniffer, FASNIF_IOCTL_GET_VERSION),
            "Sniffer device doesn't support ioctl interface")  &&
        TEST_OK_(version == FASNIF_IOCTL_VERSION,
            "Sniffer device ioctl version mismatch");
    return ok;
}

static void reset_sniffer_device(void)
{
    if (ioctl_ok)
        /* If possible use the restart command to restart the sniffer. */
        TEST_IO(ioctl(fa_sniffer, FASNIF_IOCTL_RESTART));
    else
    {
        /* Backwards compatible code: close and reopen the device. */
        TEST_IO(close(fa_sniffer));
        TEST_IO(fa_sniffer = open(fa_sniffer_device, O_RDONLY));
    }
}

static bool read_sniffer_device(struct fa_row *rows, size_t length)
{
    void *buffer = rows;
    while (length > 0)
    {
        ssize_t rx = read(fa_sniffer, buffer, length);
        if (rx <= 0)
            return false;
        length -= rx;
        buffer += rx;
    }
    return true;
}

static bool read_sniffer_status(struct fa_status *status)
{
    return TEST_IO_(ioctl(fa_sniffer, FASNIF_IOCTL_GET_STATUS, status),
        "Unable to read sniffer status");
}

struct sniffer_context sniffer_device = {
    .initialise = initialise_sniffer_device,
    .reset = reset_sniffer_device,
    .read = read_sniffer_device,
    .status = read_sniffer_status,
};



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Dummy sniffer using replay data. */

static void reset_replay(void) { ASSERT_FAIL(); }

static bool read_replay_status(struct fa_status *status)
{
    return FAIL_("Sniffer status unavailable in replay mode");
}

struct sniffer_context sniffer_replay = {
    .initialise = initialise_replay,
    .reset = reset_replay,
    .read = read_replay_block,
    .status = read_replay_status,
};



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */


static pthread_t sniffer_id;

bool initialise_sniffer(
    struct buffer *buffer, const char *device_name, bool replay)
{
    fa_block_buffer = buffer;
    sniffer_context = replay ? &sniffer_replay : &sniffer_device;
    return sniffer_context->initialise(device_name);
}

bool start_sniffer(bool boost_priority)
{
    pthread_attr_t attr;
    return
        TEST_0(pthread_attr_init(&attr))  &&
        IF_(boost_priority,
            /* If requested boost the thread priority and configure FIFO
             * scheduling to ensure that this thread gets absolute maximum
             * priority. */
            TEST_0(pthread_attr_setinheritsched(
                &attr, PTHREAD_EXPLICIT_SCHED))  &&
            TEST_0(pthread_attr_setschedpolicy(&attr, SCHED_FIFO))  &&
            TEST_0(pthread_attr_setschedparam(
                &attr, &(struct sched_param) { .sched_priority = 1 })))  &&
        TEST_0_(pthread_create(&sniffer_id, &attr, sniffer_thread, NULL),
            "Priority boosting requires real time thread support")  &&
        TEST_0(pthread_attr_destroy(&attr));
}

void terminate_sniffer(void)
{
    log_message("Waiting for sniffer...");
    pthread_cancel(sniffer_id);     // Ignore complaint if already halted
    ASSERT_0(pthread_join(sniffer_id, NULL));
    log_message("done");
}
