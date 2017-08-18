/*
    Authors:
        Pavel Březina <pbrezina@redhat.com>

    Copyright (C) 2017 Red Hat

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>

#include "lib/authselect_util.h"

void free_string_array(char **array)
{
    int i;

    if (array == NULL) {
        return;
    }

    for (i = 0; array[i] != NULL; i++) {
        free(array[i]);
    }

    free(array);
}

errno_t
trimline(const char *str, char **_trimmed)
{
    char *dup;
    const char *end;

    /* Trim beginning. */
    while (isspace(*str)) {
        str++;
    }

    if (str[0] == '\0') {
        *_trimmed = NULL;
        return EOK;
    }

    /* Trim end. */
    end = str + strlen(str) - 1;
    while (end > str && isspace(*end)) {
        end--;
    }

    dup = strndup(str, end - str + 1);
    if (dup == NULL) {
        return ENOMEM;
    }

    *_trimmed = dup;

    return EOK;
}

static errno_t
read_textfile_internal(FILE *file, const char *filename, char **_content)
{
    size_t read_bytes;
    char *buffer;
    long filelen;
    errno_t ret;

    ret = fseek(file, 0, SEEK_END);
    if (ret != 0) {
        ret = errno;
        goto done;
    }

    filelen = ftell(file);
    if (filelen == -1) {
        ret = errno;
        goto done;
    }

    rewind(file);

    buffer = malloc_zero_array(char, filelen + 1);
    if (buffer == NULL) {
        ret = ENOMEM;
        goto done;
    }

    read_bytes = fread(buffer, sizeof(char), filelen, file);
    if (read_bytes != filelen) {
        free(buffer);
        ret = EIO;
        goto done;
    }

    *_content = buffer;

    ret = EOK;

done:
    /* File descriptor is closed when file is closed. */
    fclose(file);

    if (ret != EOK) {
        ERROR("Unable to read file [%s] [%d]: %s",
              filename, ret, strerror(ret));
    }

    return ret;
}

errno_t
read_textfile(const char *filepath, char **_content)
{
    FILE *file;

    INFO("Reading file [%s]", filepath);

    file = fopen(filepath, "r");
    if (file == NULL) {
        return errno;
    }

    return read_textfile_internal(file, filepath, _content);
}

errno_t
read_textfile_dirfd(int dirfd,
                    const char *dirpath,
                    const char *filename,
                    char **_content)
{
    errno_t ret;
    FILE *file;
    int fd;

    INFO("Reading file [%s/%s]", dirpath, filename);

    fd = openat(dirfd, filename, O_RDONLY);
    if (fd == -1) {
        return errno;
    }

    file = fdopen(fd, "r");
    if (file == NULL) {
        ret = errno;
        close(fd);
        return ret;
    }

    return read_textfile_internal(file, filename, _content);
}

static bool
check_type(struct stat *statbuf,
           const char *name,
           mode_t mode)
{
    mode_t exp_type = mode & S_IFMT;

    if (statbuf == NULL) {
        ERROR("Internal error: stat cannot be NULL!");
        return false;
    }

    if (exp_type != (statbuf->st_mode & S_IFMT)) {
        switch (exp_type) {
        case S_IFDIR:
            ERROR("[%s] is not a directory!", name);
            break;
        case S_IFREG:
            ERROR("[%s] is not a regular file!", name);
            break;
        case S_IFLNK:
            ERROR("[%s] is not a symbolic link!", name);
            break;
        default:
            ERROR("[%s] has wrong type [%.7o], expected [%.7o]!",
                  name, exp_type, exp_type);
            break;
        }

        return false;
    }

    return true;
}

