/**
 * PhysicsFS; a portable, flexible file i/o abstraction.
 *
 * Documentation is in physfs.h. It's verbose, honest.  :)
 *
 * Please see the file LICENSE in the source's root directory.
 *
 *  This file written by Ryan C. Gordon.
 */

#if HAVE_CONFIG_H
#  include <config.h>
#endif

#if (defined PHYSFS_PROFILING)
#include <sys/time.h>
#include <unistd.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "physfs.h"

#define __PHYSICSFS_INTERNAL__
#include "physfs_internal.h"


typedef struct __PHYSFS_DIRHANDLE__
{
    void *opaque;  /* Instance data unique to the archiver. */
    char *dirName;  /* Path to archive in platform-dependent notation. */
    const PHYSFS_Archiver *funcs;  /* Ptr to archiver info for this handle. */
    struct __PHYSFS_DIRHANDLE__ *next;  /* linked list stuff. */
} DirHandle;


typedef struct __PHYSFS_FILEHANDLE__
{
    void *opaque;  /* Instance data unique to the archiver for this file. */
    PHYSFS_uint8 forReading; /* Non-zero if reading, zero if write/append */
    const DirHandle *dirHandle;  /* Archiver instance that created this */
    const PHYSFS_Archiver *funcs;  /* Ptr to archiver info for this handle. */
    PHYSFS_uint8 *buffer;  /* Buffer, if set (NULL otherwise). Don't touch! */
    PHYSFS_uint32 bufsize;  /* Bufsize, if set (0 otherwise). Don't touch! */
    PHYSFS_uint32 buffill;  /* Buffer fill size. Don't touch! */
    PHYSFS_uint32 bufpos;  /* Buffer position. Don't touch! */
    struct __PHYSFS_FILEHANDLE__ *next;  /* linked list stuff. */
} FileHandle;


typedef struct __PHYSFS_ERRMSGTYPE__
{
    PHYSFS_uint64 tid;
    int errorAvailable;
    char errorString[80];
    struct __PHYSFS_ERRMSGTYPE__ *next;
} ErrMsg;


/* The various i/o drivers... */

#if (defined PHYSFS_SUPPORTS_ZIP)
extern const PHYSFS_ArchiveInfo    __PHYSFS_ArchiveInfo_ZIP;
extern const PHYSFS_Archiver       __PHYSFS_Archiver_ZIP;
#endif

#if (defined PHYSFS_SUPPORTS_GRP)
extern const PHYSFS_ArchiveInfo    __PHYSFS_ArchiveInfo_GRP;
extern const PHYSFS_Archiver       __PHYSFS_Archiver_GRP;
#endif

#if (defined PHYSFS_SUPPORTS_QPAK)
extern const PHYSFS_ArchiveInfo    __PHYSFS_ArchiveInfo_QPAK;
extern const PHYSFS_Archiver       __PHYSFS_Archiver_QPAK;
#endif

#if (defined PHYSFS_SUPPORTS_HOG)
extern const PHYSFS_ArchiveInfo    __PHYSFS_ArchiveInfo_HOG;
extern const PHYSFS_Archiver       __PHYSFS_Archiver_HOG;
#endif

#if (defined PHYSFS_SUPPORTS_MVL)
extern const PHYSFS_ArchiveInfo    __PHYSFS_ArchiveInfo_MVL;
extern const PHYSFS_Archiver       __PHYSFS_Archiver_MVL;
#endif

#if (defined PHYSFS_SUPPORTS_WAD)
extern const PHYSFS_ArchiveInfo    __PHYSFS_ArchiveInfo_WAD;
extern const PHYSFS_Archiver       __PHYSFS_Archiver_WAD;
#endif

#if (defined PHYSFS_SUPPORTS_MIX)
extern const PHYSFS_ArchiveInfo    __PHYSFS_ArchiveInfo_MIX;
extern const PHYSFS_Archiver       __PHYSFS_Archiver_MIX;
#endif

extern const PHYSFS_Archiver  __PHYSFS_Archiver_DIR;


static const PHYSFS_ArchiveInfo *supported_types[] =
{
#if (defined PHYSFS_SUPPORTS_ZIP)
    &__PHYSFS_ArchiveInfo_ZIP,
#endif

#if (defined PHYSFS_SUPPORTS_GRP)
    &__PHYSFS_ArchiveInfo_GRP,
#endif

#if (defined PHYSFS_SUPPORTS_QPAK)
    &__PHYSFS_ArchiveInfo_QPAK,
#endif

#if (defined PHYSFS_SUPPORTS_HOG)
    &__PHYSFS_ArchiveInfo_HOG,
#endif

#if (defined PHYSFS_SUPPORTS_MVL)
    &__PHYSFS_ArchiveInfo_MVL,
#endif

#if (defined PHYSFS_SUPPORTS_WAD)
    &__PHYSFS_ArchiveInfo_WAD,
#endif

#if (defined PHYSFS_SUPPORTS_MIX)
    &__PHYSFS_ArchiveInfo_MIX,
#endif

    NULL
};

static const PHYSFS_Archiver *archivers[] =
{
#if (defined PHYSFS_SUPPORTS_ZIP)
    &__PHYSFS_Archiver_ZIP,
#endif

#if (defined PHYSFS_SUPPORTS_GRP)
    &__PHYSFS_Archiver_GRP,
#endif

#if (defined PHYSFS_SUPPORTS_QPAK)
    &__PHYSFS_Archiver_QPAK,
#endif

#if (defined PHYSFS_SUPPORTS_HOG)
    &__PHYSFS_Archiver_HOG,
#endif

#if (defined PHYSFS_SUPPORTS_MVL)
    &__PHYSFS_Archiver_MVL,
#endif

#if (defined PHYSFS_SUPPORTS_WAD)
    &__PHYSFS_Archiver_WAD,
#endif

#if (defined PHYSFS_SUPPORTS_MIX)
    &__PHYSFS_Archiver_MIX,
#endif

    &__PHYSFS_Archiver_DIR,
    NULL
};



/* General PhysicsFS state ... */
static int initialized = 0;
static ErrMsg *errorMessages = NULL;
static DirHandle *searchPath = NULL;
static DirHandle *writeDir = NULL;
static FileHandle *openWriteList = NULL;
static FileHandle *openReadList = NULL;
static char *baseDir = NULL;
static char *userDir = NULL;
static int allowSymLinks = 0;

/* mutexes ... */
static void *errorLock = NULL;     /* protects error message list.        */
static void *stateLock = NULL;     /* protects other PhysFS static state. */

/* allocator ... */
static int externalAllocator = 0;
static PHYSFS_Allocator allocator;


/* functions ... */

typedef struct
{
    char **list;
    PHYSFS_uint32 size;
    const char *errorstr;
} EnumStringListCallbackData;

static void enumStringListCallback(void *data, const char *str)
{
    void *ptr;
    char *newstr;
    EnumStringListCallbackData *pecd = (EnumStringListCallbackData *) data;

    if (pecd->errorstr)
        return;

    ptr = realloc(pecd->list, (pecd->size + 2) * sizeof (char *));
    newstr = malloc(strlen(str) + 1);
    if (ptr != NULL)
        pecd->list = (char **) ptr;

    if ((ptr == NULL) || (newstr == NULL))
    {
        pecd->errorstr = ERR_OUT_OF_MEMORY;
        pecd->list[pecd->size] = NULL;
        PHYSFS_freeList(pecd->list);
        return;
    } /* if */

    strcpy(newstr, str);
    pecd->list[pecd->size] = newstr;
    pecd->size++;
} /* enumStringListCallback */


static char **doEnumStringList(void (*func)(PHYSFS_StringCallback, void *))
{
    EnumStringListCallbackData ecd;
    memset(&ecd, '\0', sizeof (ecd));
    ecd.list = (char **) malloc(sizeof (char *));
    BAIL_IF_MACRO(ecd.list == NULL, ERR_OUT_OF_MEMORY, NULL);
    func(enumStringListCallback, &ecd);
    BAIL_IF_MACRO(ecd.errorstr != NULL, ecd.errorstr, NULL);
    ecd.list[ecd.size] = NULL;
    return(ecd.list);
} /* doEnumStringList */


