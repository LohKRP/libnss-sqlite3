/*
 * Copyright (C) 2007, Sébastien Le Ray
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/*
 * groups.c : Functions handling groups entries retrieval.
 */
#include "nss-sqlite.h"
#include "utils.h"

#include <errno.h>
#include <grp.h>
#include <malloc.h>
#include <pthread.h>
#include <string.h>

/*
 * struct used to store data used by getgrent.
 */
static struct {
    sqlite3* pDb;
    sqlite3_stmt* pSt;
    int try_again;      /* flag to know if NSS_TRYAGAIN
                            was returned by previous call
                            to getgrent_r */
    /* group information cache used if NSS_TRYAGAIN was returned */
    struct group entry;
} grent_data = { NULL, NULL, 0, NULL};

/* mutex used to serialize xxgrent operation */
pthread_mutex_t grent_mutex = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

/*
 * Free memory used by a dynamically allocated 2D table.
 * @param m Dynamically allocated table. Each element will
 * be freed, m will be freed too.
 * @param c Number of elements in t.
 */
static void free_2Dtable(char** t, int c) {
    int i;
    for(i = 0 ; i < c ; ++i) {
        free(t[i]);
    }
    free(t);
}

/*
 * Initialize grent functions (serial group access).
 */
enum nss_status _nss_sqlite_setgrent(void) {
    char* sql;
    pthread_mutex_lock(&grent_mutex);
    if(grent_data.pDb == NULL) {
        NSS_DEBUG("setgrent: opening DB connection\n");
        if(sqlite3_open(NSS_SQLITE_PASSWD_DB, &grent_data.pDb) != SQLITE_OK) {
            NSS_ERROR(sqlite3_errmsg(grent_data.pDb));
            return NSS_STATUS_UNAVAIL;
        }
        if(!(sql = get_query(grent_data.pDb, "setgrent")) ) {
            NSS_ERROR(sqlite3_errmsg(grent_data.pDb));
            sqlite3_close(grent_data.pDb);
            return NSS_STATUS_UNAVAIL;
        }
        if(sqlite3_prepare(grent_data.pDb, sql, -1, &grent_data.pSt, NULL) != SQLITE_OK) {
            NSS_ERROR(sqlite3_errmsg(grent_data.pDb));
            sqlite3_finalize(grent_data.pSt);
            sqlite3_close(grent_data.pDb);
            free(sql);
            return NSS_STATUS_UNAVAIL;
        }
    }
    free(sql);
    pthread_mutex_unlock(&grent_mutex);
    return NSS_STATUS_SUCCESS;
}

/*
 * Finalize grent functions.
 */
enum nss_status _nss_sqlite_endgrent(void) {
    NSS_DEBUG("endgrent: finalizing group serial access facilities\n");
    pthread_mutex_lock(&grent_mutex);
    if(grent_data.pDb != NULL) {
        sqlite3_finalize(grent_data.pSt);
        sqlite3_close(grent_data.pDb);
        grent_data.pDb = NULL;
    }
    pthread_mutex_unlock(&grent_mutex);
    return NSS_STATUS_SUCCESS;
}

/*
 * Return next group. see man getgrent_r
 * @param gbuf Buffer to store group data.
 * @param buf Buffer which will contain all string pointed
 * to by gbuf entries.
 * @param buflen buf length.
 * @param errnop Pointer to errno, will be filled if
 * an error occurs.
 */
enum nss_status
_nss_sqlite_getgrent_r(struct group *gbuf, char *buf,
                      size_t buflen, int *errnop) {
    int res;
    NSS_DEBUG("getgrent_r\n");
    pthread_mutex_lock(&grent_mutex);

    if(grent_data.pDb == NULL) {
        _nss_sqlite_setgrent();
    }

    if(grent_data.try_again) {
        res = fill_group(grent_data.pDb, gbuf, buf, buflen, grent_data.entry, errnop);
        /* buffer was long enough this time */
        if(res != NSS_STATUS_TRYAGAIN || (*errnop) != ERANGE) {
            grent_data.try_again = 0;
            pthread_mutex_unlock(&grent_mutex);
            return res;
        }
    }

    res = res2nss_status(sqlite3_step(grent_data.pSt), grent_data.pDb, grent_data.pSt);
    if(res != NSS_STATUS_SUCCESS) {
        grent_data.pDb = NULL;
        pthread_mutex_unlock(&grent_mutex);
        return res;
    }

    fill_group_sql(&grent_data.entry, grent_data.pSt);
    NSS_DEBUG("getgrent_r: fetched group #%d: %s\n", grent_data.entry.gr_gid, grent_data.entry.gr_name);

    res = fill_group(grent_data.pDb, gbuf, buf, buflen, grent_data.entry, errnop);
    if(res == NSS_STATUS_TRYAGAIN && (*errnop) == ERANGE) {
        /* cache result for next try */
        grent_data.try_again = 1;

        pthread_mutex_unlock(&grent_mutex);
        return NSS_STATUS_TRYAGAIN;
    }
    pthread_mutex_unlock(&grent_mutex);
    return NSS_STATUS_SUCCESS;
}

