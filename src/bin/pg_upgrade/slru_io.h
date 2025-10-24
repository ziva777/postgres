/*
 * slru_io.h
 *
 * Copyright (c) 2025, PostgreSQL Global Development Group
 * src/bin/pg_upgrade/slru_io.h
 */

/*
 * Some kind of iterator associated with a particular SLRU segment.  The idea is
 * to specify the segment and page number and then move through the pages.
 */

#include "postgres_fe.h"

/*
 * See access/slru.h
 *
 * Copy here, since slru.h could not be included in fe code.
 */
#define SLRU_PAGES_PER_SEGMENT 32

typedef struct SlruSegState SlruSegState;

extern SlruSegState *AllocSlruRead(char *dir);
extern char *SlruReadSwitchPage(SlruSegState *state, uint64 pageno);
extern void FreeSlruRead(SlruSegState *state);

extern SlruSegState *AllocSlruWrite(char *dir, bool long_segment_names);
extern char *SlruWriteSwitchPage(SlruSegState *state, uint64 pageno);
extern void FreeSlruWrite(SlruSegState *state);