static void __PHYSFS_bubble_sort(void *a, PHYSFS_uint32 lo, PHYSFS_uint32 hi,
                         int (*cmpfn)(void *, PHYSFS_uint32, PHYSFS_uint32),
                         void (*swapfn)(void *, PHYSFS_uint32, PHYSFS_uint32))
{
    PHYSFS_uint32 i;
    int sorted;

    do
    {
        sorted = 1;
        for (i = lo; i < hi; i++)
        {
            if (cmpfn(a, i, i + 1) > 0)
            {
                swapfn(a, i, i + 1);
                sorted = 0;
            } /* if */
        } /* for */
    } while (!sorted);
} /* __PHYSFS_bubble_sort */


static void __PHYSFS_quick_sort(void *a, PHYSFS_uint32 lo, PHYSFS_uint32 hi,
                         int (*cmpfn)(void *, PHYSFS_uint32, PHYSFS_uint32),
                         void (*swapfn)(void *, PHYSFS_uint32, PHYSFS_uint32))
{
    PHYSFS_uint32 i;
    PHYSFS_uint32 j;
    PHYSFS_uint32 v;

    if ((hi - lo) <= PHYSFS_QUICKSORT_THRESHOLD)
        __PHYSFS_bubble_sort(a, lo, hi, cmpfn, swapfn);
    else
    {
        i = (hi + lo) / 2;

        if (cmpfn(a, lo, i) > 0) swapfn(a, lo, i);
        if (cmpfn(a, lo, hi) > 0) swapfn(a, lo, hi);
        if (cmpfn(a, i, hi) > 0) swapfn(a, i, hi);

        j = hi - 1;
        swapfn(a, i, j);
        i = lo;
        v = j;
        while (1)
        {
            while(cmpfn(a, ++i, v) < 0) { /* do nothing */ }
            while(cmpfn(a, --j, v) > 0) { /* do nothing */ }
            if (j < i)
                break;
            swapfn(a, i, j);
        } /* while */
        swapfn(a, i, hi-1);
        __PHYSFS_quick_sort(a, lo, j, cmpfn, swapfn);
        __PHYSFS_quick_sort(a, i+1, hi, cmpfn, swapfn);
    } /* else */
} /* __PHYSFS_quick_sort */


void __PHYSFS_sort(void *entries, PHYSFS_uint32 max,
                   int (*cmpfn)(void *, PHYSFS_uint32, PHYSFS_uint32),
                   void (*swapfn)(void *, PHYSFS_uint32, PHYSFS_uint32))
{
    /*
     * Quicksort w/ Bubblesort fallback algorithm inspired by code from here:
     *   http://www.cs.ubc.ca/spider/harrison/Java/sorting-demo.html
     */
    __PHYSFS_quick_sort(entries, 0, max - 1, cmpfn, swapfn);
} /* __PHYSFS_sort */



#if (defined PHYSFS_PROFILING)

#define PHYSFS_TEST_SORT_ITERATIONS 150
#define PHYSFS_TEST_SORT_ELEMENTS   (64 * 1024)

static int __PHYSFS_test_sort_cmp(void *_a, PHYSFS_uint32 x, PHYSFS_uint32 y)
{
    PHYSFS_sint32 *a = (PHYSFS_sint32 *) _a;
    PHYSFS_sint32 one = a[x];
    PHYSFS_sint32 two = a[y];

    if (one < two)
        return(-1);
    else if (one > two)
        return(1);

    return(0);
} /* __PHYSFS_test_sort_cmp */


static void __PHYSFS_test_sort_swap(void *_a, PHYSFS_uint32 x, PHYSFS_uint32 y)
{
    PHYSFS_sint32 *a = (PHYSFS_sint32 *) _a;
    PHYSFS_sint32 tmp;
    tmp = a[x];
    a[x] = a[y];
    a[y] = tmp;
} /* __PHYSFS_test_sort_swap */


static int __PHYSFS_test_sort_do(PHYSFS_uint32 *timer,
                         PHYSFS_sint32 *a, PHYSFS_uint32 max,
                         int (*cmpfn)(void *, PHYSFS_uint32, PHYSFS_uint32),
                         void (*swapfn)(void *, PHYSFS_uint32, PHYSFS_uint32))
{
    PHYSFS_uint32 i;
    struct timeval starttime, endtime;

    gettimeofday(&starttime, NULL);
    __PHYSFS_sort(a, max, cmpfn, swapfn);
    gettimeofday(&endtime, NULL);

    for (i = 1; i < max; i++)
    {
        if (a[i] < a[i - 1])
            return(0);
    } /* for */

    if (timer != NULL)
    {
        *timer = ( ((endtime.tv_sec  - starttime.tv_sec) * 1000) +
                   ((endtime.tv_usec - starttime.tv_usec) / 1000) );
    } /* if */

    return(1);
} /* __PHYSFS_test_sort_time */


static void __PHYSFS_test_sort(void)
{
    PHYSFS_uint32 elasped[PHYSFS_TEST_SORT_ITERATIONS];
    PHYSFS_sint32 iter;
    PHYSFS_sint32 a[PHYSFS_TEST_SORT_ELEMENTS];
    PHYSFS_sint32 i, x;
    int success;

    printf("Testing __PHYSFS_sort (linear presorted) ... ");
    for (iter = 0; iter < PHYSFS_TEST_SORT_ITERATIONS; iter++)
    {
        /* set up array to sort. */
        for (i = 0; i < PHYSFS_TEST_SORT_ELEMENTS; i++)
            a[i] = i;

        /* sort it. */
        success = __PHYSFS_test_sort_do(&elasped[iter],
                                        a, PHYSFS_TEST_SORT_ELEMENTS,
                                        __PHYSFS_test_sort_cmp,
                                        __PHYSFS_test_sort_swap);
        if (!success)
            break;
    } /* for */

    if (!success)
        printf("Failed!\n");
    else
    {
        for (x = 0, iter = 0; iter < PHYSFS_TEST_SORT_ITERATIONS; iter++)
            x += elasped[iter];

        x /= PHYSFS_TEST_SORT_ITERATIONS;
        printf("Average run (%lu) ms.\n", (unsigned long) x);
    } /* else */

    printf("Testing __PHYSFS_sort (linear presorted reverse) ... ");
    for (iter = 0; iter < PHYSFS_TEST_SORT_ITERATIONS; iter++)
    {
        /* set up array to sort. */
        for (i = 0, x = PHYSFS_TEST_SORT_ELEMENTS;
             i < PHYSFS_TEST_SORT_ELEMENTS;
             i++, x--)
        {
            a[i] = x;
        } /* for */

        /* sort it. */
        success = __PHYSFS_test_sort_do(&elasped[iter],
                                        a, PHYSFS_TEST_SORT_ELEMENTS,
                                        __PHYSFS_test_sort_cmp,
                                        __PHYSFS_test_sort_swap);
        if (!success)
            break;
    } /* for */

    if (!success)
        printf("Failed!\n");
    else
    {
        for (x = 0, iter = 0; iter < PHYSFS_TEST_SORT_ITERATIONS; iter++)
            x += elasped[iter];

        x /= PHYSFS_TEST_SORT_ITERATIONS;
        printf("Average run (%lu) ms.\n", (unsigned long) x);
    } /* else */


    printf("Testing __PHYSFS_sort (randomized) ... ");
    for (iter = 0; iter < PHYSFS_TEST_SORT_ITERATIONS; iter++)
    {
        /* set up array to sort. */
        for (i = 0; i < PHYSFS_TEST_SORT_ELEMENTS; i++)
            a[i] = (PHYSFS_uint32) rand();

        /* sort it. */
        success = __PHYSFS_test_sort_do(&elasped[iter],
                                        a, PHYSFS_TEST_SORT_ELEMENTS,
                                        __PHYSFS_test_sort_cmp,
                                        __PHYSFS_test_sort_swap);
        if (!success)
            break;
    } /* for */

    if (!success)
        printf("Failed!\n");
    else
    {
        for (x = 0, iter = 0; iter < PHYSFS_TEST_SORT_ITERATIONS; iter++)
            x += elasped[iter];

        x /= PHYSFS_TEST_SORT_ITERATIONS;
        printf("Average run (%lu) ms.\n", (unsigned long) x);
    } /* else */

    printf("__PHYSFS_test_sort() complete.\n\n");
} /* __PHYSFS_test_sort */
#endif


