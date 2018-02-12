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
#include <fcntl.h>
#include <string.h>
#include <dirent.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "lib/authselect_private.h"

struct authselect_dir {
    int fd;
    DIR *dirstream;
    char *path;
    char **profiles;
    size_t num_profiles;
};

void
authselect_dir_free(struct authselect_dir *dir)
{
    int i;

    if (dir == NULL) {
        return;
    }

    /* Close directory and its associated file descriptor. */
    if (dir->dirstream != NULL) {
        closedir(dir->dirstream);
    }

    if (dir->path != NULL) {
        free(dir->path);
    }

    if (dir->profiles != NULL) {
        for (i = 0; dir->profiles[i] != NULL; i++) {
            free(dir->profiles[i]);
        }

        free(dir->profiles);
    }

    free(dir);
}

static errno_t
authselect_dir_add_profile(struct authselect_dir *dir,
                           const char *name)
{
    char *profile;
    char **resized;

    if (name == NULL) {
        return EINVAL;
    }

    profile = strdup(name);
    if (profile == NULL) {
        return ENOMEM;
    }

    resized = reallocarray(dir->profiles, dir->num_profiles + 2, sizeof(char *));
    if (resized == NULL) {
        free(profile);
        return ENOMEM;
    }

    resized[dir->num_profiles] = profile;
    resized[dir->num_profiles + 1] = NULL;

    dir->profiles = resized;
    dir->num_profiles++;

    return EOK;

}

static errno_t
authselect_dir_init(const char *path,
                    struct authselect_dir **_dir)
{
    struct authselect_dir *dir;
    errno_t ret;

    if (path == NULL) {
        return EINVAL;
    }

    dir = calloc(1, sizeof(struct authselect_dir));
    if (dir == NULL) {
        return ENOMEM;
    }

    dir->dirstream = NULL;
    dir->fd = -1;

    dir->path = strdup(path);
    if (dir->path == NULL) {
        ret = ENOMEM;
        goto done;
    }

    dir->profiles = calloc(1, sizeof(char *));
    if (dir->profiles == NULL) {
        ret = ENOMEM;
        goto done;
    }

    *_dir = dir;

    ret = EOK;

done:
    if (ret != EOK) {
        authselect_dir_free(dir);
    }

    return ret;
}

static errno_t
authselect_dir_open(const char *path,
                    struct authselect_dir **_dir)
{
    struct authselect_dir *dir;
    errno_t ret;

    ret = authselect_dir_init(path, &dir);
    if (ret != EOK) {
        return ret;
    }

    dir->dirstream = opendir(path);
    if (dir->dirstream == NULL) {
        ret = errno;
        /* If not found, return empty directory. */
        if (ret == ENOENT) {
            *_dir = dir;
        }
        goto done;
    }

    dir->fd = dirfd(dir->dirstream);
    if (dir->fd == -1) {
        ret = errno;
        goto done;
    }

    *_dir = dir;

    ret = EOK;

done:
    if (ret != EOK && ret != ENOENT) {
        authselect_dir_free(dir);
    }

    return ret;
}

errno_t
authselect_dir_read(const char *dirpath,
                    struct authselect_dir **_dir)
{
    struct authselect_dir *dir = NULL;
    struct dirent *entry;
    struct stat statres;
    errno_t ret;

    INFO("Reading profile directory [%s]", dirpath);

    ret = authselect_dir_open(dirpath, &dir);
    if (ret == ENOENT) {
        /* If not found, return empty directory. */
        WARN("Directory [%s] is missing!", dirpath);
        *_dir = dir;
        return EOK;
    } else if (ret != EOK) {
        ERROR("Unable to open directory [%s] [%d]: %s",
              dirpath, ret, strerror(ret));
        goto done;
    }

