/*
 * slru_io.h
 *
 * Copyright (c) 2025, PostgreSQL Global Development Group
 * src/bin/pg_upgrade/slru_io.h
 */

/*
 * See access/slru.h
 *
 * Copy here, since slru.h could not be included in fe code.
 */

typedef struct SlruSegState SlruSegState;

extern SlruSegState *AllocSlruRead(char *dir);
extern char *SlruReadSwitchPage(SlruSegState *state, uint64 pageno);
extern void FreeSlruRead(SlruSegState *state);

extern SlruSegState *AllocSlruWrite(char *dir, bool long_segment_names);
extern char *SlruWriteSwitchPage(SlruSegState *state, uint64 pageno);
extern void FreeSlruWrite(SlruSegState *state);