static ErrMsg *findErrorForCurrentThread(void)
{
    ErrMsg *i;
    PHYSFS_uint64 tid;

    if (errorLock != NULL)
        __PHYSFS_platformGrabMutex(errorLock);

    if (errorMessages != NULL)
    {
        tid = __PHYSFS_platformGetThreadID();

        for (i = errorMessages; i != NULL; i = i->next)
        {
            if (i->tid == tid)
            {
                if (errorLock != NULL)
                    __PHYSFS_platformReleaseMutex(errorLock);
                return(i);
            } /* if */
        } /* for */
    } /* if */

    if (errorLock != NULL)
        __PHYSFS_platformReleaseMutex(errorLock);

    return(NULL);   /* no error available. */
} /* findErrorForCurrentThread */


void __PHYSFS_setError(const char *str)
{
    ErrMsg *err;

    if (str == NULL)
        return;

    err = findErrorForCurrentThread();

    if (err == NULL)
    {
        err = (ErrMsg *) malloc(sizeof (ErrMsg));
        if (err == NULL)
            return;   /* uhh...? */

        memset((void *) err, '\0', sizeof (ErrMsg));
        err->tid = __PHYSFS_platformGetThreadID();

        if (errorLock != NULL)
            __PHYSFS_platformGrabMutex(errorLock);

        err->next = errorMessages;
        errorMessages = err;

        if (errorLock != NULL)
            __PHYSFS_platformReleaseMutex(errorLock);
    } /* if */

    err->errorAvailable = 1;
    strncpy(err->errorString, str, sizeof (err->errorString));
    err->errorString[sizeof (err->errorString) - 1] = '\0';
} /* __PHYSFS_setError */


const char *PHYSFS_getLastError(void)
{
    ErrMsg *err = findErrorForCurrentThread();

    if ((err == NULL) || (!err->errorAvailable))
        return(NULL);

    err->errorAvailable = 0;
    return(err->errorString);
} /* PHYSFS_getLastError */


/* MAKE SURE that errorLock is held before calling this! */
static void freeErrorMessages(void)
{
    ErrMsg *i;
    ErrMsg *next;

    for (i = errorMessages; i != NULL; i = next)
    {
        next = i->next;
        free(i);
    } /* for */

    errorMessages = NULL;
} /* freeErrorMessages */


void PHYSFS_getLinkedVersion(PHYSFS_Version *ver)
{
    if (ver != NULL)
    {
        ver->major = PHYSFS_VER_MAJOR;
        ver->minor = PHYSFS_VER_MINOR;
        ver->patch = PHYSFS_VER_PATCH;
    } /* if */
} /* PHYSFS_getLinkedVersion */


static const char *find_filename_extension(const char *fname)
{
    const char *retval = strchr(fname, '.');
    const char *p = retval;

    while (p != NULL)
    {
        p = strchr(p + 1, '.');
        if (p != NULL)
            retval = p;
    } /* while */

    if (retval != NULL)
        retval++;  /* skip '.' */

    return(retval);
} /* find_filename_extension */


static DirHandle *tryOpenDir(const PHYSFS_Archiver *funcs,
                             const char *d, int forWriting)
{
    DirHandle *retval = NULL;
    if (funcs->isArchive(d, forWriting))
    {
        void *opaque = funcs->openArchive(d, forWriting);
        if (opaque != NULL)
        {
            retval = (DirHandle *) allocator.malloc(sizeof (DirHandle));
            if (retval == NULL)
                funcs->dirClose(opaque);
            else
            {
                memset(retval, '\0', sizeof (DirHandle));
                retval->funcs = funcs;
                retval->opaque = opaque;
            } /* else */
        } /* if */
    } /* if */

    return(retval);
} /* tryOpenDir */


static DirHandle *openDirectory(const char *d, int forWriting)
{
    DirHandle *retval = NULL;
    const PHYSFS_Archiver **i;
    const char *ext;

    BAIL_IF_MACRO(!__PHYSFS_platformExists(d), ERR_NO_SUCH_FILE, NULL);

    ext = find_filename_extension(d);
    if (ext != NULL)
    {
        /* Look for archivers with matching file extensions first... */
        for (i = archivers; (*i != NULL) && (retval == NULL); i++)
        {
            if (__PHYSFS_platformStricmp(ext, (*i)->info->extension) == 0)
                retval = tryOpenDir(*i, d, forWriting);
        } /* for */

        /* failing an exact file extension match, try all the others... */
        for (i = archivers; (*i != NULL) && (retval == NULL); i++)
        {
            if (__PHYSFS_platformStricmp(ext, (*i)->info->extension) != 0)
                retval = tryOpenDir(*i, d, forWriting);
        } /* for */
    } /* if */

    else  /* no extension? Try them all. */
    {
        for (i = archivers; (*i != NULL) && (retval == NULL); i++)
            retval = tryOpenDir(*i, d, forWriting);
    } /* else */

    BAIL_IF_MACRO(retval == NULL, ERR_UNSUPPORTED_ARCHIVE, NULL);
    return(retval);
} /* openDirectory */


static DirHandle *createDirHandle(const char *newDir, int forWriting)
{
    DirHandle *dirHandle = NULL;

    BAIL_IF_MACRO(newDir == NULL, ERR_INVALID_ARGUMENT, NULL);

    dirHandle = openDirectory(newDir, forWriting);
    BAIL_IF_MACRO(dirHandle == NULL, NULL, NULL);

    dirHandle->dirName = (char *) malloc(strlen(newDir) + 1);
    if (dirHandle->dirName == NULL)
    {
        dirHandle->funcs->dirClose(dirHandle->opaque);
        free(dirHandle);
        BAIL_MACRO(ERR_OUT_OF_MEMORY, NULL);
    } /* if */

    strcpy(dirHandle->dirName, newDir);
    return(dirHandle);
} /* createDirHandle */


/* MAKE SURE you've got the stateLock held before calling this! */
static int freeDirHandle(DirHandle *dh, FileHandle *openList)
{
    FileHandle *i;

    if (dh == NULL)
        return(1);

    for (i = openList; i != NULL; i = i->next)
        BAIL_IF_MACRO(i->dirHandle == dh, ERR_FILES_STILL_OPEN, 0);
    
    dh->funcs->dirClose(dh->opaque);
    free(dh->dirName);
    free(dh);
    return(1);
} /* freeDirHandle */


static char *calculateUserDir(void)
{
    char *retval = NULL;
    const char *str = NULL;

    str = __PHYSFS_platformGetUserDir();
    if (str != NULL)
        retval = (char *) str;
    else
    {
        const char *dirsep = PHYSFS_getDirSeparator();
        const char *uname = __PHYSFS_platformGetUserName();

        str = (uname != NULL) ? uname : "default";
        retval = (char *) malloc(strlen(baseDir) + strlen(str) +
                                 strlen(dirsep) + 6);

        if (retval == NULL)
            __PHYSFS_setError(ERR_OUT_OF_MEMORY);
        else
            sprintf(retval, "%susers%s%s", baseDir, dirsep, str);

        if (uname != NULL)
            free((void *) uname);
    } /* else */

    return(retval);
} /* calculateUserDir */


static int appendDirSep(char **dir)
{
    const char *dirsep = PHYSFS_getDirSeparator();
    char *ptr;

    if (strcmp((*dir + strlen(*dir)) - strlen(dirsep), dirsep) == 0)
        return(1);

    ptr = realloc(*dir, strlen(*dir) + strlen(dirsep) + 1);
    if (!ptr)
    {
        free(*dir);
        return(0);
    } /* if */

    strcat(ptr, dirsep);
    *dir = ptr;
    return(1);
} /* appendDirSep */