static bool
check_mode(struct stat *statbuf,
           const char *name,
           uid_t uid,
           gid_t gid,
           mode_t mode)
{
    mode_t exp_perm = mode & ALLPERMS;
    bool bret;

    if (statbuf == NULL) {
        ERROR("Internal error: stat cannot be NULL!");
        return false;
    }

    bret = check_type(statbuf, name, mode);
    if (!bret) {
        return false;
    }

    if (exp_perm != (statbuf->st_mode & ALLPERMS)) {
        ERROR("[%s] has wrong mode [%.4o], expected [%.4o]!",
              name, statbuf->st_mode & ALLPERMS, exp_perm);
        return false;
    }

    if (uid != (uid_t)(-1) && statbuf->st_uid != uid) {
        ERROR("[%s] has wrong owner [%u], expected [%u]!",
              name, statbuf->st_uid, uid);
        return false;
    }

    if (gid != (gid_t)(-1) && statbuf->st_gid != gid) {
        ERROR("[%s] has wrong group [%u], expected [%u]!",
              name, statbuf->st_gid, gid);
        return false;
    }

    return true;
}

static errno_t
check_internal(const char *filepath,
               uid_t uid,
               gid_t gid,
               mode_t mode,
               bool *_result)
{
    struct stat statbuf;
    errno_t ret;

    ret = lstat(filepath, &statbuf);
    if (ret == -1) {
        ret = errno;

        if (ret == ENOENT) {
            ERROR("[%s] does not exist!", filepath);
            *_result = false;
            return EOK;
        }

        ERROR("Unable to stat [%s] [%d]: %s", filepath, ret, strerror(ret));
        return ret;
    }

    *_result = check_mode(&statbuf, filepath, uid, gid, mode);

    return EOK;
}

errno_t
check_file(const char *filepath,
           uid_t uid,
           gid_t gid,
           mode_t permissions,
           bool *_result)
{
    INFO("Checking mode of file [%s]", filepath);

    return check_internal(filepath, uid, gid, S_IFREG | permissions, _result);
}

errno_t
check_link(const char *linkpath,
           const char *destpath,
           bool *_result)
{
    char linkbuf[PATH_MAX + 1];
    ssize_t len;
    errno_t ret;

    INFO("Checking link [%s]", linkpath);

    ret = check_internal(linkpath, (uid_t)-1, (gid_t)-1,
                         S_IFLNK | ACCESSPERMS, _result);
    if (ret != EOK || *_result == false) {
        return ret;
    }

    len = readlink(linkpath, linkbuf, PATH_MAX + 1);
    if (len == -1) {
        ret = errno;
        ERROR("Unable to read link destination [%s] [%d]: %s",
              linkpath, ret, strerror(ret));
        return ret;
    }

    if (strncmp(linkbuf, destpath, len) != 0) {
        ERROR("Link [%s] does not point to [%s]", linkpath, destpath);
        *_result = false;
        return EOK;
    }

    *_result = true;
    return EOK;
}

errno_t
check_notalink(const char *linkpath,
               const char *destpath,
               bool *_result)
{
    char linkbuf[PATH_MAX + 1];
    struct stat statbuf;
    ssize_t len;
    errno_t ret;

    INFO("Checking that file [%s] is not an authselect symbolic link [%s]",
          linkpath);

    ret = lstat(linkpath, &statbuf);
    if (ret == -1) {
        ret = errno;

        if (ret == ENOENT) {
            *_result = true;
            return EOK;
        }

        ERROR("Unable to stat [%s] [%d]: %s", linkpath, ret, strerror(ret));
        return ret;
    }

    if (!S_ISLNK(statbuf.st_mode)) {
        *_result = true;
        return EOK;
    }

    len = readlink(linkpath, linkbuf, PATH_MAX + 1);
    if (len == -1) {
        ret = errno;
        ERROR("Unable to read link destination [%s] [%d]: %s",
              linkpath, ret, strerror(ret));
        return ret;
    }

    if (strncmp(linkbuf, destpath, len) == 0) {
        ERROR("Link [%s] points to [%s], it is an authselect symbolic link",
              linkpath, destpath);
        *_result = false;
        return EOK;
    }

    *_result = true;
    return EOK;
}

errno_t
check_access(const char *path, int mode)
{
    errno = 0;
    if (access(path, mode) == 0) {
        return EOK;
    }

    /* ENOENT is returned if a file is missing. */
    return errno;
}

errno_t
check_exists(const char *path)
{
    return check_access(path, F_OK);
}