/**
 * Get group by name.
 * @param name Groupname.
 * @param buf Buffer which will contain all string pointed
 * to by gbuf entries.
 * @param buflen buf length.
 * @param errnop Pointer to errno, will be filled if
 * an error occurs.
 */

enum nss_status
_nss_sqlite_getgrnam_r(const char* name, struct group *gbuf,
                      char *buf, size_t buflen, int *errnop) {
    sqlite3 *pDb;
    struct sqlite3_stmt* pSt;
    struct group entry;
    int res;
    char* sql;

    NSS_DEBUG("getgrnam_r : looking for group %s\n", name);

    if(sqlite3_open(NSS_SQLITE_PASSWD_DB, &pDb) != SQLITE_OK) {
        NSS_ERROR(sqlite3_errmsg(pDb));
        sqlite3_close(pDb);
        return NSS_STATUS_UNAVAIL;
    }

    if(!(sql = get_query(pDb, "getgrnam_r")) ) {
        NSS_ERROR(sqlite3_errmsg(pDb));
        sqlite3_close(pDb);
        return NSS_STATUS_UNAVAIL;
    }

    if(sqlite3_prepare(pDb, sql, -1, &pSt, NULL) != SQLITE_OK) {
        NSS_ERROR(sqlite3_errmsg(pDb));
        sqlite3_finalize(pSt);
        sqlite3_close(pDb);
        free(sql);
        return NSS_STATUS_UNAVAIL;
    }

    if(sqlite3_bind_text(pSt, 1, name, -1, SQLITE_STATIC) != SQLITE_OK) {
        NSS_ERROR(sqlite3_errmsg(pDb));
        sqlite3_finalize(pSt);
        sqlite3_close(pDb);
        free(sql);
        return NSS_STATUS_UNAVAIL;
    }

    res = res2nss_status(sqlite3_step(pSt), pDb, pSt);
    if(res != NSS_STATUS_SUCCESS) {
        free(sql);
        return res;
    }

    fill_group_sql(&entry, pSt);

    res = fill_group(pDb, gbuf, buf, buflen, entry, errnop);

    sqlite3_finalize(pSt);
    sqlite3_close(pDb);
    free(sql);
    return res;
}

/*
 * Get group by GID.
 * @param gid GID.
 * @param buf Buffer which will contain all string pointed
 * to by gbuf entries.
 * @param buflen buf length.
 * @param errnop Pointer to errno, will be filled if
 * an error occurs.
 */

enum nss_status
_nss_sqlite_getgrgid_r(gid_t gid, struct group *gbuf,
                      char *buf, size_t buflen, int *errnop) {
     sqlite3 *pDb;
     struct sqlite3_stmt* pSt;
     struct group entry;
     int res;
     char* sql;


    NSS_DEBUG("getgrgid_r : looking for group #%d\n", gid);

    if(sqlite3_open(NSS_SQLITE_PASSWD_DB, &pDb) != SQLITE_OK) {
        NSS_ERROR(sqlite3_errmsg(pDb));
        sqlite3_close(pDb);
        return NSS_STATUS_UNAVAIL;
    }

    if(!(sql = get_query(pDb, "getgrgid_r")) ) {
        NSS_ERROR(sqlite3_errmsg(pDb));
        sqlite3_close(pDb);
        return NSS_STATUS_UNAVAIL;
    }

    if(sqlite3_prepare(pDb, sql, -1, &pSt, NULL) != SQLITE_OK) {
        NSS_ERROR(sqlite3_errmsg(pDb));
        sqlite3_finalize(pSt);
        sqlite3_close(pDb);
        free(sql);
        return NSS_STATUS_UNAVAIL;
    }

    if(sqlite3_bind_int(pSt, 1, gid) != SQLITE_OK) {
        NSS_ERROR(sqlite3_errmsg(pDb));
        sqlite3_finalize(pSt);
        sqlite3_close(pDb);
        free(sql);
        return NSS_STATUS_UNAVAIL;
    }

    res = res2nss_status(sqlite3_step(pSt), pDb, pSt);
    if(res != NSS_STATUS_SUCCESS) {
        free(sql);
        return res;
    }

    fill_group_sql(&entry, pSt);

    res = fill_group(pDb, gbuf, buf, buflen, entry, errnop);

    sqlite3_finalize(pSt);
    sqlite3_close(pDb);
    free(sql);
    return res;

}