static char *calculateBaseDir(const char *argv0)
{
    const char *dirsep = PHYSFS_getDirSeparator();
    char *retval;
    char *ptr;

    /*
     * See if the platform driver wants to handle this for us...
     */
    retval = __PHYSFS_platformCalcBaseDir(argv0);
    if (retval != NULL)
        return(retval);

    /*
     * Determine if there's a path on argv0. If there is, that's the base dir.
     */
    ptr = strstr(argv0, dirsep);
    if (ptr != NULL)
    {
        char *p = ptr;
        size_t size;
        while (p != NULL)
        {
            ptr = p;
            p = strstr(p + 1, dirsep);
        } /* while */

        size = (size_t) (ptr - argv0);
        retval = (char *) malloc(size + 1);
        BAIL_IF_MACRO(retval == NULL, ERR_OUT_OF_MEMORY, NULL);
        memcpy(retval, argv0, size);
        retval[size] = '\0';
        return(retval);
    } /* if */

    /*
     * Last ditch effort: it's the current working directory. (*shrug*)
     */
    retval = __PHYSFS_platformCurrentDir();
    if (retval != NULL)
        return(retval);

    /*
     * Ok, current directory doesn't exist, use the root directory.
     * Not a good alternative, but it only happens if the current
     * directory was deleted from under the program.
     */
    retval = (char *) malloc(strlen(dirsep) + 1);
    strcpy(retval, dirsep);
    return(retval);
} /* calculateBaseDir */


static int initializeMutexes(void)
{
    errorLock = __PHYSFS_platformCreateMutex();
    if (errorLock == NULL)
        goto initializeMutexes_failed;

    stateLock = __PHYSFS_platformCreateMutex();
    if (stateLock == NULL)
        goto initializeMutexes_failed;

    return(1);  /* success. */

initializeMutexes_failed:
    if (errorLock != NULL)
        __PHYSFS_platformDestroyMutex(errorLock);

    if (stateLock != NULL)
        __PHYSFS_platformDestroyMutex(stateLock);

    errorLock = stateLock = NULL;
    return(0);  /* failed. */
} /* initializeMutexes */


static void setDefaultAllocator(void);

int PHYSFS_init(const char *argv0)
{
    char *ptr;

    BAIL_IF_MACRO(initialized, ERR_IS_INITIALIZED, 0);

    if (!externalAllocator)
        setDefaultAllocator();

    BAIL_IF_MACRO(!allocator.init(), NULL, 0);

    BAIL_IF_MACRO(!__PHYSFS_platformInit(), NULL, 0);

    BAIL_IF_MACRO(!initializeMutexes(), NULL, 0);

    baseDir = calculateBaseDir(argv0);
    BAIL_IF_MACRO(baseDir == NULL, NULL, 0);

    ptr = __PHYSFS_platformRealPath(baseDir);
    free(baseDir);
    BAIL_IF_MACRO(ptr == NULL, NULL, 0);
    baseDir = ptr;

    BAIL_IF_MACRO(!appendDirSep(&baseDir), NULL, 0);

    userDir = calculateUserDir();
    if (userDir != NULL)
    {
        ptr = __PHYSFS_platformRealPath(userDir);
        free(userDir);
        userDir = ptr;
    } /* if */

    if ((userDir == NULL) || (!appendDirSep(&userDir)))
    {
        free(baseDir);
        baseDir = NULL;
        return(0);
    } /* if */

    initialized = 1;

    /* This makes sure that the error subsystem is initialized. */
    __PHYSFS_setError(PHYSFS_getLastError());

#if (defined PHYSFS_PROFILING)
    srand(time(NULL));
    setbuf(stdout, NULL);
    printf("\n");
    printf("********************************************************\n");
    printf("Warning! Profiling is built into this copy of PhysicsFS!\n");
    printf("********************************************************\n");
    printf("\n");
    printf("\n");
    __PHYSFS_test_sort();
#endif

    return(1);
} /* PHYSFS_init */


/* MAKE SURE you hold stateLock before calling this! */
static int closeFileHandleList(FileHandle **list)
{
    FileHandle *i;
    FileHandle *next = NULL;

    for (i = *list; i != NULL; i = next)
    {
        next = i->next;
        if (!i->funcs->fileClose(i->opaque))
        {
            *list = i;
            return(0);
        } /* if */

        free(i);
    } /* for */

    *list = NULL;
    return(1);
} /* closeFileHandleList */


/* MAKE SURE you hold the stateLock before calling this! */
static void freeSearchPath(void)
{
    DirHandle *i;
    DirHandle *next = NULL;

    closeFileHandleList(&openReadList);

    if (searchPath != NULL)
    {
        for (i = searchPath; i != NULL; i = next)
        {
            next = i->next;
            freeDirHandle(i, openReadList);
        } /* for */
        searchPath = NULL;
    } /* if */
} /* freeSearchPath */


int PHYSFS_deinit(void)
{
    BAIL_IF_MACRO(!initialized, ERR_NOT_INITIALIZED, 0);
    BAIL_IF_MACRO(!__PHYSFS_platformDeinit(), NULL, 0);

    closeFileHandleList(&openWriteList);
    BAIL_IF_MACRO(!PHYSFS_setWriteDir(NULL), ERR_FILES_STILL_OPEN, 0);

    freeSearchPath();
    freeErrorMessages();

    if (baseDir != NULL)
    {
        free(baseDir);
        baseDir = NULL;
    } /* if */

    if (userDir != NULL)
    {
        free(userDir);
        userDir = NULL;
    } /* if */

    allowSymLinks = 0;
    initialized = 0;

    __PHYSFS_platformDestroyMutex(errorLock);
    __PHYSFS_platformDestroyMutex(stateLock);

    allocator.deinit();

    errorLock = stateLock = NULL;
    return(1);
} /* PHYSFS_deinit */


const PHYSFS_ArchiveInfo **PHYSFS_supportedArchiveTypes(void)
{
    return(supported_types);
} /* PHYSFS_supportedArchiveTypes */


void PHYSFS_freeList(void *list)
{
    void **i;
    for (i = (void **) list; *i != NULL; i++)
        free(*i);

    free(list);
} /* PHYSFS_freeList */


const char *PHYSFS_getDirSeparator(void)
{
    return(__PHYSFS_platformDirSeparator);
} /* PHYSFS_getDirSeparator */


char **PHYSFS_getCdRomDirs(void)
{
    return(doEnumStringList(__PHYSFS_platformDetectAvailableCDs));
} /* PHYSFS_getCdRomDirs */


void PHYSFS_getCdRomDirsCallback(PHYSFS_StringCallback callback, void *data)
{
    __PHYSFS_platformDetectAvailableCDs(callback, data);
} /* PHYSFS_getCdRomDirsCallback */


const char *PHYSFS_getBaseDir(void)
{
    return(baseDir);   /* this is calculated in PHYSFS_init()... */
} /* PHYSFS_getBaseDir */


const char *PHYSFS_getUserDir(void)
{
    return(userDir);   /* this is calculated in PHYSFS_init()... */
} /* PHYSFS_getUserDir */


const char *PHYSFS_getWriteDir(void)
{
    const char *retval = NULL;

    __PHYSFS_platformGrabMutex(stateLock);
    if (writeDir != NULL)
        retval = writeDir->dirName;
    __PHYSFS_platformReleaseMutex(stateLock);

    return(retval);
} /* PHYSFS_getWriteDir */


int PHYSFS_setWriteDir(const char *newDir)
{
    int retval = 1;

    __PHYSFS_platformGrabMutex(stateLock);

    if (writeDir != NULL)
    {
        BAIL_IF_MACRO_MUTEX(!freeDirHandle(writeDir, openWriteList), NULL,
                            stateLock, 0);
        writeDir = NULL;
    } /* if */

    if (newDir != NULL)
    {
        writeDir = createDirHandle(newDir, 1);
        retval = (writeDir != NULL);
    } /* if */

    __PHYSFS_platformReleaseMutex(stateLock);

    return(retval);
} /* PHYSFS_setWriteDir */


