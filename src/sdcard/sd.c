#include "sd.h"
#include "hw_config.h"
#include "f_util.h"
#include "ff.h"
#include "debug.h"
#include <stdint.h>

FATFS fs;
FRESULT fr;

struct {
    FIL file;
    FILINFO stat;
    bool isOpen;
} files[16];


struct {
    DIR dir;
    bool isOpen;
} dirs[2];


void sd_init(void) {
    // Initialize the file system
    fr = f_mount(&fs, "", 1);
    if (fr != FR_OK) {
        fatal("f_mount error: %s (%d)\n", FRESULT_str(fr), fr);
    }
}

int sd_open(const char *path, int oflag) {
    size_t fd;
    BYTE mode = 0;
    if (oflag == O_RDONLY) {
        mode = FA_READ;
    } else if (oflag == O_WRONLY) {
        mode = FA_WRITE;
    } else if (oflag == O_RDWR) {
        mode = FA_READ | FA_WRITE;
    }
    if (oflag & O_CREAT) {
        mode |= FA_CREATE_ALWAYS;
    } else if (oflag & O_TRUNC) {
        mode |= FA_CREATE_ALWAYS;
    } else if (oflag & O_APPEND) {
        mode |= FA_OPEN_APPEND;
    } else {
        mode |= FA_OPEN_EXISTING;
    }
    printf("Opening file: %s with mode: %x\n", path, mode);

    for (fd = 0; fd < sizeof(files) / sizeof(files[0]); ++fd) {
        if (!files[fd].isOpen) {
            break;
        }
    }

    // No fd available
    if (fd >= sizeof(files) / sizeof(files[0])) {
        return -1;
    }

    fr = f_open(&files[fd].file, path, mode);
    if (fr != FR_OK && fr != FR_EXIST) {
        return -1; // Error during opening file
    }

    // Get stat
    fr = f_stat(path, &files[fd].stat);
    if (fr != FR_OK) {
        f_close(&files[fd].file);
        files[fd].isOpen = false;
        return -1; // Error during getting file status
    }

    files[fd].isOpen = true;
    return fd;
}

int sd_openDir(const char *path) {
    size_t dir_fd;

    for (dir_fd = 0; dir_fd < sizeof(dirs) / sizeof(dirs[0]); ++dir_fd) {
        if (!dirs[dir_fd].isOpen) {
            break;
        }
    }

    // No dir fd available
    if (dir_fd >= sizeof(dirs) / sizeof(dirs[0])) {
        return -1;
    }

    fr = f_opendir(&dirs[dir_fd].dir, path);
    if (fr != FR_OK) {
        return -1; // Error during opening directory
    }

    dirs[dir_fd].isOpen = true;
    return dir_fd;
}

int sd_close(int fd) {
    if (fd >= sizeof(files) / sizeof(files[0]) || !files[fd].isOpen) {
        return -1; // Invalid file descriptor
    }

    fr = f_close(&files[fd].file);
    if (fr != FR_OK) {
        return -1; // Error during closing file
    }

    files[fd].isOpen = false;
    return 0;
}

int sd_closeDir(int dir_fd) {
    if (dir_fd >= sizeof(dirs) / sizeof(dirs[0]) || !dirs[dir_fd].isOpen) {
        return -1; // Invalid directory descriptor
    }

    fr = f_closedir(&dirs[dir_fd].dir);
    if (fr != FR_OK) {
        return -1; // Error during closing directory
    }

    dirs[dir_fd].isOpen = false;
    return 0;
}

void sd_flush(int fd) {
    if (fd >= sizeof(files) / sizeof(files[0]) || !files[fd].isOpen) {
        return; // Invalid file descriptor
    }

    fr = f_sync(&files[fd].file);
    if (fr != FR_OK) {
        fatal("f_sync error: %s (%d)\n", FRESULT_str(fr), fr);
    }
}

int sd_read(int fd, void *buf, size_t count) {
    if (fd >= sizeof(files) / sizeof(files[0]) || !files[fd].isOpen) {
        return -1; // Invalid file descriptor
    }

    UINT br = 0;
    fr = f_read(&files[fd].file, buf, count, &br);
    if (fr != FR_OK) {
        return -1; // Error during reading
    }

    return br; // Return number of bytes read

}
int sd_write(int fd, void *buf, size_t count) {
    if (fd >= sizeof(files) / sizeof(files[0]) || !files[fd].isOpen) {
        return -1; // Invalid file descriptor
    }

    UINT bw = 0;
    fr = f_write(&files[fd].file, buf, count, &bw);
    if (fr != FR_OK) {
        printf("f_write error: %s (%d)\n", FRESULT_str(fr), fr);
        return -1; // Error during writing
    }

    return bw; // Return number of bytes written
}

int sd_seek(int fd, int32_t offset, int whence) {
    if (fd >= sizeof(files) / sizeof(files[0]) || !files[fd].isOpen) {
        return -1; // Invalid file descriptor
    }
    switch (whence) {
        case SEEK_SET:
            // Offset is absolute
            break;
        case SEEK_CUR:
            // Offset is relative to current position
            offset += f_tell(&files[fd].file);
            break;
        case SEEK_END:
            // Offset is relative to end of file
            offset += f_size(&files[fd].file);
            break;
        default:
            return -1; // Invalid whence value
    }

    // Return 1 on error
    fr = f_lseek(&files[fd].file, offset);
    if (fr != FR_OK) {
        printf("f_lseek error: %s (%d)\n", FRESULT_str(fr), fr);
        return -1; // Error during seeking
    }

    return 0; // Success
}