/*
 * Haven't seen any detailled documentation about this function.
 * Anyway it have to fill in groups for the specified user without
 * adding his main group (group param).
 * @param user Username whose groups are wanted.
 * @param group Main group of user (should not be put in groupsp).
 * @param start Index from which groups filling must begin (initgroups_dyn
 * is called for every backend). Can be updated
 * @param size Size of groups vector. Can be modified if function needs
 * more space (should not exceed limit).
 * @param groupsp Pointer to the group vector. Can be realloc'ed if more
 * space is needed.
 * @param limit Max size of groupsp (<= 0 if no limit).
 * @param errnop Pointer to errno (filled if an error occurs).
 */

enum nss_status
_nss_sqlite_initgroups_dyn(const char *user, gid_t gid, long int *start,
                          long int *size, gid_t **groupsp, long int limit,
                                                    int *errnop) {
    sqlite3 *pDb;
    struct sqlite3_stmt *pSt;
    char* sql;
    int res;
    NSS_DEBUG("initgroups_dyn: filling groups for user : %s, main gid : %d\n", user, gid);

    if(sqlite3_open(NSS_SQLITE_PASSWD_DB, &pDb) != SQLITE_OK) {
        NSS_ERROR(sqlite3_errmsg(pDb));
        sqlite3_close(pDb);
        return NSS_STATUS_UNAVAIL;
    }

    if(!(sql = get_query(pDb, "initgroups_dyn")) ) {
        NSS_ERROR(sqlite3_errmsg(pDb));
        sqlite3_close(pDb);
        return NSS_STATUS_UNAVAIL;
    }

    if(sqlite3_prepare(pDb, sql, -1, &pSt, NULL) != SQLITE_OK) {
        NSS_ERROR(sqlite3_errmsg(pDb));
        sqlite3_finalize(pSt);
        sqlite3_close(pDb);
        free(sql);
        return NSS_STATUS_UNAVAIL;
    }

    if(sqlite3_bind_text(pSt, 1, user, -1, SQLITE_STATIC) != SQLITE_OK) {
        NSS_ERROR("Unable to bind username in initgroups_dyn\n");
        sqlite3_finalize(pSt);
        sqlite3_close(pDb);
        free(sql);
        return NSS_STATUS_UNAVAIL;
    }

    if(sqlite3_bind_int(pSt, 2, gid) != SQLITE_OK) {
        NSS_ERROR("Unable to bind gid in initgroups_dyn\n");
        sqlite3_finalize(pSt);
        sqlite3_close(pDb);
        free(sql);
        return NSS_STATUS_UNAVAIL;
    }

    res = res2nss_status(sqlite3_step(pSt), pDb, pSt);
    if(res != NSS_STATUS_SUCCESS) {
        free(sql);
        return res;
    }

    do {
        int gid = sqlite3_column_int(pSt, 0);
        NSS_DEBUG("initgroups_dyn: adding group %d\n", gid);
        /* Too short, doubling size */
        if(*start == *size) {
            if(limit > 0) {
                if(*size < limit) {
                    *size = (limit < (*size * 2)) ? limit : (*size * 2);
                } else {
                    /* limit reached, tell caller to try with a bigger one */
                    NSS_ERROR("initgroups_dyn: limit was too low\n");
                    *errnop = ERANGE;
                    sqlite3_finalize(pSt);
                    sqlite3_close(pDb);
                    free(sql);
                    return NSS_STATUS_TRYAGAIN;
                }
            } else {
                (*size) = (*size) * 2;
            }
            *groupsp = realloc(*groupsp, sizeof(**groupsp) * (*size));
        }
        (*groupsp)[*start] = gid;
        (*start)++;
        res = sqlite3_step(pSt);
    } while(res == SQLITE_ROW);
    *groupsp = realloc(*groupsp, sizeof(**groupsp) * (*start));
    *size = *start;

    sqlite3_finalize(pSt);
    sqlite3_close(pDb);
    free(sql);

    return NSS_STATUS_SUCCESS;
}