int PHYSFS_addToSearchPath(const char *newDir, int appendToPath)
{
    DirHandle *dh;
    DirHandle *prev = NULL;
    DirHandle *i;

    __PHYSFS_platformGrabMutex(stateLock);

    for (i = searchPath; i != NULL; i = i->next)
    {
        /* already in search path? */
        BAIL_IF_MACRO_MUTEX(strcmp(newDir, i->dirName)==0, NULL, stateLock, 1);
        prev = i;
    } /* for */

    dh = createDirHandle(newDir, 0);
    BAIL_IF_MACRO_MUTEX(dh == NULL, NULL, stateLock, 0);

    if (appendToPath)
    {
        if (prev == NULL)
            searchPath = dh;
        else
            prev->next = dh;
    } /* if */
    else
    {
        dh->next = searchPath;
        searchPath = dh;
    } /* else */

    __PHYSFS_platformReleaseMutex(stateLock);
    return(1);
} /* PHYSFS_addToSearchPath */


int PHYSFS_removeFromSearchPath(const char *oldDir)
{
    DirHandle *i;
    DirHandle *prev = NULL;
    DirHandle *next = NULL;

    BAIL_IF_MACRO(oldDir == NULL, ERR_INVALID_ARGUMENT, 0);

    __PHYSFS_platformGrabMutex(stateLock);
    for (i = searchPath; i != NULL; i = i->next)
    {
        if (strcmp(i->dirName, oldDir) == 0)
        {
            next = i->next;
            BAIL_IF_MACRO_MUTEX(!freeDirHandle(i, openReadList), NULL,
                                stateLock, 0);

            if (prev == NULL)
                searchPath = next;
            else
                prev->next = next;

            BAIL_MACRO_MUTEX(NULL, stateLock, 1);
        } /* if */
        prev = i;
    } /* for */

    BAIL_MACRO_MUTEX(ERR_NOT_IN_SEARCH_PATH, stateLock, 0);
} /* PHYSFS_removeFromSearchPath */


char **PHYSFS_getSearchPath(void)
{
    return(doEnumStringList(PHYSFS_getSearchPathCallback));
} /* PHYSFS_getSearchPath */


void PHYSFS_getSearchPathCallback(PHYSFS_StringCallback callback, void *data)
{
    DirHandle *i;

    __PHYSFS_platformGrabMutex(stateLock);

    for (i = searchPath; i != NULL; i = i->next)
        callback(data, i->dirName);

    __PHYSFS_platformReleaseMutex(stateLock);
} /* PHYSFS_getSearchPathCallback */


int PHYSFS_setSaneConfig(const char *organization, const char *appName,
                         const char *archiveExt, int includeCdRoms,
                         int archivesFirst)
{
    const char *basedir = PHYSFS_getBaseDir();
    const char *userdir = PHYSFS_getUserDir();
    const char *dirsep = PHYSFS_getDirSeparator();
    char *str;

    BAIL_IF_MACRO(!initialized, ERR_NOT_INITIALIZED, 0);

        /* set write dir... */
    str = malloc(strlen(userdir) + (strlen(organization) * 2) +
                 (strlen(appName) * 2) + (strlen(dirsep) * 3) + 2);
    BAIL_IF_MACRO(str == NULL, ERR_OUT_OF_MEMORY, 0);
    sprintf(str, "%s.%s%s%s", userdir, organization, dirsep, appName);

    if (!PHYSFS_setWriteDir(str))
    {
        int no_write = 0;
        sprintf(str, ".%s/%s", organization, appName);
        if ( (PHYSFS_setWriteDir(userdir)) &&
             (PHYSFS_mkdir(str)) )
        {
            sprintf(str, "%s.%s%s%s", userdir, organization, dirsep, appName);
            if (!PHYSFS_setWriteDir(str))
                no_write = 1;
        } /* if */
        else
        {
            no_write = 1;
        } /* else */

        if (no_write)
        {
            PHYSFS_setWriteDir(NULL);   /* just in case. */
            free(str);
            BAIL_MACRO(ERR_CANT_SET_WRITE_DIR, 0);
        } /* if */
    } /* if */

    /* Put write dir first in search path... */
    PHYSFS_addToSearchPath(str, 0);
    free(str);

        /* Put base path on search path... */
    PHYSFS_addToSearchPath(basedir, 1);

        /* handle CD-ROMs... */
    if (includeCdRoms)
    {
        char **cds = PHYSFS_getCdRomDirs();
        char **i;
        for (i = cds; *i != NULL; i++)
            PHYSFS_addToSearchPath(*i, 1);

        PHYSFS_freeList(cds);
    } /* if */

        /* Root out archives, and add them to search path... */
    if (archiveExt != NULL)
    {
        char **rc = PHYSFS_enumerateFiles("/");
        char **i;
        size_t extlen = strlen(archiveExt);
        char *ext;

        for (i = rc; *i != NULL; i++)
        {
            size_t l = strlen(*i);
            if ((l > extlen) && ((*i)[l - extlen - 1] == '.'))
            {
                ext = (*i) + (l - extlen);
                if (__PHYSFS_platformStricmp(ext, archiveExt) == 0)
                {
                    const char *d = PHYSFS_getRealDir(*i);
                    str = malloc(strlen(d) + strlen(dirsep) + l + 1);
                    if (str != NULL)
                    {
                        sprintf(str, "%s%s%s", d, dirsep, *i);
                        PHYSFS_addToSearchPath(str, archivesFirst == 0);
                        free(str);
                    } /* if */
                } /* if */
            } /* if */
        } /* for */

        PHYSFS_freeList(rc);
    } /* if */

    return(1);
} /* PHYSFS_setSaneConfig */


void PHYSFS_permitSymbolicLinks(int allow)
{
    allowSymLinks = allow;
} /* PHYSFS_permitSymbolicLinks */


/* string manipulation in C makes my ass itch. */
char * __PHYSFS_convertToDependent(const char *prepend,
                                              const char *dirName,
                                              const char *append)
{
    const char *dirsep = __PHYSFS_platformDirSeparator;
    size_t sepsize = strlen(dirsep);
    char *str;
    char *i1;
    char *i2;
    size_t allocSize;

    while (*dirName == '/')
        dirName++;

    allocSize = strlen(dirName) + 1;
    if (prepend != NULL)
        allocSize += strlen(prepend) + sepsize;
    if (append != NULL)
        allocSize += strlen(append) + sepsize;

        /* make sure there's enough space if the dir separator is bigger. */
    if (sepsize > 1)
    {
        str = (char *) dirName;
        do
        {
            str = strchr(str, '/');
            if (str != NULL)
            {
                allocSize += (sepsize - 1);
                str++;
            } /* if */
        } while (str != NULL);
    } /* if */

    str = (char *) malloc(allocSize);
    BAIL_IF_MACRO(str == NULL, ERR_OUT_OF_MEMORY, NULL);

    if (prepend == NULL)
        *str = '\0';
    else
    {
        strcpy(str, prepend);
        strcat(str, dirsep);
    } /* else */

    for (i1 = (char *) dirName, i2 = str + strlen(str); *i1; i1++, i2++)
    {
        if (*i1 == '/')
        {
            strcpy(i2, dirsep);
            i2 += sepsize;
        } /* if */
        else
        {
            *i2 = *i1;
        } /* else */
    } /* for */
    *i2 = '\0';

    if (append)
    {
        strcat(str, dirsep);
        strcpy(str, append);
    } /* if */

    return(str);
} /* __PHYSFS_convertToDependent */


/*
 * Verify that (fname) (in platform-independent notation), in relation
 *  to (h) is secure. That means that each element of fname is checked
 *  for symlinks (if they aren't permitted). Also, elements such as
 *  ".", "..", or ":" are flagged.
 *
 * With some exceptions (like PHYSFS_mkdir(), which builds multiple subdirs
 *  at a time), you should always pass zero for "allowMissing" for efficiency.
 *
 * Returns non-zero if string is safe, zero if there's a security issue.
 *  PHYSFS_getLastError() will specify what was wrong.
 */
