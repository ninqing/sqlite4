#!/usr/make
#
# Makefile for SQLITE
#
# This is a template makefile for SQLite.  Most people prefer to
# use the autoconf generated "configure" script to generate the
# makefile automatically.  But that does not work for everybody
# and in every situation.  If you are having problems with the
# "configure" script, you might want to try this makefile as an
# alternative.  Create a copy of this file, edit the parameters
# below and type "make".
#

#### The toplevel directory of the source tree.  This is the directory
#    that contains this "Makefile.in" and the "configure.in" script.
#
TOP = .

#### C Compiler and options for use in building executables that
#    will run on the platform that is doing the build.
#
BCC = clang -g
#BCC = /opt/ancic/bin/c89 -0

#### If the target operating system supports the "usleep()" system
#    call, then define the HAVE_USLEEP macro for all C modules.
#
#USLEEP = 
USLEEP = -DHAVE_USLEEP=1

#### Leave SQLITE4_DEBUG undefined for maximum speed.  Use SQLITE4_DEBUG=1
#    to check for memory leaks.  Use SQLITE4_DEBUG=2 to print a log of all
#    malloc()s and free()s in order to track down memory leaks.
#    
#    SQLite uses some expensive assert() statements in the inner loop.
#    You can make the library go almost twice as fast if you compile
#    with -DNDEBUG=1
#
#OPTS += -DHAVE_DLOPEN
OPTS += -DSQLITE4_DEBUG=1
OPTS += -DHAVE_GMTIME_R
OPTS += -DHAVE_LOCALTIME_R
OPTS += -DHAVE_MALLOC_USABLE_SIZE
OPTS += -DHAVE_USLEEP
#OPTS += -DLONGDOUBLE_TYPE=double
#OPTS += -DSQLITE4_64BIT_STATS
#OPTS += -DSQLITE4_CASE_SENSITIVE_LIKE
#OPTS += -DSQLITE4_CHECK_PAGES
#OPTS += -DSQLITE4_COVERAGE_TEST
#OPTS += -DSQLITE4_DEBUG
#OPTS += -DSQLITE4_DEFAULT_AUTOVACUUM=1
#OPTS += -DSQLITE4_DEFAULT_CACHE_SIZE=64
#OPTS += -DSQLITE4_DEFAULT_FILE_FORMAT=4
#OPTS += -DSQLITE4_DEFAULT_MEMSTATUS=0
#OPTS += -DSQLITE4_DEFAULT_PAGE_SIZE=1024
#OPTS += -DSQLITE4_DEFAULT_TEMP_CACHE_SIZE=32
#OPTS += -DSQLITE4_DISABLE_LFS=1
#OPTS += -DSQLITE4_ENABLE_8_3_NAMES
#OPTS += -DSQLITE4_ENABLE_ASYNCIO
#OPTS += -DSQLITE4_ENABLE_ATOMIC_WRITE
#OPTS += -DSQLITE4_ENABLE_COLUMN_METADATA
#OPTS += -DSQLITE4_ENABLE_EXPENSIVE_ASSERT
#OPTS += -DSQLITE4_ENABLE_FTS3
OPTS += -DSQLITE4_ENABLE_FTS4
#OPTS += -DSQLITE4_ENABLE_FTS3_PARENTHESIS
#OPTS += -DSQLITE4_ENABLE_INSTVFS=1
#OPTS += -DSQLITE4_ENABLE_ICU
#OPTS += -DSQLITE4_ENABLE_IOTRACE=1
#OPTS += -DSQLITE4_ENABLE_LOAD_EXTENSION=1
#OPTS += -DSQLITE4_ENABLE_LOCKING_STYLE=1
#OPTS += -DSQLITE4_ENABLE_MEMORY_MANAGEMENT=1
#OPTS += -DSQLITE4_ENABLE_MEMSYS3=1
#OPTS += -DSQLITE4_ENABLE_MEMSYS5=1
#OPTS += -DSQLITE4_ENABLE_OVERSIZE_CELL_CHECK
#OPTS += -DSQLITE4_ENABLE_PREUPDATE_HOOK
OPTS += -DSQLITE4_ENABLE_RTREE
#OPTS += -DSQLITE4_ENABLE_RTREE -DVARIANT_RSTARTREE_CHOOSESUBTREE
#OPTS += -DSQLITE4_ENABLE_SESSION
#OPTS += -DSQLITE4_ENABLE_STAT2
#OPTS += -DSQLITE4_ENABLE_STAT3
#OPTS += -DSQLITE4_ENABLE_TREE_EXPLAIN
#OPTS += -DSQLITE4_ENABLE_UNLOCK_NOTIFY
#OPTS += -DSQLITE4_ENABLE_UPDATE_DELETE_LIMIT
#OPTS += '-DSQLITE4_FILE_HEADER="MotorolaDB v001"'
#OPTS += -DSQLITE4_HAVE_ISNAN
#OPTS += -DSQLITE4_HOMEGROWN_RECURSIVE_MUTEX=1
#OPTS += -DSQLITE4_INT64_TYPE=int
#OPTS += -DSQLITE4_LOCK_TRACE
#OPTS += -DSQLITE4_MALLOC_SOFT_LIMIT=1024
#OPTS += -DSQLITE4_MAX_EXPR_DEPTH=0
#OPTS += -DSQLITE4_MAX_PAGE_SIZE=4096
#OPTS += -DSQLITE4_MAX_VARIABLE_NUMBER=500000
#OPTS += -DSQLITE4_MEMDEBUG=1
#OPTS += -DSQLITE4_MIXED_ENDIAN_64BIT_FLOAT=1
#OPTS += -DSQLITE4_MUTEX_NOOP=1
#OPTS += -DNO_GETTOD=1
OPTS += -DSQLITE4_NO_SYNC=1
#OPTS += -DSQLITE4_OMIT_ALTER_TABLE=1
#OPTS += -DSQLITE4_OMIT_AUTOMATIC_INDEX
#OPTS += -DSQLITE4_OMIT_AUTOVACUUM=1
#OPTS += -DSQLITE4_OMIT_BLOB_LITERAL=1
#OPTS += -DSQLITE4_OMIT_CHECK=1
#OPTS += -DSQLITE4_OMIT_DATETIME_FUNCS=1
#OPTS += -DSQLITE4_OMIT_DEPRECATED=1
#OPTS += -DSQLITE4_OMIT_DISKIO
#OPTS += -DSQLITE4_OMIT_FLOATING_POINT=1
#OPTS += -DSQLITE4_OMIT_ICU_LIKE
#OPTS += -DSQLITE4_OMIT_LOAD_EXTENSION=1
#OPTS += -DSQLITE4_OMIT_LOOKASIDE=1
#OPTS += -DSQLITE4_OMIT_MEMORYDB=1
#OPTS += -DSQLITE4_OMIT_MERGE_SORT=1
#OPTS += -DSQLITE4_OMIT_PROGESS_CALLBACK=1
#OPTS += -DSQLITE4_OMIT_REINDEX=1
#OPTS += -DSQLITE4_OMIT_SHARED_CACHE=1
#OPTS += -DSQLITE4_OMIT_TRACE=1
#OPTS += -DSQLITE4_OMIT_UTF16=1
#OPTS += -DSQLITE4_OMIT_VIEW=1
#OPTS += -DSQLITE4_OMIT_VIRTUALTABLE=1
#OPTS += -DSQLITE4_OMIT_WAL
#OPTS += -DSQLITE4_OMIT_WSD=1
#OPTS += -DSQLITE4_OS_OTHER=1
#OPTS += -DSQLITE4_OS_UNIX=1
#OPTS += -DSQLITE4_PCACHE_SEPARATE_HEADER
#OPTS += -DSQLITE4_PAGECACHE_BLOCKALLOC
#OPTS += -DSQLITE4_SECURE_DELETE=1
#OPTS += '-DSQLITE4_SHM_DIRECTORY="/dev/shm"'
#OPTS += -DSQLITE4_SMALL_STACK
#OPTS += -DSQLITE4_SOUNDEX=1
#OPTS += -DSQLITE4_TCL_DEFAULT_FULLMUTEX=1
#OPTS += -DSQLITE4_TEMP_STORE=3
#OPTS += -DSQLITE4_THREADSAFE=0
#OPTS += -DSQLITE4_THREADSAFE=1
#OPTS += -DSQLITE4_USE_ALLOCA
#OPTS += -DSQLITE4_ZERO_MALLOC=1
#OPTS += -DUSE_PREAD64=1
#OPTS += -DYYSTACKDEPTH=0
#OPTS += -DSQLITE4_DEFAULT_AUTOVACUUM=1
#OPTS += -DSQLITE4_DEFAULT_CACHE_SIZE=64
#OPTS += -DSQLITE4_DEFAULT_PAGE_SIZE=1024
#OPTS += -DSQLITE4_DEFAULT_TEMP_CACHE_SIZE=32
#OPTS += -DSQLITE4_DISABLE_LFS=1
#OPTS += -DSQLITE4_ENABLE_ATOMIC_WRITE=1
#OPTS += -DSQLITE4_ENABLE_IOTRACE=1
#OPTS += -DSQLITE4_ENABLE_MEMORY_MANAGEMENT=1
#OPTS += -DSQLITE4_MAX_PAGE_SIZE=4096
#OPTS += -DSQLITE4_OMIT_LOAD_EXTENSION=1
#OPTS += -DSQLITE4_OMIT_PROGRESS_CALLBACK=1
#OPTS += -DSQLITE4_OMIT_VIRTUALTABLE=1
#OPTS += -DSQLITE4_TEMP_STORE=3