/*
 * Fills all users for a given group.
 * @param buffer Buffer which will contain all users' names headed
 * with a char* pointers area containing pointer to members' names,
 * ending by NULL.
 * @param pDb DB handle to fetch users (must be opened).
 * @param gid GID.
 * @param buflen Buffer length.
 * @param errnop Pointer to errno, will be filled if an error occurs.
 */

enum nss_status get_users(sqlite3* pDb, gid_t gid, char* buffer, size_t buflen, int* errnop) {
    struct sqlite3_stmt *pSt;
    int res, msize = 20, mcount = 0, i, ptr_area_size;
    char* sql;
    char* next_member;
    char **members;
    char **ptr_area = (char**)buffer;

    NSS_DEBUG("get_users: looking for members of group #%d\n", gid);
    
    if(!(sql = get_query(pDb, "get_users")) ) {
        NSS_ERROR(sqlite3_errmsg(pDb));
        sqlite3_close(pDb);
        return NSS_STATUS_UNAVAIL;
    }

    if(sqlite3_prepare(pDb, sql, strlen(sql), &pSt, NULL) != SQLITE_OK) {
        NSS_ERROR(sqlite3_errmsg(pDb));
        sqlite3_finalize(pSt);
        return NSS_STATUS_UNAVAIL;
    }

    if(sqlite3_bind_int(pSt, 1, gid) != SQLITE_OK) {
        NSS_ERROR(sqlite3_errmsg(pDb));
        sqlite3_finalize(pSt);
        return NSS_STATUS_UNAVAIL;
        free(sql);
    }

    res = sqlite3_step(pSt);

    if(res != SQLITE_ROW) {
        sqlite3_finalize(pSt);
        if(res == SQLITE_DONE) {
            NSS_DEBUG("get_users: No member found\n");
            if(buflen < sizeof(char*)) {
                *errnop = ERANGE;
                free(sql);
                return NSS_STATUS_TRYAGAIN;
            }
            ptr_area[0] = NULL;
            free(sql);
            return NSS_STATUS_SUCCESS;
        }
        free(sql);
        return NSS_STATUS_UNAVAIL;
    }

    /* members is a buffer to temporary hold members (we need to know the count
     * before going further) and SQLite doesn't seems to offer such a
     * functionnality. */
    members = (char**)malloc(msize * sizeof(char*));
    do {
        const unsigned char* member = sqlite3_column_text(pSt, 0);

        if(msize == mcount) {
            msize *= 2;
            members = (char**)realloc(members, msize * sizeof(char*));
        }
        members[mcount] = strdup((char*)member);
        ++mcount;
        res = sqlite3_step(pSt);
    } while(res == SQLITE_ROW);

    free(sql);
    sqlite3_finalize(pSt);

    /* Here is what we want to get :
     * __________________________________________________
     * ...|@1|@2|@3|...|NULL|member1|member2|member3|...
     * --------------------------------------------------
     *    ^ gr_mem
     */

    /* Let's build addresses part */
    ptr_area_size = (mcount + 1) * sizeof(char *);

    if(buflen < ptr_area_size) {
        free_2Dtable(members, mcount);
        (*errnop) = ERANGE;
        return NSS_STATUS_TRYAGAIN;
    }

    next_member = buffer + ptr_area_size;
    buflen -= ptr_area_size;
    for(i = 0 ; i < mcount ; ++i) {
        int l = strlen(members[i]) + 1;
        if(buflen < l) {
            free_2Dtable(members, mcount);
            (*errnop) = ERANGE;
            return NSS_STATUS_TRYAGAIN;
        }
        strcpy(next_member, members[i]);
        ptr_area[i] = next_member;
        buflen -= l;
        next_member  += l;
    }
    ptr_area[i] = NULL;
    free_2Dtable(members, mcount);
    return TRUE;
}