int __PHYSFS_verifySecurity(DirHandle *h, const char *fname, int allowMissing)
{
    int retval = 1;
    char *start;
    char *end;
    char *str;

    if (*fname == '\0')  /* quick rejection. */
        return(1);

    /* !!! FIXME: Can we ditch this malloc()? */
    start = str = malloc(strlen(fname) + 1);
    BAIL_IF_MACRO(str == NULL, ERR_OUT_OF_MEMORY, 0);
    strcpy(str, fname);

    while (1)
    {
        end = strchr(start, '/');
        if (end != NULL)
            *end = '\0';

        if ( (strcmp(start, ".") == 0) ||
             (strcmp(start, "..") == 0) ||
             (strchr(start, '\\') != NULL) ||
             (strchr(start, ':') != NULL) )
        {
            __PHYSFS_setError(ERR_INSECURE_FNAME);
            retval = 0;
            break;
        } /* if */

        if (!allowSymLinks)
        {
            if (h->funcs->isSymLink(h->opaque, str, &retval))
            {
                __PHYSFS_setError(ERR_SYMLINK_DISALLOWED);
                free(str);
                return(0); /* insecure. */
            } /* if */

            /* break out early if path element is missing. */
            if (!retval)
            {
                /*
                 * We need to clear it if it's the last element of the path,
                 *  since this might be a non-existant file we're opening
                 *  for writing...
                 */
                if ((end == NULL) || (allowMissing))
                    retval = 1;
                break;
            } /* if */
        } /* if */

        if (end == NULL)
            break;

        *end = '/';
        start = end + 1;
    } /* while */

    free(str);
    return(retval);
} /* __PHYSFS_verifySecurity */


int PHYSFS_mkdir(const char *dname)
{
    DirHandle *h;
    char *str;
    char *start;
    char *end;
    int retval = 0;
    int exists = 1;  /* force existance check on first path element. */

    BAIL_IF_MACRO(dname == NULL, ERR_INVALID_ARGUMENT, 0);
    while (*dname == '/')
        dname++;

    __PHYSFS_platformGrabMutex(stateLock);
    BAIL_IF_MACRO_MUTEX(writeDir == NULL, ERR_NO_WRITE_DIR, stateLock, 0);
    h = writeDir;
    BAIL_IF_MACRO_MUTEX(!__PHYSFS_verifySecurity(h,dname,1),NULL,stateLock,0);
    start = str = malloc(strlen(dname) + 1);
    BAIL_IF_MACRO_MUTEX(str == NULL, ERR_OUT_OF_MEMORY, stateLock, 0);
    strcpy(str, dname);

    while (1)
    {
        end = strchr(start, '/');
        if (end != NULL)
            *end = '\0';

        /* only check for existance if all parent dirs existed, too... */
        if (exists)
            retval = h->funcs->isDirectory(h->opaque, str, &exists);

        if (!exists)
            retval = h->funcs->mkdir(h->opaque, str);

        if (!retval)
            break;

        if (end == NULL)
            break;

        *end = '/';
        start = end + 1;
    } /* while */

    __PHYSFS_platformReleaseMutex(stateLock);

    free(str);
    return(retval);
} /* PHYSFS_mkdir */


int PHYSFS_delete(const char *fname)
{
    int retval;
    DirHandle *h;

    BAIL_IF_MACRO(fname == NULL, ERR_INVALID_ARGUMENT, 0);
    while (*fname == '/')
        fname++;

    __PHYSFS_platformGrabMutex(stateLock);

    BAIL_IF_MACRO_MUTEX(writeDir == NULL, ERR_NO_WRITE_DIR, stateLock, 0);
    h = writeDir;
    BAIL_IF_MACRO_MUTEX(!__PHYSFS_verifySecurity(h,fname,0),NULL,stateLock,0);
    retval = h->funcs->remove(h->opaque, fname);

    __PHYSFS_platformReleaseMutex(stateLock);
    return(retval);
} /* PHYSFS_delete */


const char *PHYSFS_getRealDir(const char *filename)
{
    DirHandle *i;
    const char *retval = NULL;

    while (*filename == '/')
        filename++;

    __PHYSFS_platformGrabMutex(stateLock);
    for (i = searchPath; ((i != NULL) && (retval == NULL)); i = i->next)
    {
        if (__PHYSFS_verifySecurity(i, filename, 0))
        {
            if (i->funcs->exists(i->opaque, filename))
                retval = i->dirName;
        } /* if */
    } /* for */
    __PHYSFS_platformReleaseMutex(stateLock);

    return(retval);
} /* PHYSFS_getRealDir */


static int locateInStringList(const char *str,
                              char **list,
                              PHYSFS_uint32 *pos)
{
    PHYSFS_uint32 hi = *pos - 1;
    PHYSFS_uint32 lo = 0;
    PHYSFS_uint32 i = hi / 2;
    int cmp;

    while (hi != lo)
    {
        cmp = strcmp(list[i], str);
        if (cmp == 0)  /* it's in the list already. */
            return(1);
        else if (cmp < 0)
            hi = i;
        else
            lo = i;
        i = lo + ((hi - lo) / 2);
    } /* while */

    /* hi == lo, check it in case it's the match... */
    cmp = strcmp(list[lo], str);
    if (cmp == 0)
        return(1);

    /* not in the list, set insertion point... */
    *pos = (cmp < 0) ? lo : lo + 1;
    return(0);
} /* locateInStringList */


static void enumFilesCallback(void *data, const char *str)
{
    PHYSFS_uint32 pos;
    void *ptr;
    char *newstr;
    EnumStringListCallbackData *pecd = (EnumStringListCallbackData *) data;

    /*
     * See if file is in the list already, and if not, insert it in there
     *  alphabetically...
     */
    pos = pecd->size;
    if (pos > 0)
    {
        if (locateInStringList(str, pecd->list, &pos))
            return;  /* already in the list. */
    } /* if */

    ptr = realloc(pecd->list, (pecd->size + 2) * sizeof (char *));
    newstr = malloc(strlen(str) + 1);
    if (ptr != NULL)
        pecd->list = (char **) ptr;

    if ((ptr == NULL) || (newstr == NULL))
        return;  /* better luck next time. */

    strcpy(newstr, str);

    if (pos != pecd->size)
    {
        memmove(&pecd->list[pos+1], &pecd->list[pos],
                 sizeof (char *) * ((pecd->size) - pos));
    } /* if */

    pecd->list[pos] = newstr;
    pecd->size++;
} /* enumFilesCallback */


char **PHYSFS_enumerateFiles(const char *path)
{
    EnumStringListCallbackData ecd;
    memset(&ecd, '\0', sizeof (ecd));
    ecd.list = (char **) malloc(sizeof (char *));
    BAIL_IF_MACRO(ecd.list == NULL, ERR_OUT_OF_MEMORY, NULL);
    PHYSFS_enumerateFilesCallback(path, enumFilesCallback, &ecd);
    ecd.list[ecd.size] = NULL;
    return(ecd.list);
} /* PHYSFS_enumerateFiles */


void PHYSFS_enumerateFilesCallback(const char *path,
                                   PHYSFS_StringCallback callback,
                                   void *data)
{
    DirHandle *i;
    int noSyms;

    if ((path == NULL) || (callback == NULL))
        return;

    while (*path == '/')
        path++;

    __PHYSFS_platformGrabMutex(stateLock);
    noSyms = !allowSymLinks;
    for (i = searchPath; i != NULL; i = i->next)
    {
        if (__PHYSFS_verifySecurity(i, path, 0))
            i->funcs->enumerateFiles(i->opaque, path, noSyms, callback, data);
    } /* for */
    __PHYSFS_platformReleaseMutex(stateLock);
} /* PHYSFS_enumerateFilesCallback */


int PHYSFS_exists(const char *fname)
{
    BAIL_IF_MACRO(fname == NULL, ERR_INVALID_ARGUMENT, 0);
    while (*fname == '/')
        fname++;

    return(PHYSFS_getRealDir(fname) != NULL);
} /* PHYSFS_exists */


PHYSFS_sint64 PHYSFS_getLastModTime(const char *fname)
{
    DirHandle *i;
    PHYSFS_sint64 retval = -1;
    int fileExists = 0;

    BAIL_IF_MACRO(fname == NULL, ERR_INVALID_ARGUMENT, 0);
    while (*fname == '/')
        fname++;

    if (*fname == '\0')   /* eh...punt if it's the root dir. */
        return(1);

    __PHYSFS_platformGrabMutex(stateLock);
    for (i = searchPath; ((i != NULL) && (!fileExists)); i = i->next)
    {
        if (__PHYSFS_verifySecurity(i, fname, 0))
            retval = i->funcs->getLastModTime(i->opaque, fname, &fileExists);
    } /* for */
    __PHYSFS_platformReleaseMutex(stateLock);

    return(retval);
} /* PHYSFS_getLastModTime */