#### The suffix to add to executable files.  ".exe" for windows.
#    Nothing for unix.
#
#EXE = .exe
EXE =

#### C Compile and options for use in building executables that 
#    will run on the target platform.  This is usually the same
#    as BCC, unless you are cross-compiling.
#
#TCC = gcc -O6
#TCC = gcc -g -rdynamic -O0 -Wall -fstrict-aliasing
TCC = clang -g -O0 -Wall -fstrict-aliasing
#TCC = gcc420 -g -O2 -Wall
#TCC = gcc -g -O0 -Wall -fprofile-arcs -ftest-coverage
#TCC = /opt/mingw/bin/i386-mingw32-gcc -O6
#TCC = /opt/ansic/bin/c89 -O +z -Wl,-a,archive

#### Tools used to build a static library.
#
AR = ar cr
#AR = /opt/mingw/bin/i386-mingw32-ar cr
RANLIB = ranlib
#RANLIB = /opt/mingw/bin/i386-mingw32-ranlib

#### Extra compiler options needed for programs that use the TCL library.
#
#TCL_FLAGS =
#TCL_FLAGS = -DSTATIC_BUILD=1
#TCL_FLAGS = -I/home/drh/tcltk/85linux
#TCL_FLAGS = -I/home/drh/tcltk/86linux
#TCL_FLAGS = -I/home/drh/tcltk/84win -DSTATIC_BUILD=1
#TCL_FLAGS = -I/home/drh/tcltk/8.3hpux