uint32_t sd_tell(int fd) {
    if (fd >= sizeof(files) / sizeof(files[0]) || !files[fd].isOpen) {
        return -1; // Invalid file descriptor
    }

    return f_tell(&files[fd].file); // Return current position
}

int sd_getStat(int fd, sd_file_stat_t* const sd_stat) {
    if (fd >= sizeof(files) / sizeof(files[0]) || !files[fd].isOpen) {
        return -1; // Invalid file descriptor
    }


    fr = -1;
    //fr = f_stat(files[fd].file.fname, &fno);
    if (fr != FR_OK) {
        return -1; // Error during getting file status
    }

    sd_stat->size = files[fd].stat.fsize;
    sd_stat->adate = files[fd].stat.fdate;
    sd_stat->atime = files[fd].stat.ftime;
    sd_stat->cdate = files[fd].stat.fdate;
    sd_stat->ctime = files[fd].stat.ftime;
    sd_stat->mdate = files[fd].stat.fdate;
    sd_stat->mtime = files[fd].stat.ftime;
    sd_stat->writable = (files[fd].stat.fattrib & AM_RDO) == 0;

    return 0; // Success
}

int sd_filesize(int fd) {
    if (fd >= sizeof(files) / sizeof(files[0]) || !files[fd].isOpen) {
        return -1; // Invalid file descriptor
    }

    return f_size(&files[fd].file); // Return file size
}

int sd_mkdir(const char *path) {
    fr = f_mkdir(path);
    if (fr != FR_OK) {
        return -1; // Error during creating directory
    }
    return 0; // Success
}

int sd_exists(const char *path) {
    FILINFO fno;
    fr = f_stat(path, &fno);
    if (fr == FR_OK) {
        return 1; // Path exists
    } else if (fr == FR_NO_FILE || fr == FR_NO_PATH) {
        return 0; // Path does not exist
    } else {
        return -1; // Error during checking existence
    }
}

int sd_remove(const char* path) {
    fr = f_unlink(path);
    if (fr != FR_OK) {
        return -1; // Error during removing file
    }
    return 0; // Success
}

int sd_rmdir(const char* path) {
    fr = f_rmdir(path);
    if (fr != FR_OK) {
        return -1; // Error during removing directory
    }
    return 0; // Success
}

int sd_get_stat(int fd, gc_fileio_stat_t* const ps2_fileio_stat) {

    if (fd < 0 || fd >= sizeof(files) / sizeof(files[0]) || !files[fd].isOpen) {
        return -1; // Invalid file descriptor
    }


    return 0; // Success
}

int sd_iterate_dir(int dir, int it) {
    if (dir < 0 || dir >= sizeof(dirs) / sizeof(dirs[0]) || !dirs[dir].isOpen) {
        return -1; // Invalid directory descriptor
    }
    if (it < 0 || it >= sizeof(files) / sizeof(files[0])) {
        return -1; // Invalid iterator index
    }

    f_findnext(&dirs[dir].dir, &files[it].stat);

    return it; // Success
}

size_t sd_get_name(int fd, char* name, size_t size) {

    if (fd < 0 || fd >= sizeof(files) / sizeof(files[0]) || !files[fd].isOpen) {

        return 0; // Invalid file descriptor
    }
    strlcpy(name, files[fd].stat.fname, size);

    return strlen(name);
}

bool sd_is_dir(int fd) {

}

int sd_fd_is_open(int fd) {
    if (fd < 0 || fd >= sizeof(files) / sizeof(files[0])) {
        return 0; // Invalid file descriptor
    }
    return files[fd].isOpen; // Return whether the file is open
}

uint64_t sd_filesize64(int fd) {
    if (fd >= sizeof(files) / sizeof(files[0]) || !files[fd].isOpen) {
        return -1; // Invalid file descriptor
    }

    return f_size(&files[fd].file); // Return file size as uint64_t
}
int sd_seek64(int fd, int64_t offset, int whence) {
    if (fd >= sizeof(files) / sizeof(files[0]) || !files[fd].isOpen) {
        return -1; // Invalid file descriptor
    }
    switch (whence) {
        case SEEK_SET:
            // Offset is absolute
            break;
        case SEEK_CUR:
            // Offset is relative to current position
            offset += f_tell(&files[fd].file);
            break;
        case SEEK_END:
            // Offset is relative to end of file
            offset += f_size(&files[fd].file);
            break;
        default:
            return -1; // Invalid whence value
    }

    // Return 1 on error
    fr = f_lseek(&files[fd].file, offset);
    if (fr != FR_OK) {
        return -1; // Error during seeking
    }

    return 0; // Success
}
uint64_t sd_tell64(int fd) {
    if (fd >= sizeof(files) / sizeof(files[0]) || !files[fd].isOpen) {
        return -1; // Invalid file descriptor
    }

    return f_tell(&files[fd].file); // Return current file position as uint64_t
}