int PHYSFS_isDirectory(const char *fname)
{
    DirHandle *i;
    int retval = 0;
    int fileExists = 0;

    BAIL_IF_MACRO(fname == NULL, ERR_INVALID_ARGUMENT, 0);
    while (*fname == '/')
        fname++;

    BAIL_IF_MACRO(*fname == '\0', NULL, 1); /* Root is always a dir.  :) */

    __PHYSFS_platformGrabMutex(stateLock);
    for (i = searchPath; ((i != NULL) && (!fileExists)); i = i->next)
    {
        if (__PHYSFS_verifySecurity(i, fname, 0))
            retval = i->funcs->isDirectory(i->opaque, fname, &fileExists);
    } /* for */
    __PHYSFS_platformReleaseMutex(stateLock);

    return(retval);
} /* PHYSFS_isDirectory */


int PHYSFS_isSymbolicLink(const char *fname)
{
    DirHandle *i;
    int retval = 0;
    int fileExists = 0;

    BAIL_IF_MACRO(!allowSymLinks, ERR_SYMLINK_DISALLOWED, 0);

    BAIL_IF_MACRO(fname == NULL, ERR_INVALID_ARGUMENT, 0);
    while (*fname == '/')
        fname++;

    BAIL_IF_MACRO(*fname == '\0', NULL, 0);   /* Root is never a symlink */

    __PHYSFS_platformGrabMutex(stateLock);
    for (i = searchPath; ((i != NULL) && (!fileExists)); i = i->next)
    {
        if (__PHYSFS_verifySecurity(i, fname, 0))
            retval = i->funcs->isSymLink(i->opaque, fname, &fileExists);
    } /* for */
    __PHYSFS_platformReleaseMutex(stateLock);

    return(retval);
} /* PHYSFS_isSymbolicLink */


static PHYSFS_File *doOpenWrite(const char *fname, int appending)
{
    void *opaque = NULL;
    FileHandle *fh = NULL;
    DirHandle *h = NULL;
    const PHYSFS_Archiver *f;

    BAIL_IF_MACRO(fname == NULL, ERR_INVALID_ARGUMENT, NULL);
    while (*fname == '/')
        fname++;

    __PHYSFS_platformGrabMutex(stateLock);
    BAIL_IF_MACRO_MUTEX(!writeDir, ERR_NO_WRITE_DIR, stateLock, NULL);

    h = writeDir;
    BAIL_IF_MACRO_MUTEX(!__PHYSFS_verifySecurity(h, fname, 0), NULL,
                        stateLock, NULL);

    f = h->funcs;
    if (appending)
        opaque = f->openAppend(h->opaque, fname);
    else
        opaque = f->openWrite(h->opaque, fname);

    BAIL_IF_MACRO_MUTEX(opaque == NULL, NULL, stateLock, NULL);

    fh = (FileHandle *) malloc(sizeof (FileHandle));
    if (fh == NULL)
    {
        f->fileClose(opaque);
        BAIL_MACRO_MUTEX(ERR_OUT_OF_MEMORY, stateLock, NULL);
    } /* if */
    else
    {
        memset(fh, '\0', sizeof (FileHandle));
        fh->opaque = opaque;
        fh->dirHandle = h;
        fh->funcs = h->funcs;
        fh->next = openWriteList;
        openWriteList = fh;
    } /* else */

    __PHYSFS_platformReleaseMutex(stateLock);
    return((PHYSFS_File *) fh);
} /* doOpenWrite */


PHYSFS_File *PHYSFS_openWrite(const char *filename)
{
    return(doOpenWrite(filename, 0));
} /* PHYSFS_openWrite */


PHYSFS_File *PHYSFS_openAppend(const char *filename)
{
    return(doOpenWrite(filename, 1));
} /* PHYSFS_openAppend */


PHYSFS_File *PHYSFS_openRead(const char *fname)
{
    FileHandle *fh = NULL;
    int fileExists = 0;
    DirHandle *i = NULL;
    fvoid *opaque = NULL;

    BAIL_IF_MACRO(fname == NULL, ERR_INVALID_ARGUMENT, NULL);
    while (*fname == '/')
        fname++;

    __PHYSFS_platformGrabMutex(stateLock);
    BAIL_IF_MACRO_MUTEX(!searchPath, ERR_NOT_IN_SEARCH_PATH, stateLock, NULL);

    i = searchPath;

    do
    {
        if (__PHYSFS_verifySecurity(i, fname, 0))
        {
            opaque = i->funcs->openRead(i->opaque, fname, &fileExists);
            if (opaque)
                break;
        } /* if */
        i = i->next;
    } while ((i != NULL) && (!fileExists));

    BAIL_IF_MACRO_MUTEX(opaque == NULL, NULL, stateLock, NULL);

    fh = (FileHandle *) malloc(sizeof (FileHandle));
    if (fh == NULL)
    {
        i->funcs->fileClose(opaque);
        BAIL_MACRO_MUTEX(ERR_OUT_OF_MEMORY, stateLock, NULL);
    } /* if */

    memset(fh, '\0', sizeof (FileHandle));
    fh->opaque = opaque;
    fh->forReading = 1;
    fh->dirHandle = i;
    fh->funcs = i->funcs;
    fh->next = openReadList;
    openReadList = fh;
    __PHYSFS_platformReleaseMutex(stateLock);

    return((PHYSFS_File *) fh);
} /* PHYSFS_openRead */


static int closeHandleInOpenList(FileHandle **list, FileHandle *handle)
{
    FileHandle *prev = NULL;
    FileHandle *i;
    int rc = 1;

    for (i = *list; i != NULL; i = i->next)
    {
        if (i == handle)  /* handle is in this list? */
        {
            PHYSFS_uint8 *tmp = handle->buffer;
            rc = PHYSFS_flush((PHYSFS_File *) handle);
            if (rc)
                rc = handle->funcs->fileClose(handle->opaque);
            if (!rc)
                return(-1);

            if (tmp != NULL)  /* free any associated buffer. */
                free(tmp);

            if (prev == NULL)
                *list = handle->next;
            else
                prev->next = handle->next;

            free(handle);
            return(1);
        } /* if */
        prev = i;
    } /* for */

    return(0);
} /* closeHandleInOpenList */


int PHYSFS_close(PHYSFS_File *_handle)
{
    FileHandle *handle = (FileHandle *) _handle;
    int rc;

    __PHYSFS_platformGrabMutex(stateLock);

    /* -1 == close failure. 0 == not found. 1 == success. */
    rc = closeHandleInOpenList(&openReadList, handle);
    BAIL_IF_MACRO_MUTEX(rc == -1, NULL, stateLock, 0);
    if (!rc)
    {
        rc = closeHandleInOpenList(&openWriteList, handle);
        BAIL_IF_MACRO_MUTEX(rc == -1, NULL, stateLock, 0);
    } /* if */

    __PHYSFS_platformReleaseMutex(stateLock);
    BAIL_IF_MACRO(!rc, ERR_NOT_A_HANDLE, 0);
    return(1);
} /* PHYSFS_close */


static PHYSFS_sint64 doBufferedRead(FileHandle *fh, void *buffer,
                                    PHYSFS_uint32 objSize,
                                    PHYSFS_uint32 objCount)
{
    PHYSFS_sint64 retval = 0;
    PHYSFS_uint32 remainder = 0;

    while (objCount > 0)
    {
        PHYSFS_uint32 buffered = fh->buffill - fh->bufpos;
        PHYSFS_uint64 mustread = (objSize * objCount) - remainder;
        PHYSFS_uint32 copied;

        if (buffered == 0) /* need to refill buffer? */
        {
            PHYSFS_sint64 rc = fh->funcs->read(fh->opaque, fh->buffer,
                                                1, fh->bufsize);
            if (rc <= 0)
            {
                fh->bufpos -= remainder;
                return(((rc == -1) && (retval == 0)) ? -1 : retval);
            } /* if */

            buffered = fh->buffill = (PHYSFS_uint32) rc;
            fh->bufpos = 0;
        } /* if */

        if (buffered > mustread)
            buffered = (PHYSFS_uint32) mustread;

        memcpy(buffer, fh->buffer + fh->bufpos, (size_t) buffered);
        buffer = ((PHYSFS_uint8 *) buffer) + buffered;
        fh->bufpos += buffered;
        buffered += remainder;  /* take remainder into account. */
        copied = (buffered / objSize);
        remainder = (buffered % objSize);
        retval += copied;
        objCount -= copied;
    } /* while */

    return(retval);
} /* doBufferedRead */