    /* Read profiles from directory. */
    errno = 0;
    while ((entry = readdir(dir->dirstream)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        ret = fstatat(dir->fd, entry->d_name, &statres, 0);
        if (ret != 0) {
            ret = errno;
            goto done;
        }

        /* Continue with the next one if it is not a directory. */
        if (!S_ISDIR(statres.st_mode)) {
            WARN("Not a directory: %s", entry->d_name);
            continue;
        }

        /* Otherwise take this as a profile and remember. */
        INFO("Found profile [%s]", entry->d_name);
        ret = authselect_dir_add_profile(dir, entry->d_name);
        if (ret != EOK) {
            goto done;
        }
    }

    if (errno != 0) {
        ret = errno;
        goto done;
    }

    *_dir = dir;

    ret = EOK;

done:
    if (ret != EOK) {
        ERROR("Unable to read directory [%s] [%d]: %s",
              dirpath, ret, strerror(ret));
        authselect_dir_free(dir);
    }

    return ret;
}

static int
sort_profile_ids(const void *a, const void *b)
{
    const char *str_a = *(const char **)a;
    const char *str_b = *(const char **)b;

    if (str_a == NULL) {
        return 1;
    }

    if (str_b == NULL) {
        return -1;
    }

    bool is_custom_a = authselect_is_custom_profile(str_a, NULL);
    bool is_custom_b = authselect_is_custom_profile(str_b, NULL);

    /* Custom profiles go last, otherwise we sort alphabetically. */
    if (is_custom_a && !is_custom_b) {
        return 1;
    }

    if (!is_custom_a && is_custom_b) {
        return -1;
    }

    return strcmp(str_a, str_b);
}

static errno_t
merge_default(char **ids, size_t *index, struct authselect_dir *dir)
{
    size_t i;

    /* Add all profiles from the default profile directory. */
    for (i = 0; i < dir->num_profiles; i++) {
        ids[*index] = strdup(dir->profiles[i]);
        if (ids[*index] == NULL) {
            return ENOMEM;
        }

        (*index)++;
    }

    return EOK;
}

static errno_t
merge_vendor(char **ids, size_t *index, struct authselect_dir *dir)
{
    size_t input_index = *index;
    size_t i, j;

    /* Add only new profiles from the vendor profile directory. */
    for (i = 0; i < dir->num_profiles; i++) {
        for (j = 0; j < input_index; j++) {
            if (strcmp(dir->profiles[i], ids[j]) == 0) {
                break;
            }
        }

        if (j != input_index) {
            continue;
        }

        ids[*index] = strdup(dir->profiles[i]);
        if (ids[*index] == NULL) {
            return ENOMEM;
        }

        (*index)++;
    }

    return EOK;
}

static errno_t
merge_custom(char **ids, size_t *index, struct authselect_dir *dir)
{
    size_t i;

    /* Add all profiles from the custom profile directory,
     * prefix them with custom/. */
    for (i = 0; i < dir->num_profiles; i++) {
        ids[*index] = authselect_profile_custom_id(dir->profiles[i]);
        if (ids[*index] == NULL) {
            return ENOMEM;
        }

        (*index)++;
    }

    return EOK;
}

char **
authselect_merge_profiles(struct authselect_dir *profile,
                          struct authselect_dir *vendor,
                          struct authselect_dir *custom)
{
    size_t num_profiles = 0;
    size_t index;
    errno_t ret;
    char **ids;

    num_profiles += profile->num_profiles;
    num_profiles += vendor->num_profiles;
    num_profiles += custom->num_profiles;

    ids = calloc(num_profiles + 1, sizeof(char *));
    if (ids == NULL) {
        return NULL;
    }

    index = 0;

    ret = merge_default(ids, &index, profile);
    if (ret != EOK) {
        goto fail;
    }

    ret = merge_vendor(ids, &index, vendor);
    if (ret != EOK) {
        goto fail;
    }

    ret = merge_custom(ids, &index, custom);
    if (ret != EOK) {
        goto fail;
    }

    /* Sort the output list. */
    qsort(ids, index + 1, sizeof(char *), sort_profile_ids);

    return ids;

fail:
    free(ids);
    return NULL;
}