#### Linker options needed to link against the TCL library.
#
#LIBTCL = -ltcl8.4 -lm -ldl
#LIBTCL = /home/drh/tcltk/85linux/libtcl8.5.a -lm -ldl -lpthread
LIBTCL = -ltcl8.6 -lm -lpthread -ldl -lz
#LIBTCL = /home/drh/tcltk/84linux/libtcl84thread.a -lm -ldl
#LIBTCL = /home/drh/tcltk/8.4win/libtcl84s.a -lmsvcrt
#LIBTCL = /home/drh/tcltk/8.3hpux/libtcl8.3.a -ldld -lm -lc

#### Compiler options needed for programs that use the readline() library.
#
READLINE_FLAGS =
#READLINE_FLAGS = -DHAVE_READLINE=1 -I/usr/include/readline

#### Linker options needed by programs using readline() must link against.
#
LIBREADLINE = -ldl -lpthread
#LIBREADLINE = -static -lreadline -ltermcap

#### Should the database engine assume text is coded as UTF-8 or iso8859?
#
ENCODING  = UTF8
#ENCODING = ISO8859

#### Math library
#
MATHLIB = -lm
# MATHLIB =

#### AWK  (Needed by brain-dead Solaris systems)
#
NAWK = awk

#### THREADLIB  Libraries needed for threadtest 
#
THREADLIB = -ldl -lpthread

#OPTS += -DSQLITE4_FILE_HEADER='"bogus header..."'

# You should not have to change anything below this line
###############################################################################
include $(TOP)/main.mk