PHYSFS_sint64 PHYSFS_read(PHYSFS_File *handle, void *buffer,
                          PHYSFS_uint32 objSize, PHYSFS_uint32 objCount)
{
    FileHandle *fh = (FileHandle *) handle;

    BAIL_IF_MACRO(!fh->forReading, ERR_FILE_ALREADY_OPEN_W, -1);
    if (fh->buffer != NULL)
        return(doBufferedRead(fh, buffer, objSize, objCount));

    return(fh->funcs->read(fh->opaque, buffer, objSize, objCount));
} /* PHYSFS_read */


static PHYSFS_sint64 doBufferedWrite(PHYSFS_File *handle, const void *buffer,
                                     PHYSFS_uint32 objSize,
                                     PHYSFS_uint32 objCount)
{
    FileHandle *fh = (FileHandle *) handle;
    
    /* whole thing fits in the buffer? */
    if (fh->buffill + (objSize * objCount) < fh->bufsize)
    {
        memcpy(fh->buffer + fh->buffill, buffer, objSize * objCount);
        fh->buffill += (objSize * objCount);
        return(objCount);
    } /* if */

    /* would overflow buffer. Flush and then write the new objects, too. */
    BAIL_IF_MACRO(!PHYSFS_flush(handle), NULL, -1);
    return(fh->funcs->write(fh->opaque, buffer, objSize, objCount));
} /* doBufferedWrite */


PHYSFS_sint64 PHYSFS_write(PHYSFS_File *handle, const void *buffer,
                           PHYSFS_uint32 objSize, PHYSFS_uint32 objCount)
{
    FileHandle *fh = (FileHandle *) handle;

    BAIL_IF_MACRO(fh->forReading, ERR_FILE_ALREADY_OPEN_R, -1);
    if (fh->buffer != NULL)
        return(doBufferedWrite(handle, buffer, objSize, objCount));

    return(fh->funcs->write(fh->opaque, buffer, objSize, objCount));
} /* PHYSFS_write */


int PHYSFS_eof(PHYSFS_File *handle)
{
    FileHandle *fh = (FileHandle *) handle;

    if (!fh->forReading)  /* never EOF on files opened for write/append. */
        return(0);

    /* eof if buffer is empty and archiver says so. */
    return((fh->bufpos == fh->buffill) && (fh->funcs->eof(fh->opaque)));
} /* PHYSFS_eof */


PHYSFS_sint64 PHYSFS_tell(PHYSFS_File *handle)
{
    FileHandle *fh = (FileHandle *) handle;
    PHYSFS_sint64 pos = fh->funcs->tell(fh->opaque);
    PHYSFS_sint64 retval = fh->forReading ?
                            (pos - fh->buffill) + fh->bufpos :
                            (pos + fh->buffill);
    return(retval);
} /* PHYSFS_tell */


int PHYSFS_seek(PHYSFS_File *handle, PHYSFS_uint64 pos)
{
    FileHandle *fh = (FileHandle *) handle;
    BAIL_IF_MACRO(!PHYSFS_flush(handle), NULL, 0);

    if (fh->buffer && fh->forReading)
    {
        /* avoid throwing away our precious buffer if seeking within it. */
        PHYSFS_sint64 offset = pos - PHYSFS_tell(handle);
        if ( /* seeking within the already-buffered range? */
            ((offset >= 0) && (offset <= fh->buffill - fh->bufpos)) /* fwd */
            || ((offset < 0) && (-offset <= fh->bufpos)) /* backward */ )
        {
            fh->bufpos += offset;
            return(1); /* successful seek */
        } /* if */
    } /* if */

    /* we have to fall back to a 'raw' seek. */
    fh->buffill = fh->bufpos = 0;
    return(fh->funcs->seek(fh->opaque, pos));
} /* PHYSFS_seek */


PHYSFS_sint64 PHYSFS_fileLength(PHYSFS_File *handle)
{
    FileHandle *fh = (FileHandle *) handle;
    return(fh->funcs->fileLength(fh->opaque));
} /* PHYSFS_filelength */


int PHYSFS_setBuffer(PHYSFS_File *handle, PHYSFS_uint64 _bufsize)
{
    FileHandle *fh = (FileHandle *) handle;
    PHYSFS_uint32 bufsize;

    BAIL_IF_MACRO(_bufsize > 0xFFFFFFFF, "buffer must fit in 32-bits", 0);
    bufsize = (PHYSFS_uint32) _bufsize;

    BAIL_IF_MACRO(!PHYSFS_flush(handle), NULL, 0);

    /*
     * For reads, we need to move the file pointer to where it would be
     *  if we weren't buffering, so that the next read will get the
     *  right chunk of stuff from the file. PHYSFS_flush() handles writes.
     */
    if ((fh->forReading) && (fh->buffill != fh->bufpos))
    {
        PHYSFS_uint64 pos;
        PHYSFS_sint64 curpos = fh->funcs->tell(fh->opaque);
        BAIL_IF_MACRO(curpos == -1, NULL, 0);
        pos = ((curpos - fh->buffill) + fh->bufpos);
        BAIL_IF_MACRO(!fh->funcs->seek(fh->opaque, pos), NULL, 0);
    } /* if */

    if (bufsize == 0)  /* delete existing buffer. */
    {
        if (fh->buffer != NULL)
        {
            free(fh->buffer);
            fh->buffer = NULL;
        } /* if */
    } /* if */

    else
    {
        PHYSFS_uint8 *newbuf = realloc(fh->buffer, bufsize);
        BAIL_IF_MACRO(newbuf == NULL, ERR_OUT_OF_MEMORY, 0);
        fh->buffer = newbuf;
    } /* else */

    fh->bufsize = bufsize;
    fh->buffill = fh->bufpos = 0;
    return(1);
} /* PHYSFS_setBuffer */


int PHYSFS_flush(PHYSFS_File *handle)
{
    FileHandle *fh = (FileHandle *) handle;
    PHYSFS_sint64 rc;

    if ((fh->forReading) || (fh->bufpos == fh->buffill))
        return(1);  /* open for read or buffer empty are successful no-ops. */

    /* dump buffer to disk. */
    rc = fh->funcs->write(fh->opaque, fh->buffer + fh->bufpos,
                          fh->buffill - fh->bufpos, 1);
    BAIL_IF_MACRO(rc <= 0, NULL, 0);
    fh->bufpos = fh->buffill = 0;
    return(1);
} /* PHYSFS_flush */


int PHYSFS_setAllocator(PHYSFS_Allocator *a)
{
    BAIL_IF_MACRO(initialized, ERR_IS_INITIALIZED, 0);
    externalAllocator = (a != NULL);
    if (externalAllocator)
        memcpy(&allocator, a, sizeof (PHYSFS_Allocator));

    return(1);
} /* PHYSFS_setAllocator */


static void setDefaultAllocator(void)
{
    assert(!externalAllocator);
    allocator.init = __PHYSFS_platformAllocatorInit;
    allocator.deinit = __PHYSFS_platformAllocatorDeinit;
    allocator.malloc = __PHYSFS_platformAllocatorMalloc;
    allocator.realloc = __PHYSFS_platformAllocatorRealloc;
    allocator.free = __PHYSFS_platformAllocatorFree;
} /* setDefaultAllocator */


PHYSFS_Allocator *__PHYSFS_getAllocator(void)
{
    return(&allocator);
} /* __PHYFS_getAllocator */

/* end of physfs.c ... */

