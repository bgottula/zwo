#include "disk.h"
#include <deque>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <unistd.h>
#include <err.h>
#include <sys/syscall.h>
#include <sys/statvfs.h>
#include "Frame.h"
#include "SERFile.h"


constexpr int64_t MIN_FREE_DISK_SPACE_BYTES = 100 << 20; // 100 MiB

extern std::atomic_bool end_program;

extern std::mutex to_disk_deque_mutex;
extern std::condition_variable to_disk_deque_cv;
extern std::deque<Frame *> to_disk_deque;


// Writes frames of data to disk as quickly as possible. Run as a thread.
void write_to_disk(
    const char *filename,
    const char *camera_name,
    bool color,
    int32_t image_width,
    int32_t image_height)
{
    printf("disk thread id: %ld\n", syscall(SYS_gettid));

    std::unique_ptr<SERFile> ser_file;
    struct statvfs disk_stats;
    int32_t frame_count = 0;

    bool disk_write_enabled = filename != nullptr;
    if (disk_write_enabled)
    {
        ser_file.reset(
            new SERFile(
                filename,
                image_width,
                image_height,
                (color) ? BAYER_RGGB : MONO,
                8,
                "",
                camera_name,
                ""
            )
        );
    }
    else
    {
        warnx("No filename provided; not writing to disk!\n");
    }

    while (!end_program)
    {
        // Get next frame from deque
        std::unique_lock<std::mutex> to_disk_deque_lock(to_disk_deque_mutex);
        to_disk_deque_cv.wait(to_disk_deque_lock, [&]{return !to_disk_deque.empty() || end_program;});
        if (end_program)
        {
            break;
        }
        Frame *frame = to_disk_deque.back();
        to_disk_deque.pop_back();
        to_disk_deque_lock.unlock();

        if (disk_write_enabled)
        {
            // Check free disk space (but not every single frame)
            if (frame_count % 100 == 0)
            {
                if (statvfs(filename, &disk_stats) != 0)
                {
                    warn("Tried to check disk space with statvfs but the call failed");
                }
                else
                {
                    int64_t free_bytes = disk_stats.f_bsize * disk_stats.f_bavail;
                    if (free_bytes <= MIN_FREE_DISK_SPACE_BYTES)
                    {
                        warnx("Disk is nearly full! Disabled writes: frames going to bit bucket!");
                        disk_write_enabled = false;
                    }
                }
            }

            ser_file->addFrame(*frame);
        }

        frame->decrRefCount();
        frame_count++;
    }

    printf("Disk thread ending.\n");
}
